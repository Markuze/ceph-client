// SPDX-License-Identifier: GPL-2.0
/* Kernel-resident immutable TCP payloads. See userspace-api/io_uring-opaque. */
#include <kunit/static_stub.h>
#include <linux/bitmap.h>
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
#include <net/rps.h>
#include <net/tcp.h>
#include <trace/events/sock.h>
#include <uapi/linux/io_uring/opaque.h>

#include "io_uring.h"
#include "cancel.h"
#include "opaque_internal.h"
#include "rsrc.h"
#include "tw.h"

bool io_opaque_cache_init(struct io_ring_ctx *ctx)
{
	return io_alloc_cache_init(&ctx->opaque_cache, IO_ALLOC_CACHE_MAX,
				   sizeof(struct io_opaque_req), sizeof(struct io_opaque_req));
}

/* Frees a request left in the cache, with the vector block it may carry. */
static void io_opaque_req_destroy(const void *entry)
{
	const struct io_opaque_req *op = entry;

	kvfree(op->vec);
	kfree(entry);
}

void io_opaque_cache_free(struct io_ring_ctx *ctx)
{
	io_alloc_cache_free(&ctx->opaque_cache, io_opaque_req_destroy);
}

static void io_opaque_resume(struct io_tw_req tw_req, io_tw_token_t tw);
static void io_opaque_ready(struct io_tw_req tw_req, io_tw_token_t tw);
static void io_opaque_vec_cleanup(struct io_opaque_req *op);
VISIBLE_IF_KUNIT void io_opaque_stream_close(struct io_opaque_stream *stream);
static void io_opaque_tx_put(struct io_opaque_req *op);

static bool io_opaque_budget_ready(struct io_opaque_req *op, unsigned long now)
{
	struct io_opaque_store *store = op->store;
	u64 limit = io_opaque_rx_limit(store);
	u64 used = atomic64_read(&store->bytes);

	lockdep_assert_held(&store->wait_lock);
	return !time_before(now, op->retry_at) && used <= limit &&
	       op->need_bytes <= limit - used &&
	       atomic_read(&store->extents) < store->config.max_extents - 2;
}

/* A DISCARD at the capture cursor consumes TCP bytes without allocating. */
static bool io_opaque_discard_ready(struct io_opaque_stream *stream)
{
	struct rb_node *node = rb_first_cached(&stream->decisions);
	struct io_opaque_req *op;

	lockdep_assert_held(&stream->lock);
	if (!node)
		return false;
	op = rb_entry(node, struct io_opaque_req, decision);
	return op->op == IORING_OPAQUE_DISCARD &&
	       op->offset + op->progress == stream->next &&
	       op->progress < op->length;
}

static bool io_opaque_collect_blocked(struct io_opaque_req *op)
{
	lockdep_assert_held(&op->stream->lock);
	lockdep_assert_held(&op->store->wait_lock);
	return op->phase == IO_OPAQUE_BUDGET_POLL &&
	       !io_opaque_budget_ready(op, jiffies) &&
	       !io_opaque_discard_ready(op->stream);
}

#if IS_ENABLED(CONFIG_KUNIT)
/* Times the waiters for budget have been looked at, for a test to read. */
atomic_long_t io_opaque_budget_wakes = ATOMIC_LONG_INIT(0);
#define IO_OPAQUE_BUDGET_WAKE()	atomic_long_inc(&io_opaque_budget_wakes)
#else
#define IO_OPAQUE_BUDGET_WAKE()	((void)0)
#endif

static void io_opaque_wake_budget(struct io_opaque_store *store)
{
	struct io_opaque_req *op, *next;
	unsigned long delay = MAX_JIFFY_OFFSET;
	unsigned long now = jiffies;
	unsigned int woken = 0;
	bool stopped;

	IO_OPAQUE_BUDGET_WAKE();
	guard(spinlock)(&store->wait_lock);
	stopped = READ_ONCE(store->dead);
	list_for_each_entry_safe(op, next, &store->budget_waits, wait) {
		struct socket *sock = sock_from_file(op->req->file);

		if (!stopped && !READ_ONCE(op->stream->closed)) {
			if (time_before(now, op->retry_at)) {
				delay = min(delay, op->retry_at - now);
				continue;
			}
			if (!io_opaque_budget_ready(op, now))
				continue;
			if (woken == IO_OPAQUE_WAKE_BATCH) {
				delay = 1;
				continue;
			}
		}
		list_del_init(&op->wait);
		/* A wake is not a reservation; the collector rechecks on entry. */
		woken++;
		/* Native multishot polling also observes FIN, RST and urgent data. */
		wake_up_interruptible_poll(sk_sleep(sock->sk), EPOLLIN);
	}
	if (!stopped && delay != MAX_JIFFY_OFFSET)
		queue_delayed_work(system_dfl_wq, &store->retry_work, delay);
}

VISIBLE_IF_KUNIT void io_opaque_retry_work(struct work_struct *work)
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
		if (!stream->closed && !READ_ONCE(stream->store->dead) &&
		    io_opaque_collect_blocked(op))
			return;
		list_del_init(&op->wait);
	}
	wake_up_interruptible_poll(sk_sleep(sock->sk), EPOLLIN);
}

int io_opaque_memory_mode(struct io_opaque_store *store)
{
	unsigned int seq;
	int mode, next;
	u64 used, transient;

	if (!store->config.high_watermark)
		return IORING_OPAQUE_MEMORY_NORMAL;
	for (;;) {
		mode = atomic_read(&store->memory_mode);
		/* Watermarks measure payload held: compaction copies and framing are not. */
		do {
			seq = read_seqcount_begin(&store->transient_seq);
			used = atomic64_read(&store->bytes);
			transient = atomic64_read(&store->transient);
		} while (read_seqcount_retry(&store->transient_seq, seq));
		used = used > transient ? used - transient : 0;
		next = mode;
		if (mode == IORING_OPAQUE_MEMORY_NORMAL && used >= store->config.high_watermark)
			next = IORING_OPAQUE_MEMORY_LOWMEM;
		else if (mode == IORING_OPAQUE_MEMORY_LOWMEM && used <= store->config.low_watermark)
			next = IORING_OPAQUE_MEMORY_NORMAL;
		if (next == mode)
			return mode;
		/* Recheck occupancy after winning a concurrent mode transition. */
		atomic_try_cmpxchg(&store->memory_mode, &mode, next);
	}
}

bool io_opaque_charge(struct io_opaque_store *store, u64 bytes, bool rx)
{
	u64 limit = store->config.hard_limit - store->config.reply_reserve;
	s64 used = atomic64_read(&store->bytes);

	if (rx)
		limit -= store->config.compact_headroom;
	for (;;) {
		if (bytes > limit || used > limit - bytes)
			return false;
		if (atomic64_try_cmpxchg(&store->bytes, &used, used + bytes)) {
			io_opaque_memory_mode(store);
			return true;
		}
	}
}

void io_opaque_uncharge(struct io_opaque_store *store, u64 bytes)
{
	atomic64_sub(bytes, &store->bytes);
	io_opaque_memory_mode(store);
	io_opaque_wake_budget(store);
}

/* Charge bytes that watermarks ignore; bytes and transient change in one write. */
bool io_opaque_charge_transient(struct io_opaque_store *store, u64 bytes, u64 limit)
{
	s64 used;
	bool charged = false;

	spin_lock(&store->transient_lock);
	write_seqcount_begin(&store->transient_seq);
	used = atomic64_read(&store->bytes);
	for (;;) {
		if (bytes > limit || used > limit - bytes)
			break;
		if (atomic64_try_cmpxchg(&store->bytes, &used, used + bytes)) {
			atomic64_add(bytes, &store->transient);
			charged = true;
			break;
		}
	}
	write_seqcount_end(&store->transient_seq);
	spin_unlock(&store->transient_lock);
	return charged;
}

/* Uncharge and stop excluding transient bytes; the two amounts may differ. */
void io_opaque_release_transient(struct io_opaque_store *store, u64 uncharge,
				 u64 transient)
{
	spin_lock(&store->transient_lock);
	write_seqcount_begin(&store->transient_seq);
	atomic64_sub(uncharge, &store->bytes);
	atomic64_sub(transient, &store->transient);
	write_seqcount_end(&store->transient_seq);
	spin_unlock(&store->transient_lock);
	io_opaque_memory_mode(store);
	if (uncharge)
		io_opaque_wake_budget(store);
}

/* Framing uses ordinary capacity first and the reply reserve only when that is full. */
bool io_opaque_charge_frame(struct io_opaque_store *store, u64 bytes, bool *reserved)
{
	u64 limit = store->config.reply_reserve;
	s64 used;

	if (io_opaque_charge_transient(store, bytes, io_opaque_rx_limit(store))) {
		*reserved = false;
	} else {
		used = atomic64_read(&store->reserve_framing);
		for (;;) {
			if (bytes > limit || used > limit - bytes)
				return false;
			if (atomic64_try_cmpxchg(&store->reserve_framing, &used, used + bytes))
				break;
		}
		*reserved = true;
	}
	atomic64_add(bytes, &store->framing);
	return true;
}

void io_opaque_uncharge_frame(struct io_opaque_store *store, u64 bytes, bool reserved)
{
	atomic64_sub(bytes, &store->framing);
	if (reserved)
		atomic64_sub(bytes, &store->reserve_framing);
	else
		io_opaque_release_transient(store, bytes, bytes);
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

struct io_opaque_extent *io_opaque_extent_new(struct io_opaque_store *store,
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

/*
 * Let go of @extent, and of its backing if it was the last to use it. Returns
 * the charge of the backing that went, for the caller to give back to the
 * store: giving it back lets those waiting for budget in, which one free does
 * once and a list of them once, not once per extent.
 */
static u64 io_opaque_extent_release(struct io_opaque_store *store, struct io_opaque_extent *extent)
{
	struct io_opaque_backing *backing = extent->backing;
	u64 charge = 0;

	list_del(&extent->list);
	if (!extent->embedded)
		kfree(extent);
	atomic_dec(&store->extents);
	if (refcount_dec_and_test(&backing->refs)) {
		charge = backing->charge;

		if (backing->accounted)
			io_opaque_memcg_uncharge(store, charge);
		/* A cached unit leaves the object charge; it is not payload held. */
		if (backing->owned)
			io_opaque_unit_put(store, backing->page);
		else
			put_page(backing->page);
		kfree(container_of(backing, struct io_opaque_chunk, backing));
	}
	return charge;
}

VISIBLE_IF_KUNIT void io_opaque_extent_free(struct io_opaque_store *store,
					    struct io_opaque_extent *extent)
{
	u64 charge = io_opaque_extent_release(store, extent);

	if (charge)
		io_opaque_uncharge(store, charge);
	else
		io_opaque_wake_budget(store);
}

/*
 * Free the extents of @list. What they held is returned to the store in one
 * step, and the waiters for budget are looked at once: each look takes
 * wait_lock and walks the list of them, which for the 700 extents of a 1 MB
 * object that is not a single mapping was 700 times.
 */
VISIBLE_IF_KUNIT void io_opaque_extents_free(struct io_opaque_store *store,
					     struct list_head *list)
{
	struct io_opaque_extent *extent, *next;
	bool any = false;
	u64 charge = 0;

	list_for_each_entry_safe(extent, next, list, list) {
		charge += io_opaque_extent_release(store, extent);
		any = true;
	}
	if (charge)
		io_opaque_uncharge(store, charge);
	else if (any)
		io_opaque_wake_budget(store);
}

struct io_opaque_data *io_opaque_data_alloc(void)
{
	struct io_opaque_data *data = kzalloc_obj(*data, GFP_KERNEL_ACCOUNT);

	if (data) {
		refcount_set(&data->refs, 1);
		INIT_LIST_HEAD(&data->extents);
	}
	return data;
}

void io_opaque_data_put(struct io_opaque_store *store,
			struct io_opaque_data *data)
{
	KUNIT_STATIC_STUB_REDIRECT(io_opaque_data_put, store, data);
	if (data && refcount_dec_and_test(&data->refs)) {
		io_opaque_extents_free(store, &data->extents);
		kvfree(data->bvec);
		kfree(data);
	}
}

/*
 * Build the version's segments, and for a version of more than
 * IO_OPAQUE_SEEK_STRIDE of them the anchors that find one without walking
 * from the first, in one allocation. Both are made once, when the version is,
 * and never change.
 */
int io_opaque_vector(struct io_opaque_data *data)
{
	struct io_opaque_extent *extent;
	struct io_opaque_backing *previous = NULL;
	u32 nr = 0, anchors;
	u64 at = 0;

	data->charge = 0;
	list_for_each_entry(extent, &data->extents, list) {
		nr++;
		if (extent->backing != previous)
			data->charge += extent->backing->charge;
		previous = extent->backing;
	}
	anchors = nr > IO_OPAQUE_SEEK_STRIDE ? DIV_ROUND_UP(nr, IO_OPAQUE_SEEK_STRIDE) : 0;
	data->bvec = kvmalloc(size_add(array_size(nr, sizeof(*data->bvec)),
				       array_size(anchors, sizeof(*data->anchor))),
			      GFP_KERNEL_ACCOUNT);
	if (!data->bvec)
		return -ENOMEM;
	data->anchor = anchors ? (u64 *)(data->bvec + nr) : NULL;
	data->nr = nr;
	nr = 0;
	list_for_each_entry(extent, &data->extents, list) {
		if (anchors && !(nr % IO_OPAQUE_SEEK_STRIDE))
			data->anchor[nr / IO_OPAQUE_SEEK_STRIDE] = at;
		bvec_set_page(&data->bvec[nr++], extent->backing->page,
			      extent->length, extent->offset);
		at += extent->length;
	}
	return 0;
}

/* Handle encoding is internal; zero is never a published handle. */
u64 io_opaque_handle(u32 index, u64 generation)
{
	return generation << IO_OPAQUE_INDEX_BITS | index;
}

static u32 io_opaque_index(u64 handle)
{
	return handle & IO_OPAQUE_INDEX_MASK;
}

VISIBLE_IF_KUNIT u32 io_opaque_find_slot(unsigned long *used, u32 count, u32 *cursor)
{
	u32 index = find_next_zero_bit(used, count, *cursor);

	if (index == count && *cursor) {
		index = find_first_zero_bit(used, *cursor);
		if (index == *cursor)
			return count;
	}
	if (index < count)
		*cursor = (index + 1) % count;
	return index;
}

VISIBLE_IF_KUNIT void io_opaque_release_slot(unsigned long *used, u32 index, u64 generation)
{
	if (generation != IO_OPAQUE_GENERATION_MAX)
		__clear_bit(index, used);
}

struct io_opaque_slot *io_opaque_lookup(struct io_opaque_store *store,
					u64 handle)
{
	u32 index = io_opaque_index(handle);

	lockdep_assert_held(&store->tables);
	if (!handle || index >= store->config.max_objects)
		return NULL;
	if (store->objects[index].generation != handle >> IO_OPAQUE_INDEX_BITS)
		return NULL;
	return &store->objects[index];
}

/* Drop handle visibility, transferring the table reference to the caller. */
VISIBLE_IF_KUNIT struct io_opaque_data *io_opaque_detach(struct io_opaque_store *store,
							 struct io_opaque_slot *slot)
{
	struct io_opaque_data *data;

	lockdep_assert_held(&store->tables);
	if (!slot || !slot->data)
		return NULL;
	data = slot->data;
	slot->data = NULL;
	store->nr_objects--;
	io_opaque_release_slot(store->object_used, slot - store->objects, slot->generation);
	slot->compacting = false;
	list_del_init(&slot->candidate);
	return data;
}

VISIBLE_IF_KUNIT int io_opaque_slot_reserve(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_slot *slot;
	u32 index;

	guard(mutex)(&store->tables);
	for (;;) {
		index = io_opaque_find_slot(store->object_used, store->config.max_objects,
					    &store->object_cursor);
		if (index >= store->config.max_objects)
			return -ENOSPC;
		slot = &store->objects[index];
		__set_bit(index, store->object_used);
		if (slot->generation != IO_OPAQUE_GENERATION_MAX)
			break;
	}
	slot->reserved = true;
	slot->generation++;
	if (op->data)
		op->data->forward_only =
			io_opaque_memory_mode(store) == IORING_OPAQUE_MEMORY_LOWMEM;
	op->slot = index;
	op->handle = io_opaque_handle(index, slot->generation);
	op->slot_reserved = true;
	return 0;
}

VISIBLE_IF_KUNIT void io_opaque_stream_put(struct io_opaque_stream *stream)
{
	if (stream && refcount_dec_and_test(&stream->refs)) {
		io_opaque_extents_free(stream->store, &stream->extents);
		fput(stream->file);
		mutex_destroy(&stream->lock);
		kfree(stream);
	}
}

struct io_opaque_store *io_opaque_alloc(struct io_ring_ctx *ctx,
					const struct io_uring_opaque_config *config)
{
	struct io_uring_opaque_config cfg = *config;
	struct io_opaque_store *store;
	int ret;
	u32 i;

	if (cfg.flags || cfg.__resv1 || cfg.__resv2 ||
	    !mem_is_zero(cfg.__resv, sizeof(cfg.__resv)))
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
	if (!IS_ALIGNED(cfg.reply_reserve, PAGE_SIZE) ||
	    cfg.reply_reserve >= cfg.hard_limit - cfg.compact_headroom ||
	    (!cfg.high_watermark && cfg.low_watermark) ||
	    (cfg.high_watermark &&
	     (!cfg.reply_reserve ||
	      !IS_ALIGNED(cfg.high_watermark, PAGE_SIZE) ||
	      !IS_ALIGNED(cfg.low_watermark, PAGE_SIZE) ||
	      cfg.low_watermark >= cfg.high_watermark ||
	      cfg.high_watermark > cfg.hard_limit - cfg.compact_headroom - cfg.reply_reserve)))
		return ERR_PTR(-EINVAL);
	store = kzalloc_obj(*store, GFP_KERNEL_ACCOUNT);
	if (!store)
		return ERR_PTR(-ENOMEM);
	refcount_set(&store->refs, 1);
	refcount_set(&store->users, 1);
	store->config = cfg;
	io_opaque_unit_cache_setup(store);
	store->objects = kvcalloc(cfg.max_objects, sizeof(*store->objects),
				  GFP_KERNEL_ACCOUNT);
	store->streams = kvcalloc(cfg.max_streams, sizeof(*store->streams),
				  GFP_KERNEL_ACCOUNT);
	store->object_used = bitmap_zalloc(cfg.max_objects, GFP_KERNEL_ACCOUNT);
	store->stream_used = bitmap_zalloc(cfg.max_streams, GFP_KERNEL_ACCOUNT);
	if (!store->objects || !store->streams || !store->object_used || !store->stream_used) {
		ret = -ENOMEM;
		goto free_store;
	}
	/* Cached destination units sit outside the hard limit but not RLIMIT_MEMLOCK. */
	ret = io_account_mem(ctx->user, ctx->mm_account,
			     (cfg.hard_limit >> PAGE_SHIFT) + io_opaque_cache_pages(store));
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
	spin_lock_init(&store->wait_lock);
	spin_lock_init(&store->tx_lock);
	spin_lock_init(&store->transient_lock);
	seqcount_spinlock_init(&store->transient_seq, &store->transient_lock);
	INIT_LIST_HEAD(&store->budget_waits);
	INIT_LIST_HEAD(&store->copies);
	INIT_LIST_HEAD(&store->candidates);
	store->txs = RB_ROOT;
	INIT_LIST_HEAD(&store->txs_idle);
	for (i = 0; i < cfg.max_objects; i++)
		INIT_LIST_HEAD(&store->objects[i].candidate);
	INIT_DELAYED_WORK(&store->retry_work, io_opaque_retry_work);
	INIT_DELAYED_WORK(&store->compact_work, io_opaque_compact_work);
	return store;
free_store:
	bitmap_free(store->stream_used);
	bitmap_free(store->object_used);
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
	/* Close enrollment before draining the compaction dispatcher. */
	scoped_guard(mutex, &store->tables) {
		guard(spinlock)(&store->wait_lock);

		if (store->dead)
			return;
		WRITE_ONCE(store->dead, 1);
	}
	flush_delayed_work(&store->compact_work);
	/* Retirement refuses units once dead is visible under the cache lock. */
	io_opaque_unit_drain(store);
	cancel_delayed_work_sync(&store->retry_work);
	for (i = 0; i < store->config.max_streams; i++) {
		struct io_opaque_stream *stream;

		mutex_lock(&store->tables);
		stream = store->streams[i].stream;
		store->streams[i].stream = NULL;
		if (stream) {
			store->nr_streams--;
			io_opaque_release_slot(store->stream_used, i, store->streams[i].generation);
		}
		mutex_unlock(&store->tables);
		if (stream) {
			io_opaque_stream_close(stream);
			io_opaque_stream_put(stream);
		}
	}
	for (i = 0; i < store->config.max_objects; i++) {
		struct io_opaque_data *data;

		mutex_lock(&store->tables);
		data = io_opaque_detach(store, &store->objects[i]);
		mutex_unlock(&store->tables);
		io_opaque_data_put(store, data);
	}
	io_opaque_wake_budget(store);
}

/* Free every queue; no request may be left on one. */
VISIBLE_IF_KUNIT void io_opaque_txs_free(struct io_opaque_store *store)
{
	struct io_opaque_tx *tx, *next;

	guard(spinlock)(&store->tx_lock);
	rbtree_postorder_for_each_entry_safe(tx, next, &store->txs, node) {
		WARN_ON_ONCE(!list_empty(&tx->requests) || tx->admitting);
		kfree(tx);
	}
	store->txs = RB_ROOT;
	INIT_LIST_HEAD(&store->txs_idle);
	store->nr_txs_idle = 0;
}

void io_opaque_free(struct io_opaque_store *store)
{
	if (!store)
		return;
	io_opaque_stop(store);
	WARN_ON_ONCE(atomic64_read(&store->bytes));
	WARN_ON_ONCE(atomic_read(&store->extents));
	WARN_ON_ONCE(atomic_read(&store->requests));
	WARN_ON_ONCE(store->cache.nr_idle || store->cache.nr_cooling);
	io_opaque_txs_free(store);
	io_unaccount_mem(store->user, store->mm,
			 (store->config.hard_limit >> PAGE_SHIFT) + io_opaque_cache_pages(store));
	obj_cgroup_put(store->objcg);
	mem_cgroup_put(store->memcg);
	if (store->mm)
		mmdrop(store->mm);
	free_uid(store->user);
	bitmap_free(store->object_used);
	bitmap_free(store->stream_used);
	kvfree(store->objects);
	kvfree(store->streams);
	mutex_destroy(&store->tables);
	kfree(store);
}

void io_opaque_get(struct io_opaque_store *store)
{
	refcount_inc(&store->refs);
	refcount_inc(&store->users);
}

void io_opaque_user_put(struct io_opaque_store *store)
{
	if (refcount_dec_and_test(&store->users))
		io_opaque_stop(store);
}

void io_opaque_put(struct io_opaque_store *store)
{
	if (refcount_dec_and_test(&store->refs))
		io_opaque_free(store);
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
	op = io_uring_alloc_async_data(&req->ctx->opaque_cache, req);
	if (!op) {
		if (!control)
			atomic_dec(&store->requests);
		return ERR_PTR(-ENOMEM);
	}
	/* Not the vector block, which a recycled request brings along. */
	memset(op, 0, offsetof(struct io_opaque_req, vec));
	op->op = U16_MAX;
	op->req = req;
	op->store = store;
	op->counted = !control;
	op->retry_at = jiffies;
	INIT_LIST_HEAD(&op->wait);
	INIT_LIST_HEAD(&op->cancel);
	req->flags |= REQ_F_NEED_CLEANUP;
	return op;
}

/*
 * A finished request goes back to its ring's cache, with the vector block it
 * carries, or is freed with it when the cache is full.
 */
static void io_opaque_req_free(struct io_ring_ctx *ctx, struct io_opaque_req *op)
{
	if (!io_alloc_cache_put(&ctx->opaque_cache, op)) {
		kvfree(op->vec);
		kvfree(op);
	}
}

/* The ring lock protects enrollment; wait_lock arbitrates remote wakeups. */
static void io_opaque_enroll(struct io_opaque_req *op)
{
	lockdep_assert_held(&op->req->ctx->uring_lock);
	if (list_empty(&op->cancel))
		list_add_tail(&op->cancel, &op->req->ctx->opaque_waits);
}

void io_opaque_wait(struct io_opaque_req *op, enum io_opaque_phase phase)
{
	io_opaque_enroll(op);
	guard(spinlock)(&op->store->wait_lock);
	op->phase = phase;
}

void io_opaque_queue_ready(struct io_opaque_req *op, int result)
{
	KUNIT_STATIC_STUB_REDIRECT(io_opaque_queue_ready, op, result);
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

VISIBLE_IF_KUNIT struct io_opaque_extent *io_opaque_extent_split(struct io_opaque_store *store,
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

VISIBLE_IF_KUNIT int io_opaque_available(struct io_opaque_stream *stream, u64 off, u64 len)
{
	struct io_opaque_extent *extent;
	u64 end = off + len;
	u64 start = off;

	list_for_each_entry(extent, &stream->extents, list) {
		if (extent->start + extent->length <= off)
			continue;
		if (extent->start > off)
			break;
		off = min(end, extent->start + extent->length);
		if (off == end)
			break;
	}
	if (off != start)
		return off - start;
	return off < stream->next ? -ENODATA : 0;
}

/* AVAILABLE must not count bytes reserved by a pending decision, even if
 * pressure has left that decision's prefix in the stream extents. */
static int io_opaque_unclassified(struct io_opaque_stream *stream, u64 off, u64 len)
{
	struct rb_node *node;

	for (node = rb_first_cached(&stream->decisions); node; node = rb_next(node)) {
		struct io_opaque_req *decision = rb_entry(node, struct io_opaque_req, decision);

		if (decision->offset > off) {
			len = min(len, decision->offset - off);
			break;
		}
	}
	return io_opaque_available(stream, off, len);
}

static int io_opaque_snapshot(struct io_opaque_req *op)
{
	struct io_opaque_extent *extent;
	int ret = io_opaque_available(op->stream, op->offset, op->length);
	u64 end;

	if (ret <= 0)
		return ret;
	end = op->offset + ret;
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
	op->data->length = ret;
	return ret;
}

VISIBLE_IF_KUNIT int io_opaque_take(struct io_opaque_req *op)
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
		if (op->op == IORING_OPAQUE_RECV_OBJECT) {
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
VISIBLE_IF_KUNIT void io_opaque_restore(struct io_opaque_req *op)
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

VISIBLE_IF_KUNIT int io_opaque_range_insert(struct io_opaque_req *op)
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
		if (io_opaque_overlap(op->offset, op->length, other->offset, 1))
			return -EBUSY;
	rb_link_node(&op->decision, parent, link);
	rb_insert_color_cached(&op->decision, &stream->decisions, leftmost);
	return 0;
}

/* Validate a blocked TAKE without taking and restoring its retained prefix. */
static int io_opaque_defer_take(struct io_opaque_req *op)
{
	struct io_opaque_stream *stream = op->stream;
	struct io_opaque_extent *extent, *tail = NULL;
	u64 off = op->offset, end = min(off + op->length, stream->next);
	u64 charge = 0, left;

	/* Allocation backoff can leave an already private prefix in place. */
	if (op->progress)
		return 0;
	list_for_each_entry(extent, &stream->extents, list) {
		if (extent->start + extent->length <= off)
			continue;
		if (extent->start > off)
			break;
		if (!tail || tail->backing != extent->backing)
			charge += extent->backing->charge;
		tail = extent;
		off = min(end, extent->start + extent->length);
		if (off == end)
			break;
	}
	if (off < end)
		return -ENODATA;
	left = op->length - (off - op->offset);
	if (tail && tail->backing->copied &&
	    tail->offset + tail->length == tail->backing->filled)
		left -= min_t(u64, left, PAGE_SIZE - tail->backing->filled);
	return charge + ALIGN(left, PAGE_SIZE) > io_opaque_rx_limit(op->store) ?
		-ENOBUFS : 0;
}

static void io_opaque_stream_process(struct io_opaque_stream *stream)
{
	struct io_opaque_req *op, *next;
	struct rb_node *node, *after;
	int ret;

	list_for_each_entry_safe(op, next, &stream->reads, read) {
		ret = stream->closed ? -ECANCELED :
			op->op == IORING_OPAQUE_AVAILABLE ?
			io_opaque_unclassified(stream, op->offset, op->length) :
			io_opaque_snapshot(op);
		if (!ret && !stream->eof && !stream->closed && !stream->error)
			continue;
		list_del_init(&op->read);
		if (!ret && stream->error)
			ret = stream->error;
		io_opaque_queue_ready(op, ret);
	}
	for (node = rb_first_cached(&stream->decisions); node; node = after) {
		op = rb_entry(node, struct io_opaque_req, decision);
		after = rb_next(node);
		if (op->op == IORING_OPAQUE_RECV_OBJECT &&
		    op->offset + op->length > stream->next &&
		    stream->collector &&
		    stream->collector->phase == IO_OPAQUE_BUDGET_POLL &&
		    !stream->closed && !stream->eof && !stream->error)
			ret = io_opaque_defer_take(op);
		else
			ret = stream->closed ? -ECANCELED : io_opaque_take(op);
		if (!ret && op->op == IORING_OPAQUE_RECV_OBJECT &&
		    op->data->charge + io_opaque_dense_left(op) >
		    io_opaque_rx_limit(op->store))
			ret = -ENOBUFS;
		if (!ret && !stream->eof && !stream->closed && !stream->error)
			continue;
		rb_erase_cached(node, &stream->decisions);
		RB_CLEAR_NODE(node);
		if (!ret)
			ret = stream->error ?: -ENODATA;
		if (ret < 0 && op->op == IORING_OPAQUE_RECV_OBJECT)
			io_opaque_restore(op);
		io_opaque_queue_ready(op, ret == 1 ? op->length : ret);
	}
}

/* Only the earliest undecided range is consulted on the capture hot path. */
static struct io_opaque_req *io_opaque_next_decision(struct io_opaque_stream *stream)
{
	struct rb_node *node = rb_first_cached(&stream->decisions);

	return node ? rb_entry(node, struct io_opaque_req, decision) : NULL;
}

/* Undecided bytes are collected only when nothing is pending or a request needs later bytes. */
VISIBLE_IF_KUNIT bool io_opaque_collect_demand(struct io_opaque_stream *stream)
{
	struct io_opaque_req *op = io_opaque_next_decision(stream);

	lockdep_assert_held(&stream->lock);
	return !stream->undecided || !list_empty(&stream->reads) ||
	       (op && op->offset > stream->next);
}

static struct io_opaque_req *io_opaque_capture_keep(struct io_opaque_stream *stream)
{
	struct io_opaque_req *op = io_opaque_next_decision(stream);

	if (op && op->op == IORING_OPAQUE_RECV_OBJECT && stream->next >= op->offset &&
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
		    io_opaque_rx_limit(stream->store))
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

VISIBLE_IF_KUNIT int io_opaque_capture(struct io_opaque_stream *stream,
				       const struct sk_buff *skb, u32 offset, u32 len,
				       bool shared)
{
	u32 consumed = 0, head = skb_headlen(skb), start = head;
	bool copy_frags;
	struct sk_buff *child;
	u64 retain_limit = io_opaque_rx_limit(stream->store);
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

VISIBLE_IF_KUNIT int io_opaque_actor(read_descriptor_t *desc, struct sk_buff *skb,
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

		if (op && stream->next >= op->offset) {
			chunk = min_t(u64, chunk, op->offset + op->length - stream->next);
			if (op->op == IORING_OPAQUE_DISCARD) {
				op->progress += chunk;
				ret = chunk;
				goto consumed;
			}
		} else {
			/*
			 * Undecided bytes stay on the socket until someone asks for them.
			 * A decision further in the stream asks for the gap before it,
			 * including one that became the earliest during this pass.
			 */
			if (stream->hold_undecided && !op)
				break;
			if (op)
				chunk = min_t(u64, chunk, op->offset - stream->next);
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

VISIBLE_IF_KUNIT void io_opaque_stream_close(struct io_opaque_stream *stream)
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

int io_opaque_recv_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_opaque_store *store = xa_load(&req->ctx->opaque_stores,
						READ_ONCE(sqe->opaque_store_id));
	struct io_opaque_req *op;
	u16 flags = READ_ONCE(sqe->ioprio);

	if (!store)
		return -ENXIO;
	if (READ_ONCE(sqe->addr) || READ_ONCE(sqe->off) || READ_ONCE(sqe->addr3) ||
	    READ_ONCE(sqe->len) || READ_ONCE(sqe->msg_flags) ||
	    READ_ONCE(sqe->buf_index) || READ_ONCE(sqe->__pad2[0]) ||
	    (flags & ~IORING_RECVSEND_POLL_FIRST) ||
	    (req->flags & (REQ_F_CQE_SKIP | REQ_F_FORCE_ASYNC)))
		return -EINVAL;
	op = io_opaque_req_alloc(req, store, false);
	if (IS_ERR(op))
		return PTR_ERR(op);
	op->op = U16_MAX - 1;
	op->collect_flags = flags;
	req->flags |= REQ_F_APOLL_MULTISHOT;
	return 0;
}

/*
 * Make @op the collector of its socket. The socket is taken before tables,
 * which is the order a send admits in, with the socket owned. Nothing may wait
 * for a socket lock while it holds tables.
 */
VISIBLE_IF_KUNIT int io_opaque_attach(struct io_opaque_req *op)
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
	lock_sock(sock->sk);
	mutex_lock(&store->tables);
	if (sock->sk->sk_rx_owner || sock->sk->sk_prot != sock->sk->sk_prot_creator ||
	    tcp_sk(sock->sk)->repair || tcp_sk(sock->sk)->urg_data) {
		ret = -EBUSY;
		goto unlock;
	}
	if (sock->sk->sk_state != TCP_ESTABLISHED && sock->sk->sk_state != TCP_CLOSE_WAIT) {
		ret = -ENOTCONN;
		goto unlock;
	}
	for (;;) {
		struct io_opaque_stream_slot *slot;

		i = io_opaque_find_slot(store->stream_used, store->config.max_streams,
					&store->stream_cursor);
		if (i >= store->config.max_streams)
			break;
		slot = &store->streams[i];
		if (slot->generation == IO_OPAQUE_GENERATION_MAX) {
			__set_bit(i, store->stream_used);
			continue;
		}
		write_lock_bh(&sock->sk->sk_callback_lock);
		if (sock->sk->sk_user_data) {
			write_unlock_bh(&sock->sk->sk_callback_lock);
			ret = -EBUSY;
			break;
		}
		slot->generation++;
		stream->token = io_opaque_handle(i, slot->generation);
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
		__set_bit(i, store->stream_used);
		store->nr_streams++;
		refcount_inc(&stream->refs);
		op->stream = stream;
		ret = 0;
		break;
	}
unlock:
	mutex_unlock(&store->tables);
	release_sock(sock->sk);
	if (!ret)
		wake_up_interruptible_poll(sk_sleep(sock->sk), EPOLLIN);
	if (ret)
		io_opaque_stream_put(stream);
	return ret;
}

/* Report a budget stall once per unread offset, without ending collection. */
static int io_opaque_budget_cqe(struct io_opaque_req *op)
{
	struct io_opaque_stream *stream = op->stream;
	struct io_uring_cqe cqe[2] = {};
	u64 extra[2] = { stream->token, stream->next };

	if (op->budget_reported && op->offset == stream->next)
		return 0;
	cqe[0].res = -ENOBUFS;
	cqe[0].flags = IORING_CQE_F_MORE | ctx_cqe32_flags(op->req->ctx);
	memcpy(&cqe[1], extra, sizeof(extra));
	if (!io_req_post_cqe32(op->req, cqe))
		return -ENOSPC;
	op->budget_reported = true;
	op->offset = stream->next;
	return 0;
}

VISIBLE_IF_KUNIT void io_opaque_budget_wait(struct io_opaque_req *op, int error)
{
	lockdep_assert_held(&op->stream->lock);
	scoped_guard(spinlock, &op->store->wait_lock) {
		op->need_bytes = error == -ENOMEM ? 0 : op->stream->need_bytes;
		if (error == -ENOMEM && !time_before(jiffies, op->retry_at))
			op->retry_at = jiffies + msecs_to_jiffies(IO_OPAQUE_ALLOC_RETRY_MS);
		else if (error == -ENOBUFS)
			op->retry_at = jiffies;
		op->phase = IO_OPAQUE_BUDGET_POLL;
		list_add_tail(&op->wait, &op->store->budget_waits);
	}
	/* Recheck after enrollment to close the release/enrollment race. */
	io_opaque_wake_budget(op->store);
}

int io_opaque_recv(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_opaque_req *op = req->async_data;
	struct io_opaque_stream *stream;
	struct socket *sock = sock_from_file(req->file);
	read_descriptor_t desc = { .count = IO_OPAQUE_RX_BATCH };
	struct mem_cgroup *old;
	bool blocked;
	int ret;

	io_ring_submit_lock(req->ctx, issue_flags);
	if (!(req->flags & REQ_F_POLLED) &&
	    (op->collect_flags & IORING_RECVSEND_POLL_FIRST)) {
		ret = -EAGAIN;
		goto out;
	}
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
	mutex_lock(&stream->lock);
	if (stream->closed || READ_ONCE(op->store->dead)) {
		ret = -ECANCELED;
		goto unlock_stream;
	}
	scoped_guard(spinlock, &op->store->wait_lock) {
		blocked = io_opaque_collect_blocked(op);
		if (blocked) {
			/* Capacity may have been taken since the wake removed us. */
			if (list_empty(&op->wait))
				list_add_tail(&op->wait, &op->store->budget_waits);
		} else {
			list_del_init(&op->wait);
			op->phase = IO_OPAQUE_IDLE;
		}
	}
	desc.arg.data = stream;
	old = set_active_memcg(op->store->memcg);
	if (!blocked) {
		/* Retained prefixes precede bytes captured directly into a TAKE. */
		io_opaque_stream_process(stream);
		stream->hold_undecided = !io_opaque_collect_demand(stream);
	}
	lock_sock(sock->sk);
	if (tcp_sk(sock->sk)->urg_data)
		ret = -EOPNOTSUPP;
	else if (blocked) {
		ret = 0;
		if (sock->sk->sk_shutdown & RCV_SHUTDOWN) {
			/* Consume a FIN-only skb, never payload, while out of budget. */
			desc.count = 0;
			ret = tcp_read_sock(sock->sk, &desc, io_opaque_actor);
		}
	} else
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
	if (blocked && !stream->error && !stream->eof) {
		/* Close release/reenrollment races without retrying TCP capture. */
		io_opaque_wake_budget(op->store);
		ret = IOU_RETRY;
		goto unlock_stream;
	}
	if (blocked) {
		guard(spinlock)(&op->store->wait_lock);

		list_del_init(&op->wait);
		op->phase = IO_OPAQUE_IDLE;
	}
	io_opaque_stream_process(stream);
	if (ret == -ENOBUFS) {
		struct io_opaque_req *keep = io_opaque_capture_keep(stream);

		/* A private prefix must not pin quota indefinitely awaiting itself. */
		if (keep && keep->progress) {
			rb_erase_cached(&keep->decision, &stream->decisions);
			RB_CLEAR_NODE(&keep->decision);
			io_opaque_restore(keep);
			io_opaque_queue_ready(keep, -ENOBUFS);
		}
	}
	if (ret == -ENOBUFS && !stream->error && !stream->eof) {
		int err = io_opaque_budget_cqe(op);

		if (err) {
			/* Retain the token and captured prefix for explicit recovery. */
			stream->error = err;
			io_opaque_stream_process(stream);
		}
	}
	if (stream->error || stream->eof) {
		ret = stream->error;
	} else if (ret == -ENOBUFS || ret == -ENOMEM) {
		io_opaque_budget_wait(op, ret);
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

VISIBLE_IF_KUNIT int io_opaque_publish(struct io_opaque_req *op, u64 *object_flags)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_slot *slot = &store->objects[op->slot];

	guard(mutex)(&store->tables);
	if (store->dead || READ_ONCE(op->canceled))
		return -ECANCELED;
	if (!slot->reserved ||
	    slot->generation != op->handle >> IO_OPAQUE_INDEX_BITS)
		return -ESTALE;
	slot->data = op->data;
	if (op->data->forward_only)
		*object_flags = IORING_OPAQUE_OBJECT_F_FORWARD_ONLY;
	store->nr_objects++;
	op->data = NULL;
	slot->reserved = false;
	op->slot_reserved = false;
	slot->born = jiffies;
	slot->retry = jiffies;
	if (!slot->data->dense && !slot->data->forward_only &&
	    (store->policy.flags & IORING_OPAQUE_POLICY_F_AUTO_COMPACT)) {
		list_add_tail(&slot->candidate, &store->candidates);
		queue_delayed_work(system_dfl_wq, &store->compact_work,
				   msecs_to_jiffies(100));
	}
	return op->length;
}

static void io_opaque_ready(struct io_tw_req tw_req, io_tw_token_t tw)
{
	struct io_kiocb *req = tw_req.req;
	struct io_opaque_req *op = req->async_data;
	struct io_opaque_store *store = op->store;
	int ret = op->result;
	u64 object_flags = 0;

	io_tw_lock(req->ctx, tw);
	list_del_init(&op->cancel);
	op->phase = IO_OPAQUE_IDLE;
	if (tw.cancel || READ_ONCE(op->canceled) || READ_ONCE(store->dead))
		ret = -ECANCELED;
	if (ret >= 0 && op->op == IORING_OPAQUE_INSPECT)
		ret = io_opaque_copy(op->data, 0, op->data->length, op->addr);
	if (ret >= 0 && op->op == IORING_OPAQUE_RECV_OBJECT) {
		ret = op->data->length == op->length ?
			io_opaque_vector(op->data) : -ENODATA;
		if (!ret)
			ret = io_opaque_publish(op, &object_flags);
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
	if (ret < 0 && op->op == IORING_OPAQUE_RECV_OBJECT) {
		mutex_lock(&op->stream->lock);
		io_opaque_restore(op);
		io_opaque_stream_process(op->stream);
		mutex_unlock(&op->stream->lock);
	}
	if (ret < 0)
		req_set_fail(req);
	if (ret < 0 && req->opcode == IORING_OP_OPAQUE_OBJ_SEND && op->progress)
		ret = op->progress;
	if (ret >= 0 && op->op == IORING_OPAQUE_RECV_OBJECT)
		io_req_set_res32(req, ret, 0, op->handle, object_flags);
	else
		io_req_set_res(req, ret, op->consumed ? IORING_CQE_F_OPAQUE_CONSUMED : 0);
	io_req_task_complete(tw_req, tw);
}

static struct io_opaque_store *io_opaque_get_store(struct io_kiocb *req, u32 id)
{
	return xa_load(&req->ctx->opaque_stores, id);
}

static bool io_opaque_stream_op(u16 op)
{
	return op == IORING_OPAQUE_INSPECT || op == IORING_OPAQUE_AVAILABLE ||
	       op == IORING_OPAQUE_RECV_OBJECT ||
	       op == IORING_OPAQUE_DISCARD || op == IORING_OPAQUE_STREAM_STAT ||
	       op == IORING_OPAQUE_STREAM_CLOSE;
}

int io_opaque_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_opaque_store *store = io_opaque_get_store(req, READ_ONCE(sqe->opaque_store_id));
	struct io_opaque_req *op;
	u64 end;
	u16 cmd = READ_ONCE(sqe->ioprio);
	bool control;

	if (!store)
		return -ENXIO;
	if (READ_ONCE(sqe->fd) != -1 || READ_ONCE(sqe->rw_flags) ||
	    READ_ONCE(sqe->buf_index) || READ_ONCE(sqe->__pad2[0]) ||
	    cmd > IORING_OPAQUE_AVAILABLE ||
	    (req->flags & (REQ_F_CQE_SKIP | REQ_F_FIXED_FILE)))
		return -EINVAL;
	control = cmd == IORING_OPAQUE_FREE || cmd == IORING_OPAQUE_STAT ||
		  cmd == IORING_OPAQUE_STREAM_CLOSE || cmd == IORING_OPAQUE_SET_POLICY;
	op = io_opaque_req_alloc(req, store, control);
	if (IS_ERR(op))
		return PTR_ERR(op);
	op->op = cmd;
	if (cmd <= IORING_OPAQUE_DISCARD || cmd == IORING_OPAQUE_AVAILABLE) {
		INIT_LIST_HEAD(&op->read);
		RB_CLEAR_NODE(&op->decision);
	}
	op->handle = READ_ONCE(sqe->addr);
	op->offset = READ_ONCE(sqe->off);
	op->length = READ_ONCE(sqe->len);
	op->addr = u64_to_user_ptr(READ_ONCE(sqe->addr3));
	if (cmd == IORING_OPAQUE_CREATE) {
		u64 pointer = READ_ONCE(sqe->addr3);

		if (op->handle || op->offset || !op->length ||
		    op->length > INT_MAX || !pointer)
			return -EINVAL;
		if (op->length > store->config.max_object_size ||
		    ALIGN((u64)op->length, PAGE_SIZE) > io_opaque_rx_limit(store))
			return -EMSGSIZE;
		if (check_add_overflow(pointer, (u64)op->length, &end) ||
		    pointer > ULONG_MAX || end > ULONG_MAX ||
		    !access_ok(op->addr, op->length))
			return -EFAULT;
		return 0;
	}
	if (check_add_overflow(op->offset, (u64)op->length, &end))
		return -EOVERFLOW;
	if (cmd <= IORING_OPAQUE_READ_OBJECT || cmd == IORING_OPAQUE_AVAILABLE) {
		if (!op->length || op->length > INT_MAX)
			return -EINVAL;
		if ((cmd == IORING_OPAQUE_INSPECT || cmd == IORING_OPAQUE_READ_OBJECT) &&
		    (op->length > IO_OPAQUE_READ_MAX || !op->addr))
			return -EINVAL;
		if ((cmd == IORING_OPAQUE_RECV_OBJECT || cmd == IORING_OPAQUE_DISCARD ||
		     cmd == IORING_OPAQUE_AVAILABLE) && op->addr)
			return -EINVAL;
		if (cmd == IORING_OPAQUE_RECV_OBJECT &&
		    (op->length > store->config.max_object_size ||
		     op->length > io_opaque_rx_limit(store)))
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
		u32 index = io_opaque_index(op->handle);

		if (!op->handle || index >= store->config.max_streams ||
		    store->streams[index].generation != op->handle >> IO_OPAQUE_INDEX_BITS ||
		    !store->streams[index].stream) {
			ret = -ESTALE;
		} else {
			op->stream = store->streams[index].stream;
			refcount_inc(&op->stream->refs);
			op->req->file = get_file(op->stream->file);
		}
	} else if (cmd == IORING_OPAQUE_READ_OBJECT || cmd == IORING_OPAQUE_OBJECT_STAT) {
		slot = io_opaque_lookup(store, op->handle);
		if (!slot || !slot->data) {
			ret = -ESTALE;
		} else {
			op->data = slot->data;
			refcount_inc(&op->data->refs);
			if (cmd == IORING_OPAQUE_READ_OBJECT &&
			    (op->offset > op->data->length ||
			     op->length > op->data->length - op->offset))
				ret = -ERANGE;
		}
	}
	mutex_unlock(&store->tables);
	if (ret)
		return ret;
	if (cmd == IORING_OPAQUE_INSPECT || cmd == IORING_OPAQUE_RECV_OBJECT) {
		op->data = io_opaque_data_alloc();
		if (!op->data)
			return -ENOMEM;
	}
	if (cmd == IORING_OPAQUE_RECV_OBJECT) {
		ret = io_opaque_slot_reserve(op);
		if (ret)
			return ret;
	}
	op->resolved = true;
	return 0;
}

VISIBLE_IF_KUNIT int io_opaque_range_start(struct io_opaque_req *op)
{
	struct io_opaque_stream *stream = op->stream;
	struct rb_node *node;
	int ret = 0;

	guard(mutex)(&stream->lock);
	if (stream->closed)
		return -ECANCELED;
	if (op->op == IORING_OPAQUE_INSPECT || op->op == IORING_OPAQUE_AVAILABLE) {
		for (node = rb_first_cached(&stream->decisions); node; node = rb_next(node)) {
			struct io_opaque_req *other;

			other = rb_entry(node, struct io_opaque_req, decision);

			if (io_opaque_overlap(op->offset, 1, other->offset, other->length))
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
	/* A decision, or an INSPECT still waiting for bytes, may let collection continue. */
	if ((op->op != IORING_OPAQUE_INSPECT && op->op != IORING_OPAQUE_AVAILABLE) ||
	    !list_empty(&op->read))
		io_opaque_wake_stream(stream);
	return IOU_ISSUE_SKIP_COMPLETE;
}

static int io_opaque_stat_copy(struct io_opaque_req *op, const void *stat, u32 size)
{
	if (!op->addr || op->length < sizeof(u32) || op->length > PAGE_SIZE)
		return -EINVAL;
	return copy_struct_to_user(op->addr, op->length, stat, size, NULL);
}

static int io_opaque_stat(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_uring_opaque_stat stat = {
		.size = sizeof(stat),
		.backing_bytes = atomic64_read(&store->bytes),
		.hard_limit = store->config.hard_limit,
		.copied_bytes = atomic64_read(&store->copied),
		.compacted_bytes = atomic64_read(&store->compacted),
		.compact_count = atomic64_read(&store->compactions),
		.extents = atomic_read(&store->extents),
		.requests = atomic_read(&store->requests),
		.framing_bytes = atomic64_read(&store->framing),
		.framing_copied_bytes = atomic64_read(&store->framing_copied),
		.memory_mode = io_opaque_memory_mode(store),
	};

	/* Framing in the reply reserve is charged outside bytes. */
	stat.backing_bytes += atomic64_read(&store->reserve_framing);
	mutex_lock(&store->tables);
	stat.objects = store->nr_objects;
	stat.streams = store->nr_streams;
	mutex_unlock(&store->tables);
	return io_opaque_stat_copy(op, &stat, sizeof(stat));
}

/* Construction is synchronous under the ring lock, like READ_OBJECT copies. */
VISIBLE_IF_KUNIT int io_opaque_create_build(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct mem_cgroup *old = set_active_memcg(store->memcg);
	u64 done = 0;
	int ret = 0;

	while (done < op->length) {
		u64 pages_left = DIV_ROUND_UP_ULL(op->length - done, PAGE_SIZE);
		struct io_opaque_extent *extent;
		unsigned int order;
		struct page *page;
		u32 chunk, written = 0;

		page = io_opaque_dest_page(store, pages_left, &order);
		if (!page) {
			ret = -ENOMEM;
			goto out;
		}
		chunk = min_t(u64, op->length - done, PAGE_SIZE << order);
		extent = io_opaque_extent_new(store, page, 0, chunk, done, true);
		if (IS_ERR(extent)) {
			io_opaque_unit_put(store, page);
			ret = PTR_ERR(extent);
			goto out;
		}
		op->create_reserved -= page_size(page);
		extent->backing->copied = true;
		extent->backing->owned = true;
		list_add_tail(&extent->list, &op->data->extents);
		while (written < chunk) {
			u32 bytes = min_t(u32, chunk - written, PAGE_SIZE);
			unsigned long left;
			void *mapped;

			mapped = kmap_local_page(page + (written >> PAGE_SHIFT));
			left = copy_from_user(mapped, op->addr + done + written, bytes);
			kunmap_local(mapped);
			atomic64_add(bytes - left, &store->copied);
			if (left) {
				ret = -EFAULT;
				goto out;
			}
			written += bytes;
		}
		done += chunk;
		op->data->length = done;
		cond_resched();
	}
	ret = io_opaque_vector(op->data);
	if (!ret)
		op->data->dense = true;
out:
	set_active_memcg(old);
	return ret;
}

static int io_opaque_create(struct io_opaque_req *op, unsigned int issue_flags)
{
	struct io_ring_ctx *ctx = op->req->ctx;
	u64 charge = ALIGN((u64)op->length, PAGE_SIZE);
	u64 object_flags = 0;
	struct mem_cgroup *old;
	int ret;

	io_ring_submit_lock(ctx, issue_flags);
	if (READ_ONCE(op->store->dead)) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	old = set_active_memcg(op->store->memcg);
	op->data = io_opaque_data_alloc();
	set_active_memcg(old);
	if (!op->data) {
		ret = -ENOMEM;
		goto unlock;
	}
	ret = io_opaque_slot_reserve(op);
	if (ret)
		goto unlock;
	if (!io_opaque_charge(op->store, charge, true)) {
		ret = -ENOBUFS;
		goto unlock;
	}
	op->create_reserved = charge;
	ret = io_opaque_create_build(op);
	if (!ret)
		ret = io_opaque_publish(op, &object_flags);
unlock:
	io_req_set_res32(op->req, ret, 0, ret >= 0 ? op->handle : 0, object_flags);
	if (ret < 0)
		req_set_fail(op->req);
	io_ring_submit_unlock(ctx, issue_flags);
	return IOU_COMPLETE;
}

int io_opaque_issue(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_opaque_req *op = req->async_data;
	struct io_opaque_store *store = op->store;
	int ret;

	if (op->op == IORING_OPAQUE_CREATE)
		return io_opaque_create(op, issue_flags);
	io_ring_submit_lock(req->ctx, issue_flags);
	if (READ_ONCE(store->dead)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	ret = io_opaque_resolve(op);
	if (ret)
		goto out;
	switch (op->op) {
	case IORING_OPAQUE_INSPECT:
	case IORING_OPAQUE_AVAILABLE:
	case IORING_OPAQUE_RECV_OBJECT:
	case IORING_OPAQUE_DISCARD:
		ret = io_opaque_range_start(op);
		break;
	case IORING_OPAQUE_READ_OBJECT:
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
				.size = sizeof(stat),
				.length = op->data->length,
				.backing_bytes = op->data->charge,
				.extents = op->data->nr,
				.flags = op->data->dense ? IORING_OPAQUE_OBJECT_F_COMPACTED : 0,
			};
			struct io_opaque_slot *slot;

			if (op->data->forward_only)
				stat.flags |= IORING_OPAQUE_OBJECT_F_FORWARD_ONLY;
			mutex_lock(&store->tables);
			slot = io_opaque_lookup(store, op->handle);
			if (slot && slot->compacting)
				stat.flags |= IORING_OPAQUE_OBJECT_F_COMPACTING;
			mutex_unlock(&store->tables);
			ret = io_opaque_stat_copy(op, &stat, sizeof(stat));
		}
		break;
	case IORING_OPAQUE_STREAM_STAT:
		{
			struct io_uring_opaque_stream_stat stat = { .size = sizeof(stat) };

			mutex_lock(&op->stream->lock);
			stat.rx_next = op->stream->next;
			stat.undecided_bytes = op->stream->undecided;
			stat.flags = op->stream->eof ? IORING_OPAQUE_STREAM_F_EOF : 0;
			stat.error = op->stream->error;
			mutex_unlock(&op->stream->lock);
			ret = io_opaque_stat_copy(op, &stat, sizeof(stat));
		}
		break;
	case IORING_OPAQUE_STREAM_CLOSE:
		mutex_lock(&store->tables);
		if (store->streams[io_opaque_index(op->handle)].stream == op->stream) {
			store->streams[io_opaque_index(op->handle)].stream = NULL;
			store->nr_streams--;
			io_opaque_release_slot(store->stream_used, io_opaque_index(op->handle),
					       op->handle >> IO_OPAQUE_INDEX_BITS);
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
	bool queue;

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
	/* Claim task work before a remote TX head can promote this waiter. */
	queue = phase != IO_OPAQUE_QUEUED && phase != IO_OPAQUE_COPY_WORK;
	if (queue)
		op->phase = IO_OPAQUE_QUEUED;
	spin_unlock(&op->store->wait_lock);
	if (phase == IO_OPAQUE_RANGE_WAIT) {
		list_del_init(&op->read);
		if (!RB_EMPTY_NODE(&op->decision)) {
			rb_erase_cached(&op->decision, &op->stream->decisions);
			RB_CLEAR_NODE(&op->decision);
		}
	}
	if (op->stream) {
		if (op->op == IORING_OPAQUE_RECV_OBJECT)
			io_opaque_restore(op);
		mutex_unlock(&op->stream->lock);
	}
	if (queue)
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

VISIBLE_IF_KUNIT void io_opaque_create_release(struct io_opaque_req *op)
{
	io_opaque_data_put(op->store, op->data);
	op->data = NULL;
	if (op->create_reserved) {
		io_opaque_uncharge(op->store, op->create_reserved);
		op->create_reserved = 0;
	}
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
	/*
	 * Taking wait_lock is how cleanup waits out a waker that is still in
	 * its hold: budget_waits and copies are walked under it, using the
	 * request's file and stream after its entry has been unlinked. A send
	 * is on neither list. What reaches one from outside comes through its
	 * TX queue, which io_opaque_tx_put() below waits for, or from
	 * cancellation, which holds the ring lock as cleanup does. It has no
	 * waker to wait out, and takes the lock only if its entry is linked.
	 */
	if (req->opcode != IORING_OP_OPAQUE_OBJ_SEND || !list_empty(&op->wait)) {
		scoped_guard(spinlock, &store->wait_lock)
			list_del_init(&op->wait);
	}
	/* Waiters have stopped accessing this request before cleanup. */
	if (op->stream) {
		mutex_lock(&op->stream->lock);
		if (op->op <= IORING_OPAQUE_DISCARD || op->op == IORING_OPAQUE_AVAILABLE) {
			list_del_init(&op->read);
			if (!RB_EMPTY_NODE(&op->decision)) {
				rb_erase_cached(&op->decision, &op->stream->decisions);
				RB_CLEAR_NODE(&op->decision);
			}
			if (op->op == IORING_OPAQUE_RECV_OBJECT)
				io_opaque_restore(op);
		}
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
		io_opaque_release_slot(store->object_used, op->slot,
				       store->objects[op->slot].generation);
		mutex_unlock(&store->tables);
	}
	if (req->opcode == IORING_OP_OPAQUE_OBJ_SEND) {
		io_opaque_tx_put(op);
		kfree(op->framing);
		if (op->frame_charge)
			io_opaque_uncharge_frame(store, op->frame_charge, op->frame_reserved);
	}
	if (unlikely(op->vec))
		io_opaque_vec_cleanup(op);
	if (op->op == IORING_OPAQUE_CREATE)
		io_opaque_create_release(op);
	else
		io_opaque_data_put(store, op->data);
	if (op->op == IORING_OPAQUE_COMPACT) {
		io_opaque_data_put(store, op->replacement);
		io_opaque_compact_end(op);
	}
	io_opaque_stream_put(op->stream);
	if (op->counted)
		atomic_dec(&store->requests);
	lockdep_assert_held(&req->ctx->uring_lock);
	io_opaque_req_free(req->ctx, op);
	io_req_async_data_clear(req, 0);
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

/*
 * Snapshot framing before TX admission can consume a LAST handle. Only a send
 * with a frame gets here: one without is resolved at prep.
 */
static int io_opaque_frame_import(struct io_opaque_req *op)
{
	struct io_uring_opaque_frame frame = {};
	u32 size, bytes, charge;
	int ret;

	op->total_length = op->length;
	if (op->addr) {
		if (get_user(size, (u32 __user *)op->addr))
			return -EFAULT;
		if (size < offsetof(struct io_uring_opaque_frame, __resv) ||
		    size > PAGE_SIZE)
			return -EINVAL;
		ret = copy_struct_from_user(&frame, sizeof(frame), op->addr, size);
		if (ret)
			return ret;
		if (frame.size != size || frame.flags ||
		    !mem_is_zero(frame.__resv, sizeof(frame.__resv)) ||
		    (!frame.prefix_length && frame.prefix) ||
		    (!frame.suffix_length && frame.suffix) ||
		    check_add_overflow(frame.prefix_length, frame.suffix_length, &bytes) ||
		    bytes > IORING_OPAQUE_FRAME_MAX ||
		    check_add_overflow(op->length, bytes, &op->total_length) ||
		    op->total_length > INT_MAX)
			return -EINVAL;
		if (bytes) {
			struct mem_cgroup *old;

			charge = kmalloc_size_roundup(bytes);
			if (!io_opaque_charge_frame(op->store, charge, &op->frame_reserved))
				return -ENOBUFS;
			op->frame_charge = charge;
			old = set_active_memcg(op->store->memcg);
			op->framing = kmalloc(charge, GFP_KERNEL_ACCOUNT);
			set_active_memcg(old);
			if (!op->framing)
				return -ENOMEM;
			if ((frame.prefix_length &&
			     copy_from_user(op->framing, u64_to_user_ptr(frame.prefix),
					    frame.prefix_length)) ||
			    (frame.suffix_length &&
			     copy_from_user(op->framing + frame.prefix_length,
					    u64_to_user_ptr(frame.suffix), frame.suffix_length)))
				return -EFAULT;
			atomic64_add(bytes, &op->store->framing_copied);
			io_opaque_count(IO_OPAQUE_STAT_FRAMING_COPIED, bytes);
		}
		op->prefix_length = frame.prefix_length;
		op->suffix_length = frame.suffix_length;
	}
	if (!op->total_length ||
	    (!op->length && (op->handle || op->offset ||
			    (op->send_flags & IORING_OPAQUE_SEND_LAST))))
		return -EINVAL;
	op->resolved = true;
	return 0;
}

/*
 * A send without a frame reads nothing from user memory at first issue: its
 * total is its body, which prep already checked to be positive.
 */
VISIBLE_IF_KUNIT void io_opaque_send_resolve(struct io_opaque_req *op)
{
	if (op->addr)
		return;
	op->total_length = op->length;
	op->resolved = true;
}

#if IS_ENABLED(CONFIG_KUNIT)
/* A variable under KUnit, so that a test reaches the limit with a few segments. */
unsigned int io_opaque_send_vec_segs = IO_OPAQUE_SEND_VEC_SEGS;
#define IO_OPAQUE_VEC_SEGS_LIMIT	io_opaque_send_vec_segs
/* Segments that admission walked over to find where ranges start, for a test to read. */
atomic_long_t io_opaque_vec_stepped = ATOMIC_LONG_INIT(0);
#define IO_OPAQUE_VEC_STEPPED(n)	atomic_long_add((n), &io_opaque_vec_stepped)
/* Segments that admission counted in the ranges, for a test to read. */
atomic_long_t io_opaque_vec_counted = ATOMIC_LONG_INIT(0);
#define IO_OPAQUE_VEC_COUNTED(n)	atomic_long_add((n), &io_opaque_vec_counted)
#else
#define IO_OPAQUE_VEC_SEGS_LIMIT	IO_OPAQUE_SEND_VEC_SEGS
#define IO_OPAQUE_VEC_STEPPED(n)	((void)(n))
#define IO_OPAQUE_VEC_COUNTED(n)	((void)(n))
#endif

/*
 * Check @nr entries copied from user memory; return the body they add up to
 * and the largest of them. A handle may repeat, and a range may too.
 */
VISIBLE_IF_KUNIT int io_opaque_vec_check(const struct io_uring_opaque_send_entry *ent, u32 nr,
					 u32 *total, u32 *max_len)
{
	u64 sum = 0, end;
	u32 longest = 0, i;

	BUILD_BUG_ON(sizeof(*ent) != 24);
	if (!nr || nr > IORING_OPAQUE_SEND_VEC_MAX)
		return -EINVAL;
	for (i = 0; i < nr; i++) {
		if (ent[i].__resv || !ent[i].len ||
		    check_add_overflow(ent[i].off, (u64)ent[i].len, &end))
			return -EINVAL;
		sum += ent[i].len;
		if (sum > INT_MAX)
			return -EINVAL;
		longest = max(longest, ent[i].len);
	}
	*total = sum;
	*max_len = longest;
	return 0;
}

/*
 * Make room in the request's block for @nr entries and @segs segments, keeping
 * its first @keep entries. A block that is big enough stays as it is. This
 * allocates, so tables must not be held, and the send must hold no reference
 * yet: what admission recorded about the versions is not carried over.
 */
VISIBLE_IF_KUNIT int io_opaque_vec_grow(struct io_opaque_req *op, u32 nr, u32 segs, u32 keep)
{
	struct io_opaque_vec *old = op->vec, *vec;

	KUNIT_STATIC_STUB_REDIRECT(io_opaque_vec_grow, op, nr, segs, keep);
	lockdep_assert_not_held(&op->store->tables);
	if (old) {
		WARN_ON_ONCE(old->pinned);
		if (old->cap_nr >= nr && old->cap_segs >= segs)
			return 0;
		nr = max(nr, old->cap_nr);
		segs = max(segs, old->cap_segs);
	}
	vec = kvmalloc(size_add(sizeof(*vec),
				size_add(array_size(nr, sizeof(*vec->ent) + sizeof(*vec->pin) +
						       sizeof(*vec->start)),
					 array_size(segs, sizeof(*vec->bv)))),
		       GFP_KERNEL_ACCOUNT);
	if (!vec)
		return -ENOMEM;
	vec->cap_nr = nr;
	vec->cap_segs = segs;
	vec->ent = (void *)(vec + 1);
	vec->pin = (void *)(vec->ent + nr);
	vec->start = (void *)(vec->pin + nr);
	vec->bv = (void *)(vec->start + nr);
	vec->nr_segs = 0;
	vec->pinned = 0;
	if (old) {
		vec->nr = old->nr;
		vec->max_len = old->max_len;
		memcpy(vec->ent, old->ent, array_size(keep, sizeof(*vec->ent)));
		kvfree(old);
	} else {
		vec->nr = 0;
		vec->max_len = 0;
	}
	op->vec = vec;
	return 0;
}

/*
 * The @nr entries are in the block: validate them and make the request what
 * its length and offset say about a send, a body of their total at no offset.
 */
VISIBLE_IF_KUNIT int io_opaque_vec_commit(struct io_opaque_req *op, u32 nr)
{
	struct io_opaque_vec *vec = op->vec;
	u32 total, longest;
	int ret;

	ret = io_opaque_vec_check(vec->ent, nr, &total, &longest);
	if (ret)
		return ret;
	vec->nr = nr;
	vec->nr_segs = 0;
	vec->pinned = 0;
	vec->max_len = longest;
	op->handle = 0;
	op->offset = 0;
	op->length = total;
	return 0;
}

/*
 * Whether entry @i of an admitted vector holds a reference of its own. A LAST
 * vector takes one per entry. Any other takes one per run of entries that name
 * the same handle: within one hold of tables the same handle is the same
 * version, so the rest of the run shares the first entry's pin. This looks at
 * the entries, which never change, and not at the pins, which may be freed by
 * the time the run's owner has been released.
 */
static bool io_opaque_vec_owns(const struct io_opaque_req *op, u32 i)
{
	const struct io_opaque_vec *vec = op->vec;

	return (op->send_flags & IORING_OPAQUE_SEND_LAST) || !i ||
	       vec->ent[i].handle != vec->ent[i - 1].handle;
}

/* Drop every version reference the send holds; one that took none drops none. */
VISIBLE_IF_KUNIT void io_opaque_vec_release(struct io_opaque_req *op)
{
	struct io_opaque_vec *vec = op->vec;
	u32 i;

	for (i = 0; i < vec->pinned; i++)
		if (io_opaque_vec_owns(op, i))
			io_opaque_data_put(op->store, vec->pin[i]);
	vec->pinned = 0;
	vec->nr_segs = 0;
}

/*
 * Request cleanup: release what a vectored send pinned and let go of the block
 * unless the request takes it back to the cache. A big block is not worth
 * keeping, and under KASAN an entry in the cache is cleared when it is taken,
 * which would leak it.
 */
static void io_opaque_vec_cleanup(struct io_opaque_req *op)
{
	struct io_opaque_vec *vec = op->vec;

	io_opaque_vec_release(op);
	if (IS_ENABLED(CONFIG_KASAN) || vec->cap_nr > IO_VEC_CACHE_SOFT_CAP ||
	    vec->cap_segs > IO_VEC_CACHE_SOFT_CAP) {
		kvfree(vec);
		op->vec = NULL;
	}
}

/*
 * Find the segment of @data that holds byte @off, and return how far into it
 * @off is. @seg is a segment of the object that starts @pos bytes in and not
 * after the one wanted: the walk goes on from there, and leaves @seg and @pos
 * at the start of the segment it found.
 *
 * A segment within IO_OPAQUE_SEEK_STRIDE steps is walked to, which is all that
 * a range after the previous one in the same object ever takes. A farther one
 * is reached by jumping to the last anchor that is at or before @off, if that
 * is ahead of where the walk is, and walking on from it: under a stride more.
 * No seek takes more than two strides and a binary search over the anchors,
 * wherever it starts, however large the object is, and so a vector of any
 * ranges keeps tables for a time that its entries bound.
 */
static u32 io_opaque_vec_seek(const struct io_opaque_data *data, u64 off, u32 *seg, u64 *pos)
{
	const struct bio_vec *src = data->bvec;
	u32 i = *seg, steps = 0;
	u64 at = *pos;

	while (steps < IO_OPAQUE_SEEK_STRIDE && i < data->nr && at + src[i].bv_len <= off) {
		at += src[i++].bv_len;
		steps++;
	}
	if (data->anchor && i < data->nr && at + src[i].bv_len <= off) {
		u32 lo = 0, hi = DIV_ROUND_UP(data->nr, IO_OPAQUE_SEEK_STRIDE) - 1;

		/* The last anchor that starts at or before @off; the first always does. */
		while (lo < hi) {
			u32 mid = lo + (hi - lo + 1) / 2;

			if (data->anchor[mid] <= off)
				lo = mid;
			else
				hi = mid - 1;
		}
		if (data->anchor[lo] > at) {
			i = lo * IO_OPAQUE_SEEK_STRIDE;
			at = data->anchor[lo];
		}
	}
	while (i < data->nr && at + src[i].bv_len <= off) {
		at += src[i++].bv_len;
		steps++;
	}
	IO_OPAQUE_VEC_STEPPED(steps);
	*seg = i;
	*pos = at;
	return off - at;
}

/*
 * The segments of the range of @len bytes that starts @skip bytes into segment
 * @seg of @data, as a cursor over the whole object would visit them: the first
 * starts inside its extent, the last ends inside its own. They are stored at
 * @bv, and all of them counted, unless it is NULL: then only the count is
 * wanted, and it stops at @room + 1, since a caller that has room for no more
 * has no use for how many more there are. The range must lie within the
 * object.
 */
static u32 io_opaque_vec_span(const struct io_opaque_data *data, u32 seg, u32 skip, u32 len,
			      struct bio_vec *bv, u64 room)
{
	const struct bio_vec *src = data->bvec;
	u32 n = 0;

	while (len && seg < data->nr) {
		u32 take = min(src[seg].bv_len - skip, len);

		if (bv) {
			bv[n] = src[seg];
			bv[n].bv_offset += skip;
			bv[n].bv_len = take;
		}
		n++;
		if (!bv && n > room)
			break;
		len -= take;
		skip = 0;
		seg++;
	}
	return n;
}

/* The handle must hold a version, and the range must lie within it. */
static int io_opaque_send_range(const struct io_opaque_slot *slot, u64 off, u32 len)
{
	if (!slot || !slot->data)
		return -ESTALE;
	if (off > slot->data->length || len > slot->data->length - off)
		return -ERANGE;
	return 0;
}

/*
 * Take a send's reference on the version @slot holds: the table reference when
 * the send is LAST, which also removes the handle, else an extra one. Under
 * tables, after io_opaque_send_range().
 */
static struct io_opaque_data *io_opaque_send_pin(struct io_opaque_store *store,
						 struct io_opaque_slot *slot, bool last)
{
	if (last)
		return io_opaque_detach(store, slot);
	refcount_inc(&slot->data->refs);
	return slot->data;
}

/* Drop the references a send took at admission; a no-op when it took none. */
static void io_opaque_send_unpin(struct io_opaque_req *op)
{
	if (op->send_flags & IORING_OPAQUE_SEND_VECTOR)
		io_opaque_vec_release(op);
	io_opaque_data_put(op->store, op->data);
	op->data = NULL;
}

/*
 * The part of io_opaque_send_unpin() that a socket lock can wait for: drop
 * every reference of the send that is not the last on its version, which is a
 * decrement and cannot free anything, and say whether any is left. What is
 * left is for io_opaque_send_unpin() once the socket is released, since the
 * last reference frees the version, every extent of it, and that is not for
 * the lock to wait out. Nobody can take a reference on a version whose count
 * is one, which is not in the table, so the last one is still the last then.
 * A vector's entries that were put are cleared, so that the rest cannot put
 * them again.
 */
static bool io_opaque_send_unpin_cheap(struct io_opaque_req *op)
{
	bool left = false;

	if (op->send_flags & IORING_OPAQUE_SEND_VECTOR) {
		struct io_opaque_vec *vec = op->vec;
		u32 i;

		for (i = 0; i < vec->pinned; i++) {
			if (!vec->pin[i] || !io_opaque_vec_owns(op, i))
				continue;
			if (refcount_dec_not_one(&vec->pin[i]->refs))
				vec->pin[i] = NULL;
			else
				left = true;
		}
		if (!left) {
			vec->pinned = 0;
			vec->nr_segs = 0;
		}
	}
	if (op->data) {
		if (refcount_dec_not_one(&op->data->refs))
			op->data = NULL;
		else
			left = true;
	}
	return left;
}

/*
 * Point the send's cursor at the range it selects: @len bytes in @nr segments
 * from @bv, of which the range starts @skip bytes in and lasts for op->length.
 */
static void io_opaque_send_iter(struct io_opaque_req *op, const struct bio_vec *bv,
				unsigned int nr, size_t len, size_t skip)
{
	iov_iter_bvec(&op->iter, ITER_SOURCE, bv, nr, len);
	iov_iter_advance(&op->iter, skip);
	iov_iter_truncate(&op->iter, op->length);
}

/*
 * Admission of a vectored send, under tables; every entry sees the versions
 * as they are in this one hold, and nothing in here sleeps. Pass one resolves
 * and counts, and fails without having pinned or detached anything, so a
 * vector that cannot go consumes nothing. It finds where each range starts by
 * walking the segments of its version, and an entry that follows another in
 * the same version, not before the segment that one started in, walks on from
 * there instead of from the first: ranges in ascending order walk each object
 * once. One that is more than a stride from where the walk is jumps to the
 * version's anchors (see io_opaque_vec_seek()), so that ranges that go back
 * or alternate between large objects cost a few dozen steps each, not an
 * object. Counting the segments of the ranges stops as soon as the limit is
 * passed. What tables is held for is therefore bounded by the entries and the
 * limit, whatever the ranges. A block with too few segments is not grown here,
 * as that allocates; the number needed is returned, and io_opaque_vec_finish()
 * grows the block and asks again. Pass two takes the references and looks
 * nothing up: pass one recorded every version in pin[], and tables has been
 * held since. A LAST vector detaches each handle at its first entry, and an
 * entry that repeats the handle takes one more reference on the version it
 * found. Any other vector takes one reference for each run of entries that
 * name the same handle (see io_opaque_vec_owns()), so eight ranges of one
 * object cost one reference, and a handle that returns after another one pins
 * again. Nothing can fail from there on. The cursor over the combined segments
 * is io_opaque_vec_build()'s, once tables is dropped.
 */
static int io_opaque_vec_acquire(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	bool last = op->send_flags & IORING_OPAQUE_SEND_LAST;
	struct io_opaque_vec *vec = op->vec;
	const struct io_opaque_data *prev = NULL;
	u64 segs = 0, pos = 0;
	u32 i, seg = 0;
	int ret;

	if (store->dead)
		return -ESHUTDOWN;
	for (i = 0; i < vec->nr; i++) {
		const struct io_uring_opaque_send_entry *ent = &vec->ent[i];
		struct io_opaque_slot *slot = io_opaque_lookup(store, ent->handle);
		struct io_opaque_data *data;
		u32 skip, n;

		ret = io_opaque_send_range(slot, ent->off, ent->len);
		if (ret)
			return ret;
		data = slot->data;
		if (data != prev || ent->off < pos) {
			prev = data;
			seg = 0;
			pos = 0;
		}
		skip = io_opaque_vec_seek(data, ent->off, &seg, &pos);
		vec->start[i].seg = seg;
		vec->start[i].skip = skip;
		n = io_opaque_vec_span(data, seg, skip, ent->len, NULL,
				       IO_OPAQUE_VEC_SEGS_LIMIT - segs);
		IO_OPAQUE_VEC_COUNTED(n);
		segs += n;
		if (segs > IO_OPAQUE_VEC_SEGS_LIMIT)
			return -EMSGSIZE;
		/* Not yet a reference: pass two takes it, and a repeated handle needs it. */
		vec->pin[i] = data;
	}
	/* Within the limit, so it fits an int. */
	if (segs > vec->cap_segs)
		return segs;
	for (i = 0; i < vec->nr; i++) {
		struct io_opaque_slot *slot;

		if (!last) {
			if (io_opaque_vec_owns(op, i))
				refcount_inc(&vec->pin[i]->refs);
			continue;
		}
		slot = io_opaque_lookup(store, vec->ent[i].handle);
		if (!slot->data)
			refcount_inc(&vec->pin[i]->refs);
		else
			vec->pin[i] = io_opaque_send_pin(store, slot, true);
	}
	vec->pinned = vec->nr;
	if (last)
		op->consumed = true;
	return 0;
}

/*
 * The cursor over the combined segments, from the starts that admission
 * recorded. It takes no lock and is done without tables, which is the point of
 * it: each version is held by the send's own reference, and the segments of a
 * version never change.
 */
VISIBLE_IF_KUNIT void io_opaque_vec_build(struct io_opaque_req *op)
{
	struct io_opaque_vec *vec = op->vec;
	u32 i, filled = 0;

	KUNIT_STATIC_STUB_REDIRECT(io_opaque_vec_build, op);
	lockdep_assert_not_held(&op->store->tables);
	for (i = 0; i < vec->nr; i++)
		filled += io_opaque_vec_span(vec->pin[i], vec->start[i].seg, vec->start[i].skip,
					     vec->ent[i].len, vec->bv + filled, U64_MAX);
	vec->nr_segs = filled;
	io_opaque_send_iter(op, vec->bv, filled, op->length, 0);
}

/*
 * Resolve what a send names and take its references, under tables. A send of
 * one range also gets its cursor. A vector returns 0 with its references taken,
 * a negative errno with none taken and nothing removed, or the number of
 * segments its ranges span when its block holds fewer, to be settled by
 * io_opaque_vec_finish() once tables is dropped.
 */
VISIBLE_IF_KUNIT int io_opaque_send_acquire(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	bool last = op->send_flags & IORING_OPAQUE_SEND_LAST;
	struct io_opaque_slot *slot;
	u64 pos = 0;
	u32 seg = 0, skip;
	int ret;

	KUNIT_STATIC_STUB_REDIRECT(io_opaque_send_acquire, op);
	if (unlikely(op->send_flags & IORING_OPAQUE_SEND_VECTOR))
		return io_opaque_vec_acquire(op);
	slot = io_opaque_lookup(store, op->handle);

	if (store->dead)
		return -ESHUTDOWN;
	if (!op->length)
		return 0;
	ret = io_opaque_send_range(slot, op->offset, op->length);
	if (ret)
		return ret;
	op->data = io_opaque_send_pin(store, slot, last);
	if (last)
		op->consumed = true;
	/* The cursor starts at the segment that holds the range, found as a vector's are. */
	skip = io_opaque_vec_seek(op->data, op->offset, &seg, &pos);
	io_opaque_send_iter(op, op->data->bvec + seg, op->data->nr - seg, op->data->length - pos,
			    skip);
	return 0;
}

/*
 * What admission of a vector does without tables, after the hold in which
 * io_opaque_send_acquire() returned @ret. A block with too few segments (@ret
 * of them are needed) is grown here, where allocating is fine, and the vector
 * is resolved again in a new hold: nothing was pinned, and the versions may
 * have changed meanwhile, so every entry is looked at again. Once the vector
 * is admitted, the cursor is built.
 */
VISIBLE_IF_KUNIT int io_opaque_vec_finish(struct io_opaque_req *op, int ret)
{
	while (ret > 0) {
		ret = io_opaque_vec_grow(op, op->vec->nr, ret, op->vec->nr);
		if (ret)
			return ret;
		scoped_guard(mutex, &op->store->tables)
			ret = io_opaque_send_acquire(op);
	}
	if (!ret)
		io_opaque_vec_build(op);
	return ret;
}

/*
 * A vectored send: addr names len entries and off is zero. They are copied
 * now, like SEND_ZC's iovecs, and the array need not stay valid. The request
 * still holds the SQE's addr, off and len in handle, offset and length; the
 * commit turns them into the body, whose handle and offset are none.
 */
static int io_opaque_send_vec_prep(struct io_opaque_req *op)
{
	u32 nr = op->length;
	int ret;

	if (op->offset || !nr || nr > IORING_OPAQUE_SEND_VEC_MAX)
		return -EINVAL;
	/* Every entry spans a segment; admission grows the block if some span more. */
	ret = io_opaque_vec_grow(op, nr, nr, 0);
	if (ret)
		return ret;
	if (copy_from_user(op->vec->ent, u64_to_user_ptr(op->handle),
			   array_size(nr, sizeof(*op->vec->ent))))
		return -EFAULT;
	return io_opaque_vec_commit(op, nr);
}

int io_opaque_send_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_opaque_store *store = io_opaque_get_store(req, READ_ONCE(sqe->opaque_store_id));
	struct io_opaque_req *op;
	u16 send_flags = READ_ONCE(sqe->ioprio);
	u32 msg_flags = READ_ONCE(sqe->msg_flags);
	u64 end;
	int ret;

	if (!store)
		return -ENXIO;
	if ((send_flags & ~(IORING_OPAQUE_SEND_LAST | IORING_OPAQUE_SEND_VECTOR)) ||
	    ((send_flags & IORING_OPAQUE_SEND_LAST) && (req->flags & REQ_F_CQE_SKIP)) ||
	    READ_ONCE(sqe->buf_index) || READ_ONCE(sqe->__pad2[0]) ||
	    (msg_flags & ~(MSG_MORE | MSG_DONTWAIT)))
		return -EINVAL;
	op = io_opaque_req_alloc(req, store, false);
	if (IS_ERR(op))
		return PTR_ERR(op);
	op->op = U16_MAX;
	INIT_LIST_HEAD(&op->send);
	op->send_flags = send_flags;
	op->handle = READ_ONCE(sqe->addr);
	op->offset = READ_ONCE(sqe->off);
	op->length = READ_ONCE(sqe->len);
	op->addr = u64_to_user_ptr(READ_ONCE(sqe->addr3));
	op->msg_flags = msg_flags;
	if (op->msg_flags & MSG_DONTWAIT)
		req->flags |= REQ_F_NOWAIT;
	if (unlikely(send_flags & IORING_OPAQUE_SEND_VECTOR)) {
		ret = io_opaque_send_vec_prep(op);
		if (ret)
			return ret;
	}
	if ((!op->length && !op->addr) || op->length > INT_MAX ||
	    check_add_overflow(op->offset, (u64)op->length, &end))
		return -EINVAL;
	io_opaque_send_resolve(op);
	return 0;
}

static struct io_opaque_req *io_opaque_tx_head(struct io_opaque_tx *tx)
{
	return list_first_entry_or_null(&tx->requests, struct io_opaque_req, send);
}

/* Set under tx_lock, so that a release cannot pass a queued request by. */
static void io_opaque_tx_park(struct io_opaque_req *op)
{
	guard(spinlock)(&op->store->wait_lock);
	op->phase = IO_OPAQUE_SEND_WAIT;
}

static struct io_opaque_tx *io_opaque_tx_find(struct io_opaque_store *store,
					      struct sock *sk,
					      struct rb_node ***linkp,
					      struct rb_node **parentp)
{
	struct rb_node **link = &store->txs.rb_node, *parent = NULL;
	struct io_opaque_tx *tx = NULL;

	lockdep_assert_held(&store->tx_lock);
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
	/* On a miss these are the insertion point. */
	*linkp = link;
	*parentp = parent;
	return *link ? tx : NULL;
}

/* A queue nobody uses stays for the next frame on its socket, up to a cap. */
static void io_opaque_tx_unused(struct io_opaque_store *store, struct io_opaque_tx *tx)
{
	lockdep_assert_held(&store->tx_lock);
	if (!list_empty(&tx->requests) || tx->admitting)
		return;
	/* A failed queue starts clean: the followers it canceled are gone. */
	tx->failed = false;
	list_add_tail(&tx->idle, &store->txs_idle);
	if (++store->nr_txs_idle <= IO_OPAQUE_TX_IDLE_MAX)
		return;
	tx = list_first_entry(&store->txs_idle, struct io_opaque_tx, idle);
	list_del(&tx->idle);
	store->nr_txs_idle--;
	rb_erase(&tx->node, &store->txs);
	kfree(tx);
}

/* A queue that is about to hold a request is not idle. */
static void io_opaque_tx_busy(struct io_opaque_store *store, struct io_opaque_tx *tx)
{
	lockdep_assert_held(&store->tx_lock);
	if (!list_empty(&tx->idle)) {
		list_del_init(&tx->idle);
		store->nr_txs_idle--;
	}
}

static void io_opaque_tx_claim(struct io_opaque_store *store, struct io_opaque_tx *tx)
{
	io_opaque_tx_busy(store, tx);
	tx->admitting++;
}

/*
 * Resolve what @op names and take its references: the handles under tables,
 * and then, without it, the block and the cursor. A vector leaves tables once
 * its references are taken; one whose block is too small leaves it earlier, to
 * grow the block, and is resolved again.
 */
static int io_opaque_tx_acquire(struct io_opaque_req *op)
{
	int ret;

	scoped_guard(mutex, &op->store->tables)
		ret = io_opaque_send_acquire(op);
	if (unlikely(op->send_flags & IORING_OPAQUE_SEND_VECTOR))
		ret = io_opaque_vec_finish(op, ret);
	return ret;
}

/*
 * The admission of a request that found frames ahead of it. It takes its
 * references before it waits, so that a LAST send parked behind another has
 * consumed its object, and joins the queue afterwards, in a second tx_lock
 * hold: a queue that fails in between cancels it. It holds tables alone, not
 * the socket. Returns as io_opaque_tx_join() does.
 */
static int io_opaque_tx_settle(struct io_opaque_req *op, struct io_opaque_tx *tx)
{
	struct io_opaque_store *store = op->store;
	int ret = io_opaque_tx_acquire(op);

	spin_lock(&store->tx_lock);
	tx->admitting--;
	if (ret || tx->failed) {
		bool pinned = !ret;

		io_opaque_tx_unused(store, tx);
		spin_unlock(&store->tx_lock);
		if (!pinned)
			return ret;
		/* The head failed after the check: the pin goes, LAST stays consumed. */
		io_opaque_send_unpin(op);
		return -ECANCELED;
	}
	op->tx = tx;
	list_add_tail(&op->send, &tx->requests);
	ret = io_opaque_tx_head(tx) == op;
	if (!ret)
		io_opaque_tx_park(op);
	spin_unlock(&store->tx_lock);
	return ret;
}

/*
 * Queue @op behind the frames already admitted for @sk. Returns
 * IO_OPAQUE_TX_WAIT when it waits for one (its phase is set before tx_lock is
 * dropped, so a release cannot miss it), IO_OPAQUE_TX_HEAD when it is the head
 * and holds its references, IO_OPAQUE_TX_SETTLE when it is the head and has
 * not taken them yet, or a negative errno.
 *
 * A request that finds its queue empty links itself as the head in the one
 * tx_lock hold that finds out, and takes its references later, from
 * io_opaque_tx_settle_head(), with the socket owned. Nothing can join the
 * queue ahead of it, and a head cannot fail the queue, so nothing that
 * settling does is undone by the queue. A request that finds frames ahead
 * takes its references now, and may find itself the head when it is done
 * (io_opaque_tx_settle()). A queue that has failed refuses it.
 *
 * @op->req is not used; the caller enrolls a waiting request for cancellation.
 */
VISIBLE_IF_KUNIT int io_opaque_tx_join(struct io_opaque_req *op, struct sock *sk)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_tx *tx, *spare = NULL;
	struct rb_node **link, *parent;
	int ret = 0;

	spin_lock(&store->tx_lock);
	tx = op->tx;
	if (tx) {
		/* A resumed or retried request keeps its place. */
		ret = io_opaque_tx_head(tx) == op;
		if (!ret)
			io_opaque_tx_park(op);
		spin_unlock(&store->tx_lock);
		return ret;
	}
	tx = io_opaque_tx_find(store, sk, &link, &parent);
	if (!tx) {
		spin_unlock(&store->tx_lock);
		spare = kzalloc_obj(*spare, GFP_KERNEL_ACCOUNT);
		if (!spare)
			return -ENOMEM;
		spare->sk = sk;
		INIT_LIST_HEAD(&spare->requests);
		INIT_LIST_HEAD(&spare->idle);
		spin_lock(&store->tx_lock);
		/* A ring sharing the store may have created it meanwhile. */
		tx = io_opaque_tx_find(store, sk, &link, &parent);
		if (!tx) {
			rb_link_node(&spare->node, parent, link);
			rb_insert_color(&spare->node, &store->txs);
			tx = spare;
			spare = NULL;
		}
	}
	if (tx->failed) {
		ret = -ECANCELED;
	} else if (list_empty(&tx->requests)) {
		io_opaque_tx_busy(store, tx);
		op->tx = tx;
		list_add_tail(&op->send, &tx->requests);
		ret = IO_OPAQUE_TX_SETTLE;
	} else {
		io_opaque_tx_claim(store, tx);
	}
	spin_unlock(&store->tx_lock);
	kfree(spare);
	if (ret)
		return ret;
	return io_opaque_tx_settle(op, tx);
}

/*
 * Take the references of a head that io_opaque_tx_join() left to settle, with
 * the socket owned where it can be. Nothing is taken and nothing removed when
 * this fails, and the request is still the head: it leaves the queue through
 * io_opaque_tx_put(), which hands the queue to whoever waits behind it and does
 * not fail it, since nothing of this frame was sent.
 */
VISIBLE_IF_KUNIT int io_opaque_tx_settle_head(struct io_opaque_req *op)
{
	return io_opaque_tx_acquire(op);
}

/*
 * Remove @op from its queue. A head that attempted TCP and ended short fails
 * the queue: every follower is canceled and woken, and nothing is returned.
 * Otherwise the follower that becomes head is returned with its phase already
 * queued, for the caller to wake. Only the failure wakeups reach an io_kiocb.
 */
VISIBLE_IF_KUNIT struct io_opaque_req *io_opaque_tx_release(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_tx *tx = op->tx;
	struct io_opaque_req *next;
	bool head;

	guard(spinlock)(&store->tx_lock);
	head = io_opaque_tx_head(tx) == op;
	list_del_init(&op->send);
	op->tx = NULL;
	if (list_empty(&tx->requests)) {
		io_opaque_tx_unused(store, tx);
		return NULL;
	}
	if (!head)
		return NULL;
	/* Only a send that attempted TCP can break the following frames. */
	if (op->tx_started && op->progress != op->total_length) {
		tx->failed = true;
		list_for_each_entry(next, &tx->requests, send) {
			bool queue;

			scoped_guard(spinlock, &store->wait_lock) {
				WRITE_ONCE(next->canceled, true);
				queue = next->phase == IO_OPAQUE_SEND_WAIT;
				if (queue)
					next->phase = IO_OPAQUE_QUEUED;
			}
			if (queue)
				io_opaque_queue_ready(next, -ECANCELED);
		}
		return NULL;
	}
	next = io_opaque_tx_head(tx);
	guard(spinlock)(&store->wait_lock);
	if (next->phase != IO_OPAQUE_SEND_WAIT)
		return NULL;
	next->phase = IO_OPAQUE_QUEUED;
	return next;
}

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * Join and settle in one call, for the tests that have no socket to own.
 * io_opaque_send() does the second step with the socket owned. A head whose
 * references cannot be taken leaves its queue here; the tests that use this
 * have no ring to wake a follower with, and queue none behind such a head.
 */
int io_opaque_tx_admit(struct io_opaque_req *op, struct sock *sk)
{
	int ret = io_opaque_tx_join(op, sk);

	if (ret != IO_OPAQUE_TX_SETTLE)
		return ret;
	ret = io_opaque_tx_settle_head(op);
	if (ret) {
		WARN_ON_ONCE(io_opaque_tx_release(op));
		return ret;
	}
	return IO_OPAQUE_TX_HEAD;
}
#endif

static void io_opaque_tx_put(struct io_opaque_req *op)
{
	struct io_opaque_req *next;

	if (!op->tx)
		return;
	next = io_opaque_tx_release(op);
	if (next) {
		next->req->io_task_work.func = io_opaque_resume;
		io_req_task_work_add(next->req);
	}
}

/*
 * A body of up to send_copy_max bytes is copied by TCP: below that a page
 * reference and a fragment cost more than the copy. The object stays immutable
 * and TCP keeps no reference to it, so nothing else about the send changes.
 * A vector is spliced or copied whole; what the threshold weighs is a
 * fragment, so it is compared with the vector's largest range.
 */
VISIBLE_IF_KUNIT bool io_opaque_send_splices(const struct io_opaque_req *op)
{
	u32 max = READ_ONCE(io_opaque_send_copy_max);

	if (!max)
		return true;
	if (unlikely(op->send_flags & IORING_OPAQUE_SEND_VECTOR))
		return op->vec->max_len > max;
	return op->length > max;
}

#if IS_ENABLED(CONFIG_KUNIT)
/* tcp_sendmsg_locked() calls made by sends, for a test to tell them from sock_sendmsg()'s. */
atomic_long_t io_opaque_send_locked_calls = ATOMIC_LONG_INIT(0);
#define IO_OPAQUE_SEND_LOCKED_CALL()	atomic_long_inc(&io_opaque_send_locked_calls)
#else
#define IO_OPAQUE_SEND_LOCKED_CALL()	((void)0)
#endif

/*
 * Whether a send may call tcp_sendmsg_locked() on @sk where sock_sendmsg()
 * would end up in tcp_sendmsg(): plain TCP, whose sendmsg no ULP (kTLS,
 * espintcp) or socket map has replaced, which they do with a copy of tcp_prot
 * that sk_prot_creator does not follow. A socket with no local port has not
 * connected. TCP's proto has inet_send_prepare() leave it unbound, so this
 * does not change what a send does, and only keeps such a socket on the
 * ordinary path.
 *
 * kTLS and espintcp install their proto under the socket lock, and a socket
 * map under the bottom-half lock alone, so the answer is a hint until the
 * socket is owned and is no more than true then. A swap that comes while
 * tcp_sendmsg_locked() is in progress is what it is for a stock tcp_sendmsg()
 * in flight: that call goes on with the proto it began with, and the next one
 * takes the new, here the next part of the frame, which asks again.
 */
VISIBLE_IF_KUNIT bool io_opaque_send_direct(const struct sock *sk)
{
	const struct proto *prot = READ_ONCE(sk->sk_prot);
	/* Written when the socket binds or connects; a stale zero only spoils a hint. */
	u16 port = data_race(inet_sk(sk)->inet_num);

	return prot->sendmsg == tcp_sendmsg && prot == sk->sk_prot_creator && port;
}

static u32 io_opaque_send_msg_flags(const struct io_opaque_req *op, unsigned int issue_flags)
{
	u32 flags = MSG_NOSIGNAL | op->msg_flags;

	if (issue_flags & IO_URING_F_NONBLOCK)
		flags |= MSG_DONTWAIT;
	return flags;
}

/*
 * The security hook, for an issue that sends on the socket directly. The
 * first sock_sendmsg() of an issue asks it for the part of the frame it sends,
 * and a refusal ends that call before any byte goes: a send that has taken its
 * references and started, so that LAST is consumed and a frame behind it fails
 * with it, or is told to try again if the hook says EAGAIN. Here it is asked
 * once, for what is left of the frame, in the same place and ending the
 * issue in the same way. The ordinary path asks for every part, which a
 * policy that depends on the socket and not on the part cannot tell apart.
 */
VISIBLE_IF_KUNIT int io_opaque_send_security(struct socket *sock, const struct io_opaque_req *op,
					     unsigned int issue_flags)
{
	struct msghdr msg = { .msg_flags = io_opaque_send_msg_flags(op, issue_flags) };

	KUNIT_STATIC_STUB_REDIRECT(io_opaque_send_security, sock, op, issue_flags);
	return security_socket_sendmsg(sock, &msg, op->total_length - op->progress);
}

/*
 * One issue of an object send. A request that is not the head of its
 * destination's queue parks. The head sends its frame, prefix, body and
 * suffix, from where it stopped, and the frame leaves the queue once all of it
 * is queued to TCP.
 *
 * On a plain TCP socket (io_opaque_send_direct()) the head does all of that
 * under one lock_sock(): it takes the references it has not taken yet, asks the
 * security hook, calls tcp_sendmsg_locked() for each part of the frame, hands
 * the queue on, lets go of the references that are not the last and only then
 * releases the socket, and after it puts the last ones. While the socket is
 * unowned, ACKs and the transmit that autocorking left queued run under its
 * spinlock, for as long as they take, and the sender's next lock_sock() waits
 * for them; the less the sender does in that window, the less often it does.
 * Any other socket takes its references first and goes through sock_sendmsg()
 * for each part, with the ring lock dropped around it.
 *
 * Under IO_URING_F_UNLOCKED the ring lock is dropped before the socket is
 * taken and held again once it is released; an inline issue holds it
 * throughout, as it holds it across sock_sendmsg().
 */
int io_opaque_send(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_opaque_req *op = req->async_data;
	struct socket *sock = sock_from_file(req->file);
	struct sock *sk = NULL;
	struct msghdr msg;
	struct kvec vec;
	u32 body_end;
	bool body = false, splice = false, fresh, owned = false;
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
	if (!op->resolved) {
		ret = io_opaque_frame_import(op);
		if (ret)
			goto finish;
	}
	sk = sock->sk;
	ret = io_opaque_tx_join(op, sk);
	if (ret < 0)
		goto out;
	if (ret == IO_OPAQUE_TX_WAIT) {
		/* The ring lock keeps the wakeup from running before the request is listed. */
		io_opaque_enroll(op);
		ret = IOU_ISSUE_SKIP_COMPLETE;
		goto out;
	}
	fresh = ret == IO_OPAQUE_TX_SETTLE;

	if (io_opaque_send_direct(sk)) {
		/* The hint says plain TCP; the socket decides once it is owned. */
		io_ring_submit_unlock(req->ctx, issue_flags);
		sock_rps_record_flow(sk);
		lock_sock(sk);
		owned = io_opaque_send_direct(sk);
		if (!owned) {
			release_sock(sk);
			io_ring_submit_lock(req->ctx, issue_flags);
		}
	}
	if (fresh) {
		ret = io_opaque_tx_settle_head(op);
		if (ret) {
			/* Nothing was taken or sent: the frame behind this one goes now. */
			io_opaque_tx_put(op);
			goto leave;
		}
	}
	op->tx_started = true;
	if (owned) {
		ret = io_opaque_send_security(sock, op, issue_flags);
		if (ret)
			goto sent;
	}

again:
	memset(&msg, 0, sizeof(msg));
	msg.msg_flags = io_opaque_send_msg_flags(op, issue_flags);
	body_end = op->prefix_length + op->length;
	body = op->length && op->progress >= op->prefix_length &&
	       op->progress < body_end;
	if (!op->length) {
		vec.iov_base = op->framing + op->progress;
		vec.iov_len = op->total_length - op->progress;
	} else if (op->progress < op->prefix_length) {
		vec.iov_base = op->framing + op->progress;
		vec.iov_len = op->prefix_length - op->progress;
		msg.msg_flags |= MSG_MORE;
	} else if (body) {
		msg.msg_iter = op->iter;
		splice = io_opaque_send_splices(op);
		if (splice)
			msg.msg_flags |= MSG_SPLICE_PAGES;
		if (op->suffix_length)
			msg.msg_flags |= MSG_MORE;
	} else {
		vec.iov_base = op->framing + op->prefix_length + op->progress - body_end;
		vec.iov_len = op->total_length - op->progress;
	}
	if (!body)
		iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, &vec, 1, vec.iov_len);
	if (owned) {
		IO_OPAQUE_SEND_LOCKED_CALL();
		ret = tcp_sendmsg_locked(sk, &msg, iov_iter_count(&msg.msg_iter));
		if (trace_sock_send_length_enabled())
			trace_sock_send_length(sk, ret, 0);
	} else {
		/* io-wq may block in TCP; other requests must retain access to the ring. */
		io_ring_submit_unlock(req->ctx, issue_flags);
		ret = sock_sendmsg(sock, &msg);
		io_ring_submit_lock(req->ctx, issue_flags);
	}
sent:
	if (ret > 0) {
		if (body) {
			op->iter = msg.msg_iter;
			if (!splice)
				io_opaque_count(IO_OPAQUE_STAT_BODY_COPIED, ret);
		}
		op->progress += ret;
		if (op->progress < op->total_length && !iov_iter_count(&msg.msg_iter)) {
			/*
			 * TCP lets go of the socket while it waits for room, and a
			 * protocol may have taken it over by then: ask again.
			 */
			if (owned && !io_opaque_send_direct(sk)) {
				release_sock(sk);
				io_ring_submit_lock(req->ctx, issue_flags);
				owned = false;
			}
			goto again;
		}
		if (op->progress < op->total_length && !(req->flags & REQ_F_NOWAIT)) {
			ret = -EAGAIN;
			goto leave;
		}
	} else if (ret == -EAGAIN && (issue_flags & IO_URING_F_NONBLOCK) &&
		   !(req->flags & REQ_F_NOWAIT)) {
		goto leave;
	}
finish:
	if (ret == -ERESTARTSYS)
		ret = -EINTR;
	if (ret < 0 || (op->progress && op->progress < op->total_length))
		req_set_fail(req);
	if (op->progress)
		ret = op->progress;
	/* The frame is queued or abandoned: the next send goes now, not at cleanup. */
	io_opaque_tx_put(op);
	if (owned) {
		/* The references go with the socket, the last of a version after it. */
		bool left = io_opaque_send_unpin_cheap(op);

		release_sock(sk);
		if (left)
			io_opaque_send_unpin(op);
		io_ring_submit_lock(req->ctx, issue_flags);
	}
	io_req_set_res(req, ret, op->consumed ? IORING_CQE_F_OPAQUE_CONSUMED : 0);
	ret = IOU_COMPLETE;
	goto out;
leave:
	/* Hand the ring lock back; whatever the request still holds, it keeps. */
	if (owned) {
		release_sock(sk);
		io_ring_submit_lock(req->ctx, issue_flags);
	}
out:
	io_ring_submit_unlock(req->ctx, issue_flags);
	return ret;
}
