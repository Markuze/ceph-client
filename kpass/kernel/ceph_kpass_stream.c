// SPDX-License-Identifier: GPL-2.0
/*
 * Stream/object implementation, included after the shared RX capture helpers.
 * stream_lock protects sockets, ranges, page charges, handles and requests.
 * Socket callbacks only schedule work; user copies and handle publication run
 * in the submitting task. No request stores a pointer into the submitted SQE.
 */

#include <linux/refcount.h>
#include <linux/sizes.h>

struct kpass_backing {
	struct page *page;
	refcount_t refs;
	u64 charge;
};

struct kpass_extent {
	struct list_head list;
	struct kpass_backing *backing;
	u64 start;
	u32 offset;
	u32 length;
};

struct kpass_object {
	refcount_t refs;
	struct list_head extents;
	u64 length;
};

struct kpass_request {
	struct list_head list;
	struct kpass_session *session;
	struct kpass_sock *sock;
	struct io_uring_cmd *ioucmd;
	struct kpass_sqe_cmd cmd;
	struct kpass_object *object;
	u64 progress;
	int result;
	bool ready;
	bool canceled;
	bool cqe32;
};

struct kpass_cmd_private {
	struct kpass_request *request;
};

static void kpass_stream_complete_task(struct io_tw_req tw_req,
				       io_tw_token_t tw);

static int kpass_stream_done(struct io_uring_cmd *ioucmd, int ret, u64 handle,
			     unsigned int issue_flags, bool cqe32)
{
	if (cqe32)
		io_uring_cmd_done32(ioucmd, ret, handle, issue_flags);
	else
		io_uring_cmd_done(ioucmd, ret, issue_flags);
	return -EIOCBQUEUED;
}

static void kpass_extent_free(struct kpass_session *sess,
			      struct kpass_extent *extent)
{
	struct kpass_backing *backing = extent->backing;

	if (refcount_dec_and_test(&backing->refs)) {
		sess->backing_bytes -= backing->charge;
		put_page(backing->page);
		kfree(backing);
	}
	list_del(&extent->list);
	sess->extent_count--;
	kfree(extent);
}

static void kpass_extents_free(struct kpass_session *sess,
			       struct list_head *extents)
{
	struct kpass_extent *extent, *next;

	list_for_each_entry_safe(extent, next, extents, list)
		kpass_extent_free(sess, extent);
}

static struct kpass_object *kpass_object_alloc(void)
{
	struct kpass_object *object = kzalloc_obj(*object);

	if (object) {
		refcount_set(&object->refs, 1);
		INIT_LIST_HEAD(&object->extents);
	}
	return object;
}

static void kpass_object_put(struct kpass_session *sess,
			     struct kpass_object *object)
{
	if (object && refcount_dec_and_test(&object->refs)) {
		kpass_extents_free(sess, &object->extents);
		kfree(object);
	}
}

/* Splitting shares a backing reference and never copies payload or its charge. */
static struct kpass_extent *kpass_extent_split(struct kpass_session *sess,
					       struct kpass_extent *extent,
					       u64 at)
{
	struct kpass_extent *right;
	u32 left = at - extent->start;

	if (sess->extent_count == KPASS_MAX_EXTENTS)
		return ERR_PTR(-ENOSPC);
	right = kmalloc_obj(*right);
	if (!right)
		return ERR_PTR(-ENOMEM);
	*right = *extent;
	refcount_inc(&right->backing->refs);
	right->start = at;
	right->offset += left;
	right->length -= left;
	extent->length = left;
	list_add(&right->list, &extent->list);
	sess->extent_count++;
	return right;
}

static struct kpass_object *kpass_object_lookup(struct kpass_session *sess,
						u64 handle)
{
	u32 slot = (u32)handle;

	if (!slot || slot > KPASS_MAX_OBJECTS)
		return NULL;
	slot--;
	if (sess->generations[slot] != handle >> 32)
		return NULL;
	return sess->objects[slot];
}

static u64 kpass_object_publish(struct kpass_session *sess,
				struct kpass_object *object)
{
	unsigned int slot;

	for (slot = 0; slot < KPASS_MAX_OBJECTS; slot++) {
		/* Retire exhausted generations instead of wrapping a stale ID. */
		if (sess->objects[slot] || sess->generations[slot] == U32_MAX)
			continue;
		sess->objects[slot] = object;
		sess->object_count++;
		sess->generations[slot]++;
		return (u64)sess->generations[slot] << 32 | (slot + 1);
	}
	return 0;
}

static int kpass_object_copy(struct kpass_object *object, u64 off, u64 len,
			     void __user *dest)
{
	struct kpass_extent *extent;
	u64 end = off + len;

	list_for_each_entry(extent, &object->extents, list) {
		u64 first = max(off, extent->start);
		u64 last = min(end, extent->start + extent->length);
		void *mapped;
		unsigned long left;

		if (first >= last)
			continue;
		mapped = kmap_local_page(extent->backing->page);
		left = copy_to_user(dest + first - off,
				    mapped + extent->offset + first - extent->start,
				    last - first);
		kunmap_local(mapped);
		if (left)
			return -EFAULT;
	}
	return len;
}

static void kpass_request_finish(struct kpass_request *req, int result)
{
	if (req->ready)
		return;
	list_del_init(&req->list);
	req->ready = true;
	req->result = result;
	req->sock = NULL;
	/* Tests exercise the same state machine without fabricating io_uring. */
	if (req->ioucmd)
		io_uring_cmd_complete_in_task(req->ioucmd,
					     kpass_stream_complete_task);
}

static bool kpass_is_range(u8 op)
{
	return op == KPASS_OP_READ_STREAM || op == KPASS_OP_KEEP ||
	       op == KPASS_OP_DISCARD;
}

/* Decisions are exclusive. Multiple non-consuming header reads may overlap. */
static int kpass_range_conflict(struct kpass_request *req)
{
	struct kpass_request *other;
	u64 start = req->cmd.stream.offset;
	u64 end = start + req->cmd.stream.length;

	list_for_each_entry(other, &req->session->requests, list) {
		if (other == req || other->sock != req->sock ||
		    !kpass_is_range(other->cmd.op))
			continue;
		if (req->cmd.op == KPASS_OP_READ_STREAM &&
		    other->cmd.op == KPASS_OP_READ_STREAM)
			continue;
		if (start < other->cmd.stream.offset + other->cmd.stream.length &&
		    other->cmd.stream.offset < end)
			return -EBUSY;
	}
	return 0;
}

/* Return 1 for a complete available range, 0 for future data, or a hole error. */
static int kpass_stream_available(struct kpass_sock *ksock, u64 off, u64 len)
{
	struct kpass_extent *extent;
	u64 end = off + len;

	list_for_each_entry(extent, &ksock->stream, list) {
		if (extent->start + extent->length <= off)
			continue;
		if (extent->start > off)
			return -ENODATA;
		off = min(end, extent->start + extent->length);
		if (off == end)
			return 1;
	}
	return off < ksock->rx_next ? -ENODATA : 0;
}

static int kpass_stream_snapshot(struct kpass_request *req)
{
	struct kpass_extent *extent;
	u64 off = req->cmd.stream.offset;
	u64 end = off + req->cmd.stream.length;
	int ret;

	ret = kpass_stream_available(req->sock, off, end - off);
	if (ret != 1)
		return ret;
	list_for_each_entry(extent, &req->sock->stream, list) {
		u64 first = max(off, extent->start);
		u64 last = min(end, extent->start + extent->length);
		struct kpass_extent *copy;

		if (first >= last)
			continue;
		if (req->session->extent_count == KPASS_MAX_EXTENTS)
			return -ENOSPC;
		copy = kmalloc_obj(*copy);
		if (!copy)
			return -ENOMEM;
		*copy = *extent;
		refcount_inc(&copy->backing->refs);
		copy->start = first - off;
		copy->offset += first - extent->start;
		copy->length = last - first;
		list_add_tail(&copy->list, &req->object->extents);
		req->session->extent_count++;
	}
	req->object->length = end - off;
	return 1;
}

/* Move currently available bytes into a pending KEEP, or release a DISCARD. */
static int kpass_stream_take(struct kpass_request *req)
{
	struct kpass_sock *ksock = req->sock;
	u64 off = req->cmd.stream.offset + req->progress;
	u64 end = req->cmd.stream.offset + req->cmd.stream.length;

	while (off < end && off < ksock->rx_next) {
		struct kpass_extent *extent, *found = NULL;
		u64 last;

		list_for_each_entry(extent, &ksock->stream, list) {
			if (extent->start + extent->length <= off)
				continue;
			if (extent->start <= off)
				found = extent;
			break;
		}
		if (!found)
			return -ENODATA;
		extent = found;
		if (extent->start < off) {
			extent = kpass_extent_split(req->session, extent, off);
			if (IS_ERR(extent))
				return PTR_ERR(extent);
		}
		last = min(end, extent->start + extent->length);
		if (last < extent->start + extent->length) {
			struct kpass_extent *right;

			right = kpass_extent_split(req->session, extent, last);
			if (IS_ERR(right))
				return PTR_ERR(right);
		}
		if (req->cmd.op == KPASS_OP_KEEP) {
			extent->start -= req->cmd.stream.offset;
			list_move_tail(&extent->list, &req->object->extents);
			req->object->length += last - off;
		} else {
			kpass_extent_free(req->session, extent);
		}
		req->progress += last - off;
		off = last;
	}
	return off == end;
}

static int kpass_stream_actor(read_descriptor_t *desc, struct sk_buff *skb,
			      unsigned int offset, size_t len)
{
	struct kpass_sock *ksock = desc->arg.data;
	struct kpass_session *sess = ksock->session;
	unsigned int consumed = 0;
	int ret = 0;

	if (ksock->rx_next == U64_MAX)
		return -EOVERFLOW;
	len = min(len, desc->count);
	len = min_t(u64, len, U64_MAX - ksock->rx_next);
	while (len) {
		struct kpass_sg_entry sg;
		struct kpass_buf capture = { .sgvec = &sg, .sg_max = 1 };
		struct kpass_request *req, *discard = NULL;
		struct kpass_backing *backing;
		struct kpass_extent *extent;
		u64 stop = ksock->rx_next + len;

		/* Skip known unwanted bytes even when retained pages fill the budget. */
		list_for_each_entry(req, &sess->requests, list) {
			u64 cursor = req->cmd.stream.offset + req->progress;

			if (req->sock != ksock || req->cmd.op != KPASS_OP_DISCARD)
				continue;
			if (cursor == ksock->rx_next &&
			    req->progress < req->cmd.stream.length) {
				discard = req;
				break;
			}
			if (cursor > ksock->rx_next)
				stop = min(stop, cursor);
		}
		if (discard) {
			ret = min_t(u64, len, discard->cmd.stream.length -
				    discard->progress);
			discard->progress += ret;
			goto consumed;
		}
		/* Leave two metadata slots for decisions that split buffered data. */
		if (sess->backing_bytes >= sess->backing_limit ||
		    sess->extent_count >= KPASS_MAX_EXTENTS - 2) {
			ret = -ENOBUFS;
			break;
		}
		extent = kmalloc_obj(*extent);
		backing = kmalloc_obj(*backing);
		if (!extent || !backing) {
			kfree(extent);
			kfree(backing);
			ret = -ENOMEM;
			break;
		}
		ret = kpass_rx_skb(&capture, skb, offset,
				   min_t(u64, PAGE_SIZE, stop - ksock->rx_next),
				   false);
		if (ret <= 0) {
			kfree(extent);
			kfree(backing);
			break;
		}
		/* Conservative: charge the full compound page per capture ref. */
		backing->charge = page_size(compound_head(sg.page));
		if (backing->charge > sess->backing_limit - sess->backing_bytes) {
			put_page(sg.page);
			kfree(extent);
			kfree(backing);
			ret = -ENOBUFS;
			break;
		}
		backing->page = sg.page;
		refcount_set(&backing->refs, 1);
		sess->backing_bytes += backing->charge;
		sess->copied_bytes += capture.copied_len;
		extent->backing = backing;
		extent->start = ksock->rx_next;
		extent->offset = sg.offset;
		extent->length = ret;
		list_add_tail(&extent->list, &ksock->stream);
		sess->extent_count++;
consumed:
		ksock->rx_next += ret;
		desc->count -= ret;
		desc->written += ret;
		consumed += ret;
		offset += ret;
		len -= ret;
	}
	return consumed ?: ret;
}

static void kpass_stream_receive(struct kpass_sock *ksock)
{
	struct sock *sk = ksock->sock->sk;
	read_descriptor_t desc = { .count = SZ_64K, .arg.data = ksock };
	int ret;

	if (ksock->listening || ksock->rx_eof || ksock->error)
		return;
	lock_sock(sk);
	if (sk->sk_state == TCP_ESTABLISHED || sk->sk_state == TCP_CLOSE_WAIT)
		ksock->connected = true;
	if (!ksock->connected) {
		release_sock(sk);
		return;
	}
	ret = tcp_read_sock(sk, &desc, kpass_stream_actor);
	if (ret < 0 && ret != -ENOBUFS)
		ksock->error = ret;
	if (sk->sk_err)
		ksock->error = -sk->sk_err;
	if ((sk->sk_shutdown & RCV_SHUTDOWN) &&
	    skb_queue_empty(&sk->sk_receive_queue))
		ksock->rx_eof = true;
	release_sock(sk);
	if (!desc.count)
		queue_work(ksock->session->wq, &ksock->session->stream_work);
}

static int kpass_object_send(struct kpass_request *req)
{
	struct kpass_extent *extent;
	u64 off = req->cmd.stream.offset + req->progress;
	u64 end = req->cmd.stream.offset + req->cmd.stream.length;

	list_for_each_entry(extent, &req->object->extents, list) {
		u64 first = max(off, extent->start);
		u64 last = min(end, extent->start + extent->length);
		struct msghdr msg = { .msg_flags = MSG_SPLICE_PAGES | MSG_DONTWAIT };
		struct bio_vec bvec;
		int ret;

		if (first >= last)
			continue;
		bvec_set_page(&bvec, extent->backing->page, last - first,
			      extent->offset + first - extent->start);
		iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, last - first);
		if (last < end)
			msg.msg_flags |= MSG_MORE;
		ret = sock_sendmsg(req->sock->sock, &msg);
		if (ret == -EAGAIN)
			return 0;
		if (ret <= 0)
			return ret ?: -EPIPE;
		req->progress += ret;
		off += ret;
		if (off < last) {
			queue_work(req->session->wq, &req->session->stream_work);
			return 0;
		}
	}
	return 1;
}

static int kpass_stream_accept(struct kpass_request *req)
{
	struct socket *sock;
	struct kpass_sock *accepted;
	int ret;

	ret = kernel_accept(req->sock->sock, &sock, O_NONBLOCK);
	if (ret)
		return ret;
	accepted = kpass_sock_create(req->session);
	if (IS_ERR(accepted)) {
		sock_release(sock);
		return PTR_ERR(accepted);
	}
	kpass_sock_unhook(accepted);
	sock_release(accepted->sock);
	accepted->sock = sock;
	accepted->connected = true;
	kpass_sock_hook(accepted);
	queue_work(req->session->wq, &req->session->stream_work);
	return accepted->id;
}

static void kpass_stream_process(struct kpass_session *sess)
{
	struct kpass_request *req, *next;
	DECLARE_BITMAP(tx_blocked, CEPH_KPASS_MAX_SOCKETS);

	bitmap_zero(tx_blocked, CEPH_KPASS_MAX_SOCKETS);
	list_for_each_entry_safe(req, next, &sess->requests, list) {
		struct kpass_sock *ksock = req->sock;
		int ret = 0;

		switch (req->cmd.op) {
		case KPASS_OP_READ_STREAM:
			ret = kpass_stream_snapshot(req);
			break;
		case KPASS_OP_KEEP:
		case KPASS_OP_DISCARD:
			ret = kpass_stream_take(req);
			break;
		case KPASS_OP_OBJECT_SEND:
			if (test_bit(ksock->id, tx_blocked))
				continue;
			ret = kpass_object_send(req);
			if (!ret)
				set_bit(ksock->id, tx_blocked);
			break;
		case KPASS_OP_SOCK_CONNECT:
			/* A peer may send FIN before the connect worker runs. */
			if (ksock->connected ||
			    ksock->sock->sk->sk_state == TCP_ESTABLISHED ||
			    ksock->sock->sk->sk_state == TCP_CLOSE_WAIT) {
				ksock->connected = true;
				kpass_request_finish(req, 0);
			} else if (READ_ONCE(ksock->sock->sk->sk_err)) {
				kpass_request_finish(req,
						     -READ_ONCE(ksock->sock->sk->sk_err));
			}
			continue;
		case KPASS_OP_SOCK_ACCEPT:
			ret = kpass_stream_accept(req);
			if (ret != -EAGAIN)
				kpass_request_finish(req, ret);
			continue;
		default:
			ret = -EINVAL;
			break;
		}
		if (!ret && kpass_is_range(req->cmd.op)) {
			if (ksock->error)
				ret = ksock->error;
			else if (ksock->rx_eof)
				ret = -ENODATA;
		}
		if (ret)
			kpass_request_finish(req, ret < 0 ? ret : req->cmd.stream.length);
	}
}

static void kpass_stream_work_fn(struct work_struct *work)
{
	struct kpass_session *sess = container_of(work, struct kpass_session,
						 stream_work);
	unsigned int i;

	mutex_lock(&sess->stream_lock);
	if (sess->legacy)
		goto out;
	kpass_stream_process(sess);
	for (i = 0; i < CEPH_KPASS_MAX_SOCKETS; i++)
		if (sess->sockets[i])
			kpass_stream_receive(sess->sockets[i]);
	kpass_stream_process(sess);
out:
	mutex_unlock(&sess->stream_lock);
}

static void kpass_stream_close(struct kpass_sock *ksock)
{
	struct kpass_session *sess = ksock->session;
	struct kpass_request *req, *next;

	list_for_each_entry_safe(req, next, &sess->requests, list)
		if (req->sock == ksock)
			kpass_request_finish(req, -ECANCELED);
	kpass_extents_free(sess, &ksock->stream);
}

static int kpass_stream_cancel(struct io_uring_cmd *ioucmd)
{
	struct kpass_cmd_private *pdu = io_uring_cmd_to_pdu(ioucmd,
							  struct kpass_cmd_private);
	struct kpass_session *sess = ioucmd->file->private_data;
	struct kpass_request *req;

	mutex_lock(&sess->stream_lock);
	req = pdu->request;
	if (req) {
		req->canceled = true;
		kpass_request_finish(req, -ECANCELED);
	}
	mutex_unlock(&sess->stream_lock);
	return 0;
}

static void kpass_stream_complete_task(struct io_tw_req tw_req,
				       io_tw_token_t tw)
{
	struct io_uring_cmd *ioucmd = io_uring_cmd_from_tw(tw_req);
	struct kpass_cmd_private *pdu = io_uring_cmd_to_pdu(ioucmd,
							  struct kpass_cmd_private);
	struct kpass_request *req = pdu->request;
	struct kpass_session *sess = req->session;
	u64 handle = 0;
	int ret = req->result;
	bool cqe32 = req->cqe32;

	mutex_lock(&sess->stream_lock);
	if (tw.cancel || req->canceled)
		ret = -ECANCELED;
	if (ret >= 0 && req->cmd.op == KPASS_OP_KEEP) {
		handle = kpass_object_publish(sess, req->object);
		if (handle)
			req->object = NULL; /* table adopts the completed object */
		else
			ret = -ENOSPC;
	}
	if (ret >= 0 && (req->cmd.op == KPASS_OP_READ_STREAM ||
			 req->cmd.op == KPASS_OP_OBJECT_READ))
		ret = kpass_object_copy(req->object,
					req->cmd.op == KPASS_OP_READ_STREAM ? 0 :
					req->cmd.stream.offset,
					req->cmd.stream.length,
					u64_to_user_ptr(req->cmd.stream.addr));
	kpass_object_put(sess, req->object);
	sess->pending_count--;
	pdu->request = NULL;
	kfree(req);
	queue_work(sess->wq, &sess->stream_work);
	mutex_unlock(&sess->stream_lock);
	kpass_stream_done(ioucmd, ret, handle,
			   IO_URING_CMD_TASK_WORK_ISSUE_FLAGS, cqe32);
}

static int kpass_stream_stat(struct kpass_session *sess, struct kpass_sock *ksock,
			     u64 addr)
{
	struct kpass_stream_stat stat = {
		.received = ksock->rx_next,
		.backing_bytes = sess->backing_bytes,
		.backing_limit = sess->backing_limit,
		.copied_bytes = sess->copied_bytes,
		.objects = sess->object_count,
		.pending = sess->pending_count,
		.eof = ksock->rx_eof,
		.error = ksock->error,
		.extents = sess->extent_count,
	};
	struct kpass_extent *extent;

	list_for_each_entry(extent, &ksock->stream, list)
		stat.buffered += extent->length;
	return copy_to_user(u64_to_user_ptr(addr), &stat, sizeof(stat)) ?
		-EFAULT : 0;
}

/* Called in task work with the ring lock held, before stream_lock is acquired. */
static void kpass_stream_issue_task(struct io_tw_req tw_req, io_tw_token_t tw)
{
	struct io_uring_cmd *ioucmd = io_uring_cmd_from_tw(tw_req);
	struct kpass_cmd_private *pdu = io_uring_cmd_to_pdu(ioucmd,
							  struct kpass_cmd_private);
	struct kpass_session *sess = ioucmd->file->private_data;
	struct kpass_request *req = pdu->request;
	const struct kpass_sqe_cmd *cmd = &req->cmd;
	struct kpass_sock *ksock;
	struct kpass_object *object;
	int ret = -EINVAL;

	if (tw.cancel) {
		bool cqe32 = req->cqe32;

		pdu->request = NULL;
		kfree(req);
		kpass_stream_done(ioucmd, -ECANCELED, 0,
				   IO_URING_CMD_TASK_WORK_ISSUE_FLAGS, cqe32);
		return;
	}
	io_uring_cmd_mark_cancelable(ioucmd, IO_URING_CMD_TASK_WORK_ISSUE_FLAGS);
	mutex_lock(&sess->stream_lock);
	sess->pending_count++;
	if (sess->pending_count > KPASS_MAX_PENDING &&
	    (kpass_is_range(cmd->op) || cmd->op == KPASS_OP_OBJECT_READ ||
	     cmd->op == KPASS_OP_OBJECT_SEND || cmd->op == KPASS_OP_SOCK_ACCEPT ||
	     cmd->op == KPASS_OP_SOCK_CONNECT)) {
		ret = -ENOSPC;
		goto finish;
	}
	if (cmd->flags || sess->legacy)
		goto finish;
	if (cmd->op >= KPASS_OP_READ_STREAM &&
	    (cmd->stream.reserved[0] || cmd->stream.reserved[1]))
		goto finish;
	ksock = kpass_get_sock(sess, cmd->sock_id);
	req->sock = ksock;

	switch (cmd->op) {
	case KPASS_OP_INIT:
		if (!bitmap_empty(sess->sock_bitmap, CEPH_KPASS_MAX_SOCKETS) ||
		    sess->object_count || cmd->init.buf_size > KPASS_MAX_BUF_SIZE ||
		    !cmd->init.num_buffers)
			break;
		ret = kpass_init_buffer_pool(sess, cmd->init.num_buffers,
					     cmd->init.buf_size);
		if (!ret)
			WRITE_ONCE(sess->legacy, true);
		break;
	case KPASS_OP_SOCK_CREATE:
		ksock = kpass_sock_create(sess);
		ret = IS_ERR(ksock) ? PTR_ERR(ksock) : ksock->id;
		break;
	case KPASS_OP_SOCK_LISTEN:
		if (ksock)
			ret = kpass_sock_listen(ksock, cmd->listen.port,
						cmd->listen.backlog);
		break;
	case KPASS_OP_SOCK_CONNECT:
		if (!ksock || ksock->connected || ksock->listening)
			break;
		ret = kpass_sock_connect(ksock, cmd->connect.family,
					 cmd->connect.addr, cmd->connect.port);
		if (!ret)
			goto pending;
		break;
	case KPASS_OP_SOCK_ACCEPT:
		if (!ksock || !ksock->listening)
			break;
		goto pending;
	case KPASS_OP_SOCK_CLOSE:
		if (ksock) {
			kpass_stream_close(ksock);
			kpass_sock_destroy(ksock);
			ret = 0;
		}
		break;
	case KPASS_OP_LIMIT:
		if (cmd->stream.length < PAGE_SIZE)
			break;
		if (cmd->stream.length < sess->backing_bytes) {
			ret = -EBUSY;
			break;
		}
		sess->backing_limit = cmd->stream.length;
		ret = 0;
		break;
	case KPASS_OP_STREAM_STAT:
		if (ksock)
			ret = kpass_stream_stat(sess, ksock, cmd->stream.addr);
		break;
	case KPASS_OP_CANCEL: {
		struct kpass_request *target;

		ret = -ENOENT;
		list_for_each_entry(target, &sess->requests, list) {
			if (target->cmd.tag != cmd->stream.object ||
			    io_uring_cmd_ctx_handle(target->ioucmd) !=
			    io_uring_cmd_ctx_handle(ioucmd))
				continue;
			target->canceled = true;
			kpass_request_finish(target, -ECANCELED);
			ret = 0;
			break;
		}
		break;
	}
	case KPASS_OP_OBJECT_FREE:
		object = kpass_object_lookup(sess, cmd->stream.object);
		ret = -ESTALE;
		if (object) {
			sess->objects[(u32)cmd->stream.object - 1] = NULL;
			sess->object_count--;
			kpass_object_put(sess, object);
			ret = 0;
		}
		break;
	case KPASS_OP_OBJECT_READ:
	case KPASS_OP_OBJECT_SEND:
		object = kpass_object_lookup(sess, cmd->stream.object);
		if (!object) {
			ret = -ESTALE;
			break;
		}
		if (cmd->stream.offset > object->length ||
		    cmd->stream.length > object->length - cmd->stream.offset ||
		    cmd->stream.length > INT_MAX)
			break;
		if (cmd->op == KPASS_OP_OBJECT_SEND &&
		    (!ksock || !ksock->connected)) {
			ret = -ENOTCONN;
			break;
		}
		refcount_inc(&object->refs);
		req->object = object;
		if (cmd->op == KPASS_OP_OBJECT_SEND && cmd->stream.length)
			goto pending;
		ret = cmd->stream.length;
		break;
	case KPASS_OP_READ_STREAM:
	case KPASS_OP_KEEP:
	case KPASS_OP_DISCARD:
		if (!ksock || ksock->listening ||
		    !cmd->stream.length || cmd->stream.length > INT_MAX ||
		    cmd->stream.offset > U64_MAX - cmd->stream.length)
			break;
		ret = kpass_range_conflict(req);
		if (ret)
			break;
		ret = kpass_stream_available(ksock, cmd->stream.offset,
					     cmd->stream.length);
		if (ret < 0)
			break;
		if (cmd->op != KPASS_OP_DISCARD) {
			req->object = kpass_object_alloc();
			if (!req->object) {
				ret = -ENOMEM;
				break;
			}
		}
		goto pending;
	default:
		break;
	}
finish:
	kpass_request_finish(req, ret);
	goto out;
pending:
	list_add_tail(&req->list, &sess->requests);
	kpass_stream_process(sess);
out:
	queue_work(sess->wq, &sess->stream_work);
	mutex_unlock(&sess->stream_lock);
}

static int kpass_stream_command(struct io_uring_cmd *ioucmd,
				unsigned int issue_flags)
{
	const struct kpass_sqe_cmd *cmd =
		io_uring_sqe128_cmd(ioucmd->sqe, struct kpass_sqe_cmd);
	struct kpass_cmd_private *pdu = io_uring_cmd_to_pdu(ioucmd,
							  struct kpass_cmd_private);
	struct kpass_request *req;

	if (!(issue_flags & IO_URING_F_CQE32) && cmd->op != KPASS_OP_INIT)
		return -EOPNOTSUPP;
	if (ioucmd->cmd_op || ioucmd->flags & IORING_URING_CMD_MASK ||
	    ioucmd->sqe->flags & IOSQE_CQE_SKIP_SUCCESS)
		return kpass_stream_done(ioucmd, -EINVAL, 0, issue_flags,
					  issue_flags & IO_URING_F_CQE32);
	req = kzalloc_obj(*req, issue_flags & IO_URING_F_NONBLOCK ?
			 GFP_NOWAIT : GFP_KERNEL);
	if (!req) {
		if (issue_flags & IO_URING_F_NONBLOCK)
			return -EAGAIN;
		return kpass_stream_done(ioucmd, -ENOMEM, 0, issue_flags,
					  issue_flags & IO_URING_F_CQE32);
	}
	req->cmd = *cmd;
	req->cmd.tag = ioucmd->sqe->user_data;
	req->session = ioucmd->file->private_data;
	req->ioucmd = ioucmd;
	req->cqe32 = issue_flags & IO_URING_F_CQE32;
	INIT_LIST_HEAD(&req->list);
	pdu->request = req;
	io_uring_cmd_complete_in_task(ioucmd, kpass_stream_issue_task);
	return -EIOCBQUEUED;
}
