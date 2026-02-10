/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Ceph Zero-Copy Kernel Passthrough (kpass) - Internal Header
 *
 * Zero-Copy Architecture:
 * - RX: tcp_read_sock captures skb frag pages into sgvec (zero-copy)
 * - TX: MSG_SPLICE_PAGES sends from sgvec or pool pages (zero-copy)
 * - Userspace operates on handles (buffer IDs, socket IDs) only
 * - Optional peek API for explicit data copy when needed
 */

#ifndef _CEPH_KPASS_INTERNAL_H
#define _CEPH_KPASS_INTERNAL_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/net.h>
#include <linux/io_uring/cmd.h>

#include "../include/uapi/ceph_kpass.h"

#define KPASS_MAX_SG_ENTRIES	128	/* enough for 128KB at any frag size */
#define KPASS_MAX_BUF_SIZE	(1024 * 1024)	/* 1MB max */

/*
 * Buffer states
 */
enum kpass_buf_state {
	BUF_FREE = 0,
	BUF_ALLOCATED,
	BUF_TX_QUEUED,
	BUF_TX_INFLIGHT,
	BUF_RX_POSTED,
};

/*
 * Scatter-gather entry for zero-copy RX capture
 */
struct kpass_sg_entry {
	struct page	*page;
	u32		offset;		/* offset within page */
	u32		length;		/* bytes in this fragment */
};

/*
 * Single buffer descriptor
 */
struct kpass_buf {
	u32			id;
	enum kpass_buf_state	state;
	bool			captured;	/* true = sgvec holds skb page refs */

	/* Pool pages (for origination: poke + send) */
	struct page		**pages;
	unsigned int		nr_pages;

	/* Scatter-gather vector (for RX zero-copy capture + forwarding) */
	struct kpass_sg_entry	*sgvec;
	unsigned int		sg_count;	/* active entries */
	unsigned int		sg_max;		/* allocated capacity */
	u32			total_len;	/* total bytes across sgvec */

	/* Pending I/O context */
	struct kpass_sock	*sock;
	u32			offset;
	u32			length;
	u64			tag;
	struct io_uring_cmd	*ioucmd;

	struct list_head	list;
};

/*
 * Socket wrapper
 */
struct kpass_sock {
	u16			id;
	struct socket		*sock;
	struct kpass_session	*session;

	/* State */
	bool			connected;
	bool			listening;
	bool			closing;
	int			error;

	/* RX queue: buffers waiting for data */
	spinlock_t		rx_lock;
	struct list_head	rx_queue;
	struct work_struct	rx_work;

	/* TX queue: buffers waiting to send */
	spinlock_t		tx_lock;
	struct list_head	tx_queue;
	struct work_struct	tx_work;

	/* Pending accept (deferred to workqueue) */
	struct io_uring_cmd	*accept_ioucmd;
	u64			accept_tag;
	struct work_struct	accept_work;

	/* Original socket callbacks */
	void (*saved_data_ready)(struct sock *);
	void (*saved_write_space)(struct sock *);
	void (*saved_state_change)(struct sock *);
};

/*
 * Per-fd session state
 */
struct kpass_session {
	/* Buffer pool (kernel-only, NOT mapped to userspace) */
	struct kpass_buf	*buffers;
	u32			num_buffers;
	u32			buf_size;
	void			*pool_vm;	/* vmalloc (NOT vmalloc_user) */
	size_t			pool_size;

	spinlock_t		buf_lock;
	struct list_head	free_list;

	/* Auto-peek: copy first N bytes into recv completion */
	u32			auto_peek_size;	/* 0 = disabled, max 256 */

	/* Sockets */
	struct kpass_sock	*sockets[CEPH_KPASS_MAX_SOCKETS];
	DECLARE_BITMAP(sock_bitmap, CEPH_KPASS_MAX_SOCKETS);
	spinlock_t		sock_lock;

	/* Async work */
	struct workqueue_struct	*wq;
};

/* Function prototypes */
int kpass_init_buffer_pool(struct kpass_session *sess, u32 num_buffers, u32 buf_size);
void kpass_destroy_buffer_pool(struct kpass_session *sess);
struct kpass_buf *kpass_buf_alloc(struct kpass_session *sess);
void kpass_buf_free(struct kpass_session *sess, struct kpass_buf *buf);
struct kpass_buf *kpass_buf_get(struct kpass_session *sess, u32 id);
void *kpass_buf_kaddr(struct kpass_session *sess, struct kpass_buf *buf);

struct kpass_sock *kpass_sock_create(struct kpass_session *sess);
int kpass_sock_connect(struct kpass_sock *ksock, u16 family, const u8 *addr, u16 port);
int kpass_sock_listen(struct kpass_sock *ksock, u16 port, int backlog);
void kpass_sock_destroy(struct kpass_sock *ksock);
struct kpass_sock *kpass_get_sock(struct kpass_session *sess, u16 id);

void kpass_complete_cmd(struct io_uring_cmd *ioucmd, int res, unsigned int issue_flags);

#endif /* _CEPH_KPASS_INTERNAL_H */
