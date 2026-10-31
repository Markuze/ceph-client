// SPDX-License-Identifier: GPL-2.0
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/inet.h>
#include <linux/kobject.h>
#include <linux/kstrtox.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/random.h>
#include <linux/rtnetlink.h>
#include <linux/sched/user.h>
#include <linux/skbuff.h>
#include <linux/uaccess.h>
#include <net/hotdata.h>
#include <net/tcp.h>

#include "io_uring.h"
#include "mpscq.h"
#include "opaque_internal.h"

static void opaque_test_txs_free(void *store)
{
	io_opaque_txs_free(store);
}

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
	spin_lock_init(&store->tx_lock);
	spin_lock_init(&store->transient_lock);
	seqcount_spinlock_init(&store->transient_seq, &store->transient_lock);
	INIT_LIST_HEAD(&store->budget_waits);
	INIT_LIST_HEAD(&store->candidates);
	INIT_LIST_HEAD(&store->txs_idle);
	for (i = 0; i < 4; i++)
		INIT_LIST_HEAD(&store->objects[i].candidate);
	/* Idle TX queues are separate allocations; free them before the store goes. */
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_txs_free, store), 0);
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

/* Requests that only queue: no handle, no body and no io_kiocb behind them. */
static void opaque_test_tx_op(struct io_opaque_req *op, struct io_opaque_store *store)
{
	memset(op, 0, sizeof(*op));
	op->store = store;
	op->total_length = 100;
	INIT_LIST_HEAD(&op->wait);
	INIT_LIST_HEAD(&op->cancel);
	INIT_LIST_HEAD(&op->send);
}

/* The send queued its whole frame, or stopped after a byte of it. */
static void opaque_test_tx_done(struct io_opaque_req *op, bool whole)
{
	op->tx_started = true;
	op->progress = whole ? op->total_length : 1;
}

/* Queues in the tree, for socket @key or (NULL) for any socket. */
static unsigned int opaque_test_txs(struct io_opaque_store *store, struct sock *key)
{
	struct rb_node *node;
	unsigned int nr = 0;

	for (node = rb_first(&store->txs); node; node = rb_next(node))
		if (!key || rb_entry(node, struct io_opaque_tx, node)->sk == key)
			nr++;
	return nr;
}

/* Queues that hold or expect a request, or are not on the idle list. */
static unsigned int opaque_test_txs_busy(struct io_opaque_store *store)
{
	struct rb_node *node;
	unsigned int busy = 0;

	for (node = rb_first(&store->txs); node; node = rb_next(node)) {
		struct io_opaque_tx *tx = rb_entry(node, struct io_opaque_tx, node);

		if (!list_empty(&tx->requests) || tx->admitting || tx->failed ||
		    list_empty(&tx->idle))
			busy++;
	}
	return busy;
}

#define OPAQUE_TEST_WOKEN	4

static struct {
	struct io_opaque_req *op[OPAQUE_TEST_WOKEN];
	int result[OPAQUE_TEST_WOKEN];
	unsigned int nr;
} opaque_woken;

static void opaque_test_ready(struct io_opaque_req *op, int result)
{
	if (opaque_woken.nr < OPAQUE_TEST_WOKEN) {
		opaque_woken.op[opaque_woken.nr] = op;
		opaque_woken.result[opaque_woken.nr] = result;
	}
	opaque_woken.nr++;
}

/* A failing head wakes its followers; record them instead of queueing task work. */
static void opaque_test_wakeups(struct kunit *test)
{
	memset(&opaque_woken, 0, sizeof(opaque_woken));
	kunit_activate_static_stub(test, io_opaque_queue_ready, opaque_test_ready);
}

/* Frames leave a socket's queue in admission order; a short head fails the rest. */
static void opaque_tx_order(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct sock *sk = kunit_kcalloc(test, 2, sizeof(*sk), GFP_KERNEL);
	struct io_opaque_req a, b, c, d;

	KUNIT_ASSERT_NOT_NULL(test, sk);
	opaque_test_tx_op(&a, store);
	opaque_test_tx_op(&b, store);
	opaque_test_tx_op(&c, store);
	opaque_test_tx_op(&d, store);
	/* The first admission is the head; the next one waits behind it. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&a, &sk[0]), 1);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&b, &sk[0]), 0);
	KUNIT_EXPECT_EQ(test, b.phase, IO_OPAQUE_SEND_WAIT);
	KUNIT_ASSERT_NOT_NULL(test, a.tx);
	KUNIT_EXPECT_PTR_EQ(test, a.tx, b.tx);
	/* Another socket has its own queue. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&c, &sk[1]), 1);
	KUNIT_EXPECT_PTR_NE(test, c.tx, a.tx);
	/* A whole frame hands the queue to its follower, claimed for task work. */
	opaque_test_tx_done(&a, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&a), &b);
	KUNIT_EXPECT_EQ(test, b.phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_FALSE(test, b.canceled);
	KUNIT_EXPECT_PTR_EQ(test, a.tx, NULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&a.send));
	opaque_test_tx_done(&b, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&b), NULL);
	opaque_test_tx_done(&c, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&c), NULL);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);

	/* A head that stopped short cancels everything admitted behind it. */
	opaque_test_wakeups(test);
	opaque_test_tx_op(&a, store);
	opaque_test_tx_op(&b, store);
	opaque_test_tx_op(&c, store);
	opaque_test_tx_op(&d, store);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&a, &sk[0]), 1);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&b, &sk[0]), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&c, &sk[0]), 0);
	opaque_test_tx_done(&a, false);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&a), NULL);
	KUNIT_EXPECT_TRUE(test, b.canceled);
	KUNIT_EXPECT_TRUE(test, c.canceled);
	KUNIT_ASSERT_EQ(test, opaque_woken.nr, 2U);
	KUNIT_EXPECT_PTR_EQ(test, opaque_woken.op[0], &b);
	KUNIT_EXPECT_PTR_EQ(test, opaque_woken.op[1], &c);
	KUNIT_EXPECT_EQ(test, opaque_woken.result[0], -ECANCELED);
	KUNIT_EXPECT_EQ(test, opaque_woken.result[1], -ECANCELED);
	/* Later frames are refused while the canceled followers are queued. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&d, &sk[0]), -ECANCELED);
	KUNIT_EXPECT_PTR_EQ(test, d.tx, NULL);
	/* Their own releases promote nobody: they were all claimed at once. */
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&b), NULL);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&c), NULL);
	KUNIT_EXPECT_EQ(test, opaque_woken.nr, 2U);
	/* The drained queue forgets the failure. */
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&d, &sk[0]), 1);
	opaque_test_tx_done(&d, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&d), NULL);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
}

/* One published object in slot 0 under handle generation 9. */
static struct io_opaque_data *opaque_test_object(struct kunit *test,
						 struct io_opaque_store *store)
{
	struct io_opaque_data *data = opaque_sparse_data(test, store);

	store->objects[0].data = data;
	store->objects[0].generation = 9;
	store->nr_objects++;
	__set_bit(0, store->object_used);
	return data;
}

/* A send whose frame is fully queued leaves its socket's queue at that point. */
static void opaque_tx_inline_admission(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct sock *sk = kunit_kcalloc(test, 2, sizeof(*sk), GFP_KERNEL);
	struct io_opaque_req a, b, c, d;

	KUNIT_ASSERT_NOT_NULL(test, sk);
	opaque_test_tx_op(&a, store);
	opaque_test_tx_op(&b, store);
	opaque_test_tx_op(&c, store);
	opaque_test_tx_op(&d, store);
	/* The next send finds the queue empty and is the head at once. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&a, &sk[0]), 1);
	opaque_test_tx_done(&a, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&a), NULL);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&b, &sk[0]), 1);
	/* Another socket has a head of its own. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&c, &sk[1]), 1);
	KUNIT_EXPECT_PTR_NE(test, b.tx, c.tx);
	/* A request behind the head waits; a retried head goes on. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&d, &sk[0]), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&b, &sk[0]), 1);
	KUNIT_EXPECT_PTR_EQ(test, b.tx, d.tx);
	/* A resumed request that is not the head parks again, in its place. */
	d.phase = IO_OPAQUE_IDLE;
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&d, &sk[0]), 0);
	KUNIT_EXPECT_EQ(test, d.phase, IO_OPAQUE_SEND_WAIT);
	KUNIT_EXPECT_PTR_EQ(test, list_first_entry(&b.tx->requests, struct io_opaque_req, send),
			    &b);
	/* Once the head is done, the resumed request is the head and proceeds. */
	opaque_test_tx_done(&b, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&b), &d);
	d.phase = IO_OPAQUE_IDLE;
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&d, &sk[0]), 1);
	opaque_test_tx_done(&d, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&d), NULL);
	opaque_test_tx_done(&c, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&c), NULL);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
}

static struct io_opaque_req *opaque_race_head;

/* Pin the object for real, then fail the queue: the window before the enqueue. */
static int opaque_test_acquire_fail(struct io_opaque_req *op)
{
	int ret;

	kunit_deactivate_static_stub(kunit_get_current_test(), io_opaque_send_acquire);
	ret = io_opaque_send_acquire(op);
	opaque_race_head->tx->failed = true;
	return ret;
}

/* A failed head cancels its followers and refuses later frames until they are gone. */
static void opaque_tx_failed_head(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_test_object(test, store);
	struct sock *sk = kunit_kcalloc(test, 2, sizeof(*sk), GFP_KERNEL);
	struct io_opaque_req a, b, c, d, e;
	struct io_opaque_tx *tx;
	u64 handle = io_opaque_handle(0, 9);

	KUNIT_ASSERT_NOT_NULL(test, sk);
	opaque_test_wakeups(test);
	opaque_test_tx_op(&a, store);
	opaque_test_tx_op(&b, store);
	opaque_test_tx_op(&c, store);
	opaque_test_tx_op(&d, store);
	opaque_test_tx_op(&e, store);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&a, &sk[0]), 1);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&b, &sk[0]), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&c, &sk[0]), 0);
	tx = a.tx;
	opaque_test_tx_done(&a, false);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&a), NULL);
	KUNIT_EXPECT_TRUE(test, tx->failed);
	KUNIT_EXPECT_TRUE(test, b.canceled);
	KUNIT_EXPECT_TRUE(test, c.canceled);
	KUNIT_ASSERT_EQ(test, opaque_woken.nr, 2U);
	KUNIT_EXPECT_PTR_EQ(test, opaque_woken.op[0], &b);
	KUNIT_EXPECT_PTR_EQ(test, opaque_woken.op[1], &c);
	KUNIT_EXPECT_EQ(test, opaque_woken.result[0], -ECANCELED);
	KUNIT_EXPECT_EQ(test, opaque_woken.result[1], -ECANCELED);
	/* A new frame is refused, and a LAST send before it takes the object. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&d, &sk[0]), -ECANCELED);
	e.handle = handle;
	e.length = 100;
	e.send_flags = IORING_OPAQUE_SEND_LAST;
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&e, &sk[0]), -ECANCELED);
	KUNIT_EXPECT_FALSE(test, e.consumed);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, data);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	/* The followers' own releases wake nobody; draining clears the failure. */
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&b), NULL);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&c), NULL);
	KUNIT_EXPECT_EQ(test, opaque_woken.nr, 2U);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&d, &sk[0]), 1);
	opaque_test_tx_done(&d, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&d), NULL);

	/* A pin taken before the queue failed is dropped when the enqueue is refused. */
	opaque_test_tx_op(&a, store);
	opaque_test_tx_op(&b, store);
	opaque_test_tx_op(&c, store);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&a, &sk[1]), 1);
	opaque_race_head = &a;
	b.handle = handle;
	b.length = 100;
	kunit_activate_static_stub(test, io_opaque_send_acquire, opaque_test_acquire_fail);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&b, &sk[1]), -ECANCELED);
	KUNIT_EXPECT_FALSE(test, b.consumed);
	KUNIT_EXPECT_PTR_EQ(test, b.data, NULL);
	KUNIT_EXPECT_PTR_EQ(test, b.tx, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	/* LAST took the object itself: it stays consumed and the request still ends. */
	a.tx->failed = false;
	c.handle = handle;
	c.length = 100;
	c.send_flags = IORING_OPAQUE_SEND_LAST;
	kunit_activate_static_stub(test, io_opaque_send_acquire, opaque_test_acquire_fail);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&c, &sk[1]), -ECANCELED);
	KUNIT_EXPECT_TRUE(test, c.consumed);
	KUNIT_EXPECT_PTR_EQ(test, c.data, NULL);
	KUNIT_EXPECT_PTR_EQ(test, c.tx, NULL);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, NULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, a.tx->admitting, 0U);
	opaque_test_tx_done(&a, false);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&a), NULL);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
}

/* A drained queue stays for the next frame on its socket, up to a cap. */
static void opaque_tx_idle_cache(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct sock *sk = kunit_kcalloc(test, IO_OPAQUE_TX_IDLE_MAX + 2, sizeof(*sk),
					GFP_KERNEL);
	struct io_opaque_req a, b, held;
	struct io_opaque_tx *tx;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, sk);
	opaque_test_tx_op(&a, store);
	opaque_test_tx_op(&b, store);
	opaque_test_tx_op(&held, store);
	/* Draining keeps the queue in the tree, idle and clean. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&a, &sk[0]), 1);
	tx = a.tx;
	KUNIT_EXPECT_EQ(test, store->nr_txs_idle, 0U);
	opaque_test_tx_done(&a, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&a), NULL);
	KUNIT_EXPECT_EQ(test, store->nr_txs_idle, 1U);
	KUNIT_EXPECT_EQ(test, opaque_test_txs(store, &sk[0]), 1U);
	KUNIT_EXPECT_PTR_EQ(test, rb_entry(store->txs.rb_node, struct io_opaque_tx, node), tx);
	KUNIT_EXPECT_FALSE(test, tx->failed);
	/* The next frame on the same key takes that queue; nothing is allocated. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&b, &sk[0]), 1);
	KUNIT_EXPECT_PTR_EQ(test, b.tx, tx);
	KUNIT_EXPECT_EQ(test, store->nr_txs_idle, 0U);
	KUNIT_EXPECT_EQ(test, opaque_test_txs(store, NULL), 1U);
	opaque_test_tx_done(&b, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&b), NULL);
	/* A queue in use is never trimmed, however many others drain. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&held, &sk[IO_OPAQUE_TX_IDLE_MAX + 1]), 1);
	for (i = 1; i <= IO_OPAQUE_TX_IDLE_MAX; i++) {
		opaque_test_tx_op(&a, store);
		KUNIT_ASSERT_EQ(test, io_opaque_tx_admit(&a, &sk[i]), 1);
		opaque_test_tx_done(&a, true);
		KUNIT_ASSERT_PTR_EQ(test, io_opaque_tx_release(&a), NULL);
	}
	/* One more idle queue than the cap: the oldest, key 0, went. */
	KUNIT_EXPECT_EQ(test, store->nr_txs_idle, (u32)IO_OPAQUE_TX_IDLE_MAX);
	KUNIT_EXPECT_EQ(test, opaque_test_txs(store, &sk[0]), 0U);
	KUNIT_EXPECT_EQ(test, opaque_test_txs(store, &sk[1]), 1U);
	KUNIT_EXPECT_EQ(test, opaque_test_txs(store, &sk[IO_OPAQUE_TX_IDLE_MAX]), 1U);
	KUNIT_EXPECT_EQ(test, opaque_test_txs(store, &sk[IO_OPAQUE_TX_IDLE_MAX + 1]), 1U);
	KUNIT_EXPECT_EQ(test, opaque_test_txs(store, NULL),
			(unsigned int)IO_OPAQUE_TX_IDLE_MAX + 1);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 1U);
	opaque_test_tx_done(&held, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&held), NULL);
	KUNIT_EXPECT_EQ(test, store->nr_txs_idle, (u32)IO_OPAQUE_TX_IDLE_MAX);
	KUNIT_EXPECT_EQ(test, opaque_test_txs(store, NULL), (unsigned int)IO_OPAQUE_TX_IDLE_MAX);
	KUNIT_EXPECT_EQ(test, opaque_test_txs(store, &sk[1]), 0U);
	/* Store teardown frees every idle queue. */
	io_opaque_txs_free(store);
	KUNIT_EXPECT_TRUE(test, RB_EMPTY_ROOT(&store->txs));
	KUNIT_EXPECT_EQ(test, store->nr_txs_idle, 0U);
	KUNIT_EXPECT_TRUE(test, list_empty(&store->txs_idle));
}

/* Lock order: tables alone around the handle; tx_lock before wait_lock to park and wake. */
static void opaque_tx_lock_order(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_test_object(test, store);
	struct sock *sk = kunit_kzalloc(test, sizeof(*sk), GFP_KERNEL);
	struct io_opaque_req a, b;

	KUNIT_ASSERT_NOT_NULL(test, sk);
	opaque_test_tx_op(&a, store);
	opaque_test_tx_op(&b, store);
	a.handle = io_opaque_handle(0, 9);
	b.handle = a.handle;
	a.length = 100;
	b.length = 100;
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&a, sk), 1);
	lockdep_assert_none_held_once();
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&b, sk), 0);
	lockdep_assert_none_held_once();
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 3);
	opaque_test_tx_done(&a, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&a), &b);
	lockdep_assert_none_held_once();
	opaque_test_tx_done(&b, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&b), NULL);
	lockdep_assert_none_held_once();
	io_opaque_data_put(store, a.data);
	io_opaque_data_put(store, b.data);
	mutex_lock(&store->tables);
	data = io_opaque_detach(store, &store->objects[0]);
	mutex_unlock(&store->tables);
	io_opaque_data_put(store, data);
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
	unsigned int send_copy_max;
	bool copy_per_page;
};

static void opaque_test_params_restore(void *arg)
{
	struct opaque_test_params *saved = arg;

	io_opaque_cache_bytes = saved->cache_bytes;
	io_opaque_unit_order = saved->unit_order;
	io_opaque_inline_compact_max = saved->inline_compact_max;
	io_opaque_send_copy_max = saved->send_copy_max;
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
	saved->send_copy_max = io_opaque_send_copy_max;
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
	/* MSG_SPLICE_PAGES: one fragment and one page reference per unit. */
	skb->ip_summed = CHECKSUM_PARTIAL;
	iov_iter_bvec(&iter, ITER_SOURCE, op.replacement->bvec, op.replacement->nr, len);
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(skb, &iter, len), (ssize_t)len);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, 2);
	KUNIT_EXPECT_EQ(test, page_count(units[0]), 2);
	KUNIT_EXPECT_EQ(test, page_count(units[1]), 2);
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

struct opaque_splice {
	struct io_opaque_store *store;
	struct io_opaque_data *data;
	struct io_opaque_req op;
	struct page *units[3];
	struct sk_buff *skb;
	struct iov_iter iter;
	u8 *source;
};

/* The 80,000 byte object compacted into two units and a tail, with an empty skb. */
static void opaque_splice_setup(struct kunit *test, struct opaque_splice *f)
{
	f->store = opaque_test_store(test);
	f->data = opaque_sparse_data(test, f->store);
	f->op = (struct io_opaque_req) { .store = f->store, .data = f->data };
	f->skb = alloc_skb(0, GFP_KERNEL);
	f->source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f->skb);
	KUNIT_ASSERT_NOT_NULL(test, f->source);
	opaque_test_cache(test, f->store, 4);
	opaque_test_copy(f->data, f->source);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&f->op), 0);
	KUNIT_ASSERT_EQ(test, opaque_test_pages(f->op.replacement, f->units, 3), 3U);
	f->skb->ip_summed = CHECKSUM_PARTIAL;
	iov_iter_bvec(&f->iter, ITER_SOURCE, f->op.replacement->bvec, f->op.replacement->nr,
		      80000);
}

static void opaque_splice_done(struct opaque_splice *f)
{
	io_opaque_data_put(f->store, f->op.replacement);
	f->op.replacement = NULL;
	io_opaque_compact_end(&f->op);
	io_opaque_data_put(f->store, f->data);
}

/* A folio is one fragment and one page reference, however many calls extract it. */
static void opaque_splice_folio_runs(struct kunit *test)
{
	size_t len = OPAQUE_TEST_UNIT + 2 * PAGE_SIZE, first = 5 * PAGE_SIZE;
	struct opaque_splice f;
	skb_frag_t *frag;

	opaque_splice_setup(test, &f);
	iov_iter_truncate(&f.iter, len);
	/* The first call stops inside the first unit. */
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(f.skb, &f.iter, first), (ssize_t)first);
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 1);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 2);
	/* The rest of the unit extends that fragment; the next unit starts another. */
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(f.skb, &f.iter, len - first),
			(ssize_t)(len - first));
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 2);
	frag = &skb_shinfo(f.skb)->frags[0];
	KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), f.units[0]);
	KUNIT_EXPECT_EQ(test, skb_frag_off(frag), 0U);
	KUNIT_EXPECT_EQ(test, skb_frag_size(frag), (unsigned int)OPAQUE_TEST_UNIT);
	frag = &skb_shinfo(f.skb)->frags[1];
	KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), f.units[1]);
	KUNIT_EXPECT_EQ(test, skb_frag_off(frag), 0U);
	KUNIT_EXPECT_EQ(test, skb_frag_size(frag), (unsigned int)(2 * PAGE_SIZE));
	KUNIT_EXPECT_EQ(test, f.skb->len, (unsigned int)len);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 2);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 2);
	kfree_skb(f.skb);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 1);
	opaque_splice_done(&f);
}

/* A range that starts inside a unit leaves a multi-page fragment at that offset. */
static void opaque_splice_subrange_offset(struct kunit *test)
{
	size_t skip = PAGE_SIZE + 100, len = 40000, head = OPAQUE_TEST_UNIT - skip;
	u8 *copy = kunit_kmalloc(test, len, GFP_KERNEL);
	struct opaque_splice f;
	skb_frag_t *frag;

	KUNIT_ASSERT_NOT_NULL(test, copy);
	opaque_splice_setup(test, &f);
	/* What io_opaque_send_iter() does for op->offset and op->length. */
	iov_iter_advance(&f.iter, skip);
	iov_iter_truncate(&f.iter, len);
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(f.skb, &f.iter, len), (ssize_t)len);
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 2);
	frag = &skb_shinfo(f.skb)->frags[0];
	KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), f.units[0]);
	KUNIT_EXPECT_EQ(test, skb_frag_off(frag), (unsigned int)skip);
	KUNIT_EXPECT_EQ(test, skb_frag_size(frag), (unsigned int)head);
	frag = &skb_shinfo(f.skb)->frags[1];
	KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), f.units[1]);
	KUNIT_EXPECT_EQ(test, skb_frag_off(frag), 0U);
	KUNIT_EXPECT_EQ(test, skb_frag_size(frag), (unsigned int)(len - head));
	/* Readers walk a fragment page by page, from the page its offset lands in. */
	KUNIT_ASSERT_EQ(test, skb_copy_bits(f.skb, 0, copy, len), 0);
	KUNIT_EXPECT_MEMEQ(test, copy, f.source + skip, len);
	kfree_skb(f.skb);
	opaque_splice_done(&f);
}

/* A fragment never crosses a folio: split pages stay one fragment and reference each. */
static void opaque_splice_split_pages(struct kunit *test)
{
	size_t len = PAGE_SIZE << 4;
	struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);
	struct page *page = alloc_pages(GFP_KERNEL, 4);
	struct iov_iter iter;
	struct bio_vec bv;
	int i;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_NOT_NULL(test, page);
	/* Sixteen physically contiguous pages that are sixteen folios. */
	split_page(page, 4);
	bvec_set_page(&bv, page, len, 0);
	iov_iter_bvec(&iter, ITER_SOURCE, &bv, 1, len);
	skb->ip_summed = CHECKSUM_PARTIAL;
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(skb, &iter, len), (ssize_t)len);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, 16);
	for (i = 0; i < 16; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), page + i);
		KUNIT_EXPECT_EQ(test, skb_frag_off(frag), 0U);
		KUNIT_EXPECT_EQ(test, skb_frag_size(frag), (unsigned int)PAGE_SIZE);
		KUNIT_EXPECT_EQ(test, page_count(page + i), 2);
	}
	kfree_skb(skb);
	for (i = 0; i < 16; i++) {
		KUNIT_EXPECT_EQ(test, page_count(page + i), 1);
		__free_page(page + i);
	}
}

/* Without checksum offload every page is summed in place, whatever the fragments. */
static void opaque_splice_csum_none(struct kunit *test)
{
	size_t skip = 101, len = 2 * OPAQUE_TEST_UNIT - skip;
	struct opaque_splice f;

	opaque_splice_setup(test, &f);
	f.skb->ip_summed = CHECKSUM_NONE;
	f.skb->csum = 0;
	/* An odd start makes every later page begin at an odd offset in the skb. */
	iov_iter_advance(&f.iter, skip);
	iov_iter_truncate(&f.iter, len);
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(f.skb, &f.iter, len), (ssize_t)len);
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 2);
	KUNIT_EXPECT_EQ(test, (__force u16)csum_fold(f.skb->csum),
			(__force u16)csum_fold(csum_partial(f.source + skip, len, 0)));
	kfree_skb(f.skb);
	opaque_splice_done(&f);
}

static void opaque_test_frag_limit_restore(void *limit)
{
	net_hotdata.sysctl_max_skb_frags = (long)limit;
}

/* At the fragment limit a splice stops after the folios that fit and leaves the rest. */
static void opaque_splice_frag_limit(struct kunit *test)
{
	size_t first = OPAQUE_TEST_UNIT, rest = 80000 - OPAQUE_TEST_UNIT;
	long limit = net_hotdata.sysctl_max_skb_frags;
	struct opaque_splice f;

	opaque_splice_setup(test, &f);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_frag_limit_restore,
							(void *)limit), 0);
	net_hotdata.sysctl_max_skb_frags = 1;
	/* The first unit is the one fragment that fits; the second would need another. */
	KUNIT_EXPECT_EQ(test, skb_splice_from_iter(f.skb, &f.iter, 80000), (ssize_t)first);
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 1);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&f.iter), rest);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 2);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 1);
	/* Nothing more fits: the next call says so and leaves the iterator alone. */
	KUNIT_EXPECT_EQ(test, skb_splice_from_iter(f.skb, &f.iter, 80000), (ssize_t)-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&f.iter), rest);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 1);
	/* One more fragment takes exactly the next unit. */
	net_hotdata.sysctl_max_skb_frags = 2;
	KUNIT_EXPECT_EQ(test, skb_splice_from_iter(f.skb, &f.iter, 80000), (ssize_t)first);
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 2);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&f.iter), rest - first);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 2);
	kfree_skb(f.skb);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 1);
	opaque_splice_done(&f);
}

/* A bvec iterator is walked by segment: skips, empty and adjacent segments, resumption. */
static void opaque_splice_bvec_segments(struct kunit *test)
{
	size_t len = 4096 + 8192 + 5000 + 100, first = 6000, second = 7000;
	size_t rest = len - first - second;
	u8 *copy = kunit_kmalloc(test, len, GFP_KERNEL);
	struct opaque_splice f;
	struct bio_vec bv[5];
	skb_frag_t *frag;

	KUNIT_ASSERT_NOT_NULL(test, copy);
	opaque_splice_setup(test, &f);
	/* Two segments of one folio in a row, an empty one, two more of the next folio. */
	bvec_set_page(&bv[0], f.units[0], 4096, 0);
	bvec_set_page(&bv[1], f.units[0], 8192, 4096);
	bvec_set_page(&bv[2], f.units[1], 0, 0);
	bvec_set_page(&bv[3], f.units[1], 5000, 300);
	bvec_set_page(&bv[4], f.units[1], 100, 5300);
	iov_iter_bvec(&f.iter, ITER_SOURCE, bv, ARRAY_SIZE(bv), len);
	/* The first call ends inside the second segment, the next resumes there. */
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(f.skb, &f.iter, first), (ssize_t)first);
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 1);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&f.iter), len - first);
	/* It finishes the folio, skips the empty segment and starts the next folio. */
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(f.skb, &f.iter, second), (ssize_t)second);
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 2);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&f.iter), rest);
	/* A limit above what is left stops at the end of the iterator. */
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(f.skb, &f.iter, SZ_64K), (ssize_t)rest);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&f.iter), 0UL);
	/* Adjacent segments of a folio extend one fragment; the next folio starts another. */
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 2);
	frag = &skb_shinfo(f.skb)->frags[0];
	KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), f.units[0]);
	KUNIT_EXPECT_EQ(test, skb_frag_off(frag), 0U);
	KUNIT_EXPECT_EQ(test, skb_frag_size(frag), 4096U + 8192U);
	frag = &skb_shinfo(f.skb)->frags[1];
	KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), f.units[1]);
	KUNIT_EXPECT_EQ(test, skb_frag_off(frag), 300U);
	KUNIT_EXPECT_EQ(test, skb_frag_size(frag), 5100U);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 2);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 2);
	KUNIT_EXPECT_EQ(test, f.skb->len, (unsigned int)len);
	KUNIT_ASSERT_EQ(test, skb_copy_bits(f.skb, 0, copy, len), 0);
	KUNIT_EXPECT_MEMEQ(test, copy, f.source, 4096 + 8192);
	KUNIT_EXPECT_MEMEQ(test, copy + 4096 + 8192, f.source + OPAQUE_TEST_UNIT + 300, 5100);
	kfree_skb(f.skb);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 1);
	opaque_splice_done(&f);
}

/* Other iterators go through the page array, and a folio is still one fragment. */
static void opaque_splice_kvec_runs(struct kunit *test)
{
	size_t skip = 100, size = PAGE_SIZE << 4, len = size - 2 * skip;
	struct page *page = alloc_pages(GFP_KERNEL | __GFP_COMP, 4);
	struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);
	u8 *copy = kunit_kmalloc(test, len, GFP_KERNEL);
	struct iov_iter iter;
	struct kvec kv;
	skb_frag_t *frag;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	get_random_bytes(page_address(page), size);
	kv.iov_base = page_address(page) + skip;
	kv.iov_len = len;
	iov_iter_kvec(&iter, ITER_SOURCE, &kv, 1, len);
	skb->ip_summed = CHECKSUM_PARTIAL;
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(skb, &iter, len), (ssize_t)len);
	/* The page array holds eight pages: two batches of the same folio make one fragment. */
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, 1);
	frag = &skb_shinfo(skb)->frags[0];
	KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), page);
	KUNIT_EXPECT_EQ(test, skb_frag_off(frag), (unsigned int)skip);
	KUNIT_EXPECT_EQ(test, skb_frag_size(frag), (unsigned int)len);
	KUNIT_EXPECT_EQ(test, page_count(page), 2);
	KUNIT_ASSERT_EQ(test, skb_copy_bits(skb, 0, copy, len), 0);
	KUNIT_EXPECT_MEMEQ(test, copy, page_address(page) + skip, len);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	__free_pages(page, 4);
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
	const struct kernel_param *copy_max = opaque_test_param("io_uring_opaque.send_copy_max");
	const struct kernel_param *body_copied =
		opaque_test_param("io_uring_opaque.stat_body_copied");
	const struct kernel_param *framing_copied =
		opaque_test_param("io_uring_opaque.stat_framing_copied");
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
	KUNIT_ASSERT_NOT_NULL(test, copy_max);
	KUNIT_ASSERT_NOT_NULL(test, body_copied);
	KUNIT_ASSERT_NOT_NULL(test, framing_copied);
	KUNIT_EXPECT_EQ(test, order->perm, (u16)0644);
	KUNIT_EXPECT_EQ(test, copy_max->perm, (u16)0644);
	KUNIT_EXPECT_EQ(test, hits->perm, (u16)0444);
	KUNIT_EXPECT_EQ(test, body_copied->perm, (u16)0444);
	KUNIT_EXPECT_EQ(test, framing_copied->perm, (u16)0444);
	opaque_test_params_save(test);
	KUNIT_EXPECT_EQ(test, order->ops->set("5", order), -EINVAL);
	KUNIT_EXPECT_EQ(test, order->ops->set("4", order), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_unit_order, 4U);
	KUNIT_EXPECT_EQ(test, hits->ops->set("1", hits), -EPERM);
	KUNIT_EXPECT_GT(test, hits->ops->get(buf, hits), 0);
	KUNIT_EXPECT_EQ(test, kstrtou64(strim(buf), 10, &value), 0);
	KUNIT_EXPECT_EQ(test, value, io_opaque_stat_read(IO_OPAQUE_STAT_CACHE_HITS));
	/* The copy threshold takes effect at the next send; its counters are read-only. */
	KUNIT_EXPECT_EQ(test, copy_max->ops->set("2048", copy_max), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_send_copy_max, 2048U);
	KUNIT_EXPECT_EQ(test, body_copied->ops->set("1", body_copied), -EPERM);
	KUNIT_EXPECT_EQ(test, framing_copied->ops->set("1", framing_copied), -EPERM);
	io_opaque_count(IO_OPAQUE_STAT_BODY_COPIED, 7);
	KUNIT_EXPECT_GT(test, body_copied->ops->get(buf, body_copied), 0);
	KUNIT_EXPECT_EQ(test, kstrtou64(strim(buf), 10, &value), 0);
	KUNIT_EXPECT_EQ(test, value, io_opaque_stat_read(IO_OPAQUE_STAT_BODY_COPIED));
}

/* A ring context that preps OBJ_SEND requests for @store, which it knows as store 1. */
struct opaque_prep {
	struct io_ring_ctx *ctx;
	struct io_opaque_store *store;
	struct io_kiocb req;
	struct io_uring_sqe sqe;
};

static void opaque_prep_cleanup(void *arg)
{
	struct opaque_prep *p = arg;

	xa_destroy(&p->ctx->opaque_stores);
	io_opaque_cache_free(p->ctx);
}

static void opaque_prep_init(struct kunit *test, struct opaque_prep *p,
			     struct io_opaque_store *store)
{
	p->ctx = kunit_kzalloc(test, sizeof(*p->ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, p->ctx);
	mutex_init(&p->ctx->uring_lock);
	INIT_LIST_HEAD(&p->ctx->opaque_waits);
	xa_init(&p->ctx->opaque_stores);
	KUNIT_ASSERT_FALSE(test, io_opaque_cache_init(p->ctx));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_prep_cleanup, p), 0);
	store->config.max_requests = 8;
	KUNIT_ASSERT_EQ(test, xa_err(xa_store(&p->ctx->opaque_stores, 1, store, GFP_KERNEL)), 0);
	p->store = store;
	p->req = (struct io_kiocb) { .ctx = p->ctx, .opcode = IORING_OP_OPAQUE_OBJ_SEND };
	p->sqe = (struct io_uring_sqe) { .opaque_store_id = 1 };
}

/* Prep the SQE in @p->sqe; the request, if one was made, is left for opaque_prep_done(). */
static int opaque_prep_send(struct opaque_prep *p, struct io_opaque_req **op)
{
	int ret = io_opaque_send_prep(&p->req, &p->sqe);

	*op = p->req.async_data;
	return ret;
}

/* Request cleanup, as the core runs it after the request completes or fails prep. */
static void opaque_prep_done(struct opaque_prep *p)
{
	if (p->req.async_data) {
		mutex_lock(&p->ctx->uring_lock);
		io_opaque_cleanup(&p->req);
		mutex_unlock(&p->ctx->uring_lock);
	}
	p->req.flags = 0;
}

/*
 * A send without a frame is resolved when it is prepared, so its first issue
 * has nothing to import. One with a frame address waits for that issue, where
 * the frame is read and the total is known.
 */
static void opaque_send_resolve(struct kunit *test)
{
	static const struct {
		u64 off, addr3;
		u32 len;
		int result;
		bool resolved;
	} cases[] = {
		{ 0, 0, 1, 0, true },
		{ 5, 0, 100, 0, true },
		{ 0, 0, INT_MAX, 0, true },
		{ 0, 0x1000, 100, 0, false },	/* body and frame */
		{ 0, 0x1000, 0, 0, false },	/* a reply made of a frame alone */
		{ 0, 0, 0, -EINVAL, false },	/* nothing to send */
		{ 0, 0, (u32)INT_MAX + 1, -EINVAL, false },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_prep *p = kunit_kzalloc(test, sizeof(*p), GFP_KERNEL);
	struct io_opaque_req *op;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, p);
	opaque_prep_init(test, p, store);
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		u64 framing = io_opaque_stat_read(IO_OPAQUE_STAT_FRAMING_COPIED);

		p->sqe.addr = io_opaque_handle(0, 9);
		p->sqe.off = cases[i].off;
		p->sqe.addr3 = cases[i].addr3;
		p->sqe.len = cases[i].len;
		KUNIT_EXPECT_EQ(test, opaque_prep_send(p, &op), cases[i].result);
		if (!cases[i].result) {
			u32 total = cases[i].len;

			KUNIT_ASSERT_NOT_NULL(test, op);
			KUNIT_EXPECT_EQ(test, op->resolved, cases[i].resolved);
			if (!cases[i].resolved)
				total = 0;
			KUNIT_EXPECT_EQ(test, op->total_length, total);
			KUNIT_EXPECT_EQ(test, op->length, cases[i].len);
		}
		/* Prep reads no user memory. */
		KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_FRAMING_COPIED), framing);
		opaque_prep_done(p);
	}
	KUNIT_EXPECT_EQ(test, atomic_read(&store->requests), 0);
}

/* Bodies up to send_copy_max are copied by TCP; zero splices every body. */
static void opaque_send_copy_policy(struct kunit *test)
{
	static const u32 lengths[] = { 1, 1023, 1024, 1025, 4096, SZ_64K, INT_MAX };
	struct io_opaque_req op = {};
	unsigned int i;

	opaque_test_params_save(test);
	/* Off by default: no length is copied. */
	io_opaque_send_copy_max = 0;
	for (i = 0; i < ARRAY_SIZE(lengths); i++) {
		op.length = lengths[i];
		KUNIT_EXPECT_TRUE(test, io_opaque_send_splices(&op));
	}
	/* The threshold itself still copies; one byte more splices. */
	io_opaque_send_copy_max = 1024;
	for (i = 0; i < ARRAY_SIZE(lengths); i++) {
		op.length = lengths[i];
		KUNIT_EXPECT_EQ(test, io_opaque_send_splices(&op), lengths[i] > 1024);
	}
	io_opaque_send_copy_max = 1;
	op.length = 1;
	KUNIT_EXPECT_FALSE(test, io_opaque_send_splices(&op));
	op.length = 2;
	KUNIT_EXPECT_TRUE(test, io_opaque_send_splices(&op));
	/* A threshold above every length copies everything. */
	io_opaque_send_copy_max = UINT_MAX;
	op.length = INT_MAX;
	KUNIT_EXPECT_FALSE(test, io_opaque_send_splices(&op));
}

struct opaque_wire {
	struct io_ring_ctx *ctx;
	struct io_kiocb req[2];
	struct socket *listener, *tx, *rx;
	struct file *file;
};

static void opaque_wire_cleanup(void *arg)
{
	struct opaque_wire *w = arg;

	if (w->file)
		fput(w->file);
	else if (w->tx)
		sock_release(w->tx);
	if (w->rx)
		sock_release(w->rx);
	if (w->listener)
		sock_release(w->listener);
}

/* A TCP connection over loopback: sends go out of @tx's file, the test reads @rx. */
static void opaque_wire_init(struct kunit *test, struct opaque_wire *w)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	int ret, i;

	w->ctx = kunit_kzalloc(test, sizeof(*w->ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, w->ctx);
	mutex_init(&w->ctx->uring_lock);
	INIT_LIST_HEAD(&w->ctx->opaque_waits);
	w->ctx->flags = IORING_SETUP_DEFER_TASKRUN;
	mpscq_init(&w->ctx->work_list, &w->ctx->work_head);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_wire_cleanup, w), 0);
	rtnl_lock();
	ret = dev_change_flags(init_net.loopback_dev, IFF_UP, NULL);
	rtnl_unlock();
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &w->listener);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kernel_bind(w->listener, (struct sockaddr_unsized *)&addr, sizeof(addr));
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, kernel_listen(w->listener, 1), 0);
	KUNIT_ASSERT_GT(test, kernel_getsockname(w->listener, (struct sockaddr *)&addr), 0);
	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &w->tx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kernel_connect(w->tx, (struct sockaddr_unsized *)&addr, sizeof(addr), 0);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, kernel_accept(w->listener, &w->rx, O_NONBLOCK), 0);
	w->tx->sk->sk_sndtimeo = 5 * HZ;
	w->rx->sk->sk_rcvtimeo = 5 * HZ;
	w->file = sock_alloc_file(w->tx, 0, NULL);
	if (IS_ERR(w->file)) {
		ret = PTR_ERR(w->file);
		w->file = NULL;
		w->tx = NULL; /* sock_alloc_file released it. */
		KUNIT_ASSERT_EQ(test, ret, 0);
	}
	for (i = 0; i < 2; i++) {
		w->req[i].ctx = w->ctx;
		w->req[i].file = w->file;
	}
}

/* A TCP-held reference to a unit goes when its data is acknowledged. */
static bool opaque_wire_unit_free(struct page *unit)
{
	int i;

	for (i = 0; i < 100 && page_count(unit) != 1; i++)
		msleep(20);
	return page_count(unit) == 1;
}

/*
 * Two back-to-back body-only sends over real TCP, spliced or copied. The queue
 * is free as soon as each frame is queued, so neither send waits; the peer reads
 * both ranges in order and TCP lets go of every page.
 */
static void opaque_send_loopback(struct kunit *test)
{
	static const struct {
		u32 offset, length, copy_max;
	} cases[] = {
		{ 0, SZ_64K, 0 },	/* two whole units, spliced */
		{ 0, SZ_64K, SZ_1M },	/* the same, copied by TCP */
		{ 4196, 40000, 0 },	/* from inside a unit across to the next */
		{ 4196, 40000, SZ_1M },
		{ 1, 5000, 0 },
		{ 1, 5000, 5000 },	/* the threshold itself copies */
		{ 1, 5000, 4999 },	/* one byte over it splices */
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req compact = {
		.store = store, .data = data, .handle = io_opaque_handle(0, 9),
	};
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 2 * SZ_64K, GFP_KERNEL);
	struct page *units[3];
	unsigned int c;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_params_save(test);
	opaque_test_cache(test, store, 4);
	opaque_test_copy(data, source);
	/* Publish the compacted copy, whose units are the compound pages TCP will see. */
	store->objects[0].data = data;
	store->objects[0].generation = 9;
	store->nr_objects++;
	__set_bit(0, store->object_used);
	refcount_inc(&data->refs); /* compaction worker */
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	KUNIT_ASSERT_EQ(test, opaque_test_pages(compact.replacement, units, 3), 3U);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	io_opaque_data_put(store, compact.data);
	io_opaque_compact_end(&compact);
	opaque_wire_init(test, w);
	for (c = 0; c < ARRAY_SIZE(cases); c++) {
		u64 copied = io_opaque_stat_read(IO_OPAQUE_STAT_BODY_COPIED), expect = 0;
		u64 framing = io_opaque_stat_read(IO_OPAQUE_STAT_FRAMING_COPIED);
		u32 length[2] = { cases[c].length, cases[c].length - 1 };
		u32 offset[2] = { cases[c].offset, cases[c].offset + 1 };
		u32 total = length[0] + length[1], done = 0;
		struct io_opaque_req send[2];
		struct msghdr msg = {};
		struct kvec vec;
		int i;

		io_opaque_send_copy_max = cases[c].copy_max;
		for (i = 0; i < 2; i++) {
			send[i] = (struct io_opaque_req) {
				.store = store, .req = &w->req[i], .op = U16_MAX,
				.handle = compact.handle, .offset = offset[i],
				.length = length[i],
			};
			INIT_LIST_HEAD(&send[i].wait);
			INIT_LIST_HEAD(&send[i].cancel);
			INIT_LIST_HEAD(&send[i].send);
			w->req[i].async_data = &send[i];
			/* Prep resolves a body-only send: issue has no frame to import. */
			io_opaque_send_resolve(&send[i]);
			KUNIT_ASSERT_TRUE(test, send[i].resolved);
			KUNIT_EXPECT_EQ(test, send[i].total_length, length[i]);
			if (cases[c].copy_max && length[i] <= cases[c].copy_max)
				expect += length[i];
			/* Not parked behind the first frame: it left its queue when queued. */
			KUNIT_ASSERT_EQ(test, io_opaque_send(&w->req[i], IO_URING_F_UNLOCKED),
					IOU_COMPLETE);
			KUNIT_EXPECT_EQ(test, w->req[i].cqe.res, (s32)length[i]);
			KUNIT_EXPECT_PTR_EQ(test, send[i].tx, NULL);
			KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
		}
		KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_BODY_COPIED) - copied,
				expect);
		KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_FRAMING_COPIED), framing);
		/* The peer sees the first range, then the second. */
		while (done < total) {
			int ret;

			vec.iov_base = got + done;
			vec.iov_len = total - done;
			ret = kernel_recvmsg(w->rx, &msg, &vec, 1, vec.iov_len, 0);
			KUNIT_ASSERT_GT(test, ret, 0);
			done += ret;
		}
		KUNIT_EXPECT_MEMEQ(test, got, source + offset[0], length[0]);
		KUNIT_EXPECT_MEMEQ(test, got + length[0], source + offset[1], length[1]);
		for (i = 0; i < 2; i++)
			io_opaque_data_put(store, send[i].data);
		/* TCP lets go of a spliced body once it is acknowledged. */
		KUNIT_EXPECT_TRUE(test, opaque_wire_unit_free(units[0]));
		KUNIT_EXPECT_TRUE(test, opaque_wire_unit_free(units[1]));
	}
	/* Handle and units go with the object. */
	mutex_lock(&store->tables);
	data = io_opaque_detach(store, &store->objects[0]);
	mutex_unlock(&store->tables);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/*
 * Attaching a collector takes the socket and then tables, the order in which a
 * send admits with the socket owned. This runs it on the wire fixture's socket,
 * whose lockdep class a send shares: an attach that succeeds, one that is
 * refused after both locks are taken, and the close that undoes the first.
 * None may leave either lock held, and under lockdep none may find them in
 * the other order.
 */
static void opaque_attach_lock_order(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_req *op[2];
	struct io_opaque_stream *stream;
	struct io_rings *rings;
	struct sock *sk;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, w);
	/* One stream slot, and a CQ ring of four 32-byte entries to take its token. */
	store->config.max_streams = 1;
	store->streams = kunit_kcalloc(test, 1, sizeof(*store->streams), GFP_KERNEL);
	store->stream_used = kunit_kzalloc(test, sizeof(unsigned long), GFP_KERNEL);
	rings = kunit_kzalloc(test, struct_size(rings, cqes, 8), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, store->streams);
	KUNIT_ASSERT_NOT_NULL(test, store->stream_used);
	KUNIT_ASSERT_NOT_NULL(test, rings);
	opaque_wire_init(test, w);
	w->ctx->rings = rings;
	RCU_INIT_POINTER(w->ctx->rings_rcu, rings);
	w->ctx->cq_entries = 4;
	w->ctx->flags |= IORING_SETUP_CQE32;
	spin_lock_init(&w->ctx->completion_lock);
	sk = w->tx->sk;
	for (i = 0; i < ARRAY_SIZE(op); i++) {
		op[i] = kunit_kzalloc(test, sizeof(*op[i]), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, op[i]);
		op[i]->store = store;
		op[i]->req = &w->req[i];
		op[i]->op = U16_MAX - 1;
		INIT_LIST_HEAD(&op[i]->wait);
		INIT_LIST_HEAD(&op[i]->cancel);
		w->req[i].cqe.user_data = 0xa0 + i;
	}

	mutex_lock(&w->ctx->uring_lock);
	KUNIT_ASSERT_EQ(test, io_opaque_attach(op[0]), 0);
	mutex_unlock(&w->ctx->uring_lock);
	lockdep_assert_none_held_once();
	stream = op[0]->stream;
	KUNIT_ASSERT_NOT_NULL(test, stream);
	KUNIT_EXPECT_PTR_EQ(test, sk->sk_rx_owner, &stream->owner);
	KUNIT_EXPECT_PTR_EQ(test, store->streams[0].stream, stream);
	KUNIT_EXPECT_EQ(test, store->nr_streams, 1U);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&store->tables));
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
	/* The token went to the request in a 32-byte CQE, with MORE. */
	KUNIT_EXPECT_EQ(test, rings->cqes[0].user_data, 0xa0ULL);
	KUNIT_EXPECT_TRUE(test, rings->cqes[0].flags & IORING_CQE_F_MORE);
	KUNIT_EXPECT_EQ(test, rings->cqes[1].user_data, stream->token);

	/* A second collector is refused with both locks taken, and gives both back. */
	mutex_lock(&w->ctx->uring_lock);
	KUNIT_EXPECT_EQ(test, io_opaque_attach(op[1]), -EBUSY);
	mutex_unlock(&w->ctx->uring_lock);
	lockdep_assert_none_held_once();
	KUNIT_EXPECT_PTR_EQ(test, op[1]->stream, NULL);
	KUNIT_EXPECT_PTR_EQ(test, sk->sk_rx_owner, &stream->owner);
	KUNIT_EXPECT_EQ(test, store->nr_streams, 1U);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&store->tables));
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));

	/* Closing the stream lets go of the socket; then the slot's and the collector's refs. */
	io_opaque_stream_close(stream);
	lockdep_assert_none_held_once();
	KUNIT_EXPECT_TRUE(test, stream->closed);
	KUNIT_EXPECT_PTR_EQ(test, sk->sk_rx_owner, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&stream->refs), 2);
	store->streams[0].stream = NULL;
	store->nr_streams--;
	io_opaque_stream_put(stream);
	io_opaque_stream_put(stream);
}

/*
 * Vectored OBJ_SEND: one SQE names several object ranges, and the body is
 * their concatenation. The request owns a block with the entries, a reference
 * per entry and the combined segments; a test frees it with the request.
 */
static void opaque_vec_free(void *arg)
{
	struct io_opaque_req *op = arg;

	kvfree(op->vec);
}

static struct io_opaque_req *opaque_vec_new(struct kunit *test, struct io_opaque_store *store,
					    const struct io_uring_opaque_send_entry *ent,
					    u32 nr, u16 flags)
{
	struct io_opaque_req *op = kunit_kzalloc(test, sizeof(*op), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, op);
	op->store = store;
	op->send_flags = IORING_OPAQUE_SEND_VECTOR | flags;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_vec_free, op), 0);
	KUNIT_ASSERT_EQ(test, io_opaque_vec_grow(op, nr, nr, 0), 0);
	memcpy(op->vec->ent, ent, array_size(nr, sizeof(*ent)));
	KUNIT_ASSERT_EQ(test, io_opaque_vec_commit(op, nr), 0);
	return op;
}

/* Make the block hold @segs segments, so that admission has no need to grow it. */
static void opaque_vec_room(struct kunit *test, struct io_opaque_req *op, u32 segs)
{
	KUNIT_ASSERT_EQ(test, io_opaque_vec_grow(op, op->vec->nr, segs, op->vec->nr), 0);
}

/*
 * Admission of a vector as io_opaque_tx_admit() does it, without the queue: the
 * references are taken under tables, and the block is grown and the cursor
 * built without it.
 */
static int opaque_vec_admit(struct io_opaque_req *op)
{
	int ret;

	mutex_lock(&op->store->tables);
	ret = io_opaque_send_acquire(op);
	mutex_unlock(&op->store->tables);
	return io_opaque_vec_finish(op, ret);
}

/*
 * The segments of a range as the first version of the vectored send found
 * them: its object walked from the first segment for every range, under tables.
 * Counted when @bv is NULL, else stored there.
 */
static u32 opaque_ref_segs(const struct io_opaque_data *data, u64 off, u32 len,
			   struct bio_vec *bv)
{
	const struct bio_vec *src = data->bvec;
	u32 i = 0, n = 0, skip;
	u64 pos = 0;

	while (i < data->nr && pos + src[i].bv_len <= off)
		pos += src[i++].bv_len;
	skip = off - pos;
	while (len && i < data->nr) {
		u32 take = min(src[i].bv_len - skip, len);

		if (bv) {
			bv[n] = src[i];
			bv[n].bv_offset += skip;
			bv[n].bv_len = take;
		}
		n++;
		len -= take;
		skip = 0;
		i++;
	}
	return n;
}

/*
 * The cursor of an admitted vector, built without tables from what admission
 * recorded, is the one that walking each range from the start of the version it
 * pinned gives, and the iterator reads exactly it.
 */
static void opaque_vec_expect_cursor(struct kunit *test, struct io_opaque_req *op)
{
	struct io_opaque_vec *vec = op->vec;
	struct bio_vec *want;
	u32 i, n = 0;

	for (i = 0; i < vec->nr; i++)
		n += opaque_ref_segs(vec->pin[i], vec->ent[i].off, vec->ent[i].len, NULL);
	want = kunit_kcalloc(test, n, sizeof(*want), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, want);
	for (i = 0, n = 0; i < vec->nr; i++)
		n += opaque_ref_segs(vec->pin[i], vec->ent[i].off, vec->ent[i].len, want + n);
	KUNIT_EXPECT_EQ(test, vec->nr_segs, n);
	KUNIT_EXPECT_MEMEQ(test, vec->bv, want, n * sizeof(*want));
	KUNIT_EXPECT_PTR_EQ(test, op->iter.bvec, vec->bv);
	KUNIT_EXPECT_EQ(test, op->iter.nr_segs, (unsigned long)n);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&op->iter), (size_t)op->length);
}

/* Publish @data in @slot under @generation. */
static void opaque_test_publish(struct io_opaque_store *store, u32 slot, u64 generation,
				struct io_opaque_data *data)
{
	store->objects[slot].data = data;
	store->objects[slot].generation = generation;
	store->nr_objects++;
	__set_bit(slot, store->object_used);
}

/* Rewrite every extent of a sparse object so that it differs from the others. */
static void opaque_test_fill(struct io_opaque_data *data, u8 base)
{
	struct io_opaque_extent *extent;

	list_for_each_entry(extent, &data->extents, list) {
		void *mapped = kmap_local_page(extent->backing->page);

		memset(mapped + extent->offset, base++, extent->length);
		kunmap_local(mapped);
	}
}

/* A store that holds a few objects of a few hundred extents each. */
static struct io_opaque_store *opaque_test_big_store(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);

	store->config.hard_limit = 1024 * PAGE_SIZE;
	store->config.max_extents = 1024;
	return store;
}

/*
 * An object of @nr extents, one page each, that differ in length and in where
 * they start in their page, so that no two segments look alike. Extent i holds
 * the byte i + 1.
 */
static struct io_opaque_data *opaque_varied_data(struct kunit *test,
						 struct io_opaque_store *store, u32 nr)
{
	struct io_opaque_data *data = io_opaque_data_alloc();
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, data);
	for (i = 0; i < nr; i++) {
		u32 len = 100 + i * 37 % 300, off = i * 53 % 500;
		struct page *page = alloc_page(GFP_KERNEL);
		struct io_opaque_extent *extent;
		void *mapped;

		KUNIT_ASSERT_NOT_NULL(test, page);
		mapped = kmap_local_page(page);
		memset(mapped + off, i + 1, len);
		kunmap_local(mapped);
		extent = io_opaque_extent_new(store, page, off, len, data->length, false);
		KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
		list_add_tail(&extent->list, &data->extents);
		data->length += len;
	}
	KUNIT_ASSERT_EQ(test, io_opaque_vector(data), 0);
	return data;
}

/* The byte at which segment @seg of @data starts. */
static u64 opaque_seg_pos(const struct io_opaque_data *data, u32 seg)
{
	u64 pos = 0;
	u32 i;

	for (i = 0; i < seg; i++)
		pos += data->bvec[i].bv_len;
	return pos;
}

/* The entries are checked as a whole, without looking at any handle. */
static void opaque_send_vec_check(struct kunit *test)
{
	static const struct io_uring_opaque_send_entry bad[] = {
		{ .handle = 1, .off = 0, .len = 0 },			/* empty range */
		{ .handle = 1, .off = 0, .len = 10, .__resv = 1 },	/* reserved field in use */
		{ .handle = 1, .off = U64_MAX, .len = 1 },		/* end past U64_MAX */
		{ .handle = 1, .off = U64_MAX - 9, .len = 10 },
		{ .handle = 1, .off = 0, .len = U32_MAX },		/* more than a send holds */
	};
	struct io_uring_opaque_send_entry *ent = kunit_kcalloc(test, IORING_OPAQUE_SEND_VEC_MAX + 1,
							       sizeof(*ent), GFP_KERNEL);
	u32 total = 0, longest = 0, i;

	KUNIT_ASSERT_NOT_NULL(test, ent);
	for (i = 0; i <= IORING_OPAQUE_SEND_VEC_MAX; i++)
		ent[i] = (struct io_uring_opaque_send_entry) { .handle = 1, .off = i, .len = 10 };
	/* One entry to the maximum; the body is the sum. */
	KUNIT_EXPECT_EQ(test, io_opaque_vec_check(ent, 1, &total, &longest), 0);
	KUNIT_EXPECT_EQ(test, total, 10U);
	KUNIT_EXPECT_EQ(test, longest, 10U);
	KUNIT_EXPECT_EQ(test, io_opaque_vec_check(ent, IORING_OPAQUE_SEND_VEC_MAX, &total,
						  &longest), 0);
	KUNIT_EXPECT_EQ(test, total, 10U * IORING_OPAQUE_SEND_VEC_MAX);
	/* None, or more than fit. */
	KUNIT_EXPECT_EQ(test, io_opaque_vec_check(ent, 0, &total, &longest), -EINVAL);
	KUNIT_EXPECT_EQ(test, io_opaque_vec_check(ent, IORING_OPAQUE_SEND_VEC_MAX + 1, &total,
						  &longest), -EINVAL);
	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		ent[1] = bad[i];
		KUNIT_EXPECT_EQ(test, io_opaque_vec_check(ent, 2, &total, &longest), -EINVAL);
		/* Only the entries named are looked at. */
		KUNIT_EXPECT_EQ(test, io_opaque_vec_check(ent, 1, &total, &longest), 0);
	}
	/* A range may end exactly at U64_MAX. */
	ent[1] = (struct io_uring_opaque_send_entry) {
		.handle = 1, .off = U64_MAX - 10, .len = 10,
	};
	KUNIT_EXPECT_EQ(test, io_opaque_vec_check(ent, 2, &total, &longest), 0);
	/* The body fits INT_MAX, which the whole frame must too. */
	ent[0].len = 1U << 30;
	ent[1] = (struct io_uring_opaque_send_entry) { .handle = 1, .off = 0, .len = 1U << 30 };
	KUNIT_EXPECT_EQ(test, io_opaque_vec_check(ent, 2, &total, &longest), -EINVAL);
	ent[1].len--;
	KUNIT_EXPECT_EQ(test, io_opaque_vec_check(ent, 2, &total, &longest), 0);
	KUNIT_EXPECT_EQ(test, total, (u32)INT_MAX);
	KUNIT_EXPECT_EQ(test, longest, 1U << 30);
	/* A handle, even a range, may repeat. */
	ent[0] = (struct io_uring_opaque_send_entry) { .handle = 1, .off = 0, .len = 100 };
	ent[1] = ent[0];
	ent[2] = (struct io_uring_opaque_send_entry) { .handle = 2, .off = 5, .len = 300 };
	ent[3] = ent[0];
	KUNIT_EXPECT_EQ(test, io_opaque_vec_check(ent, 4, &total, &longest), 0);
	KUNIT_EXPECT_EQ(test, total, 600U);
	KUNIT_EXPECT_EQ(test, longest, 300U);
}

/* Every entry sees the version published when the vector is admitted, not before or after. */
static void opaque_vec_versions(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store), *replacement;
	struct io_opaque_req compact = {
		.store = store, .data = data, .handle = io_opaque_handle(0, 9),
	};
	struct io_uring_opaque_send_entry ent[2] = {
		{ .handle = compact.handle, .off = 0, .len = 1000 },
		{ .handle = compact.handle, .off = 79000, .len = 1000 },
	};
	struct io_opaque_req *first = opaque_vec_new(test, store, ent, 2, 0);
	struct io_opaque_req *second = opaque_vec_new(test, store, ent, 2, 0);

	opaque_test_publish(store, 0, 9, data);
	KUNIT_EXPECT_EQ(test, opaque_vec_admit(first), 0);
	refcount_inc(&data->refs); /* compaction worker */
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	replacement = compact.replacement;
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	KUNIT_EXPECT_EQ(test, opaque_vec_admit(second), 0);
	KUNIT_EXPECT_PTR_EQ(test, first->vec->pin[0], compact.data);
	KUNIT_EXPECT_PTR_EQ(test, first->vec->pin[1], compact.data);
	KUNIT_EXPECT_PTR_EQ(test, second->vec->pin[0], replacement);
	KUNIT_EXPECT_PTR_EQ(test, second->vec->pin[1], replacement);
	/* The table's reference and one pin, shared by the two entries. */
	KUNIT_EXPECT_EQ(test, refcount_read(&replacement->refs), 2);
	/* The same bytes either way; the first still reads the version it pinned. */
	KUNIT_EXPECT_EQ(test, iov_iter_count(&first->iter), (size_t)2000);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&second->iter), (size_t)2000);
	opaque_vec_expect_cursor(test, first);
	opaque_vec_expect_cursor(test, second);
	io_opaque_vec_release(first);
	io_opaque_data_put(store, compact.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)ALIGN(80000, PAGE_SIZE));
	io_opaque_vec_release(second);
	mutex_lock(&store->tables);
	data = io_opaque_detach(store, &store->objects[0]);
	mutex_unlock(&store->tables);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static void opaque_test_vec_segs_restore(void *limit)
{
	io_opaque_send_vec_segs = (unsigned long)limit;
}

/*
 * Admission pins every entry under one hold of tables, or none: a vector that
 * cannot go consumes nothing, and a LAST vector detaches each handle once.
 */
static void opaque_send_vec_acquire(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *a = opaque_sparse_data(test, store);
	struct io_opaque_data *b = opaque_sparse_data(test, store);
	struct io_opaque_data *c = opaque_sparse_data(test, store);
	const u64 ha = io_opaque_handle(0, 9), hb = io_opaque_handle(1, 7);
	struct io_uring_opaque_send_entry ent[3] = {
		{ .handle = ha, .off = 0, .len = 1000 },
		{ .handle = hb, .off = 5, .len = 10 },
		{ .handle = ha, .off = 79000, .len = 1000 },
	};
	/* Extents are 2000 bytes: this spans two extents of A and three of B. */
	struct io_uring_opaque_send_entry span[2] = {
		{ .handle = ha, .off = 1500, .len = 1000 },
		{ .handle = hb, .off = 3900, .len = 2200 },
	};
	struct io_uring_opaque_send_entry rep[2] = {
		{ .handle = io_opaque_handle(2, 5), .off = 1500, .len = 1000 },
		{ .handle = io_opaque_handle(2, 5), .off = 3900, .len = 2200 },
	};
	struct io_uring_opaque_send_entry bad[4][3];
	static const int errs[4] = { -ERANGE, -ESTALE, -ESTALE, -EMSGSIZE };
	struct io_opaque_req *op, *other;
	unsigned long limit;
	struct bio_vec *bv;
	unsigned int last, i;

	opaque_test_publish(store, 0, 9, a);
	opaque_test_publish(store, 1, 7, b);

	/* Three pins for three entries, one segment each; the handles stay. */
	op = opaque_vec_new(test, store, ent, 3, 0);
	KUNIT_EXPECT_EQ(test, opaque_vec_admit(op), 0);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 3);
	KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 2);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 3U);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 3U);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&op->iter), (size_t)2010);
	opaque_vec_expect_cursor(test, op);
	KUNIT_EXPECT_FALSE(test, op->consumed);
	KUNIT_EXPECT_PTR_EQ(test, op->data, NULL);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, a);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[1].data, b);
	KUNIT_EXPECT_EQ(test, store->nr_objects, 2U);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 1);

	/* A range that crosses extents is as many segments, trimmed at both ends. */
	op = opaque_vec_new(test, store, span, 2, 0);
	KUNIT_EXPECT_EQ(test, opaque_vec_admit(op), 0);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 5U);
	KUNIT_EXPECT_GE(test, op->vec->cap_segs, 5U);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&op->iter), (size_t)3200);
	opaque_vec_expect_cursor(test, op);
	bv = op->vec->bv;
	KUNIT_EXPECT_PTR_EQ(test, bv[0].bv_page, a->bvec[0].bv_page);
	KUNIT_EXPECT_EQ(test, bv[0].bv_offset, 1500U);
	KUNIT_EXPECT_EQ(test, bv[0].bv_len, 500U);
	KUNIT_EXPECT_PTR_EQ(test, bv[1].bv_page, a->bvec[1].bv_page);
	KUNIT_EXPECT_EQ(test, bv[1].bv_offset, 0U);
	KUNIT_EXPECT_EQ(test, bv[1].bv_len, 500U);
	KUNIT_EXPECT_PTR_EQ(test, bv[2].bv_page, b->bvec[1].bv_page);
	KUNIT_EXPECT_EQ(test, bv[2].bv_offset, 1900U);
	KUNIT_EXPECT_EQ(test, bv[2].bv_len, 100U);
	KUNIT_EXPECT_PTR_EQ(test, bv[3].bv_page, b->bvec[2].bv_page);
	KUNIT_EXPECT_EQ(test, bv[3].bv_offset, 0U);
	KUNIT_EXPECT_EQ(test, bv[3].bv_len, 2000U);
	KUNIT_EXPECT_PTR_EQ(test, bv[4].bv_page, b->bvec[3].bv_page);
	KUNIT_EXPECT_EQ(test, bv[4].bv_offset, 0U);
	KUNIT_EXPECT_EQ(test, bv[4].bv_len, 100U);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[0], a);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[1], b);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 1);

	/* An entry out of range, stale, empty or past the segment limit fails the vector whole. */
	for (i = 0; i < ARRAY_SIZE(bad); i++)
		memcpy(bad[i], ent, sizeof(ent));
	bad[0][2].off = 79500;				/* ends at 80500 of 80000 */
	bad[1][1].handle = io_opaque_handle(1, 6);	/* an earlier generation of B's slot */
	bad[2][1].handle = io_opaque_handle(2, 0);	/* a slot that holds nothing */
	limit = io_opaque_send_vec_segs;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_vec_segs_restore,
							(void *)limit), 0);
	io_opaque_send_vec_segs = 2;			/* three segments are one too many */
	for (last = 0; last < 2; last++) {
		for (i = 0; i < ARRAY_SIZE(bad); i++) {
			other = opaque_vec_new(test, store, bad[i], 3,
					       last ? IORING_OPAQUE_SEND_LAST : 0);
			mutex_lock(&store->tables);
			KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(other), errs[i]);
			mutex_unlock(&store->tables);
			KUNIT_EXPECT_FALSE(test, other->consumed);
			KUNIT_EXPECT_EQ(test, other->vec->pinned, 0U);
			KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
			KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 1);
			KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, a);
			KUNIT_EXPECT_PTR_EQ(test, store->objects[1].data, b);
			KUNIT_EXPECT_EQ(test, store->nr_objects, 2U);
		}
	}
	/* The limit is the number of segments that fit. */
	io_opaque_send_vec_segs = 3;
	other = opaque_vec_new(test, store, ent, 3, 0);
	mutex_lock(&store->tables);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(other), 0);
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 3);
	io_opaque_vec_release(other);
	io_opaque_send_vec_segs = IO_OPAQUE_SEND_VEC_SEGS;
	/* A store that has stopped admits nothing. */
	other = opaque_vec_new(test, store, ent, 3, 0);
	store->dead = 1;
	mutex_lock(&store->tables);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(other), -ESHUTDOWN);
	mutex_unlock(&store->tables);
	store->dead = 0;
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);

	/*
	 * A LAST vector whose entries repeat a handle and span more segments than
	 * it has entries: the block grows outside tables and the vector is
	 * resolved again, and the repeat shares the reference of the first entry.
	 */
	opaque_test_publish(store, 2, 5, c);
	op = opaque_vec_new(test, store, rep, 2, IORING_OPAQUE_SEND_LAST);
	KUNIT_EXPECT_EQ(test, opaque_vec_admit(op), 0);
	KUNIT_EXPECT_TRUE(test, op->consumed);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 5U);
	opaque_vec_expect_cursor(test, op);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[0], c);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[1], c);
	KUNIT_EXPECT_EQ(test, refcount_read(&c->refs), 2);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[2].data, NULL);
	KUNIT_EXPECT_EQ(test, store->nr_objects, 2U);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)80 * PAGE_SIZE);

	/* LAST takes each handle once; A's second entry shares the reference. */
	op = opaque_vec_new(test, store, ent, 3, IORING_OPAQUE_SEND_LAST);
	KUNIT_EXPECT_EQ(test, opaque_vec_admit(op), 0);
	/* The cursor is built from the pins, with the handles already gone. */
	opaque_vec_expect_cursor(test, op);
	KUNIT_EXPECT_TRUE(test, op->consumed);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, NULL);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[1].data, NULL);
	KUNIT_EXPECT_EQ(test, store->nr_objects, 0U);
	KUNIT_EXPECT_FALSE(test, test_bit(0, store->object_used));
	KUNIT_EXPECT_FALSE(test, test_bit(1, store->object_used));
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[0], a);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[1], b);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[2], a);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
	KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)80 * PAGE_SIZE);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
	/* And a version published after admission or before it is the one pinned. */
	opaque_vec_versions(test);
}

/*
 * Admit a vector whose entry i names object obj[i], each a range of 500 bytes
 * inside one extent, so that every entry is one segment.
 */
static struct io_opaque_req *opaque_pin_vec(struct kunit *test, struct io_opaque_store *store,
					    const u64 *handle, const u8 *obj, u32 nr, u16 flags)
{
	struct io_uring_opaque_send_entry *ent = kunit_kcalloc(test, nr, sizeof(*ent), GFP_KERNEL);
	struct io_opaque_req *op;
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, ent);
	for (i = 0; i < nr; i++)
		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = handle[obj[i]], .off = (i % 40) * 2000 + 100, .len = 500,
		};
	op = opaque_vec_new(test, store, ent, nr, flags);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	return op;
}

/*
 * A vector that is not LAST takes one reference for each run of entries that
 * name the same handle and gives back the same: eight ranges of one object
 * cost one reference, a handle that returns after another pins again. LAST is
 * as it was, a reference for every entry.
 */
static void opaque_send_vec_pin_once(struct kunit *test)
{
	static const struct {
		u8 obj[8];
		u32 nr;
		int refs[3];	/* A, B and C once admitted */
	} cases[] = {
		{ { 0, 0, 0, 0, 0, 0, 0, 0 }, 8, { 2, 1, 1 } },	/* A x8: one pin */
		{ { 0, 0, 1, 0 }, 4, { 3, 2, 1 } },		/* a repeat after B pins again */
		{ { 0, 1, 1, 0, 0, 1 }, 6, { 3, 3, 1 } },	/* two runs of each */
		{ { 0, 1, 0, 1 }, 4, { 3, 3, 1 } },		/* every entry starts a run */
		{ { 2 }, 1, { 1, 1, 2 } },			/* one entry, one pin */
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *obj[3] = {
		opaque_sparse_data(test, store), opaque_sparse_data(test, store),
		opaque_sparse_data(test, store),
	};
	static const u64 generation[3] = { 9, 7, 5 };
	const u64 handle[3] = {
		io_opaque_handle(0, 9), io_opaque_handle(1, 7), io_opaque_handle(2, 5),
	};
	const s64 charge = 40 * PAGE_SIZE;
	u8 *many = kunit_kzalloc(test, IORING_OPAQUE_SEND_VEC_MAX, GFP_KERNEL);
	struct io_opaque_req *op;
	s64 bytes;
	u32 i, j;

	KUNIT_ASSERT_NOT_NULL(test, many);
	for (i = 0; i < ARRAY_SIZE(obj); i++)
		opaque_test_publish(store, i, generation[i], obj[i]);

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		op = opaque_pin_vec(test, store, handle, cases[i].obj, cases[i].nr, 0);
		/* Every entry is set and sees its own version; only some hold a reference. */
		KUNIT_EXPECT_EQ(test, op->vec->pinned, cases[i].nr);
		for (j = 0; j < cases[i].nr; j++)
			KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[j], obj[cases[i].obj[j]]);
		for (j = 0; j < ARRAY_SIZE(obj); j++)
			KUNIT_EXPECT_EQ(test, refcount_read(&obj[j]->refs), cases[i].refs[j]);
		KUNIT_EXPECT_EQ(test, op->vec->nr_segs, cases[i].nr);
		KUNIT_EXPECT_EQ(test, iov_iter_count(&op->iter), (size_t)500 * cases[i].nr);
		opaque_vec_expect_cursor(test, op);
		KUNIT_EXPECT_FALSE(test, op->consumed);
		io_opaque_vec_release(op);
		KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
		for (j = 0; j < ARRAY_SIZE(obj); j++)
			KUNIT_EXPECT_EQ(test, refcount_read(&obj[j]->refs), 1);
		/* Releasing twice is no more than releasing once. */
		io_opaque_vec_release(op);
		for (j = 0; j < ARRAY_SIZE(obj); j++)
			KUNIT_EXPECT_EQ(test, refcount_read(&obj[j]->refs), 1);
	}

	/* The most entries a vector holds, all of one object: still one reference. */
	op = opaque_pin_vec(test, store, handle, many, IORING_OPAQUE_SEND_VEC_MAX, 0);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, (u32)IORING_OPAQUE_SEND_VEC_MAX);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[0]->refs), 2);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, (u32)IORING_OPAQUE_SEND_VEC_MAX);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[0]->refs), 1);

	/*
	 * The handle is freed while the send holds the only reference of a run.
	 * The send's release puts it once, and does not look at the entries that
	 * shared it.
	 */
	op = opaque_pin_vec(test, store, handle, cases[0].obj, cases[0].nr, 0);
	mutex_lock(&store->tables);
	io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[0]->refs), 1);
	bytes = atomic64_read(&store->bytes);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, bytes - atomic64_read(&store->bytes), charge);

	/* LAST takes one per entry, the first being the table's own, and gives them all back. */
	op = opaque_pin_vec(test, store, handle, (const u8[]) { 2, 2, 2, 2 }, 4,
			    IORING_OPAQUE_SEND_LAST);
	KUNIT_EXPECT_TRUE(test, op->consumed);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 4U);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[2]->refs), 4);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[2].data, NULL);
	bytes = atomic64_read(&store->bytes);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, bytes - atomic64_read(&store->bytes), charge);

	/* The one left is released as it always was. */
	mutex_lock(&store->tables);
	io_opaque_data_put(store, io_opaque_detach(store, &store->objects[1]));
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/* The combined cursor reads the ranges in order; TCP sees adjacent ones as one fragment. */
static void opaque_send_vec_iter(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *a = opaque_sparse_data(test, store);
	struct io_opaque_data *b = opaque_sparse_data(test, store);
	const u64 ha = io_opaque_handle(0, 9), hb = io_opaque_handle(1, 7);
	struct io_uring_opaque_send_entry ent[] = {
		{ .handle = ha, .off = 1500, .len = 1000 },
		{ .handle = hb, .off = 3900, .len = 2200 },
		{ .handle = ha, .off = 0, .len = 1 },
		{ .handle = ha, .off = 79000, .len = 1000 },
		{ .handle = hb, .off = 1999, .len = 2 },
		{ .handle = ha, .off = 1500, .len = 1000 },
	};
	u8 *src[2], *want, *got;
	u32 total = 0, done = 0, i;
	struct io_opaque_req *op;
	struct opaque_splice f;
	struct sk_buff *skb;
	skb_frag_t *frag;
	u64 h;

	src[0] = kunit_kmalloc(test, 80000, GFP_KERNEL);
	src[1] = kunit_kmalloc(test, 80000, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src[0]);
	KUNIT_ASSERT_NOT_NULL(test, src[1]);
	for (i = 0; i < ARRAY_SIZE(ent); i++)
		total += ent[i].len;
	want = kunit_kmalloc(test, total, GFP_KERNEL);
	got = kunit_kmalloc(test, total, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, want);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_fill(b, 0x80);
	opaque_test_copy(a, src[0]);
	opaque_test_copy(b, src[1]);
	for (i = 0, done = 0; i < ARRAY_SIZE(ent); i++) {
		memcpy(want + done, src[ent[i].handle == hb] + ent[i].off, ent[i].len);
		done += ent[i].len;
	}
	opaque_test_publish(store, 0, 9, a);
	opaque_test_publish(store, 1, 7, b);
	op = opaque_vec_new(test, store, ent, ARRAY_SIZE(ent), 0);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&op->iter), (size_t)total);
	opaque_vec_expect_cursor(test, op);
	/* A partial send takes some of it; the rest resumes inside the vector. */
	KUNIT_EXPECT_EQ(test, copy_from_iter(got, 1234, &op->iter), (size_t)1234);
	KUNIT_EXPECT_EQ(test, copy_from_iter(got + 1234, total - 1234, &op->iter),
			(size_t)(total - 1234));
	KUNIT_EXPECT_EQ(test, iov_iter_count(&op->iter), (size_t)0);
	KUNIT_EXPECT_MEMEQ(test, got, want, total);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 1);

	/*
	 * On the compacted object, whose extents are the units TCP sees: two
	 * adjacent ranges of one unit are one fragment, the same range twice and
	 * a range across a unit boundary are two.
	 */
	opaque_splice_setup(test, &f);
	opaque_test_publish(f.store, 0, 9, f.op.replacement);
	h = io_opaque_handle(0, 9);
	{
		struct io_uring_opaque_send_entry adj[] = {
			{ .handle = h, .off = 0, .len = 1000 },
			{ .handle = h, .off = 1000, .len = 1000 },
		}, twice[] = {
			{ .handle = h, .off = 0, .len = 1000 },
			{ .handle = h, .off = 0, .len = 1000 },
		}, cross[] = {
			{ .handle = h, .off = OPAQUE_TEST_UNIT - 500, .len = 1000 },
		};
		struct {
			struct io_uring_opaque_send_entry *ent;
			u32 nr, frags, unit0;
		} cases[] = {
			{ adj, ARRAY_SIZE(adj), 1, 1 },
			{ twice, ARRAY_SIZE(twice), 2, 2 },
			{ cross, ARRAY_SIZE(cross), 2, 1 },
		};

		for (i = 0; i < ARRAY_SIZE(cases); i++) {
			u32 len = 0, j, at = 0;

			for (j = 0; j < cases[i].nr; j++)
				len += cases[i].ent[j].len;
			op = opaque_vec_new(test, f.store, cases[i].ent, cases[i].nr, 0);
			KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
			opaque_vec_expect_cursor(test, op);
			skb = f.skb;
			if (i) {
				skb = alloc_skb(0, GFP_KERNEL);
				KUNIT_ASSERT_NOT_NULL(test, skb);
				skb->ip_summed = CHECKSUM_PARTIAL;
			}
			KUNIT_EXPECT_EQ(test, skb_splice_from_iter(skb, &op->iter, len),
					(ssize_t)len);
			KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (int)cases[i].frags);
			frag = &skb_shinfo(skb)->frags[0];
			if (i == 0) {
				KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), f.units[0]);
				KUNIT_EXPECT_EQ(test, skb_frag_off(frag), 0U);
				KUNIT_EXPECT_EQ(test, skb_frag_size(frag), 2000U);
			}
			/* One reference per fragment, on top of the object's own. */
			KUNIT_EXPECT_EQ(test, page_count(f.units[0]), (int)(1 + cases[i].unit0));
			KUNIT_ASSERT_EQ(test, skb_copy_bits(skb, 0, got, len), 0);
			for (j = 0; j < cases[i].nr; j++) {
				KUNIT_EXPECT_MEMEQ(test, got + at, f.source + cases[i].ent[j].off,
						   cases[i].ent[j].len);
				at += cases[i].ent[j].len;
			}
			kfree_skb(skb);
			io_opaque_vec_release(op);
			KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
			KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 1);
		}
	}
	/* The compacted version is the table's reference, which the fixture drops. */
	opaque_splice_done(&f);
}

/*
 * One vector of three ranges of the compacted object over real TCP: queued by
 * one send, read in order by the peer, and spliced or copied whole by the
 * threshold on its largest range.
 */
static void opaque_send_vec_loopback(struct kunit *test)
{
	static const struct {
		u32 copy_max;
		bool copied;
	} cases[] = {
		{ 0, false },		/* spliced */
		{ 40000, true },	/* the largest range itself copies */
		{ 39999, false },	/* one byte more splices, with the total far above it */
		{ SZ_1M, true },
	};
	const struct io_uring_opaque_send_entry ent[] = {
		{ .handle = io_opaque_handle(0, 9), .off = 4196, .len = 40000 },
		{ .handle = io_opaque_handle(0, 9), .off = 0, .len = 5000 },
		{ .handle = io_opaque_handle(0, 9), .off = 70000, .len = 2000 },
	};
	const u32 total = 47000;
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req compact = {
		.store = store, .data = data, .handle = io_opaque_handle(0, 9),
	};
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, total, GFP_KERNEL);
	u8 *want = kunit_kmalloc(test, total, GFP_KERNEL);
	struct page *units[3];
	struct io_opaque_req *op;
	unsigned int c, i, j;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	KUNIT_ASSERT_NOT_NULL(test, want);
	opaque_test_params_save(test);
	opaque_test_cache(test, store, 4);
	opaque_test_copy(data, source);
	for (i = 0, j = 0; i < ARRAY_SIZE(ent); i++) {
		memcpy(want + j, source + ent[i].off, ent[i].len);
		j += ent[i].len;
	}
	opaque_test_publish(store, 0, 9, data);
	refcount_inc(&data->refs); /* compaction worker */
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	KUNIT_ASSERT_EQ(test, opaque_test_pages(compact.replacement, units, 3), 3U);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	io_opaque_data_put(store, compact.data);
	io_opaque_compact_end(&compact);
	opaque_wire_init(test, w);
	for (c = 0; c <= ARRAY_SIZE(cases); c++) {
		/* The last round is a LAST vector that repeats the handle. */
		bool last = c == ARRAY_SIZE(cases);
		u64 copied = io_opaque_stat_read(IO_OPAQUE_STAT_BODY_COPIED), expect = 0;
		u32 sent = last ? ent[0].len + ent[1].len : total, done = 0;
		struct msghdr msg = {};
		unsigned int consumed;
		struct kvec vec;

		io_opaque_send_copy_max = last ? 0 : cases[c].copy_max;
		op = opaque_vec_new(test, store, ent, last ? 2 : ARRAY_SIZE(ent),
				    last ? IORING_OPAQUE_SEND_LAST : 0);
		op->req = &w->req[0];
		op->op = U16_MAX;
		INIT_LIST_HEAD(&op->wait);
		INIT_LIST_HEAD(&op->cancel);
		INIT_LIST_HEAD(&op->send);
		w->req[0].async_data = op;
		io_opaque_send_resolve(op);
		KUNIT_ASSERT_TRUE(test, op->resolved);
		KUNIT_ASSERT_EQ(test, io_opaque_send(&w->req[0], IO_URING_F_UNLOCKED),
				IOU_COMPLETE);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, (s32)sent);
		consumed = w->req[0].cqe.flags & IORING_CQE_F_OPAQUE_CONSUMED;
		KUNIT_EXPECT_EQ(test, !!consumed, (int)last);
		KUNIT_EXPECT_EQ(test, op->consumed, last);
		KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
		KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
		/* The whole vector is spliced or copied; it is counted as copied bytes. */
		if (!last && cases[c].copied)
			expect = total;
		KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_BODY_COPIED) - copied,
				expect);
		while (done < sent) {
			int ret;

			vec.iov_base = got + done;
			vec.iov_len = sent - done;
			ret = kernel_recvmsg(w->rx, &msg, &vec, 1, vec.iov_len, 0);
			KUNIT_ASSERT_GT(test, ret, 0);
			done += ret;
		}
		KUNIT_EXPECT_MEMEQ(test, got, want, sent);
		io_opaque_vec_release(op);
		if (last)
			break;
		/* TCP lets go of a spliced body once it is acknowledged. */
		for (i = 0; i < ARRAY_SIZE(units); i++)
			KUNIT_EXPECT_TRUE(test, opaque_wire_unit_free(units[i]));
		KUNIT_EXPECT_PTR_NE(test, store->objects[0].data, NULL);
	}
	/* LAST took the handle and, with the references, the object. */
	KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, NULL);
	KUNIT_EXPECT_EQ(test, store->nr_objects, 0U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/* A queue that fails after the check takes every pin back; a LAST vector stays consumed. */
static void opaque_send_vec_admit_fail(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *a = opaque_sparse_data(test, store);
	struct io_opaque_data *b = opaque_sparse_data(test, store);
	const struct io_uring_opaque_send_entry ent[] = {
		{ .handle = io_opaque_handle(0, 9), .off = 0, .len = 1000 },
		{ .handle = io_opaque_handle(1, 7), .off = 0, .len = 1000 },
		{ .handle = io_opaque_handle(0, 9), .off = 1000, .len = 1000 },
	};
	struct sock *sk = kunit_kzalloc(test, sizeof(*sk), GFP_KERNEL);
	struct io_opaque_req head, *v[2];
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, sk);
	opaque_test_publish(store, 0, 9, a);
	opaque_test_publish(store, 1, 7, b);
	v[0] = opaque_vec_new(test, store, ent, ARRAY_SIZE(ent), 0);
	v[1] = opaque_vec_new(test, store, ent, ARRAY_SIZE(ent), IORING_OPAQUE_SEND_LAST);
	opaque_test_tx_op(&head, store);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&head, sk), 1);
	for (i = 0; i < 2; i++) {
		INIT_LIST_HEAD(&v[i]->wait);
		INIT_LIST_HEAD(&v[i]->cancel);
		INIT_LIST_HEAD(&v[i]->send);
		opaque_race_head = &head;
		head.tx->failed = false;
		kunit_activate_static_stub(test, io_opaque_send_acquire, opaque_test_acquire_fail);
		KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(v[i], sk), -ECANCELED);
		KUNIT_EXPECT_EQ(test, v[i]->consumed, (bool)(i == 1));
		KUNIT_EXPECT_EQ(test, v[i]->vec->pinned, 0U);
		KUNIT_EXPECT_PTR_EQ(test, v[i]->tx, NULL);
		if (!i) {
			KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
			KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 1);
			KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, a);
		}
	}
	/* Both handles went with the LAST vector, and the pins with the failed admission. */
	KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, NULL);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[1].data, NULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, head.tx->admitting, 0U);
	opaque_test_tx_done(&head, false);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&head), NULL);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
}

/*
 * Admission finds where each range starts by walking the segments of its
 * object. An entry that follows another in the same version, not before the
 * segment that one started in, walks on from there; one before it, or in
 * another object, starts again at the first segment.
 */
struct opaque_step {
	u8 obj;		/* the object the range is in */
	u8 seg;		/* the segment it starts in */
	u16 delta;	/* bytes into that segment */
	u32 len;
	bool resumes;	/* walks on from the previous entry's segment */
};

/* Generations of the three objects of opaque_send_vec_resume, in slots 0 to 2. */
static const u64 opaque_resume_gen[3] = { 9, 7, 5 };

struct opaque_resume {
	struct io_opaque_store *store;
	struct io_opaque_data *obj[3];
	u8 *src[3], *want, *got;
};

/* One vector of the steps: where its ranges start, how far it walked, and what it reads. */
static void opaque_resume_run(struct kunit *test, struct opaque_resume *r,
			      const struct opaque_step *step, u32 nr)
{
	struct io_uring_opaque_send_entry *ent = kunit_kcalloc(test, nr, sizeof(*ent), GFP_KERNEL);
	u64 walked, expect = 0;
	struct io_opaque_req *op;
	u32 i, total = 0;

	KUNIT_ASSERT_NOT_NULL(test, ent);
	for (i = 0; i < nr; i++) {
		u8 o = step[i].obj;
		u64 off = opaque_seg_pos(r->obj[o], step[i].seg) + step[i].delta;

		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = io_opaque_handle(o, opaque_resume_gen[o]),
			.off = off, .len = step[i].len,
		};
		memcpy(r->want + total, r->src[o] + off, step[i].len);
		total += step[i].len;
		/* A resumed walk is the distance from the last start, else from the first. */
		expect += step[i].resumes ? step[i].seg - step[i - 1].seg : step[i].seg;
	}
	op = opaque_vec_new(test, r->store, ent, nr, 0);
	/* Room enough that admission does not grow the block and walk twice. */
	opaque_vec_room(test, op, 4 * nr + 64);
	walked = atomic_long_read(&io_opaque_vec_stepped);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	walked = atomic_long_read(&io_opaque_vec_stepped) - walked;
	KUNIT_EXPECT_EQ(test, walked, expect);
	for (i = 0; i < nr; i++) {
		KUNIT_EXPECT_EQ(test, op->vec->start[i].seg, (u32)step[i].seg);
		KUNIT_EXPECT_EQ(test, op->vec->start[i].skip, (u32)step[i].delta);
	}
	opaque_vec_expect_cursor(test, op);
	KUNIT_EXPECT_EQ(test, copy_from_iter(r->got, total, &op->iter), (size_t)total);
	KUNIT_EXPECT_MEMEQ(test, r->got, r->want, total);
	io_opaque_vec_release(op);
	for (i = 0; i < ARRAY_SIZE(r->obj); i++)
		KUNIT_EXPECT_EQ(test, refcount_read(&r->obj[i]->refs), 1);
}

static void opaque_send_vec_resume(struct kunit *test)
{
	static const struct opaque_step ascending[] = {
		{ 0, 20, 0, 50, false }, { 0, 21, 3, 10, true },
		{ 0, 50, 0, 100, true }, { 0, 99, 7, 1, true },
	};
	/* Not before the segment the last one started in: a lower offset in it still resumes. */
	static const struct opaque_step same_segment[] = {
		{ 0, 30, 40, 10, false }, { 0, 30, 5, 10, true },
		{ 0, 30, 0, 10, true }, { 0, 30, 0, 10, true },
	};
	static const struct opaque_step descending[] = {
		{ 0, 80, 0, 10, false }, { 0, 40, 0, 10, false }, { 0, 10, 0, 10, false },
	};
	static const struct opaque_step mixed[] = {
		{ 0, 60, 0, 10, false }, { 0, 20, 0, 10, false }, { 0, 25, 0, 10, true },
		{ 0, 10, 0, 10, false }, { 0, 98, 0, 10, true },
	};
	/* Another object starts again, and so does coming back to the first. */
	static const struct opaque_step objects[] = {
		{ 0, 30, 0, 10, false }, { 0, 35, 0, 10, true }, { 1, 20, 0, 10, false },
		{ 1, 25, 0, 10, true }, { 0, 40, 0, 10, false },
	};
	static const struct opaque_step alternate[] = {
		{ 0, 30, 0, 10, false }, { 1, 20, 0, 10, false },
		{ 0, 35, 0, 10, false }, { 1, 25, 0, 10, false },
	};
	/* A range over several segments does not move the resume point past its own start. */
	static const struct opaque_step spans[] = {
		{ 0, 10, 0, 3000, false }, { 0, 12, 0, 50, true },
		{ 0, 30, 0, 2000, true }, { 0, 31, 0, 10, true },
	};
	static const struct {
		const struct opaque_step *step;
		u32 nr;
	} cases[] = {
		{ ascending, ARRAY_SIZE(ascending) },
		{ same_segment, ARRAY_SIZE(same_segment) },
		{ descending, ARRAY_SIZE(descending) },
		{ mixed, ARRAY_SIZE(mixed) },
		{ objects, ARRAY_SIZE(objects) },
		{ alternate, ARRAY_SIZE(alternate) },
		{ spans, ARRAY_SIZE(spans) },
	};
	static const u32 extents[3] = { 100, 60, 200 };
	struct opaque_resume *r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	struct opaque_step *run;
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, r);
	r->store = opaque_test_big_store(test);
	r->want = kunit_kmalloc(test, SZ_16K, GFP_KERNEL);
	r->got = kunit_kmalloc(test, SZ_16K, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r->want);
	KUNIT_ASSERT_NOT_NULL(test, r->got);
	for (i = 0; i < ARRAY_SIZE(r->obj); i++) {
		r->obj[i] = opaque_varied_data(test, r->store, extents[i]);
		r->src[i] = kunit_kmalloc(test, r->obj[i]->length, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, r->src[i]);
		opaque_test_copy(r->obj[i], r->src[i]);
		opaque_test_publish(r->store, i, opaque_resume_gen[i], r->obj[i]);
	}
	for (i = 0; i < ARRAY_SIZE(cases); i++)
		opaque_resume_run(test, r, cases[i].step, cases[i].nr);

	/*
	 * Fifty one-byte ranges at the end of an object of 200 extents: in
	 * ascending order the object is walked once, and in descending order
	 * once for each range.
	 */
	run = kunit_kcalloc(test, 50, sizeof(*run), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, run);
	for (i = 0; i < 50; i++)
		run[i] = (struct opaque_step) { 2, 150 + i, 0, 1, i != 0 };
	opaque_resume_run(test, r, run, 50);
	for (i = 0; i < 50; i++)
		run[i] = (struct opaque_step) { 2, 199 - i, 0, 1, false };
	opaque_resume_run(test, r, run, 50);

	for (i = 0; i < ARRAY_SIZE(r->obj); i++) {
		mutex_lock(&r->store->tables);
		io_opaque_data_put(r->store, io_opaque_detach(r->store, &r->store->objects[i]));
		mutex_unlock(&r->store->tables);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&r->store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&r->store->extents), 0);
}

/*
 * Two ranges of the sparse object, whose extents are 2000 bytes: the first
 * spans two segments and the second three. A block made for two entries holds
 * two segments, so for these five it has to grow.
 */
static void opaque_grow_entries(struct io_uring_opaque_send_entry *ent, u64 handle)
{
	ent[0] = (struct io_uring_opaque_send_entry) { .handle = handle, .off = 1500, .len = 1000 };
	ent[1] = (struct io_uring_opaque_send_entry) { .handle = handle, .off = 3900, .len = 2200 };
}

/*
 * What the window in which admission has left tables to grow a block saw, and
 * does there; and whether tables was held where the block grew and where the
 * cursor was built.
 */
static struct {
	void (*between)(struct kunit *test, struct io_opaque_req *op);
	struct io_opaque_req *compact;
	int fail;
	unsigned int calls, builds;
	u32 pinned, segs, cap;
	bool consumed, grow_locked, build_locked;
	/* Hooks not yet reached; each takes itself out when it is. */
	bool grow_hook, build_hook;
} opaque_window;

static int opaque_test_grow_window(struct io_opaque_req *op, u32 nr, u32 segs, u32 keep)
{
	struct kunit *test = kunit_get_current_test();

	kunit_deactivate_static_stub(test, io_opaque_vec_grow);
	opaque_window.grow_hook = false;
	opaque_window.calls++;
	opaque_window.grow_locked = mutex_is_locked(&op->store->tables);
	opaque_window.pinned = op->vec->pinned;
	opaque_window.consumed = op->consumed;
	opaque_window.segs = segs;
	opaque_window.cap = op->vec->cap_segs;
	if (opaque_window.between)
		opaque_window.between(test, op);
	if (opaque_window.fail)
		return opaque_window.fail;
	return io_opaque_vec_grow(op, nr, segs, keep);
}

static void opaque_test_build_window(struct io_opaque_req *op)
{
	kunit_deactivate_static_stub(kunit_get_current_test(), io_opaque_vec_build);
	opaque_window.build_hook = false;
	opaque_window.builds++;
	opaque_window.build_locked = mutex_is_locked(&op->store->tables);
	io_opaque_vec_build(op);
}

/* Watch admission from here on: the grow and the build, with what is done between. */
static void opaque_window_open(struct kunit *test,
			       void (*between)(struct kunit *test, struct io_opaque_req *op))
{
	memset(&opaque_window, 0, sizeof(opaque_window));
	opaque_window.between = between;
	opaque_window.grow_hook = true;
	opaque_window.build_hook = true;
	kunit_activate_static_stub(test, io_opaque_vec_grow, opaque_test_grow_window);
	kunit_activate_static_stub(test, io_opaque_vec_build, opaque_test_build_window);
}

/* A hook that was not reached must not catch the setup of the next request. */
static void opaque_window_close(struct kunit *test)
{
	if (opaque_window.grow_hook)
		kunit_deactivate_static_stub(test, io_opaque_vec_grow);
	if (opaque_window.build_hook)
		kunit_deactivate_static_stub(test, io_opaque_vec_build);
	opaque_window.grow_hook = false;
	opaque_window.build_hook = false;
}

/* The object is compacted: its slot holds another version when admission looks again. */
static void opaque_window_compact(struct kunit *test, struct io_opaque_req *op)
{
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(opaque_window.compact), 0);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(opaque_window.compact), 0);
}

/* The handle is freed: nothing is left for admission to find. */
static void opaque_window_free(struct kunit *test, struct io_opaque_req *op)
{
	mutex_lock(&op->store->tables);
	io_opaque_data_put(op->store, io_opaque_detach(op->store, &op->store->objects[0]));
	mutex_unlock(&op->store->tables);
}

/* A vector of the two ranges, as a request that is admitted to a queue. */
static struct io_opaque_req *opaque_grow_queued(struct kunit *test, struct io_opaque_store *store,
						u64 handle, u16 flags)
{
	struct io_uring_opaque_send_entry ent[2];
	struct io_opaque_req *op;

	opaque_grow_entries(ent, handle);
	op = opaque_vec_new(test, store, ent, ARRAY_SIZE(ent), flags);
	INIT_LIST_HEAD(&op->wait);
	INIT_LIST_HEAD(&op->cancel);
	INIT_LIST_HEAD(&op->send);
	io_opaque_send_resolve(op);
	return op;
}

/*
 * A block with too few segments for what the ranges span is grown without
 * tables, and the vector is resolved again in a new hold: nothing is pinned or
 * removed before that, and what the second hold sees is what counts.
 */
static void opaque_send_vec_grow_retry(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *a = opaque_sparse_data(test, store), *replacement;
	struct io_uring_opaque_send_entry ent[2], stale[2];
	struct io_opaque_req compact = { .store = store };
	struct sock *sk = kunit_kzalloc(test, sizeof(*sk), GFP_KERNEL);
	struct io_opaque_req *op;
	unsigned int last;
	long before, pass;
	u32 n;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, sk);

	/*
	 * The hold that finds the block too small pins and removes nothing and
	 * leaves the block as it is. The retry walks the ranges again and ends
	 * as a single hold would have.
	 */
	opaque_test_publish(store, 0, 9, a);
	opaque_grow_entries(ent, io_opaque_handle(0, 9));
	for (last = 0; last < 2; last++) {
		op = opaque_vec_new(test, store, ent, ARRAY_SIZE(ent),
				    last ? IORING_OPAQUE_SEND_LAST : 0);
		KUNIT_ASSERT_EQ(test, op->vec->cap_segs, 2U);
		before = atomic_long_read(&io_opaque_vec_stepped);
		mutex_lock(&store->tables);
		ret = io_opaque_send_acquire(op);
		mutex_unlock(&store->tables);
		pass = atomic_long_read(&io_opaque_vec_stepped) - before;
		KUNIT_EXPECT_EQ(test, ret, 5);
		KUNIT_EXPECT_GT(test, pass, 0L);
		KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
		KUNIT_EXPECT_FALSE(test, op->consumed);
		KUNIT_EXPECT_EQ(test, op->vec->cap_segs, 2U);
		KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
		KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, a);
		KUNIT_EXPECT_EQ(test, store->nr_objects, 1U);
		KUNIT_EXPECT_TRUE(test, test_bit(0, store->object_used));
		KUNIT_EXPECT_EQ(test, io_opaque_vec_finish(op, ret), 0);
		/* The second hold walked as the first did: pass one was done again. */
		KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_vec_stepped) - before, 2 * pass);
		KUNIT_EXPECT_GE(test, op->vec->cap_segs, 5U);
		KUNIT_EXPECT_MEMEQ(test, op->vec->ent, ent, sizeof(ent));
		KUNIT_EXPECT_EQ(test, op->vec->pinned, 2U);
		KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 5U);
		KUNIT_EXPECT_EQ(test, op->consumed, (bool)last);
		/* LAST: the detached reference and the repeat's. Else: the table's and one pin. */
		KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
		KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, last ? NULL : a);
		opaque_vec_expect_cursor(test, op);
		io_opaque_vec_release(op);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);

	/*
	 * A vector that cannot go fails instead of growing a block it would not
	 * use: the first range spans three segments, and the second handle is
	 * stale.
	 */
	a = opaque_sparse_data(test, store);
	opaque_test_publish(store, 0, 10, a);
	stale[0] = (struct io_uring_opaque_send_entry) {
		.handle = io_opaque_handle(0, 10), .off = 1500, .len = 2600,
	};
	stale[1] = (struct io_uring_opaque_send_entry) {
		.handle = io_opaque_handle(1, 7), .off = 0, .len = 10,
	};
	op = opaque_vec_new(test, store, stale, ARRAY_SIZE(stale), 0);
	mutex_lock(&store->tables);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(op), -ESTALE);
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, io_opaque_vec_finish(op, -ESTALE), -ESTALE);
	KUNIT_EXPECT_EQ(test, op->vec->cap_segs, 2U);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
	mutex_lock(&store->tables);
	io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
	mutex_unlock(&store->tables);

	/* Through admission, a block that fits is not grown, and the cursor needs no tables. */
	a = opaque_sparse_data(test, store);
	opaque_test_publish(store, 0, 11, a);
	op = opaque_grow_queued(test, store, io_opaque_handle(0, 11), 0);
	opaque_vec_room(test, op, 5);
	opaque_window_open(test, NULL);
	KUNIT_ASSERT_EQ(test, io_opaque_tx_admit(op, sk), 1);
	opaque_window_close(test);
	lockdep_assert_none_held_once();
	KUNIT_EXPECT_EQ(test, opaque_window.calls, 0U);
	KUNIT_EXPECT_EQ(test, opaque_window.builds, 1U);
	KUNIT_EXPECT_FALSE(test, opaque_window.build_locked);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 2U);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 5U);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
	opaque_vec_expect_cursor(test, op);
	opaque_test_tx_done(op, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(op), NULL);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);

	/*
	 * A block that cannot grow ends the admission with nothing taken, LAST
	 * included, and it grows where no lock is held.
	 */
	for (last = 0; last < 2; last++) {
		op = opaque_grow_queued(test, store, io_opaque_handle(0, 11),
					last ? IORING_OPAQUE_SEND_LAST : 0);
		opaque_window_open(test, NULL);
		opaque_window.fail = -ENOMEM;
		KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(op, sk), -ENOMEM);
		opaque_window_close(test);
		lockdep_assert_none_held_once();
		KUNIT_EXPECT_EQ(test, opaque_window.calls, 1U);
		KUNIT_EXPECT_FALSE(test, opaque_window.grow_locked);
		KUNIT_EXPECT_EQ(test, opaque_window.builds, 0U);
		KUNIT_EXPECT_EQ(test, opaque_window.segs, 5U);
		KUNIT_EXPECT_EQ(test, opaque_window.cap, 2U);
		KUNIT_EXPECT_EQ(test, opaque_window.pinned, 0U);
		KUNIT_EXPECT_FALSE(test, opaque_window.consumed);
		KUNIT_EXPECT_FALSE(test, op->consumed);
		KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
		KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
		KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
		KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, a);
		KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	}

	/* The handle is gone when the second hold looks: it fails and consumes nothing. */
	op = opaque_grow_queued(test, store, io_opaque_handle(0, 11), IORING_OPAQUE_SEND_LAST);
	opaque_window_open(test, opaque_window_free);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(op, sk), -ESTALE);
	opaque_window_close(test);
	KUNIT_EXPECT_EQ(test, opaque_window.calls, 1U);
	KUNIT_EXPECT_FALSE(test, opaque_window.grow_locked);
	KUNIT_EXPECT_EQ(test, opaque_window.builds, 0U);
	KUNIT_EXPECT_FALSE(test, op->consumed);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
	KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);

	/*
	 * Nothing changes in the window, and the vector is LAST: the retry takes
	 * the handle with the references, and the cursor is built from them
	 * without tables, the handle being gone by then.
	 */
	a = opaque_sparse_data(test, store);
	opaque_test_publish(store, 0, 13, a);
	op = opaque_grow_queued(test, store, io_opaque_handle(0, 13), IORING_OPAQUE_SEND_LAST);
	opaque_window_open(test, NULL);
	KUNIT_ASSERT_EQ(test, io_opaque_tx_admit(op, sk), 1);
	opaque_window_close(test);
	lockdep_assert_none_held_once();
	KUNIT_EXPECT_EQ(test, opaque_window.calls, 1U);
	KUNIT_EXPECT_FALSE(test, opaque_window.grow_locked);
	KUNIT_EXPECT_EQ(test, opaque_window.pinned, 0U);
	KUNIT_EXPECT_FALSE(test, opaque_window.consumed);
	KUNIT_EXPECT_EQ(test, opaque_window.builds, 1U);
	KUNIT_EXPECT_FALSE(test, opaque_window.build_locked);
	KUNIT_EXPECT_TRUE(test, op->consumed);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 5U);
	opaque_vec_expect_cursor(test, op);
	opaque_test_tx_done(op, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(op), NULL);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);

	/*
	 * The object is compacted in the window: the second hold pins the
	 * replacement, and the cursor is over its segments, not the five that
	 * the first hold counted.
	 */
	a = opaque_sparse_data(test, store);
	opaque_test_publish(store, 0, 14, a);
	compact.data = a;
	compact.handle = io_opaque_handle(0, 14);
	op = opaque_grow_queued(test, store, compact.handle, 0);
	refcount_inc(&a->refs); /* compaction worker */
	opaque_window_open(test, opaque_window_compact);
	opaque_window.compact = &compact;
	KUNIT_ASSERT_EQ(test, io_opaque_tx_admit(op, sk), 1);
	opaque_window_close(test);
	lockdep_assert_none_held_once();
	replacement = store->objects[0].data;
	KUNIT_ASSERT_PTR_NE(test, replacement, a);
	KUNIT_EXPECT_EQ(test, opaque_window.calls, 1U);
	KUNIT_EXPECT_FALSE(test, opaque_window.grow_locked);
	KUNIT_EXPECT_EQ(test, opaque_window.builds, 1U);
	KUNIT_EXPECT_FALSE(test, opaque_window.build_locked);
	KUNIT_EXPECT_EQ(test, opaque_window.segs, 5U);
	KUNIT_EXPECT_EQ(test, opaque_window.cap, 2U);
	KUNIT_EXPECT_EQ(test, opaque_window.pinned, 0U);
	KUNIT_EXPECT_FALSE(test, opaque_window.consumed);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 2U);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[0], replacement);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[1], replacement);
	KUNIT_EXPECT_EQ(test, refcount_read(&replacement->refs), 2);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
	opaque_grow_entries(ent, compact.handle);
	n = opaque_ref_segs(replacement, ent[0].off, ent[0].len, NULL) +
	    opaque_ref_segs(replacement, ent[1].off, ent[1].len, NULL);
	KUNIT_ASSERT_NE(test, n, 5U);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, n);
	opaque_vec_expect_cursor(test, op);
	opaque_test_tx_done(op, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(op), NULL);
	io_opaque_vec_release(op);
	io_opaque_data_put(store, compact.data);
	io_opaque_compact_end(&compact);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)ALIGN(80000, PAGE_SIZE));
	mutex_lock(&store->tables);
	io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
}

/*
 * The cursor is built without tables from the versions the send pinned, so it
 * does not matter that the slot holds another by then, nor that the pins are
 * the last references of the version: its segments are still there.
 */
static void opaque_send_vec_build_pinned(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *a = opaque_sparse_data(test, store), *old = a, *replacement;
	struct io_opaque_req compact = {
		.store = store, .data = a, .handle = io_opaque_handle(0, 9),
	};
	struct io_uring_opaque_send_entry ent[3] = {
		{ .handle = compact.handle, .off = 1500, .len = 1000 },
		{ .handle = compact.handle, .off = 79000, .len = 1000 },
		{ .handle = compact.handle, .off = 3900, .len = 2200 },
	};
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *want = kunit_kmalloc(test, 4200, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 4200, GFP_KERNEL);
	struct io_opaque_req *op;
	u32 i, done = 0;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, want);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_copy(a, source);
	for (i = 0; i < ARRAY_SIZE(ent); i++) {
		memcpy(want + done, source + ent[i].off, ent[i].len);
		done += ent[i].len;
	}
	opaque_test_publish(store, 0, 9, a);
	op = opaque_vec_new(test, store, ent, ARRAY_SIZE(ent), 0);
	opaque_vec_room(test, op, 16);
	mutex_lock(&store->tables);
	ret = io_opaque_send_acquire(op);
	mutex_unlock(&store->tables);
	KUNIT_ASSERT_EQ(test, ret, 0);
	/* The table's reference and one pin for the three entries. */
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
	/* The object is compacted, and the compaction worker is done with the old version. */
	refcount_inc(&a->refs);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	replacement = compact.replacement;
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	io_opaque_data_put(store, compact.data);
	io_opaque_compact_end(&compact);
	KUNIT_ASSERT_PTR_EQ(test, store->objects[0].data, replacement);
	KUNIT_EXPECT_EQ(test, refcount_read(&old->refs), 1);
	/* Only now the cursor: over the old version, which only the send holds. */
	io_opaque_vec_build(op);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[0], old);
	opaque_vec_expect_cursor(test, op);
	KUNIT_EXPECT_EQ(test, copy_from_iter(got, done, &op->iter), (size_t)done);
	KUNIT_EXPECT_MEMEQ(test, got, want, done);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)ALIGN(80000, PAGE_SIZE));
	mutex_lock(&store->tables);
	io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static void opaque_prep_sqe(struct opaque_prep *p, u16 ioprio, u64 addr, u64 off, u32 len)
{
	p->sqe = (struct io_uring_sqe) {
		.opaque_store_id = 1, .ioprio = ioprio, .addr = addr, .off = off, .len = len,
	};
}

/*
 * Prep copies the entries from user memory and makes a body of their total of
 * the SQE. The request's block outlives the request, unless it is big, and a
 * request that carries a block is not a vector for that.
 */
static void opaque_send_vec_prep(struct kunit *test)
{
	static const struct {
		u16 ioprio;
		u64 off;
		u32 len;
		bool nowhere;
		int result;
	} bad[] = {
		{ IORING_OPAQUE_SEND_VECTOR, 1, 3, false, -EINVAL },		/* off is zero */
		{ IORING_OPAQUE_SEND_VECTOR, 0, 0, false, -EINVAL },
		{ IORING_OPAQUE_SEND_VECTOR, 0, IORING_OPAQUE_SEND_VEC_MAX + 1, false, -EINVAL },
		{ IORING_OPAQUE_SEND_VECTOR | (1U << 2), 0, 3, false, -EINVAL },
		{ IORING_OPAQUE_SEND_VECTOR, 0, 3, true, -EFAULT },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_test_object(test, store);
	struct opaque_prep *p = kunit_kzalloc(test, sizeof(*p), GFP_KERNEL);
	const u64 h = io_opaque_handle(0, 9);
	struct io_uring_opaque_send_entry ent[3] = {
		{ .handle = h, .off = 0, .len = 1000 },
		{ .handle = h, .off = 5, .len = 10 },
		{ .handle = h, .off = 79000, .len = 1000 },
	}, zero[3] = {}, *many;
	struct io_opaque_vec *block;
	struct io_opaque_req *op;
	unsigned long addr;
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, p);
	many = kunit_kcalloc(test, 300, sizeof(*many), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, many);
	opaque_prep_init(test, p, store);
	addr = kunit_vm_mmap(test, NULL, 0, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_ANONYMOUS | MAP_PRIVATE, 0);
	if (!addr || addr >= TASK_SIZE)
		kunit_skip(test, "no userspace mapping for the entries");
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)addr, ent, sizeof(ent)), 0UL);

	/* The SQE becomes a body of the total, and the entries are copied. */
	opaque_prep_sqe(p, IORING_OPAQUE_SEND_VECTOR, addr, 0, 3);
	KUNIT_ASSERT_EQ(test, opaque_prep_send(p, &op), 0);
	KUNIT_EXPECT_EQ(test, op->send_flags, (u16)IORING_OPAQUE_SEND_VECTOR);
	KUNIT_EXPECT_EQ(test, op->handle, 0ULL);
	KUNIT_EXPECT_EQ(test, op->offset, 0ULL);
	KUNIT_EXPECT_EQ(test, op->length, 2010U);
	KUNIT_EXPECT_TRUE(test, op->resolved);
	KUNIT_EXPECT_EQ(test, op->total_length, 2010U);
	KUNIT_ASSERT_NOT_NULL(test, op->vec);
	KUNIT_EXPECT_EQ(test, op->vec->nr, 3U);
	KUNIT_EXPECT_EQ(test, op->vec->max_len, 1000U);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
	KUNIT_EXPECT_MEMEQ(test, op->vec->ent, ent, sizeof(ent));
	/* The array may go once the SQE is prepared. */
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)addr, zero, sizeof(zero)), 0UL);
	KUNIT_EXPECT_MEMEQ(test, op->vec->ent, ent, sizeof(ent));
	KUNIT_EXPECT_EQ(test, opaque_vec_admit(op), 0);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&op->iter), (size_t)2010);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	block = op->vec;
	opaque_prep_done(p);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->requests), 0);

	/* The next request gets the block back, whichever form it takes. */
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)addr, ent, sizeof(ent)), 0UL);
	opaque_prep_sqe(p, IORING_OPAQUE_SEND_VECTOR, addr, 0, 3);
	KUNIT_ASSERT_EQ(test, opaque_prep_send(p, &op), 0);
	/* Under KASAN a cached entry is cleared when taken, so the block is not kept. */
	if (!IS_ENABLED(CONFIG_KASAN))
		KUNIT_EXPECT_PTR_EQ(test, op->vec, block);
	opaque_prep_done(p);
	opaque_prep_sqe(p, 0, h, 5, 100);
	KUNIT_ASSERT_EQ(test, opaque_prep_send(p, &op), 0);
	if (!IS_ENABLED(CONFIG_KASAN))
		KUNIT_EXPECT_PTR_EQ(test, op->vec, block);
	KUNIT_EXPECT_EQ(test, op->send_flags, (u16)0);
	KUNIT_EXPECT_EQ(test, op->handle, h);
	KUNIT_EXPECT_EQ(test, op->length, 100U);
	/* A single send admits as one whatever block its request carries. */
	mutex_lock(&store->tables);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(op), 0);
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_PTR_EQ(test, op->data, data);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&op->iter), (size_t)100);
	if (op->vec)
		KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
	opaque_prep_done(p);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);

	/* A block past the soft cap is not worth keeping. */
	for (i = 0; i < 300; i++)
		many[i] = (struct io_uring_opaque_send_entry) { .handle = h, .off = i, .len = 1 };
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)addr, many, 300 * sizeof(*many)), 0UL);
	opaque_prep_sqe(p, IORING_OPAQUE_SEND_VECTOR, addr, 0, 300);
	KUNIT_ASSERT_EQ(test, opaque_prep_send(p, &op), 0);
	KUNIT_EXPECT_EQ(test, op->length, 300U);
	KUNIT_EXPECT_EQ(test, op->vec->cap_nr, 300U);
	opaque_prep_done(p);
	opaque_prep_sqe(p, 0, h, 0, 1);
	KUNIT_ASSERT_EQ(test, opaque_prep_send(p, &op), 0);
	KUNIT_EXPECT_PTR_EQ(test, op->vec, NULL);
	opaque_prep_done(p);

	/* What prep refuses leaves nothing behind. */
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)addr, ent, sizeof(ent)), 0UL);
	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		opaque_prep_sqe(p, bad[i].ioprio, bad[i].nowhere ? 0 : addr, bad[i].off,
				bad[i].len);
		KUNIT_EXPECT_EQ(test, opaque_prep_send(p, &op), bad[i].result);
		opaque_prep_done(p);
	}
	zero[0] = ent[0];
	zero[1] = ent[1];
	zero[2] = (struct io_uring_opaque_send_entry) { .handle = h, .off = 0, .len = 0 };
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)addr, zero, sizeof(zero)), 0UL);
	opaque_prep_sqe(p, IORING_OPAQUE_SEND_VECTOR, addr, 0, 3);
	KUNIT_EXPECT_EQ(test, opaque_prep_send(p, &op), -EINVAL);	/* an empty range */
	opaque_prep_done(p);
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)addr, ent, sizeof(ent)), 0UL);
	opaque_prep_sqe(p, IORING_OPAQUE_SEND_VECTOR | IORING_OPAQUE_SEND_LAST, addr, 0, 3);
	p->req.flags |= REQ_F_CQE_SKIP;
	KUNIT_EXPECT_EQ(test, opaque_prep_send(p, &op), -EINVAL);	/* LAST must be reported */
	opaque_prep_done(p);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->requests), 0);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
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
	KUNIT_CASE(opaque_tx_order),
	KUNIT_CASE(opaque_tx_inline_admission),
	KUNIT_CASE(opaque_tx_failed_head),
	KUNIT_CASE(opaque_tx_idle_cache),
	KUNIT_CASE(opaque_tx_lock_order),
	KUNIT_CASE(opaque_cache_miss_then_hit),
	KUNIT_CASE(opaque_cache_cooling_probe),
	KUNIT_CASE(opaque_cache_tx_clone),
	KUNIT_CASE(opaque_splice_folio_runs),
	KUNIT_CASE(opaque_splice_subrange_offset),
	KUNIT_CASE(opaque_splice_split_pages),
	KUNIT_CASE(opaque_splice_csum_none),
	KUNIT_CASE(opaque_splice_frag_limit),
	KUNIT_CASE(opaque_splice_bvec_segments),
	KUNIT_CASE(opaque_splice_kvec_runs),
	KUNIT_CASE(opaque_cache_limit),
	KUNIT_CASE(opaque_cache_age),
	KUNIT_CASE(opaque_cache_accounting),
	KUNIT_CASE(opaque_cache_store_lifecycle),
	KUNIT_CASE(opaque_compact_copy_runs),
	KUNIT_CASE(opaque_compact_cancel_units),
	KUNIT_CASE(opaque_create_uses_cache),
	KUNIT_CASE(opaque_compact_inline),
	KUNIT_CASE(opaque_send_resolve),
	KUNIT_CASE(opaque_send_copy_policy),
	KUNIT_CASE(opaque_send_loopback),
	KUNIT_CASE(opaque_attach_lock_order),
	KUNIT_CASE(opaque_send_vec_check),
	KUNIT_CASE(opaque_send_vec_acquire),
	KUNIT_CASE(opaque_send_vec_pin_once),
	KUNIT_CASE(opaque_send_vec_iter),
	KUNIT_CASE(opaque_send_vec_loopback),
	KUNIT_CASE(opaque_send_vec_admit_fail),
	KUNIT_CASE(opaque_send_vec_resume),
	KUNIT_CASE(opaque_send_vec_grow_retry),
	KUNIT_CASE(opaque_send_vec_build_pinned),
	KUNIT_CASE(opaque_send_vec_prep),
	KUNIT_CASE(opaque_params_sysfs),
	{}
};

static struct kunit_suite opaque_suite = {
	.name = "io_uring-opaque",
	.test_cases = opaque_cases,
};

kunit_test_suite(opaque_suite);
