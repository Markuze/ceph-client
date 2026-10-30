// SPDX-License-Identifier: GPL-2.0
/* Workers copy immutable backing without holding object table locks. */
#include <linux/highmem.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/topology.h>
#include <kunit/static_stub.h>
#include <kunit/visibility.h>

#include "io_uring.h"
#include "opaque_internal.h"

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "io_uring_opaque."

/*
 * Evaluation tunables under /sys/module/io_uring_opaque/parameters/. They are
 * not ABI. The defaults keep the original behaviour: no unit cache, every
 * manual compaction on the dispatcher and every body spliced. cache_bytes and
 * unit_order are sampled when a store is registered; the others apply to each
 * later compaction or send.
 */
unsigned long io_opaque_cache_bytes;
module_param_named(cache_bytes, io_opaque_cache_bytes, ulong, 0644);
MODULE_PARM_DESC(cache_bytes,
		 "Per-store destination unit cache in bytes for stores registered later (0 = off; bounded by compact_headroom)");

unsigned int io_opaque_unit_order = IO_OPAQUE_UNIT_ORDER;

static int io_opaque_unit_order_set(const char *val, const struct kernel_param *kp)
{
	return param_set_uint_minmax(val, kp, 0, IO_OPAQUE_MAX_ORDER);
}

static const struct kernel_param_ops io_opaque_unit_order_ops = {
	.set = io_opaque_unit_order_set,
	.get = param_get_uint,
};
module_param_cb(unit_order, &io_opaque_unit_order_ops, &io_opaque_unit_order, 0644);
MODULE_PARM_DESC(unit_order,
		 "Page order of cached destination units for stores registered later (default 32 KiB)");

unsigned int io_opaque_inline_compact_max;
module_param_named(inline_compact_max, io_opaque_inline_compact_max, uint, 0644);
MODULE_PARM_DESC(inline_compact_max,
		 "Largest object a manual COMPACT copies in the issuing task (0 = always use the dispatcher)");

bool io_opaque_copy_per_page;
module_param_named(compact_copy_per_page, io_opaque_copy_per_page, bool, 0644);
MODULE_PARM_DESC(compact_copy_per_page,
		 "Use the original page-split compaction copy instead of one copy per source run");

unsigned int io_opaque_send_copy_max;
module_param_named(send_copy_max, io_opaque_send_copy_max, uint, 0644);
MODULE_PARM_DESC(send_copy_max,
		 "Largest send body TCP copies instead of splicing page references (0 = always splice)");

DEFINE_PER_CPU(struct io_opaque_stats, io_opaque_stats);

u64 io_opaque_stat_read(enum io_opaque_stat_item item)
{
	u64 sum = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		sum += per_cpu(io_opaque_stats, cpu).item[item];
	return sum;
}

static int io_opaque_stat_get(char *buffer, const struct kernel_param *kp)
{
	unsigned long item = (unsigned long)kp->arg;
	u64 value = io_opaque_stat_read(item);

	/* A gauge summed from per-CPU deltas can read below zero while racing. */
	if (item == IO_OPAQUE_STAT_CACHED_BYTES && (s64)value < 0)
		value = 0;
	return scnprintf(buffer, PAGE_SIZE, "%llu\n", value);
}

/* Counters are read-only, including on the kernel command line. */
static int io_opaque_stat_set(const char *val, const struct kernel_param *kp)
{
	return -EPERM;
}

static const struct kernel_param_ops io_opaque_stat_ops = {
	.set = io_opaque_stat_set,
	.get = io_opaque_stat_get,
};

#define IO_OPAQUE_STAT_PARAM(name, item) \
	module_param_cb(name, &io_opaque_stat_ops, (void *)(item), 0444)

IO_OPAQUE_STAT_PARAM(stat_cache_hits, IO_OPAQUE_STAT_CACHE_HITS);
IO_OPAQUE_STAT_PARAM(stat_cache_misses, IO_OPAQUE_STAT_CACHE_MISSES);
IO_OPAQUE_STAT_PARAM(stat_cache_fallbacks, IO_OPAQUE_STAT_CACHE_FALLBACKS);
IO_OPAQUE_STAT_PARAM(stat_cache_remote, IO_OPAQUE_STAT_CACHE_REMOTE);
IO_OPAQUE_STAT_PARAM(stat_cache_retired, IO_OPAQUE_STAT_CACHE_RETIRED);
IO_OPAQUE_STAT_PARAM(stat_cache_cooled, IO_OPAQUE_STAT_CACHE_COOLED);
IO_OPAQUE_STAT_PARAM(stat_cache_cooling_hits, IO_OPAQUE_STAT_CACHE_COOLING_HITS);
IO_OPAQUE_STAT_PARAM(stat_cache_probes_failed, IO_OPAQUE_STAT_CACHE_PROBES_FAILED);
IO_OPAQUE_STAT_PARAM(stat_cache_expired, IO_OPAQUE_STAT_CACHE_EXPIRED);
IO_OPAQUE_STAT_PARAM(stat_cache_trimmed, IO_OPAQUE_STAT_CACHE_TRIMMED);
IO_OPAQUE_STAT_PARAM(stat_cache_drained, IO_OPAQUE_STAT_CACHE_DRAINED);
IO_OPAQUE_STAT_PARAM(stat_cached_bytes, IO_OPAQUE_STAT_CACHED_BYTES);
IO_OPAQUE_STAT_PARAM(stat_compact_inline, IO_OPAQUE_STAT_COMPACT_INLINE);
IO_OPAQUE_STAT_PARAM(stat_compact_worker, IO_OPAQUE_STAT_COMPACT_WORKER);
IO_OPAQUE_STAT_PARAM(stat_compact_auto, IO_OPAQUE_STAT_COMPACT_AUTO);
IO_OPAQUE_STAT_PARAM(stat_compact_bytes, IO_OPAQUE_STAT_COMPACT_BYTES);
IO_OPAQUE_STAT_PARAM(stat_framing_copied, IO_OPAQUE_STAT_FRAMING_COPIED);
IO_OPAQUE_STAT_PARAM(stat_body_copied, IO_OPAQUE_STAT_BODY_COPIED);

static void io_opaque_auto_compact(struct io_opaque_store *store);

struct io_opaque_cursor {
	struct io_opaque_extent *extent;
	u32 offset;
};

/*
 * One copy per contiguous source run, cut only where the source extent or the
 * destination chunk ends. Without HIGHMEM a compound page is contiguous in the
 * direct map; HIGHMEM and the compact_copy_per_page tunable keep the original
 * loop, which also cuts at every source and destination page.
 */
static int io_opaque_compact_copy(struct io_opaque_req *op,
				  struct io_opaque_cursor *cursor,
				  struct page *page, u32 length)
{
	bool pages = IS_ENABLED(CONFIG_HIGHMEM) || READ_ONCE(io_opaque_copy_per_page);
	u32 written = 0;

	while (written < length) {
		struct io_opaque_extent *src = cursor->extent;
		u32 pos = src->offset + cursor->offset;
		u32 chunk = min(length - written, src->length - cursor->offset);

		if (READ_ONCE(op->canceled) || READ_ONCE(op->store->dead))
			return -ECANCELED;
		if (pages) {
			u32 in = offset_in_page(pos), out = offset_in_page(written);
			void *from, *to;

			chunk = min_t(u32, chunk, PAGE_SIZE - in);
			chunk = min_t(u32, chunk, PAGE_SIZE - out);
			to = kmap_local_page(page + (written >> PAGE_SHIFT));
			from = kmap_local_page(src->backing->page + (pos >> PAGE_SHIFT));
			memcpy(to + out, from + in, chunk);
			kunmap_local(from);
			kunmap_local(to);
		} else {
			memcpy(page_address(page) + written,
			       page_address(src->backing->page) + pos, chunk);
		}
		written += chunk;
		cursor->offset += chunk;
		if (cursor->offset == src->length) {
			cursor->offset = 0;
			if (!list_is_last(&src->list, &op->data->extents))
				cursor->extent = list_next_entry(src, list);
			else if (written < length)
				return -EIO;
		}
	}
	return 0;
}

struct page *io_opaque_owned_page(unsigned int *order)
{
	struct page *page;

	for (;;) {
		page = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_COMP | __GFP_NORETRY |
				   __GFP_NOWARN, *order);
		if (page || !*order)
			return page;
		(*order)--;
	}
}

void io_opaque_unit_cache_init(struct io_opaque_store *store, u32 limit, u32 order)
{
	struct io_opaque_unit_cache *cache = &store->cache;

	spin_lock_init(&cache->lock);
	INIT_LIST_HEAD(&cache->idle);
	INIT_LIST_HEAD(&cache->cooling);
	cache->limit = limit;
	cache->order = order;
}

/* Sample the tunables for a new store; the cache never exceeds compaction headroom. */
void io_opaque_unit_cache_setup(struct io_opaque_store *store)
{
	u32 order = min_t(u32, READ_ONCE(io_opaque_unit_order), IO_OPAQUE_MAX_ORDER);
	u64 bytes = min_t(u64, READ_ONCE(io_opaque_cache_bytes),
			  store->config.compact_headroom);

	io_opaque_unit_cache_init(store, bytes >> (PAGE_SHIFT + order), order);
}

/*
 * The unit is unreachable from every extent, so nobody can take a new page
 * reference: once only the cache holds it, it stays that way. The acquire
 * pairs with the release in the last outside put_page(), issued after the
 * device or TCP finished reading.
 */
static bool io_opaque_unit_unused(struct page *page)
{
	if (page_count(page) != 1)
		return false;
	smp_acquire__after_ctrl_dep();
	return true;
}

static void io_opaque_unit_release(struct page *page)
{
	set_page_private(page, 0);
	put_page(page);
}

/* Keep a store-owned unit whose last extent reference dropped; false frees it. */
VISIBLE_IF_KUNIT bool io_opaque_unit_retire(struct io_opaque_store *store,
					    struct page *page)
{
	struct io_opaque_unit_cache *cache = &store->cache;
	bool idle;

	if (!cache->limit || compound_order(page) != cache->order ||
	    PageHWPoison(page) || page_is_pfmemalloc(page))
		return false;
	idle = io_opaque_unit_unused(page);
	spin_lock(&cache->lock);
	/* io_opaque_stop() drains under this lock after setting dead. */
	if (READ_ONCE(store->dead)) {
		spin_unlock(&cache->lock);
		return false;
	}
	if (cache->nr_idle + cache->nr_cooling >= cache->limit) {
		cache->trimmed++;
		spin_unlock(&cache->lock);
		io_opaque_count(IO_OPAQUE_STAT_CACHE_TRIMMED, 1);
		return false;
	}
	if (idle) {
		list_add(&page->lru, &cache->idle);
		cache->nr_idle++;
	} else {
		set_page_private(page, jiffies);
		list_add_tail(&page->lru, &cache->cooling);
		cache->nr_cooling++;
		cache->cooled++;
	}
	spin_unlock(&cache->lock);
	io_opaque_count(IO_OPAQUE_STAT_CACHE_RETIRED, 1);
	if (!idle)
		io_opaque_count(IO_OPAQUE_STAT_CACHE_COOLED, 1);
	io_opaque_count(IO_OPAQUE_STAT_CACHED_BYTES, page_size(page));
	return true;
}

void io_opaque_unit_put(struct io_opaque_store *store, struct page *page)
{
	if (!io_opaque_unit_retire(store, page))
		put_page(page);
}

/* Reuse a cached unit: idle first, then cooling units nobody else references. */
VISIBLE_IF_KUNIT struct page *io_opaque_unit_take(struct io_opaque_store *store)
{
	struct io_opaque_unit_cache *cache = &store->cache;
	unsigned int probes = 0, promoted = 0, failed = 0, expired = 0;
	u64 unit = PAGE_SIZE << cache->order;
	struct page *page, *next, *unit_page = NULL;
	LIST_HEAD(drop);

	spin_lock(&cache->lock);
	if (list_empty(&cache->idle)) {
		/* Destinations ACK at different rates: probe a few, not just the head. */
		list_for_each_entry_safe(page, next, &cache->cooling, lru) {
			if (probes++ > IO_OPAQUE_UNIT_PROBES)
				break;
			if (io_opaque_unit_unused(page)) {
				list_move(&page->lru, &cache->idle);
				cache->nr_idle++;
				promoted++;
			} else if (time_after(jiffies, page_private(page) +
					      IO_OPAQUE_UNIT_COOL_MAX)) {
				/* The holder keeps the memory valid; stop waiting for it. */
				list_move(&page->lru, &drop);
				expired++;
			} else {
				failed++;
				continue;
			}
			cache->nr_cooling--;
		}
		cache->cooling_hits += promoted;
		cache->probes_failed += failed;
		cache->expired += expired;
	}
	unit_page = list_first_entry_or_null(&cache->idle, struct page, lru);
	if (unit_page) {
		list_del(&unit_page->lru);
		cache->nr_idle--;
		cache->hits++;
	} else {
		cache->misses++;
	}
	spin_unlock(&cache->lock);

	list_for_each_entry_safe(page, next, &drop, lru) {
		list_del(&page->lru);
		io_opaque_unit_release(page);
	}
	if (promoted)
		io_opaque_count(IO_OPAQUE_STAT_CACHE_COOLING_HITS, promoted);
	if (failed)
		io_opaque_count(IO_OPAQUE_STAT_CACHE_PROBES_FAILED, failed);
	if (expired)
		io_opaque_count(IO_OPAQUE_STAT_CACHE_EXPIRED, expired);
	if (!unit_page) {
		io_opaque_count(IO_OPAQUE_STAT_CACHE_MISSES, 1);
		if (expired)
			io_opaque_count(IO_OPAQUE_STAT_CACHED_BYTES, -(expired * unit));
		return NULL;
	}
	set_page_private(unit_page, 0);
	io_opaque_count(IO_OPAQUE_STAT_CACHE_HITS, 1);
	io_opaque_count(IO_OPAQUE_STAT_CACHED_BYTES, -((expired + 1) * unit));
	if (page_to_nid(unit_page) != numa_mem_id())
		io_opaque_count(IO_OPAQUE_STAT_CACHE_REMOTE, 1);
	return unit_page;
}

/* Teardown: release every cached unit; any outside holder keeps its memory. */
void io_opaque_unit_drain(struct io_opaque_store *store)
{
	struct io_opaque_unit_cache *cache = &store->cache;
	struct page *page, *next;
	LIST_HEAD(drop);
	u32 nr;

	if (!cache->limit)
		return;
	spin_lock(&cache->lock);
	list_splice_init(&cache->idle, &drop);
	list_splice_init(&cache->cooling, &drop);
	nr = cache->nr_idle + cache->nr_cooling;
	cache->nr_idle = 0;
	cache->nr_cooling = 0;
	spin_unlock(&cache->lock);
	list_for_each_entry_safe(page, next, &drop, lru) {
		list_del(&page->lru);
		io_opaque_unit_release(page);
	}
	if (nr) {
		io_opaque_count(IO_OPAQUE_STAT_CACHE_DRAINED, nr);
		io_opaque_count(IO_OPAQUE_STAT_CACHED_BYTES,
				-((u64)nr * (PAGE_SIZE << cache->order)));
	}
}

/*
 * Destination chunk for compaction and CREATE. With the unit cache enabled,
 * every whole unit comes from the cache or is allocated at the unit order;
 * the remainder, and every chunk of a store without a cache, takes the
 * largest order within the remaining size, falling back to smaller orders.
 */
struct page *io_opaque_dest_page(struct io_opaque_store *store, u64 pages_left,
				 unsigned int *order)
{
	struct io_opaque_unit_cache *cache = &store->cache;
	struct page *page;

	KUNIT_STATIC_STUB_REDIRECT(io_opaque_dest_page, store, pages_left, order);
	if (cache->limit && pages_left >= 1ULL << cache->order) {
		*order = cache->order;
		page = io_opaque_unit_take(store);
		if (page)
			return page;
		page = io_opaque_owned_page(order);
		if (page && *order != cache->order)
			io_opaque_count(IO_OPAQUE_STAT_CACHE_FALLBACKS, 1);
		return page;
	}
	*order = min_t(unsigned int, IO_OPAQUE_MAX_ORDER, ilog2(pages_left));
	return io_opaque_owned_page(order);
}

int io_opaque_compact_build(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_cursor cursor;
	u64 done = 0, length = op->data->length;
	u64 charge = ALIGN(length, PAGE_SIZE);
	struct mem_cgroup *old;
	int ret = 0;

	if (READ_ONCE(op->canceled) || READ_ONCE(store->dead))
		return -ECANCELED;
	/* Allocated replacement bytes remain inside this reservation. */
	if (!io_opaque_charge_transient(store, charge,
					store->config.hard_limit - store->config.reply_reserve))
		return -ENOBUFS;
	/* Watermarks ignore the copy until io_opaque_compact_end(). */
	op->transient = charge;
	op->reserved = charge;
	old = set_active_memcg(store->memcg);
	op->replacement = io_opaque_data_alloc();
	if (!op->replacement) {
		ret = -ENOMEM;
		goto out;
	}
	op->replacement->forward_only = op->data->forward_only;
	cursor.extent = list_first_entry(&op->data->extents, struct io_opaque_extent, list);
	cursor.offset = 0;
	while (done < length) {
		u64 pages_left = DIV_ROUND_UP_ULL(length - done, PAGE_SIZE);
		struct io_opaque_extent *extent;
		unsigned int order;
		struct page *page;
		u32 chunk;

		if (READ_ONCE(op->canceled) || READ_ONCE(store->dead)) {
			ret = -ECANCELED;
			goto out;
		}
		page = io_opaque_dest_page(store, pages_left, &order);
		if (!page) {
			ret = -ENOMEM;
			goto out;
		}
		chunk = min_t(u64, length - done, PAGE_SIZE << order);
		extent = io_opaque_extent_new(store, page, 0, chunk, done, true);
		if (IS_ERR(extent)) {
			io_opaque_unit_put(store, page);
			ret = PTR_ERR(extent);
			goto out;
		}
		extent->backing->owned = true;
		op->reserved -= page_size(page);
		list_add_tail(&extent->list, &op->replacement->extents);
		ret = io_opaque_compact_copy(op, &cursor, page, chunk);
		if (ret)
			goto out;
		io_opaque_count(IO_OPAQUE_STAT_COMPACT_BYTES, chunk);
		done += chunk;
		op->replacement->length = done;
		cond_resched();
	}
	ret = io_opaque_vector(op->replacement);
	if (!ret)
		op->replacement->dense = true;
out:
	set_active_memcg(old);
	return ret;
}

/* The copy is published or released; what remains charged is payload held. */
void io_opaque_compact_end(struct io_opaque_req *op)
{
	if (!op->transient)
		return;
	io_opaque_release_transient(op->store, op->reserved, op->transient);
	op->reserved = 0;
	op->transient = 0;
}

int io_opaque_compact_publish(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_slot *slot;
	struct io_opaque_data *old;
	int ret = -ESTALE;

	mutex_lock(&store->tables);
	slot = io_opaque_lookup(store, op->handle);
	if (slot && slot->data == op->data && !store->dead &&
	    !READ_ONCE(op->canceled)) {
		old = slot->data;
		slot->data = op->replacement;
		op->replacement = NULL;
		slot->compacting = false;
		list_del_init(&slot->candidate);
		atomic64_add(old->length, &store->compacted);
		atomic64_inc(&store->compactions);
		ret = 0;
	} else {
		old = NULL;
	}
	mutex_unlock(&store->tables);
	io_opaque_data_put(store, old);
	return ret;
}

void io_opaque_compact_work(struct work_struct *work)
{
	struct io_opaque_store *store = container_of(to_delayed_work(work),
						   struct io_opaque_store, compact_work);
	unsigned long delay = MAX_JIFFY_OFFSET;
	unsigned long now;
	unsigned int i;

	/* Once shutdown is observed, drain every accepted request in this run. */
	for (i = 0; i < 8 || READ_ONCE(store->dead); i++) {
		struct io_opaque_req *op;
		int ret;

		scoped_guard(spinlock, &store->wait_lock) {
			op = list_first_entry_or_null(&store->copies, struct io_opaque_req, wait);
			if (op)
				list_del_init(&op->wait);
		}
		if (!op)
			break;
		ret = io_opaque_compact_build(op);
		/* Cancellation cannot complete a request still owned by this worker. */
		io_opaque_queue_ready(op, ret);
	}
	io_opaque_auto_compact(store);
	/* Keep automatic scans paced even when manual requests wake us early. */
	guard(mutex)(&store->tables);
	now = jiffies;
	if (!store->dead && (store->policy.flags & IORING_OPAQUE_POLICY_F_AUTO_COMPACT) &&
	    !list_empty(&store->candidates))
		delay = time_before(now, store->auto_at) ? store->auto_at - now : 0;
	guard(spinlock)(&store->wait_lock);
	if (!list_empty(&store->copies))
		delay = 0;
	if (delay != MAX_JIFFY_OFFSET)
		queue_delayed_work(system_dfl_wq, &store->compact_work, delay);
}

/*
 * A bounded manual copy runs in the issuer under the ring lock, like CREATE:
 * claim, build, publish and release without the dispatcher or task work. It
 * is never on the dispatcher queue, so there is nothing for cancellation to
 * claim; store shutdown still stops the copy and rejects publication.
 */
static int io_opaque_compact_inline(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_slot *slot;
	int ret;

	ret = io_opaque_compact_build(op);
	if (!ret)
		ret = io_opaque_compact_publish(op);
	scoped_guard(mutex, &store->tables) {
		slot = io_opaque_lookup(store, op->handle);
		if (slot && slot->data == op->data)
			slot->compacting = false;
	}
	io_opaque_data_put(store, op->data);
	op->data = NULL;
	io_opaque_data_put(store, op->replacement);
	op->replacement = NULL;
	io_opaque_compact_end(op);
	return ret;
}

int io_opaque_compact_start(struct io_opaque_req *op)
{
	u32 inline_max = READ_ONCE(io_opaque_inline_compact_max);
	struct io_opaque_store *store = op->store;
	struct io_opaque_slot *slot;

	scoped_guard(mutex, &store->tables) {
		if (store->dead)
			return -ESHUTDOWN;
		slot = io_opaque_lookup(store, op->handle);
		if (!slot || !slot->data)
			return -ESTALE;
		if (slot->compacting)
			return -EBUSY;
		if (slot->data->dense)
			return 0;
		op->data = slot->data;
		refcount_inc(&op->data->refs);
		slot->compacting = true;
		if (!inline_max || op->data->length > inline_max) {
			io_opaque_wait(op, IO_OPAQUE_COPY_WORK);
			scoped_guard(spinlock, &store->wait_lock)
				list_add_tail(&op->wait, &store->copies);
			mod_delayed_work(system_dfl_wq, &store->compact_work, 0);
			io_opaque_count(IO_OPAQUE_STAT_COMPACT_WORKER, 1);
			return IOU_ISSUE_SKIP_COMPLETE;
		}
	}
	io_opaque_count(IO_OPAQUE_STAT_COMPACT_INLINE, 1);
	return io_opaque_compact_inline(op);
}

int io_opaque_set_policy(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_uring_opaque_policy policy;
	u32 i;
	int ret;

	if (op->length < offsetof(struct io_uring_opaque_policy, __resv) ||
	    op->length > PAGE_SIZE)
		return -EINVAL;
	ret = copy_struct_from_user(&policy, sizeof(policy), op->addr, op->length);
	if (ret)
		return ret;
	if ((policy.flags & ~IORING_OPAQUE_POLICY_F_AUTO_COMPACT) ||
	    policy.__resv0 || !mem_is_zero(policy.__resv, sizeof(policy.__resv)) ||
	    policy.bytes_per_second > SZ_1T || policy.burst_bytes > store->config.hard_limit ||
	    policy.max_temporary_bytes > store->config.hard_limit)
		return -EINVAL;
	if ((policy.flags & IORING_OPAQUE_POLICY_F_AUTO_COMPACT) &&
	    (!policy.bytes_per_second || !policy.burst_bytes ||
	     policy.max_temporary_bytes < PAGE_SIZE))
		return -EINVAL;
	mutex_lock(&store->tables);
	if (store->dead) {
		mutex_unlock(&store->tables);
		return -ESHUTDOWN;
	}
	store->policy = policy;
	store->tokens = policy.burst_bytes;
	store->token_time = jiffies;
	store->auto_at = jiffies;
	for (i = 0; i < store->config.max_objects; i++) {
		struct io_opaque_slot *slot = &store->objects[i];

		if ((policy.flags & IORING_OPAQUE_POLICY_F_AUTO_COMPACT) &&
		    slot->data && !slot->data->dense && !slot->data->forward_only &&
		    list_empty(&slot->candidate)) {
			list_add_tail(&slot->candidate, &store->candidates);
			slot->retry = jiffies;
		} else if (!(policy.flags & IORING_OPAQUE_POLICY_F_AUTO_COMPACT)) {
			list_del_init(&slot->candidate);
		}
	}
	if (policy.flags & IORING_OPAQUE_POLICY_F_AUTO_COMPACT) {
		mod_delayed_work(system_dfl_wq, &store->compact_work, 0);
	} else {
		guard(spinlock)(&store->wait_lock);
		/* Policy changes cannot cancel an accepted manual request. */
		if (list_empty(&store->copies))
			cancel_delayed_work(&store->compact_work);
	}
	mutex_unlock(&store->tables);
	return 0;
}

/* The same dispatcher handles bounded scans and the store-wide token bucket. */
static void io_opaque_auto_compact(struct io_opaque_store *store)
{
	struct io_opaque_req op = { .store = store };
	struct io_opaque_slot *slot = NULL;
	struct io_uring_opaque_policy policy;
	u64 minted, elapsed;
	unsigned int scanned = 0;
	int ret;

	mutex_lock(&store->tables);
	policy = store->policy;
	if (store->dead || !(policy.flags & IORING_OPAQUE_POLICY_F_AUTO_COMPACT) ||
	    time_before(jiffies, store->auto_at))
		goto unlock;
	store->auto_at = jiffies + msecs_to_jiffies(100);
	elapsed = jiffies - store->token_time;
	if (check_mul_overflow(elapsed, policy.bytes_per_second, &minted))
		minted = policy.burst_bytes;
	else
		minted = div_u64(minted, HZ);
	store->tokens = min(policy.burst_bytes, store->tokens + min(minted, policy.burst_bytes));
	store->token_time = jiffies;
	while (!list_empty(&store->candidates) && scanned++ < 32) {
		u64 dense, saving;

		slot = list_first_entry(&store->candidates, struct io_opaque_slot, candidate);
		list_move_tail(&slot->candidate, &store->candidates);
		if (!slot->data || slot->data->dense) {
			list_del_init(&slot->candidate);
			continue;
		}
		if (slot->compacting || time_before(jiffies, slot->retry) ||
		    time_before(jiffies, slot->born + msecs_to_jiffies(100)))
			continue;
		dense = ALIGN(slot->data->length, PAGE_SIZE);
		saving = slot->data->charge > dense ? slot->data->charge - dense : 0;
		/* Selection heuristics are implementation details, not UAPI. */
		if ((!saving && slot->data->nr == 1) ||
		    (saving && saving * 100 < slot->data->charge * 25) ||
		    dense > policy.max_temporary_bytes || slot->data->length > policy.burst_bytes) {
			/* Immutable backing cannot become eligible without a new policy. */
			list_del_init(&slot->candidate);
			continue;
		}
		if (slot->data->length > store->tokens)
			continue;
		slot->compacting = true;
		store->tokens -= slot->data->length;
		op.data = slot->data;
		refcount_inc(&op.data->refs);
		op.handle = io_opaque_handle(slot - store->objects, slot->generation);
		break;
	}
unlock:
	mutex_unlock(&store->tables);
	if (op.data) {
		io_opaque_count(IO_OPAQUE_STAT_COMPACT_AUTO, 1);
		ret = io_opaque_compact_build(&op);
		if (!ret)
			ret = io_opaque_compact_publish(&op);
		mutex_lock(&store->tables);
		slot = io_opaque_lookup(store, op.handle);
		if (slot && slot->data == op.data) {
			slot->compacting = false;
			if (ret)
				slot->retry = jiffies + HZ;
		}
		mutex_unlock(&store->tables);
		io_opaque_data_put(store, op.data);
		io_opaque_data_put(store, op.replacement);
		io_opaque_compact_end(&op);
	}
}
