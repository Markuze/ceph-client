// SPDX-License-Identifier: GPL-2.0
/* Workers copy immutable backing without holding object table locks. */
#include <linux/highmem.h>
#include <linux/memcontrol.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "io_uring.h"
#include "opaque_internal.h"

static void io_opaque_auto_compact(struct io_opaque_store *store);

struct io_opaque_cursor {
	struct io_opaque_extent *extent;
	u32 offset;
};

static int io_opaque_compact_copy(struct io_opaque_req *op,
				  struct io_opaque_cursor *cursor,
				  struct page *page, u32 length)
{
	u32 written = 0;

	while (written < length) {
		struct io_opaque_extent *src = cursor->extent;
		u32 pos = src->offset + cursor->offset;
		u32 in = offset_in_page(pos), out = offset_in_page(written);
		u32 chunk = min(length - written, src->length - cursor->offset);
		void *from, *to;

		if (READ_ONCE(op->canceled) || READ_ONCE(op->store->dead))
			return -ECANCELED;
		chunk = min_t(u32, chunk, PAGE_SIZE - in);
		chunk = min_t(u32, chunk, PAGE_SIZE - out);
		to = kmap_local_page(page + (written >> PAGE_SHIFT));
		from = kmap_local_page(src->backing->page + (pos >> PAGE_SHIFT));
		memcpy(to + out, from + in, chunk);
		kunmap_local(from);
		kunmap_local(to);
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

static struct page *io_opaque_compact_page(unsigned int *order)
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
	if (!io_opaque_charge(store, charge, false))
		return -ENOBUFS;
	op->reserved = charge;
	old = set_active_memcg(store->memcg);
	op->replacement = io_opaque_data_alloc();
	if (!op->replacement) {
		ret = -ENOMEM;
		goto out;
	}
	cursor.extent = list_first_entry(&op->data->extents, struct io_opaque_extent, list);
	cursor.offset = 0;
	while (done < length) {
		u64 pages_left = DIV_ROUND_UP_ULL(length - done, PAGE_SIZE);
		unsigned int order = min_t(unsigned int, IO_OPAQUE_MAX_ORDER,
					   ilog2(pages_left));
		struct io_opaque_extent *extent;
		struct page *page;
		u32 chunk;

		if (READ_ONCE(op->canceled) || READ_ONCE(store->dead)) {
			ret = -ECANCELED;
			goto out;
		}
		page = io_opaque_compact_page(&order);
		if (!page) {
			ret = -ENOMEM;
			goto out;
		}
		chunk = min_t(u64, length - done, PAGE_SIZE << order);
		extent = io_opaque_extent_new(store, page, 0, chunk, done, true);
		if (IS_ERR(extent)) {
			put_page(page);
			ret = PTR_ERR(extent);
			goto out;
		}
		op->reserved -= page_size(page);
		list_add_tail(&extent->list, &op->replacement->extents);
		ret = io_opaque_compact_copy(op, &cursor, page, chunk);
		if (ret)
			goto out;
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

	/* Shutdown drains every accepted request, without another invocation. */
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

int io_opaque_compact_start(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_slot *slot;

	guard(mutex)(&store->tables);
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
	io_opaque_wait(op, IO_OPAQUE_COPY_WORK);
	scoped_guard(spinlock, &store->wait_lock)
		list_add_tail(&op->wait, &store->copies);
	mod_delayed_work(system_dfl_wq, &store->compact_work, 0);
	return IOU_ISSUE_SKIP_COMPLETE;
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
		    slot->data && !slot->data->dense && list_empty(&slot->candidate)) {
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
		if (op.reserved)
			io_opaque_uncharge(store, op.reserved);
	}
}
