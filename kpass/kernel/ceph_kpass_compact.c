// SPDX-License-Identifier: GPL-2.0
/*
 * Included by the object implementation. Compaction copies into new backing
 * on a separate worker outside stream_lock. Metadata updates and publication
 * hold the lock; operations holding the old object keep using that backing.
 */

#define KPASS_COMPACT_MAX_ORDER	(PAGE_SHIFT < 16 ? 16 - PAGE_SHIFT : 0)

struct kpass_compact_cursor {
	struct kpass_extent *extent;
	u32 offset;
};

static struct page *kpass_compact_pages(unsigned int *order)
{
	struct page *page;

	for (;;) {
		page = alloc_pages(GFP_KERNEL | __GFP_COMP | __GFP_NORETRY |
				   __GFP_NOWARN, *order);
		if (page || !*order)
			return page;
		(*order)--;
	}
}

static int kpass_compact_copy_chunk(struct kpass_request *req,
				    struct kpass_compact_cursor *cursor,
				    struct page *page, u32 length)
{
	u32 written = 0;

	while (written < length) {
		struct kpass_extent *src = cursor->extent;
		u32 pos = src->offset + cursor->offset;
		u32 in = offset_in_page(pos), out = offset_in_page(written);
		u32 chunk = min(length - written, src->length - cursor->offset);
		void *from, *to;

		if (READ_ONCE(req->canceled))
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
			if (!list_is_last(&src->list, &req->object->extents))
				cursor->extent = list_next_entry(src, list);
			else if (written < length)
				return -EIO;
		}
	}
	return 0;
}

/* Caller holds stream_lock; reserve both old and new backing at once. */
static int kpass_compact_reserve(struct kpass_request *req)
{
	struct kpass_session *sess = req->session;
	u64 charge = ALIGN(req->object->length, PAGE_SIZE);

	if (req->canceled)
		return -ECANCELED;
	if (kpass_object_lookup(sess, req->cmd.stream.object) != req->object)
		return -ESTALE;
	if (charge > sess->backing_limit - sess->backing_bytes)
		return -ENOBUFS;
	sess->backing_bytes += charge;
	req->compact_reserved = charge;
	return 0;
}

static int kpass_compact_build(struct kpass_request *req, unsigned int max_order)
{
	struct kpass_session *sess = req->session;
	struct kpass_compact_cursor cursor;
	u64 done = 0, length = req->object->length;
	int ret;

	mutex_lock(&sess->stream_lock);
	ret = kpass_compact_reserve(req);
	mutex_unlock(&sess->stream_lock);
	if (ret)
		return ret;

	req->replacement = kpass_object_alloc();
	if (!req->replacement) {
		ret = -ENOMEM;
		goto out;
	}
	if (!length || list_empty(&req->object->extents)) {
		ret = -EINVAL;
		goto out;
	}
	cursor.extent = list_first_entry(&req->object->extents,
					 struct kpass_extent, list);
	cursor.offset = 0;
	while (done < length) {
		struct kpass_extent *extent;
		struct kpass_backing *backing;
		struct page *page;
		unsigned int order;
		u32 chunk;

		mutex_lock(&sess->stream_lock);
		ret = req->canceled ? -ECANCELED : 0;
		if (kpass_object_lookup(sess, req->cmd.stream.object) != req->object)
			ret = -ESTALE;
		if (!ret && sess->extent_count == KPASS_MAX_EXTENTS)
			ret = -ENOSPC;
		mutex_unlock(&sess->stream_lock);
		if (ret)
			break;

		order = min_t(unsigned int, max_order,
			      ilog2(DIV_ROUND_UP_ULL(length - done, PAGE_SIZE)));
		page = kpass_compact_pages(&order);
		extent = kmalloc_obj(*extent);
		backing = kmalloc_obj(*backing);
		if (!page || !extent || !backing) {
			if (page)
				put_page(page);
			kfree(extent);
			kfree(backing);
			ret = -ENOMEM;
			break;
		}
		backing->charge = PAGE_SIZE << order;
		backing->page = page;
		refcount_set(&backing->refs, 1);
		chunk = min_t(u64, length - done, backing->charge);
		extent->backing = backing;
		extent->start = done;
		extent->offset = 0;
		extent->length = chunk;
		mutex_lock(&sess->stream_lock);
		if (sess->extent_count == KPASS_MAX_EXTENTS) {
			mutex_unlock(&sess->stream_lock);
			put_page(page);
			kfree(extent);
			kfree(backing);
			ret = -ENOSPC;
			break;
		}
		/* The allocation's charge was reserved before dropping the lock. */
		req->compact_reserved -= backing->charge;
		sess->extent_count++;
		list_add_tail(&extent->list, &req->replacement->extents);
		mutex_unlock(&sess->stream_lock);
		ret = kpass_compact_copy_chunk(req, &cursor, page, chunk);
		if (ret)
			break;
		done += chunk;
		cond_resched();
	}
	if (!ret) {
		req->replacement->length = length;
		req->replacement->compacted = true;
	}
out:
	mutex_lock(&sess->stream_lock);
	sess->backing_bytes -= req->compact_reserved;
	req->compact_reserved = 0;
	if (ret) {
		kpass_object_put(sess, req->replacement);
		req->replacement = NULL;
	}
	mutex_unlock(&sess->stream_lock);
	return ret;
}

/* Called in completion task work while holding stream_lock. */
static int kpass_compact_publish(struct kpass_request *req, int result)
{
	struct kpass_session *sess = req->session;

	if (result < 0)
		return result;
	if (req->canceled)
		return -ECANCELED;
	if (kpass_object_lookup(sess, req->cmd.stream.object) != req->object)
		return -ESTALE;
	sess->objects[(u32)req->cmd.stream.object - 1] = req->replacement;
	req->replacement = NULL; /* table adopts the new backing */
	kpass_object_put(sess, req->object); /* drop the old table reference */
	return 0;
}

static void kpass_compact_work_fn(struct work_struct *work)
{
	struct kpass_request *req = container_of(work, struct kpass_request,
						compact_work);
	struct kpass_session *sess = req->session;
	int ret;

	ret = kpass_compact_build(req, KPASS_COMPACT_MAX_ORDER);
	mutex_lock(&sess->stream_lock);
	kpass_request_finish(req, ret);
	queue_work(sess->wq, &sess->stream_work);
	mutex_unlock(&sess->stream_lock);
}

/* Return 1 when a worker is needed, 0 for already compacted, or an error. */
static int kpass_compact_claim(struct kpass_request *req)
{
	struct kpass_object *object;

	if (req->cmd.stream.offset || req->cmd.stream.length || req->cmd.stream.addr)
		return -EINVAL;
	object = kpass_object_lookup(req->session, req->cmd.stream.object);
	if (!object)
		return -ESTALE;
	if (object->compacting)
		return -EBUSY;
	if (object->compacted)
		return 0;
	refcount_inc(&object->refs);
	object->compacting = true;
	req->object = object;
	req->compact_claimed = true;
	req->sock = NULL;
	return 1;
}

static int kpass_compact_start(struct kpass_request *req)
{
	int ret = kpass_compact_claim(req);

	if (ret != 1)
		return ret;
	INIT_WORK(&req->compact_work, kpass_compact_work_fn);
	list_add_tail(&req->list, &req->session->requests);
	queue_work(req->session->compact_wq, &req->compact_work);
	return -EIOCBQUEUED;
}

static int kpass_object_stat(struct kpass_session *sess,
			     const struct kpass_sqe_cmd *cmd)
{
	struct kpass_object *object = kpass_object_lookup(sess, cmd->stream.object);
	struct kpass_object_stat stat = {};
	struct kpass_extent *extent;

	if (!object)
		return -ESTALE;
	stat.length = object->length;
	if (object->compacted)
		stat.flags |= KPASS_OBJECT_COMPACTED;
	if (object->compacting)
		stat.flags |= KPASS_OBJECT_COMPACTING;
	list_for_each_entry(extent, &object->extents, list) {
		stat.backing_bytes += extent->backing->charge;
		stat.extents++;
	}
	return copy_to_user(u64_to_user_ptr(cmd->stream.addr), &stat, sizeof(stat)) ?
		-EFAULT : 0;
}
