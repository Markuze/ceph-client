// SPDX-License-Identifier: GPL-2.0
/* Included by opaque.c: workers copy immutable backing without table locks. */
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

static int io_opaque_compact_build(struct io_opaque_req *op)
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

static int io_opaque_compact_publish(struct io_opaque_req *op)
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

static void io_opaque_compact_work(struct work_struct *work)
{
	struct io_opaque_req *op = container_of(work, struct io_opaque_req, work);
	int ret = io_opaque_compact_build(op);

	/* Cancellation cannot complete the request while this worker owns it. */
	io_opaque_queue_ready(op, ret);
}

static int io_opaque_compact_start(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_opaque_slot *slot;

	guard(mutex)(&store->tables);
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
	INIT_WORK(&op->work, io_opaque_compact_work);
	io_opaque_wait(op, IO_OPAQUE_COPY_WORK);
	queue_work(store->wq, &op->work);
	return IOU_ISSUE_SKIP_COMPLETE;
}

static int io_opaque_set_policy(struct io_opaque_req *op)
{
	struct io_opaque_store *store = op->store;
	struct io_uring_opaque_policy policy;
	u32 i;

	if (op->length != sizeof(policy))
		return -EINVAL;
	if (copy_from_user(&policy, op->addr, sizeof(policy)))
		return -EFAULT;
	if ((policy.flags & ~IORING_OPAQUE_AUTO_COMPACT) ||
	    !mem_is_zero(policy.__resv, sizeof(policy.__resv)) ||
	    policy.min_slack_percent > 100 || policy.min_age_ms > 86400000 ||
	    policy.bytes_per_second > SZ_1T || policy.burst_bytes > store->config.hard_limit ||
	    policy.max_temporary_bytes > store->config.hard_limit)
		return -EINVAL;
	if ((policy.flags & IORING_OPAQUE_AUTO_COMPACT) &&
	    (!policy.min_age_ms || !policy.bytes_per_second || !policy.burst_bytes ||
	     policy.max_temporary_bytes < PAGE_SIZE))
		return -EINVAL;
	mutex_lock(&store->tables);
	store->policy = policy;
	store->tokens = policy.burst_bytes;
	store->token_time = jiffies;
	for (i = 0; i < store->config.max_objects; i++) {
		struct io_opaque_slot *slot = &store->objects[i];

		if ((policy.flags & IORING_OPAQUE_AUTO_COMPACT) &&
		    slot->data && !slot->data->dense && list_empty(&slot->candidate)) {
			list_add_tail(&slot->candidate, &store->candidates);
			slot->retry = jiffies;
		} else if (!(policy.flags & IORING_OPAQUE_AUTO_COMPACT)) {
			list_del_init(&slot->candidate);
		}
	}
	mutex_unlock(&store->tables);
	if (policy.flags & IORING_OPAQUE_AUTO_COMPACT)
		mod_delayed_work(store->wq, &store->auto_work, 1);
	else
		cancel_delayed_work_sync(&store->auto_work);
	return 0;
}

/* Bounded candidate scans, one copy at a time, and a context-wide token bucket. */
static void io_opaque_auto_work(struct work_struct *work)
{
	struct io_opaque_store *store = container_of(to_delayed_work(work),
						   struct io_opaque_store, auto_work);
	struct io_opaque_req op = { .store = store };
	struct io_opaque_slot *slot = NULL;
	struct io_uring_opaque_policy policy;
	u64 minted, elapsed;
	unsigned int scanned = 0;
	bool again;
	int ret;

	mutex_lock(&store->tables);
	policy = store->policy;
	if (store->dead || !(policy.flags & IORING_OPAQUE_AUTO_COMPACT))
		goto unlock;
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
		    time_before(jiffies, slot->born + msecs_to_jiffies(policy.min_age_ms)))
			continue;
		dense = ALIGN(slot->data->length, PAGE_SIZE);
		saving = slot->data->charge > dense ? slot->data->charge - dense : 0;
		if (saving < policy.min_saving || slot->data->nr < policy.min_extents ||
		    saving * 100 < slot->data->charge * policy.min_slack_percent ||
		    dense > policy.max_temporary_bytes || slot->data->length > store->tokens)
			continue;
		slot->compacting = true;
		store->tokens -= slot->data->length;
		op.data = slot->data;
		refcount_inc(&op.data->refs);
		op.handle = (u64)slot->generation << 32 | (slot - store->objects + 1);
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
	mutex_lock(&store->tables);
	again = !store->dead && (store->policy.flags & IORING_OPAQUE_AUTO_COMPACT) &&
		!list_empty(&store->candidates);
	mutex_unlock(&store->tables);
	if (again)
		queue_delayed_work(store->wq, &store->auto_work, msecs_to_jiffies(100));
}
