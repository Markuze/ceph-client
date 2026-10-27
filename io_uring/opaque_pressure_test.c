// SPDX-License-Identifier: GPL-2.0
/* Exercise the real range/collector paths with a full store and a TCP peer. */
#include <kunit/test.h>
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <net/tcp.h>

#include "io_uring.h"
#include "cancel.h"
#include "mpscq.h"
#include "opaque_internal.h"

struct pressure_range {
	struct list_head list;
	struct io_opaque_req op;
	struct io_kiocb req;
};

struct pressure_fixture {
	struct kunit *test;
	struct io_ring_ctx *ctx;
	struct io_opaque_store store;
	struct io_opaque_stream stream;
	struct io_opaque_req collector;
	struct io_kiocb req;
	struct socket *listener, *tx, *rx;
	struct file *file;
	struct list_head ranges, held;
	wait_queue_entry_t wake;
	unsigned int wakes;
	bool watching;
};

static int pressure_wake(wait_queue_entry_t *entry, unsigned int mode,
			 int flags, void *key)
{
	struct pressure_fixture *f = container_of(entry, struct pressure_fixture, wake);

	f->wakes++;
	return 0;
}

static void pressure_cleanup(void *arg)
{
	struct pressure_fixture *f = arg;
	struct pressure_range *range;

	list_del_init(&f->collector.wait);
	cancel_delayed_work_sync(&f->store.retry_work);
	if (f->watching)
		remove_wait_queue(sk_sleep(f->rx->sk), &f->wake);
	list_for_each_entry(range, &f->ranges, list)
		io_opaque_data_put(&f->store, range->op.data);
	io_opaque_extents_free(&f->store, &f->stream.extents);
	io_opaque_extents_free(&f->store, &f->held);
	KUNIT_EXPECT_EQ(f->test, atomic64_read(&f->store.bytes), 0LL);
	KUNIT_EXPECT_EQ(f->test, atomic_read(&f->store.extents), 0);
	if (f->file)
		fput(f->file);
	else if (f->rx)
		sock_release(f->rx);
	if (f->tx)
		sock_release(f->tx);
	if (f->listener)
		sock_release(f->listener);
}

static int pressure_init(struct kunit *test)
{
	struct pressure_fixture *f;
	struct sockaddr_in addr = {
		.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	test->priv = f;
	f->test = test;
	f->ctx = kunit_kzalloc(test, sizeof(*f->ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f->ctx);
	mutex_init(&f->ctx->uring_lock);
	INIT_LIST_HEAD(&f->ctx->opaque_waits);
	f->ctx->flags = IORING_SETUP_DEFER_TASKRUN;
	mpscq_init(&f->ctx->work_list, &f->ctx->work_head);
	f->store.config.hard_limit = 128 * PAGE_SIZE;
	f->store.config.max_extents = 256;
	spin_lock_init(&f->store.wait_lock);
	INIT_LIST_HEAD(&f->store.budget_waits);
	INIT_DELAYED_WORK(&f->store.retry_work, io_opaque_retry_work);
	mutex_init(&f->stream.lock);
	f->stream.store = &f->store;
	f->stream.collector = &f->collector;
	f->stream.decisions = RB_ROOT_CACHED;
	INIT_LIST_HEAD(&f->stream.extents);
	INIT_LIST_HEAD(&f->stream.reads);
	INIT_LIST_HEAD(&f->ranges);
	INIT_LIST_HEAD(&f->held);
	INIT_LIST_HEAD(&f->collector.wait);
	INIT_LIST_HEAD(&f->collector.cancel);
	f->collector.store = &f->store;
	f->collector.stream = &f->stream;
	f->collector.req = &f->req;
	f->collector.retry_at = jiffies;
	f->req.ctx = f->ctx;
	f->req.async_data = &f->collector;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, pressure_cleanup, f), 0);

	rtnl_lock();
	ret = dev_change_flags(init_net.loopback_dev, IFF_UP, NULL);
	rtnl_unlock();
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, sock_create_kern(&init_net, AF_INET, SOCK_STREAM,
					    IPPROTO_TCP, &f->listener), 0);
	KUNIT_ASSERT_EQ(test, kernel_bind(f->listener, (struct sockaddr_unsized *)&addr,
					sizeof(addr)), 0);
	KUNIT_ASSERT_EQ(test, kernel_listen(f->listener, 1), 0);
	KUNIT_ASSERT_GT(test, kernel_getsockname(f->listener, (struct sockaddr *)&addr), 0);
	KUNIT_ASSERT_EQ(test, sock_create_kern(&init_net, AF_INET, SOCK_STREAM,
					    IPPROTO_TCP, &f->tx), 0);
	f->tx->sk->sk_sndtimeo = HZ;
	KUNIT_ASSERT_EQ(test, kernel_connect(f->tx, (struct sockaddr_unsized *)&addr,
					   sizeof(addr), 0), 0);
	KUNIT_ASSERT_EQ(test, kernel_accept(f->listener, &f->rx, O_NONBLOCK), 0);
	f->file = sock_alloc_file(f->rx, O_NONBLOCK, NULL);
	if (IS_ERR(f->file)) {
		ret = PTR_ERR(f->file);
		f->file = NULL;
		f->rx = NULL; /* sock_alloc_file released it. */
		return ret;
	}
	f->req.file = f->file;
	f->stream.file = f->file;
	init_waitqueue_func_entry(&f->wake, pressure_wake);
	add_wait_queue(sk_sleep(f->rx->sk), &f->wake);
	f->watching = true;
	return 0;
}

static struct io_opaque_req *pressure_range(struct kunit *test, u16 cmd,
					  u64 offset, u32 length)
{
	struct pressure_fixture *f = test->priv;
	struct pressure_range *range = kunit_kzalloc(test, sizeof(*range), GFP_KERNEL);
	struct io_opaque_req *op;

	KUNIT_ASSERT_NOT_NULL(test, range);
	op = &range->op;
	op->store = &f->store;
	op->stream = &f->stream;
	op->req = &range->req;
	op->op = cmd;
	op->offset = offset;
	op->length = length;
	INIT_LIST_HEAD(&op->wait);
	INIT_LIST_HEAD(&op->cancel);
	INIT_LIST_HEAD(&op->read);
	RB_CLEAR_NODE(&op->decision);
	range->req.ctx = f->ctx;
	range->req.async_data = op;
	range->req.cqe.user_data = (unsigned long)op;
	list_add_tail(&range->list, &f->ranges);
	if (cmd != IORING_OPAQUE_DISCARD) {
		op->data = io_opaque_data_alloc();
		KUNIT_ASSERT_NOT_NULL(test, op->data);
	}
	return op;
}

static void pressure_start(struct kunit *test, struct io_opaque_req *op)
{
	struct pressure_fixture *f = test->priv;
	int ret;

	mutex_lock(&f->ctx->uring_lock);
	ret = io_opaque_range_start(op);
	mutex_unlock(&f->ctx->uring_lock);
	KUNIT_ASSERT_EQ(test, ret, IOU_ISSUE_SKIP_COMPLETE);
}

static int pressure_recv(struct pressure_fixture *f)
{
	return io_opaque_recv(&f->req, IO_URING_F_UNLOCKED | IO_URING_F_MULTISHOT);
}

static void pressure_send(struct kunit *test, u8 byte, size_t length)
{
	struct pressure_fixture *f = test->priv;
	u8 data[128];
	struct kvec vec = { .iov_base = data, .iov_len = length };
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL };

	KUNIT_ASSERT_LE(test, length, sizeof(data));
	memset(data, byte, length);
	KUNIT_ASSERT_EQ(test, kernel_sendmsg(f->tx, &msg, &vec, 1, length), (int)length);
}

/* Seed real charged backings; the fourth belongs to another object/stream. */
static void pressure_seed(struct kunit *test, bool extent_limit)
{
	struct pressure_fixture *f = test->priv;
	int i;

	for (i = 0; i < 4; i++) {
		struct page *page = alloc_page(GFP_KERNEL);
		struct io_opaque_extent *extent;
		void *addr;

		KUNIT_ASSERT_NOT_NULL(test, page);
		addr = kmap_local_page(page);
		memset(addr, i, PAGE_SIZE);
		kunmap_local(addr);
		extent = io_opaque_extent_new(&f->store, page, 0, 32, i * 32, false);
		if (IS_ERR(extent))
			put_page(page);
		KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
		list_add_tail(&extent->list, i == 3 ? &f->held : &f->stream.extents);
	}
	f->stream.next = 96;
	f->stream.undecided = 96;
	if (extent_limit)
		f->store.config.max_extents = 6; /* two descriptors reserved for splits */
	else
		f->store.config.hard_limit = 4 * PAGE_SIZE;
	/* The fixture has already observed the nonterminal budget notification. */
	f->collector.budget_reported = true;
	f->collector.offset = 96;
}

static void pressure_fill(struct kunit *test, bool extent_limit)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *first;

	pressure_seed(test, extent_limit);
	pressure_send(test, 0xd3, 32);
	first = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 128);
	pressure_start(test, first);
	KUNIT_ASSERT_EQ(test, first->progress, 96ULL);
	KUNIT_ASSERT_EQ(test, pressure_recv(f), IOU_RETRY);
	KUNIT_ASSERT_EQ(test, first->phase, IO_OPAQUE_QUEUED);
	KUNIT_ASSERT_EQ(test, first->result, -ENOBUFS);
	KUNIT_ASSERT_EQ(test, first->progress, 0ULL);
	KUNIT_ASSERT_EQ(test, f->collector.phase, IO_OPAQUE_BUDGET_POLL);
	KUNIT_ASSERT_PTR_EQ(test, mpscq_pop(&f->ctx->work_list, &f->ctx->work_head),
			    &first->req->io_task_work.node);
	f->wakes = 0;
}

static void pressure_retry(struct kunit *test, bool extent_limit)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;
	int i;

	pressure_fill(test, extent_limit);
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 128);
	pressure_start(test, op);
	KUNIT_EXPECT_EQ(test, op->progress, 0ULL);
	KUNIT_EXPECT_EQ(test, f->stream.undecided, 96ULL);
	KUNIT_EXPECT_EQ(test, f->collector.phase, IO_OPAQUE_BUDGET_POLL);
	KUNIT_EXPECT_EQ(test, f->wakes, 0U);
	for (i = 0; i < 8; i++) {
		KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_RETRY);
		KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_RANGE_WAIT);
		KUNIT_EXPECT_EQ(test, op->progress, 0ULL);
		KUNIT_EXPECT_EQ(test, f->stream.undecided, 96ULL);
		KUNIT_EXPECT_EQ(test, f->collector.phase, IO_OPAQUE_BUDGET_POLL);
		KUNIT_EXPECT_FALSE(test, list_empty(&f->collector.wait));
	}
	KUNIT_EXPECT_PTR_EQ(test, mpscq_pop(&f->ctx->work_list, &f->ctx->work_head), NULL);
}

static void pressure_retry_bytes(struct kunit *test)
{
	pressure_retry(test, false);
}

static void pressure_retry_extents(struct kunit *test)
{
	pressure_retry(test, true);
}

static void pressure_expect_object(struct kunit *test, struct io_opaque_req *op)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_extent *extent;
	u32 cursor = 0;

	KUNIT_ASSERT_EQ(test, op->phase, IO_OPAQUE_QUEUED);
	KUNIT_ASSERT_EQ(test, op->result, 128);
	KUNIT_ASSERT_EQ(test, op->data->length, 128ULL);
	KUNIT_EXPECT_TRUE(test, RB_EMPTY_ROOT(&f->stream.decisions.rb_root));
	KUNIT_EXPECT_EQ(test, f->stream.undecided, 0ULL);
	KUNIT_EXPECT_EQ(test, f->stream.next, 128ULL);
	list_for_each_entry(extent, &op->data->extents, list) {
		u32 i;

		KUNIT_EXPECT_EQ(test, extent->start, (u64)cursor);
		for (i = 0; i < extent->length; i++, cursor++) {
			u32 pos = extent->offset + i;
			u8 *addr = kmap_local_page(extent->backing->page + (pos >> PAGE_SHIFT));
			u8 byte = addr[offset_in_page(pos)];

			kunmap_local(addr);
			KUNIT_EXPECT_EQ(test, byte, (u8)(cursor < 96 ? cursor / 32 : 0xd3));
		}
	}
	KUNIT_EXPECT_EQ(test, cursor, 128U);
	KUNIT_EXPECT_PTR_EQ(test, mpscq_pop(&f->ctx->work_list, &f->ctx->work_head),
			   &op->req->io_task_work.node);
	KUNIT_EXPECT_PTR_EQ(test, mpscq_pop(&f->ctx->work_list, &f->ctx->work_head), NULL);
}

static void pressure_release(struct kunit *test, bool extent_limit)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;

	pressure_fill(test, extent_limit);
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 128);
	pressure_start(test, op);
	KUNIT_ASSERT_EQ(test, op->progress, 0ULL);
	io_opaque_extents_free(&f->store, &f->held);
	KUNIT_EXPECT_EQ(test, f->wakes, 1U);
	KUNIT_EXPECT_TRUE(test, list_empty(&f->collector.wait));
	KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_RETRY);
	pressure_expect_object(test, op);
}

static void pressure_release_bytes(struct kunit *test)
{
	pressure_release(test, false);
}

static void pressure_release_extents(struct kunit *test)
{
	pressure_release(test, true);
}

static void pressure_buffered_take(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;

	pressure_fill(test, true);
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 96);
	pressure_start(test, op);
	KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, op->result, 96);
	KUNIT_EXPECT_EQ(test, op->data->length, 96ULL);
	KUNIT_EXPECT_EQ(test, f->stream.undecided, 0ULL);
	KUNIT_EXPECT_EQ(test, f->stream.next, 96ULL);
	KUNIT_EXPECT_EQ(test, f->wakes, 0U);
}

static void pressure_buffered_inspect(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;

	pressure_fill(test, false);
	op = pressure_range(test, IORING_OPAQUE_INSPECT, 16, 40);
	pressure_start(test, op);
	KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, op->result, 40);
	KUNIT_EXPECT_EQ(test, op->data->length, 40ULL);
	KUNIT_EXPECT_EQ(test, f->stream.undecided, 96ULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&f->store.bytes), (s64)4 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, f->wakes, 0U);
}

static void pressure_discard_cursor(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;

	pressure_fill(test, false);
	op = pressure_range(test, IORING_OPAQUE_DISCARD, 96, 32);
	pressure_start(test, op);
	KUNIT_EXPECT_EQ(test, f->wakes, 1U);
	KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_RETRY);
	KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, op->result, 32);
	KUNIT_EXPECT_EQ(test, f->stream.next, 128ULL);
	KUNIT_EXPECT_EQ(test, f->stream.undecided, 96ULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&f->store.bytes), (s64)4 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->store.extents), 4);
}

static void pressure_discard_gap(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *later, *gap;

	pressure_fill(test, true);
	later = pressure_range(test, IORING_OPAQUE_DISCARD, 112, 16);
	pressure_start(test, later);
	KUNIT_EXPECT_EQ(test, f->wakes, 0U);
	KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_RETRY);
	KUNIT_EXPECT_EQ(test, later->phase, IO_OPAQUE_RANGE_WAIT);
	KUNIT_EXPECT_EQ(test, f->stream.next, 96ULL);
	gap = pressure_range(test, IORING_OPAQUE_DISCARD, 96, 16);
	pressure_start(test, gap);
	KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_RETRY);
	KUNIT_EXPECT_EQ(test, later->result, 16);
	KUNIT_EXPECT_EQ(test, gap->result, 16);
	KUNIT_EXPECT_EQ(test, later->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, gap->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, f->stream.next, 128ULL);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->store.extents), 4);
}

static void pressure_cancel_and_discard(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op, *discard;
	struct io_cancel_data cd = { .ctx = f->ctx };

	pressure_fill(test, false);
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 128);
	pressure_start(test, op);
	cd.data = op->req->cqe.user_data;
	KUNIT_EXPECT_EQ(test, io_opaque_cancel(f->ctx, &cd, IO_URING_F_UNLOCKED), 0);
	KUNIT_EXPECT_EQ(test, op->result, -ECANCELED);
	KUNIT_EXPECT_EQ(test, op->progress, 0ULL);
	KUNIT_EXPECT_EQ(test, f->stream.undecided, 96ULL);
	KUNIT_EXPECT_TRUE(test, RB_EMPTY_NODE(&op->decision));
	discard = pressure_range(test, IORING_OPAQUE_DISCARD, 0, 128);
	pressure_start(test, discard);
	KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_RETRY);
	KUNIT_EXPECT_EQ(test, discard->result, 128);
	KUNIT_EXPECT_EQ(test, discard->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, f->stream.undecided, 0ULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&f->store.bytes), (s64)PAGE_SIZE);
}

static void pressure_consumed_gap(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op, *discard;

	pressure_fill(test, false);
	discard = pressure_range(test, IORING_OPAQUE_DISCARD, 32, 32);
	pressure_start(test, discard);
	KUNIT_ASSERT_EQ(test, discard->result, 32);
	/* A competing CREATE reserves the just-released page before collection. */
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(&f->store, PAGE_SIZE, true));
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 128);
	pressure_start(test, op);
	KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, op->result, -ENODATA);
	KUNIT_EXPECT_EQ(test, op->progress, 0ULL);
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 32, 128);
	pressure_start(test, op);
	KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, op->result, -ENODATA);
	KUNIT_EXPECT_EQ(test, f->stream.undecided, 64ULL);
	io_opaque_uncharge(&f->store, PAGE_SIZE);
}

static void pressure_lost_wake_capacity(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;

	pressure_fill(test, false);
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 128);
	pressure_start(test, op);
	io_opaque_extents_free(&f->store, &f->held);
	KUNIT_ASSERT_EQ(test, f->wakes, 1U);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(&f->store, PAGE_SIZE, true));
	KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_RETRY);
	KUNIT_EXPECT_EQ(test, op->progress, 0ULL);
	KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_RANGE_WAIT);
	KUNIT_EXPECT_FALSE(test, list_empty(&f->collector.wait));
	io_opaque_uncharge(&f->store, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, f->wakes, 2U);
	KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_RETRY);
	pressure_expect_object(test, op);
}

static void pressure_allocation_backoff(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;
	unsigned long deadline;
	int i;

	pressure_fill(test, false);
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 128);
	pressure_start(test, op);
	io_opaque_extents_free(&f->store, &f->held);
	/* Drive the allocation-retry state without nondeterministic fault injection. */
	deadline = jiffies + 5 * HZ;
	f->collector.retry_at = deadline;
	f->collector.need_bytes = 0;
	for (i = 0; i < 8; i++) {
		KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_RETRY);
		KUNIT_EXPECT_EQ(test, f->collector.retry_at, deadline);
		KUNIT_EXPECT_EQ(test, op->progress, 0ULL);
		KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_RANGE_WAIT);
	}
	KUNIT_EXPECT_EQ(test, f->wakes, 1U);
	cancel_delayed_work_sync(&f->store.retry_work);
	f->collector.retry_at = jiffies - 1;
	queue_delayed_work(system_dfl_wq, &f->store.retry_work, 0);
	flush_delayed_work(&f->store.retry_work);
	KUNIT_EXPECT_EQ(test, f->wakes, 2U);
	KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_RETRY);
	pressure_expect_object(test, op);
}

static void pressure_socket_error(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;

	pressure_fill(test, false);
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 128);
	pressure_start(test, op);
	lock_sock(f->rx->sk);
	f->rx->sk->sk_err = ECONNRESET;
	release_sock(f->rx->sk);
	KUNIT_EXPECT_EQ(test, pressure_recv(f), -ECONNRESET);
	KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, op->result, -ECONNRESET);
	KUNIT_EXPECT_EQ(test, f->stream.undecided, 96ULL);
	KUNIT_EXPECT_EQ(test, op->data->length, 0ULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&f->collector.wait));
}

static void pressure_urgent_data(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;
	u8 byte = 1;
	struct kvec vec = { .iov_base = &byte, .iov_len = 1 };
	struct msghdr msg = { .msg_flags = MSG_OOB | MSG_DONTWAIT | MSG_NOSIGNAL };

	pressure_fill(test, false);
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 128);
	pressure_start(test, op);
	KUNIT_ASSERT_EQ(test, kernel_sendmsg(f->tx, &msg, &vec, 1, 1), 1);
	KUNIT_EXPECT_EQ(test, pressure_recv(f), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, op->result, -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, f->stream.undecided, 96ULL);
	KUNIT_EXPECT_EQ(test, op->data->length, 0ULL);
}

static void pressure_fin_buffered(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;

	pressure_fill(test, false);
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 128);
	pressure_start(test, op);
	KUNIT_ASSERT_EQ(test, kernel_sock_shutdown(f->tx, SHUT_WR), 0);
	KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_RETRY);
	KUNIT_EXPECT_FALSE(test, f->stream.eof);
	KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_RANGE_WAIT);
	io_opaque_extents_free(&f->store, &f->held);
	KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_COMPLETE);
	KUNIT_EXPECT_TRUE(test, f->stream.eof);
	pressure_expect_object(test, op);
}

static void pressure_eof_empty(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;

	pressure_seed(test, false);
	f->stream.need_bytes = PAGE_SIZE;
	mutex_lock(&f->stream.lock);
	io_opaque_budget_wait(&f->collector, -ENOBUFS);
	mutex_unlock(&f->stream.lock);
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 128);
	pressure_start(test, op);
	KUNIT_ASSERT_EQ(test, kernel_sock_shutdown(f->tx, SHUT_WR), 0);
	KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_COMPLETE);
	KUNIT_EXPECT_TRUE(test, f->stream.eof);
	KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, op->result, -ENODATA);
	KUNIT_EXPECT_EQ(test, op->data->length, 0ULL);
	KUNIT_EXPECT_EQ(test, f->stream.undecided, 96ULL);
}

static void pressure_stream_close(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;

	pressure_fill(test, false);
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 128);
	pressure_start(test, op);
	io_opaque_stream_close(&f->stream);
	KUNIT_EXPECT_EQ(test, pressure_recv(f), -ECANCELED);
	KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, op->result, -ECANCELED);
	KUNIT_EXPECT_TRUE(test, f->stream.closed);
	KUNIT_EXPECT_EQ(test, f->stream.undecided, 0ULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&f->stream.extents));
	KUNIT_EXPECT_TRUE(test, list_empty(&f->collector.wait));
}

static void pressure_impossible_prefix(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;

	pressure_fill(test, false);
	/* Length fits the store, but this sparse prefix plus dense suffix cannot. */
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 2 * PAGE_SIZE);
	pressure_start(test, op);
	KUNIT_EXPECT_EQ(test, op->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, op->result, -ENOBUFS);
	KUNIT_EXPECT_EQ(test, op->progress, 0ULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&op->data->extents));
	KUNIT_EXPECT_EQ(test, f->stream.undecided, 96ULL);
	KUNIT_EXPECT_EQ(test, f->wakes, 0U);
}

static void pressure_release_before_enrollment(struct kunit *test)
{
	struct pressure_fixture *f = test->priv;
	struct io_opaque_req *op;

	pressure_fill(test, false);
	op = pressure_range(test, IORING_OPAQUE_RECV_OBJECT, 0, 128);
	pressure_start(test, op);
	/* Deterministically order release between failed capture and enrollment. */
	mutex_lock(&f->stream.lock);
	spin_lock(&f->store.wait_lock);
	list_del_init(&f->collector.wait);
	spin_unlock(&f->store.wait_lock);
	io_opaque_extents_free(&f->store, &f->held);
	KUNIT_EXPECT_EQ(test, f->wakes, 0U);
	io_opaque_budget_wait(&f->collector, -ENOBUFS);
	mutex_unlock(&f->stream.lock);
	KUNIT_EXPECT_EQ(test, f->wakes, 1U);
	KUNIT_EXPECT_TRUE(test, list_empty(&f->collector.wait));
	KUNIT_EXPECT_EQ(test, pressure_recv(f), IOU_RETRY);
	pressure_expect_object(test, op);
}

static struct kunit_case pressure_cases[] = {
	KUNIT_CASE(pressure_retry_bytes),
	KUNIT_CASE(pressure_retry_extents),
	KUNIT_CASE(pressure_release_bytes),
	KUNIT_CASE(pressure_release_extents),
	KUNIT_CASE(pressure_buffered_take),
	KUNIT_CASE(pressure_buffered_inspect),
	KUNIT_CASE(pressure_discard_cursor),
	KUNIT_CASE(pressure_discard_gap),
	KUNIT_CASE(pressure_cancel_and_discard),
	KUNIT_CASE(pressure_consumed_gap),
	KUNIT_CASE(pressure_lost_wake_capacity),
	KUNIT_CASE(pressure_allocation_backoff),
	KUNIT_CASE(pressure_socket_error),
	KUNIT_CASE(pressure_urgent_data),
	KUNIT_CASE(pressure_fin_buffered),
	KUNIT_CASE(pressure_eof_empty),
	KUNIT_CASE(pressure_stream_close),
	KUNIT_CASE(pressure_impossible_prefix),
	KUNIT_CASE(pressure_release_before_enrollment),
	{}
};

static struct kunit_suite pressure_suite = {
	.name = "io_uring-opaque-pressure",
	.init = pressure_init,
	.test_cases = pressure_cases,
};

kunit_test_suite(pressure_suite);
