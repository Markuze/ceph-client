// SPDX-License-Identifier: GPL-2.0
/* Included after the stream tests; exercise the real copy and publication. */

static u8 kpass_compact_test_byte(u64 offset)
{
	return offset * 37 + offset / 503;
}

static u64 kpass_compact_test_source(struct kunit *test, unsigned int length)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	struct kpass_request *keep;
	unsigned int pos = 0;
	u64 handle;

	while (pos < length) {
		unsigned int i, chunk = min(length - pos, 2000U);
		struct sk_buff *skb = kpass_test_page_skb(test, 0, 96, chunk);

		if (!skb)
			return 0;
		for (i = 0; i < chunk; i++)
			skb->data[i] = kpass_compact_test_byte(pos + i);
		if (kpass_test_stream_capture(test, skb) != chunk)
			return 0;
		pos += chunk;
	}
	keep = kpass_test_request(test, KPASS_OP_KEEP, 0, length);
	if (!keep)
		return 0;
	kpass_stream_process(&ctx->session);
	if (!keep->ready || keep->result != length)
		return 0;
	handle = kpass_object_publish(&ctx->session, keep->object);
	if (handle)
		keep->object = NULL;
	return handle;
}

static void kpass_compact_test_cleanup(void *ptr)
{
	struct kpass_request *req = ptr;

	list_del_init(&req->list);
	if (req->compact_claimed)
		req->object->compacting = false;
	kpass_object_put(req->session, req->replacement);
	kpass_object_put(req->session, req->object);
}

static struct kpass_request *kpass_compact_test_request(struct kunit *test,
						       u64 handle)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	struct kpass_request *req;

	req = kunit_kzalloc(test, sizeof(*req), GFP_KERNEL);
	if (!req)
		return NULL;
	req->session = &ctx->session;
	req->cmd.op = KPASS_OP_OBJECT_COMPACT;
	req->cmd.stream.object = handle;
	INIT_LIST_HEAD(&req->list);
	if (kunit_add_action_or_reset(test, kpass_compact_test_cleanup, req))
		return NULL;
	if (kpass_compact_claim(req) != 1)
		return NULL;
	return req;
}

static void kpass_compact_expect_bytes(struct kunit *test,
				      struct kpass_object *object,
				      unsigned int length)
{
	struct kpass_extent *extent;
	u8 *expected = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u64 cursor = 0;

	KUNIT_ASSERT_NOT_NULL(test, expected);
	KUNIT_ASSERT_EQ(test, object->length, (u64)length);
	list_for_each_entry(extent, &object->extents, list) {
		unsigned int pos = 0;

		KUNIT_ASSERT_EQ(test, extent->start, cursor);
		while (pos < extent->length) {
			u32 offset = extent->offset + pos;
			u32 in = offset_in_page(offset);
			u32 chunk = min_t(u32, extent->length - pos, PAGE_SIZE - in);
			void *mapped;
			unsigned int i;

			for (i = 0; i < chunk; i++)
				expected[i] = kpass_compact_test_byte(cursor + i);
			mapped = kmap_local_page(extent->backing->page +
						(offset >> PAGE_SHIFT));
			KUNIT_EXPECT_MEMEQ(test, mapped + in, expected, chunk);
			kunmap_local(mapped);
			pos += chunk;
			cursor += chunk;
		}
	}
	KUNIT_EXPECT_EQ(test, cursor, (u64)length);
}

static void kpass_compact_packing_test(struct kunit *test)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	const unsigned int length = 80000;
	u64 handle = kpass_compact_test_source(test, length);
	struct kpass_request duplicate = { .session = &ctx->session };
	struct kpass_request *req;
	struct kpass_object *source, *packed;
	struct kpass_extent *extent;
	u64 old_charge, new_charge = 0;
	unsigned int count = 0;

	KUNIT_ASSERT_NE(test, handle, 0ULL);
	source = kpass_object_lookup(&ctx->session, handle);
	old_charge = ctx->session.backing_bytes;
	req = kpass_compact_test_request(test, handle);
	KUNIT_ASSERT_NOT_NULL(test, req);
	duplicate.cmd.stream.object = handle;
	KUNIT_EXPECT_EQ(test, kpass_compact_claim(&duplicate), -EBUSY);
	KUNIT_ASSERT_EQ(test, kpass_compact_build(req, KPASS_COMPACT_MAX_ORDER), 0);
	KUNIT_EXPECT_PTR_EQ(test, kpass_object_lookup(&ctx->session, handle), source);
	kpass_compact_expect_bytes(test, req->replacement, length);
	list_for_each_entry(extent, &req->replacement->extents, list) {
		unsigned int i, pages = extent->backing->charge >> PAGE_SHIFT;
		struct page *page = extent->backing->page;

		KUNIT_EXPECT_EQ(test, extent->offset, 0U);
		KUNIT_EXPECT_LE(test, extent->backing->charge, (u64)SZ_64K);
		KUNIT_EXPECT_EQ(test, page_size(page), (unsigned long)extent->backing->charge);
		for (i = 0; i < pages; i++)
			KUNIT_EXPECT_EQ(test, page_to_pfn(page + i), page_to_pfn(page) + i);
		new_charge += extent->backing->charge;
		count++;
	}
	KUNIT_EXPECT_EQ(test, new_charge, (u64)PAGE_ALIGN(length));
	KUNIT_EXPECT_LT(test, count, 40U);
	KUNIT_EXPECT_EQ(test, ctx->session.backing_bytes, old_charge + new_charge);
	KUNIT_ASSERT_EQ(test, kpass_compact_publish(req, 0), 0);
	packed = kpass_object_lookup(&ctx->session, handle);
	KUNIT_ASSERT_PTR_NE(test, packed, source);
	KUNIT_EXPECT_TRUE(test, packed->compacted);
	/* The request still pins the exact old backing, as an in-flight send does. */
	kpass_compact_expect_bytes(test, source, length);
	kpass_compact_expect_bytes(test, packed, length);
	KUNIT_EXPECT_EQ(test, kpass_compact_claim(&duplicate), 0);
	kunit_release_action(test, kpass_compact_test_cleanup, req);
	KUNIT_EXPECT_EQ(test, ctx->session.backing_bytes, new_charge);
	kunit_info(test, "compaction: 40 extents -> %u; charge %llu -> %llu bytes\n",
		   count, old_charge, new_charge);
}

static void kpass_compact_base_pages_test(struct kunit *test)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	const unsigned int length = 4 * PAGE_SIZE + 17;
	u64 handle = kpass_compact_test_source(test, length);
	struct kpass_request *req;
	struct kpass_extent *extent;
	unsigned int count = 0;

	KUNIT_ASSERT_NE(test, handle, 0ULL);
	req = kpass_compact_test_request(test, handle);
	KUNIT_ASSERT_NOT_NULL(test, req);
	/* Exercise the representation used when high-order allocations fall back. */
	KUNIT_ASSERT_EQ(test, kpass_compact_build(req, 0), 0);
	list_for_each_entry(extent, &req->replacement->extents, list) {
		KUNIT_EXPECT_EQ(test, extent->backing->charge, (u64)PAGE_SIZE);
		KUNIT_EXPECT_EQ(test, extent->offset, 0U);
		count++;
	}
	KUNIT_EXPECT_EQ(test, count, 5U);
	kpass_compact_expect_bytes(test, req->replacement, length);
	KUNIT_ASSERT_EQ(test, kpass_compact_publish(req, 0), 0);
	KUNIT_EXPECT_PTR_EQ(test, kpass_object_lookup(&ctx->session, handle),
			    ctx->session.objects[(u32)handle - 1]);
}

static void kpass_compact_budget_test(struct kunit *test)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	u64 handle = kpass_compact_test_source(test, 6000);
	struct kpass_request *req;
	u64 charge = ctx->session.backing_bytes;

	KUNIT_ASSERT_NE(test, handle, 0ULL);
	req = kpass_compact_test_request(test, handle);
	KUNIT_ASSERT_NOT_NULL(test, req);
	ctx->session.backing_limit = charge + PAGE_ALIGN(6000) - 1;
	KUNIT_EXPECT_EQ(test, kpass_compact_build(req, KPASS_COMPACT_MAX_ORDER), -ENOBUFS);
	KUNIT_EXPECT_PTR_EQ(test, req->replacement, NULL);
	KUNIT_EXPECT_EQ(test, req->compact_reserved, 0ULL);
	KUNIT_EXPECT_EQ(test, ctx->session.backing_bytes, charge);
	KUNIT_EXPECT_PTR_EQ(test, kpass_object_lookup(&ctx->session, handle), req->object);
	kpass_compact_expect_bytes(test, req->object, 6000);
}

static void kpass_compact_cancel_test(struct kunit *test)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	u64 handle = kpass_compact_test_source(test, 80000);
	struct kpass_request *req;
	u64 charge = ctx->session.backing_bytes;

	KUNIT_ASSERT_NE(test, handle, 0ULL);
	req = kpass_compact_test_request(test, handle);
	KUNIT_ASSERT_NOT_NULL(test, req);
	kpass_request_cancel(req);
	KUNIT_EXPECT_FALSE(test, req->ready);
	KUNIT_EXPECT_EQ(test, kpass_compact_build(req, KPASS_COMPACT_MAX_ORDER), -ECANCELED);
	KUNIT_EXPECT_EQ(test, ctx->session.backing_bytes, charge);
	/* Cancellation after the copy must also leave the published object intact. */
	req->canceled = false;
	KUNIT_ASSERT_EQ(test, kpass_compact_build(req, KPASS_COMPACT_MAX_ORDER), 0);
	kpass_request_cancel(req);
	KUNIT_EXPECT_EQ(test, kpass_compact_publish(req, 0), -ECANCELED);
	KUNIT_EXPECT_PTR_EQ(test, kpass_object_lookup(&ctx->session, handle), req->object);
	kunit_release_action(test, kpass_compact_test_cleanup, req);
	KUNIT_EXPECT_EQ(test, ctx->session.backing_bytes, charge);
}

static void kpass_compact_stale_test(struct kunit *test)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	u64 handle = kpass_compact_test_source(test, 80000), replacement_handle;
	struct kpass_request *req;
	struct kpass_object *replacement;

	KUNIT_ASSERT_NE(test, handle, 0ULL);
	req = kpass_compact_test_request(test, handle);
	KUNIT_ASSERT_NOT_NULL(test, req);
	KUNIT_ASSERT_EQ(test, kpass_compact_build(req, KPASS_COMPACT_MAX_ORDER), 0);
	ctx->session.objects[(u32)handle - 1] = NULL;
	ctx->session.object_count--;
	kpass_object_put(&ctx->session, req->object); /* free the public handle */
	replacement = kpass_object_alloc();
	KUNIT_ASSERT_NOT_NULL(test, replacement);
	replacement_handle = kpass_object_publish(&ctx->session, replacement);
	KUNIT_EXPECT_EQ(test, (u32)replacement_handle, (u32)handle);
	KUNIT_EXPECT_NE(test, replacement_handle, handle);
	KUNIT_EXPECT_EQ(test, kpass_compact_publish(req, 0), -ESTALE);
	KUNIT_EXPECT_PTR_EQ(test, kpass_object_lookup(&ctx->session, replacement_handle),
			    replacement);
	kunit_release_action(test, kpass_compact_test_cleanup, req);
	KUNIT_EXPECT_EQ(test, ctx->session.backing_bytes, 0ULL);
}

static void kpass_compact_rollback_test(struct kunit *test)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	u64 handle = kpass_compact_test_source(test, 80000);
	u64 charge = ctx->session.backing_bytes;
	u32 count = ctx->session.extent_count;
	struct kpass_request *req;
	int ret;

	KUNIT_ASSERT_NE(test, handle, 0ULL);
	req = kpass_compact_test_request(test, handle);
	KUNIT_ASSERT_NOT_NULL(test, req);
	/* One destination extent fits; the second hits the metadata cap. */
	ctx->session.extent_count = KPASS_MAX_EXTENTS - 1;
	ret = kpass_compact_build(req, KPASS_COMPACT_MAX_ORDER);
	KUNIT_EXPECT_EQ(test, ctx->session.extent_count, KPASS_MAX_EXTENTS - 1U);
	ctx->session.extent_count = count;
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	KUNIT_EXPECT_EQ(test, req->compact_reserved, 0ULL);
	KUNIT_EXPECT_PTR_EQ(test, req->replacement, NULL);
	KUNIT_EXPECT_EQ(test, ctx->session.backing_bytes, charge);
	KUNIT_EXPECT_PTR_EQ(test, kpass_object_lookup(&ctx->session, handle), req->object);
	kpass_compact_expect_bytes(test, req->object, 80000);
}

static struct kunit_case kpass_compact_test_cases[] = {
	KUNIT_CASE(kpass_compact_packing_test),
	KUNIT_CASE(kpass_compact_base_pages_test),
	KUNIT_CASE(kpass_compact_budget_test),
	KUNIT_CASE(kpass_compact_cancel_test),
	KUNIT_CASE(kpass_compact_stale_test),
	KUNIT_CASE(kpass_compact_rollback_test),
	{}
};

static struct kunit_suite kpass_compact_test_suite = {
	.name = "kpass-compact",
	.init = kpass_stream_test_init,
	.test_cases = kpass_compact_test_cases,
};

kunit_test_suite(kpass_compact_test_suite);
