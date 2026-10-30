// SPDX-License-Identifier: GPL-2.0
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/highmem.h>
#include <linux/kobject.h>
#include <linux/kstrtox.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/sched/user.h>
#include <linux/skbuff.h>
#include <linux/uaccess.h>
#include <net/tcp.h>

#include "io_uring.h"
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

#define OPAQUE_TEST_UNIT	(PAGE_SIZE << IO_OPAQUE_UNIT_ORDER)

static void opaque_test_drain(void *store)
{
	io_opaque_unit_drain(store);
}

/* Enable a cache of @limit default-order units; drained when the test ends. */
static void opaque_test_cache(struct kunit *test, struct io_opaque_store *store, u32 limit)
{
	io_opaque_unit_cache_init(store, limit, IO_OPAQUE_UNIT_ORDER);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_drain, store), 0);
}

struct opaque_test_params {
	unsigned long cache_bytes;
	unsigned int unit_order;
	unsigned int inline_compact_max;
	bool copy_per_page;
};

static void opaque_test_params_restore(void *arg)
{
	struct opaque_test_params *saved = arg;

	io_opaque_cache_bytes = saved->cache_bytes;
	io_opaque_unit_order = saved->unit_order;
	io_opaque_inline_compact_max = saved->inline_compact_max;
	io_opaque_copy_per_page = saved->copy_per_page;
}

/* Tests that change the tunables restore them even when an assertion fails. */
static void opaque_test_params_save(struct kunit *test)
{
	struct opaque_test_params *saved = kunit_kzalloc(test, sizeof(*saved), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, saved);
	saved->cache_bytes = io_opaque_cache_bytes;
	saved->unit_order = io_opaque_unit_order;
	saved->inline_compact_max = io_opaque_inline_compact_max;
	saved->copy_per_page = io_opaque_copy_per_page;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_params_restore,
							saved), 0);
}

static struct page *opaque_test_unit(struct kunit *test)
{
	struct page *page = alloc_pages(GFP_KERNEL | __GFP_COMP, IO_OPAQUE_UNIT_ORDER);

	KUNIT_ASSERT_NOT_NULL(test, page);
	return page;
}

static u32 opaque_test_pages(struct io_opaque_data *data, struct page **pages, u32 max)
{
	struct io_opaque_extent *extent;
	u32 nr = 0;

	list_for_each_entry(extent, &data->extents, list)
		if (nr < max)
			pages[nr++] = extent->backing->page;
	return nr;
}

/* Units of a freed compacted version are rewritten by the next compaction. */
static void opaque_cache_miss_then_hit(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req op = { .store = store, .data = data };
	u8 *before = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *after = kunit_kzalloc(test, 80000, GFP_KERNEL);
	struct page *first[3], *second[3];

	KUNIT_ASSERT_NOT_NULL(test, before);
	KUNIT_ASSERT_NOT_NULL(test, after);
	opaque_test_cache(test, store, 4);
	opaque_test_copy(data, before);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
	/* 80,000 bytes: two whole units and a smaller tail chunk. */
	KUNIT_ASSERT_EQ(test, opaque_test_pages(op.replacement, first, 3), 3U);
	KUNIT_EXPECT_EQ(test, compound_order(first[0]), (unsigned int)IO_OPAQUE_UNIT_ORDER);
	KUNIT_EXPECT_EQ(test, compound_order(first[1]), (unsigned int)IO_OPAQUE_UNIT_ORDER);
	KUNIT_EXPECT_LT(test, compound_order(first[2]), (unsigned int)IO_OPAQUE_UNIT_ORDER);
	KUNIT_EXPECT_EQ(test, op.replacement->charge, ALIGN(80000ULL, PAGE_SIZE));
	KUNIT_EXPECT_EQ(test, op.reserved, 0ULL);
	KUNIT_EXPECT_EQ(test, store->cache.misses, 2ULL);
	KUNIT_EXPECT_EQ(test, store->cache.hits, 0ULL);
	opaque_test_copy(op.replacement, after);
	KUNIT_EXPECT_TRUE(test, !memcmp(before, after, 80000));
	/* The tail is not a unit; observe that the store lets it go. */
	get_page(first[2]);
	io_opaque_data_put(store, op.replacement);
	op.replacement = NULL;
	io_opaque_compact_end(&op);
	KUNIT_EXPECT_EQ(test, page_count(first[2]), 1);
	put_page(first[2]);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 2U);
	KUNIT_EXPECT_EQ(test, store->cache.nr_cooling, 0U);
	KUNIT_EXPECT_EQ(test, page_count(first[0]), 1);
	KUNIT_EXPECT_EQ(test, page_count(first[1]), 1);
	/* Cached units are outside the object charge. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)40 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
	KUNIT_ASSERT_EQ(test, opaque_test_pages(op.replacement, second, 3), 3U);
	/* LIFO: the most recently retired unit is written first. */
	KUNIT_EXPECT_PTR_EQ(test, second[0], first[1]);
	KUNIT_EXPECT_PTR_EQ(test, second[1], first[0]);
	KUNIT_EXPECT_EQ(test, store->cache.hits, 2ULL);
	KUNIT_EXPECT_EQ(test, store->cache.misses, 2ULL);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 0U);
	memset(after, 0, 80000);
	opaque_test_copy(op.replacement, after);
	KUNIT_EXPECT_TRUE(test, !memcmp(before, after, 80000));
	io_opaque_data_put(store, op.replacement);
	op.replacement = NULL;
	io_opaque_compact_end(&op);
	io_opaque_data_put(store, data);
	io_opaque_unit_drain(store);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 0U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/* A unit still referenced elsewhere cools until only the cache holds it. */
static void opaque_cache_cooling_probe(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req op = { .store = store, .data = data };
	struct page *units[3], *page;
	unsigned int order;

	opaque_test_cache(test, store, 4);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
	KUNIT_ASSERT_EQ(test, opaque_test_pages(op.replacement, units, 3), 3U);
	get_page(units[0]); /* an skb fragment */
	io_opaque_data_put(store, op.replacement);
	op.replacement = NULL;
	io_opaque_compact_end(&op);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 1U);
	KUNIT_EXPECT_EQ(test, store->cache.nr_cooling, 1U);
	KUNIT_EXPECT_EQ(test, store->cache.cooled, 1ULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)40 * PAGE_SIZE);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_unit_take(store), units[1]);
	/* With idle empty the referenced unit is probed, skipped and replaced. */
	page = io_opaque_dest_page(store, 1U << IO_OPAQUE_UNIT_ORDER, &order);
	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_EXPECT_PTR_NE(test, page, units[0]);
	KUNIT_EXPECT_EQ(test, order, (unsigned int)IO_OPAQUE_UNIT_ORDER);
	KUNIT_EXPECT_EQ(test, store->cache.probes_failed, 1ULL);
	KUNIT_EXPECT_EQ(test, store->cache.misses, 3ULL);
	KUNIT_EXPECT_EQ(test, store->cache.nr_cooling, 1U);
	KUNIT_EXPECT_EQ(test, page_count(units[0]), 2);
	put_page(page);
	put_page(units[0]); /* the fragment is released */
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_unit_take(store), units[0]);
	KUNIT_EXPECT_EQ(test, store->cache.cooling_hits, 1ULL);
	KUNIT_EXPECT_EQ(test, store->cache.nr_cooling, 0U);
	io_opaque_unit_put(store, units[0]);
	io_opaque_unit_put(store, units[1]);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 2U);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/* TCP fragments, transmit clones and retransmit copies all keep a unit cooling. */
static void opaque_cache_tx_clone(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req op = { .store = store, .data = data };
	size_t len = OPAQUE_TEST_UNIT + 2 * PAGE_SIZE;
	struct sk_buff *skb = alloc_skb(0, GFP_KERNEL), *clone, *copy;
	u8 *before = kunit_kmalloc(test, 80000, GFP_KERNEL);
	struct page *units[3];
	struct iov_iter iter;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_NOT_NULL(test, before);
	opaque_test_cache(test, store, 4);
	opaque_test_copy(data, before);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
	KUNIT_ASSERT_EQ(test, opaque_test_pages(op.replacement, units, 3), 3U);
	/* MSG_SPLICE_PAGES: one fragment and one page reference per 4 KiB. */
	skb->ip_summed = CHECKSUM_PARTIAL;
	iov_iter_bvec(&iter, ITER_SOURCE, op.replacement->bvec, op.replacement->nr, len);
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(skb, &iter, len), (ssize_t)len);
	KUNIT_EXPECT_EQ(test, page_count(units[0]), 1 + (1 << IO_OPAQUE_UNIT_ORDER));
	KUNIT_EXPECT_EQ(test, page_count(units[1]), 3);
	clone = skb_clone(skb, GFP_KERNEL); /* transmission */
	KUNIT_ASSERT_NOT_NULL(test, clone);
	copy = pskb_copy(skb, GFP_KERNEL); /* retransmission of a cloned skb */
	KUNIT_ASSERT_NOT_NULL(test, copy);
	/* SEND_LAST cleanup drops the version while TCP still holds the data. */
	io_opaque_data_put(store, op.replacement);
	op.replacement = NULL;
	io_opaque_compact_end(&op);
	KUNIT_EXPECT_EQ(test, store->cache.nr_cooling, 2U);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 0U);
	kfree_skb(skb);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_unit_take(store), NULL);
	kfree_skb(clone);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_unit_take(store), NULL);
	KUNIT_EXPECT_EQ(test, store->cache.probes_failed, 4ULL);
	KUNIT_EXPECT_EQ(test, store->cache.nr_cooling, 2U);
	kfree_skb(copy);
	/* Nothing rewrote the units while any fragment referenced them. */
	KUNIT_EXPECT_TRUE(test, !memcmp(page_address(units[0]), before, OPAQUE_TEST_UNIT));
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_unit_take(store), units[1]);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_unit_take(store), units[0]);
	KUNIT_EXPECT_EQ(test, store->cache.cooling_hits, 2ULL);
	io_opaque_unit_put(store, units[0]);
	io_opaque_unit_put(store, units[1]);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/* A full cache frees further units; other orders are never cached. */
static void opaque_cache_limit(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct page *a = opaque_test_unit(test), *b = opaque_test_unit(test), *other;
	u64 cached = io_opaque_stat_read(IO_OPAQUE_STAT_CACHED_BYTES);

	opaque_test_cache(test, store, 1);
	io_opaque_unit_put(store, a);
	get_page(b);
	io_opaque_unit_put(store, b);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 1U);
	KUNIT_EXPECT_EQ(test, store->cache.trimmed, 1ULL);
	KUNIT_EXPECT_EQ(test, page_count(b), 1);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_CACHED_BYTES) - cached,
			(u64)OPAQUE_TEST_UNIT);
	put_page(b);
	io_opaque_unit_drain(store);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 0U);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_CACHED_BYTES), cached);
	other = alloc_pages(GFP_KERNEL | __GFP_COMP, IO_OPAQUE_UNIT_ORDER + 1);
	KUNIT_ASSERT_NOT_NULL(test, other);
	KUNIT_EXPECT_FALSE(test, io_opaque_unit_retire(store, other));
	put_page(other);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 0U);
}

/* A cooling unit held past IO_OPAQUE_UNIT_COOL_MAX is released, not reused. */
static void opaque_cache_age(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct page *page = opaque_test_unit(test);
	u64 cached = io_opaque_stat_read(IO_OPAQUE_STAT_CACHED_BYTES);

	opaque_test_cache(test, store, 2);
	get_page(page); /* a slow peer */
	io_opaque_unit_put(store, page);
	KUNIT_ASSERT_EQ(test, store->cache.nr_cooling, 1U);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_unit_take(store), NULL);
	KUNIT_EXPECT_EQ(test, store->cache.probes_failed, 1ULL);
	KUNIT_EXPECT_EQ(test, store->cache.nr_cooling, 1U);
	set_page_private(page, jiffies - IO_OPAQUE_UNIT_COOL_MAX - 1);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_unit_take(store), NULL);
	KUNIT_EXPECT_EQ(test, store->cache.expired, 1ULL);
	KUNIT_EXPECT_EQ(test, store->cache.nr_cooling, 0U);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_CACHED_BYTES), cached);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	KUNIT_EXPECT_EQ(test, page_private(page), 0UL);
	put_page(page);
}

/*
 * Cached units stay outside the hard-limit charge, teardown drains them and a
 * version that outlives teardown (an in-flight send) frees its units.
 */
static void opaque_cache_accounting(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *first = opaque_sparse_data(test, store);
	struct io_opaque_data *second = opaque_sparse_data(test, store), *dense;
	struct io_opaque_req op = {
		.store = store, .data = first, .handle = io_opaque_handle(0, 9),
	};
	u64 sparse = 40 * PAGE_SIZE, packed = ALIGN(80000, PAGE_SIZE);
	u64 drained = io_opaque_stat_read(IO_OPAQUE_STAT_CACHE_DRAINED);
	struct page *units[3];
	int i;

	opaque_test_cache(test, store, 4);
	INIT_LIST_HEAD(&store->copies);
	INIT_DELAYED_WORK(&store->compact_work, io_opaque_compact_work);
	INIT_DELAYED_WORK(&store->retry_work, io_opaque_retry_work);
	for (i = 0; i < 2; i++) {
		store->objects[i].data = i ? second : first;
		store->objects[i].generation = 9;
		__set_bit(i, store->object_used);
		store->nr_objects++;
	}
	refcount_inc(&first->refs); /* compaction */
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)(2 * sparse + packed));
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), (s64)packed);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&op), 0);
	io_opaque_data_put(store, op.data);
	io_opaque_compact_end(&op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)(sparse + packed));
	/* FREE: the units retire idle and leave the hard-limit charge. */
	mutex_lock(&store->tables);
	dense = io_opaque_detach(store, &store->objects[0]);
	mutex_unlock(&store->tables);
	io_opaque_data_put(store, dense);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)sparse);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 2U);
	io_opaque_unit_put(store, opaque_test_unit(test));
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 3U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)sparse);
	/* The second object reuses two units; a send keeps it past teardown. */
	op = (struct io_opaque_req) {
		.store = store, .data = second, .handle = io_opaque_handle(1, 9),
	};
	refcount_inc(&second->refs); /* compaction */
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&op), 0);
	io_opaque_data_put(store, op.data);
	io_opaque_compact_end(&op);
	KUNIT_EXPECT_EQ(test, store->cache.hits, 2ULL);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 1U);
	dense = store->objects[1].data;
	refcount_inc(&dense->refs); /* in-flight send */
	KUNIT_ASSERT_EQ(test, opaque_test_pages(dense, units, 3), 3U);
	get_page(units[0]);
	io_opaque_stop(store);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 0U);
	KUNIT_EXPECT_EQ(test, store->cache.nr_cooling, 0U);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_CACHE_DRAINED) - drained, 1ULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)packed);
	/* The send completes after shutdown: its units are freed, not cached. */
	io_opaque_data_put(store, dense);
	KUNIT_EXPECT_EQ(test, page_count(units[0]), 1);
	put_page(units[0]);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 0U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/* Registration samples the tunables, bounds the cache and locks its memory. */
static void opaque_cache_store_lifecycle(struct kunit *test)
{
	struct io_ring_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct io_uring_opaque_config cfg = {
		.hard_limit = 64 * PAGE_SIZE, .compact_headroom = 4 * OPAQUE_TEST_UNIT,
		.max_object_size = SZ_64K, .max_objects = 4, .max_streams = 1,
		.max_extents = 64, .max_requests = 4,
	};
	struct io_opaque_store *store;
	long locked;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	opaque_test_params_save(test);
	ctx->user = get_current_user();
	locked = atomic_long_read(&ctx->user->locked_vm);
	/* Rounded down to whole units. */
	io_opaque_cache_bytes = 3 * OPAQUE_TEST_UNIT + PAGE_SIZE;
	io_opaque_unit_order = IO_OPAQUE_UNIT_ORDER;
	store = io_opaque_alloc(ctx, &cfg);
	KUNIT_ASSERT_FALSE(test, IS_ERR(store));
	KUNIT_EXPECT_EQ(test, store->cache.limit, 3U);
	KUNIT_EXPECT_EQ(test, store->cache.order, (u32)IO_OPAQUE_UNIT_ORDER);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ctx->user->locked_vm) - locked,
			64L + 3 * (1L << IO_OPAQUE_UNIT_ORDER));
	/* Later tunable changes do not affect an existing store. */
	io_opaque_cache_bytes = 0;
	io_opaque_unit_put(store, opaque_test_unit(test));
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 1U);
	io_opaque_put(store);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ctx->user->locked_vm), locked);
	/* The cache never exceeds compaction headroom; a larger order is accepted. */
	io_opaque_cache_bytes = SZ_1G;
	io_opaque_unit_order = IO_OPAQUE_UNIT_ORDER + 1;
	store = io_opaque_alloc(ctx, &cfg);
	KUNIT_ASSERT_FALSE(test, IS_ERR(store));
	KUNIT_EXPECT_EQ(test, store->cache.limit, 2U);
	KUNIT_EXPECT_EQ(test, store->cache.order, (u32)IO_OPAQUE_UNIT_ORDER + 1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ctx->user->locked_vm) - locked,
			64L + 4 * (1L << IO_OPAQUE_UNIT_ORDER));
	io_opaque_put(store);
	/* Zero disables the cache and reserves nothing beyond the hard limit. */
	io_opaque_cache_bytes = 0;
	store = io_opaque_alloc(ctx, &cfg);
	KUNIT_ASSERT_FALSE(test, IS_ERR(store));
	KUNIT_EXPECT_EQ(test, store->cache.limit, 0U);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ctx->user->locked_vm) - locked, 64L);
	io_opaque_put(store);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ctx->user->locked_vm), locked);
	free_uid(ctx->user);
}

/*
 * Source extents with odd offsets and lengths, runs across destination unit
 * boundaries, sources spanning compound pages and offsets beyond PAGE_SIZE.
 */
static const struct {
	u8 order;
	u16 offset;
	u16 length;
} opaque_runs[] = {
	{ 0, 1, 1447 }, { 0, 3, 4093 }, { 2, 5000, 9000 }, { 0, 0, 4096 },
	{ 0, 2047, 1 }, { 0, 17, 3001 }, { 1, 4095, 4097 }, { 2, 12289, 4000 },
};

static struct io_opaque_data *opaque_runs_data(struct kunit *test,
					       struct io_opaque_store *store, u32 length)
{
	struct io_opaque_data *data = io_opaque_data_alloc();
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, data);
	for (i = 0; data->length < length; i++) {
		unsigned int order = opaque_runs[i % ARRAY_SIZE(opaque_runs)].order;
		u32 offset = opaque_runs[i % ARRAY_SIZE(opaque_runs)].offset;
		u32 len = min_t(u32, opaque_runs[i % ARRAY_SIZE(opaque_runs)].length,
				length - data->length);
		struct page *page = alloc_pages(GFP_KERNEL | __GFP_COMP, order);
		struct io_opaque_extent *extent;

		KUNIT_ASSERT_NOT_NULL(test, page);
		get_random_bytes(page_address(page), page_size(page));
		extent = io_opaque_extent_new(store, page, offset, len, data->length, false);
		KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
		list_add_tail(&extent->list, &data->extents);
		data->length += len;
	}
	KUNIT_ASSERT_EQ(test, io_opaque_vector(data), 0);
	return data;
}

/* The per-run copy is byte-identical to the page-split copy, with and without units. */
static void opaque_compact_copy_runs(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	u32 length = 2 * OPAQUE_TEST_UNIT + PAGE_SIZE + 369;
	struct io_opaque_data *data = opaque_runs_data(test, store, length);
	struct io_opaque_req op = { .store = store, .data = data };
	u8 *before = kunit_kmalloc(test, length, GFP_KERNEL);
	u8 *after = kunit_kmalloc(test, length, GFP_KERNEL);
	s64 charged = atomic64_read(&store->bytes);
	unsigned int mode;

	KUNIT_ASSERT_NOT_NULL(test, before);
	KUNIT_ASSERT_NOT_NULL(test, after);
	opaque_test_params_save(test);
	opaque_test_copy(data, before);
	for (mode = 0; mode < 4; mode++) {
		u64 copied = io_opaque_stat_read(IO_OPAQUE_STAT_COMPACT_BYTES);

		io_opaque_copy_per_page = mode & 1;
		if (mode == 2)
			opaque_test_cache(test, store, 4);
		memset(after, 0, length);
		KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
		/* One 64 KiB chunk without a cache, two units with one; plus the tail. */
		KUNIT_EXPECT_EQ(test, op.replacement->nr, mode < 2 ? 2U : 3U);
		opaque_test_copy(op.replacement, after);
		KUNIT_EXPECT_TRUE_MSG(test, !memcmp(before, after, length), "mode %u", mode);
		KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_COMPACT_BYTES) - copied,
				(u64)length);
		io_opaque_data_put(store, op.replacement);
		op.replacement = NULL;
		io_opaque_compact_end(&op);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), charged);
	}
	KUNIT_EXPECT_EQ(test, store->cache.hits, 2ULL);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static struct io_opaque_req *opaque_cancel_op;

/* Hand out a cached unit and cancel the copy that is about to fill it. */
static struct page *opaque_cancel_dest_page(struct io_opaque_store *store,
					    u64 pages_left, unsigned int *order)
{
	WRITE_ONCE(opaque_cancel_op->canceled, true);
	*order = store->cache.order;
	return io_opaque_unit_take(store);
}

/* Cancellation mid-copy returns its partial units to the cache, not the allocator. */
static void opaque_compact_cancel_units(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req op = { .store = store, .data = data };
	struct page *unit = opaque_test_unit(test);
	s64 charged = atomic64_read(&store->bytes);

	opaque_test_cache(test, store, 2);
	io_opaque_unit_put(store, unit);
	opaque_cancel_op = &op;
	kunit_activate_static_stub(test, io_opaque_dest_page, opaque_cancel_dest_page);
	KUNIT_EXPECT_EQ(test, io_opaque_compact_build(&op), -ECANCELED);
	kunit_deactivate_static_stub(test, io_opaque_dest_page);
	KUNIT_ASSERT_NOT_NULL(test, op.replacement);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 0U);
	KUNIT_EXPECT_PTR_EQ(test, list_first_entry(&op.replacement->extents,
						   struct io_opaque_extent, list)->backing->page,
			    unit);
	io_opaque_data_put(store, op.replacement);
	op.replacement = NULL;
	io_opaque_compact_end(&op);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 1U);
	KUNIT_EXPECT_EQ(test, page_count(unit), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), charged);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/* CREATE writes whole units taken from the cache. */
static void opaque_create_uses_cache(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_req op = {
		.store = store, .op = IORING_OPAQUE_CREATE, .length = OPAQUE_TEST_UNIT + 100,
	};
	struct page *unit = opaque_test_unit(test);
	struct io_opaque_extent *extent;
	unsigned long addr;
	u8 *pattern, *after;

	addr = kunit_vm_mmap(test, NULL, 0, 2 * OPAQUE_TEST_UNIT, PROT_READ | PROT_WRITE,
			     MAP_ANONYMOUS | MAP_PRIVATE, 0);
	if (!addr || addr >= TASK_SIZE) {
		put_page(unit);
		kunit_skip(test, "no userspace mapping for CREATE input");
	}
	pattern = kunit_kmalloc(test, op.length, GFP_KERNEL);
	after = kunit_kzalloc(test, op.length, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pattern);
	KUNIT_ASSERT_NOT_NULL(test, after);
	get_random_bytes(pattern, op.length);
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)addr, pattern, op.length), 0UL);
	op.addr = (void __user *)addr;
	opaque_test_cache(test, store, 2);
	io_opaque_unit_put(store, unit);
	op.data = io_opaque_data_alloc();
	KUNIT_ASSERT_NOT_NULL(test, op.data);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, ALIGN(op.length, PAGE_SIZE), true));
	op.create_reserved = ALIGN(op.length, PAGE_SIZE);
	KUNIT_ASSERT_EQ(test, io_opaque_create_build(&op), 0);
	KUNIT_EXPECT_EQ(test, op.create_reserved, 0ULL);
	KUNIT_EXPECT_EQ(test, op.data->nr, 2U);
	KUNIT_EXPECT_TRUE(test, op.data->dense);
	extent = list_first_entry(&op.data->extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, unit);
	KUNIT_EXPECT_TRUE(test, extent->backing->owned);
	KUNIT_EXPECT_EQ(test, store->cache.hits, 1ULL);
	opaque_test_copy(op.data, after);
	KUNIT_EXPECT_TRUE(test, !memcmp(pattern, after, op.length));
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), (s64)op.length);
	io_opaque_create_release(&op);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 1U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static const struct kernel_param *opaque_test_param(const char *name)
{
	const struct kernel_param *kp;

	for (kp = __start___param; kp < __stop___param; kp++)
		if (!strcmp(kp->name, name))
			return kp;
	return NULL;
}

/* Tunables and counters appear under /sys/module/io_uring_opaque/parameters/. */
static void opaque_params_sysfs(struct kunit *test)
{
	const struct kernel_param *order = opaque_test_param("io_uring_opaque.unit_order");
	const struct kernel_param *hits = opaque_test_param("io_uring_opaque.stat_cache_hits");
	struct kobject *kobj = kset_find_obj(module_kset, "io_uring_opaque");
	char *buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	u64 value;

	KUNIT_EXPECT_NOT_NULL(test, kobj);
	kobject_put(kobj);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	KUNIT_ASSERT_NOT_NULL(test, order);
	KUNIT_ASSERT_NOT_NULL(test, hits);
	KUNIT_EXPECT_NOT_NULL(test, opaque_test_param("io_uring_opaque.cache_bytes"));
	KUNIT_EXPECT_NOT_NULL(test, opaque_test_param("io_uring_opaque.inline_compact_max"));
	KUNIT_EXPECT_NOT_NULL(test, opaque_test_param("io_uring_opaque.compact_copy_per_page"));
	KUNIT_EXPECT_NOT_NULL(test, opaque_test_param("io_uring_opaque.stat_compact_bytes"));
	KUNIT_EXPECT_EQ(test, order->perm, (u16)0644);
	KUNIT_EXPECT_EQ(test, hits->perm, (u16)0444);
	opaque_test_params_save(test);
	KUNIT_EXPECT_EQ(test, order->ops->set("5", order), -EINVAL);
	KUNIT_EXPECT_EQ(test, order->ops->set("4", order), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_unit_order, 4U);
	KUNIT_EXPECT_EQ(test, hits->ops->set("1", hits), -EPERM);
	KUNIT_EXPECT_GT(test, hits->ops->get(buf, hits), 0);
	KUNIT_EXPECT_EQ(test, kstrtou64(strim(buf), 10, &value), 0);
	KUNIT_EXPECT_EQ(test, value, io_opaque_stat_read(IO_OPAQUE_STAT_CACHE_HITS));
}

static void opaque_test_noop_work(struct work_struct *work)
{
}

static void opaque_test_cancel_work(void *work)
{
	cancel_delayed_work_sync(work);
}

/*
 * Manual COMPACT up to inline_compact_max copies and publishes in the issuer;
 * zero (the default) and larger objects keep the dispatcher queue.
 */
static void opaque_compact_inline(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_ring_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct io_kiocb *req = kunit_kcalloc(test, 4, sizeof(*req), GFP_KERNEL);
	u64 inlined = io_opaque_stat_read(IO_OPAQUE_STAT_COMPACT_INLINE);
	u64 queued = io_opaque_stat_read(IO_OPAQUE_STAT_COMPACT_WORKER);
	static const unsigned int limits[] = { 0, 80000, 79999, 80000 };
	struct io_opaque_data *data[4];
	struct io_opaque_req ops[4];
	u64 limit;
	int i, ret[4];

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_NOT_NULL(test, req);
	opaque_test_params_save(test);
	mutex_init(&ctx->uring_lock);
	INIT_LIST_HEAD(&ctx->opaque_waits);
	store->config.hard_limit = 256 * PAGE_SIZE;
	INIT_LIST_HEAD(&store->copies);
	/* Queued requests stay queued: this dispatcher does nothing. */
	INIT_DELAYED_WORK(&store->compact_work, opaque_test_noop_work);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_cancel_work,
							&store->compact_work), 0);
	for (i = 0; i < 4; i++) {
		data[i] = opaque_sparse_data(test, store);
		store->objects[i].data = data[i];
		store->objects[i].generation = 9;
		__set_bit(i, store->object_used);
		store->nr_objects++;
		req[i].ctx = ctx;
		ops[i] = (struct io_opaque_req) {
			.store = store, .req = &req[i], .op = IORING_OPAQUE_COMPACT,
			.handle = io_opaque_handle(i, 9),
		};
		INIT_LIST_HEAD(&ops[i].wait);
		INIT_LIST_HEAD(&ops[i].cancel);
	}
	limit = store->config.hard_limit;
	mutex_lock(&ctx->uring_lock);
	for (i = 0; i < 4; i++) {
		io_opaque_inline_compact_max = limits[i];
		/* The last copy cannot reserve its destination. */
		if (i == 3)
			store->config.hard_limit = atomic64_read(&store->bytes) +
						   ALIGN(80000, PAGE_SIZE) - PAGE_SIZE;
		ret[i] = io_opaque_compact_start(&ops[i]);
	}
	mutex_unlock(&ctx->uring_lock);
	store->config.hard_limit = limit;
	KUNIT_EXPECT_EQ(test, ret[0], IOU_ISSUE_SKIP_COMPLETE);
	KUNIT_EXPECT_EQ(test, ret[1], 0);
	KUNIT_EXPECT_EQ(test, ret[2], IOU_ISSUE_SKIP_COMPLETE);
	KUNIT_EXPECT_EQ(test, ret[3], -ENOBUFS);
	/* Inline: published synchronously, nothing queued or left pinned. */
	KUNIT_EXPECT_PTR_NE(test, store->objects[1].data, data[1]);
	KUNIT_EXPECT_TRUE(test, store->objects[1].data->dense);
	KUNIT_EXPECT_FALSE(test, store->objects[1].compacting);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->compactions), 1LL);
	/* A failed inline copy keeps the original and releases the claim. */
	KUNIT_EXPECT_PTR_EQ(test, store->objects[3].data, data[3]);
	KUNIT_EXPECT_FALSE(test, store->objects[3].compacting);
	for (i = 1; i < 4; i += 2) {
		KUNIT_EXPECT_PTR_EQ(test, ops[i].data, NULL);
		KUNIT_EXPECT_PTR_EQ(test, ops[i].replacement, NULL);
		KUNIT_EXPECT_EQ(test, ops[i].transient, 0ULL);
		KUNIT_EXPECT_TRUE(test, list_empty(&ops[i].wait));
		KUNIT_EXPECT_TRUE(test, list_empty(&ops[i].cancel));
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
	/* Queued exactly as before: dispatcher list, cancellation list, pinned source. */
	for (i = 0; i < 4; i += 2) {
		KUNIT_EXPECT_EQ(test, ops[i].phase, IO_OPAQUE_COPY_WORK);
		KUNIT_EXPECT_TRUE(test, store->objects[i].compacting);
		KUNIT_EXPECT_PTR_EQ(test, ops[i].data, data[i]);
		KUNIT_EXPECT_FALSE(test, list_empty(&ops[i].cancel));
	}
	KUNIT_EXPECT_PTR_EQ(test, list_first_entry(&store->copies, struct io_opaque_req, wait),
			    &ops[0]);
	KUNIT_EXPECT_PTR_EQ(test, list_last_entry(&store->copies, struct io_opaque_req, wait),
			    &ops[2]);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_COMPACT_INLINE) - inlined, 2ULL);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_COMPACT_WORKER) - queued, 2ULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)(120 * PAGE_SIZE + ALIGN(80000, PAGE_SIZE)));
	/* Unwind the queued requests as request cleanup would. */
	cancel_delayed_work_sync(&store->compact_work);
	for (i = 0; i < 4; i += 2) {
		scoped_guard(spinlock, &store->wait_lock)
			list_del_init(&ops[i].wait);
		list_del_init(&ops[i].cancel);
		store->objects[i].compacting = false;
		io_opaque_data_put(store, ops[i].data);
	}
	for (i = 0; i < 4; i++) {
		struct io_opaque_data *old;

		mutex_lock(&store->tables);
		old = io_opaque_detach(store, &store->objects[i]);
		mutex_unlock(&store->tables);
		io_opaque_data_put(store, old);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static struct kunit_case opaque_cases[] = {
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
	KUNIT_CASE(opaque_cache_miss_then_hit),
	KUNIT_CASE(opaque_cache_cooling_probe),
	KUNIT_CASE(opaque_cache_tx_clone),
	KUNIT_CASE(opaque_cache_limit),
	KUNIT_CASE(opaque_cache_age),
	KUNIT_CASE(opaque_cache_accounting),
	KUNIT_CASE(opaque_cache_store_lifecycle),
	KUNIT_CASE(opaque_compact_copy_runs),
	KUNIT_CASE(opaque_compact_cancel_units),
	KUNIT_CASE(opaque_create_uses_cache),
	KUNIT_CASE(opaque_compact_inline),
	KUNIT_CASE(opaque_params_sysfs),
	{}
};

static struct kunit_suite opaque_suite = {
	.name = "io_uring-opaque",
	.test_cases = opaque_cases,
};

kunit_test_suite(opaque_suite);
