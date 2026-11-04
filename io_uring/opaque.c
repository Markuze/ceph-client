// SPDX-License-Identifier: GPL-2.0
/* Kernel-resident immutable TCP payloads. See userspace-api/io_uring-opaque. */
#include <kunit/static_stub.h>
#include <linux/bitmap.h>
#include <linux/bvec.h>
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/kasan.h>
#include <linux/memcontrol.h>
#include <linux/overflow.h>
#include <linux/rbtree.h>
#include <linux/sched/mm.h>
#include <linux/skbuff.h>
#include <linux/skbuff_ref.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/hotdata.h>
#include <net/page_pool/helpers.h>
#include <net/rps.h>
#include <net/tcp.h>
#include <trace/events/sock.h>
#include <uapi/linux/io_uring/opaque.h>

#include "io_uring.h"
#include "cancel.h"
#include "opaque_internal.h"
#include "rsrc.h"
#include "tw.h"
#include "zcrx.h"

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
VISIBLE_IF_KUNIT struct io_opaque_ubuf_pool *io_opaque_ubuf_pool_new(void);
VISIBLE_IF_KUNIT void io_opaque_ubuf_pool_close(struct io_opaque_ubuf_pool *pool);

static bool io_opaque_budget_ready(struct io_opaque_req *op, unsigned long now)
{
	struct io_opaque_store *store = op->store;
	u64 limit = io_opaque_rx_limit(store);
	u64 used = atomic64_read(&store->bytes);

	lockdep_assert_held(&store->wait_lock);
	return !time_before(now, op->retry_at) && used <= limit &&
	       op->need_bytes <= limit - used;
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

/*
 * Let in the waiters for budget that the budget lets go on, at most
 * IO_OPAQUE_WAKE_BATCH of them in a hold of wait_lock. Those left over are for
 * the retry work, and that is why this says whether there were any: a caller
 * that wants them in now repeats it. The work comes a jiffy later, or as late
 * as a timer that is already pending, which queueing never moves earlier.
 */
static bool io_opaque_wake_budget(struct io_opaque_store *store)
{
	struct io_opaque_req *op, *next;
	unsigned long delay = MAX_JIFFY_OFFSET;
	unsigned long now = jiffies;
	unsigned int woken = 0;
	bool stopped, more = false;

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
				more = true;
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
	return more;
}

/* As io_opaque_wake_budget(), until the batch leaves nobody behind. */
static void io_opaque_wake_budget_all(struct io_opaque_store *store)
{
	while (io_opaque_wake_budget(store))
		;
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

/* Give bytes back to the store, and let nobody in on them yet. */
static void io_opaque_uncharge_quiet(struct io_opaque_store *store, u64 bytes)
{
	atomic64_sub(bytes, &store->bytes);
	io_opaque_memory_mode(store);
}

void io_opaque_uncharge(struct io_opaque_store *store, u64 bytes)
{
	io_opaque_uncharge_quiet(store, bytes);
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

/*
 * Let go of @refs of what keeps @block; the last frees it. The last also returns what the store
 * was charged for the block, for the caller to give back; the others return 0.
 */
static u64 io_opaque_block_put(struct io_opaque_block *block, unsigned int refs)
{
	u64 charge;

	if (!refcount_sub_and_test(refs, &block->live))
		return 0;
	charge = block->charge;
	kfree(block);
	return charge;
}

/*
 * The stream hands out no more chunks of its current block: it lets go of the
 * ones that are left and of its hold. The chunks handed out keep the block. If
 * there are none, the block goes, and the store gets its charge back.
 */
VISIBLE_IF_KUNIT void io_opaque_block_retire(struct io_opaque_stream *stream)
{
	struct io_opaque_block *block = stream->block;
	u64 charge;

	if (!block)
		return;
	stream->block = NULL;
	charge = io_opaque_block_put(block, block->nr - block->used + 1);
	if (charge)
		io_opaque_uncharge(stream->store, charge);
}

/* Let go of everything that capture keeps in @stream: the block, and the room for a call. */
VISIBLE_IF_KUNIT void io_opaque_capture_release(struct io_opaque_stream *stream)
{
	io_opaque_block_retire(stream);
	kfree(stream->pending);
	stream->pending = NULL;
}

/*
 * The next chunk of the current block of @stream, in a new block if that is used up. A new
 * block is charged to the store, as capture charges a page: against the limit that capture
 * stops at, and it is given back when the last chunk of the block is released. That is one
 * charge for IO_OPAQUE_BLOCK_CHUNKS captures. Returns an error pointer: -ENOBUFS if the
 * block does not fit, -ENOMEM if it cannot be allocated.
 */
static struct io_opaque_chunk *io_opaque_chunk_take(struct io_opaque_stream *stream)
{
	struct io_opaque_block *block = stream->block;
	struct io_opaque_chunk *chunk;

	if (!block) {
		u32 charge = io_opaque_block_bytes(IO_OPAQUE_BLOCK_CHUNKS);

		if (!io_opaque_charge(stream->store, charge, true))
			return ERR_PTR(-ENOBUFS);
		block = kzalloc_flex(*block, chunk, IO_OPAQUE_BLOCK_CHUNKS, GFP_KERNEL_ACCOUNT);
		if (!block) {
			io_opaque_uncharge(stream->store, charge);
			return ERR_PTR(-ENOMEM);
		}
		block->nr = IO_OPAQUE_BLOCK_CHUNKS;
		block->charge = charge;
		/* Every chunk, to be handed out or released, and the hold of the stream. */
		refcount_set(&block->live, block->nr + 1);
		stream->block = block;
		io_opaque_count(IO_OPAQUE_STAT_CHUNK_BLOCKS, 1);
	}
	chunk = &block->chunk[block->used++];
	chunk->block = block;
	if (block->used == block->nr)
		io_opaque_block_retire(stream);
	return chunk;
}

/*
 * A chunk in a block of its own, for the callers that make one extent at a time. The block is
 * charged to the store as it is: against the hard limit, not the capture limit, which is how
 * CREATE and compaction use the room that capture leaves them. It is given back when the
 * chunk is released. Returns an error pointer as io_opaque_chunk_take() does.
 */
static struct io_opaque_chunk *io_opaque_chunk_alone(struct io_opaque_store *store)
{
	u32 charge = io_opaque_block_bytes(1);
	struct io_opaque_block *block;

	if (!io_opaque_charge(store, charge, false))
		return ERR_PTR(-ENOBUFS);
	block = kzalloc_flex(*block, chunk, 1, GFP_KERNEL_ACCOUNT);
	if (!block) {
		io_opaque_uncharge(store, charge);
		return ERR_PTR(-ENOMEM);
	}
	block->nr = 1;
	block->used = 1;
	block->charge = charge;
	refcount_set(&block->live, 1);
	block->chunk[0].block = block;
	return &block->chunk[0];
}

static struct io_opaque_extent *io_opaque_extent_make(struct io_opaque_store *store,
						      struct io_opaque_stream *stream,
						      struct page *page, u32 offset,
						      u32 length, u64 start,
						      unsigned int flags)
{
	bool reserved = flags & IO_OPAQUE_EXT_RESERVED;
	/* The memory cgroup pays for a page only if no other owner does. */
	bool memcg = !(flags & (IO_OPAQUE_EXT_RESERVED | IO_OPAQUE_EXT_NO_MEMCG));
	struct io_opaque_chunk *chunk;
	u64 charge = page_size(compound_head(page));
	int err = -ENOMEM;

	if (!reserved && !io_opaque_charge(store, charge, true))
		return ERR_PTR(-ENOBUFS);
	if (memcg && io_opaque_memcg_charge(store, charge))
		goto uncharge;
	chunk = stream ? io_opaque_chunk_take(stream) : io_opaque_chunk_alone(store);
	if (IS_ERR(chunk)) {
		err = PTR_ERR(chunk);
		goto uncharge_memcg;
	}
	refcount_set(&chunk->backing.refs, 1);
	chunk->backing.page = compound_head(page);
	chunk->backing.charge = charge;
	/* The reference of the caller passes to the backing; the caller may set another kind. */
	chunk->backing.ref = IO_OPAQUE_REF_PAGE;
	chunk->backing.units = 1;
	chunk->backing.accounted = memcg && store->objcg;
	chunk->extent.backing = &chunk->backing;
	chunk->extent.start = start;
	chunk->extent.offset = offset + (page - compound_head(page)) * PAGE_SIZE;
	chunk->extent.length = length;
	chunk->extent.embedded = true;
	INIT_LIST_HEAD(&chunk->extent.list);
	atomic_long_inc(&store->extents);
	return &chunk->extent;
uncharge_memcg:
	if (memcg)
		io_opaque_memcg_uncharge(store, charge);
uncharge:
	if (!reserved)
		io_opaque_uncharge(store, charge);
	return ERR_PTR(err);
}

struct io_opaque_extent *io_opaque_extent_new(struct io_opaque_store *store,
					      struct page *page, u32 offset,
					      u32 length, u64 start,
					      unsigned int flags)
{
	return io_opaque_extent_make(store, NULL, page, offset, length, start, flags);
}

/* As io_opaque_extent_new(), for capture: the chunk comes from a block of the stream. */
VISIBLE_IF_KUNIT struct io_opaque_extent *
io_opaque_stream_extent_new(struct io_opaque_stream *stream, struct page *page,
			    u32 offset, u32 length, u64 start, unsigned int flags)
{
	return io_opaque_extent_make(stream->store, stream, page, offset, length, start, flags);
}

/*
 * Drop the @units references of a page pool page, the way napi_pp_put_page()
 * drops the one of an skb fragment: the last one hands the page back to its
 * pool, which recycles it unless somebody else still holds a page reference.
 */
static void io_opaque_pp_put(struct page *page, unsigned int units)
{
	netmem_ref netmem = page_to_netmem(page);

	if (!IS_ENABLED(CONFIG_PAGE_POOL)) {
		WARN_ON_ONCE(1);
		return;
	}
	if (page_pool_unref_netmem(netmem, units))
		return;
	page_pool_put_unrefed_netmem(netmem_get_pp(netmem), netmem, -1, false);
}

/*
 * Give back what a backing holds of its page, as its kind says. The last
 * extent of the backing is gone, so nobody reads the page through it any more.
 */
static void io_opaque_backing_put(struct io_opaque_store *store,
				  struct io_opaque_backing *backing)
{
	unsigned int units = backing->units;

	switch (backing->ref) {
	case IO_OPAQUE_REF_PAGE:
		/* An outsider reference to a page of the area is a page reference too. */
		folio_put_refs(page_folio(backing->page), units);
		break;
	case IO_OPAQUE_REF_PP:
		io_opaque_pp_put(backing->page, units);
		break;
	case IO_OPAQUE_REF_MAIO:
		/*
		 * A pool reference of a bound store ends in the instance. The
		 * store holds its own reference to the instance until every
		 * backing is gone, so the call is valid here.
		 */
		while (units--)
			io_zcrx_maio_page_put(store->maio, backing->page);
		break;
	case IO_OPAQUE_REF_UNIT:
		/* A cached unit leaves the object charge; it is not payload held. */
		io_opaque_unit_put(store, backing->page);
		break;
	default:
		/* A pending backing holds nothing: the skb still owns its reference. */
		WARN_ON_ONCE(1);
	}
}

/*
 * Let go of @extent, and of its backing if it was the last to use it. Returns
 * what the store was charged for what went, for the caller to give back to the
 * store: the extent, if it was a record of its own; the page of the backing, if
 * that was the last extent to use it; and the block of the chunk, if that was
 * the last chunk of its block. Giving it back lets those waiting for budget in,
 * which one free does once and a list of them once, not once per extent.
 */
static u64 io_opaque_extent_release(struct io_opaque_store *store, struct io_opaque_extent *extent)
{
	struct io_opaque_backing *backing = extent->backing;
	u64 charge = 0;

	list_del(&extent->list);
	if (!extent->embedded) {
		kfree(extent);
		charge = io_opaque_extent_bytes();
	}
	atomic_long_dec(&store->extents);
	if (refcount_dec_and_test(&backing->refs)) {
		struct io_opaque_chunk *chunk;

		charge += backing->charge;
		if (backing->accounted)
			io_opaque_memcg_uncharge(store, backing->charge);
		/*
		 * A call of the actor has not settled a pending backing yet, and it
		 * will. Nobody else may release one. If it happens, leak the chunk and
		 * the page reference that the call will give it, rather than free the
		 * chunk under the call. The block stays charged with the chunk.
		 */
		if (WARN_ON_ONCE(backing->ref == IO_OPAQUE_REF_PENDING))
			return charge;
		io_opaque_backing_put(store, backing);
		chunk = container_of(backing, struct io_opaque_chunk, backing);
		charge += io_opaque_block_put(chunk->block, 1);
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
 * Free the extents of @list, and give the store back @extra bytes besides what they
 * held. What they held is returned to the store in one step, and the waiters for
 * budget are looked at in one go: each look takes wait_lock and walks the list of
 * them, which for the 700 extents of a 1 MB object that is not a single mapping was
 * 700 times. The look is repeated while it leaves ready waiters behind, as the many
 * that it replaces would have, so that they are let in by the free and not by the
 * retry work.
 */
static void io_opaque_extents_free_more(struct io_opaque_store *store,
					struct list_head *list, u64 extra)
{
	struct io_opaque_extent *extent, *next;
	bool any = extra != 0;
	u64 charge = extra;

	list_for_each_entry_safe(extent, next, list, list) {
		charge += io_opaque_extent_release(store, extent);
		any = true;
	}
	if (charge)
		io_opaque_uncharge_quiet(store, charge);
	if (any)
		io_opaque_wake_budget_all(store);
}

VISIBLE_IF_KUNIT void io_opaque_extents_free(struct io_opaque_store *store,
					     struct list_head *list)
{
	io_opaque_extents_free_more(store, list, 0);
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
		u64 vector = data->vec_bytes;

		kvfree(data->bvec);
		/* The vector was charged to the store with the object, and goes back with it. */
		io_opaque_extents_free_more(store, &data->extents, vector);
		/*
		 * The extents and the segments go now, as they always did. Only the struct
		 * waits for the grace period: a lookup that read the version from its slot
		 * in RCU may still be about to look at it (io_opaque_pin()). It reads the
		 * count, which may be zero by now, and the handle and the length, before it
		 * holds a reference. Those two are set before the version is published and
		 * never change, so they are what they were whenever the lookup reads them,
		 * and the struct is there for it to read. It reads nothing else until it
		 * holds a reference, and the segments are gone.
		 */
		kfree_rcu(data, rcu);
	}
}

/*
 * Build the version's segments, and for a version of more than
 * IO_OPAQUE_SEEK_STRIDE of them the anchors that find one without walking
 * from the first, in one allocation. Both are made once, when the version is,
 * and never change. The anchors follow the segments at the next multiple of
 * eight bytes: a segment is 16 bytes, but only 12 on 32 bits, where an odd
 * number of them would leave the anchors four-byte aligned.
 *
 * The allocation is memory of the store, as the records it is made from are: it is charged
 * once, for the whole version, and io_opaque_data_put() gives it back with the version. The
 * charge is against the hard limit and not the capture limit, because the version holds its
 * pages already and it is capture that the limit is there to stop. Returns -ENOBUFS if it
 * does not fit.
 */
int io_opaque_vector(struct io_opaque_store *store, struct io_opaque_data *data)
{
	struct io_opaque_extent *extent;
	struct io_opaque_backing *previous = NULL;
	bool mapped = true;
	u32 nr = 0, anchors;
	size_t bytes, size;
	u64 at = 0;

	data->charge = 0;
	list_for_each_entry(extent, &data->extents, list) {
		nr++;
		if (extent->backing != previous)
			data->charge += extent->backing->charge;
		previous = extent->backing;
		mapped &= extent->backing->area;
	}
	anchors = nr > IO_OPAQUE_SEEK_STRIDE ? DIV_ROUND_UP(nr, IO_OPAQUE_SEEK_STRIDE) : 0;
	/* Where the anchors start. An overflow saturates, and the allocation fails. */
	bytes = size_add(array_size(nr, sizeof(*data->bvec)), sizeof(*data->anchor) - 1);
	bytes &= ~(sizeof(*data->anchor) - 1);
	size = size_add(bytes, array_size(anchors, sizeof(*data->anchor)));
	/* More than kvmalloc() allocates, and more than vec_bytes holds. */
	if (size > INT_MAX)
		return -ENOMEM;
	if (!io_opaque_charge(store, size, false))
		return -ENOBUFS;
	data->bvec = kvmalloc(size, GFP_KERNEL_ACCOUNT);
	if (!data->bvec) {
		io_opaque_uncharge(store, size);
		return -ENOMEM;
	}
	data->vec_bytes = size;
	data->anchor = anchors ? (void *)data->bvec + bytes : NULL;
	data->nr = nr;
	data->mapped = nr && mapped;
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
	if (!slot)
		return NULL;
	data = io_opaque_slot_data(store, slot);
	if (!data)
		return NULL;
	RCU_INIT_POINTER(slot->data, NULL);
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

	guard(spinlock)(&store->tables);
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
	KUNIT_STATIC_STUB_REDIRECT(io_opaque_stream_put, stream);
	if (stream && refcount_dec_and_test(&stream->refs)) {
		io_opaque_extents_free(stream->store, &stream->extents);
		io_opaque_capture_release(stream);
		fput(stream->file);
		mutex_destroy(&stream->lock);
		kfree(stream);
	}
}

struct io_opaque_store *io_opaque_alloc(struct io_ring_ctx *ctx,
					const struct io_uring_opaque_config *config)
{
	struct io_uring_opaque_config cfg = *config;
	struct io_zcrx_ifq *maio = NULL;
	struct io_opaque_store *store;
	int ret;
	u32 i;

	if ((cfg.flags & ~IORING_OPAQUE_CFG_MAIO) || cfg.__resv1 ||
	    (cfg.zcrx_id && !(cfg.flags & IORING_OPAQUE_CFG_MAIO)) ||
	    !mem_is_zero(cfg.__resv, sizeof(cfg.__resv)))
		return ERR_PTR(-EINVAL);
	if (!cfg.hard_limit || cfg.hard_limit > SZ_1T ||
	    !IS_ALIGNED(cfg.hard_limit, PAGE_SIZE) ||
	    !IS_ALIGNED(cfg.compact_headroom, PAGE_SIZE) ||
	    cfg.compact_headroom >= cfg.hard_limit ||
	    !cfg.max_object_size || cfg.max_object_size > INT_MAX ||
	    !cfg.max_objects || cfg.max_objects > 16384 ||
	    !cfg.max_streams || cfg.max_streams > 1024 ||
	    cfg.max_extents < 4 || cfg.max_extents > 65536)
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
	/*
	 * The caller holds the lock of the ring, which io_zcrx_maio_get()
	 * needs. The store keeps the reference to the instance until
	 * io_opaque_free(), and so it outlives every page of the store.
	 */
	if (cfg.flags & IORING_OPAQUE_CFG_MAIO) {
		if (!IS_ENABLED(CONFIG_IO_URING_ZCRX))
			return ERR_PTR(-EOPNOTSUPP);
		maio = io_zcrx_maio_get(ctx, cfg.zcrx_id);
		if (!maio)
			return ERR_PTR(-ENXIO);
	}
	store = kzalloc_obj(*store, GFP_KERNEL_ACCOUNT);
	if (!store) {
		ret = -ENOMEM;
		goto put_maio;
	}
	refcount_set(&store->refs, 1);
	refcount_set(&store->users, 1);
	store->config = cfg;
	io_opaque_unit_cache_setup(store);
	store->maio = maio;
	/* The binding never changes, nor does the mark it gives the sends of the store. */
	if (maio)
		store->send_ubuf_flags = SKBFL_SHARED_FRAG;
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
	spin_lock_init(&store->tables);
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
	/* Not having one is fine: the ubufs of a store without a pool come from the cache alone. */
	store->ubuf_pool = io_opaque_ubuf_pool_new();
	return store;
free_store:
	bitmap_free(store->stream_used);
	bitmap_free(store->object_used);
	kvfree(store->streams);
	kvfree(store->objects);
	kfree(store);
put_maio:
	if (maio)
		io_zcrx_maio_put(maio);
	return ERR_PTR(ret);
}

void io_opaque_stop(struct io_opaque_store *store)
{
	u32 i;

	if (!store)
		return;
	/* Close enrollment before draining the compaction dispatcher. */
	scoped_guard(spinlock, &store->tables) {
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

		spin_lock(&store->tables);
		stream = store->streams[i].stream;
		store->streams[i].stream = NULL;
		if (stream) {
			store->nr_streams--;
			io_opaque_release_slot(store->stream_used, i, store->streams[i].generation);
		}
		spin_unlock(&store->tables);
		if (stream) {
			io_opaque_stream_close(stream);
			io_opaque_stream_put(stream);
		}
		cond_resched();
	}
	for (i = 0; i < store->config.max_objects; i++) {
		struct io_opaque_data *data;

		spin_lock(&store->tables);
		data = io_opaque_detach(store, &store->objects[i]);
		spin_unlock(&store->tables);
		io_opaque_data_put(store, data);
		cond_resched();
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
	WARN_ON_ONCE(atomic_long_read(&store->extents));
	WARN_ON_ONCE(store->cache.nr_idle || store->cache.nr_cooling);
	io_opaque_txs_free(store);
	io_opaque_ubuf_pool_close(store->ubuf_pool);
	store->ubuf_pool = NULL;
	/*
	 * Every backing is gone. The last ones released their pages into the
	 * instance, so the reference ends after them.
	 */
	if (store->maio)
		io_zcrx_maio_put(store->maio);
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

/*
 * A request from the ring's cache, or a new one, with nothing set but the flag
 * that the core cleans it up: a recycled one is what the last request left. The
 * caller sets what it reads, all of the request but its block of a vector in
 * io_opaque_req_alloc(), only what the opcode reads in io_opaque_send_prep().
 */
static struct io_opaque_req *io_opaque_req_get(struct io_kiocb *req,
					       struct io_opaque_store *store)
{
	struct io_opaque_req *op;

	if (READ_ONCE(store->dead))
		return ERR_PTR(-ESHUTDOWN);
	op = io_uring_alloc_async_data(&req->ctx->opaque_cache, req);
	if (!op)
		return ERR_PTR(-ENOMEM);
	req->flags |= REQ_F_NEED_CLEANUP;
	return op;
}

static struct io_opaque_req *io_opaque_req_alloc(struct io_kiocb *req,
						 struct io_opaque_store *store)
{
	struct io_opaque_req *op = io_opaque_req_get(req, store);

	if (IS_ERR(op))
		return op;
	/* Not the vector block, which a recycled request brings along. */
	memset(op, 0, offsetof(struct io_opaque_req, vec));
	op->op = U16_MAX;
	op->req = req;
	op->store = store;
	op->retry_at = jiffies;
	INIT_LIST_HEAD(&op->wait);
	INIT_LIST_HEAD(&op->cancel);
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
static __always_inline void io_opaque_enroll(struct io_opaque_req *op)
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

/*
 * Split @extent at @at. The new extent is a record of its own, and the store is charged for
 * it as for any record outside a block of a stream: against the hard limit and not the
 * capture limit. Capture stops at the capture limit and leaves the headroom above it, which
 * holds the splits at the ends of a take, so no record is kept back for them. Returns
 * -ENOBUFS if the record does not fit.
 */
VISIBLE_IF_KUNIT struct io_opaque_extent *io_opaque_extent_split(struct io_opaque_store *store,
								 struct io_opaque_extent *extent,
								 u64 at)
{
	struct io_opaque_extent *right;
	u32 left = at - extent->start;

	if (!io_opaque_charge(store, io_opaque_extent_bytes(), false))
		return ERR_PTR(-ENOBUFS);
	right = kmalloc_obj(*right, GFP_KERNEL_ACCOUNT);
	if (!right) {
		io_opaque_uncharge(store, io_opaque_extent_bytes());
		return ERR_PTR(-ENOMEM);
	}
	atomic_long_inc(&store->extents);
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

/*
 * Copy the extents that INSPECT spans into the version of @op. The copies are records of
 * their own: the snapshot counts them first and charges the store for all of them at once,
 * against the hard limit as a split is, and the free of the version gives them back with its
 * pages in one step. A copy that cannot be allocated gives back the charge of those after it.
 */
static int io_opaque_snapshot(struct io_opaque_req *op)
{
	struct io_opaque_extent *extent;
	int ret = io_opaque_available(op->stream, op->offset, op->length);
	u64 end, left = 0;

	if (ret <= 0)
		return ret;
	end = op->offset + ret;
	list_for_each_entry(extent, &op->stream->extents, list)
		if (max(op->offset, extent->start) < min(end, extent->start + extent->length))
			left++;
	if (!io_opaque_charge(op->store, left * io_opaque_extent_bytes(), false))
		return -ENOBUFS;
	list_for_each_entry(extent, &op->stream->extents, list) {
		u64 first = max(op->offset, extent->start);
		u64 last = min(end, extent->start + extent->length);
		struct io_opaque_extent *copy;

		if (first >= last)
			continue;
		copy = kmalloc_obj(*copy, GFP_KERNEL_ACCOUNT);
		if (!copy) {
			io_opaque_uncharge(op->store, left * io_opaque_extent_bytes());
			return -ENOMEM;
		}
		left--;
		atomic_long_inc(&op->store->extents);
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

/*
 * Whether the reference that @skb holds to @page is a page pool fragment
 * reference. That is what the stack drops for a page pool page of an skb that is
 * marked for recycling, see skb_page_unref(). It takes any other page of the
 * skb by a page reference.
 */
static bool io_opaque_pp_page(const struct sk_buff *skb, struct page *page)
{
	return IS_ENABLED(CONFIG_PAGE_POOL) && skb->pp_recycle &&
	       page_pool_page_is_pp(compound_head(page));
}

/*
 * Note that a store without an instance holds a page of the area of one. The
 * instance opens every receive queue of its device, so the page pool of a queue
 * gives the pages of the area to the driver, and an skb from the queue carries them.
 * The store holds such a page by a pool reference, as it holds the page of any pool
 * (io_opaque_retain()), and the process that maps the area can write it. The managed
 * sends of the store are marked from then on, as those of a bound store are
 * (io_opaque_send_ubuf_flags()). Set once: the mark is the store's, not the object's,
 * and a store does not go back.
 *
 * The look asks send_ubuf_flags, which the sends read, and not maio_pages, and sets both.
 * A capture that finds the store marked goes on to publish an object that the sends of
 * the store will send, and it has found there what they will read, with no order between
 * two flags to give it.
 *
 * The caller's skb holds a pool reference to @page. That keeps its pool, and with it
 * the instance and its area, for the look. A page that an unmarked skb holds by a
 * plain reference is not looked at, because the pool can let go of it at any time.
 */
static void io_opaque_note_area_page(struct io_opaque_store *store, struct page *page)
{
	/* The key is off until a pool binds a provider, so a host that has none pays a branch. */
	if (IS_ENABLED(CONFIG_PAGE_POOL) && static_branch_unlikely(&page_pool_mem_providers) &&
	    !READ_ONCE(store->send_ubuf_flags) && io_zcrx_maio_pool_area_page(page)) {
		WRITE_ONCE(store->send_ubuf_flags, SKBFL_SHARED_FRAG);
		WRITE_ONCE(store->maio_pages, true);
	}
}

/*
 * Decide how a store holds a page that an skb carries. Return false if the
 * capture must copy the bytes.
 *
 * A store without an instance holds a page pool page of an skb that is marked
 * for recycling by a page pool fragment reference, and every other page by a
 * page reference. The skb holds a reference of the same kind, and its pool
 * reference keeps the pool fields of the page stable while the store takes its
 * own. The last release hands the page back to its pool, wherever the skb is by
 * then. A page reference would make the pool give the page up when the skb is
 * freed before the store lets go. A page of the area of a MAIO instance, which a
 * pool of the instance owns, is one that the process that maps the area can write,
 * and the store notes it (io_opaque_note_area_page()).
 *
 * A reference of the pool keeps the pool alive for as long as an object holds
 * it. After the device is removed, or when its driver creates its pools again,
 * the old pool stays until the objects that hold its pages are freed, and the
 * page pool code warns of a stalled shutdown every minute meanwhile.
 *
 * A store with an instance keeps all the bytes that it captures in the area, so
 * that the process sees them through its mapping:
 *
 * - A page outside the area is copied. This includes a fallback page, which
 *   a pool of the instance got from the page allocator.
 * - An area page that a pool of the instance owns is held by a pool reference.
 *   The pool reference keeps the page out of the pool. When the last holder
 *   drops it, the page goes back to the pool and not to limbo. A pool
 *   reference is valid only if the skb holds one too. The pool then owns the
 *   page for as long as the skb lives, and a concurrent release cannot happen
 *   between the test and the reference. An skb that is not marked for page pool
 *   recycling holds a page reference. The pool can release the page at any
 *   time, so the page counts as an outsider page.
 * - Any other area page is held by a page reference.
 *
 * The kernel never parses the bytes of an object, and nothing in this file
 * relies on them staying constant. The process can write them through the
 * mapping. A length or a checksum is never read from them.
 */
static bool io_opaque_retain(struct io_opaque_store *store,
			     const struct sk_buff *skb, struct page *page,
			     enum io_opaque_hold *hold)
{
	u64 off;

	if (!store->maio) {
		if (io_opaque_pp_page(skb, page)) {
			*hold = IO_OPAQUE_HOLD_PP;
			io_opaque_note_area_page(store, page);
		} else {
			*hold = IO_OPAQUE_HOLD_PAGE;
		}
		return true;
	}
	if (!io_zcrx_maio_page_off(store->maio, page, &off))
		return false;
	if (skb->pp_recycle && io_zcrx_maio_page_owned(store->maio, page))
		*hold = IO_OPAQUE_HOLD_POOL;
	else
		*hold = IO_OPAQUE_HOLD_AREA;
	return true;
}

/* The kind of reference that a backing holds on a page that the capture got this way. */
static enum io_opaque_ref io_opaque_hold_ref(const struct io_opaque_store *store,
					     enum io_opaque_hold hold)
{
	if (hold == IO_OPAQUE_HOLD_PP)
		return IO_OPAQUE_REF_PP;
	/* A copy page of a bound store has a pool reference, of the area. */
	if (hold == IO_OPAQUE_HOLD_POOL || (hold == IO_OPAQUE_HOLD_COPY && store->maio))
		return IO_OPAQUE_REF_MAIO;
	return IO_OPAQUE_REF_PAGE;
}

/*
 * What the memory cgroup of the store pays for. The caller charged a copy page.
 * The memory cgroup does not pay for an area page, because the area was
 * allocated and accounted when the instance was registered. It pays for every
 * other page the store holds.
 */
VISIBLE_IF_KUNIT unsigned int io_opaque_hold_flags(enum io_opaque_hold hold)
{
	if (hold == IO_OPAQUE_HOLD_COPY)
		return IO_OPAQUE_EXT_RESERVED;
	if (hold == IO_OPAQUE_HOLD_AREA || hold == IO_OPAQUE_HOLD_POOL)
		return IO_OPAQUE_EXT_NO_MEMCG;
	return 0;
}

/* The kind of reference a backing holds, or the kind that settles it if it is pending. */
static enum io_opaque_ref io_opaque_backing_kind(const struct io_opaque_backing *backing)
{
	return backing->ref == IO_OPAQUE_REF_PENDING ? backing->want : backing->ref;
}

/* Take a reference of the kind of @backing to its page. */
static void io_opaque_ref_take(struct io_opaque_store *store, struct io_opaque_backing *backing)
{
	switch (backing->ref) {
	case IO_OPAQUE_REF_PP:
		page_pool_ref_netmem(page_to_netmem(backing->page));
		break;
	case IO_OPAQUE_REF_MAIO:
		io_zcrx_maio_page_get(store->maio, backing->page);
		break;
	default:
		get_page(backing->page);
	}
}

/*
 * Give @backing the reference that an entry of the pass needs. The skb holds a
 * reference of the kind of the backing to every fragment it carries. If @move,
 * that reference passes to the backing, and nothing is taken. Otherwise the
 * backing takes a reference of its own, unless it holds one already: the page
 * is pinned then, and its units do not depend on the fragment. Return whether
 * the reference of the skb passed. The counters count pages: a backing counts
 * once, however many fragments of its page it holds.
 */
static bool io_opaque_settle_ref(struct io_opaque_store *store, struct io_opaque_backing *backing,
				 bool move)
{
	if (backing->ref == IO_OPAQUE_REF_PENDING) {
		backing->ref = backing->want;
		backing->units = 1;
		if (!move) {
			io_opaque_ref_take(store, backing);
			io_opaque_count(IO_OPAQUE_STAT_PAGES_HELD, 1);
			return false;
		}
		io_opaque_count(IO_OPAQUE_STAT_PAGES_STOLEN, 1);
		return true;
	}
	if (move && backing->units < U16_MAX) {
		backing->units++;
		return true;
	}
	return false;
}

/*
 * Settle the oldest @n entries of the pass at once, each with a reference of its
 * own: the pass has more fragments to remember than the stream has room for.
 * Their fragments stay with the skb, which drops its references to them as it
 * would have.
 */
static void io_opaque_settle_oldest(struct io_opaque_stream *stream, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		io_opaque_settle_ref(stream->store, stream->pending[i].backing, false);
	stream->nr_pending -= n;
	memmove(stream->pending, stream->pending + n,
		stream->nr_pending * sizeof(*stream->pending));
}

/* Remember that @backing holds, or will hold, bytes of the fragment @src. */
static void io_opaque_note(struct io_opaque_stream *stream, struct io_opaque_backing *backing,
			   const struct io_opaque_src *src)
{
	struct io_opaque_pending *last = stream->nr_pending ?
					 &stream->pending[stream->nr_pending - 1] : NULL;

	/* More bytes of the fragment and the backing that the last entry names. */
	if (last && last->backing == backing && last->src.skb == src->skb &&
	    last->src.frag == src->frag)
		return;
	if (stream->nr_pending == IO_OPAQUE_PENDING_MAX)
		io_opaque_settle_oldest(stream, IO_OPAQUE_PENDING_MAX / 2);
	stream->pending[stream->nr_pending++] = (struct io_opaque_pending) {
		.backing = backing, .src = *src,
	};
}

/*
 * Settle the entries @n of one skb. @take says the pass may pass on the references
 * of the skb: it consumed the skb to its end, so TCP frees it as soon as the actor
 * returns, and nobody else can read the skb in the meantime.
 *
 * Only a fragment gives its reference away, and only the first backing that
 * names it, and only when the fragment lies wholly at or after the offset at
 * which the pass began. The skb ends at the first fragment that it gave away:
 * the references to the later ones that no backing took go back now, as the
 * free of the skb would drop them, and the skb forgets all of them. The
 * length of the skb and its accounting stay as they are.
 */
static void io_opaque_settle_skb(struct io_opaque_stream *stream, struct io_opaque_pending *entry,
				 u32 n, bool take)
{
	struct sk_buff *skb = entry->src.skb;
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int cut = -1, previous = -2;
	u32 i, f;

	for (i = 0; i < n; i++) {
		struct io_opaque_pending *p = &entry[i];
		bool first = p->src.frag != previous;

		previous = p->src.frag;
		p->moved = io_opaque_settle_ref(stream->store, p->backing,
						take && first && p->src.frag >= 0 &&
						p->src.from_entry);
		if (p->moved && cut < 0)
			cut = p->src.frag;
	}
	if (cut < 0)
		return;
	i = 0;
	for (f = cut; f < shinfo->nr_frags; f++) {
		bool moved = false;

		for (; i < n && entry[i].src.frag <= (int)f; i++)
			moved |= entry[i].src.frag == (int)f && entry[i].moved;
		if (!moved)
			__skb_frag_unref(&shinfo->frags[f], skb->pp_recycle);
	}
	shinfo->nr_frags = cut;
}

/*
 * The actor has consumed @consumed bytes of @skb from @entry on. Settle every
 * reference that the call left open.
 */
static void io_opaque_settle_call(struct io_opaque_stream *stream, struct sk_buff *skb,
				  u32 entry, u32 consumed)
{
	bool whole = (u64)entry + consumed == skb->len;
	u32 i = 0;

	stream->in_call = false;
	while (i < stream->nr_pending) {
		struct sk_buff *part = stream->pending[i].src.skb;
		u32 j = i;

		while (j < stream->nr_pending && stream->pending[j].src.skb == part)
			j++;
		io_opaque_settle_skb(stream, &stream->pending[i], j - i,
				     whole && !skb_zcopy(part));
		i = j;
	}
	stream->nr_pending = 0;
}

/*
 * Take a page for the bytes that the capture copies. A bound store takes a
 * free page of the area, and the area has none when it is empty. Another store
 * allocates a page. The caller owns the reference to the page.
 */
static struct page *io_opaque_copy_page(struct io_opaque_store *store)
{
	u64 off;

	if (store->maio)
		return io_zcrx_maio_copy_page(store->maio, &off);
	return alloc_page(GFP_KERNEL_ACCOUNT);
}

static void io_opaque_copy_page_put(struct io_opaque_store *store,
				    struct page *page)
{
	if (store->maio)
		io_zcrx_maio_page_put(store->maio, page);
	else
		put_page(page);
}

/*
 * What a stream needs below the capture limit to take a page of @bytes: the page and, if the
 * stream has no block to take a chunk from, the block, which is charged with it. A collector
 * that waits for budget is let in when this fits, and not when only the page does.
 */
static u64 io_opaque_need(const struct io_opaque_stream *stream, u64 bytes)
{
	if (!stream->block)
		bytes += io_opaque_block_bytes(IO_OPAQUE_BLOCK_CHUNKS);
	return bytes;
}

/*
 * Keep @length bytes of @page at @offset in the stream, in a backing that holds
 * the page. @src says where they come from, if the pass may take the reference of
 * the skb over: the backing is then left pending, and the pass settles it. Without
 * @src the backing takes its reference now.
 */
static int io_opaque_append(struct io_opaque_stream *stream, struct page *page,
			    u32 offset, u32 length, enum io_opaque_hold hold,
			    const struct io_opaque_src *src)
{
	bool copied = hold == IO_OPAQUE_HOLD_COPY;
	enum io_opaque_ref kind = io_opaque_hold_ref(stream->store, hold);
	bool defer = src && stream->in_call;
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
	/*
	 * Bytes of a page that a backing holds join it, if what the backing holds
	 * is what this capture would hold: units of another kind do not add up.
	 */
	if (tail && (tail->backing->page != page || io_opaque_backing_kind(tail->backing) != kind))
		tail = NULL;
	if (tail) {
		if (tail->start + tail->length == start &&
		    tail->offset + tail->length == offset) {
			tail->length += length;
			extent = tail;
			goto accounted;
		}
	}
	stream->need_bytes = page_size(page);
	if (tail) {
		/*
		 * The page is held already, so this is a record and no more. The store is charged
		 * for it as for a split, against the hard limit.
		 */
		if (!io_opaque_charge(stream->store, io_opaque_extent_bytes(), false))
			return -ENOBUFS;
		extent = kmalloc_obj(*extent, GFP_KERNEL_ACCOUNT);
		if (!extent) {
			io_opaque_uncharge(stream->store, io_opaque_extent_bytes());
			return -ENOMEM;
		}
		atomic_long_inc(&stream->store->extents);
		*extent = (struct io_opaque_extent) {
			.backing = tail->backing, .start = start,
			.offset = offset, .length = length,
		};
		refcount_inc(&extent->backing->refs);
	} else {
		u64 left = op ? ALIGN(op->length - op->progress - length, PAGE_SIZE) : 0;
		bool bound = !!stream->store->maio;
		unsigned int flags = io_opaque_hold_flags(hold);

		/* Leave room for dense copies of a private KEEP's remaining bytes. */
		if (!copied && op && op->data->charge + stream->need_bytes + left >
		    io_opaque_rx_limit(stream->store))
			return -ENOBUFS;
		stream->need_bytes = io_opaque_need(stream, stream->need_bytes);
		extent = io_opaque_stream_extent_new(stream, page, offset, length, start, flags);
		if (IS_ERR(extent))
			return PTR_ERR(extent);
		extent->backing->copied = copied;
		/* A bound store copies every byte outside the area into it. */
		extent->backing->area = hold == IO_OPAQUE_HOLD_AREA ||
					hold == IO_OPAQUE_HOLD_POOL || (copied && bound);
		extent->backing->ref = kind;
		if (defer) {
			/* The reference is the skb's until the pass has consumed the skb. */
			extent->backing->ref = IO_OPAQUE_REF_PENDING;
			extent->backing->want = kind;
			extent->backing->units = 0;
		} else if (!copied) {
			io_opaque_ref_take(stream->store, extent->backing);
			io_opaque_count(IO_OPAQUE_STAT_PAGES_HELD, 1);
		} else {
			io_opaque_count(IO_OPAQUE_STAT_PAGES_COPIED, 1);
		}
		new_backing = true;
	}
	list_add_tail(&extent->list, list);
accounted:
	if (defer)
		io_opaque_note(stream, extent->backing, src);
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

/*
 * @base is where @skb begins in the skb that TCP handed to the actor, which is
 * not 0 for a child on its frag_list.
 */
static int io_opaque_capture_part(struct io_opaque_stream *stream, struct sk_buff *skb,
				  u32 offset, u32 len, bool shared, u32 base)
{
	u32 consumed = 0, head = skb_headlen(skb), start = head;
	bool copy_frags;
	struct sk_buff *child;
	struct io_opaque_src src = { .skb = skb };
	u64 retain_limit = io_opaque_rx_limit(stream->store);
	enum io_opaque_hold hold;
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
			if (page_size(compound_head(page)) > retain_limit ||
			    !io_opaque_retain(stream->store, skb, page, &hold))
				goto copy;
			src.frag = -1;
			ret = io_opaque_append(stream, page, offset_in_page(data),
					       chunk, hold, &src);
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
		if (copy_frags || page_size(compound_head(skb_frag_page(frag))) > retain_limit ||
		    !io_opaque_retain(stream->store, skb, skb_frag_page(frag), &hold))
			goto copy;
		src.frag = i;
		src.from_entry = base + start >= stream->call_entry;
		ret = io_opaque_append(stream, skb_frag_page(frag),
				       skb_frag_off(frag) + offset - start, chunk, hold, &src);
		if (ret == -ENOBUFS)
			goto copy;
		goto head_done;
	}
	skb_walk_frags(skb, child) {
		if (offset >= start + child->len) {
			start += child->len;
			continue;
		}
		return io_opaque_capture_part(stream, child, offset - start,
					      min(start + child->len - offset, len),
					      copy_frags, base + start);
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
			stream->need_bytes = io_opaque_need(stream, PAGE_SIZE);
			if (!io_opaque_charge(stream->store, PAGE_SIZE, true))
				return -ENOBUFS;
			/*
			 * An empty area fails like a failed allocation. The
			 * collector then waits IO_OPAQUE_ALLOC_RETRY_MS and
			 * retries, and the stream and its captured prefix stay
			 * as they are.
			 */
			page = io_opaque_copy_page(stream->store);
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
			ret = io_opaque_append(stream, page, pos, chunk, IO_OPAQUE_HOLD_COPY, NULL);
		if (ret < 0 && allocated) {
			io_opaque_copy_page_put(stream->store, page);
			io_opaque_uncharge(stream->store, PAGE_SIZE);
		}
	}
head_done:
	if (ret > 0)
		consumed += ret;
	return consumed ?: ret;
}

/*
 * Capture up to @len bytes of @skb from @offset on, from one fragment, or from the
 * head, or copied. Outside a call of the actor the backings take the references
 * they need at once.
 */
VISIBLE_IF_KUNIT int io_opaque_capture(struct io_opaque_stream *stream, struct sk_buff *skb,
				       u32 offset, u32 len, bool shared)
{
	return io_opaque_capture_part(stream, skb, offset, len, shared, 0);
}

/*
 * The sk_read_actor_t of a collector pass. A call consumes the rest of @skb from
 * @offset on, unless a decision, a budget or a gate stops it. The batch of the
 * pass is soft: a call that begins with batch left finishes its skb, and the
 * pass ends after it with none, so that the call can take over the references to
 * the fragments of the skb. One that begins without any consumes nothing.
 *
 * While the call runs, the backings it creates hold nothing, because the
 * reference of the skb to the fragment is the one that they will hold, if the call
 * consumes the skb to its end. TCP frees the skb at once then, with its
 * fragments, and the call has taken them out of the skb before it returns: those
 * that a backing holds are no longer the skb's to drop, see io_opaque_settle_skb().
 * Otherwise the backings take references of their own.
 */
VISIBLE_IF_KUNIT int io_opaque_actor(read_descriptor_t *desc, struct sk_buff *skb,
				     unsigned int offset, size_t length)
{
	struct io_opaque_stream *stream = desc->arg.data;
	struct io_opaque_req *ready, *next;
	unsigned int entry = offset;
	LIST_HEAD(done);
	u32 consumed = 0;
	int ret = 0;

	length = min_t(u64, length, U64_MAX - stream->next);
	if (!desc->count)
		length = 0;
	/*
	 * A stream that attached has the room for the call. One without it, which
	 * only a test makes, takes the references at once.
	 */
	stream->in_call = !!stream->pending;
	stream->call_entry = entry;
	stream->nr_pending = 0;
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
		desc->count -= min_t(size_t, desc->count, ret);
		if (op && op->progress == op->length) {
			rb_erase_cached(&op->decision, &stream->decisions);
			RB_CLEAR_NODE(&op->decision);
			/* Reported below, when nothing the range holds is pending. */
			list_add_tail(&op->read, &done);
		}
	}
	io_opaque_settle_call(stream, skb, entry, consumed);
	/*
	 * The completion of a range is task work of the ring of its request. That
	 * can be another ring than the one that runs the pass, if the store is
	 * shared, and then it runs at once, on another CPU, and does not take the
	 * lock of the stream: it publishes the object, and the application may
	 * free it. The backings of the range must have their references by then.
	 */
	list_for_each_entry_safe(ready, next, &done, read) {
		list_del_init(&ready->read);
		io_opaque_queue_ready(ready, ready->length);
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
	/* No capture will follow: the chunks that are left in the block can go. */
	io_opaque_block_retire(stream);
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
	op = io_opaque_req_alloc(req, store);
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
	/* The room that a call of the actor remembers its backings in, see io_opaque_actor(). */
	stream->pending = kmalloc_objs(*stream->pending, IO_OPAQUE_PENDING_MAX,
				       GFP_KERNEL_ACCOUNT);
	if (!stream->pending) {
		kfree(stream);
		return -ENOMEM;
	}
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
	spin_lock(&store->tables);
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
	spin_unlock(&store->tables);
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
	struct io_opaque_data *data = op->data;

	guard(spinlock)(&store->tables);
	if (store->dead || READ_ONCE(op->canceled))
		return -ECANCELED;
	if (!slot->reserved ||
	    slot->generation != op->handle >> IO_OPAQUE_INDEX_BITS)
		return -ESTALE;
	/* The handle is set before the version is seen, and not changed again. */
	data->handle = op->handle;
	rcu_assign_pointer(slot->data, data);
	if (data->forward_only)
		*object_flags = IORING_OPAQUE_OBJECT_F_FORWARD_ONLY;
	if (data->mapped)
		*object_flags |= IORING_OPAQUE_OBJECT_F_MAPPED;
	store->nr_objects++;
	op->data = NULL;
	slot->reserved = false;
	op->slot_reserved = false;
	slot->born = jiffies;
	slot->retry = jiffies;
	if (!data->dense && !data->forward_only && !data->mapped &&
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
			io_opaque_vector(store, op->data) : -ENODATA;
		if (!ret)
			ret = io_opaque_publish(op, &object_flags);
	}
	if (op->op == IORING_OPAQUE_COMPACT) {
		if (ret >= 0)
			ret = io_opaque_compact_publish(op);
		spin_lock(&store->tables);
		{
			struct io_opaque_slot *slot = io_opaque_lookup(store, op->handle);

			if (slot)
				slot->compacting = false;
		}
		spin_unlock(&store->tables);
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

	if (!store)
		return -ENXIO;
	if (READ_ONCE(sqe->fd) != -1 || READ_ONCE(sqe->rw_flags) ||
	    READ_ONCE(sqe->buf_index) || READ_ONCE(sqe->__pad2[0]) ||
	    cmd > IORING_OPAQUE_OBJECT_EXTENTS ||
	    (req->flags & (REQ_F_CQE_SKIP | REQ_F_FIXED_FILE)))
		return -EINVAL;
	op = io_opaque_req_alloc(req, store);
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
	if (cmd == IORING_OPAQUE_OBJECT_EXTENTS) {
		/* A buffer of size 0 asks for the size that the vector needs. */
		if (op->length && !op->addr)
			return -EINVAL;
		if (!store->maio)
			return -EOPNOTSUPP;
	}
	return 0;
}

/* Resolve resources when links permit execution, never while preparing SQEs. */
static int io_opaque_resolve(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_data *data;
	struct io_opaque_slot *slot;
	u16 cmd = op->op;
	int ret = 0;

	if (op->resolved)
		return 0;
	spin_lock(&store->tables);
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
	} else if (cmd == IORING_OPAQUE_READ_OBJECT || cmd == IORING_OPAQUE_OBJECT_STAT ||
		   cmd == IORING_OPAQUE_OBJECT_EXTENTS) {
		slot = io_opaque_lookup(store, op->handle);
		data = slot ? io_opaque_slot_data(store, slot) : NULL;
		if (!data) {
			ret = -ESTALE;
		} else {
			op->data = data;
			refcount_inc(&data->refs);
			if (cmd == IORING_OPAQUE_READ_OBJECT &&
			    (op->offset > data->length || op->length > data->length - op->offset))
				ret = -ERANGE;
		}
	}
	spin_unlock(&store->tables);
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

/*
 * The pair that names the bytes of an extent in the area of the instance of
 * the store: the area offset of the page, plus the offset of the extent in the
 * page. An extent outside the area gets IORING_OPAQUE_EXTENT_UNMAPPED. Capture
 * copies every byte outside the area into it, so a captured object has none.
 * IORING_OPAQUE_CREATE copies into ordinary pages even in a bound store, so
 * all the extents of a created object are of this kind. The mark and the lookup
 * together keep the answer true for both.
 */
VISIBLE_IF_KUNIT struct io_uring_opaque_extent
io_opaque_extent_pair(struct io_opaque_store *store, const struct io_opaque_extent *extent)
{
	struct io_uring_opaque_extent pair = {
		.area_off = IORING_OPAQUE_EXTENT_UNMAPPED,
		.len = extent->length,
	};
	u64 page_off;

	if (extent->backing->area &&
	    io_zcrx_maio_page_off(store->maio, extent->backing->page, &page_off))
		pair.area_off = page_off + extent->offset;
	return pair;
}

/*
 * Write the extents of the object of @op to the user buffer, and return the
 * number of bytes. *@needed is the size of the whole vector. The list of
 * extents does not change after the object is published, and nr counts it.
 */
static int io_opaque_extents(struct io_opaque_req *op, u64 *needed)
{
	struct io_opaque_extent *extent;
	u64 done = 0;

	*needed = (u64)op->data->nr * sizeof(struct io_uring_opaque_extent);
	if (op->length < *needed)
		return -EOVERFLOW;
	list_for_each_entry(extent, &op->data->extents, list) {
		struct io_uring_opaque_extent pair = io_opaque_extent_pair(op->store, extent);

		/* Never write past the vector that the buffer was checked for. */
		if (WARN_ON_ONCE(done == *needed))
			break;
		if (copy_to_user(op->addr + done, &pair, sizeof(pair)))
			return -EFAULT;
		done += sizeof(pair);
	}
	return done;
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
		/* The uapi field is 32 bits; the count of records is not bounded by it. */
		.extents = min_t(unsigned long, atomic_long_read(&store->extents), U32_MAX),
		.framing_bytes = atomic64_read(&store->framing),
		.framing_copied_bytes = atomic64_read(&store->framing_copied),
		.memory_mode = io_opaque_memory_mode(store),
	};

	/* Framing in the reply reserve is charged outside bytes. */
	stat.backing_bytes += atomic64_read(&store->reserve_framing);
	spin_lock(&store->tables);
	stat.objects = store->nr_objects;
	stat.streams = store->nr_streams;
	spin_unlock(&store->tables);
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
		extent = io_opaque_extent_new(store, page, 0, chunk, done,
					      IO_OPAQUE_EXT_RESERVED);
		if (IS_ERR(extent)) {
			io_opaque_unit_put(store, page);
			ret = PTR_ERR(extent);
			goto out;
		}
		op->create_reserved -= page_size(page);
		extent->backing->copied = true;
		extent->backing->ref = IO_OPAQUE_REF_UNIT;
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
	ret = io_opaque_vector(store, op->data);
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
	u64 needed = 0;
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
		spin_lock(&store->tables);
		{
			struct io_opaque_data *data = io_opaque_detach(store,
						io_opaque_lookup(store, op->handle));

			ret = data ? 0 : -ESTALE;
			spin_unlock(&store->tables);
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
			if (op->data->mapped)
				stat.flags |= IORING_OPAQUE_OBJECT_F_MAPPED;
			spin_lock(&store->tables);
			slot = io_opaque_lookup(store, op->handle);
			if (slot && slot->compacting)
				stat.flags |= IORING_OPAQUE_OBJECT_F_COMPACTING;
			spin_unlock(&store->tables);
			ret = io_opaque_stat_copy(op, &stat, sizeof(stat));
		}
		break;
	case IORING_OPAQUE_OBJECT_EXTENTS:
		ret = io_opaque_extents(op, &needed);
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
		spin_lock(&store->tables);
		if (store->streams[io_opaque_index(op->handle)].stream == op->stream) {
			store->streams[io_opaque_index(op->handle)].stream = NULL;
			store->nr_streams--;
			io_opaque_release_slot(store->stream_used, io_opaque_index(op->handle),
					       op->handle >> IO_OPAQUE_INDEX_BITS);
			spin_unlock(&store->tables);
			io_opaque_stream_close(op->stream);
			io_opaque_stream_put(op->stream);
			ret = 0;
		} else {
			spin_unlock(&store->tables);
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
		/* An EXTENTS buffer that is too small learns the size it needs. */
		if (ret == -EOVERFLOW && op->op == IORING_OPAQUE_OBJECT_EXTENTS)
			io_req_set_res32(req, ret, 0, needed, 0);
		else
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
	 * request's file and stream after its entry has been unlinked.
	 */
	scoped_guard(spinlock, &store->wait_lock)
		list_del_init(&op->wait);
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
		spin_lock(&store->tables);
		store->objects[op->slot].reserved = false;
		io_opaque_release_slot(store->object_used, op->slot,
				       store->objects[op->slot].generation);
		spin_unlock(&store->tables);
	}
	if (unlikely(op->vec))
		io_opaque_vec_cleanup(op);
	if (op->op == IORING_OPAQUE_CREATE)
		io_opaque_create_release(op);
	else if (op->data)
		io_opaque_data_put(store, op->data);
	if (op->op == IORING_OPAQUE_COMPACT) {
		io_opaque_data_put(store, op->replacement);
		io_opaque_compact_end(op);
	}
	if (op->stream)
		io_opaque_stream_put(op->stream);
	lockdep_assert_held(&req->ctx->uring_lock);
	io_opaque_req_free(req->ctx, op);
	io_req_async_data_clear(req, 0);
}

/*
 * Request cleanup of a send, which holds only what a send can hold: a place in
 * a queue, a frame and its charge, references (a version, and those of a vector in
 * its block), and the block. A send that ended with its socket owned has given
 * back all but the frame and the decision about the block (io_opaque_send()); any
 * other has all of them to give back here, once, and each is given back only if it
 * is there: a call that is made to find that out is a call for nothing. A vector
 * whose admission failed, with the socket owned or not, is one of those: it left the
 * references it had taken for this (io_opaque_vec_acquire()). So is a frame that a queue
 * which failed refused after it had taken its references (io_opaque_tx_settle()).
 *
 * A send is on neither list that a waker walks under wait_lock, budget_waits and
 * copies, so it has no waker to wait out, and takes the lock only if its entry is
 * linked. What reaches a send from outside comes through its TX queue, which
 * io_opaque_tx_put() waits for, or from cancellation, which holds the ring lock as
 * cleanup does. Its stream is none, and it reserves no slot.
 */
void io_opaque_send_cleanup(struct io_kiocb *req)
{
	struct io_opaque_req *op = req->async_data;
	struct io_opaque_store *store;

	if (!op)
		return;
	store = op->store;
	if (!list_empty(&op->cancel))
		list_del_init(&op->cancel);
	if (!list_empty(&op->wait)) {
		scoped_guard(spinlock, &store->wait_lock)
			list_del_init(&op->wait);
	}
	if (op->tx)
		io_opaque_tx_put(op);
	if (op->framing)
		kfree(op->framing);
	if (op->frame_charge)
		io_opaque_uncharge_frame(store, op->frame_charge, op->frame_reserved);
	if (unlikely(op->vec))
		io_opaque_vec_cleanup(op);
	if (op->data)
		io_opaque_data_put(store, op->data);
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
/* Calls of io_opaque_vec_seek(), for a test to read. */
atomic_long_t io_opaque_vec_seeks = ATOMIC_LONG_INIT(0);
#define IO_OPAQUE_VEC_SEEKED()		atomic_long_inc(&io_opaque_vec_seeks)
/* Segments that admission walked over to find where ranges start, for a test to read. */
atomic_long_t io_opaque_vec_stepped = ATOMIC_LONG_INIT(0);
#define IO_OPAQUE_VEC_STEPPED(n)	atomic_long_add((n), &io_opaque_vec_stepped)
/* Segments that admission counted in the ranges, for a test to read. */
atomic_long_t io_opaque_vec_counted = ATOMIC_LONG_INIT(0);
#define IO_OPAQUE_VEC_COUNTED(n)	atomic_long_add((n), &io_opaque_vec_counted)
#else
#define IO_OPAQUE_VEC_SEGS_LIMIT	IO_OPAQUE_SEND_VEC_SEGS
#define IO_OPAQUE_VEC_SEEKED()		((void)0)
#define IO_OPAQUE_VEC_STEPPED(n)	((void)(n))
#define IO_OPAQUE_VEC_COUNTED(n)	((void)(n))
#endif

/* Whether the request's block, if it has one, holds @nr entries and @segs segments already. */
static inline bool io_opaque_vec_fits(const struct io_opaque_vec *vec, u32 nr, u32 segs)
{
	return vec && vec->cap_nr >= nr && vec->cap_segs >= segs;
}

/*
 * Make room in the request's block for @nr entries and @segs segments, keeping its first @keep
 * entries and the references that admission took on them: the new block holds the versions
 * that the old one did, and admission walks the entries again over them (see
 * io_opaque_vec_acquire()). A reference on an entry that is not kept would be lost, which is
 * what the warning says. A block that is big enough stays as it is, and a caller that can tell
 * (io_opaque_vec_fits()) does not call. This allocates, so tables must not be held.
 */
VISIBLE_IF_KUNIT int io_opaque_vec_grow(struct io_opaque_req *op, u32 nr, u32 segs, u32 keep)
{
	struct io_opaque_vec *old = op->vec, *vec;

	KUNIT_STATIC_STUB_REDIRECT(io_opaque_vec_grow, op, nr, segs, keep);
	lockdep_assert_not_held(&op->store->tables);
	if (io_opaque_vec_fits(old, nr, segs))
		return 0;
	WARN_ON_ONCE(old && old->pinned > keep);
	if (old) {
		nr = max(nr, old->cap_nr);
		segs = max(segs, old->cap_segs);
	}
	vec = kvmalloc(size_add(sizeof(*vec),
				size_add(array_size(nr, sizeof(*vec->ent) + sizeof(*vec->pin)),
					 array_size(segs, sizeof(*vec->bv)))),
		       GFP_KERNEL_ACCOUNT);
	if (!vec)
		return -ENOMEM;
	vec->cap_nr = nr;
	vec->cap_segs = segs;
	vec->ent = (void *)(vec + 1);
	vec->pin = (void *)(vec->ent + nr);
	vec->bv = (void *)(vec->pin + nr);
	vec->nr_segs = 0;
	vec->pinned = 0;
	if (old) {
		vec->nr = old->nr;
		vec->max_len = old->max_len;
		memcpy(vec->ent, old->ent, array_size(keep, sizeof(*vec->ent)));
		vec->pinned = min(old->pinned, keep);
		memcpy(vec->pin, old->pin, array_size(vec->pinned, sizeof(*vec->pin)));
		kvfree(old);
	} else {
		vec->nr = 0;
		vec->max_len = 0;
	}
	op->vec = vec;
	return 0;
}

/*
 * The @nr entries are in the block and add up to @total bytes, the largest being
 * @longest: make the request what its length and offset say about a send, a body
 * of their total at no offset.
 */
VISIBLE_IF_KUNIT void io_opaque_vec_commit(struct io_opaque_req *op, u32 nr, u32 total,
					   u32 longest)
{
	struct io_opaque_vec *vec = op->vec;

	vec->nr = nr;
	vec->nr_segs = 0;
	vec->pinned = 0;
	vec->max_len = longest;
	op->handle = 0;
	op->offset = 0;
	op->length = total;
}

/*
 * Read the @nr entries at @uent into the request's block and check them as they
 * are read, in the one pass that SEND_ZC makes over its iovecs
 * (copy_iovec_from_user()). A copy_from_user() would also check the block as a
 * heap object that the copy must not overrun, and a pass over the copy would
 * check the entries. Each field is read from user memory once and what is checked
 * is what is kept, so an array that changes meanwhile cannot make the block
 * differ from what was checked. @nr is from one to IORING_OPAQUE_SEND_VEC_MAX,
 * and the block holds that many.
 *
 * An entry must name a range: no reserved bits, at least a byte, and an end that
 * does not pass U64_MAX. The lengths add up to the body, which with the frame
 * must fit an int. A handle may repeat, and a range may too. All of the entries
 * are read before one that is not valid is refused, so that a fault is -EFAULT
 * whatever they hold, as it is when the copy comes first. Returns 0, with the
 * request made of the vector by io_opaque_vec_commit(), or an error, with the
 * block as far as it was read.
 */
VISIBLE_IF_KUNIT int io_opaque_vec_copy_in(struct io_opaque_req *op,
					   const struct io_uring_opaque_send_entry __user *uent,
					   u32 nr)
{
	struct io_uring_opaque_send_entry *ent = op->vec->ent;
	u32 longest = 0, i;
	bool bad = false;
	u64 sum = 0, end;

	BUILD_BUG_ON(sizeof(*ent) != 24);
	scoped_user_read_access_size(uent, array_size(nr, sizeof(*uent)), efault) {
		for (i = 0; i < nr; i++) {
			unsafe_get_user(ent[i].handle, &uent[i].handle, efault);
			unsafe_get_user(ent[i].off, &uent[i].off, efault);
			unsafe_get_user(ent[i].len, &uent[i].len, efault);
			unsafe_get_user(ent[i].__resv, &uent[i].__resv, efault);
			bad |= !!ent[i].__resv | !ent[i].len |
			       check_add_overflow(ent[i].off, (u64)ent[i].len, &end);
			sum += ent[i].len;
			longest = max(longest, ent[i].len);
		}
	}
	/* The sum only grows, and a thousand lengths of 32 bits cannot overflow it. */
	if (bad || sum > INT_MAX)
		return -EINVAL;
	io_opaque_vec_commit(op, nr, sum, longest);
	return 0;
efault:
	return -EFAULT;
}

/*
 * Whether entry @i of an admitted vector holds a reference of its own. A LAST
 * vector takes one per entry. Any other takes one per run of entries that name
 * the same handle: a run is one version, the one that its first entry pinned,
 * and the rest of it uses that pin and does not look the handle up again, for
 * the slot may hold another version by then. This looks at the entries, which
 * never change, and not at the pins, which may be freed by the time the run's
 * owner has been released.
 */
static bool io_opaque_vec_owns(const struct io_opaque_req *op, u32 i)
{
	const struct io_opaque_vec *vec = op->vec;

	return (op->send_flags & IORING_OPAQUE_SEND_LAST) || !i ||
	       vec->ent[i].handle != vec->ent[i - 1].handle;
}

/*
 * Drop every version reference the send holds; one that took none drops none. This puts the
 * last reference of a version if it is the send's, and it is for a context that does not own the
 * socket: the end of a send, after the socket is released, and request cleanup. That includes
 * the references that a failed admission leaves in the block (io_opaque_vec_acquire()).
 */
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

	IO_OPAQUE_VEC_SEEKED();
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
 * starts inside its extent, the last ends inside its own. All of them are
 * counted and the first @fit stored at @bv, since a caller whose block is too
 * short has to learn how many segments it needs, and the ones stored are of no
 * use to it then. The count stops at @room + 1, since a caller that has room
 * for no more has no use for how many more there are. The range must lie
 * within the object.
 */
static u32 io_opaque_vec_span(const struct io_opaque_data *data, u32 seg, u32 skip, u32 len,
			      struct bio_vec *bv, u32 fit, u64 room)
{
	const struct bio_vec *src = data->bvec;
	u32 n = 0;

	while (len && seg < data->nr) {
		u32 take = min(src[seg].bv_len - skip, len);

		if (n < fit) {
			bv[n] = src[seg];
			bv[n].bv_offset += skip;
			bv[n].bv_len = take;
		}
		n++;
		if (n > room)
			break;
		len -= take;
		skip = 0;
		seg++;
	}
	return n;
}

/* The range must lie within the version. */
static int io_opaque_send_range(const struct io_opaque_data *data, u64 off, u32 len)
{
	if (off > data->length || len > data->length - off)
		return -ERANGE;
	return 0;
}

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * Run by io_opaque_pin() before it reads the slot, with no lock held and not in RCU, which is
 * after its caller has looked at store->dead: for a test to stop the store, or to hold the
 * lookup until it is stopped. It may sleep.
 */
void (*io_opaque_lookup_before)(struct io_opaque_store *store);
#define IO_OPAQUE_LOOKUP_BEFORE(store)				\
	do {							\
		if (io_opaque_lookup_before)			\
			io_opaque_lookup_before(store);		\
	} while (0)
/*
 * Run by io_opaque_pin() in the RCU section, after it has read the version from its slot and
 * before it takes a reference, for a test to change what the slot holds there.
 */
void (*io_opaque_lookup_between)(struct io_opaque_store *store);
#define IO_OPAQUE_LOOKUP_BETWEEN(store)				\
	do {							\
		if (io_opaque_lookup_between)			\
			io_opaque_lookup_between(store);	\
	} while (0)
/*
 * Run by io_opaque_pin() when it has the reference and returns it, with no lock held and not
 * in RCU: a lookup that returns an error has not got here. For a test to change what the
 * other holders of the version do, and to see that a lookup that fails never held it.
 */
void (*io_opaque_lookup_after)(struct io_opaque_store *store);
#define IO_OPAQUE_LOOKUP_AFTER(store)				\
	do {							\
		if (io_opaque_lookup_after)			\
			io_opaque_lookup_after(store);		\
	} while (0)
/* Lookups that the table settled, with tables held: those that found nothing to take in RCU. */
atomic_long_t io_opaque_pin_slow = ATOMIC_LONG_INIT(0);
#define IO_OPAQUE_PIN_SLOW()		atomic_long_inc(&io_opaque_pin_slow)
#else
#define IO_OPAQUE_LOOKUP_BEFORE(store)	((void)(store))
#define IO_OPAQUE_LOOKUP_BETWEEN(store)	((void)(store))
#define IO_OPAQUE_LOOKUP_AFTER(store)	((void)(store))
#define IO_OPAQUE_PIN_SLOW()		((void)0)
#endif

/*
 * The lookup of io_opaque_pin() that is settled with tables held, where nothing changes
 * meanwhile: the slot was empty, or held a version of another handle, or a version that
 * everyone has put since it was read. The handle names another version now, a replacement of the
 * one that was read for example, or none. A store that has stopped is looked at first. It stops
 * by setting dead with tables held and detaching its objects afterwards, so that a handle that
 * was there when the send began can be gone by now, and is not stale: the send is ordered after
 * the stop, and the answer it gets is the store's, as it is for a send that begins after it.
 * Like the lookup in RCU, this takes a reference or returns an error and holds none.
 */
static struct io_opaque_data *io_opaque_pin_locked(struct io_opaque_store *store, u64 handle,
						   u64 off, u32 len)
{
	struct io_opaque_slot *slot;
	struct io_opaque_data *data;
	int ret;

	IO_OPAQUE_PIN_SLOW();
	guard(spinlock)(&store->tables);
	if (store->dead)
		return ERR_PTR(-ESHUTDOWN);
	slot = io_opaque_lookup(store, handle);
	data = slot ? io_opaque_slot_data(store, slot) : NULL;
	if (!data)
		return ERR_PTR(-ESTALE);
	ret = io_opaque_send_range(data, off, len);
	if (ret)
		return ERR_PTR(ret);
	refcount_inc(&data->refs);
	return data;
}

/*
 * Take a reference on the version that @handle names, for a send that does not consume it and
 * wants @len bytes of it from @off, without taking tables. The slot is read in RCU, and the
 * count raised if it is not zero. What is read is the version that the slot held. A FREE, a LAST
 * send or a compaction that runs at the same time has either not changed it yet, and then the
 * send is ordered before it, or has: the version has left the table by then, but it is still the
 * one that the handle named when it was read, and sending it is what a send that came first
 * would have done.
 *
 * The version must be the handle's, and have the range, and both are settled in the RCU
 * section, before the reference is taken. The version knows the handle it was published for and
 * its length, both set before it was and never changed, and its struct is there until a grace
 * period after the last put (io_opaque_data_put()). So a lookup returns a version with a
 * reference that the send keeps, or an error and no reference at all, and a send never has to
 * give back what it took. That matters for the last reference of a version, which a send that
 * owns the socket must not put (a free is not for the socket lock to wait out, see
 * io_opaque_send_unpin_cheap()): admission runs with the socket, and the version may have left
 * the table since the reference was taken, so that giving it back would be the last put.
 *
 * Whatever does not give a version to take is asked of the table with tables held, where the
 * answer is the one it always was (io_opaque_pin_locked()): an empty slot, a version of another
 * handle (the slot was freed and taken by another object), and a count of zero, which says that
 * the version has left the table and that everyone has put it. Its extents and segments are
 * freed and only the struct is left, so that the count, the handle and the length can still be
 * read. A store that has stopped is answered there too. The caller has looked at store->dead,
 * and the store may stop after that and before the slot is read: it sets dead under tables and
 * detaches its objects later, and the lookup that finds one detached is told so. A range outside
 * the version that is the handle's is the answer without tables.
 */
static struct io_opaque_data *io_opaque_pin(struct io_opaque_store *store, u64 handle,
					    u64 off, u32 len)
{
	u32 index = io_opaque_index(handle);
	struct io_opaque_data *data;
	bool taken = false;
	int ret = 0;

	if (!handle || index >= store->config.max_objects)
		return ERR_PTR(-ESTALE);
	IO_OPAQUE_LOOKUP_BEFORE(store);
	rcu_read_lock();
	data = rcu_dereference(store->objects[index].data);
	IO_OPAQUE_LOOKUP_BETWEEN(store);
	if (data && data->handle == handle) {
		ret = io_opaque_send_range(data, off, len);
		if (!ret)
			taken = refcount_inc_not_zero(&data->refs);
	}
	rcu_read_unlock();
	if (ret)
		return ERR_PTR(ret);
	if (unlikely(!taken)) {
		data = io_opaque_pin_locked(store, handle, off, len);
		if (IS_ERR(data))
			return data;
	}
	IO_OPAQUE_LOOKUP_AFTER(store);
	return data;
}

/*
 * Drop the references a send took at admission; a no-op when it took none. The last of a version
 * is put here, so this is for the end of a send once its socket is released, and nothing that
 * has the socket calls it.
 */
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
 * the lock to wait out. A version whose count is one has left the table, but
 * a lookup in RCU that read it before it did can still take a reference on it
 * (io_opaque_pin()). Then the one put after the socket is not the last. The
 * last is put by the send of that lookup, when it gives its references back:
 * after it has released its socket, if it ends owning it as this one does, or
 * in its cleanup, since a lookup puts nothing, a single range that fails holds
 * no reference, and a vector that fails leaves what it pinned for its cleanup
 * (io_opaque_vec_acquire()). So does a frame that joins a queue which fails
 * meanwhile: it is refused with its references (io_opaque_tx_settle()), and
 * its cleanup puts them. A decrement never takes a count from one to zero. A
 * vector's entries that were put are cleared, so that the rest cannot put them
 * again. A send that consumed its object does not call it (see io_opaque_send()).
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
 * Admission of a vectored send. Pass one resolves the entries, stores the segments of their
 * ranges and counts them, and fails having removed nothing from the table, so a vector that
 * cannot go consumes nothing. It finds where each range starts by walking the segments of its
 * version, and an entry that follows another in the same version, not before the segment that
 * one started in, walks on from there instead of from the first: ranges in ascending order walk
 * each object once, and a range that starts in the segment the walk is at, as every range of an
 * object of one segment does, is found without a call. One that is more than a stride from where
 * the walk is jumps to the version's anchors (see io_opaque_vec_seek()), so that ranges that go
 * back or alternate between large objects cost a few dozen steps each, not an object. The
 * segments of each range go to the block as the range is found, in the order of the ranges,
 * which is the cursor over the bytes of the send, and are counted whether the block has room for
 * them or not. Counting stops as soon as the limit is passed. What admission does is therefore
 * bounded by the entries and the limit, whatever the ranges. A block with too few segments is
 * not grown here, as that allocates; the number needed is returned, and io_opaque_vec_finish()
 * grows the block and asks again.
 *
 * A vector that consumes what it names (LAST) runs under tables, which
 * io_opaque_send_acquire() takes for it. Pass one pins nothing there, so every entry sees the
 * versions as they are in that one hold, and pass two, which nothing can fail, detaches each
 * handle at its first entry; an entry that repeats the handle takes one more reference on the
 * version it found. Nothing is held when it returns an error or asks for a bigger block.
 *
 * Any other vector does not take tables and has no second pass. It pins each version as pass
 * one comes to it (io_opaque_pin()), for the walk reads the segments of the version and they
 * must not be freed under it, and it pins once for each run of entries that name the same
 * handle (see io_opaque_vec_owns()): eight ranges of one object cost one reference. The
 * entries after the first of a run use its pin and do not look at the table. A handle that
 * returns after another one pins again, and may then find a replacement that a compaction has
 * published meanwhile, which has the same bytes.
 *
 * What it pinned stays in the block, whatever it returns, and is not given back here: admission
 * may own the socket, where the last reference of a version is not to be put, and a version
 * that is freed meanwhile makes the pin of an entry the last. A vector that fails is left with
 * its references for the cleanup of the request, which gives them back with the socket
 * released and the ring locked (io_opaque_vec_release()). One that needs a bigger block keeps
 * them across io_opaque_vec_grow(), and pass one is made again over the same versions: an entry
 * that is pinned already (@vec->pinned) is not looked up again, so the vector goes on with the
 * versions it began with, and the block is the only thing that is made again.
 */
static int io_opaque_vec_acquire(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	bool last = op->send_flags & IORING_OPAQUE_SEND_LAST;
	struct io_opaque_vec *vec = op->vec;
	const struct io_opaque_data *prev = NULL;
	struct io_opaque_data *data = NULL;
	u64 segs = 0, pos = 0;
	u32 i, seg = 0;
	int ret;

	if (READ_ONCE(store->dead))
		return -ESHUTDOWN;
	for (i = 0; i < vec->nr; i++) {
		const struct io_uring_opaque_send_entry *ent = &vec->ent[i];
		u32 skip, n, stored;

		if (last) {
			struct io_opaque_slot *slot = io_opaque_lookup(store, ent->handle);

			data = slot ? io_opaque_slot_data(store, slot) : NULL;
			if (!data)
				return -ESTALE;
		} else if (i < vec->pinned) {
			/* Pinned before the block grew. */
			data = vec->pin[i];
		} else if (io_opaque_vec_owns(op, i)) {
			data = io_opaque_pin(store, ent->handle, ent->off, ent->len);
			if (IS_ERR(data))
				return PTR_ERR(data);
		}
		/* A LAST vector takes the reference in pass two, and a repeated handle needs it. */
		vec->pin[i] = data;
		if (!last)
			vec->pinned = max(vec->pinned, i + 1);
		ret = io_opaque_send_range(data, ent->off, ent->len);
		if (ret)
			return ret;
		if (data != prev || ent->off < pos) {
			prev = data;
			seg = 0;
			pos = 0;
		}
		/* Usually the walk is at the range already, and there is nothing to call for. */
		if (seg < data->nr && ent->off - pos < data->bvec[seg].bv_len)
			skip = ent->off - pos;
		else
			skip = io_opaque_vec_seek(data, ent->off, &seg, &pos);
		/* What the block holds so far; once it is full the rest is only counted. */
		stored = min_t(u64, segs, vec->cap_segs);
		n = io_opaque_vec_span(data, seg, skip, ent->len, vec->bv + stored,
				       vec->cap_segs - stored, IO_OPAQUE_VEC_SEGS_LIMIT - segs);
		IO_OPAQUE_VEC_COUNTED(n);
		segs += n;
		if (segs > IO_OPAQUE_VEC_SEGS_LIMIT)
			return -EMSGSIZE;
	}
	/* Within the limit, so it fits an int. */
	if (segs > vec->cap_segs)
		return segs;
	vec->nr_segs = segs;
	iov_iter_bvec(&op->iter, ITER_SOURCE, vec->bv, segs, op->length);
	if (!last)
		return 0;
	for (i = 0; i < vec->nr; i++) {
		struct io_opaque_slot *slot = io_opaque_lookup(store, vec->ent[i].handle);

		if (!io_opaque_slot_data(store, slot))
			refcount_inc(&vec->pin[i]->refs);
		else
			vec->pin[i] = io_opaque_detach(store, slot);
	}
	vec->pinned = vec->nr;
	op->consumed = true;
	return 0;
}

/*
 * The cursor of a single send over the range it names in its version, which it holds a
 * reference on, found as a vector's ranges are.
 */
static void io_opaque_send_cursor(struct io_opaque_req *op)
{
	const struct io_opaque_data *data = op->data;
	u64 pos = 0;
	u32 seg = 0, skip;

	skip = io_opaque_vec_seek(data, op->offset, &seg, &pos);
	io_opaque_send_iter(op, data->bvec + seg, data->nr - seg, data->length - pos, skip);
}

/*
 * A send of one range. A LAST send takes the table's reference, which also removes the handle,
 * under tables; any other sends the version that the handle names now, and takes a reference
 * of its own without tables (io_opaque_pin()), which has checked the range by then and holds
 * nothing if it refuses it. The cursor is made once the version is held.
 */
static int io_opaque_send_acquire_one(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_data *data;
	int ret;

	/*
	 * A send that consumes what it names looks under tables, and has the answer. Any other
	 * is told again by its lookup, if it finds no version to take (io_opaque_pin()).
	 */
	if (READ_ONCE(store->dead))
		return -ESHUTDOWN;
	if (!op->length)
		return 0;
	if (op->send_flags & IORING_OPAQUE_SEND_LAST) {
		struct io_opaque_slot *slot = io_opaque_lookup(store, op->handle);

		data = slot ? io_opaque_slot_data(store, slot) : NULL;
		if (!data)
			return -ESTALE;
		ret = io_opaque_send_range(data, op->offset, op->length);
		if (ret)
			return ret;
		op->data = io_opaque_detach(store, slot);
		op->consumed = true;
	} else {
		data = io_opaque_pin(store, op->handle, op->offset, op->length);
		if (IS_ERR(data))
			return PTR_ERR(data);
		op->data = data;
	}
	io_opaque_send_cursor(op);
	return 0;
}

/* The references of a send, as io_opaque_send_acquire() takes them, with tables if it needs it. */
static int io_opaque_acquire(struct io_opaque_req *op)
{
	if (unlikely(op->send_flags & IORING_OPAQUE_SEND_VECTOR))
		return io_opaque_vec_acquire(op);
	return io_opaque_send_acquire_one(op);
}

/*
 * Resolve what a send names and take its references, and make its cursor: one range gets it
 * over the segments of its version, a vector over the block's. A send that consumes what it
 * names (LAST) does that under tables, which is taken here, as removing the handle is a write
 * of the table; any other does not take tables (io_opaque_pin()). A single range returns 0 with
 * its reference taken, or a negative errno with none. A vector returns 0 with its references
 * taken, a negative errno with nothing removed, or the number of segments its ranges span when
 * its block holds fewer, to be settled by io_opaque_vec_finish(). It keeps the references that it
 * has taken in the last two cases and does not give them back (io_opaque_vec_acquire()): this
 * may run with the socket owned, and the request gives them back when it is cleaned up.
 */
VISIBLE_IF_KUNIT int io_opaque_send_acquire(struct io_opaque_req *op)
{
	KUNIT_STATIC_STUB_REDIRECT(io_opaque_send_acquire, op);
	if (!(op->send_flags & IORING_OPAQUE_SEND_LAST))
		return io_opaque_acquire(op);
	guard(spinlock)(&op->store->tables);
	return io_opaque_acquire(op);
}

/*
 * What admission of a vector does after io_opaque_send_acquire() has returned @ret. A block with
 * too few segments (@ret of them are needed) is grown here, where allocating is fine, and the
 * vector is resolved again. A vector that consumes what it names holds nothing between the
 * attempts, the versions may have changed meanwhile, so every entry is looked at again, and the
 * block may still be short. Any other keeps the references of the first attempt, and the second
 * walks the entries over the versions they pinned, in a block that holds all the segments they
 * span. A vector that goes at the first attempt is counted as a fast admission, and one that
 * needed the block grown, however often, as a slow one.
 */
VISIBLE_IF_KUNIT int io_opaque_vec_finish(struct io_opaque_req *op, int ret)
{
	bool slow = ret > 0;

	while (ret > 0) {
		ret = io_opaque_vec_grow(op, op->vec->nr, ret, op->vec->nr);
		if (ret)
			return ret;
		ret = io_opaque_send_acquire(op);
	}
	if (!ret)
		io_opaque_count(slow ? IO_OPAQUE_STAT_SEND_ADMIT_SLOW :
				       IO_OPAQUE_STAT_SEND_ADMIT_FAST, 1);
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
	/* A request that is made ready for a vector holds no reference in its block. */
	WARN_ON_ONCE(op->vec && op->vec->pinned);
	/* Every entry spans a segment; admission grows the block if some span more. */
	if (!io_opaque_vec_fits(op->vec, nr, nr)) {
		ret = io_opaque_vec_grow(op, nr, nr, 0);
		if (ret)
			return ret;
	}
	return io_opaque_vec_copy_in(op, u64_to_user_ptr(op->handle), nr);
}

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * Fill the request of a send with the poison of the test (io_opaque_send_poison)
 * before the send sets what it reads, as the message and the msg_ubuf are filled
 * in io_opaque_send(), so that a field that is read before it is written does not
 * get away with what the last request left, or with zero. Not the block of a
 * vector, which a request brings along.
 */
#define IO_OPAQUE_SEND_POISON_REQ(op)					\
	do {								\
		if (io_opaque_send_poison >= 0)				\
			memset((op), io_opaque_send_poison,		\
			       offsetof(struct io_opaque_req, vec));	\
	} while (0)
#else
#define IO_OPAQUE_SEND_POISON_REQ(op)	((void)(op))
#endif

/*
 * A send sets the members of its request that it reads before it writes them,
 * and no more: the request is shared with the receives and the control operations,
 * whose members it never looks at, and a recycled one is what the last request
 * left, apart from the block of a vector that it keeps. What a send writes
 * before it reads is left alone: iter (admission), send (the join, which links
 * it into the queue), frame_reserved (the charge that sets frame_charge) and
 * result (the wakeup that sets it). The stream is none because cancellation looks
 * at it for every request that waits, and the total is none until the request is
 * resolved, by this prep or by the import of its frame.
 */
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
	op = io_opaque_req_get(req, store);
	if (IS_ERR(op))
		return PTR_ERR(op);
	IO_OPAQUE_SEND_POISON_REQ(op);
	op->req = req;
	op->store = store;
	op->stream = NULL;
	op->data = NULL;
	INIT_LIST_HEAD(&op->wait);
	INIT_LIST_HEAD(&op->cancel);
	op->progress = 0;
	op->op = U16_MAX;
	op->phase = IO_OPAQUE_IDLE;
	op->canceled = false;
	op->consumed = false;
	op->resolved = false;
	op->tx = NULL;
	op->framing = NULL;
	op->frame_charge = 0;
	op->prefix_length = 0;
	op->suffix_length = 0;
	op->total_length = 0;
	op->tx_started = false;
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
 * Resolve what @op names and take its references, and make its cursor. A send that consumes
 * what it names does it under tables, which io_opaque_send_acquire() takes; any other takes no
 * lock. A vector whose block is too small grows it and is resolved again. A send that fails
 * gives nothing back here, and the socket may be owned: what a vector took before it failed is
 * left to the cleanup of the request.
 */
static int io_opaque_tx_acquire(struct io_opaque_req *op)
{
	int ret = io_opaque_send_acquire(op);

	if (unlikely(op->send_flags & IORING_OPAQUE_SEND_VECTOR))
		ret = io_opaque_vec_finish(op, ret);
	return ret;
}

/*
 * The admission of a request that found frames ahead of it. It takes its
 * references before it waits, so that a LAST send parked behind another has
 * consumed its object, and joins the queue afterwards, in a second tx_lock
 * hold: a queue that fails in between cancels it. It holds no lock but the one that
 * a LAST send takes for its references (tables, alone), and the socket if its caller
 * owns it. Returns as io_opaque_tx_join() does.
 *
 * A request that is refused, because admission failed or the queue did, keeps the
 * references it took, and a LAST send stays consumed: the socket may be owned, where a
 * send puts no version, and a reference of the request may be the last one on its version
 * (another ring frees the object once the request has it). The cleanup of the request gives
 * them back, with the socket released and the ring locked, as it does for any send that ends
 * without having the socket (io_opaque_vec_acquire()).
 */
static int io_opaque_tx_settle(struct io_opaque_req *op, struct io_opaque_tx *tx)
{
	struct io_opaque_store *store = op->store;
	int ret = io_opaque_tx_acquire(op);

	spin_lock(&store->tx_lock);
	tx->admitting--;
	if (ret || tx->failed) {
		io_opaque_tx_unused(store, tx);
		spin_unlock(&store->tx_lock);
		return ret ?: -ECANCELED;
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
 * io_opaque_send() joins in two places, for an io-wq issue and for an inline one. The join
 * is part of it, not a call, as it was when it had one place: both are inlined, except
 * in a test, which calls the join and has it as a function.
 */
#if IS_ENABLED(CONFIG_KUNIT)
#define IO_OPAQUE_JOIN_INLINE
#else
#define IO_OPAQUE_JOIN_INLINE	__always_inline
#endif

/*
 * Queue @op behind the frames already admitted for @sk. Returns
 * IO_OPAQUE_TX_WAIT when it waits for one (its phase is set before tx_lock is
 * dropped, so a release cannot miss it), IO_OPAQUE_TX_HEAD when it is the head
 * and holds its references, IO_OPAQUE_TX_SETTLE when it is the head and has
 * not taken them yet, or a negative errno, with the references that the request took
 * before it was refused still its own, for its cleanup to give back.
 *
 * A request that finds its queue empty links itself as the head in the one
 * tx_lock hold that finds out, and takes its references later, from
 * io_opaque_tx_settle_head(), with the socket owned. Nothing can join the
 * queue ahead of it, and a head cannot fail the queue, so nothing that
 * settling does is undone by the queue. A request that finds frames ahead
 * takes its references now, and may find itself the head when it is done
 * (io_opaque_tx_settle()). A queue that has failed refuses it.
 *
 * A send that is issued inline on a plain TCP socket joins with the socket
 * owned (io_opaque_send()): tx_lock is taken under the socket, wait_lock under
 * tx_lock to park, and tables under the socket and not with tx_lock. An io-wq
 * issue joins before it takes the socket. tx_lock and tables are never held
 * when the socket is taken.
 *
 * @op->req is not used; the caller enrolls a waiting request for cancellation.
 */
VISIBLE_IF_KUNIT IO_OPAQUE_JOIN_INLINE int
io_opaque_tx_join(struct io_opaque_req *op, struct sock *sk)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_tx *tx, *spare = NULL;
	struct rb_node **link, *parent;
	int ret = 0;

	KUNIT_STATIC_STUB_REDIRECT(io_opaque_tx_join, op, sk);
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
	/* Only a queue that another ring made first leaves one. */
	if (unlikely(spare))
		kfree(spare);
	if (ret)
		return ret;
	return io_opaque_tx_settle(op, tx);
}

/*
 * Take the references of a head that io_opaque_tx_join() left to settle, with
 * the socket owned where it can be. Nothing is removed when this fails, and
 * the request is still the head: it leaves the queue through io_opaque_tx_put(),
 * which hands the queue to whoever waits behind it and does not fail it, since
 * nothing of this frame was sent. A vector that pinned versions before it failed
 * still holds them, for the cleanup of the request: nothing is put with the socket
 * owned (io_opaque_vec_acquire()).
 */
VISIBLE_IF_KUNIT int io_opaque_tx_settle_head(struct io_opaque_req *op)
{
	KUNIT_STATIC_STUB_REDIRECT(io_opaque_tx_settle_head, op);
	return io_opaque_tx_acquire(op);
}

/*
 * Cancel and wake the frames admitted behind @op, a head that attempted TCP and
 * ended short. @op has left @tx, which has failed already, and goes back in for
 * the walk. At most IO_OPAQUE_WAKE_BATCH followers are looked at in a hold of
 * tx_lock, and they are woken after it: how many there are is up to the process,
 * and every socket and ring of the store takes tx_lock.
 *
 * The followers stay in the queue and leave it as they always did, each when its
 * request is cleaned up, so the queue stays failed until the last has gone. @op
 * is where the walk has got to. It moves behind each batch and leaves with the
 * last, and nobody else moves it: the followers after it have not been looked at,
 * whoever leaves meanwhile. It is not parked, and a release hands the queue only
 * to a parked follower, so a canceled follower that is released as the first of
 * the queue cannot promote one that is not canceled yet.
 *
 * A follower is touched in a hold of tx_lock, where it cannot be cleaned up, for
 * its cleanup leaves the queue first. After the hold the walk touches only those
 * it made queued: nobody else will wake them, and a request ends only once it is
 * woken. A follower that was queued before is a cancellation's to wake. It is
 * marked here, as it always was, and left alone.
 */
static noinline_for_stack void io_opaque_tx_fail(struct io_opaque_store *store,
						 struct io_opaque_tx *tx,
						 struct io_opaque_req *op)
{
	struct io_opaque_req *woken[IO_OPAQUE_WAKE_BATCH];

	lockdep_assert_held(&store->tx_lock);
	op->tx = tx;
	list_add(&op->send, &tx->requests);
	while (op->send.next != &tx->requests) {
		struct list_head *pos = op->send.next;
		unsigned int seen, nr = 0, i;

		for (seen = 0; seen < IO_OPAQUE_WAKE_BATCH && pos != &tx->requests; seen++) {
			struct io_opaque_req *next = list_entry(pos, struct io_opaque_req, send);
			bool queue;

			scoped_guard(spinlock, &store->wait_lock) {
				WRITE_ONCE(next->canceled, true);
				queue = next->phase == IO_OPAQUE_SEND_WAIT;
				if (queue)
					next->phase = IO_OPAQUE_QUEUED;
			}
			if (queue)
				woken[nr++] = next;
			pos = pos->next;
		}
		/* Behind the ones looked at: what follows is what is left to do. */
		list_move(&op->send, pos->prev);
		spin_unlock(&store->tx_lock);
		for (i = 0; i < nr; i++)
			io_opaque_queue_ready(woken[i], -ECANCELED);
		spin_lock(&store->tx_lock);
	}
	list_del_init(&op->send);
	op->tx = NULL;
	/* The followers may all have gone while the walk was out of the lock. */
	io_opaque_tx_unused(store, tx);
}

/*
 * Remove @op from its queue. A head that attempted TCP and ended short fails
 * the queue: every follower is canceled and woken (io_opaque_tx_fail()), and
 * nothing is returned. Otherwise the follower that becomes head is returned with
 * its phase already queued, for the caller to wake. Only the failure wakeups
 * reach an io_kiocb.
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
		io_opaque_tx_fail(store, tx, op);
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
 * Managed fragments. Spliced, a body costs TCP a page reference for every
 * fragment, taken and dropped by whichever CPU gets to it, and a charge of
 * socket memory for every byte. SEND_ZC does without both: its skbs have
 * managed fragments (SKBFL_MANAGED_FRAG_REFS), which take no page reference
 * and, in a pure zerocopy skb, no socket memory, and the ubuf_info of the skb
 * keeps alive what they point to. That ubuf_info holds a notification, which
 * holds the registered buffer. This one holds the pages, and nothing else.
 *
 * It is made for each skb that gets fragments (io_opaque_link_skb()), owns one
 * reference on every distinct folio that the fragments of the skbs sharing it
 * use, and drops them when the last of those is freed, from whatever context
 * that is: the ACK that frees a sent skb, the sender's own backlog, the
 * completion of a device, a tap that held a clone (io_opaque_ubuf_complete()).
 * It has no pointer to the request, the store or the version; it points to a
 * pool that the store made, which it keeps alive. A send gives
 * back its references on its version as it did, and a LAST send hands the
 * extents back to the store as it did: what holds the pages then is the
 * ubuf, as it was TCP's references before, and the unit cache keeps a unit
 * cooling until it completes. What the store has charged, the handles and the
 * lifetime of the request do not depend on it.
 *
 * Nor can a consumer be left with a page that is recycled: a clone shares the
 * shinfo, the shinfo holds the ubuf, and the ubuf holds the folios. An skb that
 * gets a shinfo of its own, by a split, a segmentation or the copy of a clone,
 * takes references on the pages for itself first and one on the ubuf. The
 * completion is not a notification that anyone waits for, so the ubuf asks not to
 * be copied on the receive path (SKBFL_DONT_ORPHAN_RX): a packet socket, or a
 * socket of this host, may hold the skb for as long as it likes, as it may hold a
 * spliced page. What the page holds can change meanwhile for a store that binds an
 * instance or holds a page of one, as it can for a spliced page, and those skbs
 * say so (see the flags below).
 */

/*
 * The flags of the ubuf of every store. Not SKBFL_MANAGED_FRAG_REFS, which belongs
 * to the shinfo and is set by the fill: were it the ubuf's, an skb derived from
 * this one with skb_zerocopy_clone() would take no references of its own.
 *
 * And not SKBFL_SHARED_FRAG, which the stack takes to mean that someone else may
 * write the fragments while the skb is queued, as after vmsplice() or sendfile(),
 * and so copies them before it sums them in software or changes them in place
 * (skb_checksum_help(), skb_segment(), ESP). The pages of an ordinary store cannot
 * change once their object is published, and no process maps them. An ordinary
 * store is one that binds no MAIO instance and holds no page of one. The pages of
 * a store that binds an instance are the area's, which a process maps read-write
 * and may write at any time, and so are the area pages that a store without an
 * instance keeps when it captures from a queue that the instance owns
 * (io_opaque_note_area_page()). The splice path that took such pages before
 * managed fragments (tcp_sendmsg_locked()) marked every page it took. So the skbs
 * of these stores get the flag as well, io_opaque_send_ubuf_flags(): a copy where
 * the stack sums in software, and nothing where the device does. It is the store's
 * and not the object's: an object that CREATE or compaction made in such a store
 * is in ordinary pages, and is marked too.
 *
 * A store without an instance gets the mark from a flag that the capture sets when
 * it keeps an area page. Copying such a page at the capture instead would cost a
 * copy per page on the path that exists to avoid one, where the mark costs a copy
 * only when the stack must sum in software.
 */
#define IO_OPAQUE_UBUF_FLAGS \
	(SKBFL_ZEROCOPY_ENABLE | SKBFL_DONT_ORPHAN | SKBFL_DONT_ORPHAN_RX)

static struct kmem_cache *io_opaque_ubuf_cache __ro_after_init;

VISIBLE_IF_KUNIT const struct ubuf_info_ops io_opaque_ubuf_ops;
VISIBLE_IF_KUNIT const struct ubuf_info_ops io_opaque_msg_ubuf_ops;

VISIBLE_IF_KUNIT u8 io_opaque_ubuf_flags(void)
{
	KUNIT_STATIC_STUB_REDIRECT(io_opaque_ubuf_flags);
	return IO_OPAQUE_UBUF_FLAGS;
}

/*
 * What a store adds to those flags for the ubufs of its sends: one byte of the store
 * (send_ubuf_flags), so that a send tests neither the binding nor the flag of the
 * capture. The binding is set when the store is made and never changes, and the byte of
 * a bound store with it. The byte of a store without an instance goes from off to on,
 * once, when the capture keeps a page of an area, and never back: a send that races it
 * sees either, as it saw either value of the flag. A ubuf that the store's pool kept is
 * made whole again by link_skb() with the flags of the send that has it now, so the
 * ubufs of a send all have the same, whether they are new or were kept, and whenever
 * they were made.
 */
VISIBLE_IF_KUNIT u8 io_opaque_send_ubuf_flags(const struct io_opaque_store *store)
{
	return READ_ONCE(store->send_ubuf_flags);
}

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * Ubufs taken from the cache and not yet given back to it, and pools made and not yet
 * freed, for a test to say that they are all back.
 */
atomic_long_t io_opaque_ubufs_live = ATOMIC_LONG_INIT(0);
atomic_long_t io_opaque_ubuf_pools_live = ATOMIC_LONG_INIT(0);
#define IO_OPAQUE_UBUF_TAKEN()		atomic_long_inc(&io_opaque_ubufs_live)
#define IO_OPAQUE_UBUF_GIVEN()		atomic_long_dec(&io_opaque_ubufs_live)
#define IO_OPAQUE_UBUF_POOL_MADE()	atomic_long_inc(&io_opaque_ubuf_pools_live)
#define IO_OPAQUE_UBUF_POOL_GONE()	atomic_long_dec(&io_opaque_ubuf_pools_live)
#else
#define IO_OPAQUE_UBUF_TAKEN()		((void)0)
#define IO_OPAQUE_UBUF_GIVEN()		((void)0)
#define IO_OPAQUE_UBUF_POOL_MADE()	((void)0)
#define IO_OPAQUE_UBUF_POOL_GONE()	((void)0)
#endif

/*
 * A ubuf is made for each skb that gets fragments and freed where the last skb
 * that shares it is, which is an ACK on whatever CPU gets it: the sender takes
 * from the cache what other CPUs give back to it. A store keeps the ubufs that
 * have completed, up to IO_OPAQUE_UBUF_POOL_MAX of them on a list that any context
 * can push to and a few more that the send side holds, and a send takes one of
 * those before it asks the cache. Nothing about what a ubuf holds changes: it
 * drops its folios when its last skb goes, whole until then for as long as an skb,
 * a clone, a split or a tap of one holds it, and it is not kept longer than the
 * pool has room for.
 *
 * The pool is the store's, because that is what a send is accounted to, but no skb
 * has a pointer to the store or to the ring, and either may be gone while an
 * skb waits in a queue for an ACK or at a tap. So the ubuf points to the pool, and
 * the pool is freed by the last ubuf that points to it: the count of its
 * references is one for the store and one for each ubuf made by the cache for it
 * and not yet given back, which a ubuf that is only reused does not touch. A store
 * that goes closes the pool: the ubufs it holds are freed, and a ubuf that
 * completes later is refused and freed to the cache as it would be with no pool.
 *
 * The two ends. A completion pushes with a compare-and-exchange, and nothing else:
 * it runs in softirq, in the context of a task closing a socket, or with interrupts
 * off, on any CPU. The send side runs with the socket owned, but for any socket and
 * any ring of the store, in a task that may be preempted: it takes whatever has
 * been pushed in one exchange, into a stock that it hands out of under a lock that
 * no completion takes, so that the head of the list is touched once for each batch
 * and not once for each skb. Closing a pool takes the list with a sentinel in its
 * place, so that a ubuf that completes after it finds the pool closed on its own
 * push and is not left on a list that nobody takes. The nr that bounds the list is
 * not exact: it is set to nothing when the list is taken, and a ubuf that completes
 * at that moment may be on the list and not counted. A pool therefore holds at most
 * twice IO_OPAQUE_UBUF_POOL_MAX ubufs and a few more, in done and in stock, and no
 * more than the skbs of the store ever had in flight.
 *
 * KASAN. A ubuf in the pool is poisoned, as an entry of io_alloc_cache is, so that an
 * access through a pointer that an skb kept to a ubuf that has completed is reported
 * where it happens. It is poisoned before it is pushed, which is when the send side
 * may take it, and unpoisoned when it is taken out or freed. The link is not: the
 * lists are read and written while the ubufs on them are poisoned. KASAN keeps the
 * track of the free in the first bytes of the object, where the count is: a ubuf in
 * the pool has a count of zero only in a build without KASAN, and with it the access
 * that would have read the count is reported first. Without KASAN none of this is
 * code.
 */
#define IO_OPAQUE_UBUF_POOL_CLOSED	((struct llist_node *)1UL)

/*
 * A ubuf goes into the pool poisoned, but for its link. False is KASAN's word that it
 * was freed already, which it has reported; the ubuf is not the pool's.
 */
static __always_inline bool io_opaque_ubuf_poison(struct io_opaque_ubuf *u)
{
	if (!kasan_mempool_poison_object(u))
		return false;
	kasan_unpoison_range(&u->node, sizeof(u->node));
	return true;
}

/* It comes out of the pool, to be used by a skb or freed to the cache. */
static __always_inline void io_opaque_ubuf_unpoison(struct io_opaque_ubuf *u)
{
	kasan_mempool_unpoison_object(u, sizeof(*u));
}

VISIBLE_IF_KUNIT struct io_opaque_ubuf_pool *io_opaque_ubuf_pool_new(void)
{
	struct io_opaque_ubuf_pool *pool = kzalloc_obj(*pool, GFP_KERNEL_ACCOUNT);

	if (!pool)
		return NULL;
	refcount_set(&pool->refs, 1);
	spin_lock_init(&pool->lock);
	IO_OPAQUE_UBUF_POOL_MADE();
	return pool;
}

static void io_opaque_ubuf_pool_unref(struct io_opaque_ubuf_pool *pool)
{
	if (refcount_dec_and_test(&pool->refs)) {
		IO_OPAQUE_UBUF_POOL_GONE();
		kfree(pool);
	}
}

/* Give a ubuf that nothing holds back to the cache, and let go of the pool that it was made for. */
static void io_opaque_ubuf_free(struct io_opaque_ubuf *u)
{
	struct io_opaque_ubuf_pool *pool = u->pool;

	kmem_cache_free(io_opaque_ubuf_cache, u);
	IO_OPAQUE_UBUF_GIVEN();
	if (pool)
		io_opaque_ubuf_pool_unref(pool);
}

/* Free a chain of ubufs that were in a pool. */
static void io_opaque_ubuf_free_chain(struct llist_node *node)
{
	while (node) {
		struct llist_node *next = node->next;
		struct io_opaque_ubuf *u = llist_entry(node, struct io_opaque_ubuf, node);

		io_opaque_ubuf_unpoison(u);
		io_opaque_ubuf_free(u);
		node = next;
	}
}

/*
 * Keep a ubuf that has completed, if its pool is open and has room: true, and it is
 * the pool's, to be taken by the send side. False, and it is the caller's to free.
 * Any context, any CPU, and nothing but atomics.
 */
static bool io_opaque_ubuf_pool_push(struct io_opaque_ubuf_pool *pool, struct io_opaque_ubuf *u)
{
	struct llist_node *first;

	if (atomic_inc_return(&pool->nr) > IO_OPAQUE_UBUF_POOL_MAX)
		goto refused;
	/* Before the exchange that lets the send side have it, not after. */
	if (!io_opaque_ubuf_poison(u))
		goto refused;
	first = READ_ONCE(pool->done.first);
	do {
		if (first == IO_OPAQUE_UBUF_POOL_CLOSED) {
			io_opaque_ubuf_unpoison(u);
			goto refused;
		}
		u->node.next = first;
	} while (!try_cmpxchg(&pool->done.first, &first, &u->node));
	return true;
refused:
	atomic_dec(&pool->nr);
	return false;
}

/*
 * A ubuf for a new skb from the pool, or none. The send side takes everything that
 * the completions have pushed in one exchange when its stock has run out, and hands
 * out of the stock one at a time. pool->lock is for the sends of the store, which
 * run on many sockets and rings; the completions never take it.
 */
VISIBLE_IF_KUNIT struct io_opaque_ubuf *io_opaque_ubuf_pool_get(struct io_opaque_ubuf_pool *pool)
{
	struct io_opaque_ubuf *u;
	struct llist_node *node;

	spin_lock(&pool->lock);
	node = pool->stock;
	if (!node) {
		node = READ_ONCE(pool->done.first);
		while (node && node != IO_OPAQUE_UBUF_POOL_CLOSED &&
		       !try_cmpxchg(&pool->done.first, &node, NULL))
			;
		if (node == IO_OPAQUE_UBUF_POOL_CLOSED)
			node = NULL;
		if (node)
			atomic_set(&pool->nr, 0);
	}
	if (node)
		pool->stock = node->next;
	spin_unlock(&pool->lock);
	if (!node)
		return NULL;
	/* After the link is read, and out of the lock: KASAN has a stack to save. */
	u = llist_entry(node, struct io_opaque_ubuf, node);
	io_opaque_ubuf_unpoison(u);
	return u;
}

/*
 * The store has gone, with no send running: free what the pool holds, and refuse what
 * completes from now on, which is freed as it would be without a pool. The pool
 * itself goes with the last of the ubufs that skbs still hold, if there are any, for
 * which the store's reference is dropped last.
 */
VISIBLE_IF_KUNIT void io_opaque_ubuf_pool_close(struct io_opaque_ubuf_pool *pool)
{
	struct llist_node *done, *stock;

	if (!pool)
		return;
	done = xchg(&pool->done.first, IO_OPAQUE_UBUF_POOL_CLOSED);
	if (WARN_ON_ONCE(done == IO_OPAQUE_UBUF_POOL_CLOSED))
		done = NULL;
	spin_lock(&pool->lock);
	stock = pool->stock;
	pool->stock = NULL;
	spin_unlock(&pool->lock);
	io_opaque_ubuf_free_chain(done);
	io_opaque_ubuf_free_chain(stock);
	io_opaque_ubuf_pool_unref(pool);
}

/* The pages go back, whichever context frees the last skb that shares the ubuf. */
static void io_opaque_ubuf_complete(struct sk_buff *skb, struct ubuf_info *uarg, bool success)
{
	struct io_opaque_ubuf *u = container_of(uarg, struct io_opaque_ubuf, uarg);
	unsigned int i;

	/*
	 * Told by skb_copy_ubufs() that the data was copied: a consumer did not wait. Or by
	 * __pskb_pull_tail(), which says it when the last fragment of an skb is pulled into the
	 * head, as the stack does to sum the skb of a bound store in software.
	 */
	if (!success)
		io_opaque_count(IO_OPAQUE_STAT_SEND_UBUF_COPIED, 1);
	if (!refcount_dec_and_test(&uarg->refcnt))
		return;
	for (i = 0; i < u->nr; i++)
		folio_put(u->folio[i]);
	/* Whole until here, and nothing holds it from here: another skb may have it at once. */
	if (u->pool && io_opaque_ubuf_pool_push(u->pool, u))
		return;
	io_opaque_ubuf_free(u);
}

/*
 * Give @skb a ubuf of its own, called by TCP before it fills a skb and with
 * the socket owned, where it may sleep. The new ubuf has the one reference
 * that the skb's shinfo owns; the skbs that share the shinfo and the ones
 * derived from it take theirs. A skb that has one of ours joins it, for the
 * price of the comparison: a send appends to the tail of the write queue, and
 * it does not matter whose send began that skb. Another provider's skb is a
 * boundary, as io_uring's own link_skb() makes it for the provider it does not
 * mix with: TCP pushes it and starts a new one.
 *
 * So is a skb of ours that has all the fragments it may have. -EEXIST makes
 * TCP push it and start another, which is what is wanted of a skb that is
 * full, and it does so before the fill: a fill that is let in finds no room,
 * adds nothing and says -EMSGSIZE, and skb_zerocopy_iter_stream() then takes
 * back what it did, ___pskb_trim() walking every fragment of the skb, for TCP
 * to do the same. A range that would have extended the last fragment starts
 * the next skb instead.
 *
 * The ubuf is one that the send's store has kept (io_opaque_ubuf_pool_get()) if
 * the msg_ubuf names the pool and has one, else the cache's. Its flags are set here
 * whichever way it was got: those of every ubuf, and what the flags of the msg_ubuf
 * add, which is SKBFL_SHARED_FRAG for a store that binds an instance or holds a page of
 * one. A skb of ours is joined only by a send that adds the same, so that the fragments of
 * such a store are never in a skb that is not marked, nor those of another store in
 * one that is: when a bound store and another store send on one socket, the skb is
 * a boundary, as another provider's is. So it is for a store whose flag turned on
 * behind its own skbs in the queue. TCP does not set skb->sk on a skb of its write
 * queue, so the allocation has no socket to take its mask from. GFP_KERNEL is what
 * a user's socket allocates its skbs with.
 */
VISIBLE_IF_KUNIT int io_opaque_link_skb(struct sk_buff *skb, struct ubuf_info *uarg)
{
	struct io_opaque_ubuf_pool *pool = NULL;
	struct ubuf_info *cur = skb_zcopy(skb);
	struct io_opaque_ubuf *u;
	u8 flags = 0;

	/* TCP passes the msg_ubuf of the send; a caller that is not a send passes none. */
	if (uarg && uarg->ops == &io_opaque_msg_ubuf_ops) {
		const struct io_opaque_msg_ubuf *mub =
			container_of(uarg, struct io_opaque_msg_ubuf, uarg);

		pool = mub->pool;
		flags = uarg->flags;
	}
	if (cur) {
		if (cur->ops != &io_opaque_ubuf_ops ||
		    ((cur->flags ^ flags) & SKBFL_SHARED_FRAG) ||
		    skb_shinfo(skb)->nr_frags >= READ_ONCE(net_hotdata.sysctl_max_skb_frags))
			return -EEXIST;
		return 0;
	}
	u = pool ? io_opaque_ubuf_pool_get(pool) : NULL;
	if (u) {
		io_opaque_count(IO_OPAQUE_STAT_SEND_UBUF_REUSED, 1);
	} else {
		u = kmem_cache_alloc(io_opaque_ubuf_cache, GFP_KERNEL);
		if (!u)
			return -ENOMEM;
		IO_OPAQUE_UBUF_TAKEN();
		/* The pool lasts as long as a ubuf made for it does. */
		if (pool)
			refcount_inc(&pool->refs);
		u->pool = pool;
	}
	u->uarg.ops = &io_opaque_ubuf_ops;
	u->uarg.flags = io_opaque_ubuf_flags() | flags;
	refcount_set(&u->uarg.refcnt, 1);
	u->nr = 0;
	skb_zcopy_init(skb, &u->uarg);
	io_opaque_count(IO_OPAQUE_STAT_SEND_MANAGED_SKBS, 1);
	return 0;
}

VISIBLE_IF_KUNIT const struct ubuf_info_ops io_opaque_ubuf_ops = {
	.complete	= io_opaque_ubuf_complete,
	.link_skb	= io_opaque_link_skb,
};

/*
 * The ops of what msg_ubuf points at during a managed send, a struct
 * io_opaque_msg_ubuf on the stack of io_opaque_send(), for
 * skb_zerocopy_iter_stream() to find link_skb through. TCP never links it to
 * an skb, because link_skb has given each skb its own, and it must not: it
 * lives for the one call. The one path that would is fast open, whose SYN
 * takes the msg_ubuf of its first send; a socket that could take that path is
 * not sent managed, and a send never has MSG_FASTOPEN.
 */
static void io_opaque_msg_ubuf_complete(struct sk_buff *skb, struct ubuf_info *uarg, bool success)
{
	WARN_ON_ONCE(1);
}

VISIBLE_IF_KUNIT const struct ubuf_info_ops io_opaque_msg_ubuf_ops = {
	.complete	= io_opaque_msg_ubuf_complete,
	.link_skb	= io_opaque_link_skb,
};

static bool io_opaque_ubuf_holds(const struct io_opaque_ubuf *u, const struct folio *folio)
{
	unsigned int i = u->nr;

	/* Ranges come in the order they are stored, so the last entry is nearly always the one. */
	while (i--)
		if (u->folio[i] == folio)
			return true;
	return false;
}

/*
 * The page of a segment of a send's cursor. It is the first page of its folio,
 * and the offset counts from it: io_opaque_extent_new() and io_opaque_append()
 * keep the compound head of a backing and the offset from the head, compaction
 * and CREATE fill whole compound pages from offset 0, and the cursor only moves
 * the offset up and the length down inside a segment (io_opaque_vec_span(),
 * io_opaque_send_iter() and the fill itself). So a segment is a fragment as it
 * stands, with no lookup of its folio and no cut where the folio ends; a kernel
 * with CONFIG_DEBUG_VM checks both.
 */
static inline struct page *io_opaque_seg_page(const struct bio_vec *bv)
{
	VM_WARN_ON_ONCE_PAGE(PageTail(bv->bv_page), bv->bv_page);
	VM_WARN_ON_ONCE((size_t)bv->bv_offset + bv->bv_len > page_size(bv->bv_page));
	return bv->bv_page;
}

/*
 * Add the bytes of @from to @skb as managed fragments, at most @length of
 * them: what a splice would add, with a folio's reference taken once for the
 * ubuf and not once for each fragment, and no page reference in the skb. A
 * fragment starts where the previous one ends if it continues in the same
 * folio (skb_can_coalesce() refuses zerocopy skbs). The fragment limit is
 * the splice's, and so is what a fragment weighs, its bytes, for a skb that
 * owns no page. A reference is taken together with the fragment that needs
 * it, before the fragment is published, so that every fragment of the skb
 * has its folio in the ubuf. The list can hold as many folios as an skb has
 * fragments, which it does if the skb is the only one to use the ubuf, as it
 * is while the skb is managed; one that is full ends the fill like a full skb.
 *
 * Per fragment, the loop is io_sg_from_iter()'s: the segment is the fragment
 * (io_opaque_seg_page()), to which the fill adds the test against the last
 * fragment and, when the folio is not that fragment's, a look at the ubuf's
 * list. The cursor is advanced here, as io_sg_from_iter() advances it, and not
 * walked again by iov_iter_advance().
 *
 * A skb that has fragments of its own cannot be managed: the fill takes a
 * reference for each, as a splice does (a framed send's prefix is copied into
 * the skb first). Returns 0 with the bytes added, or -EMSGSIZE if the skb
 * cannot take any more, in which case TCP starts another when nothing was added.
 */
VISIBLE_IF_KUNIT int io_opaque_sg_from_iter(struct sk_buff *skb, struct iov_iter *from,
					    size_t length)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	struct ubuf_info *uarg = skb_zcopy(skb);
	unsigned int limit = READ_ONCE(net_hotdata.sysctl_max_skb_frags);
	unsigned int nr = shinfo->nr_frags, refs = 0;
	const struct bio_vec *bv = from->bvec;
	unsigned long segs = from->nr_segs;
	size_t skip = from->iov_offset;
	size_t left = min(from->count, length), added = 0;
	struct page *held = NULL;
	skb_frag_t *last = NULL;
	struct io_opaque_ubuf *u;
	int ret = 0;

	if (WARN_ON_ONCE(!uarg || uarg->ops != &io_opaque_ubuf_ops || !iov_iter_is_bvec(from)))
		return -EFAULT;
	if (nr && !skb_zcopy_managed(skb)) {
		ssize_t spliced;

		io_opaque_count(IO_OPAQUE_STAT_SEND_FRAG_FALLBACK, 1);
		spliced = skb_splice_from_iter(skb, from, left);
		if (spliced == -EMSGSIZE)
			return -EMSGSIZE;
		/* TCP takes -EFAULT as the end of the send, and no other error is worth more. */
		return spliced < 0 ? -EFAULT : 0;
	}
	u = container_of(uarg, struct io_opaque_ubuf, uarg);
	if (!nr) {
		shinfo->flags |= SKBFL_MANAGED_FRAG_REFS;
	} else {
		/* Every fragment of a managed skb is the head of a folio that the ubuf holds. */
		last = &shinfo->frags[nr - 1];
		held = skb_frag_page(last);
	}

	while (left) {
		struct page *page;
		size_t off, part;

		/* The cursor holds as many bytes as its count says, or the count is wrong. */
		if (unlikely(!segs)) {
			ret = -EFAULT;
			break;
		}
		part = min(bv->bv_len - skip, left);
		if (unlikely(!part)) {
			bv++;
			segs--;
			skip = 0;
			continue;
		}
		page = io_opaque_seg_page(bv);
		off = bv->bv_offset + skip;

		if (last && skb_frag_page(last) == page &&
		    skb_frag_off(last) + skb_frag_size(last) == off) {
			skb_frag_size_add(last, part);
		} else {
			if (nr >= limit) {
				ret = -EMSGSIZE;
				break;
			}
			if (page != held) {
				struct folio *folio = page_folio(page);

				if (!io_opaque_ubuf_holds(u, folio)) {
					if (u->nr == ARRAY_SIZE(u->folio)) {
						ret = -EMSGSIZE;
						break;
					}
					folio_get(folio);
					u->folio[u->nr++] = folio;
					refs++;
				}
				held = page;
			}
			last = &shinfo->frags[nr];
			__skb_fill_page_desc_noacc(shinfo, nr++, page, off, part);
		}
		added += part;
		left -= part;
		skip += part;
		if (skip == bv->bv_len) {
			bv++;
			segs--;
			skip = 0;
		}
	}
	shinfo->nr_frags = nr;
	skb_len_add(skb, added);
	/* What iov_iter_advance(from, added) comes to, without walking the segments again. */
	from->bvec = bv;
	from->nr_segs = segs;
	from->iov_offset = skip;
	from->count -= added;
	if (refs)
		io_opaque_count(IO_OPAQUE_STAT_SEND_FOLIO_REFS, refs);
	return ret;
}

/*
 * Whether a body that is spliced goes out as managed fragments: when the
 * send calls tcp_sendmsg_locked() itself, not on a socket that TCP fast open
 * could take (see io_opaque_msg_ubuf_ops), and unless the evaluation
 * parameter turned it off. Anywhere else the body is spliced as before.
 */
static bool io_opaque_sends_managed(const struct sock *sk)
{
	return READ_ONCE(io_opaque_send_managed) && io_opaque_ubuf_cache &&
	       !inet_test_bit(DEFER_CONNECT, sk);
}

static int __init io_opaque_ubuf_init(void)
{
	BUILD_BUG_ON(MAX_SKB_FRAGS > U8_MAX);
	/* A cache that failed leaves every send spliced. */
	io_opaque_ubuf_cache = KMEM_CACHE(io_opaque_ubuf, 0);
	return 0;
}
device_initcall(io_opaque_ubuf_init);

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
/*
 * Called with the message of every part of a frame, just before it goes to TCP
 * (@owned) or to sock_sendmsg(), and the send's msg_ubuf, for a test to look at.
 */
void (*io_opaque_send_part)(const struct msghdr *msg,
			    const struct io_opaque_msg_ubuf *mub, bool owned);
#define IO_OPAQUE_SEND_PART(msg, mub, owned)				\
	do {								\
		if (io_opaque_send_part)				\
			io_opaque_send_part((msg), (mub), (owned));	\
	} while (0)
/*
 * A byte that a test fills the message and the msg_ubuf of a send with before they
 * are set (-1: leave them), so that a field that is read before it is written
 * does not get away with a zero.
 */
int io_opaque_send_poison = -1;
#define IO_OPAQUE_SEND_POISON(p)					\
	do {								\
		if (io_opaque_send_poison >= 0)				\
			memset((p), io_opaque_send_poison, sizeof(*(p)));	\
	} while (0)
#else
#define IO_OPAQUE_SEND_LOCKED_CALL()	((void)0)
#define IO_OPAQUE_SEND_PART(msg, mub, owned)	((void)(msg), (void)(mub), (void)(owned))
#define IO_OPAQUE_SEND_POISON(p)	((void)(p))
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
	const struct proto *prot;
	u16 port;

	KUNIT_STATIC_STUB_REDIRECT(io_opaque_send_direct, sk);
	prot = READ_ONCE(sk->sk_prot);
	/* Written when the socket binds or connects; a stale zero only spoils a hint. */
	port = data_race(inet_sk(sk)->inet_num);
	return prot->sendmsg == tcp_sendmsg && prot == sk->sk_prot_creator && port;
}

/*
 * Join the queue of the destination for an issue, under the ring lock. Returns 0
 * when the request goes on, as the head (@fresh if it has still to take its
 * references), IOU_ISSUE_SKIP_COMPLETE when it has parked behind another frame
 * and is listed for cancellation, or the errno that ends the issue. Inlined, so
 * that the join stays part of io_opaque_send() in a profile, as it was.
 */
static __always_inline int io_opaque_send_join(struct io_opaque_req *op, struct sock *sk,
					       bool *fresh)
{
	int ret = io_opaque_tx_join(op, sk);

	if (ret < 0)
		return ret;
	if (ret == IO_OPAQUE_TX_WAIT) {
		/* The ring lock keeps the wakeup from running before the request is listed. */
		io_opaque_enroll(op);
		return IOU_ISSUE_SKIP_COMPLETE;
	}
	*fresh = ret == IO_OPAQUE_TX_SETTLE;
	return 0;
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
 * once, for what is left of the frame. The caller keeps the verdict and
 * applies it where the first sock_sendmsg() would have met it, so that the
 * issue ends in the same way. The ordinary path asks for every part, which a
 * policy that depends on the socket and not on the part cannot tell apart.
 *
 * It is asked with the socket not owned, because a hook may take the socket
 * lock itself: AppArmor's finds the address with getname(), which does, and
 * would wait for the owner of the socket for ever.
 *
 * @msg is the message of the issue, as it is before any part is made of it: the
 * flags of the issue, no address, no control data, no bytes. It is the one the
 * parts are sent with, as it is for the first sock_sendmsg() of the other path,
 * so the issue zeroes one message and not two.
 */
VISIBLE_IF_KUNIT int io_opaque_send_security(struct socket *sock, struct msghdr *msg,
					     const struct io_opaque_req *op)
{
	KUNIT_STATIC_STUB_REDIRECT(io_opaque_send_security, sock, msg, op);
	lockdep_assert_not_held(&sock->sk->sk_lock);
	return security_socket_sendmsg(sock, msg, op->total_length - op->progress);
}

/*
 * One issue of an object send. A request that is not the head of its
 * destination's queue parks. The head sends its frame, prefix, body and
 * suffix, from where it stopped, and the frame leaves the queue once all of it
 * is queued to TCP.
 *
 * On a plain TCP socket (io_opaque_send_direct()) the head asks the security
 * hook, and then does all of the rest under one lock_sock(): it joins the queue
 * if it is an inline issue, takes the references it has not taken yet, applies
 * what the hook said, calls tcp_sendmsg_locked() for each part of the frame,
 * hands the queue on, lets go of the references that are not the last and only
 * then releases the socket, and after it puts the last ones. While the socket
 * is unowned, ACKs and the transmit that autocorking left queued run under its
 * spinlock, for as long as they take, and the sender's next lock_sock() waits
 * for them; the less the sender does in that window, the less often it does.
 * Any other socket takes its references first and goes through sock_sendmsg()
 * for each part, with the ring lock dropped around it.
 *
 * An inline issue that finds a frame ahead of it, or its own place not at the
 * front, has asked the hook and taken the socket for nothing, and parks. The
 * join has set its phase under tx_lock (io_opaque_tx_join()), the request is
 * then listed for cancellation (io_opaque_enroll()), and only then is the
 * socket released. The head may hand the queue on, and queue the wakeup of the
 * request, as soon as tx_lock is dropped. The wakeup is task work, which runs
 * with the ring lock held, and an inline issue holds that throughout, so it
 * cannot run until the request is listed and the issue has returned. That is
 * the rare case, and the hook's verdict is dropped.
 *
 * Under IO_URING_F_UNLOCKED the ring lock is dropped before the socket is
 * taken, and an issue that owned the socket returns without taking it again,
 * as io-wq expects; an inline issue holds it throughout, as it holds it across
 * sock_sendmsg(). A request that waits is listed under the ring lock, before
 * the wakeup of its predecessor can run (io_opaque_enroll()), so an io-wq issue
 * joins the queue before it drops the ring lock, and parks without the hook or
 * the socket.
 */
int io_opaque_send(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_opaque_req *op = req->async_data;
	struct socket *sock = sock_from_file(req->file);
	/*
	 * Neither is zeroed on entry, as CONFIG_INIT_STACK_ALL_ZERO would: the message
	 * is zeroed once below, and the msg_ubuf is set when a body is managed.
	 */
	struct io_opaque_msg_ubuf mub __uninitialized;
	struct msghdr msg __uninitialized;
	struct sock *sk = NULL;
	struct kvec vec;
	u32 body_end;
	bool body = false, splice = false, fresh = false, left, owned = false;
	bool joined = false;
	int ret, sec = 0;

	IO_OPAQUE_SEND_POISON(&mub);
	IO_OPAQUE_SEND_POISON(&msg);
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
	/*
	 * An issue that holds the ring lock throughout joins the queue once it owns the
	 * socket, below. io-wq drops the ring lock for the socket, and a request that
	 * waits is listed under the ring lock, before its wakeup can run: it joins
	 * here, as it always did, and never waits with the socket owned.
	 */
	if (unlikely(issue_flags & IO_URING_F_UNLOCKED)) {
		ret = io_opaque_send_join(op, sk, &fresh);
		if (ret)
			goto out;
		joined = true;
	}

	/*
	 * The one message of the issue, zeroed once: TCP and the security hooks read
	 * the address and the control data, which are none. Each part sets the fields
	 * it uses, and no other.
	 */
	memset(&msg, 0, sizeof(msg));
	msg.msg_flags = io_opaque_send_msg_flags(op, issue_flags);
	if (io_opaque_send_direct(sk)) {
		/*
		 * The hint says plain TCP; the socket decides once it is owned.
		 * The security hook is asked now, with it not owned, and what it
		 * says is applied below, after the references are taken.
		 */
		io_ring_submit_unlock(req->ctx, issue_flags);
		sec = io_opaque_send_security(sock, &msg, op);
		sock_rps_record_flow(sk);
		lock_sock(sk);
		owned = io_opaque_send_direct(sk);
		if (!owned) {
			release_sock(sk);
			io_ring_submit_lock(req->ctx, issue_flags);
		}
	}
	if (!joined) {
		/*
		 * A head finds its queue free under the socket, and goes on. One that finds a
		 * frame ahead, or its own place not at the front, is parked and listed by the
		 * join, and then lets the socket go with the ring lock still held.
		 */
		ret = io_opaque_send_join(op, sk, &fresh);
		if (ret) {
			if (owned)
				release_sock(sk);
			goto out;
		}
	}
	if (fresh) {
		ret = io_opaque_tx_settle_head(op);
		if (ret) {
			/* Nothing was sent or removed: the frame behind this one goes now. */
			io_opaque_tx_put(op);
			goto leave;
		}
	}
	op->tx_started = true;
	if (owned && sec) {
		/* As the first sock_sendmsg() would have ended with it. */
		ret = sec;
		goto sent;
	}

again:
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
		if (splice && owned && io_opaque_sends_managed(sk)) {
			const struct io_opaque_store *store = op->store;

			/* The ubuf_info is whole: its ops, no count, and the store's flags. */
			mub.uarg = (struct ubuf_info) {
				.ops = &io_opaque_msg_ubuf_ops,
				.flags = io_opaque_send_ubuf_flags(store),
			};
			mub.pool = READ_ONCE(store->ubuf_pool);
			msg.msg_flags |= MSG_ZEROCOPY;
			msg.msg_ubuf = &mub.uarg;
			msg.sg_from_iter = io_opaque_sg_from_iter;
		} else if (splice) {
			msg.msg_flags |= MSG_SPLICE_PAGES;
		}
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
		IO_OPAQUE_SEND_PART(&msg, &mub, true);
		ret = tcp_sendmsg_locked(sk, &msg, iov_iter_count(&msg.msg_iter));
		if (trace_sock_send_length_enabled())
			trace_sock_send_length(sk, ret, 0);
	} else {
		/* io-wq may block in TCP; other requests must retain access to the ring. */
		io_ring_submit_unlock(req->ctx, issue_flags);
		IO_OPAQUE_SEND_PART(&msg, &mub, false);
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
			/*
			 * The next part sets the fields it uses; it must not find the
			 * body's ubuf and fill, or its zerocopy flag.
			 */
			msg.msg_flags = io_opaque_send_msg_flags(op, issue_flags);
			msg.msg_ubuf = NULL;
			msg.sg_from_iter = NULL;
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
	/*
	 * The references go with the socket, the last of a version after it: what cleanup
	 * is left to give back for this send is the request itself. A send that consumed its
	 * object (LAST) has nothing for the socket to wait out. It holds the table's reference,
	 * which is the last one unless a send or a copy of the version is still under way, and
	 * an extra one for each entry of a vector that names a handle again, whose decrement
	 * frees nothing and is as good after the socket. All of them are for
	 * io_opaque_send_unpin(), and the call that would find the count at one and drop
	 * nothing is not made.
	 */
	left = false;
	if (owned)
		left = op->consumed || io_opaque_send_unpin_cheap(op);
	io_req_set_res(req, ret, op->consumed ? IORING_CQE_F_OPAQUE_CONSUMED : 0);
	if (owned) {
		/*
		 * Nothing is left for the ring lock, which an io-wq issue dropped
		 * for the socket and an inline one still holds: taking it only to
		 * give it up would be a round trip for the caller.
		 */
		release_sock(sk);
		if (left)
			io_opaque_send_unpin(op);
		return IOU_COMPLETE;
	}
	ret = IOU_COMPLETE;
	goto out;
leave:
	/* The request keeps whatever it still holds, and the ring lock is as the caller left it. */
	if (owned) {
		release_sock(sk);
		return ret;
	}
out:
	io_ring_submit_unlock(req->ctx, issue_flags);
	return ret;
}
