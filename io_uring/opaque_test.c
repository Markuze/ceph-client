// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <linux/highmem.h>
#include <linux/skbuff.h>
#include <net/tcp.h>

#include "opaque_internal.h"
#ifdef CONFIG_IO_URING_ZCRX_KUNIT_TEST
#include <net/page_pool/helpers.h>
#include "zcrx.h"
#endif

static struct io_opaque_store *opaque_test_store(struct kunit *test)
{
	struct io_opaque_store *store = kunit_kzalloc(test, sizeof(*store), GFP_KERNEL);
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, store);
	store->config.hard_limit = 128 * PAGE_SIZE;
	store->config.max_extents = 256;
	store->config.max_objects = 4;
	store->objects = kunit_kcalloc(test, 4, sizeof(*store->objects), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, store->objects);
	store->object_used = kunit_kzalloc(test, BITS_TO_LONGS(4) * sizeof(unsigned long),
					   GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, store->object_used);
	mutex_init(&store->tables);
	spin_lock_init(&store->wait_lock);
	spin_lock_init(&store->transient_lock);
	seqcount_spinlock_init(&store->transient_seq, &store->transient_lock);
	INIT_LIST_HEAD(&store->budget_waits);
	INIT_LIST_HEAD(&store->candidates);
	for (i = 0; i < 4; i++)
		INIT_LIST_HEAD(&store->objects[i].candidate);
	return store;
}

static void opaque_test_stream(struct io_opaque_stream *stream,
			       struct io_opaque_store *store)
{
	memset(stream, 0, sizeof(*stream));
	stream->store = store;
	INIT_LIST_HEAD(&stream->extents);
	INIT_LIST_HEAD(&stream->reads);
	stream->decisions = RB_ROOT_CACHED;
}

static struct sk_buff *opaque_test_frag(struct kunit *test, struct page *page,
					unsigned int offset, unsigned int length)
{
	struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	get_page(page);
	skb_add_rx_frag(skb, 0, page, offset, length, page_size(compound_head(page)));
	return skb;
}

static void opaque_capture_identity(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct page *page = alloc_page(GFP_KERNEL);
	struct sk_buff *skb;

	KUNIT_ASSERT_NOT_NULL(test, page);
	opaque_test_stream(&stream, store);
	skb = opaque_test_frag(test, page, 128, 1024);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 1024, false), 1024);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, page);
	KUNIT_EXPECT_EQ(test, extent->offset, 128U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 0LL);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, page_count(page), 2);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	put_page(page);
}

static void opaque_capture_clone_copy(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct sk_buff *skb = alloc_skb(32, GFP_KERNEL), *clone;
	void *mapped;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	memset(skb_put(skb, 32), 0x5a, 32);
	clone = skb_clone(skb, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, clone);
	opaque_test_stream(&stream, store);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, clone, 0, 32, false), 32);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 32LL);
	memset(skb->data, 0xa5, 32);
	kfree_skb(skb);
	kfree_skb(clone);
	mapped = kmap_local_page(extent->backing->page);
	KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[0], (u8)0x5a);
	KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[31], (u8)0x5a);
	kunmap_local(mapped);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

static void opaque_capture_shared_copy(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct page *page = alloc_page(GFP_KERNEL);
	struct sk_buff *skb;
	void *mapped;

	KUNIT_ASSERT_NOT_NULL(test, page);
	opaque_test_stream(&stream, store);
	mapped = kmap_local_page(page);
	memset(mapped, 0x65, 32);
	kunmap_local(mapped);
	skb = opaque_test_frag(test, page, 0, 32);
	skb_get(skb);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 32, false), 32);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_NE(test, extent->backing->page, page);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 32LL);
	mapped = kmap_local_page(page);
	memset(mapped, 0x98, 32);
	kunmap_local(mapped);
	kfree_skb(skb);
	kfree_skb(skb);
	mapped = kmap_local_page(extent->backing->page);
	KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[0], (u8)0x65);
	KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[31], (u8)0x65);
	kunmap_local(mapped);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	put_page(page);
}

static void opaque_capture_compound(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct page *page = alloc_pages(GFP_KERNEL | __GFP_COMP, 1);
	struct sk_buff *skb;

	KUNIT_ASSERT_NOT_NULL(test, page);
	opaque_test_stream(&stream, store);
	skb = opaque_test_frag(test, page, 0, PAGE_SIZE + 128);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, PAGE_SIZE + 128, false),
			(int)PAGE_SIZE + 128);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, page);
	KUNIT_EXPECT_EQ(test, extent->length, (u32)PAGE_SIZE + 128);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)2 * PAGE_SIZE);
	kfree_skb(skb);
	io_opaque_extents_free(store, &stream.extents);
	put_page(page);
}

static void opaque_capture_oversized(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct page *page = alloc_pages(GFP_KERNEL | __GFP_COMP, 1);
	struct sk_buff *skb;
	void *mapped;

	KUNIT_ASSERT_NOT_NULL(test, page);
	store->config.hard_limit = PAGE_SIZE;
	opaque_test_stream(&stream, store);
	mapped = kmap_local_page(page);
	memset(mapped, 0x37, 32);
	kunmap_local(mapped);
	skb = opaque_test_frag(test, page, 0, 32);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 32, false), 32);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_NE(test, extent->backing->page, page);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 32LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE);
	mapped = kmap_local_page(extent->backing->page);
	KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[0], (u8)0x37);
	KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[31], (u8)0x37);
	kunmap_local(mapped);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	put_page(page);
}

static void opaque_capture_budget(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct page *first = alloc_page(GFP_KERNEL), *second = alloc_page(GFP_KERNEL);
	struct sk_buff *a, *b;

	KUNIT_ASSERT_NOT_NULL(test, first);
	KUNIT_ASSERT_NOT_NULL(test, second);
	store->config.hard_limit = PAGE_SIZE;
	opaque_test_stream(&stream, store);
	a = opaque_test_frag(test, first, 0, 32);
	b = opaque_test_frag(test, second, 0, 32);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, a, 0, 32, false), 32);
	stream.next = 32;
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, b, 0, 32, false), -ENOBUFS);
	KUNIT_EXPECT_EQ(test, page_count(second), 2);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 1);
	kfree_skb(a);
	kfree_skb(b);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	put_page(first);
	put_page(second);
}

static void opaque_capture_gaps(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_data *data = io_opaque_data_alloc();
	struct page *page = alloc_pages(GFP_KERNEL | __GFP_COMP, 2);
	struct sk_buff *skb;
	int i;

	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_NOT_NULL(test, page);
	opaque_test_stream(&stream, store);
	for (i = 0; i < 8; i++) {
		skb = opaque_test_frag(test, page, i * 1024, 512);
		KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 512, false), 512);
		stream.next += 512;
		kfree_skb(skb);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)4 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 8);
	KUNIT_EXPECT_EQ(test, page_count(page), 2);
	list_splice_init(&stream.extents, &data->extents);
	data->length = stream.next;
	KUNIT_ASSERT_EQ(test, io_opaque_vector(data), 0);
	KUNIT_EXPECT_EQ(test, data->charge, (u64)4 * PAGE_SIZE);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	put_page(page);
}

static void opaque_capture_copy_pack(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	int i;

	opaque_test_stream(&stream, store);
	for (i = 0; i < 16; i++) {
		struct sk_buff *skb = alloc_skb(32, GFP_KERNEL);

		KUNIT_ASSERT_NOT_NULL(test, skb);
		memset(skb_put(skb, 32), i, 32);
		skb_get(skb);
		KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 32, false), 32);
		stream.next += 32;
		kfree_skb(skb);
		kfree_skb(skb);
	}
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_EQ(test, extent->length, 512U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 512LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 1);
	for (i = 0; i < 16; i++) {
		void *mapped = kmap_local_page(extent->backing->page);

		KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[i * 32], (u8)i);
		KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[i * 32 + 31], (u8)i);
		kunmap_local(mapped);
	}
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

static void opaque_capture_partial_pressure(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct page *first = alloc_page(GFP_KERNEL), *second = alloc_page(GFP_KERNEL);
	struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);
	read_descriptor_t desc = { .count = PAGE_SIZE, .arg.data = &stream };

	KUNIT_ASSERT_NOT_NULL(test, first);
	KUNIT_ASSERT_NOT_NULL(test, second);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	store->config.hard_limit = PAGE_SIZE;
	opaque_test_stream(&stream, store);
	get_page(first);
	get_page(second);
	skb_add_rx_frag(skb, 0, first, 0, 512, PAGE_SIZE);
	skb_add_rx_frag(skb, 1, second, 0, 512, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, 1024), 512);
	KUNIT_EXPECT_EQ(test, desc.error, -ENOBUFS);
	KUNIT_EXPECT_EQ(test, stream.next, 512ULL);
	kfree_skb(skb);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	put_page(first);
	put_page(second);
}

static void opaque_capture_private_budget(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_data *data = io_opaque_data_alloc();
	struct page *page = alloc_pages(GFP_KERNEL | __GFP_COMP, 2);
	struct io_opaque_req op = {
		.store = store, .stream = &stream, .data = data,
		.op = IORING_OPAQUE_RECV_OBJECT, .length = 8 * PAGE_SIZE,
	};
	int i;

	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_NOT_NULL(test, page);
	store->config.hard_limit = 8 * PAGE_SIZE;
	opaque_test_stream(&stream, store);
	KUNIT_ASSERT_EQ(test, io_opaque_range_insert(&op), 0);
	for (i = 0; i < 32; i++) {
		struct sk_buff *skb = opaque_test_frag(test, page, (i % 8) * PAGE_SIZE / 2,
						      PAGE_SIZE / 4);

		KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, PAGE_SIZE / 4, false),
				(int)PAGE_SIZE / 4);
		stream.next += PAGE_SIZE / 4;
		kfree_skb(skb);
	}
	KUNIT_EXPECT_EQ(test, data->length, (u64)8 * PAGE_SIZE);
	KUNIT_EXPECT_LE(test, atomic64_read(&store->bytes), (s64)8 * PAGE_SIZE);
	KUNIT_EXPECT_GT(test, atomic64_read(&store->copied), 0LL);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
	put_page(page);
}

static void opaque_split_shared_charge(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_extent *left, *right;
	struct page *page = alloc_page(GFP_KERNEL);
	LIST_HEAD(extents);

	KUNIT_ASSERT_NOT_NULL(test, page);
	left = io_opaque_extent_new(store, page, 0, 128, 0, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(left));
	list_add(&left->list, &extents);
	right = io_opaque_extent_split(store, left, 64);
	KUNIT_ASSERT_FALSE(test, IS_ERR(right));
	KUNIT_EXPECT_PTR_EQ(test, left->backing, right->backing);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE);
	io_opaque_extent_free(store, left);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE);
	io_opaque_extent_free(store, right);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

static void opaque_keep_restore(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_data *data = io_opaque_data_alloc();
	struct page *page = alloc_page(GFP_KERNEL);
	struct io_opaque_extent *extent;
	struct io_opaque_req op = {
		.store = store, .stream = &stream, .data = data,
		.op = IORING_OPAQUE_RECV_OBJECT, .length = 4,
	};

	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_NOT_NULL(test, page);
	opaque_test_stream(&stream, store);
	mutex_init(&stream.lock);
	extent = io_opaque_extent_new(store, page, 0, 8, 0, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
	list_add(&extent->list, &stream.extents);
	stream.next = 8;
	stream.undecided = 8;
	mutex_lock(&stream.lock);
	KUNIT_EXPECT_EQ(test, io_opaque_take(&op), 1);
	KUNIT_EXPECT_EQ(test, stream.undecided, 4ULL);
	io_opaque_restore(&op);
	KUNIT_EXPECT_EQ(test, stream.undecided, 8ULL);
	KUNIT_EXPECT_EQ(test, io_opaque_available(&stream, 0, 8), 8);
	KUNIT_EXPECT_TRUE(test, list_empty(&data->extents));
	mutex_unlock(&stream.lock);
	io_opaque_data_put(store, data);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
	mutex_destroy(&stream.lock);
}

static struct io_opaque_data *opaque_sparse_data(struct kunit *test,
						 struct io_opaque_store *store)
{
	struct io_opaque_data *data = io_opaque_data_alloc();
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, data);
	for (i = 0; i < 40; i++) {
		struct page *page = alloc_page(GFP_KERNEL);
		struct io_opaque_extent *extent;
		void *mapped;

		KUNIT_ASSERT_NOT_NULL(test, page);
		mapped = kmap_local_page(page);
		memset(mapped, i, 2000);
		kunmap_local(mapped);
		extent = io_opaque_extent_new(store, page, 0, 2000, i * 2000, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
		list_add_tail(&extent->list, &data->extents);
		data->length += 2000;
	}
	KUNIT_ASSERT_EQ(test, io_opaque_vector(data), 0);
	return data;
}

static void opaque_test_copy(struct io_opaque_data *data, u8 *buffer)
{
	struct io_opaque_extent *extent;

	list_for_each_entry(extent, &data->extents, list) {
		u32 done = 0;

		while (done < extent->length) {
			u32 pos = extent->offset + done;
			u32 len = min_t(u32, extent->length - done,
					PAGE_SIZE - offset_in_page(pos));
			void *mapped = kmap_local_page(extent->backing->page + (pos >> PAGE_SHIFT));

			memcpy(buffer + extent->start + done, mapped + offset_in_page(pos), len);
			kunmap_local(mapped);
			done += len;
		}
	}
}

static void opaque_compact_versions(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req op = {
		.store = store, .data = data,
		.handle = 9ULL << IO_OPAQUE_INDEX_BITS,
	};
	u8 *before = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *after = kunit_kmalloc(test, 80000, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, before);
	KUNIT_ASSERT_NOT_NULL(test, after);
	store->objects[0].data = data;
	store->nr_objects++;
	__set_bit(0, store->object_used);
	store->objects[0].generation = 9;
	refcount_add(2, &data->refs); /* worker and an old read/send version */
	opaque_test_copy(data, before);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
	KUNIT_EXPECT_EQ(test, op.reserved, 0ULL);
	KUNIT_EXPECT_EQ(test, op.replacement->charge, ALIGN(80000ULL, PAGE_SIZE));
	KUNIT_EXPECT_LE(test, op.replacement->nr, (u32)DIV_ROUND_UP(80000, PAGE_SIZE));
	opaque_test_copy(op.replacement, after);
	KUNIT_EXPECT_MEMEQ(test, before, after, 80000);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)(40 * PAGE_SIZE + ALIGN(80000, PAGE_SIZE)));
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&op), 0);
	KUNIT_EXPECT_EQ(test, store->objects[0].generation, 9ULL);
	KUNIT_EXPECT_PTR_NE(test, store->objects[0].data, data);
	io_opaque_data_put(store, data); /* worker */
	io_opaque_compact_end(&op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)(40 * PAGE_SIZE + ALIGN(80000, PAGE_SIZE)));
	io_opaque_data_put(store, data); /* old read/send */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)ALIGN(80000, PAGE_SIZE));
	io_opaque_data_put(store, store->objects[0].data);
	store->objects[0].data = NULL;
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static void opaque_compact_watermarks(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req op = {
		.store = store, .data = data, .handle = 9ULL << IO_OPAQUE_INDEX_BITS,
	};
	u64 sparse = 40 * PAGE_SIZE, dense = ALIGN(80000, PAGE_SIZE);

	/* Counting the copy would cross the high watermark. */
	store->config.high_watermark = sparse + PAGE_SIZE;
	store->config.low_watermark = PAGE_SIZE;
	store->objects[0].data = data;
	store->objects[0].generation = 9;
	store->nr_objects++;
	__set_bit(0, store->object_used);
	refcount_inc(&data->refs); /* worker */
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_NORMAL);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)(sparse + dense));
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_NORMAL);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&op), 0);
	io_opaque_data_put(store, op.data); /* worker */
	io_opaque_compact_end(&op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)dense);
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_NORMAL);
	io_opaque_data_put(store, store->objects[0].data);
	store->objects[0].data = NULL;
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

static void opaque_compact_reservation(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req op = { .store = store, .data = data };
	s64 old = atomic64_read(&store->bytes);

	store->config.hard_limit = old + ALIGN(data->length, PAGE_SIZE) - PAGE_SIZE;
	KUNIT_EXPECT_EQ(test, io_opaque_compact_build(&op), -ENOBUFS);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), old);
	KUNIT_EXPECT_PTR_EQ(test, op.replacement, NULL);
	KUNIT_EXPECT_EQ(test, op.reserved, 0ULL);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

static void opaque_compact_canceled(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req op = { .store = store, .data = data, .canceled = true };
	s64 old = atomic64_read(&store->bytes);

	KUNIT_EXPECT_EQ(test, io_opaque_compact_build(&op), -ECANCELED);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), old);
	KUNIT_EXPECT_EQ(test, op.reserved, 0ULL);
	io_opaque_data_put(store, data);
}

static void opaque_generation_retirement(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_req op = { .store = store };

	store->objects[0].generation = IO_OPAQUE_GENERATION_MAX;
	store->objects[1].reserved = true;
	__set_bit(1, store->object_used);
	KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&op), 0);
	KUNIT_EXPECT_EQ(test, op.handle, (1ULL << IO_OPAQUE_INDEX_BITS) | 2);
	KUNIT_EXPECT_EQ(test, store->objects[0].generation, IO_OPAQUE_GENERATION_MAX);
}

static void opaque_slot_rotation(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_req op = { .store = store };
	u32 i;

	for (i = 0; i < store->config.max_objects; i++) {
		KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&op), 0);
		KUNIT_EXPECT_EQ(test, op.slot, i);
	}
	store->object_cursor = 2;
	KUNIT_EXPECT_EQ(test, io_opaque_slot_reserve(&op), -ENOSPC);
	store->objects[1].reserved = false;
	io_opaque_release_slot(store->object_used, 1, store->objects[1].generation);
	KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&op), 0);
	KUNIT_EXPECT_EQ(test, op.slot, 1U);
	KUNIT_EXPECT_EQ(test, store->objects[1].generation, 2ULL);
}

static void opaque_generation_width(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_req op = { .store = store };

	store->objects[0].generation = U32_MAX;
	KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&op), 0);
	KUNIT_EXPECT_EQ(test, store->objects[0].generation, (u64)U32_MAX + 1);
	mutex_lock(&store->tables);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_lookup(store, op.handle), &store->objects[0]);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_lookup(store, io_opaque_handle(0, U32_MAX)), NULL);
	mutex_unlock(&store->tables);
}

static void opaque_send_last_ownership(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_slot *slot = &store->objects[0];
	struct io_opaque_req send = {
		.store = store, .handle = 9ULL << IO_OPAQUE_INDEX_BITS,
		.offset = 79999, .length = 2, .send_flags = IORING_OPAQUE_SEND_LAST,
	};
	struct io_opaque_req other = {
		.store = store, .handle = send.handle, .length = 1,
		.send_flags = IORING_OPAQUE_SEND_LAST,
	};
	u8 actual[12], expected[12] = { 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

	slot->data = data;
	store->nr_objects++;
	__set_bit(0, store->object_used);
	slot->generation = 9;
	slot->compacting = true;
	list_add(&slot->candidate, &store->candidates);
	refcount_inc(&data->refs); /* an already issued read/send */
	mutex_lock(&store->tables);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(&send), -ERANGE);
	KUNIT_EXPECT_FALSE(test, send.consumed);
	KUNIT_EXPECT_PTR_EQ(test, slot->data, data);
	KUNIT_EXPECT_FALSE(test, list_empty(&slot->candidate));
	send.offset = 1997;
	send.length = sizeof(actual);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(&send), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(&other), -ESTALE);
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_TRUE(test, send.consumed);
	KUNIT_EXPECT_FALSE(test, other.consumed);
	KUNIT_EXPECT_PTR_EQ(test, slot->data, NULL);
	KUNIT_EXPECT_FALSE(test, slot->compacting);
	KUNIT_EXPECT_TRUE(test, list_empty(&store->candidates));
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	KUNIT_EXPECT_EQ(test, copy_from_iter(actual, sizeof(actual), &send.iter), sizeof(actual));
	KUNIT_EXPECT_MEMEQ(test, actual, expected, sizeof(actual));
	io_opaque_data_put(store, send.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)40 * PAGE_SIZE);
	io_opaque_data_put(store, data); /* the issued read/send */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static void opaque_send_last_compacted(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store), *replacement;
	struct io_opaque_req compact = {
		.store = store, .data = data, .handle = 9ULL << IO_OPAQUE_INDEX_BITS,
	};
	struct io_opaque_req send = {
		.store = store, .handle = compact.handle, .length = 80000,
		.send_flags = IORING_OPAQUE_SEND_LAST,
	};

	store->objects[0].data = data;
	store->nr_objects++;
	__set_bit(0, store->object_used);
	store->objects[0].generation = 9;
	refcount_inc(&data->refs); /* compaction worker */
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	replacement = compact.replacement;
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	mutex_lock(&store->tables);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(&send), 0);
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_PTR_EQ(test, send.data, replacement);
	KUNIT_EXPECT_EQ(test, refcount_read(&replacement->refs), 1);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&send.iter), (size_t)80000);
	io_opaque_data_put(store, compact.data);
	io_opaque_data_put(store, send.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static void opaque_send_last_compact_race(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store), *next;
	struct io_opaque_req compact = {
		.store = store, .data = data, .handle = 9ULL << IO_OPAQUE_INDEX_BITS,
	};
	struct io_opaque_req send = {
		.store = store, .handle = compact.handle, .length = 80000,
		.send_flags = IORING_OPAQUE_SEND_LAST,
	};
	struct io_opaque_req reuse = { .store = store };

	store->objects[0].data = data;
	store->nr_objects++;
	__set_bit(0, store->object_used);
	store->objects[0].generation = 9;
	refcount_inc(&data->refs); /* compaction worker */
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	mutex_lock(&store->tables);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(&send), 0);
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, io_opaque_compact_publish(&compact), -ESTALE);
	KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&reuse), 0);
	next = opaque_sparse_data(test, store);
	store->objects[0].data = next;
	store->nr_objects++;
	store->objects[0].reserved = false;
	store->objects[0].compacting = true;
	KUNIT_EXPECT_EQ(test, reuse.handle, 10ULL << IO_OPAQUE_INDEX_BITS);
	KUNIT_EXPECT_EQ(test, io_opaque_compact_publish(&compact), -ESTALE);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, next);
	KUNIT_EXPECT_TRUE(test, store->objects[0].compacting);
	io_opaque_data_put(store, compact.replacement);
	io_opaque_data_put(store, compact.data);
	io_opaque_data_put(store, send.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)40 * PAGE_SIZE);
	mutex_lock(&store->tables);
	next = io_opaque_detach(store, &store->objects[0]);
	mutex_unlock(&store->tables);
	io_opaque_data_put(store, next);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static void opaque_send_versions(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store), *replacement;
	struct io_opaque_req compact = {
		.store = store, .data = data, .handle = 9ULL << IO_OPAQUE_INDEX_BITS,
	};
	struct io_opaque_req first = {
		.store = store, .handle = compact.handle, .length = 80000,
	};
	struct io_opaque_req second = first;

	store->objects[0].data = data;
	store->nr_objects++;
	__set_bit(0, store->object_used);
	store->objects[0].generation = 9;
	mutex_lock(&store->tables);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(&first), 0);
	mutex_unlock(&store->tables);
	refcount_inc(&data->refs); /* compaction worker */
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	replacement = compact.replacement;
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	mutex_lock(&store->tables);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(&second), 0);
	data = io_opaque_detach(store, &store->objects[0]);
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_PTR_EQ(test, first.data, compact.data);
	KUNIT_EXPECT_PTR_EQ(test, second.data, replacement);
	KUNIT_EXPECT_FALSE(test, first.consumed);
	KUNIT_EXPECT_FALSE(test, second.consumed);
	io_opaque_data_put(store, data);
	io_opaque_data_put(store, compact.data);
	io_opaque_data_put(store, first.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)ALIGN(80000, PAGE_SIZE));
	io_opaque_data_put(store, second.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static void opaque_inspection_windows(struct kunit *test)
{
	struct io_opaque_stream stream;
	struct io_opaque_extent first = { .start = 0, .length = 22 };
	struct io_opaque_extent next = { .start = 22, .length = 8 };
	struct io_opaque_extent after_gap = { .start = 40, .length = 4 };

	opaque_test_stream(&stream, NULL);
	stream.next = 44;
	list_add_tail(&first.list, &stream.extents);
	list_add_tail(&next.list, &stream.extents);
	list_add_tail(&after_gap.list, &stream.extents);
	KUNIT_EXPECT_EQ(test, io_opaque_available(&stream, 0, 512), 30);
	KUNIT_EXPECT_EQ(test, io_opaque_available(&stream, 0, 10), 10);
	KUNIT_EXPECT_EQ(test, io_opaque_available(&stream, 4, 512), 26);
	KUNIT_EXPECT_EQ(test, io_opaque_available(&stream, 30, 512), -ENODATA);
	KUNIT_EXPECT_EQ(test, io_opaque_available(&stream, 40, 512), 4);
	KUNIT_EXPECT_EQ(test, io_opaque_available(&stream, 44, 512), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_available(&stream, 48, 512), 0);
}

static void opaque_memory_watermarks(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);

	store->config.high_watermark = 4 * PAGE_SIZE;
	store->config.low_watermark = 2 * PAGE_SIZE;
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, 3 * PAGE_SIZE, true));
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_NORMAL);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, PAGE_SIZE, true));
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_LOWMEM);
	io_opaque_uncharge(store, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_LOWMEM);
	io_opaque_uncharge(store, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_NORMAL);
	io_opaque_uncharge(store, 2 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

static void opaque_reply_reserve(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	bool reserved;

	store->config.hard_limit = 8 * PAGE_SIZE;
	store->config.compact_headroom = 2 * PAGE_SIZE;
	store->config.reply_reserve = 2 * PAGE_SIZE;
	/* Framing uses ordinary capacity first. */
	KUNIT_ASSERT_TRUE(test, io_opaque_charge_frame(store, PAGE_SIZE, &reserved));
	KUNIT_EXPECT_FALSE(test, reserved);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, 3 * PAGE_SIZE, true));
	KUNIT_EXPECT_FALSE(test, io_opaque_charge(store, 1, true));
	/* With ordinary capacity full, only framing may use the reserve. */
	KUNIT_ASSERT_TRUE(test, io_opaque_charge_frame(store, 2 * PAGE_SIZE, &reserved));
	KUNIT_EXPECT_TRUE(test, reserved);
	KUNIT_EXPECT_FALSE(test, io_opaque_charge_frame(store, 1, &reserved));
	/* Compaction may use its headroom, never the reserve. */
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, 2 * PAGE_SIZE, false));
	KUNIT_EXPECT_FALSE(test, io_opaque_charge(store, 1, false));
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes) +
			atomic64_read(&store->reserve_framing), (s64)store->config.hard_limit);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->framing), (s64)3 * PAGE_SIZE);
	io_opaque_uncharge_frame(store, 2 * PAGE_SIZE, true);
	io_opaque_uncharge_frame(store, PAGE_SIZE, false);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)5 * PAGE_SIZE);
	io_opaque_uncharge(store, 5 * PAGE_SIZE);
	store->config.reply_reserve = 0;
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, 6 * PAGE_SIZE, true));
	KUNIT_EXPECT_FALSE(test, io_opaque_charge_frame(store, 1, &reserved));
	io_opaque_uncharge(store, 6 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->framing), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->reserve_framing), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
}

static void opaque_collect_gate(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_req discard = {
		.store = store, .stream = &stream, .op = IORING_OPAQUE_DISCARD,
	};
	read_descriptor_t desc = { .count = 4 * PAGE_SIZE, .arg.data = &stream };
	struct page *page = alloc_page(GFP_KERNEL);
	struct list_head waiting;
	struct sk_buff *skb;

	KUNIT_ASSERT_NOT_NULL(test, page);
	opaque_test_stream(&stream, store);
	mutex_init(&stream.lock);
	/* Open gate: undecided bytes are retained, not copied. */
	skb = opaque_test_frag(test, page, 0, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, PAGE_SIZE), (int)PAGE_SIZE);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, desc.error, 0);
	KUNIT_EXPECT_EQ(test, stream.next, (u64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, stream.undecided, (u64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 0LL);
	/* Held gate: no further undecided bytes leave the socket. */
	stream.hold_undecided = true;
	skb = opaque_test_frag(test, page, 0, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, PAGE_SIZE), 0);
	KUNIT_EXPECT_EQ(test, desc.error, 0);
	KUNIT_EXPECT_EQ(test, stream.next, (u64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, stream.undecided, (u64)PAGE_SIZE);
	/* Decided bytes pass a held gate; the DISCARD must not complete here. */
	discard.offset = stream.next;
	discard.length = 2 * PAGE_SIZE;
	KUNIT_ASSERT_EQ(test, io_opaque_range_insert(&discard), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, PAGE_SIZE), (int)PAGE_SIZE);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, discard.progress, (u64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, stream.next, (u64)2 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, stream.undecided, (u64)PAGE_SIZE);
	rb_erase_cached(&discard.decision, &stream.decisions);
	/* The predicate. */
	mutex_lock(&stream.lock);
	KUNIT_EXPECT_FALSE(test, io_opaque_collect_demand(&stream));
	list_add(&waiting, &stream.reads);
	KUNIT_EXPECT_TRUE(test, io_opaque_collect_demand(&stream));
	list_del(&waiting);
	discard.offset = stream.next + PAGE_SIZE;
	discard.length = PAGE_SIZE;
	KUNIT_ASSERT_EQ(test, io_opaque_range_insert(&discard), 0);
	KUNIT_EXPECT_TRUE(test, io_opaque_collect_demand(&stream));
	rb_erase_cached(&discard.decision, &stream.decisions);
	io_opaque_extents_free(store, &stream.extents);
	stream.undecided = 0;
	KUNIT_EXPECT_TRUE(test, io_opaque_collect_demand(&stream));
	mutex_unlock(&stream.lock);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	put_page(page);
}

static void opaque_forward_only_versions(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req compact = {
		.store = store, .data = data, .handle = 9ULL << IO_OPAQUE_INDEX_BITS,
	};
	struct io_opaque_req send = {
		.store = store, .handle = compact.handle, .length = 80000,
	};
	struct io_opaque_req last = {
		.store = store, .handle = compact.handle, .length = 80000,
		.send_flags = IORING_OPAQUE_SEND_LAST,
	};

	data->forward_only = true;
	store->objects[0].data = data;
	store->objects[0].generation = 9;
	store->nr_objects++;
	__set_bit(0, store->object_used);
	refcount_inc(&data->refs);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	KUNIT_EXPECT_TRUE(test, compact.replacement->forward_only);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	mutex_lock(&store->tables);
	/* The classification is advisory: an ordinary send pins a version. */
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(&send), 0);
	KUNIT_EXPECT_FALSE(test, send.consumed);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(&last), 0);
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_TRUE(test, last.consumed);
	KUNIT_EXPECT_TRUE(test, send.data->forward_only);
	KUNIT_EXPECT_PTR_EQ(test, send.data, last.data);
	io_opaque_data_put(store, compact.data);
	io_opaque_compact_end(&compact);
	io_opaque_data_put(store, send.data);
	io_opaque_data_put(store, last.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

#ifdef CONFIG_IO_URING_ZCRX_KUNIT_TEST
/*
 * Tests of a store that binds a MAIO instance. The aids of zcrx_test.c create
 * the instance and a page pool that uses its provider, without a device. Every
 * test returns all pages before it ends, and the deferred actions of the aids
 * destroy the pool and the instance.
 */
#define OPAQUE_BATCH	PP_ALLOC_CACHE_REFILL

/* A store that binds the instance. The test owns the reference to it. */
static struct io_opaque_store *opaque_maio_store(struct kunit *test,
						 struct io_zcrx_ifq *ifq)
{
	struct io_opaque_store *store = opaque_test_store(test);

	store->maio = ifq;
	return store;
}

/* A page that the pool owns, as the driver gets it for a receive buffer. */
static struct page *opaque_pool_page(struct kunit *test, struct page_pool *pp)
{
	netmem_ref netmem = page_pool_alloc_netmems(pp, GFP_KERNEL);

	KUNIT_ASSERT_NE(test, (unsigned long)netmem, 0UL);
	return netmem_to_page(netmem);
}

/* Give the pool reference of the driver back, as the stack does for an skb. */
static void opaque_pool_free(struct page_pool *pp, struct page *page)
{
	page_pool_put_unrefed_netmem(pp, page_to_netmem(page), -1, true);
}

/* The skb of a driver: a pool page in a fragment, and the pool reference. */
static struct sk_buff *opaque_pool_skb(struct kunit *test, struct page *page,
				       unsigned int offset, unsigned int length)
{
	struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_add_rx_frag(skb, 0, page, offset, length, PAGE_SIZE);
	skb_mark_for_recycle(skb);
	return skb;
}

/* Capture len bytes of the skb, the way the collector does in several calls. */
static void opaque_maio_take(struct kunit *test, struct io_opaque_stream *stream,
			     const struct sk_buff *skb, u32 len)
{
	u32 done = 0;

	while (done < len) {
		int ret = io_opaque_capture(stream, skb, done, len - done, false);

		KUNIT_ASSERT_GT(test, ret, 0);
		stream->next += ret;
		done += ret;
	}
}

/* Make an object of everything that the stream holds, as RECV_OBJECT does. */
static struct io_opaque_data *opaque_maio_object(struct kunit *test,
						 struct io_opaque_stream *stream)
{
	struct io_opaque_data *data = io_opaque_data_alloc();

	KUNIT_ASSERT_NOT_NULL(test, data);
	list_splice_init(&stream->extents, &data->extents);
	data->length = stream->next;
	KUNIT_ASSERT_EQ(test, io_opaque_vector(data), 0);
	return data;
}

static u32 opaque_slots_in(struct io_zcrx_ifq *ifq, int state)
{
	struct io_zcrx_area *area = ifq->areas[0];
	u32 i, n = 0;

	for (i = 0; i < area->nia.num_niovs; i++)
		n += area->kmem->state[i] == state;
	return n;
}

static u32 opaque_slot_state(struct kunit *test, struct io_zcrx_ifq *ifq, struct page *page)
{
	struct io_zcrx_area *area = ifq->areas[0];
	u32 idx;

	KUNIT_ASSERT_TRUE(test, io_zcrx_kmem_lookup(area, page, &idx));
	return area->kmem->state[idx];
}

/* Registration binds the instance, keeps a reference and releases it at the end. */
static void opaque_maio_bind(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);
	struct io_ring_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct io_uring_opaque_config cfg = {
		.hard_limit = 64 * PAGE_SIZE, .max_object_size = SZ_1M,
		.max_objects = 4, .max_streams = 2, .max_extents = 64,
		.max_requests = 4, .flags = IORING_OPAQUE_CFG_MAIO,
	};
	struct user_struct *user = kunit_kzalloc(test, sizeof(*user), GFP_KERNEL);
	struct io_opaque_store *store;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_NOT_NULL(test, user);
	mutex_init(&ctx->uring_lock);
	xa_init_flags(&ctx->zcrx_ctxs, XA_FLAGS_ALLOC);
	KUNIT_ASSERT_EQ(test, xa_err(xa_store(&ctx->zcrx_ctxs, 0, ifq, GFP_KERNEL)), 0);
	mutex_lock(&ctx->uring_lock);

	/* The flag and the id of the instance bind the store. */
	store = io_opaque_alloc(ctx, &cfg);
	KUNIT_ASSERT_FALSE(test, IS_ERR(store));
	KUNIT_EXPECT_PTR_EQ(test, store->maio, ifq);
	KUNIT_EXPECT_EQ(test, refcount_read(&ifq->refs), 2U);
	io_opaque_free(store);
	KUNIT_EXPECT_EQ(test, refcount_read(&ifq->refs), 1U);

	/* The ring has no instance with this id. */
	cfg.zcrx_id = 1;
	KUNIT_EXPECT_EQ(test, PTR_ERR(io_opaque_alloc(ctx, &cfg)), -ENXIO);
	/* An id needs the flag, and no other flag exists. */
	cfg.flags = 0;
	KUNIT_EXPECT_EQ(test, PTR_ERR(io_opaque_alloc(ctx, &cfg)), -EINVAL);
	cfg.zcrx_id = 0;
	cfg.flags = IORING_OPAQUE_CFG_MAIO << 1;
	KUNIT_EXPECT_EQ(test, PTR_ERR(io_opaque_alloc(ctx, &cfg)), -EINVAL);
	cfg.flags = IORING_OPAQUE_CFG_MAIO;
	cfg.__resv1 = 1;
	KUNIT_EXPECT_EQ(test, PTR_ERR(io_opaque_alloc(ctx, &cfg)), -EINVAL);
	cfg.__resv1 = 0;
	/* A failure after the lookup gives the reference back. */
	ctx->user = user;
	atomic_long_set(&user->locked_vm, LONG_MAX / 2);
	KUNIT_EXPECT_EQ(test, PTR_ERR(io_opaque_alloc(ctx, &cfg)), -ENOMEM);
	KUNIT_EXPECT_EQ(test, refcount_read(&ifq->refs), 1U);
	ctx->user = NULL;

	/* A store without the flag does not bind. */
	cfg.flags = 0;
	store = io_opaque_alloc(ctx, &cfg);
	KUNIT_ASSERT_FALSE(test, IS_ERR(store));
	KUNIT_EXPECT_NULL(test, store->maio);
	KUNIT_EXPECT_EQ(test, refcount_read(&ifq->refs), 1U);
	io_opaque_free(store);

	mutex_unlock(&ctx->uring_lock);
	xa_destroy(&ctx->zcrx_ctxs);
}

/*
 * A fragment in a pool page of the instance, with the skb marked for recycling:
 * the store takes a pool reference and marks the backing. The pool gets the
 * page back at the release, and not the freelist.
 */
static void opaque_maio_pool_ref(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 2 * OPAQUE_BATCH);
	struct page_pool *pp = io_zcrx_maio_test_pool(test, ifq);
	struct io_opaque_store *store = opaque_maio_store(test, ifq);
	struct io_zcrx_area *area = ifq->areas[0];
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct io_opaque_data *data;
	struct page *page = opaque_pool_page(test, pp);
	struct sk_buff *skb;
	u32 nfree = area->free_count, hold = pp->pages_state_hold_cnt;
	u64 off;

	KUNIT_ASSERT_TRUE(test, io_zcrx_maio_page_off(ifq, page, &off));
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_owned(ifq, page));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 1L);
	opaque_test_stream(&stream, store);
	skb = opaque_pool_skb(test, page, 128, 1024);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 1024, false), 1024);
	stream.next = 1024;
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, page);
	KUNIT_EXPECT_EQ(test, extent->offset, 128U);
	KUNIT_EXPECT_TRUE(test, extent->backing->area);
	KUNIT_EXPECT_TRUE(test, extent->backing->pool);
	KUNIT_EXPECT_FALSE(test, extent->backing->copied);
	/* A pool reference, not a page reference. */
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 2L);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE);

	/* The stack frees the skb and its pool reference. The store keeps its own. */
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 1L);
	KUNIT_EXPECT_EQ(test, opaque_slot_state(test, ifq, page), (u32)IO_ZCRX_KSLOT_OWNED);

	data = opaque_maio_object(test, &stream);
	KUNIT_EXPECT_TRUE(test, data->mapped);

	/* The last release hands the page to the pool. The freelist stays as it was. */
	KUNIT_EXPECT_TRUE(test, ptr_ring_empty(&pp->ring));
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_FALSE(test, ptr_ring_empty(&pp->ring));
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	KUNIT_EXPECT_EQ(test, pp->pages_state_hold_cnt, hold);
	KUNIT_EXPECT_EQ(test, area->free_count, nfree);
	KUNIT_EXPECT_EQ(test, opaque_slot_state(test, ifq, page), (u32)IO_ZCRX_KSLOT_OWNED);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/*
 * The skb holds a page reference when it is not marked for recycling. The pool
 * can release the page at any time, so the store takes a page reference too.
 * The page still counts as in the area.
 */
static void opaque_maio_page_ref(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 2 * OPAQUE_BATCH);
	struct page_pool *pp = io_zcrx_maio_test_pool(test, ifq);
	struct io_opaque_store *store = opaque_maio_store(test, ifq);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct page *pooled = opaque_pool_page(test, pp), *plain;
	struct sk_buff *skb;
	u64 off;

	opaque_test_stream(&stream, store);

	/* A pool page in an skb that holds a page reference. */
	get_page(pooled);
	skb = alloc_skb(0, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_add_rx_frag(skb, 0, pooled, 0, 512, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 512, false), 512);
	stream.next = 512;
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, pooled);
	KUNIT_EXPECT_TRUE(test, extent->backing->area);
	KUNIT_EXPECT_FALSE(test, extent->backing->pool);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&pooled->pp_ref_count), 1L);
	KUNIT_EXPECT_EQ(test, page_count(pooled), 3);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, page_count(pooled), 2);

	/* A page of the area that no pool owns, in an skb marked for recycling. */
	plain = io_zcrx_maio_copy_page(ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, plain);
	get_page(plain);
	skb = opaque_pool_skb(test, plain, 0, 256);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 256, false), 256);
	stream.next += 256;
	extent = list_last_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, plain);
	KUNIT_EXPECT_TRUE(test, extent->backing->area);
	KUNIT_EXPECT_FALSE(test, extent->backing->pool);
	KUNIT_EXPECT_EQ(test, page_count(plain), 3);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, page_count(plain), 2);

	/* Neither page is counted as copied. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)2 * PAGE_SIZE);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, page_count(pooled), 1);
	KUNIT_EXPECT_EQ(test, page_count(plain), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	opaque_pool_free(pp, pooled);
	io_zcrx_maio_page_put(ifq, plain);
}

/*
 * Bytes outside the area go into area pages: the linear part of an skb and
 * fragments of ordinary pages. The pages fill up before the next one is taken,
 * the object is in the area, copied_bytes counts the bytes, and the release
 * gives every page to the freelist.
 */
static void opaque_maio_copy(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);
	struct io_opaque_store *store = opaque_maio_store(test, ifq);
	struct io_zcrx_area *area = ifq->areas[0];
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent, *second;
	struct io_opaque_data *data;
	struct page *a = alloc_page(GFP_KERNEL), *b = alloc_page(GFP_KERNEL);
	struct sk_buff *skb = alloc_skb(100, GFP_KERNEL);
	u8 *expected = kunit_kmalloc(test, 6100, GFP_KERNEL);
	u8 *actual = kunit_kmalloc(test, 6100, GFP_KERNEL);
	u64 off;
	u32 i;
	void *mapped;

	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_NOT_NULL(test, expected);
	KUNIT_ASSERT_NOT_NULL(test, actual);
	/* A head of 100 bytes and two fragments of 3000 bytes in ordinary pages. */
	for (i = 0; i < 6100; i++)
		expected[i] = (u8)(i * 7 + i / 251);
	memcpy(skb_put(skb, 100), expected, 100);
	mapped = kmap_local_page(a);
	memcpy(mapped, expected + 100, 3000);
	kunmap_local(mapped);
	mapped = kmap_local_page(b);
	memcpy(mapped, expected + 3100, 3000);
	kunmap_local(mapped);
	skb_add_rx_frag(skb, 0, a, 0, 3000, PAGE_SIZE);
	skb_add_rx_frag(skb, 1, b, 0, 3000, PAGE_SIZE);
	opaque_test_stream(&stream, store);

	opaque_maio_take(test, &stream, skb, 6100);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 6100LL);
	KUNIT_EXPECT_EQ(test, area->free_count, 14U);
	/* The first page took 4096 bytes before the second page was taken. */
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	second = list_next_entry(extent, list);
	KUNIT_EXPECT_EQ(test, extent->length, (u32)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, second->length, 6100U - PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 2);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)2 * PAGE_SIZE);
	for (i = 0; i < 2; i++) {
		extent = i ? second : extent;
		KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_off(ifq, extent->backing->page, &off));
		KUNIT_EXPECT_TRUE(test, extent->backing->copied);
		KUNIT_EXPECT_TRUE(test, extent->backing->area);
		KUNIT_EXPECT_TRUE(test, extent->backing->pool);
		KUNIT_EXPECT_NULL(test, extent->backing->page->pp);
	}
	/* The head and the two fragments stay ordinary: the skb still holds them. */
	kfree_skb(skb);

	data = opaque_maio_object(test, &stream);
	KUNIT_EXPECT_TRUE(test, data->mapped);
	KUNIT_EXPECT_EQ(test, data->nr, 2U);
	opaque_test_copy(data, actual);
	KUNIT_EXPECT_MEMEQ(test, actual, expected, 6100);

	/* The release gives both pages back to the freelist. */
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, area->free_count, 16U);
	KUNIT_EXPECT_EQ(test, opaque_slots_in(ifq, IO_ZCRX_KSLOT_OWNED), 0U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/*
 * A shared or cloned skb carries pool pages too, but the store must not keep
 * them. It copies the bytes into area pages and leaves the pool reference of
 * the skb alone.
 */
static void opaque_maio_shared_copy(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 2 * OPAQUE_BATCH);
	struct page_pool *pp = io_zcrx_maio_test_pool(test, ifq);
	struct io_opaque_store *store = opaque_maio_store(test, ifq);
	struct io_zcrx_area *area = ifq->areas[0];
	struct page *page = opaque_pool_page(test, pp);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct sk_buff *skb, *clone;
	u32 nfree = area->free_count;
	void *mapped;
	u64 off;

	mapped = kmap_local_page(page);
	memset(mapped + 64, 0x3c, 200);
	kunmap_local(mapped);
	skb = opaque_pool_skb(test, page, 64, 200);

	/* Shared: a second user of the skb. */
	opaque_test_stream(&stream, store);
	skb_get(skb);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 200, false), 200);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_NE(test, extent->backing->page, page);
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_off(ifq, extent->backing->page, &off));
	KUNIT_EXPECT_TRUE(test, extent->backing->copied);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 200LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 1L);
	KUNIT_EXPECT_EQ(test, area->free_count, nfree - 1);
	kfree_skb(skb);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, area->free_count, nfree);

	/* Cloned: the data of the skb has a second owner. */
	opaque_test_stream(&stream, store);
	clone = skb_clone(skb, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, clone);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, clone, 0, 200, false), 200);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_NE(test, extent->backing->page, page);
	KUNIT_EXPECT_TRUE(test, extent->backing->copied);
	mapped = kmap_local_page(extent->backing->page);
	KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[0], (u8)0x3c);
	KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[199], (u8)0x3c);
	kunmap_local(mapped);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 1L);
	kfree_skb(clone);
	kfree_skb(skb);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, area->free_count, nfree);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * A fallback page belongs to a pool of the instance, but it is not in the
 * area. The store copies its bytes into an area page.
 */
static void opaque_maio_fallback_copy(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, OPAQUE_BATCH + 1);
	struct page_pool *pp = io_zcrx_maio_test_pool(test, ifq);
	struct io_opaque_store *store = opaque_maio_store(test, ifq);
	struct io_zcrx_area *area = ifq->areas[0];
	struct page *held[OPAQUE_BATCH], *copy, *fallback;
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct sk_buff *skb;
	unsigned int i;
	void *mapped;
	u64 off;

	/* One page for the copy, a batch for the pool, and then nothing. */
	copy = io_zcrx_maio_copy_page(ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	for (i = 0; i < OPAQUE_BATCH; i++)
		held[i] = opaque_pool_page(test, pp);
	KUNIT_EXPECT_EQ(test, area->free_count, 0U);
	fallback = opaque_pool_page(test, pp);
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_off(ifq, fallback, &off));
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_owned(ifq, fallback));
	mapped = kmap_local_page(fallback);
	memset(mapped + 16, 0xa7, 300);
	kunmap_local(mapped);

	/* The area gets a free page back. */
	io_zcrx_maio_page_put(ifq, copy);
	KUNIT_EXPECT_EQ(test, area->free_count, 1U);

	opaque_test_stream(&stream, store);
	skb = opaque_pool_skb(test, fallback, 16, 300);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 300, false), 300);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_NE(test, extent->backing->page, fallback);
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_off(ifq, extent->backing->page, &off));
	KUNIT_EXPECT_TRUE(test, extent->backing->copied);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 300LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&fallback->pp_ref_count), 1L);
	KUNIT_EXPECT_EQ(test, area->free_count, 0U);
	mapped = kmap_local_page(extent->backing->page);
	KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[0], (u8)0xa7);
	KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[299], (u8)0xa7);
	kunmap_local(mapped);

	/* The skb gives the fallback page to its pool, which frees it. */
	kfree_skb(skb);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, area->free_count, 1U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	for (i = 0; i < OPAQUE_BATCH; i++)
		opaque_pool_free(pp, held[i]);
}

/*
 * EXTENTS pairs: the area offset of the page plus the offset in the page, and
 * the length. An extent at the end of a page, a whole page and an extent in
 * the middle of a page cover the cases around a page boundary. The lengths add
 * up to the length of the object.
 */
static void opaque_maio_extents(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 2 * OPAQUE_BATCH);
	struct page_pool *pp = io_zcrx_maio_test_pool(test, ifq);
	struct io_opaque_store *store = opaque_maio_store(test, ifq);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct io_opaque_data *data;
	struct page *pages[3] = {
		opaque_pool_page(test, pp), opaque_pool_page(test, pp), opaque_pool_page(test, pp),
	};
	static const u32 offs[3] = { PAGE_SIZE - 1000, 0, 96 };
	static const u32 lens[3] = { 1000, PAGE_SIZE, 2000 };
	struct io_uring_opaque_extent pair;
	/* The extent below owns the only reference to this page. */
	struct page *outside = alloc_page(GFP_KERNEL);
	struct sk_buff *skb[3];
	u64 base[3], total = 0;
	u32 i = 0;

	KUNIT_ASSERT_NOT_NULL(test, outside);
	opaque_test_stream(&stream, store);
	for (i = 0; i < 3; i++) {
		KUNIT_ASSERT_TRUE(test, io_zcrx_maio_page_off(ifq, pages[i], &base[i]));
		skb[i] = opaque_pool_skb(test, pages[i], offs[i], lens[i]);
		opaque_maio_take(test, &stream, skb[i], lens[i]);
	}
	data = opaque_maio_object(test, &stream);
	KUNIT_ASSERT_EQ(test, data->nr, 3U);
	KUNIT_EXPECT_EQ(test, data->length, (u64)lens[0] + lens[1] + lens[2]);
	KUNIT_EXPECT_EQ(test, (u64)data->nr * sizeof(pair), 3 * sizeof(pair));

	i = 0;
	list_for_each_entry(extent, &data->extents, list) {
		pair = io_opaque_extent_pair(store, extent);
		KUNIT_EXPECT_EQ(test, pair.area_off, base[i] + offs[i]);
		KUNIT_EXPECT_EQ(test, pair.len, (u64)lens[i]);
		/* An extent stays inside one page of the area. */
		KUNIT_EXPECT_LE(test, (pair.area_off & (PAGE_SIZE - 1)) + pair.len, (u64)PAGE_SIZE);
		total += pair.len;
		i++;
	}
	KUNIT_EXPECT_EQ(test, total, data->length);

	/* The pair of an extent outside the area, by mark and by lookup. */
	extent = io_opaque_extent_new(store, outside, 0, 100, 0, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
	pair = io_opaque_extent_pair(store, extent);
	KUNIT_EXPECT_EQ(test, pair.area_off, IORING_OPAQUE_EXTENT_UNMAPPED);
	KUNIT_EXPECT_EQ(test, pair.len, 100ULL);
	extent->backing->area = true;
	pair = io_opaque_extent_pair(store, extent);
	KUNIT_EXPECT_EQ(test, pair.area_off, IORING_OPAQUE_EXTENT_UNMAPPED);
	list_add(&extent->list, &stream.extents);
	io_opaque_extents_free(store, &stream.extents);

	for (i = 0; i < 3; i++)
		kfree_skb(skb[i]);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/*
 * An empty area fails a copy like a failed allocation: -ENOMEM, no charge, no
 * progress. A prefix that the capture copied before the area ran out stays.
 */
static void opaque_maio_exhausted(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 2);
	struct io_opaque_store *store = opaque_maio_store(test, ifq);
	struct io_zcrx_area *area = ifq->areas[0];
	struct page *held[2], *tail = alloc_page(GFP_KERNEL);
	struct sk_buff *skb = alloc_skb(100, GFP_KERNEL), *two;
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = PAGE_SIZE + 100, .arg.data = &stream };
	u64 off;

	KUNIT_ASSERT_NOT_NULL(test, tail);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	held[0] = io_zcrx_maio_copy_page(ifq, &off);
	held[1] = io_zcrx_maio_copy_page(ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, held[0]);
	KUNIT_ASSERT_NOT_NULL(test, held[1]);
	KUNIT_EXPECT_NULL(test, io_zcrx_maio_copy_page(ifq, &off));
	memset(skb_put(skb, 100), 0x51, 100);
	opaque_test_stream(&stream, store);

	/* No free page: nothing is charged and nothing is kept. */
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 100, false), -ENOMEM);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
	KUNIT_EXPECT_TRUE(test, list_empty(&stream.extents));
	KUNIT_EXPECT_EQ(test, stream.undecided, 0ULL);

	/* One free page takes the first page of bytes, and the second page fails. */
	io_zcrx_maio_page_put(ifq, held[0]);
	two = alloc_skb(PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, two);
	memset(skb_put(two, PAGE_SIZE), 0x62, PAGE_SIZE);
	skb_add_rx_frag(two, 0, tail, 0, 100, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, two, 0, PAGE_SIZE + 100), (int)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, desc.error, -ENOMEM);
	KUNIT_EXPECT_EQ(test, stream.next, (u64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, stream.undecided, (u64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), (s64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, area->free_count, 0U);

	/* A page comes back: the retry takes the rest. */
	io_zcrx_maio_page_put(ifq, held[1]);
	desc.count = 100;
	desc.error = 0;
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, two, PAGE_SIZE, 100), 100);
	KUNIT_EXPECT_EQ(test, desc.error, 0);
	KUNIT_EXPECT_EQ(test, stream.next, (u64)PAGE_SIZE + 100);

	kfree_skb(skb);
	kfree_skb(two);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, area->free_count, 2U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * An object in the area stays there. A manual COMPACT is refused. The check
 * comes first, so no request state is needed. An object of a store without an
 * instance is not in the area.
 */
static void opaque_maio_compact(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);
	struct io_opaque_store *store = opaque_maio_store(test, ifq);
	struct io_opaque_store *plain = opaque_test_store(test);
	struct io_opaque_req op = {
		.store = store, .handle = 9ULL << IO_OPAQUE_INDEX_BITS,
	};
	struct io_opaque_stream stream;
	struct io_opaque_data *data, *sparse;
	struct sk_buff *skb = alloc_skb(100, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	memset(skb_put(skb, 100), 0x11, 100);
	opaque_test_stream(&stream, store);
	opaque_maio_take(test, &stream, skb, 100);
	data = opaque_maio_object(test, &stream);
	KUNIT_ASSERT_TRUE(test, data->mapped);
	store->objects[0].data = data;
	store->objects[0].generation = 9;
	store->nr_objects++;
	__set_bit(0, store->object_used);
	KUNIT_EXPECT_EQ(test, io_opaque_compact_start(&op), -EOPNOTSUPP);
	KUNIT_EXPECT_FALSE(test, store->objects[0].compacting);
	op.handle = 10ULL << IO_OPAQUE_INDEX_BITS;
	KUNIT_EXPECT_EQ(test, io_opaque_compact_start(&op), -ESTALE);

	sparse = opaque_sparse_data(test, plain);
	KUNIT_EXPECT_FALSE(test, sparse->mapped);

	kfree_skb(skb);
	store->objects[0].data = NULL;
	io_opaque_data_put(store, data);
	io_opaque_data_put(plain, sparse);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}
#endif

static struct kunit_case opaque_cases[] = {
	KUNIT_CASE(opaque_memory_watermarks),
	KUNIT_CASE(opaque_reply_reserve),
	KUNIT_CASE(opaque_collect_gate),
	KUNIT_CASE(opaque_forward_only_versions),
	KUNIT_CASE(opaque_inspection_windows),
	KUNIT_CASE(opaque_capture_identity),
	KUNIT_CASE(opaque_capture_clone_copy),
	KUNIT_CASE(opaque_capture_shared_copy),
	KUNIT_CASE(opaque_capture_compound),
	KUNIT_CASE(opaque_capture_oversized),
	KUNIT_CASE(opaque_capture_budget),
	KUNIT_CASE(opaque_capture_gaps),
	KUNIT_CASE(opaque_capture_copy_pack),
	KUNIT_CASE(opaque_capture_partial_pressure),
	KUNIT_CASE(opaque_capture_private_budget),
	KUNIT_CASE(opaque_split_shared_charge),
	KUNIT_CASE(opaque_keep_restore),
	KUNIT_CASE(opaque_compact_versions),
	KUNIT_CASE(opaque_compact_watermarks),
	KUNIT_CASE(opaque_compact_reservation),
	KUNIT_CASE(opaque_compact_canceled),
	KUNIT_CASE(opaque_generation_retirement),
	KUNIT_CASE(opaque_slot_rotation),
	KUNIT_CASE(opaque_generation_width),
	KUNIT_CASE(opaque_send_last_ownership),
	KUNIT_CASE(opaque_send_last_compacted),
	KUNIT_CASE(opaque_send_last_compact_race),
	KUNIT_CASE(opaque_send_versions),
#ifdef CONFIG_IO_URING_ZCRX_KUNIT_TEST
	KUNIT_CASE(opaque_maio_bind),
	KUNIT_CASE(opaque_maio_pool_ref),
	KUNIT_CASE(opaque_maio_page_ref),
	KUNIT_CASE(opaque_maio_copy),
	KUNIT_CASE(opaque_maio_shared_copy),
	KUNIT_CASE(opaque_maio_fallback_copy),
	KUNIT_CASE(opaque_maio_extents),
	KUNIT_CASE(opaque_maio_exhausted),
	KUNIT_CASE(opaque_maio_compact),
#endif
	{}
};

static struct kunit_suite opaque_suite = {
	.name = "io_uring-opaque",
	.test_cases = opaque_cases,
};

kunit_test_suite(opaque_suite);
