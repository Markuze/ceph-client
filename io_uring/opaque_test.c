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
	/* The head links with tx_lock alone, and takes its references later, with tables alone. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&a, sk), IO_OPAQUE_TX_SETTLE);
	lockdep_assert_none_held_once();
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_settle_head(&a), 0);
	lockdep_assert_none_held_once();
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	/* A follower takes its references before it parks, with tables and then tx_lock. */
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

/*
 * The head of a queue links before it takes its references, so one that cannot
 * take them has to leave as a head that sent nothing does: whoever waits behind
 * it is handed the queue with its own pin intact, and the queue does not fail.
 */
static void opaque_send_head_admit_fails(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_test_object(test, store);
	struct sock *sk = kunit_kzalloc(test, sizeof(*sk), GFP_KERNEL);
	struct io_opaque_req head, follower, last;
	struct io_opaque_tx *tx;

	KUNIT_ASSERT_NOT_NULL(test, sk);
	opaque_test_wakeups(test);
	opaque_test_tx_op(&head, store);
	opaque_test_tx_op(&follower, store);
	/* Slot 1 holds nothing, so the head's handle is stale; the follower's is good. */
	head.handle = io_opaque_handle(1, 3);
	head.length = 100;
	follower.handle = io_opaque_handle(0, 9);
	follower.length = 100;

	/* The queue is free: the head links at once and has taken nothing. */
	KUNIT_ASSERT_EQ(test, io_opaque_tx_join(&head, sk), IO_OPAQUE_TX_SETTLE);
	tx = head.tx;
	KUNIT_ASSERT_NOT_NULL(test, tx);
	KUNIT_EXPECT_PTR_EQ(test,
			    list_first_entry(&tx->requests, struct io_opaque_req, send), &head);
	KUNIT_EXPECT_EQ(test, tx->admitting, 0U);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 1U);
	/* A frame that arrives meanwhile waits behind it, with its reference taken. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&follower, sk), IO_OPAQUE_TX_WAIT);
	KUNIT_EXPECT_EQ(test, follower.phase, IO_OPAQUE_SEND_WAIT);
	KUNIT_EXPECT_PTR_EQ(test, follower.data, data);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	/* The head's references cannot be taken; it takes nothing and is still the head. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_settle_head(&head), -ESTALE);
	lockdep_assert_none_held_once();
	KUNIT_EXPECT_FALSE(test, head.consumed);
	KUNIT_EXPECT_PTR_EQ(test, head.data, NULL);
	KUNIT_EXPECT_PTR_EQ(test, head.tx, tx);
	/* It leaves without having started: the follower is promoted and nothing fails. */
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&head), &follower);
	KUNIT_EXPECT_PTR_EQ(test, head.tx, NULL);
	KUNIT_EXPECT_EQ(test, follower.phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_FALSE(test, follower.canceled);
	KUNIT_EXPECT_FALSE(test, tx->failed);
	KUNIT_EXPECT_EQ(test, opaque_woken.nr, 0U);
	KUNIT_EXPECT_PTR_EQ(test, follower.data, data);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	/* The follower resumes as the head, with its pin, and goes through. */
	follower.phase = IO_OPAQUE_IDLE;
	KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&follower, sk), IO_OPAQUE_TX_HEAD);
	opaque_test_tx_done(&follower, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&follower), NULL);
	io_opaque_data_put(store, follower.data);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);

	/* Alone, through the pair that the other tests use: the queue is idle and clean after. */
	opaque_test_tx_op(&head, store);
	head.handle = io_opaque_handle(1, 3);
	head.length = 100;
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&head, sk), -ESTALE);
	KUNIT_EXPECT_PTR_EQ(test, head.tx, NULL);
	KUNIT_EXPECT_FALSE(test, tx->failed);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	/* A LAST send whose handle is gone consumes nothing, as one that fails behind another. */
	opaque_test_tx_op(&last, store);
	last.handle = io_opaque_handle(1, 3);
	last.length = 100;
	last.send_flags = IORING_OPAQUE_SEND_LAST;
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&last, sk), -ESTALE);
	KUNIT_EXPECT_FALSE(test, last.consumed);
	KUNIT_EXPECT_PTR_EQ(test, last.tx, NULL);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, data);
	/* A queue that took a head that failed this way takes the next frame as its head. */
	opaque_test_tx_op(&follower, store);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&follower, sk), IO_OPAQUE_TX_HEAD);
	opaque_test_tx_done(&follower, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&follower), NULL);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);

	mutex_lock(&store->tables);
	io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/*
 * A frame that arrives while the head has yet to take its references takes its
 * own first, and the head, which is ahead of it in the queue, finds what that
 * left. A head that then succeeds sends first and hands the queue on. A LAST
 * frame behind it that took the handle leaves the head with ESTALE and no
 * claim on LAST, nothing consumed and the queue not failed, and is promoted
 * with the references it took: the winner of two that contend for a handle is
 * the one that took it first, not the one first in the queue.
 */
static void opaque_send_head_races_follower(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct sock *sk = kunit_kzalloc(test, sizeof(*sk), GFP_KERNEL);
	u64 handle = io_opaque_handle(0, 9);
	unsigned int c;

	KUNIT_ASSERT_NOT_NULL(test, sk);
	opaque_test_wakeups(test);
	for (c = 0; c < 3; c++) {
		/* 0: the head goes on; 1: a LAST frame takes the handle; 2: both are LAST. */
		struct io_opaque_data *data = opaque_test_object(test, store);
		struct io_opaque_req head, follower;
		struct io_opaque_tx *tx;

		opaque_test_tx_op(&head, store);
		opaque_test_tx_op(&follower, store);
		head.handle = handle;
		head.length = 100;
		follower.handle = handle;
		follower.length = 100;
		follower.send_flags = c ? IORING_OPAQUE_SEND_LAST : 0;
		head.send_flags = c == 2 ? IORING_OPAQUE_SEND_LAST : 0;

		KUNIT_ASSERT_EQ(test, io_opaque_tx_join(&head, sk), IO_OPAQUE_TX_SETTLE);
		tx = head.tx;
		KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&follower, sk), IO_OPAQUE_TX_WAIT);
		lockdep_assert_none_held_once();
		KUNIT_EXPECT_EQ(test, follower.phase, IO_OPAQUE_SEND_WAIT);
		KUNIT_EXPECT_PTR_EQ(test, follower.data, data);
		KUNIT_EXPECT_EQ(test, follower.consumed, !!c);
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), c ? 1 : 2);
		KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, c ? NULL : data);

		if (!c) {
			/* The head takes its own, and goes first. */
			KUNIT_EXPECT_EQ(test, io_opaque_tx_settle_head(&head), 0);
			lockdep_assert_none_held_once();
			KUNIT_EXPECT_PTR_EQ(test, head.data, data);
			KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 3);
			opaque_test_tx_done(&head, true);
			KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&head), &follower);
			io_opaque_data_put(store, head.data);
			head.data = NULL;
			KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
		} else {
			/* The handle is gone. The head asks nothing more of the queue. */
			KUNIT_EXPECT_EQ(test, io_opaque_tx_settle_head(&head), -ESTALE);
			lockdep_assert_none_held_once();
			KUNIT_EXPECT_FALSE(test, head.consumed);
			KUNIT_EXPECT_PTR_EQ(test, head.data, NULL);
			KUNIT_EXPECT_PTR_EQ(test, head.tx, tx);
			KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&head), &follower);
		}
		/* Either way the follower is promoted, whole, and the queue has not failed. */
		KUNIT_EXPECT_PTR_EQ(test, head.tx, NULL);
		KUNIT_EXPECT_EQ(test, follower.phase, IO_OPAQUE_QUEUED);
		KUNIT_EXPECT_FALSE(test, follower.canceled);
		KUNIT_EXPECT_FALSE(test, tx->failed);
		KUNIT_EXPECT_EQ(test, opaque_woken.nr, 0U);
		KUNIT_EXPECT_PTR_EQ(test, follower.data, data);
		follower.phase = IO_OPAQUE_IDLE;
		KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&follower, sk), IO_OPAQUE_TX_HEAD);
		opaque_test_tx_done(&follower, true);
		KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&follower), NULL);
		io_opaque_data_put(store, follower.data);
		if (!c) {
			KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
			mutex_lock(&store->tables);
			io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
			mutex_unlock(&store->tables);
		}
		/* A LAST frame's put freed the object, which nobody else held. */
		KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
	}
}

static int opaque_test_sendmsg(struct sock *sk, struct msghdr *msg, size_t size);

/*
 * Only plain, bound TCP is sent on with tcp_sendmsg_locked(): not a socket
 * whose proto sends some other way, not one whose proto is not the one it was
 * made with, and not one with no local port, which has not connected. Each of
 * the three is enough on its own.
 */
static void opaque_send_direct_predicate(struct kunit *test)
{
	struct tcp_sock *tp = kunit_kzalloc(test, sizeof(*tp), GFP_KERNEL);
	struct proto *other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);
	struct sock *sk;

	KUNIT_ASSERT_NOT_NULL(test, tp);
	KUNIT_ASSERT_NOT_NULL(test, other);
	sk = (struct sock *)tp;
	sk->sk_prot = &tcp_prot;
	sk->sk_prot_creator = &tcp_prot;
	inet_sk(sk)->inet_num = 4242;
	KUNIT_EXPECT_TRUE(test, io_opaque_send_direct(sk));

	/* A ULP or a socket map has a proto of its own, a copy with another sendmsg. */
	*other = tcp_prot;
	other->sendmsg = opaque_test_sendmsg;
	sk->sk_prot = other;
	sk->sk_prot_creator = other;
	KUNIT_EXPECT_FALSE(test, io_opaque_send_direct(sk));
	/* A proto that is not the one the socket was made with, whatever it sends with. */
	other->sendmsg = tcp_sendmsg;
	sk->sk_prot_creator = &tcp_prot;
	KUNIT_EXPECT_FALSE(test, io_opaque_send_direct(sk));
	sk->sk_prot = &tcp_prot;
	sk->sk_prot_creator = other;
	KUNIT_EXPECT_FALSE(test, io_opaque_send_direct(sk));
	/* A socket with no local port has not connected, and stays on the ordinary path. */
	sk->sk_prot_creator = &tcp_prot;
	inet_sk(sk)->inet_num = 0;
	KUNIT_EXPECT_FALSE(test, io_opaque_send_direct(sk));
	inet_sk(sk)->inet_num = 1;
	KUNIT_EXPECT_TRUE(test, io_opaque_send_direct(sk));
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

/* A socket buffer of @bytes, which TCP does not resize. */
static void opaque_wire_sndbuf(struct sock *sk, int bytes)
{
	lock_sock(sk);
	sk->sk_userlocks |= SOCK_SNDBUF_LOCK;
	WRITE_ONCE(sk->sk_sndbuf, bytes);
	release_sock(sk);
}

/* Read what the peer has without waiting, up to @room bytes. */
static u32 opaque_wire_drain(struct opaque_wire *w, u8 *buf, u32 room)
{
	u32 done = 0;

	while (done < room) {
		struct msghdr msg = {};
		struct kvec vec = { .iov_base = buf + done, .iov_len = room - done };
		int ret = kernel_recvmsg(w->rx, &msg, &vec, 1, vec.iov_len, MSG_DONTWAIT);

		if (ret <= 0)
			break;
		done += ret;
	}
	return done;
}

/* Read @len more bytes from the peer, waiting for them. */
static void opaque_wire_read(struct kunit *test, struct opaque_wire *w, u8 *buf, u32 done,
			     u32 len)
{
	while (done < len) {
		struct msghdr msg = {};
		struct kvec vec = { .iov_base = buf + done, .iov_len = len - done };
		int ret = kernel_recvmsg(w->rx, &msg, &vec, 1, vec.iov_len, 0);

		KUNIT_ASSERT_GT(test, ret, 0);
		done += ret;
	}
}

/*
 * Watch the puts of versions made while a send runs: the references each held
 * when it was put and whether @sk was owned. A put of the last reference is the
 * one that frees the version, and must not be made with the socket owned.
 */
static struct {
	struct sock *sk;
	unsigned int n;
	int refs[8];
	bool owned[8];
} opaque_put_spy;

static void opaque_test_data_put_spy(struct io_opaque_store *store, struct io_opaque_data *data)
{
	struct kunit *test = kunit_get_current_test();

	if (data) {
		if (opaque_put_spy.n < ARRAY_SIZE(opaque_put_spy.refs)) {
			opaque_put_spy.refs[opaque_put_spy.n] = refcount_read(&data->refs);
			opaque_put_spy.owned[opaque_put_spy.n] =
				sock_owned_by_user_nocheck(opaque_put_spy.sk);
		}
		opaque_put_spy.n++;
	}
	kunit_deactivate_static_stub(test, io_opaque_data_put);
	io_opaque_data_put(store, data);
	kunit_activate_static_stub(test, io_opaque_data_put, opaque_test_data_put_spy);
}

static void opaque_put_spy_start(struct kunit *test, struct sock *sk)
{
	memset(&opaque_put_spy, 0, sizeof(opaque_put_spy));
	opaque_put_spy.sk = sk;
	kunit_activate_static_stub(test, io_opaque_data_put, opaque_test_data_put_spy);
}

static void opaque_put_spy_stop(struct kunit *test)
{
	kunit_deactivate_static_stub(test, io_opaque_data_put);
}

/* Every put of a last reference seen since the spy started, and none made with the socket owned. */
static void opaque_put_spy_expect(struct kunit *test, unsigned int last)
{
	unsigned int i, seen = min_t(unsigned int, opaque_put_spy.n,
				     ARRAY_SIZE(opaque_put_spy.refs));

	KUNIT_EXPECT_EQ(test, opaque_put_spy.n, last);
	for (i = 0; i < seen; i++) {
		KUNIT_EXPECT_EQ(test, opaque_put_spy.refs[i], 1);
		KUNIT_EXPECT_FALSE(test, opaque_put_spy.owned[i]);
	}
}

/* One send over the wire fixture: the shape of the body and of the frame, and how it is issued. */
struct opaque_send_case {
	bool vector;		/* three ranges of one handle, else one range */
	bool last;
	bool copy;		/* TCP copies the body instead of splicing it */
	bool unlocked;		/* issued as io-wq does, else inline under the ring lock */
	bool frame_only;	/* a reply made of the frame alone */
	u32 prefix, suffix;
};

/*
 * Run one send of @c on request 0 of the fixture, with no cleanup after it, and
 * check what it leaves: its result, one tcp_sendmsg_locked() per part of the
 * frame, the references it took and the queue place it held all given back by
 * the send itself, and the bytes the peer reads.
 */
static void opaque_wire_send(struct kunit *test, struct opaque_wire *w,
			     struct io_opaque_store *store, const struct opaque_send_case *c)
{
	const u64 handle = io_opaque_handle(0, 9);
	struct io_uring_opaque_send_entry ent[3] = {
		{ .handle = handle, .off = 4196, .len = 3000 },
		{ .handle = handle, .off = 70000, .len = 2000 },
		{ .handle = handle, .off = 100, .len = 1500 },
	};
	u32 body = c->frame_only ? 0 : c->vector ? 6500 : 40000;
	u32 total = c->prefix + body + c->suffix, parts, at, i;
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *frame = kunit_kmalloc(test, c->prefix + c->suffix + 1, GFP_KERNEL);
	u8 *want = kunit_kmalloc(test, total, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, total, GFP_KERNEL);
	u64 copied = io_opaque_stat_read(IO_OPAQUE_STAT_BODY_COPIED);
	long calls = atomic_long_read(&io_opaque_send_locked_calls);
	struct io_opaque_req *op;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, frame);
	KUNIT_ASSERT_NOT_NULL(test, want);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	for (i = 0; i < c->prefix + c->suffix; i++)
		frame[i] = 0xc0 + i;
	/* The wire carries the prefix, the ranges in order, and the suffix. */
	memcpy(want, frame, c->prefix);
	at = c->prefix;
	if (c->vector) {
		for (i = 0; i < ARRAY_SIZE(ent); i++) {
			memcpy(want + at, source + ent[i].off, ent[i].len);
			at += ent[i].len;
		}
	} else if (body) {
		memcpy(want + at, source + 4196, body);
		at += body;
	}
	memcpy(want + at, frame + c->prefix, c->suffix);

	if (c->vector) {
		op = opaque_vec_new(test, store, ent, ARRAY_SIZE(ent),
				    c->last ? IORING_OPAQUE_SEND_LAST : 0);
	} else {
		op = kunit_kzalloc(test, sizeof(*op), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, op);
		op->store = store;
		op->send_flags = c->last ? IORING_OPAQUE_SEND_LAST : 0;
		op->handle = c->frame_only ? 0 : handle;
		op->offset = c->frame_only ? 0 : 4196;
		op->length = body;
	}
	op->req = &w->req[0];
	op->op = U16_MAX;
	INIT_LIST_HEAD(&op->wait);
	INIT_LIST_HEAD(&op->cancel);
	INIT_LIST_HEAD(&op->send);
	w->req[0].async_data = op;
	w->req[0].flags = 0;
	memset(&w->req[0].cqe, 0, sizeof(w->req[0].cqe));
	if (c->prefix || c->suffix) {
		/* As the first issue leaves a request that has imported its frame. */
		op->framing = frame;
		op->prefix_length = c->prefix;
		op->suffix_length = c->suffix;
		op->total_length = total;
		op->resolved = true;
	} else {
		io_opaque_send_resolve(op);
	}
	KUNIT_ASSERT_TRUE(test, op->resolved);

	io_opaque_send_copy_max = c->copy ? SZ_1M : 0;
	opaque_put_spy_start(test, w->tx->sk);
	if (c->unlocked) {
		ret = io_opaque_send(&w->req[0], IO_URING_F_UNLOCKED);
	} else {
		mutex_lock(&w->ctx->uring_lock);
		ret = io_opaque_send(&w->req[0], IO_URING_F_NONBLOCK);
		mutex_unlock(&w->ctx->uring_lock);
	}
	opaque_put_spy_stop(test);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
	/* LAST holds the table's reference, and its put frees the object: after the socket. */
	opaque_put_spy_expect(test, c->last && !c->frame_only ? 1 : 0);
	KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, (s32)total);
	KUNIT_EXPECT_EQ(test, !!(w->req[0].cqe.flags & IORING_CQE_F_OPAQUE_CONSUMED), c->last);
	KUNIT_EXPECT_EQ(test, op->consumed, c->last);
	/* One call per part of the frame; the frame alone is a single part. */
	parts = c->frame_only ? 1 : !!c->prefix + 1 + !!c->suffix;
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_send_locked_calls) - calls, (long)parts);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_BODY_COPIED) - copied,
			c->copy ? (u64)body : 0);
	/* What the send held it has given back itself: there is no cleanup in this test. */
	KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
	KUNIT_EXPECT_PTR_EQ(test, op->data, NULL);
	if (c->vector)
		KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&store->tables));
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(w->tx->sk));

	opaque_wire_read(test, w, got, 0, total);
	KUNIT_EXPECT_MEMEQ(test, got, want, total);

	if (c->last) {
		/* The send held the only reference: the object went with it. */
		KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, NULL);
	} else {
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
		mutex_lock(&store->tables);
		io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
		mutex_unlock(&store->tables);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/*
 * The head of a queue sends its whole frame with the socket owned, from its
 * references to their release: one range or a vector, LAST or not, spliced or
 * copied, issued inline under the ring lock or as io-wq does, with and without
 * a frame. Each ends with nothing held, and the peer reads the frame.
 */
static void opaque_send_locked_path(struct kunit *test)
{
	static const struct opaque_send_case cases[] = {
		{ .unlocked = true },
		{ },
		{ .last = true, .unlocked = true },
		{ .last = true },
		{ .vector = true, .unlocked = true },
		{ .vector = true },
		{ .vector = true, .last = true, .unlocked = true },
		{ .vector = true, .last = true },
		{ .copy = true, .unlocked = true },
		{ .copy = true, .vector = true },
		{ .prefix = 24, .suffix = 8, .unlocked = true },
		{ .prefix = 24, .suffix = 8, .vector = true, .last = true },
		{ .prefix = 24 },
		{ .suffix = 8, .copy = true },
		{ .prefix = 16, .suffix = 16, .frame_only = true, .unlocked = true },
		{ .prefix = 16, .frame_only = true },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_test_params_save(test);
	opaque_wire_init(test, w);
	/* Room enough that nothing here waits for space. */
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	for (i = 0; i < ARRAY_SIZE(cases); i++)
		opaque_wire_send(test, w, store, &cases[i]);
}

/*
 * A frame bigger than the socket has room for: the head queues what fits and
 * returns EAGAIN with everything it holds still held, its references, its
 * place at the head of the queue and its cursor, and a later issue goes on
 * from the cursor until the peer has read it all. The socket is not owned
 * between issues.
 */
static void opaque_wire_eagain(struct kunit *test, struct opaque_wire *w,
			       struct io_opaque_store *store, bool vector)
{
	const u64 handle = io_opaque_handle(0, 9);
	const u32 prefix = 24, suffix = 8, range = 80000, nr = vector ? 6 : 1;
	const u32 total = prefix + nr * range + suffix;
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_uring_opaque_send_entry *ent = kunit_kcalloc(test, nr, sizeof(*ent), GFP_KERNEL);
	u8 *source = kunit_kmalloc(test, range, GFP_KERNEL);
	u8 *frame = kunit_kmalloc(test, prefix + suffix, GFP_KERNEL);
	u8 *want = kunit_kmalloc(test, total, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, total, GFP_KERNEL);
	unsigned int tries, i, done = 0;
	struct io_opaque_req *op;
	u64 progress;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ent);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, frame);
	KUNIT_ASSERT_NOT_NULL(test, want);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	for (i = 0; i < prefix + suffix; i++)
		frame[i] = 0xc0 + i;
	memcpy(want, frame, prefix);
	for (i = 0; i < nr; i++) {
		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = handle, .off = 0, .len = range,
		};
		memcpy(want + prefix + i * range, source, range);
	}
	memcpy(want + prefix + nr * range, frame + prefix, suffix);

	if (vector) {
		/* More segments than entries: the block grows with the socket owned. */
		op = opaque_vec_new(test, store, ent, nr, IORING_OPAQUE_SEND_LAST);
	} else {
		op = kunit_kzalloc(test, sizeof(*op), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, op);
		op->store = store;
		op->handle = handle;
		op->length = range;
	}
	op->req = &w->req[0];
	op->op = U16_MAX;
	INIT_LIST_HEAD(&op->wait);
	INIT_LIST_HEAD(&op->cancel);
	INIT_LIST_HEAD(&op->send);
	op->framing = frame;
	op->prefix_length = prefix;
	op->suffix_length = suffix;
	op->total_length = total;
	op->resolved = true;
	w->req[0].async_data = op;
	w->req[0].flags = 0;
	memset(&w->req[0].cqe, 0, sizeof(w->req[0].cqe));
	io_opaque_send_copy_max = 0;
	/* A buffer far below the frame: one issue cannot queue it. */
	opaque_wire_sndbuf(w->tx->sk, SZ_32K);

	mutex_lock(&w->ctx->uring_lock);
	ret = io_opaque_send(&w->req[0], IO_URING_F_NONBLOCK);
	mutex_unlock(&w->ctx->uring_lock);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_EQ(test, ret, -EAGAIN);
	progress = op->progress;
	KUNIT_EXPECT_GT(test, progress, 0ULL);
	KUNIT_EXPECT_LT(test, progress, (u64)total);
	/* Held, not given back: no CQE, the references, the head's place, and the LAST claim. */
	KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 0);
	KUNIT_ASSERT_NOT_NULL(test, op->tx);
	KUNIT_EXPECT_PTR_EQ(test, list_first_entry(&op->tx->requests, struct io_opaque_req, send),
			    op);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 1U);
	if (vector) {
		KUNIT_EXPECT_EQ(test, op->vec->pinned, nr);
		KUNIT_EXPECT_TRUE(test, op->consumed);
		KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, NULL);
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), (int)nr);
	} else {
		KUNIT_EXPECT_PTR_EQ(test, op->data, data);
		KUNIT_EXPECT_FALSE(test, op->consumed);
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	}
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&store->tables));
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(w->tx->sk));

	/* The peer has read nothing; each issue goes on from the cursor, then the peer reads. */
	opaque_put_spy_start(test, w->tx->sk);
	for (tries = 0; tries < 4000; tries++) {
		u64 before = op->progress;

		mutex_lock(&w->ctx->uring_lock);
		ret = io_opaque_send(&w->req[0], IO_URING_F_NONBLOCK);
		mutex_unlock(&w->ctx->uring_lock);
		if (ret == IOU_COMPLETE)
			break;
		KUNIT_ASSERT_EQ(test, ret, -EAGAIN);
		KUNIT_ASSERT_GE(test, op->progress, before);
		KUNIT_ASSERT_NOT_NULL(test, op->tx);
		done += opaque_wire_drain(w, got + done, total - done);
		usleep_range(500, 1000);
	}
	opaque_put_spy_stop(test);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
	/* The LAST vector's object is freed once, by the issue that completes, after the socket. */
	opaque_put_spy_expect(test, vector ? 1 : 0);
	KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, (s32)total);
	KUNIT_EXPECT_EQ(test, op->progress, (u64)total);
	KUNIT_EXPECT_EQ(test, !!(w->req[0].cqe.flags & IORING_CQE_F_OPAQUE_CONSUMED), vector);
	KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
	KUNIT_EXPECT_PTR_EQ(test, op->data, NULL);
	if (vector)
		KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(w->tx->sk));
	opaque_wire_read(test, w, got, done, total);
	KUNIT_EXPECT_MEMEQ(test, got, want, total);

	if (!vector) {
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
		mutex_lock(&store->tables);
		io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
		mutex_unlock(&store->tables);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static void opaque_send_locked_eagain(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_test_params_save(test);
	opaque_wire_init(test, w);
	/* One range, then a LAST vector, on the same connection. */
	opaque_wire_eagain(test, w, store, false);
	opaque_wire_eagain(test, w, store, true);
}

/* A request for the fixture's request @i, a frame-less send of @length bytes of the object. */
static struct io_opaque_req *opaque_wire_req(struct kunit *test, struct opaque_wire *w,
					     struct io_opaque_store *store, int i, u16 flags,
					     u64 offset, u32 length)
{
	struct io_opaque_req *op = kunit_kzalloc(test, sizeof(*op), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, op);
	op->store = store;
	op->req = &w->req[i];
	op->op = U16_MAX;
	op->send_flags = flags;
	op->handle = io_opaque_handle(0, 9);
	op->offset = offset;
	op->length = length;
	INIT_LIST_HEAD(&op->wait);
	INIT_LIST_HEAD(&op->cancel);
	INIT_LIST_HEAD(&op->send);
	io_opaque_send_resolve(op);
	w->req[i].async_data = op;
	w->req[i].flags = 0;
	return op;
}

/*
 * A head that ends short under the socket lock fails its queue before it lets
 * go of the socket: the follower parked behind it, which took its references
 * on joining, is canceled, and the head's own references have gone with the
 * CQE it completes.
 */
static void opaque_send_locked_short(struct kunit *test)
{
	const struct io_uring_opaque_send_entry ent[6] = {
		[0 ... 5] = { .handle = io_opaque_handle(0, 9), .off = 0, .len = 80000 },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req *head, *follower, third;
	struct io_opaque_tx *tx;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_test_publish(store, 0, 9, data);
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_32K);
	/* Far more than the socket and its peer will take without being read. */
	head = opaque_vec_new(test, store, ent, ARRAY_SIZE(ent), 0);
	head->req = &w->req[0];
	head->op = U16_MAX;
	INIT_LIST_HEAD(&head->wait);
	INIT_LIST_HEAD(&head->cancel);
	INIT_LIST_HEAD(&head->send);
	io_opaque_send_resolve(head);
	w->req[0].async_data = head;
	follower = opaque_wire_req(test, w, store, 1, 0, 100, 1000);

	/* The head queues what fits, and keeps its place. */
	mutex_lock(&w->ctx->uring_lock);
	ret = io_opaque_send(&w->req[0], IO_URING_F_NONBLOCK);
	mutex_unlock(&w->ctx->uring_lock);
	KUNIT_ASSERT_EQ(test, ret, -EAGAIN);
	tx = head->tx;
	KUNIT_ASSERT_NOT_NULL(test, tx);
	/* A frame for the same socket parks behind it, holding its reference. */
	mutex_lock(&w->ctx->uring_lock);
	ret = io_opaque_send(&w->req[1], IO_URING_F_NONBLOCK);
	mutex_unlock(&w->ctx->uring_lock);
	KUNIT_ASSERT_EQ(test, ret, IOU_ISSUE_SKIP_COMPLETE);
	KUNIT_EXPECT_EQ(test, follower->phase, IO_OPAQUE_SEND_WAIT);
	KUNIT_EXPECT_PTR_EQ(test, follower->tx, tx);
	KUNIT_EXPECT_PTR_EQ(test, follower->data, data);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 3);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&store->tables));
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(w->tx->sk));

	/* MSG_DONTWAIT is in force when the head is retried: it ends short, and must say so. */
	w->req[0].flags |= REQ_F_NOWAIT;
	mutex_lock(&w->ctx->uring_lock);
	ret = io_opaque_send(&w->req[0], IO_URING_F_NONBLOCK);
	mutex_unlock(&w->ctx->uring_lock);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
	KUNIT_EXPECT_GT(test, head->progress, 0ULL);
	KUNIT_EXPECT_LT(test, head->progress, (u64)head->total_length);
	KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, (s32)head->progress);
	KUNIT_EXPECT_TRUE(test, w->req[0].flags & REQ_F_FAIL);
	KUNIT_EXPECT_PTR_EQ(test, head->tx, NULL);
	KUNIT_EXPECT_EQ(test, head->vec->pinned, 0U);
	/* The queue failed with the head: its follower is canceled, and queued for task work. */
	KUNIT_EXPECT_TRUE(test, tx->failed);
	KUNIT_EXPECT_TRUE(test, follower->canceled);
	KUNIT_EXPECT_EQ(test, follower->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_EQ(test, follower->result, -ECANCELED);
	KUNIT_EXPECT_PTR_EQ(test, mpscq_pop(&w->ctx->work_list, &w->ctx->work_head),
			    &w->req[1].io_task_work.node);
	/* Only the follower's pin is left on the object besides the table's. */
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&store->tables));
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(w->tx->sk));
	/* A frame that comes now is refused, until the canceled follower has gone. */
	opaque_test_tx_op(&third, store);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&third, w->tx->sk), -ECANCELED);
	KUNIT_EXPECT_PTR_EQ(test, third.tx, NULL);
	/* Its cleanup: it leaves the queue, which then forgets the failure. */
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(follower), NULL);
	KUNIT_EXPECT_FALSE(test, tx->failed);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	io_opaque_data_put(store, follower->data);
	follower->data = NULL;
	mutex_lock(&store->tables);
	io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/*
 * A send that fails under the socket lock undoes itself there: the CQE says
 * why, the queue place and the references are gone, and LAST has still
 * consumed the object, as it does on every outcome once it has been admitted.
 */
static void opaque_send_locked_error(struct kunit *test)
{
	const struct io_uring_opaque_send_entry ent[2] = {
		{ .handle = io_opaque_handle(0, 9), .off = 0, .len = 3000 },
		{ .handle = io_opaque_handle(0, 9), .off = 10000, .len = 3000 },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	unsigned int last, vector;

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_wire_init(test, w);
	/* The write side is shut: TCP refuses every send with EPIPE, and no signal. */
	KUNIT_ASSERT_EQ(test, kernel_sock_shutdown(w->tx, SHUT_WR), 0);
	for (last = 0; last < 2; last++) {
		for (vector = 0; vector < 2; vector++) {
			struct io_opaque_data *data = opaque_sparse_data(test, store);
			u16 flags = last ? IORING_OPAQUE_SEND_LAST : 0;
			struct io_opaque_req *op;
			int ret;

			opaque_test_publish(store, 0, 9, data);
			if (vector) {
				op = opaque_vec_new(test, store, ent, ARRAY_SIZE(ent), flags);
				op->req = &w->req[0];
				op->op = U16_MAX;
				INIT_LIST_HEAD(&op->wait);
				INIT_LIST_HEAD(&op->cancel);
				INIT_LIST_HEAD(&op->send);
				io_opaque_send_resolve(op);
				w->req[0].async_data = op;
				w->req[0].flags = 0;
			} else {
				op = opaque_wire_req(test, w, store, 0, flags, 100, 3000);
			}
			w->req[0].cqe.res = 0;
			w->req[0].cqe.flags = 0;
			opaque_put_spy_start(test, w->tx->sk);
			ret = io_opaque_send(&w->req[0], IO_URING_F_UNLOCKED);
			opaque_put_spy_stop(test);
			lockdep_assert_none_held_once();
			KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
			/* An error frees what LAST took as a success does: after the socket. */
			opaque_put_spy_expect(test, last ? 1 : 0);
			KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, -EPIPE);
			KUNIT_EXPECT_TRUE(test, w->req[0].flags & REQ_F_FAIL);
			KUNIT_EXPECT_EQ(test, w->req[0].cqe.flags & IORING_CQE_F_OPAQUE_CONSUMED,
					last ? IORING_CQE_F_OPAQUE_CONSUMED : 0);
			KUNIT_EXPECT_EQ(test, op->progress, 0ULL);
			KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
			KUNIT_EXPECT_PTR_EQ(test, op->data, NULL);
			if (vector)
				KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
			KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
			KUNIT_EXPECT_FALSE(test, mutex_is_locked(&store->tables));
			KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(w->tx->sk));
			if (last) {
				KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, NULL);
			} else {
				KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
				mutex_lock(&store->tables);
				io_opaque_data_put(store,
						   io_opaque_detach(store, &store->objects[0]));
				mutex_unlock(&store->tables);
			}
			KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
			KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
		}
	}
}

/*
 * A LAST vector over two versions, one of them twice: each reference that is
 * not the last on its version goes with the socket, the last of each after it,
 * and none is put twice.
 */
static void opaque_send_locked_last_versions(struct kunit *test)
{
	const struct io_uring_opaque_send_entry ent[3] = {
		{ .handle = io_opaque_handle(0, 9), .off = 100, .len = 1000 },
		{ .handle = io_opaque_handle(1, 7), .off = 3000, .len = 2000 },
		{ .handle = io_opaque_handle(0, 9), .off = 10000, .len = 500 },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *a = opaque_sparse_data(test, store);
	struct io_opaque_data *b = opaque_sparse_data(test, store);
	struct io_opaque_req *op;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_test_publish(store, 0, 9, a);
	opaque_test_publish(store, 1, 7, b);
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	op = opaque_vec_new(test, store, ent, ARRAY_SIZE(ent), IORING_OPAQUE_SEND_LAST);
	op->req = &w->req[0];
	op->op = U16_MAX;
	INIT_LIST_HEAD(&op->wait);
	INIT_LIST_HEAD(&op->cancel);
	INIT_LIST_HEAD(&op->send);
	io_opaque_send_resolve(op);
	w->req[0].async_data = op;

	opaque_put_spy_start(test, w->tx->sk);
	ret = io_opaque_send(&w->req[0], IO_URING_F_UNLOCKED);
	opaque_put_spy_stop(test);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
	KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 3500);
	KUNIT_EXPECT_TRUE(test, op->consumed);
	/* B's only reference and A's last one: A's other was a decrement under the socket. */
	opaque_put_spy_expect(test, 2);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, NULL);
	KUNIT_EXPECT_PTR_EQ(test, store->objects[1].data, NULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static atomic_t opaque_test_proto_calls = ATOMIC_INIT(0);

/* A refusal for the next send that asks, and a frame that joins the queue as it does. */
static struct {
	int err;
	bool armed;
	struct io_opaque_req *follower;
	int joined;
} opaque_refuse;

static int opaque_refuse_now(struct sock *sk)
{
	if (!opaque_refuse.armed)
		return 0;
	opaque_refuse.armed = false;
	opaque_refuse.joined = io_opaque_tx_join(opaque_refuse.follower, sk);
	return opaque_refuse.err;
}

static int opaque_test_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
{
	int ret = opaque_refuse_now(sk);

	if (ret)
		return ret;
	atomic_inc(&opaque_test_proto_calls);
	return tcp_sendmsg(sk, msg, size);
}

static struct proto opaque_test_proto;

static void opaque_test_proto_restore(void *arg)
{
	struct sock *sk = arg;

	lock_sock(sk);
	sk->sk_prot = sk->sk_prot_creator;
	release_sock(sk);
}

/*
 * A socket whose proto has been replaced, as kTLS and a socket map do it, is
 * not sent on with tcp_sendmsg_locked(): the send takes its references first,
 * calls sock_sendmsg() for each part of the frame, which reaches the proto's
 * own sendmsg, and leaves its references to cleanup, as it always did.
 */
static void opaque_send_fallback(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	const u64 handle = io_opaque_handle(0, 9);
	const u32 prefix = 24, suffix = 8, body = 20000, total = prefix + body + suffix;
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *frame = kunit_kmalloc(test, prefix + suffix, GFP_KERNEL);
	u8 *want = kunit_kmalloc(test, total, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, total, GFP_KERNEL);
	long calls = atomic_long_read(&io_opaque_send_locked_calls);
	struct io_opaque_req *op;
	struct sock *sk;
	unsigned int i;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, frame);
	KUNIT_ASSERT_NOT_NULL(test, want);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_params_save(test);
	opaque_wire_init(test, w);
	sk = w->tx->sk;
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	for (i = 0; i < prefix + suffix; i++)
		frame[i] = 0xc0 + i;
	memcpy(want, frame, prefix);
	memcpy(want + prefix, source + 4196, body);
	memcpy(want + prefix + body, frame + prefix, suffix);

	op = kunit_kzalloc(test, sizeof(*op), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, op);
	op->store = store;
	op->req = &w->req[0];
	op->op = U16_MAX;
	op->handle = handle;
	op->offset = 4196;
	op->length = body;
	INIT_LIST_HEAD(&op->wait);
	INIT_LIST_HEAD(&op->cancel);
	INIT_LIST_HEAD(&op->send);
	op->framing = frame;
	op->prefix_length = prefix;
	op->suffix_length = suffix;
	op->total_length = total;
	op->resolved = true;
	w->req[0].async_data = op;

	/* A copy of tcp_prot with a sendmsg of its own, which sk_prot_creator does not follow. */
	opaque_test_proto = tcp_prot;
	opaque_test_proto.sendmsg = opaque_test_sendmsg;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_proto_restore, sk), 0);
	lock_sock(sk);
	sk->sk_prot = &opaque_test_proto;
	release_sock(sk);
	KUNIT_EXPECT_FALSE(test, io_opaque_send_direct(sk));
	atomic_set(&opaque_test_proto_calls, 0);
	io_opaque_send_copy_max = 0;

	ret = io_opaque_send(&w->req[0], IO_URING_F_UNLOCKED);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
	KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, (s32)total);
	/* The proto saw every part, and tcp_sendmsg_locked() was not called for any. */
	KUNIT_EXPECT_EQ(test, atomic_read(&opaque_test_proto_calls), 3);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_send_locked_calls), calls);
	/* The queue place went, but the references stay with the request until cleanup. */
	KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	KUNIT_EXPECT_PTR_EQ(test, op->data, data);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	opaque_wire_read(test, w, got, 0, total);
	KUNIT_EXPECT_MEMEQ(test, got, want, total);
	io_opaque_data_put(store, op->data);
	op->data = NULL;
	mutex_lock(&store->tables);
	io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/*
 * Attaching a collector takes the socket and then tables, the order in which a
 * send admits with the socket owned. This runs it on the wire fixture's socket,
 * whose lockdep class a send shares: an attach that succeeds, one that is
 * refused after both locks are taken, sends on the same socket, and the close
 * that undoes the attach. None may leave either lock held, and under lockdep
 * none may find them in the other order.
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

	/*
	 * A send on the same socket takes it and then tables too: with the
	 * collector attached, lockdep sees both from one class, and has nothing to
	 * say about the order.
	 */
	opaque_test_params_save(test);
	opaque_wire_send(test, w, store, &(struct opaque_send_case) { .unlocked = true });
	opaque_wire_send(test, w, store, &(struct opaque_send_case) { .vector = true });

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

/*
 * The steps a seek takes from segment @from to segment @to of a version of @nr
 * segments: up to a stride by walking, then, if that was not enough and the
 * version has anchors, a jump to the last anchor at or before the target when
 * that is ahead, and a walk from there.
 */
static u32 opaque_seek_steps(u32 from, u32 to, u32 nr)
{
	u32 first = min(to - from, (u32)IO_OPAQUE_SEEK_STRIDE), at = from + first;

	if (at == to || nr <= IO_OPAQUE_SEEK_STRIDE)
		return to - from;
	at = max(at, to / IO_OPAQUE_SEEK_STRIDE * IO_OPAQUE_SEEK_STRIDE);
	return first + to - at;
}

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
		/* A resumed walk begins at the last start, else at the first segment. */
		expect += opaque_seek_steps(step[i].resumes ? step[i - 1].seg : 0, step[i].seg,
					    r->obj[o]->nr);
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
	 * ascending order the way to the first is found through the anchors and
	 * each range after it is a step on, and in descending order every range
	 * starts again and is found through the anchors, a stride and a bit each.
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
 * A version of more segments than a stride has anchors, one per stride, each
 * the byte at which its segment starts; a smaller one has none. They share the
 * allocation of the segments, so putting the version frees them (KASAN would
 * see otherwise).
 */
static void opaque_vector_anchors(struct kunit *test)
{
	static const u32 sizes[] = { 1, 15, 16, 17, 32, 33, 400 };
	struct io_opaque_store *store = opaque_test_big_store(test);
	unsigned int i;
	u32 k;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		struct io_opaque_data *data = opaque_varied_data(test, store, sizes[i]);

		KUNIT_ASSERT_EQ(test, data->nr, sizes[i]);
		if (sizes[i] <= IO_OPAQUE_SEEK_STRIDE) {
			KUNIT_EXPECT_PTR_EQ(test, data->anchor, NULL);
		} else {
			KUNIT_ASSERT_NOT_NULL(test, data->anchor);
			/* Right after the segments, in their allocation. */
			KUNIT_EXPECT_PTR_EQ(test, (void *)data->anchor,
					    (void *)(data->bvec + data->nr));
			for (k = 0; k < DIV_ROUND_UP(sizes[i], IO_OPAQUE_SEEK_STRIDE); k++)
				KUNIT_EXPECT_EQ(test, data->anchor[k],
						opaque_seg_pos(data, k * IO_OPAQUE_SEEK_STRIDE));
		}
		io_opaque_data_put(store, data);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/*
 * Whatever the ranges are, finding where they start is bounded: the most
 * entries a vector holds, alternating between two large objects or going back
 * in one, cost a stride and a bit each, and not the object each. Ranges that
 * follow each other cost a step each, the anchors not being touched.
 */
static void opaque_send_vec_seek_bound(struct kunit *test)
{
	const u32 objs = 400, nr = IORING_OPAQUE_SEND_VEC_MAX;
	struct io_opaque_store *store = opaque_test_big_store(test);
	struct io_opaque_data *obj[2] = {
		opaque_varied_data(test, store, objs), opaque_varied_data(test, store, objs),
	};
	struct io_uring_opaque_send_entry *ent = kunit_kcalloc(test, nr, sizeof(*ent), GFP_KERNEL);
	const u32 last = objs - 1;
	struct io_opaque_req *op;
	u64 walked;
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, ent);
	opaque_test_publish(store, 0, 9, obj[0]);
	opaque_test_publish(store, 1, 7, obj[1]);

	/* Alternating objects, each at its last segment: every range starts the walk again. */
	for (i = 0; i < nr; i++)
		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = io_opaque_handle(i % 2, i % 2 ? 7 : 9),
			.off = opaque_seg_pos(obj[i % 2], last), .len = 1,
		};
	op = opaque_vec_new(test, store, ent, nr, 0);
	walked = atomic_long_read(&io_opaque_vec_stepped);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	walked = atomic_long_read(&io_opaque_vec_stepped) - walked;
	KUNIT_EXPECT_EQ(test, walked, (u64)nr * opaque_seek_steps(0, last, objs));
	KUNIT_EXPECT_LT(test, walked, (u64)nr * 2 * IO_OPAQUE_SEEK_STRIDE);
	opaque_vec_expect_cursor(test, op);
	io_opaque_vec_release(op);

	/* Back and forth in one object: the way back starts again, the way forward resumes. */
	for (i = 0; i < nr; i++)
		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = io_opaque_handle(0, 9),
			.off = i % 2 ? 0 : opaque_seg_pos(obj[0], last), .len = 1,
		};
	op = opaque_vec_new(test, store, ent, nr, 0);
	walked = atomic_long_read(&io_opaque_vec_stepped);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	walked = atomic_long_read(&io_opaque_vec_stepped) - walked;
	KUNIT_EXPECT_EQ(test, walked, (u64)nr / 2 * opaque_seek_steps(0, last, objs));
	opaque_vec_expect_cursor(test, op);
	io_opaque_vec_release(op);

	/* One range on from the last, from the start: a step each, no jump. */
	for (i = 0; i < 300; i++)
		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = io_opaque_handle(1, 7),
			.off = opaque_seg_pos(obj[1], i), .len = 1,
		};
	op = opaque_vec_new(test, store, ent, 300, 0);
	walked = atomic_long_read(&io_opaque_vec_stepped);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	walked = atomic_long_read(&io_opaque_vec_stepped) - walked;
	KUNIT_EXPECT_EQ(test, walked, 299ULL);
	opaque_vec_expect_cursor(test, op);
	io_opaque_vec_release(op);

	for (i = 0; i < ARRAY_SIZE(obj); i++) {
		mutex_lock(&store->tables);
		io_opaque_data_put(store, io_opaque_detach(store, &store->objects[i]));
		mutex_unlock(&store->tables);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/*
 * Counting the segments of the ranges stops once the limit is passed: a range
 * over a whole large object, or one that crosses the limit, counts as many as
 * it takes to know, and the vector fails having pinned and removed nothing.
 */
static void opaque_send_vec_count_cap(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_big_store(test);
	struct io_opaque_data *data = opaque_varied_data(test, store, 400);
	const u64 start = opaque_seg_pos(data, 100);
	struct io_uring_opaque_send_entry ent[2] = {
		{ .handle = io_opaque_handle(0, 9), .off = start,
		  .len = opaque_seg_pos(data, 108) - start },
		{ .handle = io_opaque_handle(0, 9), .off = 0, .len = data->length },
	};
	struct io_opaque_req *op;
	unsigned int last;
	unsigned long limit;
	long counted;

	opaque_test_publish(store, 0, 9, data);
	limit = io_opaque_send_vec_segs;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_vec_segs_restore,
							(void *)limit), 0);
	io_opaque_send_vec_segs = 10;
	for (last = 0; last < 2; last++) {
		u16 flags = last ? IORING_OPAQUE_SEND_LAST : 0;

		/* The whole object, 400 segments, with room for ten: eleven are counted. */
		op = opaque_vec_new(test, store, &ent[1], 1, flags);
		counted = atomic_long_read(&io_opaque_vec_counted);
		mutex_lock(&store->tables);
		KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(op), -EMSGSIZE);
		mutex_unlock(&store->tables);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_vec_counted) - counted, 11L);
		/* Eight fit; the second range crosses the limit after three more. */
		op = opaque_vec_new(test, store, ent, 2, flags);
		counted = atomic_long_read(&io_opaque_vec_counted);
		mutex_lock(&store->tables);
		KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(op), -EMSGSIZE);
		mutex_unlock(&store->tables);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_vec_counted) - counted, 11L);
		KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
		KUNIT_EXPECT_FALSE(test, op->consumed);
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
		KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, data);
	}
	/* At the limit exactly, all of it is counted and the vector goes. */
	io_opaque_send_vec_segs = 8;
	op = opaque_vec_new(test, store, ent, 1, 0);
	opaque_vec_room(test, op, 8);
	counted = atomic_long_read(&io_opaque_vec_counted);
	KUNIT_EXPECT_EQ(test, opaque_vec_admit(op), 0);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_vec_counted) - counted, 8L);
	io_opaque_vec_release(op);
	mutex_lock(&store->tables);
	io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/*
 * A send of one range puts its cursor at the segment that holds it, found as
 * a vector's ranges are, and not by advancing an iterator over the segments
 * before it: the cursor reads the same bytes, starts in the same segment, and
 * the walk to it is bounded.
 */
static void opaque_send_single_seek(struct kunit *test)
{
	static const struct {
		u32 seg, delta, len;
	} cases[] = {
		{ 0, 0, 100 }, { 0, 50, 100 }, { 7, 3, 500 }, { 20, 0, 1 }, { 21, 90, 700 },
		{ 250, 5, 3000 }, { 399, 0, 1 }, { 399, 5, 50 },
	};
	struct io_opaque_store *store = opaque_test_big_store(test);
	struct io_opaque_data *data = opaque_varied_data(test, store, 400);
	u8 *source = kunit_kmalloc(test, data->length, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 4096, GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_copy(data, source);
	opaque_test_publish(store, 0, 9, data);
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		u64 off = opaque_seg_pos(data, cases[i].seg) + cases[i].delta, walked;
		struct io_opaque_req *op = kunit_kzalloc(test, sizeof(*op), GFP_KERNEL);

		KUNIT_ASSERT_NOT_NULL(test, op);
		op->store = store;
		op->handle = io_opaque_handle(0, 9);
		op->offset = off;
		op->length = min(cases[i].len, (u32)(data->length - off));
		walked = atomic_long_read(&io_opaque_vec_stepped);
		mutex_lock(&store->tables);
		KUNIT_ASSERT_EQ(test, io_opaque_send_acquire(op), 0);
		mutex_unlock(&store->tables);
		walked = atomic_long_read(&io_opaque_vec_stepped) - walked;
		KUNIT_EXPECT_EQ(test, walked, (u64)opaque_seek_steps(0, cases[i].seg, 400));
		KUNIT_EXPECT_PTR_EQ(test, op->iter.bvec, data->bvec + cases[i].seg);
		KUNIT_EXPECT_EQ(test, op->iter.iov_offset, (size_t)cases[i].delta);
		KUNIT_EXPECT_EQ(test, iov_iter_count(&op->iter), (size_t)op->length);
		KUNIT_EXPECT_EQ(test, copy_from_iter(got, op->length, &op->iter),
				(size_t)op->length);
		KUNIT_EXPECT_MEMEQ(test, got, source + off, op->length);
		io_opaque_data_put(store, op->data);
	}
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	mutex_lock(&store->tables);
	io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
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

/*
 * A policy change marks the candidates for automatic compaction, in chunks of
 * the table of handles with tables dropped between them. What it leaves is what
 * one walk would, whatever else happens in between: an object that joins the
 * candidates meanwhile is not added twice, a newer policy that was installed
 * between two chunks is not overwritten by the rest of the older walk, and a
 * store that has stopped ends it.
 */
#define OPAQUE_POLICY_SLOTS	12

static struct {
	unsigned int calls, candidates[4], at;
	bool tables_free;
	/* What to do between chunks. */
	enum { POLICY_NOTHING, POLICY_OVERTAKE, POLICY_STOP, POLICY_JOIN } what;
	struct io_uring_opaque_policy *newer;
	bool inside;
} opaque_policy_hook;

static unsigned int opaque_policy_count(struct io_opaque_store *store)
{
	struct io_opaque_slot *slot;
	unsigned int n = 0;

	list_for_each_entry(slot, &store->candidates, candidate)
		n++;
	return n;
}

static void opaque_policy_between(struct io_opaque_store *store)
{
	struct io_opaque_slot *slot;

	if (opaque_policy_hook.inside)
		return;
	opaque_policy_hook.inside = true;
	opaque_policy_hook.tables_free &= !mutex_is_locked(&store->tables);
	if (opaque_policy_hook.at < ARRAY_SIZE(opaque_policy_hook.candidates))
		opaque_policy_hook.candidates[opaque_policy_hook.at] = opaque_policy_count(store);
	opaque_policy_hook.at++;
	opaque_policy_hook.calls++;
	switch (opaque_policy_hook.what) {
	case POLICY_OVERTAKE:
		/* The first chunk is behind us: a newer policy walks all of it. */
		if (opaque_policy_hook.calls == 1)
			io_opaque_apply_policy(store, opaque_policy_hook.newer);
		break;
	case POLICY_STOP:
		mutex_lock(&store->tables);
		store->dead = 1;
		mutex_unlock(&store->tables);
		break;
	case POLICY_JOIN:
		/* The publish path adds an object that the walk has not reached yet. */
		slot = &store->objects[OPAQUE_POLICY_SLOTS - 1];
		mutex_lock(&store->tables);
		if (list_empty(&slot->candidate))
			list_add_tail(&slot->candidate, &store->candidates);
		mutex_unlock(&store->tables);
		break;
	default:
		break;
	}
	opaque_policy_hook.inside = false;
}

static void opaque_policy_restore(void *arg)
{
	io_opaque_policy_chunk = IO_OPAQUE_POLICY_CHUNK;
	io_opaque_policy_between = NULL;
}

/* The slots that a policy marks, in order, as a bit mask. */
static u32 opaque_policy_mask(struct io_opaque_store *store)
{
	struct io_opaque_slot *slot;
	u32 mask = 0;

	list_for_each_entry(slot, &store->candidates, candidate)
		mask |= 1U << (slot - store->objects);
	return mask;
}

static void opaque_policy_chunks(struct kunit *test)
{
	/* Slots 0, 2, 3, 5, 8, 9 and 11 hold a version that compaction could improve. */
	const u32 eligible = BIT(0) | BIT(2) | BIT(3) | BIT(5) | BIT(8) | BIT(9) | BIT(11);
	struct io_uring_opaque_policy on = {
		.flags = IORING_OPAQUE_POLICY_F_AUTO_COMPACT, .bytes_per_second = SZ_1M,
		.burst_bytes = SZ_64K, .max_temporary_bytes = SZ_64K,
	};
	struct io_uring_opaque_policy off = { };
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data[OPAQUE_POLICY_SLOTS] = {};
	u32 i, seq;

	store->config.max_objects = OPAQUE_POLICY_SLOTS;
	store->config.hard_limit = SZ_1M;
	store->objects = kunit_kcalloc(test, OPAQUE_POLICY_SLOTS, sizeof(*store->objects),
				       GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, store->objects);
	INIT_LIST_HEAD(&store->copies);
	INIT_DELAYED_WORK(&store->compact_work, opaque_test_noop_work);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_cancel_work,
							&store->compact_work), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_policy_restore, NULL), 0);
	for (i = 0; i < OPAQUE_POLICY_SLOTS; i++)
		INIT_LIST_HEAD(&store->objects[i].candidate);
	for (i = 0; i < OPAQUE_POLICY_SLOTS; i++) {
		if (i == 6 || i == 7)
			continue;	/* nothing published */
		data[i] = io_opaque_data_alloc();
		KUNIT_ASSERT_NOT_NULL(test, data[i]);
		data[i]->dense = i == 1 || i == 10;
		data[i]->forward_only = i == 4;
		store->objects[i].data = data[i];
	}
	memset(&opaque_policy_hook, 0, sizeof(opaque_policy_hook));
	opaque_policy_hook.newer = &off;
	opaque_policy_hook.tables_free = true;

	/* In one go: every candidate, and the policy and its counter. */
	KUNIT_EXPECT_EQ(test, io_opaque_apply_policy(store, &on), 0);
	KUNIT_EXPECT_EQ(test, opaque_policy_mask(store), eligible);
	KUNIT_EXPECT_EQ(test, store->policy.flags, (u32)IORING_OPAQUE_POLICY_F_AUTO_COMPACT);
	KUNIT_EXPECT_EQ(test, store->tokens, (u64)SZ_64K);
	KUNIT_EXPECT_EQ(test, store->policy_seq, 1U);
	KUNIT_EXPECT_EQ(test, opaque_policy_hook.calls, 0U);
	/* Again: nothing is added twice. */
	KUNIT_EXPECT_EQ(test, io_opaque_apply_policy(store, &on), 0);
	KUNIT_EXPECT_EQ(test, opaque_policy_count(store), (unsigned int)hweight32(eligible));
	KUNIT_EXPECT_EQ(test, io_opaque_apply_policy(store, &off), 0);
	KUNIT_EXPECT_EQ(test, opaque_policy_mask(store), 0U);
	KUNIT_EXPECT_EQ(test, store->policy.flags, 0U);

	/* In chunks of three: the same, with tables free between, and the walk seen half done. */
	io_opaque_policy_chunk = 3;
	io_opaque_policy_between = opaque_policy_between;
	KUNIT_EXPECT_EQ(test, io_opaque_apply_policy(store, &on), 0);
	KUNIT_EXPECT_EQ(test, opaque_policy_mask(store), eligible);
	KUNIT_EXPECT_EQ(test, opaque_policy_hook.calls, 3U);
	KUNIT_EXPECT_TRUE(test, opaque_policy_hook.tables_free);
	KUNIT_EXPECT_EQ(test, opaque_policy_hook.candidates[0], 2U);	/* slots 0 and 2 */
	KUNIT_EXPECT_EQ(test, opaque_policy_hook.candidates[1], 4U);	/* and 3 and 5 */
	KUNIT_EXPECT_EQ(test, opaque_policy_hook.candidates[2], 5U);	/* and 8 */
	KUNIT_EXPECT_EQ(test, io_opaque_apply_policy(store, &off), 0);
	KUNIT_EXPECT_EQ(test, opaque_policy_mask(store), 0U);

	/* An object that joins the candidates between chunks is on the list once. */
	memset(&opaque_policy_hook, 0, sizeof(opaque_policy_hook));
	opaque_policy_hook.tables_free = true;
	opaque_policy_hook.what = POLICY_JOIN;
	KUNIT_EXPECT_EQ(test, io_opaque_apply_policy(store, &on), 0);
	KUNIT_EXPECT_EQ(test, opaque_policy_mask(store), eligible);
	KUNIT_EXPECT_EQ(test, opaque_policy_count(store), (unsigned int)hweight32(eligible));
	KUNIT_EXPECT_EQ(test, io_opaque_apply_policy(store, &off), 0);

	/*
	 * A newer policy that comes in after the first chunk, and is off: it clears
	 * every candidate and the older walk, on, gives up without adding the rest.
	 */
	memset(&opaque_policy_hook, 0, sizeof(opaque_policy_hook));
	opaque_policy_hook.tables_free = true;
	opaque_policy_hook.what = POLICY_OVERTAKE;
	opaque_policy_hook.newer = &off;
	seq = store->policy_seq;
	KUNIT_EXPECT_EQ(test, io_opaque_apply_policy(store, &on), 0);
	KUNIT_EXPECT_EQ(test, store->policy_seq, seq + 2);
	KUNIT_EXPECT_EQ(test, store->policy.flags, 0U);
	KUNIT_EXPECT_EQ(test, opaque_policy_mask(store), 0U);
	KUNIT_EXPECT_EQ(test, opaque_policy_hook.calls, 1U);
	/* And the other way round: the newer one is on, and has every candidate once. */
	memset(&opaque_policy_hook, 0, sizeof(opaque_policy_hook));
	opaque_policy_hook.tables_free = true;
	opaque_policy_hook.what = POLICY_OVERTAKE;
	opaque_policy_hook.newer = &on;
	KUNIT_EXPECT_EQ(test, io_opaque_apply_policy(store, &off), 0);
	KUNIT_EXPECT_EQ(test, store->policy.flags, (u32)IORING_OPAQUE_POLICY_F_AUTO_COMPACT);
	KUNIT_EXPECT_EQ(test, opaque_policy_mask(store), eligible);
	KUNIT_EXPECT_EQ(test, opaque_policy_count(store), (unsigned int)hweight32(eligible));
	KUNIT_EXPECT_EQ(test, io_opaque_apply_policy(store, &off), 0);

	/* A store that stops between chunks ends the change, with what was walked left. */
	memset(&opaque_policy_hook, 0, sizeof(opaque_policy_hook));
	opaque_policy_hook.tables_free = true;
	opaque_policy_hook.what = POLICY_STOP;
	KUNIT_EXPECT_EQ(test, io_opaque_apply_policy(store, &on), -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, opaque_policy_hook.calls, 1U);
	KUNIT_EXPECT_EQ(test, opaque_policy_mask(store), (u32)(BIT(0) | BIT(2)));
	/* And one that has stopped takes no change at all. */
	KUNIT_EXPECT_EQ(test, io_opaque_apply_policy(store, &off), -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, store->policy.flags, (u32)IORING_OPAQUE_POLICY_F_AUTO_COMPACT);

	for (i = 0; i < OPAQUE_POLICY_SLOTS; i++) {
		list_del_init(&store->objects[i].candidate);
		kfree(data[i]);
	}
}

/*
 * Freeing extents gives back what their backings held in one step and looks at
 * the waiters for budget once, whatever the number: a version of forty extents,
 * and two that share a backing, freed in two lists, of which the first gives
 * nothing back and still looks. Putting a reference that is not the last looks
 * at nobody, and neither does an empty list.
 */
static void opaque_extents_free_wakes_once(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct page *page = alloc_page(GFP_KERNEL);
	struct io_opaque_extent *left, *right;
	LIST_HEAD(both);
	LIST_HEAD(first);
	LIST_HEAD(second);
	long wakes = atomic_long_read(&io_opaque_budget_wakes);

	KUNIT_ASSERT_NOT_NULL(test, page);
	refcount_inc(&data->refs);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_budget_wakes) - wakes, 0L);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 40);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_budget_wakes) - wakes, 1L);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);

	left = io_opaque_extent_new(store, page, 0, 128, 0, false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(left));
	list_add(&left->list, &both);
	right = io_opaque_extent_split(store, left, 64);
	KUNIT_ASSERT_FALSE(test, IS_ERR(right));
	list_move(&left->list, &first);
	list_move(&right->list, &second);
	wakes = atomic_long_read(&io_opaque_budget_wakes);
	io_opaque_extents_free(store, &first);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_budget_wakes) - wakes, 1L);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE);
	io_opaque_extents_free(store, &second);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_budget_wakes) - wakes, 2L);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	io_opaque_extents_free(store, &first);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_budget_wakes) - wakes, 2L);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/* The security hook on a socket that is sent on directly: it refuses, or says EAGAIN, once. */
static int opaque_test_security(struct socket *sock, const struct io_opaque_req *op,
				unsigned int issue_flags)
{
	return opaque_refuse_now(sock->sk);
}

/*
 * What a refusal from the security hook, or its EAGAIN, does to an issue is the
 * same on a socket that is sent on directly, where the hook is asked once, and
 * on one that goes through sock_sendmsg(), where it is asked by the first call:
 * both have taken the references and started the frame by then. A refusal
 * completes the send with its error and LAST consumed, and fails the frame
 * parked behind it; EAGAIN returns with the place at the head, the references
 * and the claim on LAST kept, and the next issue sends the frame. Only what
 * happens to the references afterwards differs: the direct path gives them back
 * itself, the other leaves them to cleanup, which here is the test.
 */
static void opaque_security_run(struct kunit *test, bool direct, int err, bool last)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	const u32 off = 100, len = 4000;
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, len, GFP_KERNEL);
	struct io_opaque_req *head, *follower;
	struct io_opaque_tx *tx;
	struct sock *sk;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	opaque_wire_init(test, w);
	sk = w->tx->sk;
	opaque_wire_sndbuf(sk, SZ_1M);
	head = opaque_wire_req(test, w, store, 0, last ? IORING_OPAQUE_SEND_LAST : 0, off, len);
	follower = opaque_wire_req(test, w, store, 1, 0, 200, 300);
	memset(&opaque_refuse, 0, sizeof(opaque_refuse));
	opaque_refuse.err = err;
	opaque_refuse.armed = true;
	opaque_refuse.follower = follower;
	if (direct) {
		kunit_activate_static_stub(test, io_opaque_send_security, opaque_test_security);
	} else {
		opaque_test_proto = tcp_prot;
		opaque_test_proto.sendmsg = opaque_test_sendmsg;
		ret = kunit_add_action_or_reset(test, opaque_test_proto_restore, sk);
		KUNIT_ASSERT_EQ(test, ret, 0);
		lock_sock(sk);
		sk->sk_prot = &opaque_test_proto;
		release_sock(sk);
	}
	KUNIT_ASSERT_EQ(test, io_opaque_send_direct(sk), direct);

	mutex_lock(&w->ctx->uring_lock);
	ret = io_opaque_send(&w->req[0], IO_URING_F_NONBLOCK);
	mutex_unlock(&w->ctx->uring_lock);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_FALSE(test, opaque_refuse.armed);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&store->tables));
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
	KUNIT_EXPECT_EQ(test, head->consumed, last);
	/* LAST had taken the handle by then: a frame that comes after finds it gone. */
	KUNIT_EXPECT_EQ(test, opaque_refuse.joined, last ? -ESTALE : IO_OPAQUE_TX_WAIT);
	tx = last ? NULL : follower->tx;

	if (err == -EAGAIN) {
		KUNIT_ASSERT_EQ(test, ret, -EAGAIN);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 0);
		KUNIT_ASSERT_NOT_NULL(test, head->tx);
		tx = head->tx;
		KUNIT_EXPECT_PTR_EQ(test,
				    list_first_entry(&tx->requests, struct io_opaque_req, send),
				    head);
		KUNIT_EXPECT_TRUE(test, head->tx_started);
		KUNIT_EXPECT_PTR_EQ(test, head->data, data);
		KUNIT_EXPECT_FALSE(test, w->req[0].flags & REQ_F_FAIL);
		if (!last)
			KUNIT_EXPECT_EQ(test, follower->phase, IO_OPAQUE_SEND_WAIT);
		/* The next issue sends the frame, and a frame behind it is handed the queue. */
		ret = 0;
		mutex_lock(&w->ctx->uring_lock);
		ret = io_opaque_send(&w->req[0], IO_URING_F_NONBLOCK);
		mutex_unlock(&w->ctx->uring_lock);
		KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, (s32)len);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.flags & IORING_CQE_F_OPAQUE_CONSUMED,
				last ? IORING_CQE_F_OPAQUE_CONSUMED : 0);
		KUNIT_EXPECT_PTR_EQ(test, head->tx, NULL);
		opaque_wire_read(test, w, got, 0, len);
		KUNIT_EXPECT_MEMEQ(test, got, source + off, len);
		if (!last) {
			KUNIT_EXPECT_EQ(test, follower->phase, IO_OPAQUE_QUEUED);
			KUNIT_EXPECT_FALSE(test, follower->canceled);
			KUNIT_EXPECT_PTR_EQ(test, mpscq_pop(&w->ctx->work_list, &w->ctx->work_head),
					    &w->req[1].io_task_work.node);
		}
	} else {
		KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, err);
		KUNIT_EXPECT_TRUE(test, w->req[0].flags & REQ_F_FAIL);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.flags & IORING_CQE_F_OPAQUE_CONSUMED,
				last ? IORING_CQE_F_OPAQUE_CONSUMED : 0);
		KUNIT_EXPECT_PTR_EQ(test, head->tx, NULL);
		KUNIT_EXPECT_EQ(test, head->progress, 0ULL);
		if (!last) {
			/* The head had started, so its refusal fails the frame behind it. */
			KUNIT_EXPECT_TRUE(test, tx->failed);
			KUNIT_EXPECT_TRUE(test, follower->canceled);
			KUNIT_EXPECT_EQ(test, follower->phase, IO_OPAQUE_QUEUED);
			KUNIT_EXPECT_EQ(test, follower->result, -ECANCELED);
			KUNIT_EXPECT_PTR_EQ(test, mpscq_pop(&w->ctx->work_list, &w->ctx->work_head),
					    &w->req[1].io_task_work.node);
		}
	}
	/* The references: given back by the direct path, left to cleanup by the other. */
	KUNIT_EXPECT_PTR_EQ(test, head->data, direct ? NULL : data);
	if (!direct)
		io_opaque_data_put(store, head->data);
	head->data = NULL;
	if (!last) {
		KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(follower), NULL);
		KUNIT_EXPECT_FALSE(test, tx->failed);
		io_opaque_data_put(store, follower->data);
		follower->data = NULL;
		mutex_lock(&store->tables);
		io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
		mutex_unlock(&store->tables);
	}
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
	if (direct)
		kunit_deactivate_static_stub(test, io_opaque_send_security);
}

static void opaque_send_security_refusal(struct kunit *test)
{
	unsigned int path, err, last;

	for (path = 0; path < 2; path++)
		for (err = 0; err < 2; err++)
			for (last = 0; last < 2; last++)
				opaque_security_run(test, path, err ? -EAGAIN : -EACCES, last);
}

/* Issue request @i of the fixture, as io-wq does or inline under the ring lock. */
static int opaque_wire_issue(struct opaque_wire *w, int i, bool unlocked)
{
	int ret;

	if (unlocked)
		return io_opaque_send(&w->req[i], IO_URING_F_UNLOCKED);
	mutex_lock(&w->ctx->uring_lock);
	ret = io_opaque_send(&w->req[i], IO_URING_F_NONBLOCK);
	mutex_unlock(&w->ctx->uring_lock);
	return ret;
}

/* A vectored send as request @i of the fixture, ready to be issued. */
static struct io_opaque_req *opaque_wire_vec(struct kunit *test, struct opaque_wire *w,
					     struct io_opaque_store *store, int i,
					     const struct io_uring_opaque_send_entry *ent, u32 nr,
					     u16 flags)
{
	struct io_opaque_req *op = opaque_vec_new(test, store, ent, nr, flags);

	op->req = &w->req[i];
	op->op = U16_MAX;
	INIT_LIST_HEAD(&op->wait);
	INIT_LIST_HEAD(&op->cancel);
	INIT_LIST_HEAD(&op->send);
	io_opaque_send_resolve(op);
	w->req[i].async_data = op;
	w->req[i].flags = 0;
	return op;
}

/* What the window of an admission that has to grow its block saw, and the frame it lets in. */
static struct {
	struct sock *sk;
	struct io_opaque_req *follower;
	bool owned;
	int joined;
} opaque_admit_hook;

static void opaque_admit_between(struct kunit *test, struct io_opaque_req *op)
{
	opaque_admit_hook.owned = sock_owned_by_user_nocheck(opaque_admit_hook.sk);
	if (opaque_admit_hook.follower)
		opaque_admit_hook.joined = io_opaque_tx_join(opaque_admit_hook.follower,
							     opaque_admit_hook.sk);
}

/*
 * A head that cannot take its references while it owns the socket: a stale
 * handle, LAST or not, and a vector whose block cannot grow, which fails with
 * the socket owned and a frame that joined meanwhile parked behind it. It
 * leaves with the error, which is not a completion, nothing sent, nothing
 * consumed and nothing held, and releases the socket and does not fail the
 * queue; the frame behind it is promoted with its references and sends them.
 */
static void opaque_send_locked_admit_fails(struct kunit *test)
{
	struct io_uring_opaque_send_entry ent[2];
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 300, GFP_KERNEL);
	unsigned int mode, last;
	struct sock *sk;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	opaque_wire_init(test, w);
	sk = w->tx->sk;
	opaque_wire_sndbuf(sk, SZ_1M);
	for (mode = 0; mode < 2; mode++) {
		bool unlocked = !mode;

		for (last = 0; last < 2; last++) {
			u16 flags = last ? IORING_OPAQUE_SEND_LAST : 0;
			struct io_opaque_req *head, *follower;
			long calls = atomic_long_read(&io_opaque_send_locked_calls);
			int ret;

			/* A handle that holds nothing: ESTALE, with nobody and nothing involved. */
			head = opaque_wire_req(test, w, store, 0, flags, 100, 1000);
			head->handle = io_opaque_handle(1, 3);
			memset(&w->req[0].cqe, 0, sizeof(w->req[0].cqe));
			ret = opaque_wire_issue(w, 0, unlocked);
			lockdep_assert_none_held_once();
			KUNIT_EXPECT_EQ(test, ret, -ESTALE);
			KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 0);
			KUNIT_EXPECT_FALSE(test, head->consumed);
			KUNIT_EXPECT_PTR_EQ(test, head->tx, NULL);
			KUNIT_EXPECT_PTR_EQ(test, head->data, NULL);
			KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
			KUNIT_EXPECT_FALSE(test, mutex_is_locked(&store->tables));
			KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
			KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_send_locked_calls),
					calls);

			/* A vector whose block cannot grow, with a frame that joins as it fails. */
			opaque_grow_entries(ent, io_opaque_handle(0, 9));
			head = opaque_wire_vec(test, w, store, 0, ent, ARRAY_SIZE(ent), flags);
			follower = opaque_wire_req(test, w, store, 1, 0, 200, 300);
			memset(&opaque_admit_hook, 0, sizeof(opaque_admit_hook));
			opaque_admit_hook.sk = sk;
			opaque_admit_hook.follower = follower;
			opaque_window_open(test, opaque_admit_between);
			opaque_window.fail = -ENOMEM;
			ret = opaque_wire_issue(w, 0, unlocked);
			opaque_window_close(test);
			lockdep_assert_none_held_once();
			KUNIT_EXPECT_EQ(test, ret, -ENOMEM);
			/* It was the owner of the socket that needed the block, and let it go. */
			KUNIT_EXPECT_TRUE(test, opaque_admit_hook.owned);
			KUNIT_EXPECT_EQ(test, opaque_window.calls, 1U);
			KUNIT_EXPECT_FALSE(test, mutex_is_locked(&store->tables));
			KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
			KUNIT_EXPECT_FALSE(test, head->consumed);
			KUNIT_EXPECT_EQ(test, head->vec->pinned, 0U);
			KUNIT_EXPECT_PTR_EQ(test, head->tx, NULL);
			KUNIT_EXPECT_PTR_EQ(test, store->objects[0].data, data);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_send_locked_calls),
					calls);
			/* The frame that came in was parked with its pin, and is promoted whole. */
			KUNIT_EXPECT_EQ(test, opaque_admit_hook.joined, IO_OPAQUE_TX_WAIT);
			KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
			KUNIT_EXPECT_EQ(test, follower->phase, IO_OPAQUE_QUEUED);
			KUNIT_EXPECT_FALSE(test, follower->canceled);
			KUNIT_ASSERT_NOT_NULL(test, follower->tx);
			KUNIT_EXPECT_FALSE(test, follower->tx->failed);
			KUNIT_EXPECT_PTR_EQ(test, mpscq_pop(&w->ctx->work_list, &w->ctx->work_head),
					    &w->req[1].io_task_work.node);
			/* It resumes as the head, and sends what it holds. */
			follower->phase = IO_OPAQUE_IDLE;
			ret = opaque_wire_issue(w, 1, unlocked);
			KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
			KUNIT_EXPECT_EQ(test, w->req[1].cqe.res, 300);
			KUNIT_EXPECT_PTR_EQ(test, follower->data, NULL);
			KUNIT_EXPECT_PTR_EQ(test, follower->tx, NULL);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_send_locked_calls),
					calls + 1);
			opaque_wire_read(test, w, got, 0, 300);
			KUNIT_EXPECT_MEMEQ(test, got, source + 200, 300);
			KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
			KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
		}
	}
	mutex_lock(&store->tables);
	io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

/*
 * The proto of the socket is taken over while a frame is sent, as a ULP or a
 * socket map would do it: before the socket is owned, between the first check
 * and the second, or in the middle of the frame, after a part, when TCP may
 * have given the socket up. The parts sent by then went with tcp_sendmsg_locked(),
 * the rest go through the proto's own sendmsg, once the socket has been
 * released, and the frame arrives whole. A send that did not own the socket
 * when it took its references still took them where the ordinary path takes
 * them, and the references it took stay with it until cleanup.
 */
static struct {
	unsigned int calls, swap_at;
} opaque_direct;

static bool opaque_test_direct(const struct sock *sk)
{
	struct kunit *test = kunit_get_current_test();
	bool ret;

	if (++opaque_direct.calls == opaque_direct.swap_at)
		((struct sock *)sk)->sk_prot = &opaque_test_proto;
	kunit_deactivate_static_stub(test, io_opaque_send_direct);
	ret = io_opaque_send_direct(sk);
	kunit_activate_static_stub(test, io_opaque_send_direct, opaque_test_direct);
	return ret;
}

static void opaque_direct_switch_run(struct kunit *test, unsigned int swap_at)
{
	const u32 prefix = 24, suffix = 8, body = 3200, total = prefix + body + suffix;
	struct io_uring_opaque_send_entry ent[2];
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *frame = kunit_kmalloc(test, prefix + suffix, GFP_KERNEL);
	u8 *want = kunit_kmalloc(test, total, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, total, GFP_KERNEL);
	long calls = atomic_long_read(&io_opaque_send_locked_calls);
	/* The parts that went with the socket owned: none, the prefix, it and the body, all. */
	const unsigned int locked = swap_at == 2 ? 0 : swap_at == 3 ? 1 : swap_at == 4 ? 2 : 3;
	struct io_opaque_req *op;
	struct sock *sk;
	unsigned int i;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, frame);
	KUNIT_ASSERT_NOT_NULL(test, want);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	opaque_wire_init(test, w);
	sk = w->tx->sk;
	opaque_wire_sndbuf(sk, SZ_1M);
	for (i = 0; i < prefix + suffix; i++)
		frame[i] = 0xc0 + i;
	opaque_grow_entries(ent, io_opaque_handle(0, 9));
	memcpy(want, frame, prefix);
	memcpy(want + prefix, source + ent[0].off, ent[0].len);
	memcpy(want + prefix + ent[0].len, source + ent[1].off, ent[1].len);
	memcpy(want + prefix + body, frame + prefix, suffix);
	/* A vector whose block must grow, which shows where the references are taken. */
	op = opaque_wire_vec(test, w, store, 0, ent, ARRAY_SIZE(ent), 0);
	op->framing = frame;
	op->prefix_length = prefix;
	op->suffix_length = suffix;
	op->total_length = total;
	op->resolved = true;
	opaque_test_proto = tcp_prot;
	opaque_test_proto.sendmsg = opaque_test_sendmsg;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_proto_restore, sk), 0);
	memset(&opaque_admit_hook, 0, sizeof(opaque_admit_hook));
	opaque_admit_hook.sk = sk;
	memset(&opaque_refuse, 0, sizeof(opaque_refuse));
	atomic_set(&opaque_test_proto_calls, 0);
	opaque_direct.calls = 0;
	opaque_direct.swap_at = swap_at;
	if (swap_at)
		kunit_activate_static_stub(test, io_opaque_send_direct, opaque_test_direct);
	opaque_window_open(test, opaque_admit_between);

	ret = opaque_wire_issue(w, 0, true);
	opaque_window_close(test);
	if (swap_at)
		kunit_deactivate_static_stub(test, io_opaque_send_direct);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
	KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, (s32)total);
	KUNIT_EXPECT_EQ(test, opaque_window.calls, 1U);
	/* The socket was owned when the block grew if the decision came out as direct. */
	KUNIT_EXPECT_EQ(test, opaque_admit_hook.owned, swap_at != 2);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_send_locked_calls) - calls, (long)locked);
	KUNIT_EXPECT_EQ(test, atomic_read(&opaque_test_proto_calls), (int)(3 - locked));
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
	KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	opaque_wire_read(test, w, got, 0, total);
	KUNIT_EXPECT_MEMEQ(test, got, want, total);
	/* Given back by a send that was direct to the end, else left to cleanup. */
	KUNIT_EXPECT_EQ(test, op->vec->pinned, swap_at && locked < 3 ? ARRAY_SIZE(ent) : 0U);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), swap_at && locked < 3 ? 2 : 1);
	if (swap_at && locked < 3)
		io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	mutex_lock(&store->tables);
	io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_read(&store->extents), 0);
}

static void opaque_send_direct_switch(struct kunit *test)
{
	/* Never; at the check under the lock; after the first part; after the second. */
	static const unsigned int swaps[] = { 0, 2, 3, 4 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(swaps); i++)
		opaque_direct_switch_run(test, swaps[i]);
}

/*
 * A blocking send on the direct path sleeps in TCP when the buffers are full,
 * which gives the socket up while it waits: with the ring lock not held, as
 * io-wq issues it. A peer that starts to read a while later lets it go on,
 * and it completes with the whole frame, from the one call that went to
 * sleep.
 */
static struct {
	struct delayed_work work;
	struct opaque_wire *w;
	u8 *got;
	u32 total;
	bool ring_free, unowned;
	struct completion done;
} opaque_reader;

static void opaque_reader_fn(struct work_struct *work)
{
	u32 done = 0;

	/* The sender went to sleep in TCP long ago: the ring lock is free, the socket unowned. */
	opaque_reader.ring_free = !mutex_is_locked(&opaque_reader.w->ctx->uring_lock);
	opaque_reader.unowned = !sock_owned_by_user_nocheck(opaque_reader.w->tx->sk);
	while (done < opaque_reader.total) {
		struct msghdr msg = {};
		struct kvec vec = { .iov_base = opaque_reader.got + done,
				    .iov_len = opaque_reader.total - done };
		int ret = kernel_recvmsg(opaque_reader.w->rx, &msg, &vec, 1, vec.iov_len, 0);

		if (ret <= 0)
			break;
		done += ret;
	}
	opaque_reader.total = done;
	complete(&opaque_reader.done);
}

static void opaque_reader_cancel(void *arg)
{
	cancel_delayed_work_sync(&opaque_reader.work);
}

static void opaque_send_locked_blocking(struct kunit *test)
{
	struct io_uring_opaque_send_entry ent[6];
	const u32 total = 6 * 80000;
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, total, GFP_KERNEL);
	long calls = atomic_long_read(&io_opaque_send_locked_calls);
	struct io_opaque_req *op;
	ktime_t start;
	unsigned int i;
	s64 ms;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	opaque_wire_init(test, w);
	/* Far more than the buffers on both sides will take before a read. */
	opaque_wire_sndbuf(w->tx->sk, SZ_32K);
	for (i = 0; i < ARRAY_SIZE(ent); i++)
		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = io_opaque_handle(0, 9), .off = 0, .len = 80000,
		};
	op = opaque_wire_vec(test, w, store, 0, ent, ARRAY_SIZE(ent), 0);
	memset(&opaque_reader, 0, sizeof(opaque_reader));
	opaque_reader.w = w;
	opaque_reader.got = got;
	opaque_reader.total = total;
	init_completion(&opaque_reader.done);
	INIT_DELAYED_WORK(&opaque_reader.work, opaque_reader_fn);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_reader_cancel, NULL), 0);
	schedule_delayed_work(&opaque_reader.work, msecs_to_jiffies(150));

	start = ktime_get();
	ret = opaque_wire_issue(w, 0, true);
	ms = ktime_ms_delta(ktime_get(), start);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
	KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, (s32)total);
	/* It could not have finished before the peer began to read. */
	KUNIT_EXPECT_GE(test, ms, 100LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_send_locked_calls) - calls, 1L);
	KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(w->tx->sk));
	KUNIT_ASSERT_GT(test, wait_for_completion_timeout(&opaque_reader.done, 10 * HZ), 0UL);
	KUNIT_EXPECT_TRUE(test, opaque_reader.ring_free);
	KUNIT_EXPECT_TRUE(test, opaque_reader.unowned);
	KUNIT_ASSERT_EQ(test, opaque_reader.total, total);
	for (i = 0; i < ARRAY_SIZE(ent); i++)
		KUNIT_EXPECT_MEMEQ(test, got + i * 80000, source, 80000);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	mutex_lock(&store->tables);
	io_opaque_data_put(store, io_opaque_detach(store, &store->objects[0]));
	mutex_unlock(&store->tables);
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
	KUNIT_CASE(opaque_send_head_admit_fails),
	KUNIT_CASE(opaque_send_head_races_follower),
	KUNIT_CASE(opaque_send_direct_predicate),
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
	KUNIT_CASE(opaque_send_locked_path),
	KUNIT_CASE(opaque_send_locked_eagain),
	KUNIT_CASE(opaque_send_locked_short),
	KUNIT_CASE(opaque_send_locked_error),
	KUNIT_CASE(opaque_send_locked_last_versions),
	KUNIT_CASE(opaque_send_fallback),
	KUNIT_CASE(opaque_send_security_refusal),
	KUNIT_CASE(opaque_send_locked_admit_fails),
	KUNIT_CASE(opaque_send_direct_switch),
	KUNIT_CASE(opaque_send_locked_blocking),
	KUNIT_CASE(opaque_attach_lock_order),
	KUNIT_CASE(opaque_send_vec_check),
	KUNIT_CASE(opaque_send_vec_acquire),
	KUNIT_CASE(opaque_send_vec_pin_once),
	KUNIT_CASE(opaque_send_vec_iter),
	KUNIT_CASE(opaque_send_vec_loopback),
	KUNIT_CASE(opaque_send_vec_admit_fail),
	KUNIT_CASE(opaque_send_vec_resume),
	KUNIT_CASE(opaque_vector_anchors),
	KUNIT_CASE(opaque_send_vec_seek_bound),
	KUNIT_CASE(opaque_send_vec_count_cap),
	KUNIT_CASE(opaque_send_single_seek),
	KUNIT_CASE(opaque_send_vec_grow_retry),
	KUNIT_CASE(opaque_send_vec_build_pinned),
	KUNIT_CASE(opaque_send_vec_prep),
	KUNIT_CASE(opaque_policy_chunks),
	KUNIT_CASE(opaque_extents_free_wakes_once),
	KUNIT_CASE(opaque_params_sysfs),
	{}
};

static struct kunit_suite opaque_suite = {
	.name = "io_uring-opaque",
	.test_cases = opaque_cases,
};

kunit_test_suite(opaque_suite);
