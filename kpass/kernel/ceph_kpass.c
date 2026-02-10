// SPDX-License-Identifier: GPL-2.0
/*
 * Ceph Zero-Copy Kernel Passthrough (kpass) Module
 *
 * Provides /dev/ceph_kpass character device for zero-copy network I/O.
 * Userspace coordinates via io_uring_cmd using handles only.
 *
 * Zero-Copy Architecture:
 * - RX: tcp_read_sock captures skb frag pages into sgvec (zero-copy)
 * - TX: MSG_SPLICE_PAGES sends from sgvec or pool pages (zero-copy)
 * - No mmap - userspace cannot access buffer data directly
 * - Userspace operates on handles (buffer IDs, socket IDs) only
 * - Optional peek API for explicit data copy when needed
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/tcp.h>
#include <linux/workqueue.h>
#include <linux/io_uring/cmd.h>
#include <linux/highmem.h>
#include <net/sock.h>
#include <net/tcp.h>

#include "ceph_kpass_internal.h"
#include "../include/uapi/ceph_kpass.h"

#define KPASS_DEVICE_NAME "ceph_kpass"

static struct miscdevice kpass_miscdev;

/*
 * ============================================================================
 * Buffer Pool Management
 * ============================================================================
 */

int kpass_init_buffer_pool(struct kpass_session *sess, u32 num_buffers, u32 buf_size)
{
	unsigned int pages_per_buf;
	int i, j;

	sess->num_buffers = min_t(u32, num_buffers, CEPH_KPASS_MAX_BUFFERS);
	sess->buf_size = buf_size ?: CEPH_KPASS_BUF_SIZE;
	pages_per_buf = DIV_ROUND_UP(sess->buf_size, PAGE_SIZE);
	sess->pool_size = (size_t)sess->num_buffers * sess->buf_size;

	/*
	 * Use vmalloc(), NOT vmalloc_user().
	 * Buffer pool stays in kernel, never mapped to userspace.
	 */
	sess->pool_vm = vmalloc(sess->pool_size);
	if (!sess->pool_vm)
		return -ENOMEM;

	sess->buffers = kcalloc(sess->num_buffers, sizeof(struct kpass_buf), GFP_KERNEL);
	if (!sess->buffers)
		goto err_vfree;

	INIT_LIST_HEAD(&sess->free_list);

	for (i = 0; i < sess->num_buffers; i++) {
		struct kpass_buf *buf = &sess->buffers[i];

		buf->id = i;
		buf->state = BUF_FREE;
		buf->nr_pages = pages_per_buf;
		buf->captured = false;
		INIT_LIST_HEAD(&buf->list);

		buf->pages = kcalloc(pages_per_buf, sizeof(struct page *), GFP_KERNEL);
		if (!buf->pages)
			goto err_free_pages;

		for (j = 0; j < pages_per_buf; j++) {
			void *addr = sess->pool_vm + (i * sess->buf_size) + (j * PAGE_SIZE);
			buf->pages[j] = vmalloc_to_page(addr);
		}

		buf->sgvec = kcalloc(KPASS_MAX_SG_ENTRIES,
				     sizeof(struct kpass_sg_entry), GFP_KERNEL);
		if (!buf->sgvec)
			goto err_free_pages;
		buf->sg_max = KPASS_MAX_SG_ENTRIES;
		buf->sg_count = 0;
		buf->total_len = 0;

		list_add_tail(&buf->list, &sess->free_list);
	}

	pr_info("ceph_kpass: initialized %u buffers of %u bytes (%zu KB total)\n",
		sess->num_buffers, sess->buf_size, sess->pool_size / 1024);

	return 0;

err_free_pages:
	for (j = 0; j <= i; j++) {
		kfree(sess->buffers[j].sgvec);
		kfree(sess->buffers[j].pages);
	}
	kfree(sess->buffers);
err_vfree:
	vfree(sess->pool_vm);
	sess->pool_vm = NULL;
	return -ENOMEM;
}

void kpass_destroy_buffer_pool(struct kpass_session *sess)
{
	int i;
	unsigned int j;

	if (sess->buffers) {
		for (i = 0; i < sess->num_buffers; i++) {
			/* Release any captured pages */
			if (sess->buffers[i].captured) {
				for (j = 0; j < sess->buffers[i].sg_count; j++)
					put_page(sess->buffers[i].sgvec[j].page);
			}
			kfree(sess->buffers[i].sgvec);
			kfree(sess->buffers[i].pages);
		}
		kfree(sess->buffers);
		sess->buffers = NULL;
	}

	if (sess->pool_vm) {
		vfree(sess->pool_vm);
		sess->pool_vm = NULL;
	}
}

struct kpass_buf *kpass_buf_alloc(struct kpass_session *sess)
{
	struct kpass_buf *buf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&sess->buf_lock, flags);
	if (!list_empty(&sess->free_list)) {
		buf = list_first_entry(&sess->free_list, struct kpass_buf, list);
		list_del_init(&buf->list);
		buf->state = BUF_ALLOCATED;
		buf->sock = NULL;
		buf->ioucmd = NULL;
		buf->sg_count = 0;
		buf->total_len = 0;
		buf->captured = false;
	}
	spin_unlock_irqrestore(&sess->buf_lock, flags);

	return buf;
}

void kpass_buf_free(struct kpass_session *sess, struct kpass_buf *buf)
{
	unsigned long flags;
	unsigned int i;

	/* Release captured page references */
	if (buf->captured) {
		for (i = 0; i < buf->sg_count; i++)
			put_page(buf->sgvec[i].page);
		buf->sg_count = 0;
		buf->total_len = 0;
		buf->captured = false;
	}

	spin_lock_irqsave(&sess->buf_lock, flags);
	buf->state = BUF_FREE;
	buf->sock = NULL;
	buf->ioucmd = NULL;
	list_add_tail(&buf->list, &sess->free_list);
	spin_unlock_irqrestore(&sess->buf_lock, flags);
}

struct kpass_buf *kpass_buf_get(struct kpass_session *sess, u32 id)
{
	if (id >= sess->num_buffers)
		return NULL;
	return &sess->buffers[id];
}

void *kpass_buf_kaddr(struct kpass_session *sess, struct kpass_buf *buf)
{
	return sess->pool_vm + ((size_t)buf->id * sess->buf_size);
}

/*
 * ============================================================================
 * Socket Management
 * ============================================================================
 */

static void kpass_sk_data_ready(struct sock *sk);
static void kpass_sk_write_space(struct sock *sk);
static void kpass_sk_state_change(struct sock *sk);
static void kpass_rx_work_fn(struct work_struct *work);
static void kpass_tx_work_fn(struct work_struct *work);
static void kpass_accept_work_fn(struct work_struct *work);

static u16 alloc_sock_id(struct kpass_session *sess)
{
	int id;
	unsigned long flags;

	spin_lock_irqsave(&sess->sock_lock, flags);
	id = find_first_zero_bit(sess->sock_bitmap, CEPH_KPASS_MAX_SOCKETS);
	if (id < CEPH_KPASS_MAX_SOCKETS)
		set_bit(id, sess->sock_bitmap);
	spin_unlock_irqrestore(&sess->sock_lock, flags);

	return (id < CEPH_KPASS_MAX_SOCKETS) ? id : 0xFFFF;
}

static void free_sock_id(struct kpass_session *sess, u16 id)
{
	unsigned long flags;

	spin_lock_irqsave(&sess->sock_lock, flags);
	if (id < CEPH_KPASS_MAX_SOCKETS)
		clear_bit(id, sess->sock_bitmap);
	spin_unlock_irqrestore(&sess->sock_lock, flags);
}

struct kpass_sock *kpass_sock_create(struct kpass_session *sess)
{
	struct kpass_sock *ksock;
	struct socket *sock;
	u16 id;
	int ret;

	id = alloc_sock_id(sess);
	if (id == 0xFFFF)
		return ERR_PTR(-EMFILE);

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (ret < 0) {
		free_sock_id(sess, id);
		return ERR_PTR(ret);
	}

	ksock = kzalloc(sizeof(*ksock), GFP_KERNEL);
	if (!ksock) {
		sock_release(sock);
		free_sock_id(sess, id);
		return ERR_PTR(-ENOMEM);
	}

	ksock->id = id;
	ksock->sock = sock;
	ksock->session = sess;
	ksock->connected = false;
	ksock->listening = false;
	ksock->closing = false;
	ksock->error = 0;

	spin_lock_init(&ksock->rx_lock);
	spin_lock_init(&ksock->tx_lock);
	INIT_LIST_HEAD(&ksock->rx_queue);
	INIT_LIST_HEAD(&ksock->tx_queue);
	INIT_WORK(&ksock->rx_work, kpass_rx_work_fn);
	INIT_WORK(&ksock->tx_work, kpass_tx_work_fn);
	INIT_WORK(&ksock->accept_work, kpass_accept_work_fn);

	/* Install our callbacks */
	ksock->saved_data_ready = sock->sk->sk_data_ready;
	ksock->saved_write_space = sock->sk->sk_write_space;
	ksock->saved_state_change = sock->sk->sk_state_change;

	sock->sk->sk_user_data = ksock;
	sock->sk->sk_data_ready = kpass_sk_data_ready;
	sock->sk->sk_write_space = kpass_sk_write_space;
	sock->sk->sk_state_change = kpass_sk_state_change;

	sess->sockets[id] = ksock;

	pr_debug("ceph_kpass: created socket %u\n", id);
	return ksock;
}

int kpass_sock_connect(struct kpass_sock *ksock, u16 family, const u8 *addr, u16 port)
{
	struct sockaddr_in sin;
	int ret;

	if (family != AF_INET)
		return -EAFNOSUPPORT;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	memcpy(&sin.sin_addr.s_addr, addr, 4);

	ret = kernel_connect(ksock->sock, (struct sockaddr *)&sin, sizeof(sin), O_NONBLOCK);
	if (ret < 0 && ret != -EINPROGRESS)
		return ret;

	return 0;
}

int kpass_sock_listen(struct kpass_sock *ksock, u16 port, int backlog)
{
	struct sockaddr_in sin;
	int ret;

	sock_set_reuseaddr(ksock->sock->sk);

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = INADDR_ANY;

	ret = kernel_bind(ksock->sock, (struct sockaddr *)&sin, sizeof(sin));
	if (ret < 0)
		return ret;

	ret = kernel_listen(ksock->sock, backlog);
	if (ret < 0)
		return ret;

	ksock->listening = true;
	pr_debug("ceph_kpass: socket %u listening on port %u\n", ksock->id, port);
	return 0;
}

void kpass_sock_destroy(struct kpass_sock *ksock)
{
	struct kpass_session *sess = ksock->session;
	struct kpass_buf *buf, *tmp;
	unsigned long flags;

	ksock->closing = true;

	/* Cancel pending work */
	cancel_work_sync(&ksock->accept_work);
	cancel_work_sync(&ksock->rx_work);
	cancel_work_sync(&ksock->tx_work);

	/* Fail pending RX */
	spin_lock_irqsave(&ksock->rx_lock, flags);
	list_for_each_entry_safe(buf, tmp, &ksock->rx_queue, list) {
		list_del_init(&buf->list);
		buf->state = BUF_ALLOCATED;
		if (buf->ioucmd) {
			io_uring_cmd_done(buf->ioucmd, -ECONNRESET,
					  IO_URING_F_UNLOCKED);
			buf->ioucmd = NULL;
		}
	}
	spin_unlock_irqrestore(&ksock->rx_lock, flags);

	/* Fail pending TX */
	spin_lock_irqsave(&ksock->tx_lock, flags);
	list_for_each_entry_safe(buf, tmp, &ksock->tx_queue, list) {
		list_del_init(&buf->list);
		buf->state = BUF_ALLOCATED;
		if (buf->ioucmd) {
			io_uring_cmd_done(buf->ioucmd, -ECONNRESET,
					  IO_URING_F_UNLOCKED);
			buf->ioucmd = NULL;
		}
	}
	spin_unlock_irqrestore(&ksock->tx_lock, flags);

	/* Fail pending accept */
	if (ksock->accept_ioucmd) {
		io_uring_cmd_done(ksock->accept_ioucmd, -ECONNRESET,
				  IO_URING_F_UNLOCKED);
		ksock->accept_ioucmd = NULL;
	}

	/* Restore original callbacks and close socket */
	if (ksock->sock) {
		ksock->sock->sk->sk_user_data = NULL;
		ksock->sock->sk->sk_data_ready = ksock->saved_data_ready;
		ksock->sock->sk->sk_write_space = ksock->saved_write_space;
		ksock->sock->sk->sk_state_change = ksock->saved_state_change;
		sock_release(ksock->sock);
	}

	sess->sockets[ksock->id] = NULL;
	free_sock_id(sess, ksock->id);
	pr_debug("ceph_kpass: destroyed socket %u\n", ksock->id);
	kfree(ksock);
}

struct kpass_sock *kpass_get_sock(struct kpass_session *sess, u16 id)
{
	if (id >= CEPH_KPASS_MAX_SOCKETS)
		return NULL;
	if (!test_bit(id, sess->sock_bitmap))
		return NULL;
	return sess->sockets[id];
}

/*
 * ============================================================================
 * Socket Callbacks - Called from softirq context
 * ============================================================================
 */

static void kpass_sk_data_ready(struct sock *sk)
{
	struct kpass_sock *ksock = sk->sk_user_data;

	if (!ksock || ksock->closing)
		return;

	/* Handle accept on listening socket - defer to workqueue */
	if (ksock->listening && ksock->accept_ioucmd) {
		queue_work(ksock->session->wq, &ksock->accept_work);
		return;
	}

	/* Schedule RX work for connected sockets */
	if (!list_empty(&ksock->rx_queue))
		queue_work(ksock->session->wq, &ksock->rx_work);
}

static void kpass_sk_write_space(struct sock *sk)
{
	struct kpass_sock *ksock = sk->sk_user_data;

	if (!ksock || ksock->closing)
		return;

	if (!list_empty(&ksock->tx_queue))
		queue_work(ksock->session->wq, &ksock->tx_work);
}

static void kpass_sk_state_change(struct sock *sk)
{
	struct kpass_sock *ksock = sk->sk_user_data;

	if (!ksock)
		return;

	switch (sk->sk_state) {
	case TCP_ESTABLISHED:
		if (!ksock->connected) {
			ksock->connected = true;
			pr_debug("ceph_kpass: socket %u connected\n", ksock->id);
		}
		break;

	case TCP_CLOSE:
	case TCP_CLOSE_WAIT:
		if (!ksock->error && !ksock->closing) {
			ksock->error = -ECONNRESET;
			pr_debug("ceph_kpass: socket %u closed\n", ksock->id);
		}
		break;
	}
}

/*
 * ============================================================================
 * Accept Work Function - Deferred from softirq to workqueue
 * ============================================================================
 */

static void kpass_accept_work_fn(struct work_struct *work)
{
	struct kpass_sock *ksock = container_of(work, struct kpass_sock, accept_work);
	struct socket *newsock = NULL;
	struct kpass_sock *new_ksock;
	int ret;

	if (!ksock->accept_ioucmd || ksock->closing)
		return;

	ret = kernel_accept(ksock->sock, &newsock, O_NONBLOCK);
	if (ret < 0 || !newsock) {
		if (ret != -EAGAIN) {
			io_uring_cmd_done(ksock->accept_ioucmd, ret,
					  IO_URING_F_UNLOCKED);
			ksock->accept_ioucmd = NULL;
		}
		return;
	}

	new_ksock = kpass_sock_create(ksock->session);
	if (IS_ERR(new_ksock)) {
		sock_release(newsock);
		io_uring_cmd_done(ksock->accept_ioucmd, PTR_ERR(new_ksock),
				  IO_URING_F_UNLOCKED);
		ksock->accept_ioucmd = NULL;
		return;
	}

	/* Replace auto-created socket with accepted one */
	sock_release(new_ksock->sock);
	new_ksock->sock = newsock;
	new_ksock->connected = true;
	newsock->sk->sk_user_data = new_ksock;
	newsock->sk->sk_data_ready = kpass_sk_data_ready;
	newsock->sk->sk_write_space = kpass_sk_write_space;
	newsock->sk->sk_state_change = kpass_sk_state_change;

	io_uring_cmd_done(ksock->accept_ioucmd, new_ksock->id,
			  IO_URING_F_UNLOCKED);
	ksock->accept_ioucmd = NULL;
}

/*
 * ============================================================================
 * RX/TX Work Functions - Zero-copy I/O
 * ============================================================================
 */

/*
 * tcp_read_sock actor: capture skb fragment pages into buffer sgvec.
 * Linear data (rare) is copied into pool pages. Paged fragments are
 * captured zero-copy via get_page.
 */
static int kpass_tcp_recv_actor(read_descriptor_t *desc,
				struct sk_buff *skb,
				unsigned int offset, size_t len)
{
	struct kpass_buf *buf = desc->arg.data;
	struct kpass_session *sess = buf->sock->session;
	unsigned int consumed = 0;

	if (!skb_frags_readable(skb))
		return -EIO;

	/* Linear data (skb->data) — must copy, slab pages aren't safe to ref */
	if (offset < skb_headlen(skb)) {
		u32 linear_avail = skb_headlen(skb) - offset;
		u32 linear_len = min_t(u32, linear_avail, len);
		u32 pool_off = buf->total_len;
		void *dst;

		if (pool_off + linear_len > sess->buf_size)
			linear_len = sess->buf_size - pool_off;
		if (linear_len == 0 || buf->sg_count >= buf->sg_max)
			return consumed ?: -ENOSPC;

		dst = kpass_buf_kaddr(sess, buf) + pool_off;
		skb_copy_bits(skb, offset, dst, linear_len);

		/* Reference pool page in sgvec (no get_page — pool pages are stable) */
		buf->sgvec[buf->sg_count].page = buf->pages[pool_off / PAGE_SIZE];
		buf->sgvec[buf->sg_count].offset = pool_off % PAGE_SIZE;
		buf->sgvec[buf->sg_count].length = linear_len;
		buf->sg_count++;

		consumed += linear_len;
		offset += linear_len;
		len -= linear_len;
		buf->total_len += linear_len;
	}

	/* Paged fragments — zero-copy capture */
	if (offset >= skb_headlen(skb) && len > 0) {
		u32 frag_off_in_skb = offset - skb_headlen(skb);
		int fi;

		/* Find starting fragment */
		for (fi = 0; fi < skb_shinfo(skb)->nr_frags; fi++) {
			skb_frag_t *f = &skb_shinfo(skb)->frags[fi];

			if (frag_off_in_skb < skb_frag_size(f))
				break;
			frag_off_in_skb -= skb_frag_size(f);
		}

		for (; fi < skb_shinfo(skb)->nr_frags && len > 0
		       && buf->sg_count < buf->sg_max; fi++) {
			skb_frag_t *f = &skb_shinfo(skb)->frags[fi];
			struct page *page = skb_frag_page(f);
			u32 foff = skb_frag_off(f) + frag_off_in_skb;
			u32 flen = skb_frag_size(f) - frag_off_in_skb;

			if (!page)
				break;  /* net_iov / devmem — can't capture */

			flen = min_t(u32, flen, len);
			get_page(page);

			buf->sgvec[buf->sg_count].page = page;
			buf->sgvec[buf->sg_count].offset = foff;
			buf->sgvec[buf->sg_count].length = flen;
			buf->sg_count++;

			consumed += flen;
			len -= flen;
			buf->total_len += flen;
			frag_off_in_skb = 0;
		}
	}

	desc->written += consumed;
	desc->count -= consumed;
	return consumed;
}

static void kpass_rx_work_fn(struct work_struct *work)
{
	struct kpass_sock *ksock = container_of(work, struct kpass_sock, rx_work);
	struct kpass_buf *buf;
	unsigned long flags;
	int ret;

	while (!ksock->closing) {
		read_descriptor_t desc;

		spin_lock_irqsave(&ksock->rx_lock, flags);
		if (list_empty(&ksock->rx_queue)) {
			spin_unlock_irqrestore(&ksock->rx_lock, flags);
			return;
		}
		buf = list_first_entry(&ksock->rx_queue, struct kpass_buf, list);
		spin_unlock_irqrestore(&ksock->rx_lock, flags);

		/* Reset sgvec state for this receive */
		buf->sg_count = 0;
		buf->total_len = 0;
		buf->captured = false;

		memset(&desc, 0, sizeof(desc));
		desc.count = buf->length;
		desc.arg.data = buf;

		lock_sock(ksock->sock->sk);
		ret = tcp_read_sock(ksock->sock->sk, &desc, kpass_tcp_recv_actor);
		release_sock(ksock->sock->sk);

		if (ret == 0) /* no data available */
			return;

		/* Dequeue the buffer */
		spin_lock_irqsave(&ksock->rx_lock, flags);
		list_del_init(&buf->list);
		spin_unlock_irqrestore(&ksock->rx_lock, flags);

		buf->state = BUF_ALLOCATED;
		if (ret > 0)
			buf->captured = true;

		if (buf->ioucmd) {
			io_uring_cmd_done(buf->ioucmd,
					  ret > 0 ? (s32)buf->total_len : ret,
					  IO_URING_F_UNLOCKED);
			buf->ioucmd = NULL;
		}
	}
}

static void kpass_tx_work_fn(struct work_struct *work)
{
	struct kpass_sock *ksock = container_of(work, struct kpass_sock, tx_work);
	struct kpass_buf *buf;
	unsigned long flags;

	while (!ksock->closing) {
		ssize_t total_sent = 0;
		ssize_t ret;

		spin_lock_irqsave(&ksock->tx_lock, flags);
		if (list_empty(&ksock->tx_queue)) {
			spin_unlock_irqrestore(&ksock->tx_lock, flags);
			return;
		}
		buf = list_first_entry(&ksock->tx_queue, struct kpass_buf, list);
		buf->state = BUF_TX_INFLIGHT;
		spin_unlock_irqrestore(&ksock->tx_lock, flags);

		if (buf->captured) {
			/* --- Captured buffer: send from sgvec --- */
			u32 skip = buf->offset;
			unsigned int si = 0;

			/* Advance past already-sent entries */
			while (si < buf->sg_count && skip >= buf->sgvec[si].length) {
				skip -= buf->sgvec[si].length;
				si++;
			}

			while (si < buf->sg_count) {
				struct kpass_sg_entry *sg = &buf->sgvec[si];
				u32 off = sg->offset + skip;
				u32 len = sg->length - skip;
				struct bio_vec bvec;
				struct msghdr msg = {};

				bvec_set_page(&bvec, sg->page, len, off);
				iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, len);
				msg.msg_flags = MSG_SPLICE_PAGES | MSG_DONTWAIT;
				if (si < buf->sg_count - 1)
					msg.msg_flags |= MSG_MORE;

				ret = sock_sendmsg(ksock->sock, &msg);
				if (ret == -EAGAIN || ret == -EWOULDBLOCK) {
					buf->offset += total_sent;
					return; /* write_space reschedules */
				}
				if (ret < 0)
					goto tx_error;

				total_sent += ret;
				skip = 0;
				if ((u32)ret < len) {
					/* partial send within this entry */
					buf->offset += total_sent;
					return;
				}
				si++;
			}
		} else {
			/* --- Pool buffer: send from pages array --- */
			u32 offset = buf->offset;
			u32 remain = buf->length;

			while (remain > 0) {
				unsigned int page_idx = offset / PAGE_SIZE;
				u32 page_off = offset % PAGE_SIZE;
				u32 chunk = min_t(u32, remain, PAGE_SIZE - page_off);
				struct bio_vec bvec;
				struct msghdr msg = {};

				bvec_set_page(&bvec, buf->pages[page_idx], chunk, page_off);
				iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, chunk);
				msg.msg_flags = MSG_SPLICE_PAGES | MSG_DONTWAIT;
				if (remain > chunk)
					msg.msg_flags |= MSG_MORE;

				ret = sock_sendmsg(ksock->sock, &msg);
				if (ret == -EAGAIN || ret == -EWOULDBLOCK) {
					buf->offset = offset;
					buf->length = remain;
					return;
				}
				if (ret < 0)
					goto tx_error;

				total_sent += ret;
				offset += ret;
				remain -= ret;
			}
		}

		/* Success — dequeue and complete */
		spin_lock_irqsave(&ksock->tx_lock, flags);
		list_del_init(&buf->list);
		spin_unlock_irqrestore(&ksock->tx_lock, flags);

		buf->state = BUF_ALLOCATED;
		if (buf->ioucmd) {
			io_uring_cmd_done(buf->ioucmd, total_sent,
					  IO_URING_F_UNLOCKED);
			buf->ioucmd = NULL;
		}
		continue;

tx_error:
		spin_lock_irqsave(&ksock->tx_lock, flags);
		list_del_init(&buf->list);
		spin_unlock_irqrestore(&ksock->tx_lock, flags);

		buf->state = BUF_ALLOCATED;
		if (buf->ioucmd) {
			io_uring_cmd_done(buf->ioucmd, ret,
					  IO_URING_F_UNLOCKED);
			buf->ioucmd = NULL;
		}
	}
}

/*
 * ============================================================================
 * io_uring Command Handler
 * ============================================================================
 */

void kpass_complete_cmd(struct io_uring_cmd *ioucmd, int res,
			unsigned int issue_flags)
{
	io_uring_cmd_done(ioucmd, res, issue_flags);
}

static int kpass_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	struct file *file = ioucmd->file;
	struct kpass_session *sess = file->private_data;
	const struct kpass_sqe_cmd *cmd = (const struct kpass_sqe_cmd *)ioucmd->cmd;
	struct kpass_buf *buf;
	struct kpass_sock *ksock;
	unsigned long irqflags;
	int ret;

	if (!sess)
		return -EINVAL;

	switch (cmd->op) {
	case KPASS_OP_INIT:
		if (sess->pool_vm) {
			kpass_complete_cmd(ioucmd, -EEXIST, issue_flags);
			return -EIOCBQUEUED;
		}
		if (cmd->init.buf_size > KPASS_MAX_BUF_SIZE) {
			kpass_complete_cmd(ioucmd, -EINVAL, issue_flags);
			return -EIOCBQUEUED;
		}
		ret = kpass_init_buffer_pool(sess, cmd->init.num_buffers, cmd->init.buf_size);
		kpass_complete_cmd(ioucmd, ret, issue_flags);
		return -EIOCBQUEUED;

	case KPASS_OP_SOCK_CREATE:
		ksock = kpass_sock_create(sess);
		if (IS_ERR(ksock)) {
			kpass_complete_cmd(ioucmd, PTR_ERR(ksock), issue_flags);
		} else {
			kpass_complete_cmd(ioucmd, ksock->id, issue_flags);
		}
		return -EIOCBQUEUED;

	case KPASS_OP_SOCK_CONNECT:
		ksock = kpass_get_sock(sess, cmd->sock_id);
		if (!ksock) {
			kpass_complete_cmd(ioucmd, -ENOENT, issue_flags);
			return -EIOCBQUEUED;
		}
		ret = kpass_sock_connect(ksock, cmd->connect.family,
					cmd->connect.addr, cmd->connect.port);
		kpass_complete_cmd(ioucmd, ret, issue_flags);
		return -EIOCBQUEUED;

	case KPASS_OP_SOCK_LISTEN:
		ksock = kpass_get_sock(sess, cmd->sock_id);
		if (!ksock) {
			kpass_complete_cmd(ioucmd, -ENOENT, issue_flags);
			return -EIOCBQUEUED;
		}
		ret = kpass_sock_listen(ksock, cmd->listen.port, cmd->listen.backlog);
		kpass_complete_cmd(ioucmd, ret, issue_flags);
		return -EIOCBQUEUED;

	case KPASS_OP_SOCK_ACCEPT:
		ksock = kpass_get_sock(sess, cmd->sock_id);
		if (!ksock || !ksock->listening) {
			kpass_complete_cmd(ioucmd, -EINVAL, issue_flags);
			return -EIOCBQUEUED;
		}
		/* Store ioucmd and schedule accept work */
		ksock->accept_ioucmd = ioucmd;
		ksock->accept_tag = cmd->tag;
		queue_work(sess->wq, &ksock->accept_work);
		return -EIOCBQUEUED;

	case KPASS_OP_SOCK_CLOSE:
		ksock = kpass_get_sock(sess, cmd->sock_id);
		if (!ksock) {
			kpass_complete_cmd(ioucmd, -ENOENT, issue_flags);
			return -EIOCBQUEUED;
		}
		kpass_sock_destroy(ksock);
		kpass_complete_cmd(ioucmd, 0, issue_flags);
		return -EIOCBQUEUED;

	case KPASS_OP_BUF_ALLOC:
		buf = kpass_buf_alloc(sess);
		if (!buf) {
			kpass_complete_cmd(ioucmd, -ENOMEM, issue_flags);
		} else {
			kpass_complete_cmd(ioucmd, buf->id, issue_flags);
		}
		return -EIOCBQUEUED;

	case KPASS_OP_BUF_FREE:
		buf = kpass_buf_get(sess, cmd->buf_id);
		if (!buf || buf->state != BUF_ALLOCATED) {
			kpass_complete_cmd(ioucmd, -EINVAL, issue_flags);
			return -EIOCBQUEUED;
		}
		kpass_buf_free(sess, buf);
		kpass_complete_cmd(ioucmd, 0, issue_flags);
		return -EIOCBQUEUED;

	case KPASS_OP_SEND:
		buf = kpass_buf_get(sess, cmd->buf_id);
		if (!buf || buf->state != BUF_ALLOCATED) {
			kpass_complete_cmd(ioucmd, -EINVAL, issue_flags);
			return -EIOCBQUEUED;
		}
		ksock = kpass_get_sock(sess, cmd->sock_id);
		if (!ksock || !ksock->connected) {
			kpass_complete_cmd(ioucmd, -ENOTCONN, issue_flags);
			return -EIOCBQUEUED;
		}
		/* Bounds check for non-captured pool sends */
		if (!buf->captured &&
		    cmd->send.offset + cmd->send.len > sess->buf_size) {
			kpass_complete_cmd(ioucmd, -EINVAL, issue_flags);
			return -EIOCBQUEUED;
		}

		buf->state = BUF_TX_QUEUED;
		buf->sock = ksock;
		buf->offset = cmd->send.offset;
		buf->length = cmd->send.len;
		buf->tag = cmd->tag;
		buf->ioucmd = ioucmd;

		spin_lock_irqsave(&ksock->tx_lock, irqflags);
		list_add_tail(&buf->list, &ksock->tx_queue);
		spin_unlock_irqrestore(&ksock->tx_lock, irqflags);

		queue_work(sess->wq, &ksock->tx_work);
		return -EIOCBQUEUED;

	case KPASS_OP_RECV:
		buf = kpass_buf_get(sess, cmd->buf_id);
		if (!buf || buf->state != BUF_ALLOCATED) {
			kpass_complete_cmd(ioucmd, -EINVAL, issue_flags);
			return -EIOCBQUEUED;
		}
		ksock = kpass_get_sock(sess, cmd->sock_id);
		if (!ksock) {
			kpass_complete_cmd(ioucmd, -ENOENT, issue_flags);
			return -EIOCBQUEUED;
		}
		/* Bounds check */
		if (cmd->recv.offset + cmd->recv.len > sess->buf_size) {
			kpass_complete_cmd(ioucmd, -EINVAL, issue_flags);
			return -EIOCBQUEUED;
		}

		buf->state = BUF_RX_POSTED;
		buf->sock = ksock;
		buf->offset = cmd->recv.offset;
		buf->length = cmd->recv.len;
		buf->tag = cmd->tag;
		buf->ioucmd = ioucmd;

		spin_lock_irqsave(&ksock->rx_lock, irqflags);
		list_add_tail(&buf->list, &ksock->rx_queue);
		spin_unlock_irqrestore(&ksock->rx_lock, irqflags);

		queue_work(sess->wq, &ksock->rx_work);
		return -EIOCBQUEUED;

	default:
		kpass_complete_cmd(ioucmd, -EINVAL, issue_flags);
		return -EIOCBQUEUED;
	}
}

/*
 * ============================================================================
 * File Operations
 * ============================================================================
 */

static int kpass_open(struct inode *inode, struct file *file)
{
	struct kpass_session *sess;

	sess = kzalloc(sizeof(*sess), GFP_KERNEL);
	if (!sess)
		return -ENOMEM;

	spin_lock_init(&sess->buf_lock);
	spin_lock_init(&sess->sock_lock);
	INIT_LIST_HEAD(&sess->free_list);
	bitmap_zero(sess->sock_bitmap, CEPH_KPASS_MAX_SOCKETS);

	sess->wq = alloc_workqueue("ceph_kpass_%d", WQ_UNBOUND | WQ_HIGHPRI, 0,
				   current->pid);
	if (!sess->wq) {
		kfree(sess);
		return -ENOMEM;
	}

	file->private_data = sess;
	pr_debug("ceph_kpass: session opened\n");
	return 0;
}

static int kpass_release(struct inode *inode, struct file *file)
{
	struct kpass_session *sess = file->private_data;
	int i;

	if (!sess)
		return 0;

	/* Close all sockets */
	for (i = 0; i < CEPH_KPASS_MAX_SOCKETS; i++) {
		if (test_bit(i, sess->sock_bitmap) && sess->sockets[i])
			kpass_sock_destroy(sess->sockets[i]);
	}

	destroy_workqueue(sess->wq);
	kpass_destroy_buffer_pool(sess);
	kfree(sess);

	pr_debug("ceph_kpass: session closed\n");
	return 0;
}

static long kpass_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct kpass_session *sess = file->private_data;

	if (!sess)
		return -EINVAL;

	switch (cmd) {
	case _IOR('K', 1, struct kpass_info): /* KPASS_IOC_GET_INFO */
	{
		struct kpass_info info = {
			.version = 2,
			.max_sockets = CEPH_KPASS_MAX_SOCKETS,
			.max_buffers = CEPH_KPASS_MAX_BUFFERS,
			.default_buf_size = CEPH_KPASS_BUF_SIZE,
		};
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}

	case _IOR('K', 3, struct kpass_buf_alloc_args): /* KPASS_IOC_BUF_ALLOC */
	{
		struct kpass_buf_alloc_args alloc_args;
		struct kpass_buf *buf;

		if (!sess->pool_vm)
			return -EINVAL;

		buf = kpass_buf_alloc(sess);
		alloc_args.buf_id = buf ? buf->id : -1;
		alloc_args._reserved = 0;

		if (copy_to_user((void __user *)arg, &alloc_args, sizeof(alloc_args)))
			return -EFAULT;
		return 0;
	}

	case _IOW('K', 4, u32): /* KPASS_IOC_BUF_FREE */
	{
		u32 buf_id;
		struct kpass_buf *buf;

		if (copy_from_user(&buf_id, (void __user *)arg, sizeof(buf_id)))
			return -EFAULT;

		buf = kpass_buf_get(sess, buf_id);
		if (!buf || buf->state != BUF_ALLOCATED)
			return -EINVAL;

		kpass_buf_free(sess, buf);
		return 0;
	}

	case _IOWR('K', 5, struct kpass_peek_args): /* KPASS_IOC_PEEK */
	{
		struct kpass_peek_args peek;
		struct kpass_buf *buf;

		if (copy_from_user(&peek, (void __user *)arg, sizeof(peek)))
			return -EFAULT;

		/* Validate buffer */
		if (peek.buf_id >= sess->num_buffers)
			return -EINVAL;

		buf = &sess->buffers[peek.buf_id];
		if (buf->state != BUF_ALLOCATED)
			return -EINVAL;

		if (buf->captured) {
			/* Walk sgvec to satisfy peek */
			u32 off = peek.offset;
			u32 remain;
			u8 __user *dest = (u8 __user *)peek.dest_addr;
			u32 copied = 0;
			int i;

			remain = (buf->total_len > peek.offset) ?
				 buf->total_len - peek.offset : 0;
			if (remain > peek.length)
				remain = peek.length;

			for (i = 0; i < buf->sg_count && remain > 0; i++) {
				struct kpass_sg_entry *sg = &buf->sgvec[i];
				u32 chunk;
				void *src;

				if (off >= sg->length) {
					off -= sg->length;
					continue;
				}
				chunk = sg->length - off;
				if (chunk > remain)
					chunk = remain;
				src = kmap_local_page(sg->page);
				if (copy_to_user(dest, src + sg->offset + off,
						 chunk)) {
					kunmap_local(src);
					return -EFAULT;
				}
				kunmap_local(src);
				dest += chunk;
				copied += chunk;
				remain -= chunk;
				off = 0;
			}
			peek.bytes_copied = copied;
		} else {
			/* Pool buffer path */
			void *src;
			u32 avail;

			if (peek.offset >= sess->buf_size)
				return -EINVAL;

			avail = sess->buf_size - peek.offset;
			peek.bytes_copied = min(peek.length, avail);

			src = sess->pool_vm +
			      ((size_t)peek.buf_id * sess->buf_size) +
			      peek.offset;
			if (copy_to_user((void __user *)peek.dest_addr,
					 src, peek.bytes_copied))
				return -EFAULT;
		}

		/* Return actual bytes copied */
		if (copy_to_user((void __user *)arg, &peek, sizeof(peek)))
			return -EFAULT;

		return 0;
	}

	case _IOW('K', 6, struct kpass_auto_peek_args): /* KPASS_IOC_SET_AUTO_PEEK */
	{
		struct kpass_auto_peek_args ap;

		if (copy_from_user(&ap, (void __user *)arg, sizeof(ap)))
			return -EFAULT;

		/* Clamp to maximum */
		sess->auto_peek_size = min_t(u32, ap.size, KPASS_AUTO_PEEK_MAX);
		return 0;
	}

	case _IOR('K', 7, struct kpass_auto_peek_args): /* KPASS_IOC_GET_AUTO_PEEK */
	{
		struct kpass_auto_peek_args ap = {
			.size = sess->auto_peek_size,
			._reserved = 0,
		};
		if (copy_to_user((void __user *)arg, &ap, sizeof(ap)))
			return -EFAULT;
		return 0;
	}

	case _IOWR('K', 8, struct kpass_peek_args): /* KPASS_IOC_POKE */
	{
		struct kpass_peek_args poke;
		struct kpass_buf *buf;
		void *dst;
		u32 avail;

		if (copy_from_user(&poke, (void __user *)arg, sizeof(poke)))
			return -EFAULT;

		/* Validate buffer */
		if (poke.buf_id >= sess->num_buffers)
			return -EINVAL;

		buf = &sess->buffers[poke.buf_id];
		if (buf->state != BUF_ALLOCATED)
			return -EINVAL;

		/* Can't poke into captured RX buffers */
		if (buf->captured)
			return -EINVAL;

		/* Validate offset */
		if (poke.offset >= sess->buf_size)
			return -EINVAL;

		/* Calculate available space and copy */
		avail = sess->buf_size - poke.offset;
		poke.bytes_copied = min(poke.length, avail);

		dst = sess->pool_vm + ((size_t)poke.buf_id * sess->buf_size) + poke.offset;
		if (copy_from_user(dst, (void __user *)poke.dest_addr, poke.bytes_copied))
			return -EFAULT;

		/* Return actual bytes copied */
		if (copy_to_user((void __user *)arg, &poke, sizeof(poke)))
			return -EFAULT;

		return 0;
	}

	default:
		return -ENOTTY;
	}
}

/*
 * File operations - NO mmap in zero-copy mode
 */
static const struct file_operations kpass_fops = {
	.owner		= THIS_MODULE,
	.open		= kpass_open,
	.release	= kpass_release,
	.unlocked_ioctl	= kpass_ioctl,
	.uring_cmd	= kpass_uring_cmd,
};

/*
 * ============================================================================
 * Module Init/Exit
 * ============================================================================
 */

static struct miscdevice kpass_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= KPASS_DEVICE_NAME,
	.fops	= &kpass_fops,
};

static int __init ceph_kpass_init(void)
{
	int ret;

	ret = misc_register(&kpass_miscdev);
	if (ret) {
		pr_err("ceph_kpass: failed to register misc device: %d\n", ret);
		return ret;
	}

	pr_info("ceph_kpass: loaded, device /dev/%s\n", KPASS_DEVICE_NAME);
	return 0;
}

static void __exit ceph_kpass_exit(void)
{
	misc_deregister(&kpass_miscdev);
	pr_info("ceph_kpass: unloaded\n");
}

module_init(ceph_kpass_init);
module_exit(ceph_kpass_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ceph Zero-Copy Kernel Passthrough");
MODULE_DESCRIPTION("Kernel module for zero-copy network I/O via tcp_read_sock + MSG_SPLICE_PAGES");
