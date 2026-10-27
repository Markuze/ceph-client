// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <kunit/static_stub.h>
#include <linux/completion.h>
#include <linux/highmem.h>
#include <linux/kthread.h>
#include <linux/skbuff.h>
#include <net/tcp.h>

#include "opaque_internal.h"

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
	left = io_opaque_extent_new(store, page, 0, 128, 0, false);
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
	extent = io_opaque_extent_new(store, page, 0, 8, 0, false);
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
		extent = io_opaque_extent_new(store, page, 0, 2000, i * 2000, false);
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

static void opaque_create_reservation_unwind(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_req op = { .store = store, .op = IORING_OPAQUE_CREATE };
	struct io_opaque_extent *extent;
	struct page *page;

	op.data = io_opaque_data_alloc();
	KUNIT_ASSERT_NOT_NULL(test, op.data);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, 2 * PAGE_SIZE, true));
	op.create_reserved = 2 * PAGE_SIZE;
	store->config.max_extents = 1;
	page = alloc_page(GFP_KERNEL_ACCOUNT);
	KUNIT_ASSERT_NOT_NULL(test, page);
	extent = io_opaque_extent_new(store, page, 0, PAGE_SIZE, 0, true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
	list_add_tail(&extent->list, &op.data->extents);
	op.create_reserved -= page_size(page);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)2 * PAGE_SIZE);
	page = alloc_page(GFP_KERNEL_ACCOUNT);
	KUNIT_ASSERT_NOT_NULL(test, page);
	extent = io_opaque_extent_new(store, page, 0, 1, PAGE_SIZE, true);
	KUNIT_EXPECT_EQ(test, PTR_ERR(extent), -ENOBUFS);
	put_page(page);
	io_opaque_create_release(&op);
	KUNIT_EXPECT_EQ(test, op.create_reserved, 0ULL);
	KUNIT_EXPECT_PTR_EQ(test, op.data, NULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static void opaque_create_publish_revalidate(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_kiocb *req = kunit_kzalloc(test, sizeof(*req), GFP_KERNEL);
	struct io_opaque_req op = {
		.store = store, .req = req, .op = IORING_OPAQUE_CREATE, .length = 1,
	};
	u64 flags = 0;

	KUNIT_ASSERT_NOT_NULL(test, req);
	op.data = io_opaque_data_alloc();
	KUNIT_ASSERT_NOT_NULL(test, op.data);
	KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&op), 0);
	store->objects[op.slot].generation++;
	KUNIT_EXPECT_EQ(test, io_opaque_publish(&op, &flags), -ESTALE);
	store->objects[op.slot].generation--;
	store->dead = true;
	KUNIT_EXPECT_EQ(test, io_opaque_publish(&op, &flags), -ECANCELED);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[op.slot].data, NULL);
	KUNIT_EXPECT_EQ(test, store->nr_objects, 0U);
	KUNIT_EXPECT_EQ(test, flags, 0ULL);
	io_opaque_create_release(&op);
}

static void opaque_create_dense_classification(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_kiocb *req = kunit_kzalloc(test, sizeof(*req), GFP_KERNEL);
	struct io_opaque_req op = {
		.store = store, .req = req, .op = IORING_OPAQUE_CREATE, .length = 1,
	};
	struct io_opaque_extent *extent;
	struct io_opaque_data *data;
	struct page *page;
	u64 flags = 0;

	KUNIT_ASSERT_NOT_NULL(test, req);
	store->config.high_watermark = PAGE_SIZE;
	store->config.low_watermark = 0;
	store->objects[0].generation = IO_OPAQUE_GENERATION_MAX;
	op.data = io_opaque_data_alloc();
	KUNIT_ASSERT_NOT_NULL(test, op.data);
	KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&op), 0);
	KUNIT_EXPECT_NE(test, op.slot, 0U);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, PAGE_SIZE, true));
	op.create_reserved = PAGE_SIZE;
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_LOWMEM);
	KUNIT_EXPECT_FALSE(test, op.data->forward_only);
	page = alloc_page(GFP_KERNEL_ACCOUNT);
	KUNIT_ASSERT_NOT_NULL(test, page);
	extent = io_opaque_extent_new(store, page, 0, 1, 0, true);
	KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
	op.create_reserved -= page_size(page);
	list_add_tail(&extent->list, &op.data->extents);
	op.data->length = 1;
	op.data->dense = true;
	KUNIT_ASSERT_EQ(test, io_opaque_vector(op.data), 0);
	store->policy.flags = IORING_OPAQUE_POLICY_F_AUTO_COMPACT;
	KUNIT_ASSERT_EQ(test, io_opaque_publish(&op, &flags), 1);
	KUNIT_EXPECT_EQ(test, flags, 0ULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&store->candidates));
	KUNIT_EXPECT_FALSE(test, op.slot_reserved);
	mutex_lock(&store->tables);
	data = io_opaque_detach(store, &store->objects[op.slot]);
	mutex_unlock(&store->tables);
	io_opaque_data_put(store, data);
	io_opaque_create_release(&op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

struct opaque_release_trace {
	struct io_opaque_store *store;
	struct page *page;
	unsigned int nr, retired, published, batches, initial_extents;
	s64 initial_bytes;
	bool shared, last_shared, watermarks;
	u64 extra_bytes;
};

static void opaque_release_observe(struct io_opaque_store *store,
				   struct io_opaque_data *data, unsigned int retired,
				   bool flushed)
{
	struct kunit *test = kunit_get_current_test();
	struct opaque_release_trace *t = test->priv;
	u64 released;

	KUNIT_EXPECT_PTR_EQ(test, store, t->store);
	if (flushed) {
		t->published += retired;
		t->batches++;
	} else {
		t->retired++;
		KUNIT_EXPECT_EQ(test, retired, t->retired - t->published);
	}
	KUNIT_EXPECT_GT(test, retired, 0U);
	KUNIT_EXPECT_LE(test, retired, (unsigned int)IO_OPAQUE_RELEASE_BATCH);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents),
			(int)(t->initial_extents - t->published));
	/* No credits for live entries; the gap is this destructor's retired entries. */
	KUNIT_EXPECT_GE(test, atomic_read(&store->extents),
			(int)(t->initial_extents - t->retired));
	if (t->retired == t->nr)
		KUNIT_EXPECT_TRUE(test, list_empty(&data->extents));
	if (t->shared)
		released = t->last_shared && t->retired == t->nr ? PAGE_SIZE : 0;
	else
		released = (u64)t->retired * PAGE_SIZE;
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)(t->initial_bytes - released + t->extra_bytes));
	KUNIT_EXPECT_EQ(test, page_count(t->page),
			t->shared ? (released ? 1 : 2) : (int)(t->nr - t->retired + 1));
	if (t->watermarks) {
		/* Read the mode itself: calling memory_mode here would hide a missing update. */
		int mode = t->retired < 32 || (t->extra_bytes && t->retired < 64) ?
			IORING_OPAQUE_MEMORY_LOWMEM : IORING_OPAQUE_MEMORY_NORMAL;

		KUNIT_EXPECT_EQ(test, atomic_read(&store->memory_mode), mode);
		if (!flushed && t->retired == 33) {
			/* Cross high again while this batch still withholds its count credit. */
			KUNIT_EXPECT_TRUE(test, io_opaque_charge(store, 32 * PAGE_SIZE, true));
			t->extra_bytes = 32 * PAGE_SIZE;
			KUNIT_EXPECT_EQ(test, atomic_read(&store->memory_mode),
					IORING_OPAQUE_MEMORY_LOWMEM);
		}
	}
}

static void opaque_release_trace_start(struct kunit *test, struct opaque_release_trace *t,
				       struct io_opaque_store *store, struct page *page,
				       unsigned int nr)
{
	memset(t, 0, sizeof(*t));
	t->store = store;
	t->page = page;
	t->nr = nr;
	t->initial_extents = atomic_read(&store->extents);
	t->initial_bytes = atomic64_read(&store->bytes);
	test->priv = t;
	kunit_activate_static_stub(test, io_opaque_release_observe, opaque_release_observe);
}

/* Separate backings deliberately share one ordinary page; each owns a reference. */
static struct io_opaque_data *opaque_release_data(struct kunit *test,
						 struct io_opaque_store *store,
						 struct page *page, unsigned int nr,
						 bool reserved)
{
	struct io_opaque_data *data = io_opaque_data_alloc();
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, data);
	if (reserved)
		KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, (u64)nr * PAGE_SIZE, true));
	for (i = 0; i < nr; i++) {
		struct io_opaque_extent *extent;

		get_page(page);
		extent = io_opaque_extent_new(store, page, 0, 1, i, reserved);
		if (IS_ERR(extent))
			put_page(page);
		KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
		extent->backing->copied = reserved;
		list_add_tail(&extent->list, &data->extents);
	}
	data->length = nr;
	if (nr)
		KUNIT_ASSERT_EQ(test, io_opaque_vector(data), 0);
	return data;
}

static void opaque_release_boundaries(struct kunit *test)
{
	const unsigned int sizes[] = { 0, 1, 31, 32, 33, 64, 65, 256, 65536 };
	struct io_opaque_store *store = opaque_test_store(test);
	struct page *page = alloc_page(GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, page);
	store->config.hard_limit = SZ_1T;
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		unsigned int nr = sizes[i];
		struct io_opaque_data *data;
		struct opaque_release_trace trace;

		store->config.max_extents = max(256U, nr);
		kunit_info(test, "final data: %u extents (limit %u)\n", nr,
			   store->config.max_extents);
		data = opaque_release_data(test, store, page, nr, false);
		if (nr == store->config.max_extents)
			KUNIT_EXPECT_EQ(test,
				PTR_ERR(io_opaque_extent_new(store, page, 0, 1, nr, false)),
				-ENOBUFS);
		opaque_release_trace_start(test, &trace, store, page, nr);
		refcount_inc(&data->refs);
		io_opaque_data_put(store, data); /* fanout is not final destruction */
		KUNIT_EXPECT_EQ(test, trace.retired, 0U);
		KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), (int)nr);
		io_opaque_data_put(store, NULL);
		io_opaque_data_put(store, data);
		KUNIT_EXPECT_EQ(test, trace.retired, nr);
		KUNIT_EXPECT_EQ(test, trace.published, nr);
		KUNIT_EXPECT_EQ(test, trace.batches, DIV_ROUND_UP(nr, IO_OPAQUE_RELEASE_BATCH));
		KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, page_count(page), 1);
		kunit_deactivate_static_stub(test, io_opaque_release_observe);
	}
	put_page(page);
}

static struct io_opaque_chunk *opaque_fail_chunk_alloc(void)
{
	return NULL;
}

static void opaque_release_allocation_unwind(struct kunit *test)
{
	const unsigned int sizes[] = { 0, 1, 31, 32, 33, 64, 65 };
	struct io_opaque_store *store = opaque_test_store(test);
	struct page *page = alloc_page(GFP_KERNEL);
	unsigned int i;
	bool reserved;

	KUNIT_ASSERT_NOT_NULL(test, page);
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		unsigned int nr = sizes[i];

		for (reserved = false; ; reserved = true) {
			struct io_opaque_req op = { .store = store };
			struct opaque_release_trace trace;
			struct io_opaque_extent *extent;

			op.data = opaque_release_data(test, store, page, nr, reserved);
			if (reserved) {
				KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, PAGE_SIZE, true));
				op.create_reserved = PAGE_SIZE;
			}
			kunit_activate_static_stub(test, io_opaque_chunk_alloc,
					   opaque_fail_chunk_alloc);
			/* Real reservation/charge paths run before the failing allocation. */
			extent = io_opaque_extent_new(store, page, 0, 1, nr, reserved);
			KUNIT_EXPECT_EQ(test, PTR_ERR(extent), -ENOMEM);
			KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), (int)nr);
			KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
					(s64)(nr * PAGE_SIZE + op.create_reserved));
			kunit_deactivate_static_stub(test, io_opaque_chunk_alloc);
			opaque_release_trace_start(test, &trace, store, page, nr);
			io_opaque_create_release(&op);
			KUNIT_EXPECT_PTR_EQ(test, op.data, NULL);
			KUNIT_EXPECT_EQ(test, op.create_reserved, 0ULL);
			KUNIT_EXPECT_EQ(test, trace.published, nr);
			KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
			KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
			KUNIT_EXPECT_EQ(test, page_count(page), 1);
			kunit_deactivate_static_stub(test, io_opaque_release_observe);
			if (reserved)
				break;
		}
	}
	put_page(page);
}

static void opaque_release_shared_versions(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	bool split_first;

	for (split_first = false; ; split_first = true) {
		struct io_opaque_data *a = io_opaque_data_alloc(), *b = io_opaque_data_alloc();
		struct io_opaque_data *first = split_first ? b : a, *last = split_first ? a : b;
		struct io_opaque_extent *extent, *next;
		struct opaque_release_trace trace;
		struct page *page = alloc_page(GFP_KERNEL);
		unsigned int i;

		KUNIT_ASSERT_NOT_NULL(test, a);
		KUNIT_ASSERT_NOT_NULL(test, b);
		KUNIT_ASSERT_NOT_NULL(test, page);
		get_page(page);
		extent = io_opaque_extent_new(store, page, 0, 66, 0, false);
		KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
		list_add(&extent->list, &a->extents);
		for (i = 1; i < 66; i++) {
			extent = io_opaque_extent_split(store, extent, i);
			KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
		}
		i = 0;
		list_for_each_entry_safe(extent, next, &a->extents, list)
			if (i++ >= 33)
				list_move_tail(&extent->list, &b->extents);
		/* Both independent versions cross a batch boundary on the same backing. */
		opaque_release_trace_start(test, &trace, store, page, 33);
		trace.shared = true;
		io_opaque_data_put(store, first);
		KUNIT_EXPECT_EQ(test, trace.batches, 2U);
		KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 33);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE);
		opaque_release_trace_start(test, &trace, store, page, 33);
		trace.shared = true;
		trace.last_shared = true;
		io_opaque_data_put(store, last);
		KUNIT_EXPECT_EQ(test, trace.batches, 2U);
		KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, page_count(page), 1);
		kunit_deactivate_static_stub(test, io_opaque_release_observe);
		put_page(page);
		if (split_first)
			break;
	}
}

static void opaque_release_watermarks(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct page *page = alloc_page(GFP_KERNEL);
	bool copied;

	KUNIT_ASSERT_NOT_NULL(test, page);
	store->config.hard_limit = 75 * PAGE_SIZE;
	store->config.reply_reserve = 2 * PAGE_SIZE;
	store->config.high_watermark = 64 * PAGE_SIZE;
	store->config.low_watermark = 33 * PAGE_SIZE;
	for (copied = false; ; copied = true) {
		struct io_opaque_data *data = opaque_release_data(test, store, page, 65, copied);
		struct opaque_release_trace trace;
		bool frame_reserved;

		KUNIT_ASSERT_TRUE(test, io_opaque_charge_transient(store, 5 * PAGE_SIZE,
						store->config.hard_limit -
						store->config.reply_reserve));
		KUNIT_ASSERT_TRUE(test, io_opaque_charge_frame(store, 3 * PAGE_SIZE,
						    &frame_reserved));
		KUNIT_ASSERT_FALSE(test, frame_reserved);
		KUNIT_ASSERT_TRUE(test, io_opaque_charge_frame(store, 2 * PAGE_SIZE,
						    &frame_reserved));
		KUNIT_ASSERT_TRUE(test, frame_reserved);
		opaque_release_trace_start(test, &trace, store, page, 65);
		trace.watermarks = true;
		io_opaque_data_put(store, data);
		KUNIT_EXPECT_EQ(test, trace.batches, 3U);
		KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
		kunit_deactivate_static_stub(test, io_opaque_release_observe);
		io_opaque_uncharge(store, trace.extra_bytes);
		io_opaque_uncharge_frame(store, 2 * PAGE_SIZE, true);
		io_opaque_uncharge_frame(store, 3 * PAGE_SIZE, false);
		io_opaque_release_transient(store, 5 * PAGE_SIZE, 5 * PAGE_SIZE);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->framing), 0LL);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->reserve_framing), 0LL);
		if (copied)
			break;
	}
	put_page(page);
}

struct opaque_release_thread {
	struct kunit *test;
	struct io_opaque_store *store;
	struct io_opaque_data *data;
	struct completion paused, resume, done;
	struct task_struct *task;
	bool paused_once;
};

struct opaque_release_concurrent {
	struct opaque_release_thread threads[2];
};

static void opaque_release_pause(struct io_opaque_store *store,
				 struct io_opaque_data *data, unsigned int retired, bool flushed)
{
	struct kunit *test = kunit_get_current_test();
	struct opaque_release_concurrent *c = test->priv;
	unsigned int i;

	if (flushed || retired != IO_OPAQUE_RELEASE_BATCH)
		return;
	for (i = 0; i < ARRAY_SIZE(c->threads); i++) {
		struct opaque_release_thread *t = &c->threads[i];

		if (data == t->data && !t->paused_once) {
			t->paused_once = true;
			complete(&t->paused);
			wait_for_completion(&t->resume);
			break;
		}
	}
}

static int opaque_release_thread(void *arg)
{
	struct opaque_release_thread *t = arg;

	current->kunit_test = t->test;
	io_opaque_data_put(t->store, t->data);
	current->kunit_test = NULL;
	complete(&t->done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

static void opaque_release_join(void *arg)
{
	struct opaque_release_thread *t = arg;

	complete_all(&t->resume);
	kthread_stop(t->task);
}

static void opaque_release_concurrent_reserve(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_release_concurrent *c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	struct io_opaque_data *held = io_opaque_data_alloc();
	struct page *page = alloc_page(GFP_KERNEL);
	unsigned int i, pass;

	KUNIT_ASSERT_NOT_NULL(test, c);
	KUNIT_ASSERT_NOT_NULL(test, held);
	KUNIT_ASSERT_NOT_NULL(test, page);
	store->config.hard_limit = 256 * PAGE_SIZE;
	store->config.max_extents = 132;
	test->priv = c;
	kunit_activate_static_stub(test, io_opaque_release_observe, opaque_release_pause);
	for (i = 0; i < 2; i++) {
		struct opaque_release_thread *t = &c->threads[i];

		t->test = test;
		t->store = store;
		t->data = opaque_release_data(test, store, page, 65, false);
		init_completion(&t->paused);
		init_completion(&t->resume);
		init_completion(&t->done);
	}
	for (i = 0; i < 2; i++) {
		struct opaque_release_thread *t = &c->threads[i];

		t->task = kthread_run(opaque_release_thread, t, "opaque-release-%u", i);
		KUNIT_ASSERT_FALSE(test, IS_ERR(t->task));
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_release_join, t), 0);
		KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&t->paused, 5 * HZ), 0UL);
	}
	/* Preempt both at the full bound, before either publishes any count credits. */
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 130);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)66 * PAGE_SIZE);
	for (pass = 0; pass < 2; pass++) {
		unsigned int nr = pass ? 65 : 2;

		for (i = 0; i < nr; i++) {
			struct io_opaque_extent *extent;

			get_page(page);
			extent = io_opaque_extent_new(store, page, 0, 1, i, false);
			KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
			list_add_tail(&extent->list, &held->extents);
		}
		KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 132);
		KUNIT_EXPECT_EQ(test, PTR_ERR(io_opaque_extent_new(store, page, 0, 1, 0, false)),
				-ENOBUFS);
		complete(&c->threads[pass].resume);
		KUNIT_ASSERT_NE(test,
			wait_for_completion_timeout(&c->threads[pass].done, 5 * HZ), 0UL);
		KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 67);
	}
	kunit_deactivate_static_stub(test, io_opaque_release_observe);
	io_opaque_data_put(store, held);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	put_page(page);
}

static void opaque_release_user_stop(struct kunit *test)
{
	struct io_ring_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct io_uring_opaque_config config = {
		.hard_limit = 128 * PAGE_SIZE, .max_object_size = 64 * PAGE_SIZE,
		.max_objects = 4, .max_streams = 1, .max_extents = 256, .max_requests = 4,
	};
	struct io_opaque_store *store;
	struct io_opaque_data *data;
	struct page *page = alloc_page(GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_NOT_NULL(test, page);
	store = io_opaque_alloc(ctx, &config);
	KUNIT_ASSERT_FALSE(test, IS_ERR(store));
	data = opaque_release_data(test, store, page, 65, false);
	store->objects[0].data = data;
	store->nr_objects = 1;
	__set_bit(0, store->object_used);
	refcount_add(2, &data->refs); /* ordinary fanout and late LAST cleanup */
	io_opaque_get(store); /* another imported ring: users and lifetime refs */
	io_opaque_user_put(store);
	KUNIT_EXPECT_FALSE(test, store->dead);
	io_opaque_user_put(store);
	KUNIT_EXPECT_TRUE(test, store->dead);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 65);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 65);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	io_opaque_put(store);
	io_opaque_put(store);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	put_page(page);
}

struct opaque_release_accounting {
	struct page *pages[40];
	u64 charges[40];
	u64 released, transient;
	s64 bytes;
	unsigned int nr, seen, published, batches, extents;
	int mode;
};

static void opaque_release_accounting_observe(struct io_opaque_store *store,
					       struct io_opaque_data *data,
					       unsigned int retired, bool flushed)
{
	struct kunit *test = kunit_get_current_test();
	struct opaque_release_accounting *a = test->priv;
	u64 used;

	if (flushed) {
		KUNIT_EXPECT_EQ(test, retired, a->seen - a->published);
		a->published += retired;
		a->batches++;
	} else {
		a->released += a->charges[a->seen++];
		KUNIT_EXPECT_EQ(test, retired, a->seen - a->published);
		/* Test-held ordinary page refs can outlive backing/chunk retirement. */
		KUNIT_EXPECT_EQ(test, page_count(a->pages[a->seen - 1]), 1);
	}
	KUNIT_EXPECT_LE(test, retired, (unsigned int)IO_OPAQUE_RELEASE_BATCH);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)(a->bytes - a->released));
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), (s64)a->transient);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), (int)(a->extents - a->published));
	/* Independent hysteresis model; do not reevaluate production memory_mode here. */
	used = a->bytes - a->released;
	used = used > a->transient ? used - a->transient : 0;
	if (a->mode == IORING_OPAQUE_MEMORY_NORMAL && used >= store->config.high_watermark)
		a->mode = IORING_OPAQUE_MEMORY_LOWMEM;
	else if (a->mode == IORING_OPAQUE_MEMORY_LOWMEM && used <= store->config.low_watermark)
		a->mode = IORING_OPAQUE_MEMORY_NORMAL;
	KUNIT_EXPECT_EQ(test, atomic_read(&store->memory_mode), a->mode);
	if (a->seen == a->nr)
		KUNIT_EXPECT_TRUE(test, list_empty(&data->extents));
}

static void opaque_release_accounting(struct kunit *test, struct io_opaque_store *store,
				      struct io_opaque_data *data)
{
	struct opaque_release_accounting a = {
		.bytes = atomic64_read(&store->bytes),
		.transient = atomic64_read(&store->transient),
		.extents = atomic_read(&store->extents),
		.mode = atomic_read(&store->memory_mode),
	};
	struct io_opaque_extent *extent;
	unsigned int i;

	list_for_each_entry(extent, &data->extents, list) {
		KUNIT_ASSERT_LT(test, a.nr, ARRAY_SIZE(a.pages));
		a.pages[a.nr] = extent->backing->page;
		a.charges[a.nr++] = extent->backing->charge;
		get_page(extent->backing->page);
	}
	test->priv = &a;
	kunit_activate_static_stub(test, io_opaque_release_observe,
				   opaque_release_accounting_observe);
	io_opaque_data_put(store, data);
	kunit_deactivate_static_stub(test, io_opaque_release_observe);
	KUNIT_EXPECT_EQ(test, a.seen, a.nr);
	KUNIT_EXPECT_EQ(test, a.published, a.nr);
	KUNIT_EXPECT_EQ(test, a.batches, DIV_ROUND_UP(a.nr, IO_OPAQUE_RELEASE_BATCH));
	for (i = 0; i < a.nr; i++) {
		KUNIT_EXPECT_EQ(test, page_count(a.pages[i]), 1);
		put_page(a.pages[i]);
	}
}

static void opaque_release_compact_cleanup(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	unsigned int stale;

	store->config.high_watermark = 32 * PAGE_SIZE;
	store->config.low_watermark = 16 * PAGE_SIZE;
	for (stale = 0; stale < 2; stale++) {
		struct io_opaque_data *data = opaque_sparse_data(test, store);
		struct io_opaque_req op = {
			.store = store, .data = data, .handle = 9ULL << IO_OPAQUE_INDEX_BITS,
		};
		bool reserved;

		store->objects[0].data = data;
		store->objects[0].generation = 9;
		store->nr_objects++;
		__set_bit(0, store->object_used);
		refcount_inc(&data->refs); /* worker owns the failed replacement */
		KUNIT_ASSERT_TRUE(test, io_opaque_charge_frame(store, PAGE_SIZE, &reserved));
		KUNIT_ASSERT_FALSE(test, reserved);
		KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
		KUNIT_EXPECT_EQ(test, atomic_read(&store->memory_mode),
				IORING_OPAQUE_MEMORY_LOWMEM);
		if (stale)
			store->objects[0].generation++;
		else
			op.canceled = true; /* cancellation after actual copy construction */
		KUNIT_EXPECT_EQ(test, io_opaque_compact_publish(&op), -ESTALE);
		/* Actual copied/reserved backings release bytes before their count flush. */
		opaque_release_accounting(test, store, op.replacement);
		op.replacement = NULL;
		io_opaque_compact_end(&op);
		KUNIT_EXPECT_EQ(test, op.reserved, 0ULL);
		KUNIT_EXPECT_EQ(test, op.transient, 0ULL);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)41 * PAGE_SIZE);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), (s64)PAGE_SIZE);
		io_opaque_data_put(store, op.data); /* worker: original version remains */
		scoped_guard(mutex, &store->tables)
			data = io_opaque_detach(store, &store->objects[0]);
		/* Borrowed old backings cross low before the first 32-entry count flush. */
		opaque_release_accounting(test, store, data);
		io_opaque_uncharge_frame(store, PAGE_SIZE, false);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->framing), 0LL);
		KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
		KUNIT_EXPECT_EQ(test, store->nr_objects, 0U);
	}
}

static struct kunit_case opaque_cases[] = {
	KUNIT_CASE(opaque_release_compact_cleanup),
	KUNIT_CASE(opaque_release_boundaries),
	KUNIT_CASE(opaque_release_allocation_unwind),
	KUNIT_CASE(opaque_release_shared_versions),
	KUNIT_CASE(opaque_release_watermarks),
	KUNIT_CASE(opaque_release_concurrent_reserve),
	KUNIT_CASE(opaque_release_user_stop),
	KUNIT_CASE(opaque_create_reservation_unwind),
	KUNIT_CASE(opaque_create_publish_revalidate),
	KUNIT_CASE(opaque_create_dense_classification),
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
	{}
};

static struct kunit_suite opaque_suite = {
	.name = "io_uring-opaque",
	.test_cases = opaque_cases,
};

kunit_test_suite(opaque_suite);
