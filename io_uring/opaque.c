// SPDX-License-Identifier: GPL-2.0
/* Kernel-resident immutable TCP payloads. See userspace-api/io_uring-opaque. */
#include <linux/bvec.h>
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/memcontrol.h>
#include <linux/overflow.h>
#include <linux/rbtree.h>
#include <linux/sched/mm.h>
#include <linux/skbuff.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/tcp.h>
#include <uapi/linux/io_uring/opaque.h>

#include "io_uring.h"
#include "cancel.h"
#include "opaque.h"
#include "rsrc.h"
#include "tw.h"
#include "zcrx.h"

#define IO_OPAQUE_READ_MAX	SZ_64K
#define IO_OPAQUE_RX_BATCH	SZ_256K
#define IO_OPAQUE_MAX_ORDER	(PAGE_SHIFT < 16 ? 16 - PAGE_SHIFT : 0)
#define IO_OPAQUE_ALLOC_RETRY_MS	50
#define IO_OPAQUE_WAKE_BATCH	32

struct io_opaque_backing {
	refcount_t refs;
	struct page *page;
	u64 charge;
	u32 filled;
	bool copied;
	bool accounted;
};

struct io_opaque_extent {
	struct list_head list;
	struct io_opaque_backing *backing;
	u64 start;
	u32 offset;
	u32 length;
	bool embedded;
};

/* One allocation for the common capture; splits allocate only an extent. */
struct io_opaque_chunk {
	struct io_opaque_backing backing;
	struct io_opaque_extent extent;
};

struct io_opaque_data {
	refcount_t refs;
	struct list_head extents;
	struct bio_vec *bvec;
	u64 length;
	u64 charge;
	u32 nr;
	bool dense;
};

struct io_opaque_slot {
	struct io_opaque_data *data;
	u32 generation;
	bool reserved;
	bool compacting;
	struct list_head candidate;
	unsigned long born;
	unsigned long retry;
};

struct io_opaque_stream_slot {
	struct io_opaque_stream *stream;
	u32 generation;
};

struct io_opaque_tx {
	struct rb_node node;
	struct sock *sk;
	struct list_head requests;
	bool failed;
};

enum io_opaque_phase {
	IO_OPAQUE_IDLE,
	IO_OPAQUE_RANGE_WAIT,
	IO_OPAQUE_BUDGET_POLL,
	IO_OPAQUE_SEND_WAIT,
	IO_OPAQUE_COPY_WORK,
	IO_OPAQUE_QUEUED,
};

struct io_opaque_req {
	struct io_kiocb *req;
	struct io_opaque_store *store;
	struct io_opaque_stream *stream;
	struct io_opaque_data *data;
	struct io_opaque_data *replacement;
	struct io_opaque_tx *tx;
	struct list_head wait;
	struct list_head read;
	struct list_head send;
	struct list_head cancel;
	struct rb_node decision;
	struct iov_iter iter;
	u64 handle;
	u64 offset;
	u64 progress;
	u64 reserved;
	u64 need_bytes;
	unsigned long retry_at;
	void __user *addr;
	u32 length;
	u32 slot;
	u32 msg_flags;
	u16 op;
	u16 send_flags;
	int result;
	enum io_opaque_phase phase;
	bool canceled;
	bool counted;
	bool slot_reserved;
	bool consumed;
	bool resolved;
};

struct io_opaque_stream {
	refcount_t refs;
	/* Extents, range decisions and collector state. */
	struct mutex lock;
	struct io_opaque_store *store;
	struct file *file;
	struct sock_rx_owner owner;
	struct list_head extents;
	struct list_head reads;
	struct rb_root_cached decisions;
	struct io_opaque_req *collector;
	u64 token;
	u64 next;
	u64 undecided;
	u64 need_bytes;
	int error;
	bool eof;
	bool closed;
};

struct io_opaque_store {
	struct io_uring_opaque_config config;
	/* Handles, candidates and TX queues; never held during copies or I/O. */
	struct mutex tables;
	struct io_opaque_slot *objects;
	struct io_opaque_stream_slot *streams;
	struct rb_root txs;
	struct user_struct *user;
	struct mm_struct *mm;
	struct mem_cgroup *memcg;
	struct obj_cgroup *objcg;
	atomic64_t bytes;
	atomic64_t copied;
	atomic64_t compacted;
	atomic64_t compactions;
	atomic_t extents;
	atomic_t requests;
	/* Budget waiters and remote wakeup/cancellation arbitration. */
	spinlock_t wait_lock;
	struct list_head budget_waits;
	struct list_head copies;
	/* Serialize manual and automatic copies outside the submission lock. */
	struct mutex copy_lock;
	struct work_struct copy_work;
	struct delayed_work retry_work;
	struct delayed_work auto_work;
	struct list_head candidates;
	struct io_uring_opaque_policy policy;
	u64 tokens;
	unsigned long token_time;
	int dead;
};

static void io_opaque_resume(struct io_tw_req tw_req, io_tw_token_t tw);
static void io_opaque_ready(struct io_tw_req tw_req, io_tw_token_t tw);
static void io_opaque_auto_work(struct work_struct *work);
static void io_opaque_compact_work(struct work_struct *work);
static void io_opaque_stream_close(struct io_opaque_stream *stream);
static int io_opaque_compact_start(struct io_opaque_req *op);
static int io_opaque_set_policy(struct io_opaque_req *op);
static int io_opaque_compact_publish(struct io_opaque_req *op);
static void io_opaque_tx_put(struct io_opaque_req *op);

static void io_opaque_wake_budget(struct io_opaque_store *store)
{
	struct io_opaque_req *op, *next;
	unsigned long delay = MAX_JIFFY_OFFSET;
	unsigned long now = jiffies;
	u64 limit = store->config.hard_limit - store->config.compact_headroom;
	u64 used = atomic64_read(&store->bytes);
	unsigned int woken = 0;
	bool stopped;

	guard(spinlock)(&store->wait_lock);
	stopped = READ_ONCE(store->dead);
	list_for_each_entry_safe(op, next, &store->budget_waits, wait) {
		struct socket *sock = sock_from_file(op->req->file);

		if (!stopped && !READ_ONCE(op->stream->closed)) {
			if (time_before(now, op->retry_at)) {
				delay = min(delay, op->retry_at - now);
				continue;
			}
			if (used > limit || op->need_bytes > limit - used ||
			    atomic_read(&store->extents) >= store->config.max_extents - 2)
				continue;
			if (woken == IO_OPAQUE_WAKE_BATCH) {
				delay = 1;
				continue;
			}
		}
		list_del_init(&op->wait);
		op->phase = IO_OPAQUE_IDLE;
		woken++;
		/* Native multishot polling also observes FIN, RST and urgent data. */
		wake_up_interruptible_poll(sk_sleep(sock->sk), EPOLLIN);
	}
	if (!stopped && delay != MAX_JIFFY_OFFSET)
		queue_delayed_work(system_unbound_wq, &store->retry_work, delay);
}

static void io_opaque_retry_work(struct work_struct *work)
{
	struct io_opaque_store *store = container_of(to_delayed_work(work),
						   struct io_opaque_store, retry_work);

	io_opaque_wake_budget(store);
}

static void io_opaque_wake_stream(struct io_opaque_stream *stream)
{
	struct io_opaque_req *op = stream->collector;
	struct socket *sock = sock_from_file(stream->file);

	lockdep_assert_held(&stream->lock);
	if (!op)
		return;
	scoped_guard(spinlock, &stream->store->wait_lock) {
		list_del_init(&op->wait);
		op->phase = IO_OPAQUE_IDLE;
	}
	wake_up_interruptible_poll(sk_sleep(sock->sk), EPOLLIN);
}

static bool io_opaque_charge(struct io_opaque_store *store, u64 bytes, bool rx)
{
	u64 limit = store->config.hard_limit;
	s64 used = atomic64_read(&store->bytes);

	if (rx)
		limit -= store->config.compact_headroom;
	for (;;) {
		if (bytes > limit || used > limit - bytes)
			return false;
		if (atomic64_try_cmpxchg(&store->bytes, &used, used + bytes))
			return true;
	}
}

static void io_opaque_uncharge(struct io_opaque_store *store, u64 bytes)
{
	atomic64_sub(bytes, &store->bytes);
	io_opaque_wake_budget(store);
}

static bool io_opaque_extent_reserve(struct io_opaque_store *store)
{
	int nr = atomic_read(&store->extents);

	while (nr < store->config.max_extents) {
		if (atomic_try_cmpxchg(&store->extents, &nr, nr + 1))
			return true;
	}
	return false;
}

static int io_opaque_memcg_charge(struct io_opaque_store *store, u64 bytes)
{
#ifdef CONFIG_MEMCG
	if (store->objcg)
		return obj_cgroup_charge(store->objcg,
					GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN, bytes);
#endif
	return 0;
}

static void io_opaque_memcg_uncharge(struct io_opaque_store *store, u64 bytes)
{
#ifdef CONFIG_MEMCG
	if (store->objcg)
		obj_cgroup_uncharge(store->objcg, bytes);
#endif
}

static struct io_opaque_extent *io_opaque_extent_new(struct io_opaque_store *store,
						     struct page *page, u32 offset,
						  u32 length, u64 start,
						  bool reserved)
{
	struct io_opaque_chunk *chunk;
	u64 charge = page_size(compound_head(page));

	if (!io_opaque_extent_reserve(store))
		return ERR_PTR(-ENOBUFS);
	if (!reserved && !io_opaque_charge(store, charge, true))
		goto no_space;
	if (!reserved && io_opaque_memcg_charge(store, charge))
		goto no_memory;
	chunk = kzalloc_obj(*chunk, GFP_KERNEL_ACCOUNT);
	if (!chunk) {
		if (!reserved)
			io_opaque_memcg_uncharge(store, charge);
		goto no_memory;
	}
	refcount_set(&chunk->backing.refs, 1);
	chunk->backing.page = compound_head(page);
	chunk->backing.charge = charge;
	chunk->backing.accounted = !reserved && store->objcg;
	chunk->extent.backing = &chunk->backing;
	chunk->extent.start = start;
	chunk->extent.offset = offset + (page - compound_head(page)) * PAGE_SIZE;
	chunk->extent.length = length;
	chunk->extent.embedded = true;
	INIT_LIST_HEAD(&chunk->extent.list);
	return &chunk->extent;
no_memory:
	if (!reserved)
		io_opaque_uncharge(store, charge);
	atomic_dec(&store->extents);
	return ERR_PTR(-ENOMEM);
no_space:
	atomic_dec(&store->extents);
	return ERR_PTR(-ENOBUFS);
}

static void io_opaque_extent_free(struct io_opaque_store *store,
				  struct io_opaque_extent *extent)
{
	struct io_opaque_backing *backing = extent->backing;

	list_del(&extent->list);
	if (!extent->embedded)
		kfree(extent);
	atomic_dec(&store->extents);
	if (refcount_dec_and_test(&backing->refs)) {
		u64 charge = backing->charge;

		if (backing->accounted)
			io_opaque_memcg_uncharge(store, charge);
		put_page(backing->page);
		kfree(container_of(backing, struct io_opaque_chunk, backing));
		io_opaque_uncharge(store, charge);
	} else {
		io_opaque_wake_budget(store);
	}
}

static void io_opaque_extents_free(struct io_opaque_store *store,
				   struct list_head *list)
{
	struct io_opaque_extent *extent, *next;

	list_for_each_entry_safe(extent, next, list, list)
		io_opaque_extent_free(store, extent);
}

static struct io_opaque_data *io_opaque_data_alloc(void)
{
	struct io_opaque_data *data = kzalloc_obj(*data, GFP_KERNEL_ACCOUNT);

	if (data) {
		refcount_set(&data->refs, 1);
		INIT_LIST_HEAD(&data->extents);
	}
	return data;
}

static void io_opaque_data_put(struct io_opaque_store *store,
			       struct io_opaque_data *data)
{
	if (data && refcount_dec_and_test(&data->refs)) {
		io_opaque_extents_free(store, &data->extents);
		kvfree(data->bvec);
		kfree(data);
	}
}

static int io_opaque_vector(struct io_opaque_data *data)
{
	struct io_opaque_extent *extent;
	struct io_opaque_backing *previous = NULL;
	u32 nr = 0;

	data->charge = 0;
	list_for_each_entry(extent, &data->extents, list) {
		nr++;
		if (extent->backing != previous)
			data->charge += extent->backing->charge;
		previous = extent->backing;
	}
	data->bvec = kvmalloc_objs(*data->bvec, nr, GFP_KERNEL_ACCOUNT);
	if (!data->bvec)
		return -ENOMEM;
	data->nr = nr;
	nr = 0;
	list_for_each_entry(extent, &data->extents, list)
		bvec_set_page(&data->bvec[nr++], extent->backing->page,
			      extent->length, extent->offset);
	return 0;
}

static struct io_opaque_slot *io_opaque_lookup(struct io_opaque_store *store,
					       u64 handle)
{
	u32 slot = (u32)handle;

	lockdep_assert_held(&store->tables);
	if (!slot || slot > store->config.max_objects)
		return NULL;
	if (store->objects[slot - 1].generation != handle >> 32)
		return NULL;
	return &store->objects[slot - 1];
}

/* Drop handle visibility, transferring the table reference to the caller. */
static struct io_opaque_data *io_opaque_detach(struct io_opaque_store *store,
					       struct io_opaque_slot *slot)
{
	struct io_opaque_data *data;

	lockdep_assert_held(&store->tables);
	if (!slot || !slot->data)
		return NULL;
	data = slot->data;
	slot->data = NULL;
	slot->compacting = false;
	list_del_init(&slot->candidate);
	return data;
}

static int io_opaque_slot_reserve(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	u32 i;

	guard(mutex)(&store->tables);
	for (i = 0; i < store->config.max_objects; i++) {
		struct io_opaque_slot *slot = &store->objects[i];

		if (slot->data || slot->reserved || slot->generation == U32_MAX)
			continue;
		slot->reserved = true;
		slot->generation++;
		op->slot = i;
		op->handle = (u64)slot->generation << 32 | (i + 1);
		op->slot_reserved = true;
		return 0;
	}
	return -ENOSPC;
}

static void io_opaque_stream_put(struct io_opaque_stream *stream)
{
	if (stream && refcount_dec_and_test(&stream->refs)) {
		io_opaque_extents_free(stream->store, &stream->extents);
		fput(stream->file);
		mutex_destroy(&stream->lock);
		kfree(stream);
	}
}

struct io_opaque_store *io_opaque_alloc(struct io_ring_ctx *ctx, u64 config)
{
	struct io_uring_opaque_config cfg;
	struct io_opaque_store *store;
	int ret;
	u32 i;

	if (copy_from_user(&cfg, u64_to_user_ptr(config), sizeof(cfg)))
		return ERR_PTR(-EFAULT);
	if (cfg.flags || !mem_is_zero(cfg.__resv, sizeof(cfg.__resv)))
		return ERR_PTR(-EINVAL);
	if (!cfg.hard_limit || cfg.hard_limit > SZ_1T ||
	    !IS_ALIGNED(cfg.hard_limit, PAGE_SIZE) ||
	    !IS_ALIGNED(cfg.compact_headroom, PAGE_SIZE) ||
	    cfg.compact_headroom >= cfg.hard_limit ||
	    !cfg.max_object_size || cfg.max_object_size > INT_MAX ||
	    !cfg.max_objects || cfg.max_objects > 16384 ||
	    !cfg.max_streams || cfg.max_streams > 1024 ||
	    cfg.max_extents < 4 || cfg.max_extents > 65536 ||
	    !cfg.max_requests || cfg.max_requests > 4096)
		return ERR_PTR(-EINVAL);
	store = kzalloc_obj(*store, GFP_KERNEL_ACCOUNT);
	if (!store)
		return ERR_PTR(-ENOMEM);
	store->config = cfg;
	store->objects = kvcalloc(cfg.max_objects, sizeof(*store->objects),
				  GFP_KERNEL_ACCOUNT);
	store->streams = kvcalloc(cfg.max_streams, sizeof(*store->streams),
				  GFP_KERNEL_ACCOUNT);
	if (!store->objects || !store->streams) {
		ret = -ENOMEM;
		goto free_store;
	}
	ret = io_account_mem(ctx->user, ctx->mm_account, cfg.hard_limit >> PAGE_SHIFT);
	if (ret)
		goto free_store;
	store->user = ctx->user ? get_uid(ctx->user) : NULL;
	store->mm = ctx->mm_account;
	if (store->mm)
		mmgrab(store->mm);
	store->memcg = get_mem_cgroup_from_mm(store->mm);
#ifdef CONFIG_MEMCG
	if (store->memcg && !mem_cgroup_is_root(store->memcg)) {
		struct mem_cgroup *old = set_active_memcg(store->memcg);

		store->objcg = get_obj_cgroup_from_current();
		set_active_memcg(old);
	}
#endif
	mutex_init(&store->tables);
	mutex_init(&store->copy_lock);
	spin_lock_init(&store->wait_lock);
	INIT_LIST_HEAD(&store->budget_waits);
	INIT_LIST_HEAD(&store->copies);
	INIT_LIST_HEAD(&store->candidates);
	store->txs = RB_ROOT;
	for (i = 0; i < cfg.max_objects; i++)
		INIT_LIST_HEAD(&store->objects[i].candidate);
	INIT_DELAYED_WORK(&store->auto_work, io_opaque_auto_work);
	INIT_DELAYED_WORK(&store->retry_work, io_opaque_retry_work);
	INIT_WORK(&store->copy_work, io_opaque_compact_work);
	return store;
free_store:
	kvfree(store->streams);
	kvfree(store->objects);
	kfree(store);
	return ERR_PTR(ret);
}

void io_opaque_stop(struct io_opaque_store *store)
{
	u32 i;

	if (!store)
		return;
	scoped_guard(spinlock, &store->wait_lock) {
		if (store->dead)
			return;
		WRITE_ONCE(store->dead, 1);
	}
	cancel_delayed_work_sync(&store->auto_work);
	cancel_delayed_work_sync(&store->retry_work);
	for (i = 0; i < store->config.max_streams; i++) {
		struct io_opaque_stream *stream;

		mutex_lock(&store->tables);
		stream = store->streams[i].stream;
		store->streams[i].stream = NULL;
		mutex_unlock(&store->tables);
		if (stream) {
			io_opaque_stream_close(stream);
			io_opaque_stream_put(stream);
		}
	}
	for (i = 0; i < store->config.max_objects; i++) {
		struct io_opaque_data *data;

		mutex_lock(&store->tables);
		data = store->objects[i].data;
		store->objects[i].data = NULL;
		list_del_init(&store->objects[i].candidate);
		mutex_unlock(&store->tables);
		io_opaque_data_put(store, data);
	}
	io_opaque_wake_budget(store);
}

void io_opaque_free(struct io_opaque_store *store)
{
	if (!store)
		return;
	io_opaque_stop(store);
	flush_work(&store->copy_work);
	WARN_ON_ONCE(atomic64_read(&store->bytes));
	WARN_ON_ONCE(atomic_read(&store->extents));
	WARN_ON_ONCE(atomic_read(&store->requests));
	io_unaccount_mem(store->user, store->mm,
			 store->config.hard_limit >> PAGE_SHIFT);
	obj_cgroup_put(store->objcg);
	mem_cgroup_put(store->memcg);
	if (store->mm)
		mmdrop(store->mm);
	free_uid(store->user);
	kvfree(store->objects);
	kvfree(store->streams);
	mutex_destroy(&store->tables);
	kfree(store);
}

static struct io_opaque_req *io_opaque_req_alloc(struct io_kiocb *req,
						 struct io_opaque_store *store,
					       bool control)
{
	struct io_opaque_req *op;

	if (READ_ONCE(store->dead))
		return ERR_PTR(-ESHUTDOWN);
	if (!control && atomic_inc_return(&store->requests) > store->config.max_requests) {
		atomic_dec(&store->requests);
		return ERR_PTR(-EAGAIN);
	}
	op = kzalloc_obj(*op, GFP_KERNEL_ACCOUNT);
	if (!op) {
		if (!control)
			atomic_dec(&store->requests);
		return ERR_PTR(-ENOMEM);
	}
	op->req = req;
	op->store = store;
	op->counted = !control;
	op->retry_at = jiffies;
	INIT_LIST_HEAD(&op->wait);
	INIT_LIST_HEAD(&op->read);
	INIT_LIST_HEAD(&op->send);
	INIT_LIST_HEAD(&op->cancel);
	RB_CLEAR_NODE(&op->decision);
	req->async_data = op;
	req->flags |= REQ_F_ASYNC_DATA | REQ_F_NEED_CLEANUP;
	return op;
}

/* The ring lock protects enrollment; wait_lock arbitrates remote wakeups. */
static void io_opaque_wait(struct io_opaque_req *op, enum io_opaque_phase phase)
{
	lockdep_assert_held(&op->req->ctx->uring_lock);
	if (list_empty(&op->cancel))
		list_add_tail(&op->cancel, &op->req->ctx->opaque_waits);
	guard(spinlock)(&op->store->wait_lock);
	op->phase = phase;
}

static void io_opaque_queue_ready(struct io_opaque_req *op, int result)
{
	guard(spinlock)(&op->store->wait_lock);
	op->result = result;
	op->phase = IO_OPAQUE_QUEUED;
	op->req->io_task_work.func = io_opaque_ready;
	io_req_task_work_add(op->req);
}

static void io_opaque_resume(struct io_tw_req tw_req, io_tw_token_t tw)
{
	struct io_kiocb *req = tw_req.req;
	struct io_opaque_req *op = req->async_data;

	io_tw_lock(req->ctx, tw);
	list_del_init(&op->cancel);
	op->phase = IO_OPAQUE_IDLE;
	if (tw.cancel || READ_ONCE(op->canceled) || READ_ONCE(op->store->dead))
		io_req_defer_failed(req, -ECANCELED);
	else
		io_req_task_submit(tw_req, tw);
}

static struct io_opaque_extent *io_opaque_extent_split(struct io_opaque_store *store,
						       struct io_opaque_extent *extent,
						    u64 at)
{
	struct io_opaque_extent *right;
	u32 left = at - extent->start;

	if (!io_opaque_extent_reserve(store))
		return ERR_PTR(-ENOBUFS);
	right = kmalloc_obj(*right, GFP_KERNEL_ACCOUNT);
	if (!right) {
		atomic_dec(&store->extents);
		return ERR_PTR(-ENOMEM);
	}
	*right = *extent;
	right->embedded = false;
	refcount_inc(&right->backing->refs);
	right->start = at;
	right->offset += left;
	right->length -= left;
	extent->length = left;
	list_add(&right->list, &extent->list);
	return right;
}

static int io_opaque_available(struct io_opaque_stream *stream, u64 off, u64 len)
{
	struct io_opaque_extent *extent;
	u64 end = off + len;

	list_for_each_entry(extent, &stream->extents, list) {
		if (extent->start + extent->length <= off)
			continue;
		if (extent->start > off)
			return -ENODATA;
		off = min(end, extent->start + extent->length);
		if (off == end)
			return 1;
	}
	return off < stream->next ? -ENODATA : 0;
}

static int io_opaque_snapshot(struct io_opaque_req *op)
{
	struct io_opaque_extent *extent;
	u64 end = op->offset + op->length;
	int ret = io_opaque_available(op->stream, op->offset, op->length);

	if (ret != 1)
		return ret;
	list_for_each_entry(extent, &op->stream->extents, list) {
		u64 first = max(op->offset, extent->start);
		u64 last = min(end, extent->start + extent->length);
		struct io_opaque_extent *copy;

		if (first >= last)
			continue;
		if (!io_opaque_extent_reserve(op->store))
			return -ENOBUFS;
		copy = kmalloc_obj(*copy, GFP_KERNEL_ACCOUNT);
		if (!copy) {
			atomic_dec(&op->store->extents);
			return -ENOMEM;
		}
		*copy = *extent;
		copy->embedded = false;
		refcount_inc(&copy->backing->refs);
		copy->start = first - op->offset;
		copy->offset += first - extent->start;
		copy->length = last - first;
		list_add_tail(&copy->list, &op->data->extents);
	}
	op->data->length = op->length;
	return 1;
}

static int io_opaque_take(struct io_opaque_req *op)
{
	struct io_opaque_stream *stream = op->stream;
	u64 off = op->offset + op->progress;
	u64 end = op->offset + op->length;

	while (off < end && off < stream->next) {
		struct io_opaque_extent *extent, *found = NULL;
		u64 last;

		list_for_each_entry(extent, &stream->extents, list) {
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
			extent = io_opaque_extent_split(op->store, extent, off);
			if (IS_ERR(extent))
				return PTR_ERR(extent);
		}
		last = min(end, extent->start + extent->length);
		if (last < extent->start + extent->length) {
			struct io_opaque_extent *right;

			right = io_opaque_extent_split(op->store, extent, last);
			if (IS_ERR(right))
				return PTR_ERR(right);
		}
		stream->undecided -= last - off;
		if (op->op == IORING_OPAQUE_KEEP) {
			struct io_opaque_extent *tail;

			tail = list_last_entry_or_null(&op->data->extents,
						       struct io_opaque_extent, list);
			if (!tail || tail->backing != extent->backing)
				op->data->charge += extent->backing->charge;
			extent->start -= op->offset;
			list_move_tail(&extent->list, &op->data->extents);
			op->data->length += last - off;
		} else {
			io_opaque_extent_free(op->store, extent);
		}
		op->progress += last - off;
		off = last;
	}
	return off == end;
}

/* An unsuccessful KEEP returns its private prefix to the undecided stream. */
static void io_opaque_restore(struct io_opaque_req *op)
{
	struct io_opaque_stream *stream = op->stream;
	struct io_opaque_extent *extent, *next;
	struct list_head *before = &stream->extents;

	lockdep_assert_held(&stream->lock);
	if (!op->data || !op->data->length || stream->closed)
		return;
	list_for_each_entry(extent, &stream->extents, list) {
		if (extent->start > op->offset) {
			before = &extent->list;
			break;
		}
	}
	list_for_each_entry_safe(extent, next, &op->data->extents, list) {
		extent->start += op->offset;
		list_move_tail(&extent->list, before);
	}
	stream->undecided += op->data->length;
	op->data->length = 0;
	op->data->charge = 0;
	op->progress = 0;
}

static u64 io_opaque_dense_left(struct io_opaque_req *op)
{
	struct io_opaque_extent *tail;
	u64 left = op->length - op->progress;

	tail = list_last_entry_or_null(&op->data->extents, struct io_opaque_extent, list);
	if (tail && tail->backing->copied &&
	    tail->offset + tail->length == tail->backing->filled) {
		u32 room = PAGE_SIZE - tail->backing->filled;

		left -= min_t(u64, left, room);
	}
	return ALIGN(left, PAGE_SIZE);
}

static bool io_opaque_overlap(u64 a, u64 alen, u64 b, u64 blen)
{
	return a < b + blen && b < a + alen;
}

static int io_opaque_range_insert(struct io_opaque_req *op)
{
	struct io_opaque_stream *stream = op->stream;
	struct rb_node **link = &stream->decisions.rb_root.rb_node, *parent = NULL;
	struct io_opaque_req *other;
	bool leftmost = true;

	while (*link) {
		parent = *link;
		other = rb_entry(parent, struct io_opaque_req, decision);
		if (io_opaque_overlap(op->offset, op->length, other->offset, other->length))
			return -EBUSY;
		if (op->offset < other->offset) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}
	list_for_each_entry(other, &stream->reads, read)
		if (io_opaque_overlap(op->offset, op->length, other->offset, other->length))
			return -EBUSY;
	rb_link_node(&op->decision, parent, link);
	rb_insert_color_cached(&op->decision, &stream->decisions, leftmost);
	return 0;
}

static void io_opaque_stream_process(struct io_opaque_stream *stream)
{
	struct io_opaque_req *op, *next;
	struct rb_node *node, *after;
	int ret;

	list_for_each_entry_safe(op, next, &stream->reads, read) {
		ret = stream->closed ? -ECANCELED : io_opaque_snapshot(op);
		if (!ret && !stream->eof && !stream->closed && !stream->error)
			continue;
		list_del_init(&op->read);
		if (!ret)
			ret = stream->error ?: -ENODATA;
		io_opaque_queue_ready(op, ret == 1 ? op->length : ret);
	}
	for (node = rb_first_cached(&stream->decisions); node; node = after) {
		op = rb_entry(node, struct io_opaque_req, decision);
		after = rb_next(node);
		ret = stream->closed ? -ECANCELED : io_opaque_take(op);
		if (!ret && op->op == IORING_OPAQUE_KEEP &&
		    op->data->charge + io_opaque_dense_left(op) >
		    op->store->config.hard_limit - op->store->config.compact_headroom)
			ret = -ENOBUFS;
		if (!ret && !stream->eof && !stream->closed && !stream->error)
			continue;
		rb_erase_cached(node, &stream->decisions);
		RB_CLEAR_NODE(node);
		if (!ret)
			ret = stream->error ?: -ENODATA;
		io_opaque_queue_ready(op, ret == 1 ? op->length : ret);
	}
}

/* Only the earliest undecided range is consulted on the capture hot path. */
static struct io_opaque_req *io_opaque_next_decision(struct io_opaque_stream *stream)
{
	struct rb_node *node = rb_first_cached(&stream->decisions);

	return node ? rb_entry(node, struct io_opaque_req, decision) : NULL;
}

static struct io_opaque_req *io_opaque_capture_keep(struct io_opaque_stream *stream)
{
	struct io_opaque_req *op = io_opaque_next_decision(stream);

	if (op && op->op == IORING_OPAQUE_KEEP && stream->next >= op->offset &&
	    stream->next < op->offset + op->length)
		return op;
	return NULL;
}

static struct io_opaque_extent *io_opaque_capture_tail(struct io_opaque_stream *stream)
{
	struct io_opaque_req *op = io_opaque_capture_keep(stream);
	struct list_head *list = op ? &op->data->extents : &stream->extents;

	return list_last_entry_or_null(list, struct io_opaque_extent, list);
}

static int io_opaque_append(struct io_opaque_stream *stream, struct page *page,
			    u32 offset, u32 length, bool take_ref, bool copied)
{
	struct io_opaque_req *op = io_opaque_capture_keep(stream);
	struct list_head *list = &stream->extents;
	struct io_opaque_extent *extent, *tail;
	u64 start = stream->next;
	bool new_backing = false;

	if (op) {
		list = &op->data->extents;
		start -= op->offset;
	}
	tail = list_last_entry_or_null(list, struct io_opaque_extent, list);
	offset += (page - compound_head(page)) * PAGE_SIZE;
	page = compound_head(page);
	if (tail && tail->backing->page == page) {
		if (tail->start + tail->length == start &&
		    tail->offset + tail->length == offset) {
			tail->length += length;
			extent = tail;
			goto accounted;
		}
	}
	stream->need_bytes = page_size(page);
	if (atomic_read(&stream->store->extents) >= stream->store->config.max_extents - 2)
		return -ENOBUFS;
	if (tail && tail->backing->page == page) {
		if (!io_opaque_extent_reserve(stream->store))
			return -ENOBUFS;
		extent = kmalloc_obj(*extent, GFP_KERNEL_ACCOUNT);
		if (!extent) {
			atomic_dec(&stream->store->extents);
			return -ENOMEM;
		}
		*extent = (struct io_opaque_extent) {
			.backing = tail->backing, .start = start,
			.offset = offset, .length = length,
		};
		refcount_inc(&extent->backing->refs);
	} else {
		u64 left = op ? ALIGN(op->length - op->progress - length, PAGE_SIZE) : 0;

		/* Leave room for dense copies of a private KEEP's remaining bytes. */
		if (!copied && op && op->data->charge + stream->need_bytes + left >
		    stream->store->config.hard_limit - stream->store->config.compact_headroom)
			return -ENOBUFS;
		extent = io_opaque_extent_new(stream->store, page, offset, length, start, copied);
		if (IS_ERR(extent))
			return PTR_ERR(extent);
		if (take_ref)
			get_page(page);
		extent->backing->copied = copied;
		new_backing = true;
	}
	list_add_tail(&extent->list, list);
accounted:
	if (op) {
		op->progress += length;
		op->data->length += length;
		if (new_backing)
			op->data->charge += extent->backing->charge;
	} else {
		stream->undecided += length;
	}
	if (copied) {
		extent->backing->filled = offset + length;
		atomic64_add(length, &stream->store->copied);
	}
	return length;
}

static int io_opaque_capture(struct io_opaque_stream *stream,
			     const struct sk_buff *skb, u32 offset, u32 len,
			     bool shared)
{
	u32 consumed = 0, head = skb_headlen(skb), start = head;
	bool copy_frags;
	struct sk_buff *child;
	u64 retain_limit = stream->store->config.hard_limit -
			   stream->store->config.compact_headroom;
	u32 chunk;
	int i, ret = -EFAULT;

	shared |= skb_shared(skb);
	copy_frags = shared || skb_cloned(skb) || skb_has_shared_frag(skb);
	if (!skb_frags_readable(skb))
		return -EIO;
	if (offset < head) {
		chunk = min(head - offset, len);
		if (!shared && !skb_head_is_locked(skb)) {
			void *data = skb->data + offset;
			struct page *page = virt_to_page(data);

			/* A compound allocation larger than the limit can never fit. */
			if (page_size(compound_head(page)) > retain_limit)
				goto copy;
			ret = io_opaque_append(stream, page, offset_in_page(data),
					       chunk, true, false);
		} else {
			goto copy;
		}
		if (ret == -ENOBUFS)
			goto copy;
		goto head_done;
	}
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		u32 size = skb_frag_size(frag);

		if (offset >= start + size) {
			start += size;
			continue;
		}
		chunk = min(start + size - offset, len);
		if (copy_frags || page_size(compound_head(skb_frag_page(frag))) > retain_limit)
			goto copy;
		ret = io_opaque_append(stream, skb_frag_page(frag),
				       skb_frag_off(frag) + offset - start, chunk, true, false);
		if (ret == -ENOBUFS)
			goto copy;
		goto head_done;
	}
	skb_walk_frags(skb, child) {
		if (offset >= start + child->len) {
			start += child->len;
			continue;
		}
		return io_opaque_capture(stream, child, offset - start,
					 min(start + child->len - offset, len), copy_frags);
	}
	return -EFAULT;
copy:
	{
		struct io_opaque_extent *tail = io_opaque_capture_tail(stream);
		struct page *page;
		u32 pos = 0;
		bool allocated = true;
		void *mapped;

		if (tail && tail->backing->copied && tail->backing->filled < PAGE_SIZE &&
		    tail->offset + tail->length == tail->backing->filled) {
			page = tail->backing->page;
			pos = tail->backing->filled;
			allocated = false;
		} else {
			stream->need_bytes = PAGE_SIZE;
			if (!io_opaque_charge(stream->store, PAGE_SIZE, true))
				return -ENOBUFS;
			page = alloc_page(GFP_KERNEL_ACCOUNT);
			if (!page) {
				io_opaque_uncharge(stream->store, PAGE_SIZE);
				return -ENOMEM;
			}
		}
		chunk = min_t(u32, chunk, PAGE_SIZE - pos);
		mapped = kmap_local_page(page);
		ret = skb_copy_bits(skb, offset, mapped + pos, chunk);
		kunmap_local(mapped);
		if (!ret)
			ret = io_opaque_append(stream, page, pos, chunk, false, true);
		if (ret < 0 && allocated) {
			put_page(page);
			io_opaque_uncharge(stream->store, PAGE_SIZE);
		}
	}
head_done:
	if (ret > 0)
		consumed += ret;
	return consumed ?: ret;
}

static int io_opaque_actor(read_descriptor_t *desc, struct sk_buff *skb,
			   unsigned int offset, size_t length)
{
	struct io_opaque_stream *stream = desc->arg.data;
	u32 consumed = 0;
	int ret = 0;

	length = min(length, desc->count);
	length = min_t(u64, length, U64_MAX - stream->next);
	while (length) {
		struct io_opaque_req *op = io_opaque_next_decision(stream);
		u32 chunk = length;

		if (op) {
			if (stream->next < op->offset) {
				chunk = min_t(u64, chunk, op->offset - stream->next);
			} else {
				chunk = min_t(u64, chunk, op->offset + op->length - stream->next);
				if (op->op == IORING_OPAQUE_DISCARD) {
					op->progress += chunk;
					ret = chunk;
					goto consumed;
				}
			}
		}
		ret = io_opaque_capture(stream, skb, offset, chunk, false);
		if (ret <= 0) {
			desc->error = ret;
			break;
		}
consumed:
		stream->next += ret;
		consumed += ret;
		offset += ret;
		length -= ret;
		desc->count -= ret;
		if (op && op->progress == op->length) {
			rb_erase_cached(&op->decision, &stream->decisions);
			RB_CLEAR_NODE(&op->decision);
			io_opaque_queue_ready(op, op->length);
		}
	}
	if (!length && stream->next == U64_MAX)
		stream->error = -EOVERFLOW;
	return consumed ?: ret;
}

static void io_opaque_stream_close(struct io_opaque_stream *stream)
{
	struct socket *sock = sock_from_file(stream->file);

	guard(mutex)(&stream->lock);
	stream->closed = true;
	stream->error = -ECANCELED;
	lock_sock(sock->sk);
	write_lock_bh(&sock->sk->sk_callback_lock);
	if (sock->sk->sk_rx_owner == &stream->owner)
		WRITE_ONCE(sock->sk->sk_rx_owner, NULL);
	write_unlock_bh(&sock->sk->sk_callback_lock);
	release_sock(sock->sk);
	io_opaque_extents_free(stream->store, &stream->extents);
	stream->undecided = 0;
	io_opaque_stream_process(stream);
	wake_up_interruptible_poll(sk_sleep(sock->sk), EPOLLIN | EPOLLHUP);
	io_opaque_wake_budget(stream->store);
}

int io_opaque_recv_prep(struct io_kiocb *req, struct io_opaque_store *store)
{
	return PTR_ERR_OR_ZERO(io_opaque_req_alloc(req, store, false));
}

static int io_opaque_attach(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_stream *stream;
	struct socket *sock = sock_from_file(op->req->file);
	struct io_uring_cqe cqe[2] = {};
	u32 i;
	int ret = -ENOSPC;

	if (!sock || sock->type != SOCK_STREAM || sock->sk->sk_protocol != IPPROTO_TCP)
		return -EOPNOTSUPP;
	stream = kzalloc_obj(*stream, GFP_KERNEL_ACCOUNT);
	if (!stream)
		return -ENOMEM;
	refcount_set(&stream->refs, 1);
	mutex_init(&stream->lock);
	INIT_LIST_HEAD(&stream->extents);
	INIT_LIST_HEAD(&stream->reads);
	stream->decisions = RB_ROOT_CACHED;
	stream->store = store;
	stream->file = get_file(op->req->file);
	stream->owner.actor = io_opaque_actor;
	stream->owner.data = stream;
	stream->collector = op;
	mutex_lock(&store->tables);
	lock_sock(sock->sk);
	if (sock->sk->sk_rx_owner || sock->sk->sk_prot != sock->sk->sk_prot_creator ||
	    tcp_sk(sock->sk)->repair || tcp_sk(sock->sk)->urg_data) {
		ret = -EBUSY;
		goto unlock;
	}
	if (sock->sk->sk_state != TCP_ESTABLISHED && sock->sk->sk_state != TCP_CLOSE_WAIT) {
		ret = -ENOTCONN;
		goto unlock;
	}
	for (i = 0; i < store->config.max_streams; i++) {
		struct io_opaque_stream_slot *slot = &store->streams[i];

		if (slot->stream || slot->generation == U32_MAX)
			continue;
		write_lock_bh(&sock->sk->sk_callback_lock);
		if (sock->sk->sk_user_data) {
			write_unlock_bh(&sock->sk->sk_callback_lock);
			ret = -EBUSY;
			break;
		}
		slot->generation++;
		stream->token = (u64)slot->generation << 32 | (i + 1);
		cqe[0].user_data = op->req->cqe.user_data;
		cqe[0].flags = IORING_CQE_F_MORE | ctx_cqe32_flags(op->req->ctx);
		memcpy(&cqe[1], &stream->token, sizeof(stream->token));
		if (!io_req_post_cqe32(op->req, cqe)) {
			write_unlock_bh(&sock->sk->sk_callback_lock);
			ret = -ENOSPC;
			break;
		}
		WRITE_ONCE(sock->sk->sk_rx_owner, &stream->owner);
		write_unlock_bh(&sock->sk->sk_callback_lock);
		slot->stream = stream;
		refcount_inc(&stream->refs);
		op->stream = stream;
		ret = 0;
		break;
	}
unlock:
	release_sock(sock->sk);
	mutex_unlock(&store->tables);
	if (!ret)
		wake_up_interruptible_poll(sk_sleep(sock->sk), EPOLLIN);
	if (ret)
		io_opaque_stream_put(stream);
	return ret;
}

int io_opaque_recv(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_opaque_req *op = req->async_data;
	struct io_opaque_stream *stream;
	struct socket *sock = sock_from_file(req->file);
	read_descriptor_t desc = { .count = IO_OPAQUE_RX_BATCH };
	struct mem_cgroup *old;
	int ret;

	io_ring_submit_lock(req->ctx, issue_flags);
	if (!op->stream && (issue_flags & IO_URING_F_UNLOCKED)) {
		io_req_task_queue(req);
		ret = IOU_ISSUE_SKIP_COMPLETE;
		goto out;
	}
	if (!op->stream) {
		ret = io_opaque_attach(op);
		if (ret)
			goto out;
	}
	stream = op->stream;
	scoped_guard(spinlock, &op->store->wait_lock) {
		list_del_init(&op->wait);
		op->phase = IO_OPAQUE_IDLE;
	}
	mutex_lock(&stream->lock);
	if (stream->closed || READ_ONCE(op->store->dead)) {
		ret = -ECANCELED;
		goto unlock_stream;
	}
	desc.arg.data = stream;
	old = set_active_memcg(op->store->memcg);
	lock_sock(sock->sk);
	if (tcp_sk(sock->sk)->urg_data)
		ret = -EOPNOTSUPP;
	else if (time_before(jiffies, op->retry_at))
		ret = -ENOMEM;
	else
		ret = tcp_read_sock(sock->sk, &desc, io_opaque_actor);
	if (desc.error)
		ret = desc.error;
	if (sock->sk->sk_err)
		stream->error = sock_error(sock->sk);
	if ((sock->sk->sk_shutdown & RCV_SHUTDOWN) &&
	    skb_queue_empty(&sock->sk->sk_receive_queue))
		stream->eof = true;
	release_sock(sock->sk);
	set_active_memcg(old);
	if (ret < 0 && ret != -ENOBUFS && ret != -ENOMEM)
		stream->error = ret;
	io_opaque_stream_process(stream);
	if (ret == -ENOBUFS) {
		struct io_opaque_req *keep = io_opaque_capture_keep(stream);

		/* A private prefix must not pin quota indefinitely awaiting itself. */
		if (keep && keep->progress) {
			rb_erase_cached(&keep->decision, &stream->decisions);
			RB_CLEAR_NODE(&keep->decision);
			io_opaque_queue_ready(keep, -ENOBUFS);
		}
	}
	if (stream->error || stream->eof) {
		ret = stream->error;
	} else if (ret == -ENOBUFS || ret == -ENOMEM) {
		scoped_guard(spinlock, &op->store->wait_lock) {
			op->need_bytes = ret == -ENOMEM ? 0 : stream->need_bytes;
			if (ret == -ENOMEM && !time_before(jiffies, op->retry_at))
				op->retry_at = jiffies + msecs_to_jiffies(IO_OPAQUE_ALLOC_RETRY_MS);
			else if (ret == -ENOBUFS)
				op->retry_at = jiffies;
			op->phase = IO_OPAQUE_BUDGET_POLL;
			list_add_tail(&op->wait, &op->store->budget_waits);
		}
		/* Recheck after enrollment to close the release/enrollment race. */
		io_opaque_wake_budget(op->store);
		ret = IOU_RETRY;
	} else if (!desc.count && (issue_flags & IO_URING_F_MULTISHOT)) {
		/* Drain another bounded batch without waiting for a new TCP edge. */
		ret = IOU_REQUEUE;
	} else {
		ret = IOU_RETRY;
	}
unlock_stream:
	mutex_unlock(&stream->lock);
out:
	if (ret >= 0 && ret != IOU_ISSUE_SKIP_COMPLETE) {
		io_req_set_res(req, ret, 0);
		ret = IOU_COMPLETE;
	}
	io_ring_submit_unlock(req->ctx, issue_flags);
	return ret;
}

static int io_opaque_copy(struct io_opaque_data *data, u64 off, u32 length,
			  void __user *dest)
{
	struct io_opaque_extent *extent;
	u64 end = off + length, done = off;

	if (end > data->length)
		return -ENODATA;

	list_for_each_entry(extent, &data->extents, list) {
		u64 first = max(off, extent->start);
		u64 last = min(end, extent->start + extent->length);
		u32 pos = extent->offset + first - extent->start;

		if (first >= last)
			continue;
		if (first != done)
			return -ENODATA;
		while (first < last) {
			u32 offset = offset_in_page(pos);
			u32 chunk = min_t(u64, last - first, PAGE_SIZE - offset);
			void *mapped = kmap_local_page(extent->backing->page +
						      (pos >> PAGE_SHIFT));
			unsigned long left;

			left = copy_to_user(dest + first - off, mapped + offset, chunk);
			kunmap_local(mapped);
			if (left)
				return -EFAULT;
			pos += chunk;
			first += chunk;
		}
		done = last;
	}
	return done == end ? length : -ENODATA;
}

static void io_opaque_ready(struct io_tw_req tw_req, io_tw_token_t tw)
{
	struct io_kiocb *req = tw_req.req;
	struct io_opaque_req *op = req->async_data;
	struct io_opaque_store *store = op->store;
	int ret = op->result;

	io_tw_lock(req->ctx, tw);
	list_del_init(&op->cancel);
	op->phase = IO_OPAQUE_IDLE;
	if (tw.cancel || READ_ONCE(op->canceled) || READ_ONCE(store->dead))
		ret = -ECANCELED;
	if (ret >= 0 && op->op == IORING_OPAQUE_READ_STREAM)
		ret = io_opaque_copy(op->data, 0, op->length, op->addr);
	if (ret >= 0 && op->op == IORING_OPAQUE_KEEP) {
		ret = op->data->length == op->length ? io_opaque_vector(op->data) : -ENODATA;
		if (!ret) {
			struct io_opaque_slot *slot = &store->objects[op->slot];

			mutex_lock(&store->tables);
			if (store->dead || !slot->reserved ||
			    slot->generation != op->handle >> 32) {
				ret = -ESTALE;
			} else {
				slot->data = op->data;
				op->data = NULL;
				slot->reserved = false;
				op->slot_reserved = false;
				slot->born = jiffies;
				slot->retry = jiffies;
				if (store->policy.flags & IORING_OPAQUE_AUTO_COMPACT) {
					list_add_tail(&slot->candidate, &store->candidates);
					mod_delayed_work(system_unbound_wq, &store->auto_work, 1);
				}
				ret = op->length;
			}
			mutex_unlock(&store->tables);
		}
	}
	if (op->op == IORING_OPAQUE_COMPACT) {
		if (ret >= 0)
			ret = io_opaque_compact_publish(op);
		mutex_lock(&store->tables);
		{
			struct io_opaque_slot *slot = io_opaque_lookup(store, op->handle);

			if (slot)
				slot->compacting = false;
		}
		mutex_unlock(&store->tables);
	}
	if (ret < 0 && op->op == IORING_OPAQUE_KEEP) {
		mutex_lock(&op->stream->lock);
		io_opaque_restore(op);
		io_opaque_stream_process(op->stream);
		mutex_unlock(&op->stream->lock);
	}
	if (ret < 0)
		req_set_fail(req);
	if (ret < 0 && req->opcode == IORING_OP_OPAQUE_OBJ_SEND && op->progress)
		ret = op->progress;
	if (ret >= 0 && op->op == IORING_OPAQUE_KEEP)
		io_req_set_res32(req, ret, 0, op->handle, 0);
	else
		io_req_set_res(req, ret, op->consumed ? IORING_CQE_F_OPAQUE_CONSUMED : 0);
	io_req_task_complete(tw_req, tw);
}

static struct io_opaque_store *io_opaque_get_store(struct io_kiocb *req, u32 id)
{
	struct io_zcrx_ifq *ifq = xa_load(&req->ctx->zcrx_ctxs, id);

	return ifq ? ifq->opaque : NULL;
}

static bool io_opaque_stream_op(u16 op)
{
	return op == IORING_OPAQUE_READ_STREAM || op == IORING_OPAQUE_KEEP ||
	       op == IORING_OPAQUE_DISCARD || op == IORING_OPAQUE_STREAM_STAT ||
	       op == IORING_OPAQUE_STREAM_CLOSE;
}

int io_opaque_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_opaque_store *store = io_opaque_get_store(req, READ_ONCE(sqe->zcrx_ifq_idx));
	struct io_opaque_req *op;
	u64 end;
	u16 cmd = READ_ONCE(sqe->ioprio);
	bool control;

	if (!store)
		return -ENXIO;
	if (READ_ONCE(sqe->fd) != -1 || READ_ONCE(sqe->rw_flags) ||
	    READ_ONCE(sqe->buf_index) || READ_ONCE(sqe->__pad2[0]) ||
	    cmd > IORING_OPAQUE_SET_POLICY ||
	    (req->flags & (REQ_F_CQE_SKIP | REQ_F_FIXED_FILE)))
		return -EINVAL;
	control = cmd == IORING_OPAQUE_FREE || cmd == IORING_OPAQUE_STAT ||
		  cmd == IORING_OPAQUE_STREAM_CLOSE || cmd == IORING_OPAQUE_SET_POLICY;
	op = io_opaque_req_alloc(req, store, control);
	if (IS_ERR(op))
		return PTR_ERR(op);
	op->op = cmd;
	op->handle = READ_ONCE(sqe->addr);
	op->offset = READ_ONCE(sqe->off);
	op->length = READ_ONCE(sqe->len);
	op->addr = u64_to_user_ptr(READ_ONCE(sqe->addr3));
	if (check_add_overflow(op->offset, (u64)op->length, &end))
		return -EOVERFLOW;
	if (cmd <= IORING_OPAQUE_READ) {
		if (!op->length || op->length > INT_MAX)
			return -EINVAL;
		if ((cmd == IORING_OPAQUE_READ_STREAM || cmd == IORING_OPAQUE_READ) &&
		    (op->length > IO_OPAQUE_READ_MAX || !op->addr))
			return -EINVAL;
		if ((cmd == IORING_OPAQUE_KEEP || cmd == IORING_OPAQUE_DISCARD) && op->addr)
			return -EINVAL;
		if (cmd == IORING_OPAQUE_KEEP &&
		    (op->length > store->config.max_object_size ||
		     op->length > store->config.hard_limit - store->config.compact_headroom))
			return -EMSGSIZE;
	} else if (op->offset) {
		return -EINVAL;
	}
	if (cmd == IORING_OPAQUE_FREE || cmd == IORING_OPAQUE_STREAM_CLOSE ||
	    cmd == IORING_OPAQUE_COMPACT) {
		if (op->length || op->addr)
			return -EINVAL;
	}
	if (cmd == IORING_OPAQUE_STAT || cmd == IORING_OPAQUE_SET_POLICY) {
		if (op->handle || !op->addr)
			return -EINVAL;
	}
	return 0;
}

/* Resolve resources when links permit execution, never while preparing SQEs. */
static int io_opaque_resolve(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_slot *slot;
	u16 cmd = op->op;
	int ret = 0;

	if (op->resolved)
		return 0;
	mutex_lock(&store->tables);
	if (io_opaque_stream_op(cmd)) {
		u32 index = (u32)op->handle;

		if (!index || index > store->config.max_streams ||
		    store->streams[index - 1].generation != op->handle >> 32 ||
		    !store->streams[index - 1].stream) {
			ret = -ESTALE;
		} else {
			op->stream = store->streams[index - 1].stream;
			refcount_inc(&op->stream->refs);
			op->req->file = get_file(op->stream->file);
		}
	} else if (cmd == IORING_OPAQUE_READ || cmd == IORING_OPAQUE_OBJECT_STAT) {
		slot = io_opaque_lookup(store, op->handle);
		if (!slot || !slot->data) {
			ret = -ESTALE;
		} else {
			op->data = slot->data;
			refcount_inc(&op->data->refs);
			if (cmd == IORING_OPAQUE_READ &&
			    (op->offset > op->data->length ||
			     op->length > op->data->length - op->offset))
				ret = -ERANGE;
		}
	}
	mutex_unlock(&store->tables);
	if (ret)
		return ret;
	if (cmd == IORING_OPAQUE_READ_STREAM || cmd == IORING_OPAQUE_KEEP) {
		op->data = io_opaque_data_alloc();
		if (!op->data)
			return -ENOMEM;
	}
	if (cmd == IORING_OPAQUE_KEEP) {
		ret = io_opaque_slot_reserve(op);
		if (ret)
			return ret;
	}
	op->resolved = true;
	return 0;
}

static int io_opaque_range_start(struct io_opaque_req *op)
{
	struct io_opaque_stream *stream = op->stream;
	struct rb_node *node;
	int ret = 0;

	guard(mutex)(&stream->lock);
	if (stream->closed)
		return -ECANCELED;
	if (op->op == IORING_OPAQUE_READ_STREAM) {
		for (node = rb_first_cached(&stream->decisions); node; node = rb_next(node)) {
			struct io_opaque_req *other;

			other = rb_entry(node, struct io_opaque_req, decision);

			if (io_opaque_overlap(op->offset, op->length, other->offset, other->length))
				return -EBUSY;
		}
		list_add_tail(&op->read, &stream->reads);
	} else {
		ret = io_opaque_range_insert(op);
		if (ret)
			return ret;
	}
	io_opaque_wait(op, IO_OPAQUE_RANGE_WAIT);
	io_opaque_stream_process(stream);
	/* DISCARD can make progress without allocating any payload backing. */
	if (op->op == IORING_OPAQUE_DISCARD)
		io_opaque_wake_stream(stream);
	return IOU_ISSUE_SKIP_COMPLETE;
}

static int io_opaque_stat(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_uring_opaque_stat stat = {
		.backing_bytes = atomic64_read(&store->bytes),
		.hard_limit = store->config.hard_limit,
		.copied_bytes = atomic64_read(&store->copied),
		.compacted_bytes = atomic64_read(&store->compacted),
		.compact_count = atomic64_read(&store->compactions),
		.extents = atomic_read(&store->extents),
		.requests = atomic_read(&store->requests),
	};
	u32 i;

	if (op->length != sizeof(stat))
		return -EINVAL;
	mutex_lock(&store->tables);
	for (i = 0; i < store->config.max_objects; i++)
		stat.objects += !!store->objects[i].data;
	for (i = 0; i < store->config.max_streams; i++)
		stat.streams += !!store->streams[i].stream;
	mutex_unlock(&store->tables);
	return copy_to_user(op->addr, &stat, sizeof(stat)) ? -EFAULT : 0;
}

int io_opaque_issue(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_opaque_req *op = req->async_data;
	struct io_opaque_store *store = op->store;
	int ret;

	io_ring_submit_lock(req->ctx, issue_flags);
	if (READ_ONCE(store->dead)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	ret = io_opaque_resolve(op);
	if (ret)
		goto out;
	switch (op->op) {
	case IORING_OPAQUE_READ_STREAM:
	case IORING_OPAQUE_KEEP:
	case IORING_OPAQUE_DISCARD:
		ret = io_opaque_range_start(op);
		break;
	case IORING_OPAQUE_READ:
		ret = io_opaque_copy(op->data, op->offset, op->length, op->addr);
		break;
	case IORING_OPAQUE_FREE:
		mutex_lock(&store->tables);
		{
			struct io_opaque_data *data = io_opaque_detach(store,
						io_opaque_lookup(store, op->handle));

			ret = data ? 0 : -ESTALE;
			mutex_unlock(&store->tables);
			io_opaque_data_put(store, data);
		}
		break;
	case IORING_OPAQUE_STAT:
		ret = io_opaque_stat(op);
		break;
	case IORING_OPAQUE_OBJECT_STAT:
		{
			struct io_uring_opaque_object_stat stat = {
				.length = op->data->length,
				.backing_bytes = op->data->charge,
				.extents = op->data->nr,
				.flags = op->data->dense ? IORING_OPAQUE_COMPACTED : 0,
			};
			struct io_opaque_slot *slot;

			mutex_lock(&store->tables);
			slot = io_opaque_lookup(store, op->handle);
			if (slot && slot->compacting)
				stat.flags |= IORING_OPAQUE_COMPACTING;
			mutex_unlock(&store->tables);
			ret = op->length != sizeof(stat) ? -EINVAL :
			      copy_to_user(op->addr, &stat, sizeof(stat)) ? -EFAULT : 0;
		}
		break;
	case IORING_OPAQUE_STREAM_STAT:
		{
			struct io_uring_opaque_stream_stat stat;

			mutex_lock(&op->stream->lock);
			stat.rx_next = op->stream->next;
			stat.undecided_bytes = op->stream->undecided;
			stat.flags = op->stream->eof ? IORING_OPAQUE_STREAM_EOF : 0;
			stat.error = op->stream->error;
			mutex_unlock(&op->stream->lock);
			ret = op->length != sizeof(stat) ? -EINVAL :
			      copy_to_user(op->addr, &stat, sizeof(stat)) ? -EFAULT : 0;
		}
		break;
	case IORING_OPAQUE_STREAM_CLOSE:
		mutex_lock(&store->tables);
		if (store->streams[(u32)op->handle - 1].stream == op->stream) {
			store->streams[(u32)op->handle - 1].stream = NULL;
			mutex_unlock(&store->tables);
			io_opaque_stream_close(op->stream);
			io_opaque_stream_put(op->stream);
			ret = 0;
		} else {
			mutex_unlock(&store->tables);
			ret = -ESTALE;
		}
		break;
	case IORING_OPAQUE_COMPACT:
		ret = io_opaque_compact_start(op);
		break;
	case IORING_OPAQUE_SET_POLICY:
		ret = io_opaque_set_policy(op);
		break;
	default:
		ret = -EINVAL;
	}
out:
	if (ret != IOU_ISSUE_SKIP_COMPLETE) {
		if (ret < 0)
			req_set_fail(req);
		io_req_set_res(req, ret, 0);
		ret = IOU_COMPLETE;
	}
	io_ring_submit_unlock(req->ctx, issue_flags);
	return ret;
}

static bool io_opaque_cancel_req(struct io_opaque_req *op)
{
	enum io_opaque_phase phase;

	if (op->stream)
		mutex_lock(&op->stream->lock);
	spin_lock(&op->store->wait_lock);
	phase = op->phase;
	if (op->canceled || phase == IO_OPAQUE_IDLE) {
		spin_unlock(&op->store->wait_lock);
		if (op->stream)
			mutex_unlock(&op->stream->lock);
		return false;
	}
	WRITE_ONCE(op->canceled, true);
	/* A queued copy is owned by the shared worker until it reports readiness. */
	if (phase != IO_OPAQUE_COPY_WORK)
		list_del_init(&op->wait);
	spin_unlock(&op->store->wait_lock);
	if (phase == IO_OPAQUE_RANGE_WAIT) {
		list_del_init(&op->read);
		if (!RB_EMPTY_NODE(&op->decision)) {
			rb_erase_cached(&op->decision, &op->stream->decisions);
			RB_CLEAR_NODE(&op->decision);
		}
	}
	if (op->stream)
		mutex_unlock(&op->stream->lock);
	if (phase != IO_OPAQUE_QUEUED && phase != IO_OPAQUE_COPY_WORK)
		io_opaque_queue_ready(op, -ECANCELED);
	return true;
}

int io_opaque_cancel(struct io_ring_ctx *ctx, struct io_cancel_data *cd,
		     unsigned int issue_flags)
{
	struct io_opaque_req *op;
	int nr = 0;

	io_ring_submit_lock(ctx, issue_flags);
	list_for_each_entry(op, &ctx->opaque_waits, cancel) {
		if (!io_cancel_req_match(op->req, cd) || !io_opaque_cancel_req(op))
			continue;
		nr++;
		break;
	}
	io_ring_submit_unlock(ctx, issue_flags);
	return nr ? 0 : -ENOENT;
}

bool io_opaque_cancel_all(struct io_ring_ctx *ctx, struct io_uring_task *tctx,
			  bool cancel_all)
{
	struct io_opaque_req *op;
	bool found = false;

	lockdep_assert_held(&ctx->uring_lock);
	list_for_each_entry(op, &ctx->opaque_waits, cancel)
		if (io_match_task_safe(op->req, tctx, cancel_all))
			found |= io_opaque_cancel_req(op);
	return found;
}

void io_opaque_cleanup(struct io_kiocb *req)
{
	struct io_opaque_req *op = req->async_data;
	struct io_opaque_store *store;
	bool close = false;

	if (!op)
		return;
	store = op->store;
	list_del_init(&op->cancel);
	scoped_guard(spinlock, &store->wait_lock)
		list_del_init(&op->wait);
	/* Waiters have stopped accessing this request before cleanup. */
	if (op->stream) {
		mutex_lock(&op->stream->lock);
		list_del_init(&op->read);
		if (!RB_EMPTY_NODE(&op->decision)) {
			rb_erase_cached(&op->decision, &op->stream->decisions);
			RB_CLEAR_NODE(&op->decision);
		}
		if (op->op == IORING_OPAQUE_KEEP)
			io_opaque_restore(op);
		if (op->stream->collector == op) {
			op->stream->collector = NULL;
			close = !op->stream->eof && !op->stream->closed && !op->stream->error;
		}
		mutex_unlock(&op->stream->lock);
		if (close)
			io_opaque_stream_close(op->stream);
	}
	if (op->slot_reserved) {
		mutex_lock(&store->tables);
		store->objects[op->slot].reserved = false;
		mutex_unlock(&store->tables);
	}
	io_opaque_tx_put(op);
	io_opaque_data_put(store, op->data);
	io_opaque_data_put(store, op->replacement);
	if (op->reserved)
		io_opaque_uncharge(store, op->reserved);
	io_opaque_stream_put(op->stream);
	if (op->counted)
		atomic_dec(&store->requests);
	io_req_async_data_free(req);
}

void io_opaque_fail(struct io_kiocb *req)
{
	struct io_opaque_req *op = req->async_data;

	if (op && req->opcode == IORING_OP_OPAQUE_OBJ_SEND) {
		if (op->progress)
			req->cqe.res = op->progress;
		if (op->consumed)
			req->cqe.flags |= IORING_CQE_F_OPAQUE_CONSUMED;
	}
}

static void io_opaque_send_iter(struct io_opaque_req *op)
{
	iov_iter_bvec(&op->iter, ITER_SOURCE, op->data->bvec, op->data->nr,
		      op->data->length);
	iov_iter_advance(&op->iter, op->offset);
	iov_iter_truncate(&op->iter, op->length);
}

static int io_opaque_send_acquire(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_slot *slot = io_opaque_lookup(store, op->handle);

	if (store->dead)
		return -ESHUTDOWN;
	if (!slot || !slot->data)
		return -ESTALE;
	if (op->offset > slot->data->length ||
	    op->length > slot->data->length - op->offset)
		return -ERANGE;
	if (op->send_flags & IORING_OPAQUE_SEND_LAST) {
		op->data = io_opaque_detach(store, slot);
		op->consumed = true;
	} else {
		op->data = slot->data;
		refcount_inc(&op->data->refs);
	}
	io_opaque_send_iter(op);
	return 0;
}

int io_opaque_send_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_opaque_store *store = io_opaque_get_store(req, READ_ONCE(sqe->zcrx_ifq_idx));
	struct io_opaque_req *op;
	u16 send_flags = READ_ONCE(sqe->ioprio);
	u32 msg_flags = READ_ONCE(sqe->msg_flags);
	u64 end;

	if (!store)
		return -ENXIO;
	if ((send_flags & ~IORING_OPAQUE_SEND_LAST) ||
	    ((send_flags & IORING_OPAQUE_SEND_LAST) && (req->flags & REQ_F_CQE_SKIP)) ||
	    READ_ONCE(sqe->buf_index) || READ_ONCE(sqe->addr3) || READ_ONCE(sqe->__pad2[0]) ||
	    (msg_flags & ~(MSG_MORE | MSG_DONTWAIT)))
		return -EINVAL;
	op = io_opaque_req_alloc(req, store, false);
	if (IS_ERR(op))
		return PTR_ERR(op);
	op->op = U16_MAX;
	op->send_flags = send_flags;
	op->handle = READ_ONCE(sqe->addr);
	op->offset = READ_ONCE(sqe->off);
	op->length = READ_ONCE(sqe->len);
	op->msg_flags = msg_flags;
	if (op->msg_flags & MSG_DONTWAIT)
		req->flags |= REQ_F_NOWAIT;
	if (!op->length || op->length > INT_MAX ||
	    check_add_overflow(op->offset, (u64)op->length, &end))
		return -EINVAL;
	return 0;
}

static int io_opaque_tx_enter(struct io_opaque_req *op, struct sock *sk)
{
	struct io_opaque_store *store = op->store;
	struct rb_node **link = &store->txs.rb_node, *parent = NULL;
	struct io_opaque_tx *tx = NULL;

	guard(mutex)(&store->tables);
	if (!op->tx) {
		while (*link) {
			parent = *link;
			tx = rb_entry(parent, struct io_opaque_tx, node);
			if ((unsigned long)sk < (unsigned long)tx->sk)
				link = &parent->rb_left;
			else if ((unsigned long)sk > (unsigned long)tx->sk)
				link = &parent->rb_right;
			else
				break;
		}
		if (!*link) {
			tx = kzalloc_obj(*tx, GFP_KERNEL_ACCOUNT);
			if (!tx)
				return -ENOMEM;
			tx->sk = sk;
			INIT_LIST_HEAD(&tx->requests);
		}
		if (tx->failed)
			return -ECANCELED;
		{
			int ret = io_opaque_send_acquire(op);

			if (ret) {
				if (!*link)
					kfree(tx);
				return ret;
			}
		}
		if (!*link) {
			rb_link_node(&tx->node, parent, link);
			rb_insert_color(&tx->node, &store->txs);
		}
		op->tx = tx;
		list_add_tail(&op->send, &tx->requests);
	}
	if (list_first_entry(&op->tx->requests, struct io_opaque_req, send) != op) {
		io_opaque_wait(op, IO_OPAQUE_SEND_WAIT);
		return IOU_ISSUE_SKIP_COMPLETE;
	}
	return 0;
}

static void io_opaque_tx_put(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_tx *tx = op->tx;
	struct io_opaque_req *next;
	bool head;

	if (!tx)
		return;
	guard(mutex)(&store->tables);
	head = list_first_entry(&tx->requests, struct io_opaque_req, send) == op;
	list_del_init(&op->send);
	if (list_empty(&tx->requests)) {
		rb_erase(&tx->node, &store->txs);
		kfree(tx);
	} else if (head) {
		/* A broken object must not splice the next queued object onto TCP. */
		if (op->progress != op->length) {
			tx->failed = true;
			list_for_each_entry(next, &tx->requests, send) {
				WRITE_ONCE(next->canceled, true);
				if (next->phase == IO_OPAQUE_SEND_WAIT)
					io_opaque_queue_ready(next, -ECANCELED);
			}
			goto out;
		}
		next = list_first_entry(&tx->requests, struct io_opaque_req, send);
		guard(spinlock)(&store->wait_lock);
		if (next->phase == IO_OPAQUE_SEND_WAIT) {
			next->phase = IO_OPAQUE_QUEUED;
			next->req->io_task_work.func = io_opaque_resume;
			io_req_task_work_add(next->req);
		}
	}
out:
	op->tx = NULL;
}

int io_opaque_send(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_opaque_req *op = req->async_data;
	struct socket *sock = sock_from_file(req->file);
	struct msghdr msg = {
		.msg_flags = MSG_SPLICE_PAGES | MSG_NOSIGNAL | op->msg_flags,
	};
	int ret;

	io_ring_submit_lock(req->ctx, issue_flags);
	if (!sock || sock->type != SOCK_STREAM || sock->sk->sk_protocol != IPPROTO_TCP) {
		ret = -EOPNOTSUPP;
		goto finish;
	}
	if (READ_ONCE(op->store->dead)) {
		ret = -ESHUTDOWN;
		goto finish;
	}
	ret = io_opaque_tx_enter(op, sock->sk);
	if (ret)
		goto out;
	if (issue_flags & IO_URING_F_NONBLOCK)
		msg.msg_flags |= MSG_DONTWAIT;
	msg.msg_iter = op->iter;
	/* io-wq may block in TCP; other requests must retain access to the ring. */
	io_ring_submit_unlock(req->ctx, issue_flags);
	ret = sock_sendmsg(sock, &msg);
	io_ring_submit_lock(req->ctx, issue_flags);
	if (ret > 0) {
		op->iter = msg.msg_iter;
		op->progress += ret;
		if (iov_iter_count(&op->iter) && !(req->flags & REQ_F_NOWAIT)) {
			ret = -EAGAIN;
			goto out;
		}
	} else if (ret == -EAGAIN && (issue_flags & IO_URING_F_NONBLOCK) &&
		   !(req->flags & REQ_F_NOWAIT)) {
		goto out;
	}
finish:
	if (ret == -ERESTARTSYS)
		ret = -EINTR;
	if (ret < 0)
		req_set_fail(req);
	if (op->progress)
		ret = op->progress;
	io_req_set_res(req, ret, op->consumed ? IORING_CQE_F_OPAQUE_CONSUMED : 0);
	ret = IOU_COMPLETE;
out:
	io_ring_submit_unlock(req->ctx, issue_flags);
	return ret;
}

#include "opaque_compact.c"

#ifdef CONFIG_IO_URING_OPAQUE_OBJ_KUNIT_TEST
#include "opaque_test.c"
#endif
