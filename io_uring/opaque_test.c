// SPDX-License-Identifier: GPL-2.0
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <linux/gcd.h>
#include <linux/highmem.h>
#include <linux/inet.h>
#include <linux/interrupt.h>
#include <linux/kobject.h>
#include <linux/kstrtox.h>
#include <linux/kthread.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/prandom.h>
#include <linux/random.h>
#include <linux/rtnetlink.h>
#include <linux/sched/user.h>
#include <linux/skbuff.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>
#include <net/hotdata.h>
#include <net/tcp.h>

#include "cancel.h"
#include "io_uring.h"
#include "mpscq.h"
#include "notif.h"
#include "opaque_internal.h"
#if IS_ENABLED(CONFIG_PAGE_POOL)
#include <net/page_pool/helpers.h>
#endif
#ifdef CONFIG_IO_URING_ZCRX_KUNIT_TEST
#include "zcrx.h"
#endif

static void opaque_test_txs_free(void *store)
{
	io_opaque_txs_free(store);
}

/*
 * The tables of handles of a fixture are reached through the functions below, so that a test
 * says which slot it means and not where the slots are kept.
 */

/* Slot @i of the objects, in a test that is alone with the table. */
static struct io_opaque_slot *opaque_test_slot(struct io_opaque_store *store, u32 i)
{
	return &store->objects[i];
}

/* Where slot @slot is in the table of objects. */
static u32 opaque_test_slot_index(struct io_opaque_store *store, const struct io_opaque_slot *slot)
{
	return slot - store->objects;
}

/* Slot @i of the streams, in a test that is alone with the table. */
static struct io_opaque_stream_slot *opaque_test_stream_slot(struct io_opaque_store *store, u32 i)
{
	return &store->streams[i];
}

/* Whether the table counts slot @i of the objects as taken: reserved, holding one, or retired. */
static bool opaque_test_used(struct io_opaque_store *store, u32 i)
{
	return test_bit(i, store->object_used);
}

/* Count slot @i of the objects as taken, as a reservation does. */
static void opaque_test_use(struct io_opaque_store *store, u32 i)
{
	__set_bit(i, store->object_used);
}

/* Give slot @i of the objects back as FREE does: a slot on its last generation is retired. */
static void opaque_test_release(struct io_opaque_store *store, u32 i)
{
	io_opaque_release_slot(store->object_used, i, opaque_test_slot(store, i)->generation);
}

/* Tables of @nr object slots, for a test that needs more than the fixture's or fewer. */
static void opaque_test_slots(struct kunit *test, struct io_opaque_store *store, u32 nr)
{
	u32 i;

	store->config.max_objects = nr;
	store->objects = kunit_kcalloc(test, nr, sizeof(*store->objects), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, store->objects);
	store->object_used = kunit_kzalloc(test, BITS_TO_LONGS(nr) * sizeof(unsigned long),
					   GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, store->object_used);
	for (i = 0; i < nr; i++)
		INIT_LIST_HEAD(&store->objects[i].candidate);
}

/* Tables of @nr stream slots. */
static void opaque_test_streams(struct kunit *test, struct io_opaque_store *store, u32 nr)
{
	store->config.max_streams = nr;
	store->streams = kunit_kcalloc(test, nr, sizeof(*store->streams), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, store->streams);
	store->stream_used = kunit_kzalloc(test, BITS_TO_LONGS(nr) * sizeof(unsigned long),
					   GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, store->stream_used);
}

/* The tables a fixture starts with: four object slots. */
static void opaque_test_tables(struct kunit *test, struct io_opaque_store *store)
{
	opaque_test_slots(test, store, 4);
}

static struct io_opaque_store *opaque_test_store(struct kunit *test)
{
	struct io_opaque_store *store = kunit_kzalloc(test, sizeof(*store), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, store);
	store->config.hard_limit = 128 * PAGE_SIZE;
	opaque_test_tables(test, store);
	spin_lock_init(&store->tables);
	spin_lock_init(&store->wait_lock);
	spin_lock_init(&store->tx_lock);
	spin_lock_init(&store->transient_lock);
	seqcount_spinlock_init(&store->transient_seq, &store->transient_lock);
	INIT_LIST_HEAD(&store->budget_waits);
	INIT_LIST_HEAD(&store->candidates);
	INIT_LIST_HEAD(&store->txs_idle);
	/* Idle TX queues are separate allocations; free them before the store goes. */
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_txs_free, store), 0);
	return store;
}

/*
 * The start of a test that races two CPUs. Without CONFIG_SMP there is nothing to race on, and
 * the test is skipped. With it and fewer than two CPUs online the suite was not run as it is
 * meant to be: UML boots one CPU unless it is told ncpus= (and seccomp=on, for them to run), and
 * a test that skipped there would let the suite report ok having raced nothing. That is a
 * failure, and the message says what to boot with.
 */
static void opaque_race_needs_cpus(struct kunit *test)
{
	if (!IS_ENABLED(CONFIG_SMP))
		kunit_skip(test, "needs two CPUs to race on");
	if (num_online_cpus() < 2)
		KUNIT_FAIL_AND_ABORT(test,
				     "CONFIG_SMP=y with %u CPU online, and the race tests need two: boot with ncpus=4 seccomp=on (kunit.py: --kernel_args=ncpus=4 --kernel_args=seccomp=on)",
				     num_online_cpus());
}

/*
 * io_opaque_send_acquire() as admission calls it, with no lock held: a send that consumes
 * its object takes tables itself, and any other reads the table in RCU.
 */
static int opaque_test_acquire(struct io_opaque_store *store, struct io_opaque_req *op)
{
	lockdep_assert_not_held(&store->tables);
	return io_opaque_send_acquire(op);
}

/*
 * The version that slot @i holds, in a test that is alone with the table: nothing changes it
 * while the test looks, and it holds no lock.
 */
static struct io_opaque_data *opaque_slot_data(struct io_opaque_store *store, u32 i)
{
	return rcu_dereference_protected(opaque_test_slot(store, i)->data, true);
}

/*
 * Make @data what slot @i holds, or nothing, in a test that is alone with the table. A version
 * carries the handle that its slot gives it, which the slot's generation must already be.
 */
static void opaque_slot_set(struct io_opaque_store *store, u32 i, struct io_opaque_data *data)
{
	if (data)
		data->handle = io_opaque_handle(i, opaque_test_slot(store, i)->generation);
	rcu_assign_pointer(opaque_test_slot(store, i)->data, data);
}

/* Publish @data in @slot under @generation. */
static void opaque_test_publish(struct io_opaque_store *store, u32 slot, u64 generation,
				struct io_opaque_data *data)
{
	opaque_test_slot(store, slot)->generation = generation;
	opaque_slot_set(store, slot, data);
	store->nr_objects++;
	opaque_test_use(store, slot);
}

/* FREE as the issue does it: the handle goes under tables, and the version is put after. */
static void opaque_test_free(struct io_opaque_store *store, u32 index)
{
	struct io_opaque_data *data;

	spin_lock(&store->tables);
	data = io_opaque_detach(store, opaque_test_slot(store, index));
	spin_unlock(&store->tables);
	io_opaque_data_put(store, data);
}

/*
 * What a test can say about a spinlock: that this context holds it, that it does not, or that
 * nothing can be told. Lockdep knows who holds a lock. Without it spin_is_locked() says whether
 * anyone does, which means something on SMP and with CONFIG_DEBUG_SPINLOCK, and nothing on a
 * uniprocessor kernel without it, where it is always 0 (spinlock_up.h).
 */
#define OPAQUE_LOCK_UNKNOWN	(-1)

static int opaque_test_lock_state(spinlock_t *lock)
{
#ifdef CONFIG_LOCKDEP
	switch (lockdep_is_held(lock)) {
	case LOCK_STATE_HELD:
		return 1;
	case LOCK_STATE_NOT_HELD:
		return 0;
	}
	/* Lockdep has turned itself off, after a report. */
	return OPAQUE_LOCK_UNKNOWN;
#elif defined(CONFIG_SMP) || defined(CONFIG_DEBUG_SPINLOCK)
	return spin_is_locked(lock);
#else
	return OPAQUE_LOCK_UNKNOWN;
#endif
}

/*
 * Whether @lock is held, for the expectations that it is not. A lock that cannot be told counts
 * as free, so those expectations are not checked there, and the console says so once;
 * opaque_lock_held_check() skips on such a kernel, which shows in the results.
 */
static bool opaque_test_held(spinlock_t *lock)
{
	int state = opaque_test_lock_state(lock);

	if (state == OPAQUE_LOCK_UNKNOWN)
		pr_info_once("opaque_test: neither lockdep nor the lock can say whether a spinlock is held: the expectations that one is free are not checked\n");
	return state == 1;
}

/*
 * Lockdep turns itself off after its first report, and an RCU report is one: a test that is about
 * the locks and the RCU sections of a lookup says at its end that lockdep is still on, so that a
 * report made in it is a failure and not a line on the console. Where nothing can tell, it is
 * silent.
 */
static void opaque_expect_lockdep(struct kunit *test, struct io_opaque_store *store)
{
	if (IS_ENABLED(CONFIG_LOCKDEP))
		KUNIT_EXPECT_NE(test, opaque_test_lock_state(&store->tables), OPAQUE_LOCK_UNKNOWN);
}

/* Fragments of a test skb: MAX_SKB_FRAGS is 17 to 45, depending on the configuration. */
#define OPAQUE_TEST_FRAGS	17
static_assert(OPAQUE_TEST_FRAGS <= MAX_SKB_FRAGS);

/* Set the fragment limit for the rest of the test. */
static void opaque_max_frags_restore(void *arg)
{
	WRITE_ONCE(net_hotdata.sysctl_max_skb_frags, (int)(long)arg);
}

static void opaque_max_frags_set(struct kunit *test, int frags)
{
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_max_frags_restore,
				(void *)(long)READ_ONCE(net_hotdata.sysctl_max_skb_frags)), 0);
	WRITE_ONCE(net_hotdata.sysctl_max_skb_frags, frags);
}

/*
 * Segments for a fill to run past the fragment limit of any configuration, each in a page of
 * its own: the 40 extents of a fixture's object are fewer than the 45 fragments that an skb
 * may have.
 */
struct opaque_segs {
	struct bio_vec bv[MAX_SKB_FRAGS + 4];
	u32 nr;
};

static void opaque_segs_put(void *arg)
{
	struct opaque_segs *s = arg;

	while (s->nr)
		put_page(s->bv[--s->nr].bv_page);
}

/* @nr segments of @len bytes at the start of a page each, which the test lets go of at its end. */
static struct opaque_segs *opaque_segs_new(struct kunit *test, u32 nr, u32 len)
{
	struct opaque_segs *segs = kunit_kzalloc(test, sizeof(*segs), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, segs);
	KUNIT_ASSERT_LE(test, nr, (u32)ARRAY_SIZE(segs->bv));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_segs_put, segs), 0);
	while (segs->nr < nr) {
		struct page *page = alloc_page(GFP_KERNEL);

		KUNIT_ASSERT_NOT_NULL(test, page);
		bvec_set_page(&segs->bv[segs->nr++], page, len, 0);
	}
	return segs;
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

/*
 * What the store is charged besides pages, in the units the cases count in: the block of a
 * stream, a chunk that is a block of its own (CREATE and compaction make them, and so does
 * a case that makes an extent by hand), and an extent that is a record of its own (a split,
 * a snapshot, a second extent of a page). A stream's block is charged when its first chunk
 * is taken and given back with its last, whatever the stream has done with the others.
 */
#define OPAQUE_TEST_BLOCK	((s64)io_opaque_block_bytes)
#define OPAQUE_TEST_ALONE	((s64)io_opaque_alone_bytes)
#define OPAQUE_TEST_EXTENT	((s64)io_opaque_extent_bytes)

/*
 * What the store is charged for a version besides its pages, when every extent of it is a chunk of
 * its own, as CREATE and compaction make them and the fixtures that make extents by hand: a block
 * of one chunk for each extent, and the vector.
 */
static s64 opaque_records(const struct io_opaque_data *data)
{
	return (s64)data->nr * OPAQUE_TEST_ALONE + data->vec_bytes;
}

/* What opaque_sparse_data() holds: forty pages, and what goes with them. */
static s64 opaque_sparse_held(const struct io_opaque_data *data)
{
	return 40 * PAGE_SIZE + opaque_records(data);
}

/*
 * The room that attaching gives a stream for the references of a call.
 * io_opaque_capture_release() frees it.
 */
static void opaque_test_scratch(struct kunit *test, struct io_opaque_stream *stream)
{
	stream->pending = kmalloc_objs(*stream->pending, IO_OPAQUE_PENDING_MAX, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, stream->pending);
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
	io_opaque_capture_release(&stream);
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
	io_opaque_capture_release(&stream);
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
	io_opaque_capture_release(&stream);
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
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)2 * PAGE_SIZE + OPAQUE_TEST_BLOCK);
	kfree_skb(skb);
	io_opaque_extents_free(store, &stream.extents);
	put_page(page);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

static void opaque_capture_oversized(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct page *page = alloc_pages(GFP_KERNEL | __GFP_COMP, 2);
	struct sk_buff *skb;
	void *mapped;

	KUNIT_ASSERT_NOT_NULL(test, page);
	/* A page to copy into and the block for its chunk: less than the page that is captured. */
	store->config.hard_limit = PAGE_SIZE + OPAQUE_TEST_BLOCK;
	opaque_test_stream(&stream, store);
	mapped = kmap_local_page(page);
	memset(mapped, 0x37, 32);
	kunmap_local(mapped);
	skb = opaque_test_frag(test, page, 0, 32);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 32, false), 32);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_NE(test, extent->backing->page, page);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 32LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE + OPAQUE_TEST_BLOCK);
	mapped = kmap_local_page(extent->backing->page);
	KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[0], (u8)0x37);
	KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[31], (u8)0x37);
	kunmap_local(mapped);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	io_opaque_extents_free(store, &stream.extents);
	io_opaque_capture_release(&stream);
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
	store->config.hard_limit = PAGE_SIZE + OPAQUE_TEST_BLOCK;
	opaque_test_stream(&stream, store);
	a = opaque_test_frag(test, first, 0, 32);
	b = opaque_test_frag(test, second, 0, 32);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, a, 0, 32, false), 32);
	stream.next = 32;
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, b, 0, 32, false), -ENOBUFS);
	KUNIT_EXPECT_EQ(test, page_count(second), 2);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 1);
	kfree_skb(a);
	kfree_skb(b);
	io_opaque_extents_free(store, &stream.extents);
	io_opaque_capture_release(&stream);
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
	/* The first fragment makes the backing and takes a chunk, the other seven are records. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)4 * PAGE_SIZE + OPAQUE_TEST_BLOCK + 7 * OPAQUE_TEST_EXTENT);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 8);
	KUNIT_EXPECT_EQ(test, page_count(page), 2);
	list_splice_init(&stream.extents, &data->extents);
	data->length = stream.next;
	KUNIT_ASSERT_EQ(test, io_opaque_vector(store, data), 0);
	KUNIT_EXPECT_EQ(test, data->charge, (u64)4 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)4 * PAGE_SIZE + OPAQUE_TEST_BLOCK + 7 * OPAQUE_TEST_EXTENT +
			data->vec_bytes);
	io_opaque_data_put(store, data);
	/* The version gave back its pages, records and vector; the stream still holds its block. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), OPAQUE_TEST_BLOCK);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE + OPAQUE_TEST_BLOCK);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 512LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 1);
	for (i = 0; i < 16; i++) {
		void *mapped = kmap_local_page(extent->backing->page);

		KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[i * 32], (u8)i);
		KUNIT_EXPECT_EQ(test, ((u8 *)mapped)[i * 32 + 31], (u8)i);
		kunmap_local(mapped);
	}
	io_opaque_extents_free(store, &stream.extents);
	io_opaque_capture_release(&stream);
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
	store->config.hard_limit = PAGE_SIZE + OPAQUE_TEST_BLOCK;
	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	get_page(first);
	get_page(second);
	skb_add_rx_frag(skb, 0, first, 0, 512, PAGE_SIZE);
	skb_add_rx_frag(skb, 1, second, 0, 512, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, 1024), 512);
	KUNIT_EXPECT_EQ(test, desc.error, -ENOBUFS);
	KUNIT_EXPECT_EQ(test, stream.next, 512ULL);
	kfree_skb(skb);
	io_opaque_extents_free(store, &stream.extents);
	io_opaque_capture_release(&stream);
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
	/* The pages that the object may hold, and the block that the stream takes chunks from. */
	store->config.hard_limit = 8 * PAGE_SIZE + OPAQUE_TEST_BLOCK;
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
	KUNIT_EXPECT_LE(test, atomic64_read(&store->bytes), (s64)store->config.hard_limit);
	KUNIT_EXPECT_GT(test, atomic64_read(&store->copied), 0LL);
	io_opaque_data_put(store, data);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	/* The page, the block of one chunk that holds the backing, and the record of the split. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)PAGE_SIZE + OPAQUE_TEST_ALONE + OPAQUE_TEST_EXTENT);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 2);
	/* The extent in the chunk goes; the backing, its page and block stay for the other. */
	io_opaque_extent_free(store, left);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)PAGE_SIZE + OPAQUE_TEST_ALONE + OPAQUE_TEST_EXTENT);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 1);
	io_opaque_extent_free(store, right);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/* The last release gives back every reference that the backing holds, of the kind it holds. */
static void opaque_release_units(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_extent *extent;
	struct page *page = alloc_page(GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, page);
	/* The test keeps one reference, and the backing holds three. */
	page_ref_add(page, 3);
	extent = io_opaque_extent_new(store, page, 0, 128, 0, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PAGE);
	KUNIT_EXPECT_EQ(test, extent->backing->units, (u16)1);
	extent->backing->units = 3;
	KUNIT_EXPECT_EQ(test, page_count(page), 4);
	io_opaque_extent_free(store, extent);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	put_page(page);
}

/*
 * The counters tell how capture got its pages: by a reference of its own, or by
 * copying into a page of its own. Bytes that join a page already held add
 * nothing.
 */
static void opaque_capture_page_stats(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct page *page = alloc_page(GFP_KERNEL);
	u64 held = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD);
	u64 copied = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_COPIED);
	u64 stolen = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN);
	struct sk_buff *skb;
	int i;

	KUNIT_ASSERT_NOT_NULL(test, page);
	opaque_test_stream(&stream, store);
	skb = opaque_test_frag(test, page, 0, 512);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 512, false), 512);
	stream.next = 512;
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held + 1);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_COPIED), copied);
	kfree_skb(skb);

	/* Linear packets are copied, and eight of them fill one page. */
	for (i = 0; i < 8; i++) {
		skb = alloc_skb(64, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, skb);
		memset(skb_put(skb, 64), i, 64);
		KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 64, false), 64);
		stream.next += 64;
		kfree_skb(skb);
	}
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_COPIED), copied + 1);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held + 1);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN), stolen);
	io_opaque_extents_free(store, &stream.extents);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	put_page(page);
}

/* The extent of a stream for a page of its own; the backing takes the reference to the page. */
static struct io_opaque_extent *opaque_block_extent(struct kunit *test,
						    struct io_opaque_stream *stream, u64 start)
{
	struct page *page = alloc_page(GFP_KERNEL);
	struct io_opaque_extent *extent;

	KUNIT_ASSERT_NOT_NULL(test, page);
	extent = io_opaque_stream_extent_new(stream, page, 0, 128, start, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
	list_add_tail(&extent->list, &stream->extents);
	return extent;
}

static struct io_opaque_block *opaque_block_of(struct io_opaque_extent *extent)
{
	return container_of(extent->backing, struct io_opaque_chunk, backing)->block;
}

/*
 * The test is the last to hold a block that it took a reference to, and it frees the block as the
 * release of the last chunk would, which gives the store back what it was charged for it.
 */
static void opaque_test_block_free(struct kunit *test, struct io_opaque_store *store,
				   struct io_opaque_block *block)
{
	u32 charge = block->charge;

	KUNIT_EXPECT_TRUE(test, refcount_dec_and_test(&block->live));
	kfree(block);
	io_opaque_uncharge(store, charge);
}

/*
 * The chunks of a stream come in blocks. A block lives until the stream has
 * let go of it and the backing of every chunk that it handed out is released,
 * in whatever order, and not longer. The test keeps a reference of its own to
 * the blocks that it counts, so it frees them, as the last holder, and a
 * release that goes wrong shows in the count.
 */
static void opaque_block_lifetime(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_extent *ext[IO_OPAQUE_BLOCK_CHUNKS + 8];
	struct io_opaque_block *first, *second, *third;
	unsigned int i, n = ARRAY_SIZE(ext), stride = 7;
	u64 blocks = io_opaque_stat_read(IO_OPAQUE_STAT_CHUNK_BLOCKS);

	opaque_test_stream(&stream, store);
	KUNIT_EXPECT_NULL(test, stream.block);
	/* A block fills the slab object it gets, and holds no fewer chunks than 32 of them. */
	KUNIT_EXPECT_GE(test, IO_OPAQUE_BLOCK_CHUNKS, 32);
	KUNIT_EXPECT_LE(test, struct_size_t(struct io_opaque_block, chunk, IO_OPAQUE_BLOCK_CHUNKS),
			(size_t)SZ_4K);
	KUNIT_EXPECT_GT(test, struct_size_t(struct io_opaque_block, chunk,
					    IO_OPAQUE_BLOCK_CHUNKS + 1), (size_t)SZ_4K);
	kunit_info(test, "a block of %d chunks, %zu bytes", IO_OPAQUE_BLOCK_CHUNKS,
		   struct_size_t(struct io_opaque_block, chunk, IO_OPAQUE_BLOCK_CHUNKS));

	/* The first chunk makes a block: every chunk and the stream hold it. */
	ext[0] = opaque_block_extent(test, &stream, 0);
	first = opaque_block_of(ext[0]);
	KUNIT_EXPECT_PTR_EQ(test, stream.block, first);
	KUNIT_EXPECT_EQ(test, first->used, 1U);
	KUNIT_EXPECT_EQ(test, refcount_read(&first->live), IO_OPAQUE_BLOCK_CHUNKS + 1);
	/* The block is charged with its first chunk, once, and it is the slab object of 4 KiB. */
	KUNIT_EXPECT_EQ(test, first->charge, (u32)SZ_4K);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE + SZ_4K);
	refcount_inc(&first->live);
	for (i = 1; i < IO_OPAQUE_BLOCK_CHUNKS; i++) {
		ext[i] = opaque_block_extent(test, &stream, i * 128);
		KUNIT_EXPECT_PTR_EQ(test, opaque_block_of(ext[i]), first);
	}
	/* The last chunk uses the block up, and the stream lets go of it. */
	KUNIT_EXPECT_NULL(test, stream.block);
	KUNIT_EXPECT_EQ(test, first->used, (u32)IO_OPAQUE_BLOCK_CHUNKS);
	KUNIT_EXPECT_EQ(test, refcount_read(&first->live), IO_OPAQUE_BLOCK_CHUNKS + 1);

	/* The next chunks are in another block, which the stream keeps. */
	for (; i < n; i++)
		ext[i] = opaque_block_extent(test, &stream, i * 128);
	second = stream.block;
	KUNIT_ASSERT_NOT_NULL(test, second);
	KUNIT_EXPECT_PTR_NE(test, second, first);
	KUNIT_EXPECT_PTR_EQ(test, opaque_block_of(ext[n - 1]), second);
	KUNIT_EXPECT_EQ(test, second->used, n - IO_OPAQUE_BLOCK_CHUNKS);
	refcount_inc(&second->live);
	KUNIT_EXPECT_EQ(test, refcount_read(&second->live), IO_OPAQUE_BLOCK_CHUNKS + 2);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), (int)n);
	/* The pages, and one charge for each of the two blocks and not one for each chunk. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)n * PAGE_SIZE + 2 * SZ_4K);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_CHUNK_BLOCKS), blocks + 2);

	/* Free the first block's chunks out of order: each takes one hold off the block. */
	while (gcd(stride, IO_OPAQUE_BLOCK_CHUNKS) != 1)
		stride++;
	for (i = 0; i < IO_OPAQUE_BLOCK_CHUNKS; i++) {
		io_opaque_extent_free(store, ext[(i * stride) % IO_OPAQUE_BLOCK_CHUNKS]);
		KUNIT_EXPECT_EQ(test, refcount_read(&first->live),
				IO_OPAQUE_BLOCK_CHUNKS + 1 - (int)i - 1);
	}
	/* Only the test holds it now, and its release is the one that frees it. */
	KUNIT_EXPECT_EQ(test, refcount_read(&first->live), 1);
	/* The pages are back, and the block is not: a chunk of it is still referenced. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)(n - IO_OPAQUE_BLOCK_CHUNKS) * PAGE_SIZE + 2 * SZ_4K);
	opaque_test_block_free(test, store, first);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)(n - IO_OPAQUE_BLOCK_CHUNKS) * PAGE_SIZE + SZ_4K);

	/* The second block is not freed while the stream may hand out more chunks. */
	for (i = n; i-- > IO_OPAQUE_BLOCK_CHUNKS;)
		io_opaque_extent_free(store, ext[i]);
	KUNIT_EXPECT_PTR_EQ(test, stream.block, second);
	KUNIT_EXPECT_EQ(test, refcount_read(&second->live), IO_OPAQUE_BLOCK_CHUNKS + 2 - 8);
	/* The pages are back, and the block that the stream may hand out chunks of is charged. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)SZ_4K);
	/* Retired, it keeps only the test's hold. */
	io_opaque_block_retire(&stream);
	KUNIT_EXPECT_NULL(test, stream.block);
	KUNIT_EXPECT_EQ(test, refcount_read(&second->live), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)SZ_4K);
	opaque_test_block_free(test, store, second);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);

	/* A block retired with chunks out lives until the last of them is released. */
	for (i = 0; i < 3; i++)
		ext[i] = opaque_block_extent(test, &stream, i * 128);
	third = opaque_block_of(ext[0]);
	refcount_inc(&third->live);
	io_opaque_block_retire(&stream);
	KUNIT_EXPECT_EQ(test, refcount_read(&third->live), 3 + 1);
	io_opaque_extent_free(store, ext[1]);
	io_opaque_extent_free(store, ext[2]);
	KUNIT_EXPECT_EQ(test, refcount_read(&third->live), 1 + 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE + SZ_4K);
	io_opaque_extent_free(store, ext[0]);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)SZ_4K);
	opaque_test_block_free(test, store, third);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);

	/* A chunk made alone is a block of its own, which its release frees. */
	{
		LIST_HEAD(alone);
		struct page *page = alloc_page(GFP_KERNEL);
		struct io_opaque_extent *lone;

		KUNIT_ASSERT_NOT_NULL(test, page);
		lone = io_opaque_extent_new(store, page, 0, 128, 0, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(lone));
		list_add(&lone->list, &alone);
		third = opaque_block_of(lone);
		KUNIT_EXPECT_EQ(test, third->nr, 1U);
		KUNIT_EXPECT_EQ(test, refcount_read(&third->live), 1);
		/* It is charged for the block of one chunk, at the size class of it. */
		KUNIT_EXPECT_EQ(test, third->charge, (u32)OPAQUE_TEST_ALONE);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
				(s64)PAGE_SIZE + OPAQUE_TEST_ALONE);
		refcount_inc(&third->live);
		io_opaque_extent_free(store, lone);
		KUNIT_EXPECT_EQ(test, refcount_read(&third->live), 1);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), OPAQUE_TEST_ALONE);
		opaque_test_block_free(test, store, third);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	}

	/* Without the test's hold the last release frees the block: KASAN sees a mistake. */
	for (i = 0; i < 3; i++)
		ext[i] = opaque_block_extent(test, &stream, i * 128);
	io_opaque_block_retire(&stream);
	io_opaque_extent_free(store, ext[2]);
	io_opaque_extent_free(store, ext[0]);
	/* The last release gives the block back with the page of the chunk. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE + SZ_4K);
	io_opaque_extent_free(store, ext[1]);
	KUNIT_EXPECT_NULL(test, stream.block);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * A stream's block is charged to the store when the stream takes the first chunk of it, once for
 * all of its chunks, and given back with the release of the last of them, whether the stream
 * retired it before or after. The pages are charged one by one, as they always were.
 */
static void opaque_block_charge(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_extent *ext[IO_OPAQUE_BLOCK_CHUNKS + 1];
	unsigned int i;

	opaque_test_stream(&stream, store);
	/* A block is the slab object of 4 KiB, whatever the configuration. */
	KUNIT_EXPECT_EQ(test, OPAQUE_TEST_BLOCK, (s64)SZ_4K);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);

	/* The first chunk charges the block and its page, the others their pages. */
	ext[0] = opaque_block_extent(test, &stream, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE + SZ_4K);
	for (i = 1; i < IO_OPAQUE_BLOCK_CHUNKS; i++) {
		ext[i] = opaque_block_extent(test, &stream, i * 128);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
				(s64)(i + 1) * PAGE_SIZE + SZ_4K);
	}
	/* The block is used up: the next chunk charges another. */
	KUNIT_EXPECT_NULL(test, stream.block);
	ext[i] = opaque_block_extent(test, &stream, i * 128);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)(i + 1) * PAGE_SIZE + 2 * SZ_4K);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), (long)IO_OPAQUE_BLOCK_CHUNKS + 1);

	/* The pages go back one by one, and a block with the last of its chunks. */
	for (i = 0; i < IO_OPAQUE_BLOCK_CHUNKS - 1; i++)
		io_opaque_extent_free(store, ext[i]);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)2 * PAGE_SIZE + 2 * SZ_4K);
	io_opaque_extent_free(store, ext[IO_OPAQUE_BLOCK_CHUNKS - 1]);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE + SZ_4K);
	/* The stream is still to take chunks of the second block, and holds it. */
	io_opaque_extent_free(store, ext[IO_OPAQUE_BLOCK_CHUNKS]);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)SZ_4K);
	io_opaque_block_retire(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);

	/* Retired with chunks out, a block stays charged until the last of them is released. */
	for (i = 0; i < 3; i++)
		ext[i] = opaque_block_extent(test, &stream, i * 128);
	io_opaque_block_retire(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)3 * PAGE_SIZE + SZ_4K);
	io_opaque_extent_free(store, ext[1]);
	io_opaque_extent_free(store, ext[0]);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE + SZ_4K);
	io_opaque_extent_free(store, ext[2]);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);

	/* What capture keeps in a stream goes with the stream: the extents, then the block. */
	ext[0] = opaque_block_extent(test, &stream, 0);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)SZ_4K);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/*
 * The block is charged against the limit that capture stops at, as a page is: capture takes a page
 * if it fits and the block of its chunk does not, and neither is charged then. The budget the
 * waiter needs is the two, and the page alone when the stream has a block.
 */
static void opaque_block_charge_limit(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct page *page = alloc_page(GFP_KERNEL), *next = alloc_page(GFP_KERNEL);
	struct sk_buff *skb;
	s64 rx, held;

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_NOT_NULL(test, next);
	store->config.hard_limit = 64 * PAGE_SIZE;
	store->config.compact_headroom = 8 * PAGE_SIZE;
	store->config.reply_reserve = 2 * PAGE_SIZE;
	rx = io_opaque_rx_limit(store);
	opaque_test_stream(&stream, store);

	/* Not even the page fits: the stream has no block, and what it needs is both. */
	held = rx - PAGE_SIZE + 1;
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, held, true));
	skb = opaque_test_frag(test, page, 0, 32);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 32, false), -ENOBUFS);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), held);
	KUNIT_EXPECT_EQ(test, stream.need_bytes, (u64)PAGE_SIZE + SZ_4K);
	kfree_skb(skb);

	/* A page and one byte of a block fit below the capture limit: neither is kept. */
	io_opaque_uncharge(store, SZ_4K);
	held -= SZ_4K;
	skb = opaque_test_frag(test, page, 0, 32);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 32, false), -ENOBUFS);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), held);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	KUNIT_EXPECT_NULL(test, stream.block);
	KUNIT_EXPECT_TRUE(test, list_empty(&stream.extents));
	KUNIT_EXPECT_EQ(test, page_count(page), 2);
	/* Without a block the stream needs the page and the block to go on. */
	KUNIT_EXPECT_EQ(test, stream.need_bytes, (u64)PAGE_SIZE + SZ_4K);
	kfree_skb(skb);

	/* One byte more, and both fit: the page and one block, and the capture limit is reached. */
	io_opaque_uncharge(store, 1);
	held--;
	skb = opaque_test_frag(test, page, 0, 32);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 32, false), 32);
	stream.next = 32;
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), rx);
	KUNIT_ASSERT_NOT_NULL(test, stream.block);
	KUNIT_EXPECT_EQ(test, stream.block->charge, (u32)SZ_4K);

	/* The stream has its block now: what it needs from the next page is the page. */
	skb = opaque_test_frag(test, next, 0, 32);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 32, false), -ENOBUFS);
	KUNIT_EXPECT_EQ(test, stream.need_bytes, (u64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), rx);
	kfree_skb(skb);

	/* The one chunk that was taken goes back, and the block with it. */
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	io_opaque_extent_free(store, extent);
	io_opaque_capture_release(&stream);
	io_opaque_uncharge(store, held);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	put_page(page);
	put_page(next);
}

/*
 * A split is a record of its own, charged against the hard limit less the reserve and not against
 * the limit that capture stops at: the headroom above it holds the splits at the ends of a take,
 * and nothing else needs to be kept back for them. It fails only when the record does not fit
 * there, and then it changes nothing.
 */
static void opaque_split_charge(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_extent *first, *second, *third;
	struct page *page = alloc_page(GFP_KERNEL);
	LIST_HEAD(extents);
	s64 rx, hard, held = 0, charge;

	KUNIT_ASSERT_NOT_NULL(test, page);
	store->config.hard_limit = 64 * PAGE_SIZE;
	store->config.compact_headroom = 8 * PAGE_SIZE;
	store->config.reply_reserve = 2 * PAGE_SIZE;
	rx = io_opaque_rx_limit(store);
	hard = store->config.hard_limit - store->config.reply_reserve;
	first = io_opaque_extent_new(store, page, 0, 128, 0, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(first));
	list_add(&first->list, &extents);

	/* The store is full as far as capture is concerned. */
	charge = rx - atomic64_read(&store->bytes);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, charge, true));
	held += charge;
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), rx);
	KUNIT_EXPECT_FALSE(test, io_opaque_charge(store, 1, true));

	/* The split is not capture: the record is taken from the headroom. */
	second = io_opaque_extent_split(store, first, 64);
	KUNIT_ASSERT_FALSE(test, IS_ERR(second));
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), rx + OPAQUE_TEST_EXTENT);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 2);

	/* One byte short of the room for a record under the hard limit less the reserve. */
	charge = hard - OPAQUE_TEST_EXTENT + 1 - atomic64_read(&store->bytes);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, charge, false));
	held += charge;
	third = io_opaque_extent_split(store, first, 32);
	KUNIT_EXPECT_EQ(test, PTR_ERR(third), -ENOBUFS);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), hard - OPAQUE_TEST_EXTENT + 1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 2);
	KUNIT_EXPECT_EQ(test, first->length, 64U);

	/* With the byte, it fits, and the reserve is untouched. */
	io_opaque_uncharge(store, 1);
	held--;
	third = io_opaque_extent_split(store, first, 32);
	KUNIT_ASSERT_FALSE(test, IS_ERR(third));
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), hard);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 3);
	KUNIT_EXPECT_FALSE(test, io_opaque_charge(store, 1, false));

	/* The records go back with their extents, and the page and the chunk with the last. */
	io_opaque_extent_free(store, third);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), hard - OPAQUE_TEST_EXTENT);
	io_opaque_extent_free(store, second);
	io_opaque_extent_free(store, first);
	io_opaque_uncharge(store, held);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/*
 * A chunk made alone, as CREATE and compaction make them for the destination units, is a record
 * charged against the hard limit less the reserve, not the capture limit, while the page is the
 * caller's to have charged (IO_OPAQUE_EXT_RESERVED). When the record does not fit the extent is
 * refused with -ENOBUFS and nothing is left charged.
 */
static void opaque_chunk_alone_charge(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_extent *extent;
	struct page *page = alloc_page(GFP_KERNEL), *refused = alloc_page(GFP_KERNEL);
	s64 rx, hard, held, charge;

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_NOT_NULL(test, refused);
	store->config.hard_limit = 64 * PAGE_SIZE;
	store->config.compact_headroom = 8 * PAGE_SIZE;
	store->config.reply_reserve = 2 * PAGE_SIZE;
	rx = io_opaque_rx_limit(store);
	hard = store->config.hard_limit - store->config.reply_reserve;

	/* The caller has charged its pages up to the capture limit, with the extent's own. */
	held = rx - PAGE_SIZE;
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, held, true));
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, PAGE_SIZE, true));
	extent = io_opaque_extent_new(store, page, 0, 128, 0, IO_OPAQUE_EXT_RESERVED);
	KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), rx + OPAQUE_TEST_ALONE);
	KUNIT_EXPECT_EQ(test, extent->backing->charge, (u64)PAGE_SIZE);

	/* One byte short of the room for another record. */
	charge = hard - OPAQUE_TEST_ALONE + 1 - atomic64_read(&store->bytes);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, charge, false));
	held += charge;
	KUNIT_EXPECT_EQ(test, PTR_ERR(io_opaque_extent_new(store, refused, 0, 128, 128,
							   IO_OPAQUE_EXT_RESERVED)), -ENOBUFS);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), hard - OPAQUE_TEST_ALONE + 1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 1);
	KUNIT_EXPECT_EQ(test, page_count(refused), 1);

	/* A page that the store is to charge is refused at the capture limit, with no record. */
	io_opaque_uncharge(store, 1);
	held--;
	KUNIT_EXPECT_EQ(test, PTR_ERR(io_opaque_extent_new(store, refused, 0, 128, 128, 0)),
			-ENOBUFS);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), hard - OPAQUE_TEST_ALONE);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 1);

	io_opaque_extent_free(store, extent);
	io_opaque_uncharge(store, held);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	put_page(refused);
}

/*
 * A second extent of a page that capture holds is a record of its own, charged against the hard
 * limit less the reserve: capture can add a fragment of a page it holds when it can take no other
 * page, and stops when the record does not fit, with nothing charged or counted for it.
 */
static void opaque_second_extent_charge(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct page *page = alloc_pages(GFP_KERNEL | __GFP_COMP, 2);
	struct sk_buff *skb;
	s64 rx, hard, held = 0, charge;

	KUNIT_ASSERT_NOT_NULL(test, page);
	store->config.hard_limit = 64 * PAGE_SIZE;
	store->config.compact_headroom = 8 * PAGE_SIZE;
	store->config.reply_reserve = 2 * PAGE_SIZE;
	rx = io_opaque_rx_limit(store);
	hard = store->config.hard_limit - store->config.reply_reserve;
	opaque_test_stream(&stream, store);

	/* The first fragment: the page of four, and the block of its chunk. */
	skb = opaque_test_frag(test, page, 0, 512);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 512, false), 512);
	stream.next = 512;
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)4 * PAGE_SIZE + OPAQUE_TEST_BLOCK);

	/* Capture can take no page, and a fragment of the page that it holds still goes in. */
	charge = rx - atomic64_read(&store->bytes);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, charge, true));
	held += charge;
	skb = opaque_test_frag(test, page, 1024, 512);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 512, false), 512);
	stream.next += 512;
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), rx + OPAQUE_TEST_EXTENT);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 2);

	/* One byte short of the room for the next record. */
	charge = hard - OPAQUE_TEST_EXTENT + 1 - atomic64_read(&store->bytes);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, charge, false));
	held += charge;
	skb = opaque_test_frag(test, page, 2048, 512);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 512, false), -ENOBUFS);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), hard - OPAQUE_TEST_EXTENT + 1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 2);
	kfree_skb(skb);

	io_opaque_uncharge(store, 1);
	held--;
	skb = opaque_test_frag(test, page, 2048, 512);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 512, false), 512);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), hard);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 3);

	/* The three extents are one chunk and two records, and go with the page. */
	io_opaque_extents_free(store, &stream.extents);
	io_opaque_capture_release(&stream);
	io_opaque_uncharge(store, held);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	put_page(page);
}

/* A version of @nr extents, each over a page of its own, for the cases about its vector. */
static struct io_opaque_data *opaque_vector_data(struct kunit *test, struct io_opaque_store *store,
						 u32 nr)
{
	struct io_opaque_data *data = io_opaque_data_alloc();
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, data);
	for (i = 0; i < nr; i++) {
		struct page *page = alloc_page(GFP_KERNEL);
		struct io_opaque_extent *extent;

		KUNIT_ASSERT_NOT_NULL(test, page);
		extent = io_opaque_extent_new(store, page, 0, 16, i * 16, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
		list_add_tail(&extent->list, &data->extents);
		data->length += 16;
	}
	return data;
}

/*
 * The vector of a version, its segments and the anchors that follow them, is memory of the store:
 * charged once for the whole of it when it is built, against the hard limit less the reserve, and
 * given back, in the same step as the extents, when the version is. A vector that does not fit
 * leaves nothing behind.
 */
static void opaque_vector_charge(struct kunit *test)
{
	static const struct { u32 nr, bytes; } cases[] = {
		/* A segment is 16 bytes; over 16 of them, 8 bytes of anchor for each 16. */
		{ 1, 16 }, { 16, 256 }, { 17, 288 }, { 40, 664 },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data;
	s64 before, rx;
	long wakes;
	u32 i;

	store->config.hard_limit = 128 * PAGE_SIZE;
	store->config.compact_headroom = 8 * PAGE_SIZE;
	store->config.reply_reserve = 2 * PAGE_SIZE;
	rx = io_opaque_rx_limit(store);
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		data = opaque_vector_data(test, store, cases[i].nr);
		before = atomic64_read(&store->bytes);
		KUNIT_ASSERT_EQ(test, io_opaque_vector(store, data), 0);
		KUNIT_EXPECT_EQ(test, data->nr, cases[i].nr);
		KUNIT_EXPECT_EQ(test, data->vec_bytes, cases[i].bytes);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), before + cases[i].bytes);
		/* The pages are not counted again: the charge of the version is theirs alone. */
		KUNIT_EXPECT_EQ(test, data->charge, (u64)cases[i].nr * PAGE_SIZE);
		wakes = atomic_long_read(&io_opaque_budget_wakes);
		io_opaque_data_put(store, data);
		/* The vector, records and pages go back at once, with one look at the waiters. */
		KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_budget_wakes) - wakes, 1L);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	}

	/* The vector is not capture: it is built when the capture limit is reached. */
	data = opaque_vector_data(test, store, 1);
	before = atomic64_read(&store->bytes);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, rx - before, true));
	KUNIT_EXPECT_EQ(test, io_opaque_vector(store, data), 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), rx + 16);
	io_opaque_uncharge(store, rx - before);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);

	/* It does not fit one byte short, and nothing is left of the attempt. */
	data = opaque_vector_data(test, store, 17);
	before = atomic64_read(&store->bytes);
	store->config.hard_limit = before + 288 - 1 + store->config.reply_reserve;
	KUNIT_EXPECT_EQ(test, io_opaque_vector(store, data), -ENOBUFS);
	KUNIT_EXPECT_PTR_EQ(test, data->bvec, NULL);
	KUNIT_EXPECT_EQ(test, data->vec_bytes, 0U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), before);
	store->config.hard_limit++;
	KUNIT_EXPECT_EQ(test, io_opaque_vector(store, data), 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), before + 288);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/* STAT reports the count of records in 32 bits, clamped. */
static void opaque_stat_extents_clamp(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);

	KUNIT_EXPECT_EQ(test, io_opaque_stat_extents(store), 0U);
	atomic_long_set(&store->extents, 1234);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_extents(store), 1234U);
	atomic_long_set(&store->extents, U32_MAX);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_extents(store), U32_MAX);
	if (BITS_PER_LONG > 32) {
		atomic_long_set(&store->extents, (long)U32_MAX + 1);
		KUNIT_EXPECT_EQ(test, io_opaque_stat_extents(store), U32_MAX);
		atomic_long_set(&store->extents, (long)U32_MAX * 4);
		KUNIT_EXPECT_EQ(test, io_opaque_stat_extents(store), U32_MAX);
	}
	/* The count is a count and is checked against zero when the store is freed. */
	atomic_long_set(&store->extents, 0);
}

/*
 * Nobody but its call may release a pending backing: the call settles it. If it
 * happens, the chunk stays for the call to find, and is not freed under it.
 */
static void opaque_release_pending(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_extent *extent;
	struct io_opaque_block *block;
	struct page *page = alloc_page(GFP_KERNEL);
	LIST_HEAD(list);

	KUNIT_ASSERT_NOT_NULL(test, page);
	extent = io_opaque_extent_new(store, page, 0, 128, 0, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
	list_add(&extent->list, &list);
	block = opaque_block_of(extent);
	extent->backing->ref = IO_OPAQUE_REF_PENDING;
	extent->backing->want = IO_OPAQUE_REF_PAGE;
	extent->backing->units = 0;
	kunit_warning_suppress(test) {
		io_opaque_extent_free(store, extent);
	}
	/* The chunk is there, and so is the page. */
	KUNIT_EXPECT_EQ(test, refcount_read(&block->live), 1);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	/* The page is the extent's and went back; the block stays charged with the chunk. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), OPAQUE_TEST_ALONE);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	kfree(block);
	io_opaque_uncharge(store, OPAQUE_TEST_ALONE);
	put_page(page);
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
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	KUNIT_ASSERT_EQ(test, io_opaque_vector(store, data), 0);
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
	opaque_test_publish(store, 0, 9, data);
	refcount_add(2, &data->refs); /* worker and an old read/send version */
	opaque_test_copy(data, before);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
	KUNIT_EXPECT_EQ(test, op.reserved, 0ULL);
	KUNIT_EXPECT_EQ(test, op.replacement->charge, ALIGN(80000ULL, PAGE_SIZE));
	KUNIT_EXPECT_LE(test, op.replacement->nr, (u32)DIV_ROUND_UP(80000, PAGE_SIZE));
	opaque_test_copy(op.replacement, after);
	KUNIT_EXPECT_MEMEQ(test, before, after, 80000);
	/* The copy is charged as a version, its pages, chunks and vector, besides the original. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			opaque_sparse_held(data) +
			ALIGN(80000, PAGE_SIZE) + opaque_records(op.replacement));
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&op), 0);
	KUNIT_EXPECT_EQ(test, opaque_test_slot(store, 0)->generation, 9ULL);
	KUNIT_EXPECT_PTR_NE(test, opaque_slot_data(store, 0), data);
	io_opaque_data_put(store, data); /* worker */
	io_opaque_compact_end(&op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			opaque_sparse_held(data) +
			ALIGN(80000, PAGE_SIZE) + opaque_records(opaque_slot_data(store, 0)));
	io_opaque_data_put(store, data); /* old read/send */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			ALIGN(80000, PAGE_SIZE) + opaque_records(opaque_slot_data(store, 0)));
	io_opaque_data_put(store, opaque_slot_data(store, 0));
	opaque_slot_set(store, 0, NULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

static void opaque_compact_watermarks(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req op = {
		.store = store, .data = data, .handle = 9ULL << IO_OPAQUE_INDEX_BITS,
	};
	u64 sparse = opaque_sparse_held(data), dense;

	/* Counting the copy would cross the high watermark. */
	store->config.high_watermark = sparse + PAGE_SIZE;
	store->config.low_watermark = PAGE_SIZE;
	opaque_test_publish(store, 0, 9, data);
	refcount_inc(&data->refs); /* worker */
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_NORMAL);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
	dense = ALIGN(80000, PAGE_SIZE) + opaque_records(op.replacement);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)(sparse + dense));
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_NORMAL);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&op), 0);
	io_opaque_data_put(store, op.data); /* worker */
	io_opaque_compact_end(&op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)dense);
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_NORMAL);
	io_opaque_data_put(store, opaque_slot_data(store, 0));
	opaque_slot_set(store, 0, NULL);
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

	opaque_test_slot(store, 0)->generation = IO_OPAQUE_GENERATION_MAX;
	opaque_test_slot(store, 1)->reserved = true;
	opaque_test_use(store, 1);
	KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&op), 0);
	KUNIT_EXPECT_EQ(test, op.handle, (1ULL << IO_OPAQUE_INDEX_BITS) | 2);
	KUNIT_EXPECT_EQ(test, opaque_test_slot(store, 0)->generation, IO_OPAQUE_GENERATION_MAX);
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
	opaque_test_slot(store, 1)->reserved = false;
	opaque_test_release(store, 1);
	KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&op), 0);
	KUNIT_EXPECT_EQ(test, op.slot, 1U);
	KUNIT_EXPECT_EQ(test, opaque_test_slot(store, 1)->generation, 2ULL);
}

static void opaque_generation_width(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_req op = { .store = store };
	struct io_opaque_slot *found, *stale;

	opaque_test_slot(store, 0)->generation = U32_MAX;
	KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&op), 0);
	KUNIT_EXPECT_EQ(test, opaque_test_slot(store, 0)->generation, (u64)U32_MAX + 1);
	spin_lock(&store->tables);
	found = io_opaque_lookup(store, op.handle);
	stale = io_opaque_lookup(store, io_opaque_handle(0, U32_MAX));
	spin_unlock(&store->tables);
	KUNIT_EXPECT_PTR_EQ(test, found, opaque_test_slot(store, 0));
	KUNIT_EXPECT_PTR_EQ(test, stale, NULL);
}

static void opaque_send_last_ownership(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_slot *slot = opaque_test_slot(store, 0);
	struct io_opaque_req send = {
		.store = store, .handle = 9ULL << IO_OPAQUE_INDEX_BITS,
		.offset = 79999, .length = 2, .send_flags = IORING_OPAQUE_SEND_LAST,
	};
	struct io_opaque_req other = {
		.store = store, .handle = send.handle, .length = 1,
		.send_flags = IORING_OPAQUE_SEND_LAST,
	};
	u8 actual[12], expected[12] = { 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

	opaque_test_publish(store, 0, 9, data);
	slot->compacting = true;
	list_add(&slot->candidate, &store->candidates);
	refcount_inc(&data->refs); /* an already issued read/send */
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &send), -ERANGE);
	KUNIT_EXPECT_FALSE(test, send.consumed);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), data);
	KUNIT_EXPECT_FALSE(test, list_empty(&slot->candidate));
	send.offset = 1997;
	send.length = sizeof(actual);
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &send), 0);
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &other), -ESTALE);
	KUNIT_EXPECT_TRUE(test, send.consumed);
	KUNIT_EXPECT_FALSE(test, other.consumed);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
	KUNIT_EXPECT_FALSE(test, slot->compacting);
	KUNIT_EXPECT_TRUE(test, list_empty(&store->candidates));
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	KUNIT_EXPECT_EQ(test, copy_from_iter(actual, sizeof(actual), &send.iter), sizeof(actual));
	KUNIT_EXPECT_MEMEQ(test, actual, expected, sizeof(actual));
	io_opaque_data_put(store, send.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), opaque_sparse_held(data));
	io_opaque_data_put(store, data); /* the issued read/send */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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

	opaque_test_publish(store, 0, 9, data);
	refcount_inc(&data->refs); /* compaction worker */
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	replacement = compact.replacement;
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &send), 0);
	KUNIT_EXPECT_PTR_EQ(test, send.data, replacement);
	KUNIT_EXPECT_EQ(test, refcount_read(&replacement->refs), 1);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&send.iter), (size_t)80000);
	io_opaque_data_put(store, compact.data);
	io_opaque_data_put(store, send.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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

	opaque_test_publish(store, 0, 9, data);
	refcount_inc(&data->refs); /* compaction worker */
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &send), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_compact_publish(&compact), -ESTALE);
	KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&reuse), 0);
	next = opaque_sparse_data(test, store);
	opaque_slot_set(store, 0, next);
	store->nr_objects++;
	opaque_test_slot(store, 0)->reserved = false;
	opaque_test_slot(store, 0)->compacting = true;
	KUNIT_EXPECT_EQ(test, reuse.handle, 10ULL << IO_OPAQUE_INDEX_BITS);
	KUNIT_EXPECT_EQ(test, io_opaque_compact_publish(&compact), -ESTALE);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), next);
	KUNIT_EXPECT_TRUE(test, opaque_test_slot(store, 0)->compacting);
	io_opaque_data_put(store, compact.replacement);
	io_opaque_data_put(store, compact.data);
	io_opaque_data_put(store, send.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), opaque_sparse_held(next));
	spin_lock(&store->tables);
	next = io_opaque_detach(store, opaque_test_slot(store, 0));
	spin_unlock(&store->tables);
	io_opaque_data_put(store, next);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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

	opaque_test_publish(store, 0, 9, data);
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &first), 0);
	refcount_inc(&data->refs); /* compaction worker */
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	replacement = compact.replacement;
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &second), 0);
	spin_lock(&store->tables);
	data = io_opaque_detach(store, opaque_test_slot(store, 0));
	spin_unlock(&store->tables);
	KUNIT_EXPECT_PTR_EQ(test, first.data, compact.data);
	KUNIT_EXPECT_PTR_EQ(test, second.data, replacement);
	KUNIT_EXPECT_FALSE(test, first.consumed);
	KUNIT_EXPECT_FALSE(test, second.consumed);
	io_opaque_data_put(store, data);
	io_opaque_data_put(store, compact.data);
	io_opaque_data_put(store, first.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			ALIGN(80000, PAGE_SIZE) + opaque_records(replacement));
	io_opaque_data_put(store, second.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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

	opaque_test_publish(store, 0, 9, data);
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
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), data);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	/* The followers' own releases wake nobody; draining clears the failure. */
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&b), NULL);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&c), NULL);
	KUNIT_EXPECT_EQ(test, opaque_woken.nr, 2U);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&d, &sk[0]), 1);
	opaque_test_tx_done(&d, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&d), NULL);

	/*
	 * A pin taken before the queue failed stays with the request when the enqueue is refused:
	 * the socket may be owned, where a send puts no version, and the cleanup of the request
	 * gives the pin back (io_opaque_tx_settle()).
	 */
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
	KUNIT_EXPECT_PTR_EQ(test, b.data, data);
	KUNIT_EXPECT_PTR_EQ(test, b.tx, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	io_opaque_data_put(store, b.data);
	b.data = NULL;
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	/* LAST took the object itself: it stays consumed and the request still ends. */
	a.tx->failed = false;
	c.handle = handle;
	c.length = 100;
	c.send_flags = IORING_OPAQUE_SEND_LAST;
	kunit_activate_static_stub(test, io_opaque_send_acquire, opaque_test_acquire_fail);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&c, &sk[1]), -ECANCELED);
	KUNIT_EXPECT_TRUE(test, c.consumed);
	KUNIT_EXPECT_PTR_EQ(test, c.data, data);
	KUNIT_EXPECT_PTR_EQ(test, c.tx, NULL);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
	/* The table's reference is the request's now, and the object is charged until it is put. */
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	KUNIT_EXPECT_GT(test, atomic64_read(&store->bytes), 0LL);
	io_opaque_data_put(store, c.data);
	c.data = NULL;
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, a.tx->admitting, 0U);
	opaque_test_tx_done(&a, false);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&a), NULL);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
}

/*
 * A head that fails a long queue. The followers are the requests of a fake ring, each listed for
 * cancellation as a parked send is and freed when it is cleaned up, so that a walk that touches
 * one after it was woken, which a request need not survive, is caught by KASAN. The walk looks at
 * IO_OPAQUE_WAKE_BATCH of them in a hold of tx_lock and wakes them after it.
 */
struct opaque_fol {
	struct io_opaque_req op;
	struct io_kiocb req;
};

struct opaque_fail {
	struct io_opaque_store *store;
	struct io_ring_ctx *ctx;
	struct sock *sk;
	struct io_opaque_tx *tx;
	struct io_opaque_req head;
	/* The followers not cleaned up yet, and how often each was woken, by whoever did it. */
	struct opaque_fol **fol;
	atomic_t *woken;
	unsigned int nr;
};

static void opaque_fail_free(void *arg)
{
	struct opaque_fail *fx = arg;
	unsigned int i;

	for (i = 0; i < fx->nr; i++)
		kfree(fx->fol[i]);
}

static struct opaque_fail *opaque_fail_new(struct kunit *test, unsigned int nr)
{
	struct opaque_fail *fx = kunit_kzalloc(test, sizeof(*fx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, fx);
	fx->store = opaque_test_store(test);
	fx->ctx = kunit_kzalloc(test, sizeof(*fx->ctx), GFP_KERNEL);
	fx->sk = kunit_kzalloc(test, sizeof(*fx->sk), GFP_KERNEL);
	fx->fol = kunit_kcalloc(test, nr, sizeof(*fx->fol), GFP_KERNEL);
	fx->woken = kunit_kcalloc(test, nr, sizeof(*fx->woken), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fx->ctx);
	KUNIT_ASSERT_NOT_NULL(test, fx->sk);
	KUNIT_ASSERT_NOT_NULL(test, fx->fol);
	KUNIT_ASSERT_NOT_NULL(test, fx->woken);
	fx->nr = nr;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_fail_free, fx), 0);
	mutex_init(&fx->ctx->uring_lock);
	fx->ctx->flags = IORING_SETUP_DEFER_TASKRUN;
	return fx;
}

/* The queue of the socket with its head admitted and all the followers parked behind it. */
static void opaque_fail_fill(struct kunit *test, struct opaque_fail *fx)
{
	unsigned int i;

	mpscq_init(&fx->ctx->work_list, &fx->ctx->work_head);
	INIT_LIST_HEAD(&fx->ctx->opaque_waits);
	opaque_test_tx_op(&fx->head, fx->store);
	KUNIT_ASSERT_EQ(test, io_opaque_tx_admit(&fx->head, fx->sk), 1);
	fx->tx = fx->head.tx;
	for (i = 0; i < fx->nr; i++) {
		struct opaque_fol *f = kzalloc(sizeof(*f), GFP_KERNEL);

		KUNIT_ASSERT_NOT_NULL(test, f);
		fx->fol[i] = f;
		atomic_set(&fx->woken[i], 0);
		opaque_test_tx_op(&f->op, fx->store);
		f->op.req = &f->req;
		f->req.ctx = fx->ctx;
		f->req.async_data = &f->op;
		f->req.cqe.user_data = i + 1;
		KUNIT_ASSERT_EQ(test, io_opaque_tx_admit(&f->op, fx->sk), 0);
		KUNIT_ASSERT_EQ(test, f->op.phase, IO_OPAQUE_SEND_WAIT);
		mutex_lock(&fx->ctx->uring_lock);
		list_add_tail(&f->op.cancel, &fx->ctx->opaque_waits);
		mutex_unlock(&fx->ctx->uring_lock);
	}
}

/*
 * The cleanup of follower @i once it has been woken: off the ring's list, out of its queue, and
 * freed. Says whom the release handed the queue to, which for a canceled follower is nobody.
 */
static struct io_opaque_req *opaque_fail_end(struct opaque_fail *fx, unsigned int i)
{
	struct opaque_fol *f = fx->fol[i];
	struct io_opaque_req *next;

	mutex_lock(&fx->ctx->uring_lock);
	list_del_init(&f->op.cancel);
	f->op.phase = IO_OPAQUE_IDLE;
	mutex_unlock(&fx->ctx->uring_lock);
	next = io_opaque_tx_release(&f->op);
	WRITE_ONCE(fx->fol[i], NULL);
	kfree(f);
	return next;
}

#define OPAQUE_FAIL_NR		(3 * IO_OPAQUE_WAKE_BATCH + 5)
/* A follower of the second batch, which the hook cancels before the walk gets to it. */
#define OPAQUE_FAIL_CUT		(IO_OPAQUE_WAKE_BATCH + 8)

static struct {
	struct kunit *test;
	struct opaque_fail *fx;
	/* Wakeups by the walk, and by the cancellation that the hook starts. */
	unsigned int walk, cancel;
	/* The most followers canceled and not woken yet at a wakeup; wakeups with tx_lock held. */
	unsigned int queued, locked;
	/* Cleanups that handed the queue to a follower. */
	unsigned int promoted;
	bool inside, drain;
} opaque_fail_hook;

static void opaque_fail_ready(struct io_opaque_req *op, int result)
{
	struct opaque_fail *fx = opaque_fail_hook.fx;
	struct kunit *test = opaque_fail_hook.test;
	struct io_cancel_data cd = { .ctx = fx->ctx };
	unsigned int i = op->req->cqe.user_data - 1, k, queued = 0, walk;
	struct io_opaque_req j;

	KUNIT_EXPECT_EQ(test, result, -ECANCELED);
	KUNIT_EXPECT_EQ(test, atomic_inc_return(&fx->woken[i]), 1);
	/*
	 * What follows takes tx_lock, so with it held it is a deadlock and not a failure. Where a
	 * spinlock cannot be told held, opaque_test_held() says it is not, and this is not checked.
	 */
	if (opaque_test_held(&fx->store->tx_lock)) {
		opaque_fail_hook.locked++;
		return;
	}
	if (opaque_fail_hook.inside) {
		opaque_fail_hook.cancel++;
		return;
	}
	walk = opaque_fail_hook.walk++;
	for (k = 0; k < fx->nr; k++)
		if (fx->fol[k] && fx->fol[k]->op.canceled && !atomic_read(&fx->woken[k]))
			queued++;
	opaque_fail_hook.queued = max(opaque_fail_hook.queued, queued);

	if (walk == IO_OPAQUE_WAKE_BATCH - 1) {
		/* The first batch is woken and the second not looked at: the walk left tx_lock. */
		opaque_fail_hook.inside = true;
		KUNIT_EXPECT_TRUE(test, fx->tx->failed);
		KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(fx->store), 1U);
		/* A frame that comes now is refused, as it is for the whole walk. */
		opaque_test_tx_op(&j, fx->store);
		KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&j, fx->sk), -ECANCELED);
		KUNIT_EXPECT_PTR_EQ(test, j.tx, NULL);
		/* A cancellation finds a follower that the walk has not reached, and wakes it. */
		cd.data = OPAQUE_FAIL_CUT + 1;
		KUNIT_EXPECT_EQ(test, io_opaque_cancel(fx->ctx, &cd, IO_URING_F_UNLOCKED), 0);
		KUNIT_EXPECT_EQ(test, atomic_read(&fx->woken[OPAQUE_FAIL_CUT]), 1);
		/* And nothing in one that the walk has made queued: it is left to the walk. */
		cd.data = 1;
		KUNIT_EXPECT_EQ(test, io_opaque_cancel(fx->ctx, &cd, IO_URING_F_UNLOCKED), -ENOENT);
		KUNIT_EXPECT_EQ(test, atomic_read(&fx->woken[0]), 1);
		/*
		 * The batch is cleaned up, each as the first of the queue. The last has the walk
		 * next to it, and the followers it has not looked at behind that: none may be
		 * promoted.
		 */
		for (k = 0; k < IO_OPAQUE_WAKE_BATCH; k++)
			if (opaque_fail_end(fx, k))
				opaque_fail_hook.promoted++;
		/* And the one that was canceled out of turn, which was not the first. */
		if (opaque_fail_end(fx, OPAQUE_FAIL_CUT))
			opaque_fail_hook.promoted++;
		opaque_fail_hook.inside = false;
	} else if (walk == IO_OPAQUE_WAKE_BATCH) {
		KUNIT_EXPECT_TRUE(test, fx->tx->failed);
		KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(fx->store), 1U);
	} else if (opaque_fail_hook.drain && walk == OPAQUE_FAIL_NR - 2) {
		/*
		 * The last wakeup: all the followers are woken, and they all go before the walk is
		 * back. The queue is theirs to forget once the walk leaves it, and the walk has to
		 * see that. @op is gone with its follower.
		 */
		for (k = 0; k < fx->nr; k++)
			if (fx->fol[k] && opaque_fail_end(fx, k))
				opaque_fail_hook.promoted++;
	}
}

/*
 * A head ends short, and the frames queued behind it are many more than a batch. Each is woken
 * once; a batch is made queued before any of it is woken, and no more than a batch is; tx_lock is
 * free for the wakeups. The queue stays failed, and in place, until the last follower has gone,
 * be that one of them or the walk. In between, a frame that comes is refused; a cancellation
 * takes a follower that the walk has not got to and finds nothing in one that it has; and the
 * followers that are cleaned up first do not hand the queue to those that are not canceled yet.
 */
static void opaque_tx_failed_long(struct kunit *test)
{
	struct opaque_fail *fx = opaque_fail_new(test, OPAQUE_FAIL_NR);
	struct io_opaque_req next;
	unsigned int drain, i, wrong, left;

	for (drain = 0; drain < 2; drain++) {
		opaque_fail_fill(test, fx);
		memset(&opaque_fail_hook, 0, sizeof(opaque_fail_hook));
		opaque_fail_hook.test = test;
		opaque_fail_hook.fx = fx;
		opaque_fail_hook.drain = drain;
		kunit_activate_static_stub(test, io_opaque_queue_ready, opaque_fail_ready);
		opaque_test_tx_done(&fx->head, false);
		KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&fx->head), NULL);
		kunit_deactivate_static_stub(test, io_opaque_queue_ready);

		KUNIT_EXPECT_PTR_EQ(test, fx->head.tx, NULL);
		KUNIT_EXPECT_TRUE(test, list_empty(&fx->head.send));
		/* Every follower once: the walk woke all but the one the cancellation woke. */
		KUNIT_EXPECT_EQ(test, opaque_fail_hook.walk, (unsigned int)OPAQUE_FAIL_NR - 1);
		KUNIT_EXPECT_EQ(test, opaque_fail_hook.cancel, 1U);
		for (i = 0, wrong = 0; i < fx->nr; i++)
			wrong += atomic_read(&fx->woken[i]) != 1;
		KUNIT_EXPECT_EQ(test, wrong, 0U);
		/* A batch is made queued before it is woken, and not more. */
		KUNIT_EXPECT_GT(test, opaque_fail_hook.queued, 0U);
		KUNIT_EXPECT_LE(test, opaque_fail_hook.queued, (unsigned int)IO_OPAQUE_WAKE_BATCH);
		KUNIT_EXPECT_EQ(test, opaque_fail_hook.locked, 0U);
		KUNIT_EXPECT_EQ(test, opaque_fail_hook.promoted, 0U);

		if (drain) {
			/* The walk was last out: the queue is idle and forgets the failure. */
			KUNIT_EXPECT_FALSE(test, fx->tx->failed);
			KUNIT_EXPECT_TRUE(test, list_empty(&fx->tx->requests));
			KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(fx->store), 0U);
			KUNIT_EXPECT_EQ(test, fx->store->nr_txs_idle, 1U);
		} else {
			/* The followers still queued keep the queue failed, in the tree. */
			left = OPAQUE_FAIL_NR - IO_OPAQUE_WAKE_BATCH - 1;
			KUNIT_EXPECT_TRUE(test, fx->tx->failed);
			KUNIT_EXPECT_EQ(test, list_count_nodes(&fx->tx->requests), left);
			KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(fx->store), 1U);
			opaque_test_tx_op(&next, fx->store);
			KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&next, fx->sk), -ECANCELED);
			for (i = 0; i < fx->nr; i++)
				if (fx->fol[i])
					KUNIT_EXPECT_PTR_EQ(test, opaque_fail_end(fx, i), NULL);
			KUNIT_EXPECT_FALSE(test, fx->tx->failed);
			KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(fx->store), 0U);
		}
		/* The queue takes a frame again. */
		opaque_test_tx_op(&next, fx->store);
		KUNIT_ASSERT_EQ(test, io_opaque_tx_admit(&next, fx->sk), 1);
		opaque_test_tx_done(&next, true);
		KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&next), NULL);
		KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(fx->store), 0U);
	}
}

#define OPAQUE_FAIL_TASKS	3
#define OPAQUE_FAIL_SMP_NR	1024
#define OPAQUE_FAIL_SMP_ROUNDS	100

struct opaque_fail_smp {
	struct opaque_fail *fx;
	atomic_t ready, go, stop;
	/* Cleanups begun, and those that handed the queue to a follower. */
	atomic_t done, promoted;
	/* Followers that a cancellation woke; frames refused by the queue, and let in too soon. */
	atomic_t canceled, refused, early;
	/* Followers woken twice, and anything else that is not as it should be. */
	atomic_t bad;
	struct completion fin[OPAQUE_FAIL_TASKS];
	u32 seed;
};

static void opaque_fail_start(struct opaque_fail_smp *s)
{
	atomic_inc(&s->ready);
	while (!atomic_read(&s->go))
		cond_resched();
}

/*
 * The walk's wakeup, made a little slower than the cleanups that follow the wakeups: they have
 * the time to run in the windows that the walk leaves between batches, which they otherwise
 * seldom do. The wakeup itself is the real one.
 */
static void opaque_fail_slow_ready(struct io_opaque_req *op, int result)
{
	struct kunit *test = opaque_fail_hook.test;

	kunit_deactivate_static_stub(test, io_opaque_queue_ready);
	io_opaque_queue_ready(op, result);
	kunit_activate_static_stub(test, io_opaque_queue_ready, opaque_fail_slow_ready);
	if (!(++opaque_fail_hook.walk & 7))
		udelay(10);
}

/* The task of the ring: what wakes a follower reaches it, and it cleans the follower up. */
static int opaque_fail_completer(void *data)
{
	struct opaque_fail_smp *s = data;
	struct opaque_fail *fx = s->fx;

	opaque_fail_start(s);
	while (atomic_read(&s->done) < fx->nr && !atomic_read(&s->stop)) {
		struct llist_node *node = mpscq_pop(&fx->ctx->work_list, &fx->ctx->work_head);
		struct io_kiocb *req;
		unsigned int i;

		if (!node) {
			cond_resched();
			continue;
		}
		req = container_of(node, struct io_kiocb, io_task_work.node);
		i = req->cqe.user_data - 1;
		if (atomic_inc_return(&fx->woken[i]) != 1) {
			atomic_inc(&s->bad);
			break;
		}
		atomic_inc(&s->done);
		if (opaque_fail_end(fx, i))
			atomic_inc(&s->promoted);
	}
	complete(&s->fin[0]);
	return 0;
}

/* Cancellations of followers picked at random, from some time into the walk. */
static int opaque_fail_canceler(void *data)
{
	struct opaque_fail_smp *s = data;
	struct opaque_fail *fx = s->fx;
	struct io_cancel_data cd = { .ctx = fx->ctx };
	struct rnd_state rs;
	unsigned int n;

	prandom_seed_state(&rs, s->seed);
	opaque_fail_start(s);
	udelay(prandom_u32_state(&rs) % 64);
	for (n = 0; n < fx->nr && !atomic_read(&s->stop); n++) {
		cd.data = 1 + prandom_u32_state(&rs) % fx->nr;
		if (!io_opaque_cancel(fx->ctx, &cd, IO_URING_F_UNLOCKED))
			atomic_inc(&s->canceled);
	}
	complete(&s->fin[1]);
	return 0;
}

/*
 * Frames that come while the queue is failed, which refuses them; one that comes when it is
 * empty goes in, and no follower may be left by then.
 */
static int opaque_fail_joiner(void *data)
{
	struct opaque_fail_smp *s = data;
	struct opaque_fail *fx = s->fx;

	opaque_fail_start(s);
	/* A frame that came before the queue failed would be a follower, with nobody to wake it. */
	while (!READ_ONCE(fx->tx->failed) && !atomic_read(&s->stop))
		cond_resched();
	while (!atomic_read(&s->stop)) {
		struct io_opaque_req j;
		int ret;

		opaque_test_tx_op(&j, fx->store);
		ret = io_opaque_tx_join(&j, fx->sk);
		if (ret == -ECANCELED) {
			atomic_inc(&s->refused);
			cond_resched();
			continue;
		}
		if (ret < 0 || j.tx != fx->tx)
			atomic_inc(&s->bad);
		else if (atomic_read(&s->done) != fx->nr)
			atomic_inc(&s->early);
		if (ret >= 0 && io_opaque_tx_release(&j))
			atomic_inc(&s->bad);
		break;
	}
	complete(&s->fin[2]);
	return 0;
}

/*
 * The same on several CPUs, with the real wakeups: the walk, the cancellations of a ring
 * and the cleanups that follow them, and frames that join, all at once. No follower is woken
 * twice or lost, no cleanup hands the queue to a follower, and a frame is not let in while
 * one is left. It needs two CPUs to race on.
 */
static void opaque_tx_failed_smp(struct kunit *test)
{
	static int (*const fn[OPAQUE_FAIL_TASKS])(void *) = {
		opaque_fail_completer, opaque_fail_canceler, opaque_fail_joiner,
	};
	struct opaque_fail *fx;
	struct opaque_fail_smp *s;
	unsigned int round, canceled = 0, refused = 0;

	opaque_race_needs_cpus(test);
	fx = opaque_fail_new(test, OPAQUE_FAIL_SMP_NR);
	s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, s);
	s->fx = fx;
	memset(&opaque_fail_hook, 0, sizeof(opaque_fail_hook));
	opaque_fail_hook.test = test;
	for (round = 0; round < OPAQUE_FAIL_SMP_ROUNDS; round++) {
		struct task_struct *task[OPAQUE_FAIL_TASKS];
		unsigned int i, wrong;

		opaque_fail_fill(test, fx);
		atomic_set(&s->ready, 0);
		atomic_set(&s->go, 0);
		atomic_set(&s->stop, 0);
		atomic_set(&s->done, 0);
		atomic_set(&s->promoted, 0);
		atomic_set(&s->canceled, 0);
		atomic_set(&s->refused, 0);
		atomic_set(&s->early, 0);
		atomic_set(&s->bad, 0);
		s->seed = 0x9e3779b1 * (round + 1);
		for (i = 0; i < OPAQUE_FAIL_TASKS; i++) {
			init_completion(&s->fin[i]);
			task[i] = kthread_create(fn[i], s, "opaque_fail%u", i);
			if (IS_ERR(task[i])) {
				while (i--)
					kthread_stop(task[i]);
				KUNIT_FAIL(test, "no task for the race");
				return;
			}
		}
		for (i = 0; i < OPAQUE_FAIL_TASKS; i++)
			wake_up_process(task[i]);
		/* The others run when the walk starts, not some time after it has ended. */
		while (atomic_read(&s->ready) < OPAQUE_FAIL_TASKS)
			cond_resched();
		atomic_set(&s->go, 1);
		opaque_test_tx_done(&fx->head, false);
		kunit_activate_static_stub(test, io_opaque_queue_ready, opaque_fail_slow_ready);
		if (io_opaque_tx_release(&fx->head))
			atomic_inc(&s->bad);
		kunit_deactivate_static_stub(test, io_opaque_queue_ready);
		if (!wait_for_completion_timeout(&s->fin[0], 10 * HZ)) {
			KUNIT_FAIL(test, "round %u: %d of %u followers cleaned up", round,
				   atomic_read(&s->done), fx->nr);
			atomic_set(&s->stop, 1);
			wait_for_completion(&s->fin[0]);
		}
		atomic_set(&s->stop, 1);
		wait_for_completion(&s->fin[1]);
		wait_for_completion(&s->fin[2]);

		for (i = 0, wrong = 0; i < fx->nr; i++)
			wrong += atomic_read(&fx->woken[i]) != 1 || fx->fol[i];
		canceled += atomic_read(&s->canceled);
		refused += atomic_read(&s->refused);
		if (wrong || atomic_read(&s->promoted) || atomic_read(&s->bad) ||
		    atomic_read(&s->early) || !mpscq_empty(&fx->ctx->work_list) ||
		    fx->tx->failed || opaque_test_txs_busy(fx->store)) {
			KUNIT_FAIL(test,
				   "round %u: %u followers woken other than once or left, %d promoted, %d bad, %d early, queue %s",
				   round, wrong, atomic_read(&s->promoted), atomic_read(&s->bad),
				   atomic_read(&s->early),
				   fx->tx->failed ? "failed" : "not drained");
			return;
		}
	}
	kunit_info(test,
		   "%u rounds of %u followers on %u CPUs: %u woken by a cancellation, %u frames refused",
		   OPAQUE_FAIL_SMP_ROUNDS, OPAQUE_FAIL_SMP_NR, num_online_cpus(), canceled,
		   refused);
	KUNIT_EXPECT_GT(test, canceled, 0U);
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

/*
 * The helper sees a hold of a spinlock and the end of it. If it cannot, the expectations that
 * rest on it prove nothing here, and this is where that shows.
 */
static void opaque_lock_held_check(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	bool held;

	if (opaque_test_lock_state(&store->tables) == OPAQUE_LOCK_UNKNOWN)
		kunit_skip(test,
			   "no lockdep, and a spinlock has no state to read: uniprocessor without CONFIG_DEBUG_SPINLOCK");
	KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
	spin_lock(&store->tables);
	/* A failed expectation allocates, which a spinlock does not allow: look after the hold. */
	held = opaque_test_held(&store->tables);
	spin_unlock(&store->tables);
	KUNIT_EXPECT_TRUE(test, held);
	KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
}

static void opaque_test_sock_release(void *sock)
{
	sock_release(sock);
}

/*
 * Lock order: tables alone around the handle, for a send that consumes its object (any other
 * takes no lock for it); tx_lock before wait_lock to park and wake; and, for an inline send,
 * which joins and takes its references once it owns the socket, the socket before tx_lock,
 * wait_lock and tables, none of which is held when the socket is taken.
 */
static void opaque_tx_lock_order(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_test_object(test, store);
	struct sock *sk = kunit_kzalloc(test, sizeof(*sk), GFP_KERNEL);
	struct io_opaque_req a, b, c, d, e;
	struct socket *sock;
	opaque_test_tx_op(&a, store);
	opaque_test_tx_op(&b, store);
	a.handle = io_opaque_handle(0, 9);
	b.handle = a.handle;
	a.length = 100;
	b.length = 100;
	/* The head links with tx_lock alone, and takes its references later, with no lock. */
	KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&a, sk), IO_OPAQUE_TX_SETTLE);
	lockdep_assert_none_held_once();
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_settle_head(&a), 0);
	lockdep_assert_none_held_once();
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	/* A follower takes its references before it parks, with no lock, and then tx_lock. */
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

	/*
	 * With the socket owned, as an inline send joins: a real socket, whose lockdep class is
	 * the sends'. The sends consume their objects, as it is they that take tables. The head
	 * links (the socket, then tx_lock) and takes its references (the socket, then tables), a
	 * frame behind it takes its references and parks (the socket, then tables, then tx_lock
	 * and wait_lock), and the head hands the queue on, all before the socket is let go. The
	 * reverse order of any of them is a splat here and in every other test that takes the
	 * socket.
	 */
	KUNIT_ASSERT_EQ(test, sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock),
			0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_sock_release, sock), 0);
	opaque_test_publish(store, 1, 7, opaque_sparse_data(test, store));
	opaque_test_publish(store, 2, 5, opaque_sparse_data(test, store));
	opaque_test_tx_op(&c, store);
	opaque_test_tx_op(&d, store);
	opaque_test_tx_op(&e, store);
	c.handle = io_opaque_handle(0, 9);
	d.handle = io_opaque_handle(1, 7);
	e.handle = io_opaque_handle(2, 5);
	c.send_flags = IORING_OPAQUE_SEND_LAST;
	d.send_flags = IORING_OPAQUE_SEND_LAST;
	e.send_flags = IORING_OPAQUE_SEND_LAST;
	c.length = 100;
	d.length = 100;
	e.length = 100;
	lock_sock(sock->sk);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&c, sock->sk), IO_OPAQUE_TX_SETTLE);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_settle_head(&c), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&d, sock->sk), IO_OPAQUE_TX_WAIT);
	KUNIT_EXPECT_EQ(test, d.phase, IO_OPAQUE_SEND_WAIT);
	opaque_test_tx_done(&c, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&c), &d);
	/* A frame that was parked and is looked at again, still not the head, parks again. */
	d.phase = IO_OPAQUE_IDLE;
	KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&d, sock->sk), IO_OPAQUE_TX_HEAD);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&e, sock->sk), IO_OPAQUE_TX_WAIT);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&e, sock->sk), IO_OPAQUE_TX_WAIT);
	opaque_test_tx_done(&d, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&d), &e);
	e.phase = IO_OPAQUE_IDLE;
	KUNIT_EXPECT_EQ(test, io_opaque_tx_join(&e, sock->sk), IO_OPAQUE_TX_HEAD);
	opaque_test_tx_done(&e, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&e), NULL);
	release_sock(sock->sk);
	lockdep_assert_none_held_once();
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	io_opaque_data_put(store, c.data);
	io_opaque_data_put(store, d.data);
	io_opaque_data_put(store, e.data);

	spin_lock(&store->tables);
	data = io_opaque_detach(store, opaque_test_slot(store, 0));
	spin_unlock(&store->tables);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), data);
	/* A queue that took a head that failed this way takes the next frame as its head. */
	opaque_test_tx_op(&follower, store);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&follower, sk), IO_OPAQUE_TX_HEAD);
	opaque_test_tx_done(&follower, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&follower), NULL);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);

	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), c ? NULL : data);

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
			opaque_test_free(store, 0);
		}
		/* A LAST frame's put freed the object, which nobody else held. */
		KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	opaque_test_scratch(test, &stream);
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
	io_opaque_capture_release(&stream);
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
	opaque_test_publish(store, 0, 9, data);
	refcount_inc(&data->refs);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	KUNIT_EXPECT_TRUE(test, compact.replacement->forward_only);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	/* The classification is advisory: an ordinary send pins a version. */
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &send), 0);
	KUNIT_EXPECT_FALSE(test, send.consumed);
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &last), 0);
	KUNIT_EXPECT_TRUE(test, last.consumed);
	KUNIT_EXPECT_TRUE(test, send.data->forward_only);
	KUNIT_EXPECT_PTR_EQ(test, send.data, last.data);
	io_opaque_data_put(store, compact.data);
	io_opaque_compact_end(&compact);
	io_opaque_data_put(store, send.data);
	io_opaque_data_put(store, last.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	/* The limit holds the two reserved pages and the record of the first extent, no more. */
	store->config.hard_limit = 2 * PAGE_SIZE + OPAQUE_TEST_ALONE;
	page = alloc_page(GFP_KERNEL_ACCOUNT);
	KUNIT_ASSERT_NOT_NULL(test, page);
	extent = io_opaque_extent_new(store, page, 0, PAGE_SIZE, 0, IO_OPAQUE_EXT_RESERVED);
	KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
	list_add_tail(&extent->list, &op.data->extents);
	op.create_reserved -= page_size(page);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)2 * PAGE_SIZE + OPAQUE_TEST_ALONE);
	page = alloc_page(GFP_KERNEL_ACCOUNT);
	KUNIT_ASSERT_NOT_NULL(test, page);
	extent = io_opaque_extent_new(store, page, 0, 1, PAGE_SIZE, IO_OPAQUE_EXT_RESERVED);
	KUNIT_EXPECT_EQ(test, PTR_ERR(extent), -ENOBUFS);
	/* The record that did not fit left nothing charged and nothing counted. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)2 * PAGE_SIZE + OPAQUE_TEST_ALONE);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 1);
	put_page(page);
	io_opaque_create_release(&op);
	KUNIT_EXPECT_EQ(test, op.create_reserved, 0ULL);
	KUNIT_EXPECT_PTR_EQ(test, op.data, NULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	opaque_test_slot(store, op.slot)->generation++;
	KUNIT_EXPECT_EQ(test, io_opaque_publish(&op, &flags), -ESTALE);
	opaque_test_slot(store, op.slot)->generation--;
	store->dead = true;
	KUNIT_EXPECT_EQ(test, io_opaque_publish(&op, &flags), -ECANCELED);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, op.slot), NULL);
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
	opaque_test_slot(store, 0)->generation = IO_OPAQUE_GENERATION_MAX;
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
	extent = io_opaque_extent_new(store, page, 0, 1, 0, IO_OPAQUE_EXT_RESERVED);
	KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
	op.create_reserved -= page_size(page);
	list_add_tail(&extent->list, &op.data->extents);
	op.data->length = 1;
	op.data->dense = true;
	KUNIT_ASSERT_EQ(test, io_opaque_vector(store, op.data), 0);
	store->policy.flags = IORING_OPAQUE_POLICY_F_AUTO_COMPACT;
	KUNIT_ASSERT_EQ(test, io_opaque_publish(&op, &flags), 1);
	KUNIT_EXPECT_EQ(test, flags, 0ULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&store->candidates));
	KUNIT_EXPECT_FALSE(test, op.slot_reserved);
	spin_lock(&store->tables);
	data = io_opaque_detach(store, opaque_test_slot(store, op.slot));
	spin_unlock(&store->tables);
	io_opaque_data_put(store, data);
	io_opaque_create_release(&op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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

/* A store that io_opaque_stop() can stop: it has the work and the cache that the stop drains. */
static void opaque_test_stoppable(struct kunit *test, struct io_opaque_store *store)
{
	INIT_LIST_HEAD(&store->copies);
	INIT_DELAYED_WORK(&store->compact_work, io_opaque_compact_work);
	INIT_DELAYED_WORK(&store->retry_work, io_opaque_retry_work);
	opaque_test_cache(test, store, 2);
}

struct opaque_test_params {
	unsigned long cache_bytes;
	unsigned int unit_order;
	unsigned int inline_compact_max;
	unsigned int send_copy_max;
	bool copy_per_page;
	bool send_managed;
	int send_poison;
	void (*send_part)(const struct msghdr *msg, const struct io_opaque_msg_ubuf *mub,
			  bool owned);
};

static void opaque_test_params_restore(void *arg)
{
	struct opaque_test_params *saved = arg;

	io_opaque_cache_bytes = saved->cache_bytes;
	io_opaque_unit_order = saved->unit_order;
	io_opaque_inline_compact_max = saved->inline_compact_max;
	io_opaque_send_copy_max = saved->send_copy_max;
	io_opaque_copy_per_page = saved->copy_per_page;
	io_opaque_send_managed = saved->send_managed;
	io_opaque_send_poison = saved->send_poison;
	io_opaque_send_part = saved->send_part;
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
	saved->send_managed = io_opaque_send_managed;
	saved->send_poison = io_opaque_send_poison;
	saved->send_part = io_opaque_send_part;
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

/* A reference of the test's on each page of an object, to look at them after it is gone. */
struct opaque_page_refs {
	struct page *page[40];
	u32 nr;
};

static void opaque_page_refs_put(void *arg)
{
	struct opaque_page_refs *r = arg;

	while (r->nr)
		put_page(r->page[--r->nr]);
}

static struct opaque_page_refs *opaque_page_refs_take(struct kunit *test,
						      struct io_opaque_data *data)
{
	struct opaque_page_refs *r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, r);
	r->nr = opaque_test_pages(data, r->page, ARRAY_SIZE(r->page));
	for (i = 0; i < r->nr; i++)
		get_page(r->page[i]);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_page_refs_put, r), 0);
	return r;
}

/*
 * An skb without a ubuf that a reader frees on another CPU than the one that allocated it is not
 * freed then: it is queued for that CPU (skb_attempt_defer_free()), which frees it in its next
 * NET_RX softirq, and on a quiet system that can be a long time off. A case that waits for the
 * page references of such an skb to go raises the softirq on every CPU between its looks.
 */
static void opaque_rx_kick(void *unused)
{
	raise_softirq_irqoff(NET_RX_SOFTIRQ);
}

/*
 * TCP, the peer's queue and the ubufs have let go of the pages: the test's
 * reference is left, and the backing's if @held, the object being still there.
 */
static bool opaque_pages_settle(const struct opaque_page_refs *r, bool held)
{
	int tries;
	u32 i;

	for (tries = 0; tries < 100; tries++) {
		for (i = 0; i < r->nr; i++)
			if (page_count(r->page[i]) != 1 + held)
				break;
		if (i == r->nr)
			return true;
		on_each_cpu(opaque_rx_kick, NULL, 1);
		msleep(20);
	}
	return false;
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
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), opaque_sparse_held(data));
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
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), opaque_sparse_held(data));
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
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	s64 sparse = opaque_sparse_held(first), packed = ALIGN(80000, PAGE_SIZE), records;
	u64 drained = io_opaque_stat_read(IO_OPAQUE_STAT_CACHE_DRAINED);
	struct page *units[3];
	int i;

	opaque_test_cache(test, store, 4);
	INIT_LIST_HEAD(&store->copies);
	INIT_DELAYED_WORK(&store->compact_work, io_opaque_compact_work);
	INIT_DELAYED_WORK(&store->retry_work, io_opaque_retry_work);
	for (i = 0; i < 2; i++) {
		opaque_test_publish(store, i, 9, i ? second : first);
	}
	refcount_inc(&first->refs); /* compaction */
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
	/* The copy is two units and a tail: a chunk of its own for each, and the vector. */
	records = opaque_records(op.replacement);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 2 * sparse + packed + records);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), packed);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&op), 0);
	io_opaque_data_put(store, op.data);
	io_opaque_compact_end(&op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), sparse + packed + records);
	/* FREE: the units retire idle and leave the hard-limit charge. */
	spin_lock(&store->tables);
	dense = io_opaque_detach(store, opaque_test_slot(store, 0));
	spin_unlock(&store->tables);
	io_opaque_data_put(store, dense);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), sparse);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 2U);
	io_opaque_unit_put(store, opaque_test_unit(test));
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 3U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), sparse);
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
	dense = opaque_slot_data(store, 1);
	refcount_inc(&dense->refs); /* in-flight send */
	KUNIT_ASSERT_EQ(test, opaque_test_pages(dense, units, 3), 3U);
	get_page(units[0]);
	io_opaque_stop(store);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 0U);
	KUNIT_EXPECT_EQ(test, store->cache.nr_cooling, 0U);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_CACHE_DRAINED) - drained, 1ULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), packed + records);
	/* The send completes after shutdown: its units are freed, not cached. */
	io_opaque_data_put(store, dense);
	KUNIT_EXPECT_EQ(test, page_count(units[0]), 1);
	put_page(units[0]);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 0U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/* Registration samples the tunables, bounds the cache and locks its memory. */
static void opaque_cache_store_lifecycle(struct kunit *test)
{
	struct io_ring_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct io_uring_opaque_config cfg = {
		.hard_limit = 64 * PAGE_SIZE, .compact_headroom = 4 * OPAQUE_TEST_UNIT,
		.max_object_size = SZ_64K, .max_objects = 4, .max_streams = 1,
		.max_extents = 64,
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
		extent = io_opaque_extent_new(store, page, offset, len, data->length, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
		list_add_tail(&extent->list, &data->extents);
		data->length += len;
	}
	KUNIT_ASSERT_EQ(test, io_opaque_vector(store, data), 0);
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
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_UNIT);
	KUNIT_EXPECT_EQ(test, store->cache.hits, 1ULL);
	opaque_test_copy(op.data, after);
	KUNIT_EXPECT_TRUE(test, !memcmp(pattern, after, op.length));
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), (s64)op.length);
	io_opaque_create_release(&op);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 1U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	const struct kernel_param *managed = opaque_test_param("io_uring_opaque.send_managed");
	const struct kernel_param *body_copied =
		opaque_test_param("io_uring_opaque.stat_body_copied");
	static const char * const send_stats[] = {
		"io_uring_opaque.stat_send_managed_skbs",
		"io_uring_opaque.stat_send_folio_refs",
		"io_uring_opaque.stat_send_frag_fallback",
		"io_uring_opaque.stat_send_ubuf_copied",
		"io_uring_opaque.stat_send_admit_fast",
		"io_uring_opaque.stat_send_admit_slow",
		"io_uring_opaque.stat_send_ubuf_reused",
	};
	const struct kernel_param *framing_copied =
		opaque_test_param("io_uring_opaque.stat_framing_copied");
	struct kobject *kobj = kset_find_obj(module_kset, "io_uring_opaque");
	char *buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	unsigned int i;
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
	KUNIT_ASSERT_NOT_NULL(test, managed);
	KUNIT_ASSERT_NOT_NULL(test, body_copied);
	KUNIT_ASSERT_NOT_NULL(test, framing_copied);
	KUNIT_EXPECT_EQ(test, order->perm, (u16)0644);
	KUNIT_EXPECT_EQ(test, copy_max->perm, (u16)0644);
	KUNIT_EXPECT_EQ(test, managed->perm, (u16)0644);
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
	/* Managed fragments are on by default, and off for the next send once it is said. */
	KUNIT_EXPECT_TRUE(test, io_opaque_send_managed);
	KUNIT_EXPECT_EQ(test, managed->ops->set("N", managed), 0);
	KUNIT_EXPECT_FALSE(test, io_opaque_send_managed);
	/* The copy threshold takes effect at the next send; its counters are read-only. */
	KUNIT_EXPECT_EQ(test, copy_max->ops->set("2048", copy_max), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_send_copy_max, 2048U);
	KUNIT_EXPECT_EQ(test, body_copied->ops->set("1", body_copied), -EPERM);
	KUNIT_EXPECT_EQ(test, framing_copied->ops->set("1", framing_copied), -EPERM);
	io_opaque_count(IO_OPAQUE_STAT_BODY_COPIED, 7);
	KUNIT_EXPECT_GT(test, body_copied->ops->get(buf, body_copied), 0);
	KUNIT_EXPECT_EQ(test, kstrtou64(strim(buf), 10, &value), 0);
	KUNIT_EXPECT_EQ(test, value, io_opaque_stat_read(IO_OPAQUE_STAT_BODY_COPIED));
	/* The counters of managed sends are there to be read, and to be read only. */
	for (i = 0; i < ARRAY_SIZE(send_stats); i++) {
		const struct kernel_param *stat = opaque_test_param(send_stats[i]);

		KUNIT_ASSERT_NOT_NULL(test, stat);
		KUNIT_EXPECT_EQ(test, stat->perm, (u16)0444);
		KUNIT_EXPECT_EQ(test, stat->ops->set("1", stat), -EPERM);
		io_opaque_count(IO_OPAQUE_STAT_SEND_MANAGED_SKBS + i, 3);
		KUNIT_EXPECT_GT(test, stat->ops->get(buf, stat), 0);
		KUNIT_EXPECT_EQ(test, kstrtou64(strim(buf), 10, &value), 0);
		KUNIT_EXPECT_EQ(test, value,
				io_opaque_stat_read(IO_OPAQUE_STAT_SEND_MANAGED_SKBS + i));
	}
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
		io_opaque_send_cleanup(&p->req);
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
	bool prepped;
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
 * Requests that are prepared, issued and cleaned up by the functions that the core calls, on the
 * fixture's ring. The ring knows @store as store 1 and keeps the requests that are cleaned up for
 * the next prep, as a ring does.
 */
static void opaque_wire_prep_cleanup(void *arg)
{
	struct io_ring_ctx *ctx = arg;

	io_opaque_cache_free(ctx);
	xa_destroy(&ctx->opaque_stores);
}

static void opaque_wire_prep_setup(struct kunit *test, struct opaque_wire *w,
				   struct io_opaque_store *store)
{
	unsigned int i;

	if (w->prepped)
		return;
	w->prepped = true;
	xa_init(&w->ctx->opaque_stores);
	KUNIT_ASSERT_FALSE(test, io_opaque_cache_init(w->ctx));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_wire_prep_cleanup, w->ctx), 0);
	KUNIT_ASSERT_EQ(test, xa_err(xa_store(&w->ctx->opaque_stores, 1, store, GFP_KERNEL)), 0);
	for (i = 0; i < ARRAY_SIZE(w->req); i++)
		w->req[i].opcode = IORING_OP_OPAQUE_OBJ_SEND;
}

/*
 * Prep request @i as its SQE would: a range, or the vector of @len entries at @addr, with
 * a frame at @addr3 if there is one.
 */
static struct io_opaque_req *opaque_wire_prep(struct kunit *test, struct opaque_wire *w, int i,
					      u16 ioprio, u64 addr, u64 off, u32 len, u64 addr3)
{
	struct io_uring_sqe sqe = {
		.opaque_store_id = 1, .ioprio = ioprio, .addr = addr, .off = off, .len = len,
		.addr3 = addr3,
	};

	w->req[i].flags = 0;
	w->req[i].async_data = NULL;
	memset(&w->req[i].cqe, 0, sizeof(w->req[i].cqe));
	KUNIT_ASSERT_EQ(test, io_opaque_send_prep(&w->req[i], &sqe), 0);
	KUNIT_ASSERT_NOT_NULL(test, w->req[i].async_data);
	return w->req[i].async_data;
}

/* What the core does with a request that has ended or failed: its cleanup, under the ring lock. */
static void opaque_wire_req_cleanup(struct opaque_wire *w, int i)
{
	mutex_lock(&w->ctx->uring_lock);
	io_opaque_send_cleanup(&w->req[i]);
	mutex_unlock(&w->ctx->uring_lock);
	w->req[i].flags &= ~REQ_F_NEED_CLEANUP;
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
	opaque_test_publish(store, 0, 9, data);
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
	spin_lock(&store->tables);
	data = io_opaque_detach(store, opaque_test_slot(store, 0));
	spin_unlock(&store->tables);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/*
 * The rules for the entries of a vector, as prep applied them when it copied
 * all of them and walked the copy (io_opaque_vec_check(), which the copy-in
 * took over): the verdict, the body the entries add up to and the largest of
 * them. The copy-in is held to it. A handle may repeat, and a range may too.
 */
static int opaque_vec_model(const struct io_uring_opaque_send_entry *ent, u32 nr, u32 *total,
			    u32 *max_len)
{
	u64 sum = 0, end;
	u32 longest = 0, i;

	if (!nr || nr > IORING_OPAQUE_SEND_VEC_MAX)
		return -EINVAL;
	for (i = 0; i < nr; i++) {
		if (ent[i].__resv || !ent[i].len ||
		    check_add_overflow(ent[i].off, (u64)ent[i].len, &end))
			return -EINVAL;
		sum += ent[i].len;
		if (sum > INT_MAX)
			return -EINVAL;
		longest = max(longest, ent[i].len);
	}
	*total = sum;
	*max_len = longest;
	return 0;
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
	u32 total, longest;

	KUNIT_ASSERT_NOT_NULL(test, op);
	op->store = store;
	op->send_flags = IORING_OPAQUE_SEND_VECTOR | flags;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_vec_free, op), 0);
	KUNIT_ASSERT_EQ(test, io_opaque_vec_grow(op, nr, nr, 0), 0);
	memcpy(op->vec->ent, ent, array_size(nr, sizeof(*ent)));
	KUNIT_ASSERT_EQ(test, opaque_vec_model(ent, nr, &total, &longest), 0);
	io_opaque_vec_commit(op, nr, total, longest);
	return op;
}

/* A request with a block for @nr entries and none in it, as prep leaves it before the copy. */
static struct io_opaque_req *opaque_vec_blank(struct kunit *test, struct io_opaque_store *store,
					      u32 nr)
{
	struct io_opaque_req *op = kunit_kzalloc(test, sizeof(*op), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, op);
	op->store = store;
	op->send_flags = IORING_OPAQUE_SEND_VECTOR;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_vec_free, op), 0);
	KUNIT_ASSERT_EQ(test, io_opaque_vec_grow(op, nr, nr, 0), 0);
	return op;
}

/*
 * User memory for entries: ten pages hold the most that a vector can with room to
 * spare. Page 4 is unmapped, for a copy to fault on when it reaches it.
 */
#define OPAQUE_UVEC_PAGES	10
#define OPAQUE_UVEC_HOLE	4

static unsigned long opaque_uvec_map(struct kunit *test)
{
	unsigned long addr = kunit_vm_mmap(test, NULL, 0, OPAQUE_UVEC_PAGES * PAGE_SIZE,
					   PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0);

	if (!addr || addr >= TASK_SIZE)
		kunit_skip(test, "no userspace mapping for the entries");
	return addr;
}

static void opaque_uvec_hole(struct kunit *test, unsigned long addr)
{
	KUNIT_ASSERT_EQ(test, vm_munmap(addr + OPAQUE_UVEC_HOLE * PAGE_SIZE, PAGE_SIZE), 0);
}

static void opaque_uvec_put(struct kunit *test, unsigned long addr, const void *src, size_t bytes)
{
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)addr, src, bytes), 0UL);
}

/* Copy @nr entries from user address @addr into the block of @op, which holds 0xa5 at first. */
static int opaque_vec_copy(struct io_opaque_req *op, unsigned long addr, u32 nr)
{
	memset(op->vec->ent, 0xa5, array_size(nr, sizeof(*op->vec->ent)));
	op->vec->nr = 0xa5a5a5a5;
	op->vec->nr_segs = 0xa5a5a5a5;
	op->vec->pinned = 0xa5a5a5a5;
	op->vec->max_len = 0xa5a5a5a5;
	op->length = 0xa5a5a5a5;
	op->handle = 0xa5a5a5a5a5a5a5a5ULL;
	op->offset = 0xa5a5a5a5a5a5a5a5ULL;
	return io_opaque_vec_copy_in(op, (const void __user *)addr, nr);
}

/*
 * The copy-in of @nr entries at @addr, which hold @ent, gives the verdict of the
 * model, and a request made of the vector when it is a yes: the entries as they
 * are, the body of their sum, the largest of them, nothing pinned.
 */
static void opaque_vec_expect_copy(struct kunit *test, struct io_opaque_req *op,
				   unsigned long addr, const struct io_uring_opaque_send_entry *ent,
				   u32 nr)
{
	u32 total = 0, longest = 0;
	int want = opaque_vec_model(ent, nr, &total, &longest);

	KUNIT_ASSERT_EQ(test, opaque_vec_copy(op, addr, nr), want);
	if (want)
		return;
	KUNIT_EXPECT_MEMEQ(test, op->vec->ent, ent, array_size(nr, sizeof(*ent)));
	KUNIT_EXPECT_EQ(test, op->length, total);
	KUNIT_EXPECT_EQ(test, op->vec->max_len, longest);
	KUNIT_EXPECT_EQ(test, op->vec->nr, nr);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 0U);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
	KUNIT_EXPECT_EQ(test, op->handle, 0ULL);
	KUNIT_EXPECT_EQ(test, op->offset, 0ULL);
}

/* Make the block hold @segs segments, so that admission has no need to grow it. */
static void opaque_vec_room(struct kunit *test, struct io_opaque_req *op, u32 segs)
{
	KUNIT_ASSERT_EQ(test, io_opaque_vec_grow(op, op->vec->nr, segs, op->vec->nr), 0);
}

/*
 * Admission of a vector as io_opaque_tx_admit() does it, without the queue: the
 * references are taken by io_opaque_send_acquire(), with tables for a LAST vector
 * and without it for any other, and the block is grown and the cursor built
 * without it.
 */
static int opaque_vec_admit(struct io_opaque_req *op)
{
	return io_opaque_vec_finish(op, opaque_test_acquire(op->store, op));
}

/*
 * A vector whose admission failed (or that did not get so far as to be admitted) has not given back
 * what it pinned: @pinned entries of its block are set, each run of entries that name one handle
 * holding a reference, and no cursor was made (io_opaque_vec_acquire()). The cleanup of the request
 * gives them back, without the socket; this does what it does.
 */
static void opaque_vec_failed(struct kunit *test, struct io_opaque_req *op, u32 pinned)
{
	KUNIT_EXPECT_EQ(test, op->vec->pinned, pinned);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 0U);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
}

/*
 * The segments of a range as the first version of the vectored send found
 * them: its object walked from the first segment for every range. Counted when
 * @bv is NULL, else stored there.
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
		extent = io_opaque_extent_new(store, page, off, len, data->length, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
		list_add_tail(&extent->list, &data->extents);
		data->length += len;
	}
	KUNIT_ASSERT_EQ(test, io_opaque_vector(store, data), 0);
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
 * one that frees the version, and must not be made with the socket owned. A
 * reference that a send drops with a decrement under the socket does not come
 * through here: a send that did not consume its object does that for each of its
 * references but the last of a version, and every reference of a LAST send is put
 * after the socket is released.
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

/*
 * The puts seen since the spy started are @puts, of which @frees found the last reference
 * of a version, and none was made with the socket owned.
 */
static void opaque_put_spy_expect(struct kunit *test, unsigned int puts, unsigned int frees)
{
	unsigned int i, last = 0, seen = min_t(unsigned int, opaque_put_spy.n,
					       ARRAY_SIZE(opaque_put_spy.refs));

	KUNIT_EXPECT_EQ(test, opaque_put_spy.n, puts);
	for (i = 0; i < seen; i++) {
		last += opaque_put_spy.refs[i] == 1;
		KUNIT_EXPECT_FALSE(test, opaque_put_spy.owned[i]);
	}
	KUNIT_EXPECT_EQ(test, last, frees);
}

/*
 * A security hook like AppArmor's, which finds the address of the socket with
 * getname() and so takes its lock: it has to be asked with the socket not
 * owned, or it would wait for the owner, here the same task, for ever. This one
 * allows the send, counts the times it is asked, and takes the lock only if it
 * can, so that a send that gets this wrong fails the test and does not hang it.
 */
static unsigned int opaque_hook_calls;

static int opaque_test_security_probe(struct socket *sock, struct msghdr *msg,
				      const struct io_opaque_req *op)
{
	struct kunit *test = kunit_get_current_test();
	struct sock *sk = sock->sk;
	bool owned = sock_owned_by_user_nocheck(sk);
	struct msghdr want;

	KUNIT_EXPECT_FALSE(test, owned);
	/*
	 * The message is the issue's own, zeroed once (by io_opaque_send(), even where the stack
	 * held something else): the flags of the issue and nothing more. io-wq issues with
	 * IO_URING_F_UNLOCKED and the rest are not allowed to block.
	 */
	memset(&want, 0, sizeof(want));
	want.msg_flags = msg->msg_flags;
	KUNIT_EXPECT_MEMEQ(test, msg, &want, sizeof(want));
	KUNIT_EXPECT_EQ(test, msg->msg_flags & ~MSG_DONTWAIT,
			(MSG_NOSIGNAL | op->msg_flags) & ~MSG_DONTWAIT);
	opaque_hook_calls++;
	if (!owned) {
		lock_sock(sk);
		release_sock(sk);
	}
	return 0;
}

/* One send over the wire fixture: the shape of the body and of the frame, and how it is issued. */
struct opaque_send_case {
	bool vector;		/* three ranges of one handle, else one range */
	bool last;
	bool copy;		/* TCP copies the body instead of splicing it */
	bool unlocked;		/* issued as io-wq does, else inline under the ring lock */
	bool frame_only;	/* a reply made of the frame alone */
	bool unmanaged;		/* send_managed is N: a body that is not copied is spliced */
	bool prep;		/* the request is made by prep, from an SQE, and cleaned up after */
	u32 prefix, suffix;
};

/*
 * What each part of a frame was sent with, as io_opaque_send_part() saw it just
 * before the part went to TCP: the message, the msg_ubuf, and whether the socket
 * was owned.
 */
#define OPAQUE_PARTS_MAX	8

static struct {
	unsigned int n;
	struct {
		struct msghdr msg;
		struct io_opaque_msg_ubuf mub;
		bool own_ubuf, owned;
	} part[OPAQUE_PARTS_MAX];
} opaque_parts;

static void opaque_test_part(const struct msghdr *msg, const struct io_opaque_msg_ubuf *mub,
			     bool owned)
{
	unsigned int n = opaque_parts.n++;

	if (n >= OPAQUE_PARTS_MAX)
		return;
	opaque_parts.part[n].msg = *msg;
	opaque_parts.part[n].mub = *mub;
	opaque_parts.part[n].own_ubuf = msg->msg_ubuf == &mub->uarg;
	opaque_parts.part[n].owned = owned;
}

/* How the body of a send goes to TCP. */
enum {
	OPAQUE_BODY_COPY,	/* TCP copies it */
	OPAQUE_BODY_SPLICE,	/* page references (MSG_SPLICE_PAGES) */
	OPAQUE_BODY_MANAGED,	/* managed fragments, with the send's ubuf and fill */
};

struct opaque_part_want {
	u32 flags;
	size_t bytes;
	bool kvec;
};

/*
 * The parts a frame is sent in, with what each is sent with. The flags of the
 * issue are in all of them. The prefix, and the body when a suffix follows, say
 * that more is coming. A body that is not copied says how it is not: MSG_ZEROCOPY
 * with the send's ubuf and fill, or MSG_SPLICE_PAGES. The parts after the body
 * have none of that, and neither has any field but these: no address, no control
 * data, no ubuf, no fill.
 */
static unsigned int opaque_parts_want(struct opaque_part_want *w, u32 base, u32 prefix, u32 body,
				      u32 suffix, int mode)
{
	unsigned int n = 0;

	if (!body) {
		w[n++] = (struct opaque_part_want) {
			.flags = base, .bytes = prefix + suffix, .kvec = true,
		};
		return n;
	}
	if (prefix)
		w[n++] = (struct opaque_part_want) {
			.flags = base | MSG_MORE, .bytes = prefix, .kvec = true,
		};
	w[n] = (struct opaque_part_want) { .flags = base, .bytes = body };
	if (suffix)
		w[n].flags |= MSG_MORE;
	if (mode == OPAQUE_BODY_MANAGED)
		w[n].flags |= MSG_ZEROCOPY;
	else if (mode == OPAQUE_BODY_SPLICE)
		w[n].flags |= MSG_SPLICE_PAGES;
	n++;
	if (suffix)
		w[n++] = (struct opaque_part_want) { .flags = base, .bytes = suffix, .kvec = true };
	return n;
}

/*
 * The parts that the send logged are the parts of the frame, each with its own message:
 * everything in it that is not set for the part is zero, to the last padding byte.
 */
static void opaque_parts_expect(struct kunit *test, const struct opaque_part_want *want,
				unsigned int n, int mode, bool owned)
{
	unsigned int i;

	KUNIT_ASSERT_EQ(test, opaque_parts.n, n);
	for (i = 0; i < n; i++) {
		bool managed = mode == OPAQUE_BODY_MANAGED && !want[i].kvec && want[i].bytes;
		const struct msghdr *got = &opaque_parts.part[i].msg;
		struct msghdr zero;

		memset(&zero, 0, sizeof(zero));
		zero.msg_iter = got->msg_iter;
		zero.msg_flags = want[i].flags;
		if (managed) {
			zero.msg_ubuf = got->msg_ubuf;
			zero.sg_from_iter = io_opaque_sg_from_iter;
		}
		KUNIT_EXPECT_MEMEQ_MSG(test, got, &zero, sizeof(zero), "part %u of %u", i, n);
		KUNIT_EXPECT_EQ_MSG(test, iov_iter_count(&got->msg_iter), want[i].bytes,
				    "part %u of %u", i, n);
		KUNIT_EXPECT_EQ_MSG(test, iov_iter_is_kvec(&got->msg_iter), want[i].kvec,
				    "part %u of %u", i, n);
		KUNIT_EXPECT_EQ_MSG(test, opaque_parts.part[i].owned, owned, "part %u of %u", i, n);
		KUNIT_EXPECT_EQ_MSG(test, opaque_parts.part[i].own_ubuf, managed,
				    "part %u of %u", i, n);
		if (managed) {
			const struct io_opaque_msg_ubuf *mub = &opaque_parts.part[i].mub;

			/*
			 * The msg_ubuf as TCP has it: its ops, no count, and the flags that
			 * the store adds to its ubufs. These stores bind no instance, and
			 * add none.
			 */
			KUNIT_EXPECT_PTR_EQ(test, mub->uarg.ops, &io_opaque_msg_ubuf_ops);
			KUNIT_EXPECT_EQ(test, refcount_read(&mub->uarg.refcnt), 0);
			KUNIT_EXPECT_EQ(test, mub->uarg.flags, (u8)0);
		}
	}
}

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
	struct opaque_part_want want_part[3];
	unsigned int nwant;
	int mode;
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

	if (c->prep) {
		/* The request as prep makes it from the SQE: all that a send reads is set there. */
		u16 ioprio = (c->vector ? IORING_OPAQUE_SEND_VECTOR : 0) |
			     (c->last ? IORING_OPAQUE_SEND_LAST : 0);
		unsigned long user = 0;

		opaque_wire_prep_setup(test, w, store);
		if (c->vector) {
			user = opaque_uvec_map(test);
			opaque_uvec_put(test, user, ent, sizeof(ent));
		}
		/* A frame is a pointer that prep does not follow: the first issue imports it. */
		op = opaque_wire_prep(test, w, 0, ioprio,
				      c->vector ? user : c->frame_only ? 0 : handle,
				      c->vector || c->frame_only ? 0 : 4196,
				      c->vector ? ARRAY_SIZE(ent) : body,
				      c->prefix || c->suffix ? 0x1000 : 0);
	} else {
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
	}
	if (c->prefix || c->suffix) {
		/* As the first issue leaves a request that has imported its frame. */
		op->framing = c->prep ? kmemdup(frame, c->prefix + c->suffix, GFP_KERNEL) : frame;
		KUNIT_ASSERT_NOT_NULL(test, op->framing);
		op->prefix_length = c->prefix;
		op->suffix_length = c->suffix;
		op->total_length = total;
		op->resolved = true;
	} else if (!c->prep) {
		io_opaque_send_resolve(op);
	}
	KUNIT_ASSERT_TRUE(test, op->resolved);

	io_opaque_send_copy_max = c->copy ? SZ_1M : 0;
	io_opaque_send_managed = !c->unmanaged;
	memset(&opaque_parts, 0, sizeof(opaque_parts));
	WRITE_ONCE(io_opaque_send_part, opaque_test_part);
	opaque_put_spy_start(test, w->tx->sk);
	if (c->unlocked) {
		ret = io_opaque_send(&w->req[0], IO_URING_F_UNLOCKED);
	} else {
		mutex_lock(&w->ctx->uring_lock);
		ret = io_opaque_send(&w->req[0], IO_URING_F_NONBLOCK);
		mutex_unlock(&w->ctx->uring_lock);
	}
	opaque_put_spy_stop(test);
	WRITE_ONCE(io_opaque_send_part, NULL);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
	/*
	 * LAST holds the table's reference, and its put frees the object: after the socket, as is
	 * the put of the extra reference of each further entry of a vector.
	 */
	opaque_put_spy_expect(test,
			      c->last && !c->frame_only ? (c->vector ? ARRAY_SIZE(ent) : 1) : 0,
			      c->last && !c->frame_only ? 1 : 0);
	KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, (s32)total);
	KUNIT_EXPECT_EQ(test, !!(w->req[0].cqe.flags & IORING_CQE_F_OPAQUE_CONSUMED), c->last);
	KUNIT_EXPECT_EQ(test, op->consumed, c->last);
	/* One call per part of the frame; the frame alone is a single part. */
	parts = c->frame_only ? 1 : !!c->prefix + 1 + !!c->suffix;
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_send_locked_calls) - calls, (long)parts);
	/* Each part had its own message: its flags, and no field of another part's. */
	mode = c->copy ? OPAQUE_BODY_COPY : c->unmanaged ? OPAQUE_BODY_SPLICE : OPAQUE_BODY_MANAGED;
	nwant = opaque_parts_want(want_part, MSG_NOSIGNAL | (c->unlocked ? 0 : MSG_DONTWAIT),
				  c->prefix, body, c->suffix, mode);
	KUNIT_EXPECT_EQ(test, nwant, parts);
	opaque_parts_expect(test, want_part, nwant, mode, true);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_BODY_COPIED) - copied,
			c->copy ? (u64)body : 0);
	/* What the send held it has given back itself: there is no cleanup before this. */
	KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
	KUNIT_EXPECT_PTR_EQ(test, op->data, NULL);
	if (c->vector)
		KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(w->tx->sk));
	/* Cleanup has the frame and the request. */
	if (c->prep)
		opaque_wire_req_cleanup(w, 0);

	opaque_wire_read(test, w, got, 0, total);
	KUNIT_EXPECT_MEMEQ(test, got, want, total);

	if (c->last) {
		/* The send held the only reference: the object went with it. */
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
	} else {
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
		opaque_test_free(store, 0);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	/* Every issue asks a hook that takes the socket lock, once, with the socket not owned. */
	opaque_hook_calls = 0;
	kunit_activate_static_stub(test, io_opaque_send_security, opaque_test_security_probe);
	for (i = 0; i < ARRAY_SIZE(cases); i++)
		opaque_wire_send(test, w, store, &cases[i]);
	KUNIT_EXPECT_EQ(test, opaque_hook_calls, (unsigned int)ARRAY_SIZE(cases));
	kunit_deactivate_static_stub(test, io_opaque_send_security);
}

/*
 * Every part of a frame goes to TCP in a message made for it: the flags of the
 * issue, more to come after the prefix and after a body that has a suffix, and for
 * a body how it is not copied (MSG_ZEROCOPY with the send's ubuf and fill, or
 * MSG_SPLICE_PAGES); and nothing of the part before it, so the prefix and the suffix have no
 * ubuf, no fill and no zerocopy flag after a managed body, and no field of any part but the
 * flags and the data is set. The three ways of sending a body, ranged and vectored, with
 * and without a prefix and a suffix, with the stack of the send full of 0xa5 and
 * without: opaque_wire_send() holds the message of each part to the last byte
 * (opaque_parts_expect()), and the security hook's, which the issue shares with them, to
 * its flags alone.
 */
static void opaque_send_msghdr_parts(struct kunit *test)
{
	static const struct opaque_send_case cases[] = {
		{ },
		{ .prefix = 24 },
		{ .suffix = 8 },
		{ .prefix = 24, .suffix = 8 },
		{ .prefix = 24, .suffix = 8, .last = true },
		{ .prefix = 24, .suffix = 8, .vector = true },
		{ .prefix = 24, .suffix = 8, .copy = true },
		{ .prefix = 24, .suffix = 8, .vector = true, .copy = true },
		{ .prefix = 24, .suffix = 8, .unmanaged = true },
		{ .prefix = 24, .suffix = 8, .vector = true, .unmanaged = true },
		{ .suffix = 8, .unmanaged = true },
		{ .vector = true },
		{ .prefix = 16, .suffix = 16, .frame_only = true },
		{ .prefix = 16, .frame_only = true, .unlocked = true },
		{ .prefix = 24, .unlocked = true },
		{ .prefix = 24, .suffix = 8, .unlocked = true, .vector = true, .last = true },
		/* The request as prep makes it, poisoned as the stack of the send is. */
		{ .prep = true },
		{ .prep = true, .last = true, .unlocked = true },
		{ .prep = true, .prefix = 24, .suffix = 8 },
		{ .prep = true, .prefix = 24, .suffix = 8, .unmanaged = true },
		{ .prep = true, .prefix = 24, .suffix = 8, .copy = true, .unlocked = true },
		{ .prep = true, .prefix = 16, .frame_only = true },
		{ .prep = true, .vector = true },
		{ .prep = true, .vector = true, .last = true, .unlocked = true },
		{ .prep = true, .vector = true, .prefix = 24, .suffix = 8 },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	unsigned int i, poison;

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_test_params_save(test);
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	opaque_hook_calls = 0;
	kunit_activate_static_stub(test, io_opaque_send_security, opaque_test_security_probe);
	for (poison = 0; poison < 2; poison++) {
		io_opaque_send_poison = poison ? 0xa5 : -1;
		for (i = 0; i < ARRAY_SIZE(cases); i++)
			opaque_wire_send(test, w, store, &cases[i]);
	}
	KUNIT_EXPECT_EQ(test, opaque_hook_calls, 2U * ARRAY_SIZE(cases));
	kunit_deactivate_static_stub(test, io_opaque_send_security);
}

/* What happens to a held send's handle while it waits for room. */
enum {
	OPAQUE_EAGAIN_KEEP,
	OPAQUE_EAGAIN_FREE,
	OPAQUE_EAGAIN_COMPACT,
};

/*
 * A frame bigger than the socket has room for: the head queues what fits and
 * returns EAGAIN with everything it holds still held, its references, its
 * place at the head of the queue and its cursor, and a later issue goes on
 * from the cursor until the peer has read it all. The socket is not owned
 * between issues. A send that is not LAST may find its handle freed or
 * compacted meanwhile: the table's reference goes, the send's is the last
 * one the version has, and the issue that completes puts it after the socket.
 */
static void opaque_wire_eagain(struct kunit *test, struct opaque_wire *w,
			       struct io_opaque_store *store, bool vector, bool last,
			       unsigned int action)
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
	struct opaque_page_refs *pages;
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
	pages = opaque_page_refs_take(test, data);
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
		op = opaque_vec_new(test, store, ent, nr, last ? IORING_OPAQUE_SEND_LAST : 0);
	} else {
		op = kunit_kzalloc(test, sizeof(*op), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, op);
		op->store = store;
		op->send_flags = last ? IORING_OPAQUE_SEND_LAST : 0;
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
	if (vector)
		KUNIT_EXPECT_EQ(test, op->vec->pinned, nr);
	else
		KUNIT_EXPECT_PTR_EQ(test, op->data, data);
	/* LAST takes the table's reference for each entry; any other send one for the run. */
	KUNIT_EXPECT_EQ(test, op->consumed, last);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), last ? NULL : data);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), last ? (int)nr : 2);
	KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(w->tx->sk));

	/* The handle goes while the send is held, with nobody to put the version but the send. */
	if (action == OPAQUE_EAGAIN_FREE) {
		opaque_test_free(store, 0);
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
	} else if (action == OPAQUE_EAGAIN_COMPACT) {
		struct io_opaque_req compact = { .store = store, .data = data, .handle = handle };

		refcount_inc(&data->refs); /* compaction worker */
		KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
		KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
		io_opaque_data_put(store, compact.data);
		io_opaque_compact_end(&compact);
		KUNIT_EXPECT_PTR_NE(test, opaque_slot_data(store, 0), data);
	}
	if (action != OPAQUE_EAGAIN_KEEP)
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);

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
	/*
	 * A version the send holds last is freed once, by the issue that completes it, which puts
	 * the other references of a LAST vector after the socket as well.
	 */
	opaque_put_spy_expect(test, last ? nr : action != OPAQUE_EAGAIN_KEEP,
			      last || action != OPAQUE_EAGAIN_KEEP);
	KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, (s32)total);
	KUNIT_EXPECT_EQ(test, op->progress, (u64)total);
	KUNIT_EXPECT_EQ(test, !!(w->req[0].cqe.flags & IORING_CQE_F_OPAQUE_CONSUMED), last);
	KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
	KUNIT_EXPECT_PTR_EQ(test, op->data, NULL);
	if (vector)
		KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(w->tx->sk));
	opaque_wire_read(test, w, got, done, total);
	KUNIT_EXPECT_MEMEQ(test, got, want, total);
	/*
	 * Nothing that TCP or a ubuf held is still held, or was put twice: the pages
	 * of the version are the test's and, while the table has it, the table's.
	 */
	KUNIT_EXPECT_TRUE(test, opaque_pages_settle(pages, !last && action == OPAQUE_EAGAIN_KEEP));

	/* What the table still holds, the version or its compacted copy, is the table's alone. */
	if (!last && action != OPAQUE_EAGAIN_FREE) {
		KUNIT_ASSERT_NOT_NULL(test, opaque_slot_data(store, 0));
		KUNIT_EXPECT_EQ(test, refcount_read(&opaque_slot_data(store, 0)->refs), 1);
		opaque_test_free(store, 0);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

static void opaque_send_locked_eagain(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	unsigned int action, vector;

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_test_params_save(test);
	opaque_wire_init(test, w);
	/* One range, then a LAST vector, on the same connection. */
	opaque_wire_eagain(test, w, store, false, false, OPAQUE_EAGAIN_KEEP);
	opaque_wire_eagain(test, w, store, true, true, OPAQUE_EAGAIN_KEEP);
	/* A send held while its handle is freed or compacted ends up with the last reference. */
	for (action = OPAQUE_EAGAIN_FREE; action <= OPAQUE_EAGAIN_COMPACT; action++)
		for (vector = 0; vector < 2; vector++)
			opaque_wire_eagain(test, w, store, vector, false, action);
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
	KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
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
	KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
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
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
			opaque_put_spy_expect(test, last ? (vector ? ARRAY_SIZE(ent) : 1) : 0,
					      last ? 1 : 0);
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
			KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
			KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(w->tx->sk));
			if (last) {
				KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
			} else {
				KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
				opaque_test_free(store, 0);
			}
			KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
		}
	}
}

/*
 * A LAST vector over two versions, one of them twice: each of its references is
 * put after the socket, the last of each version frees it, and none is put twice.
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
	/* A's first reference, B's only one and A's last: B and A are freed after the socket. */
	opaque_put_spy_expect(test, 3, 2);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 1), NULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

static atomic_t opaque_test_proto_calls = ATOMIC_INIT(0);

/* A refusal for the next send that asks. */
static struct {
	int err;
	bool armed;
} opaque_refuse;

static int opaque_refuse_now(void)
{
	if (!opaque_refuse.armed)
		return 0;
	opaque_refuse.armed = false;
	return opaque_refuse.err;
}

static int opaque_test_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
{
	int ret = opaque_refuse_now();

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
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	opaque_test_streams(test, store, 1);
	rings = kunit_kzalloc(test, struct_size(rings, cqes, 8), GFP_KERNEL);
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
	KUNIT_EXPECT_NOT_NULL(test, stream->pending);
	KUNIT_EXPECT_PTR_EQ(test, sk->sk_rx_owner, &stream->owner);
	KUNIT_EXPECT_PTR_EQ(test, opaque_test_stream_slot(store, 0)->stream, stream);
	KUNIT_EXPECT_EQ(test, store->nr_streams, 1U);
	KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
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
	KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
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
	opaque_test_stream_slot(store, 0)->stream = NULL;
	store->nr_streams--;
	io_opaque_stream_put(stream);
	io_opaque_stream_put(stream);
}

/*
 * The entries are checked as a whole, without looking at any handle: as they
 * are read from user memory, and with the answers the model gives. Where the
 * number of entries is wrong prep says so before it reads any
 * (opaque_send_vec_prep_errors()).
 */
static void opaque_send_vec_check(struct kunit *test)
{
	static const struct io_uring_opaque_send_entry bad[] = {
		{ .handle = 1, .off = 0, .len = 0 },			/* empty range */
		{ .handle = 1, .off = 0, .len = 10, .__resv = 1 },	/* reserved field in use */
		{ .handle = 1, .off = U64_MAX, .len = 1 },		/* end past U64_MAX */
		{ .handle = 1, .off = U64_MAX - 9, .len = 10 },
		{ .handle = 1, .off = 0, .len = U32_MAX },		/* more than a send holds */
	};
	struct io_uring_opaque_send_entry *ent = kunit_kcalloc(test, IORING_OPAQUE_SEND_VEC_MAX,
							       sizeof(*ent), GFP_KERNEL);
	struct io_opaque_req *op = opaque_vec_blank(test, opaque_test_store(test),
						    IORING_OPAQUE_SEND_VEC_MAX);
	unsigned long addr = opaque_uvec_map(test);
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, ent);
	for (i = 0; i < IORING_OPAQUE_SEND_VEC_MAX; i++)
		ent[i] = (struct io_uring_opaque_send_entry) { .handle = 1, .off = i, .len = 10 };
	/* One entry to the maximum; the body is the sum. */
	opaque_uvec_put(test, addr, ent, array_size(IORING_OPAQUE_SEND_VEC_MAX, sizeof(*ent)));
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, 1), 0);
	KUNIT_EXPECT_EQ(test, op->length, 10U);
	KUNIT_EXPECT_EQ(test, op->vec->max_len, 10U);
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, IORING_OPAQUE_SEND_VEC_MAX), 0);
	KUNIT_EXPECT_EQ(test, op->length, 10U * IORING_OPAQUE_SEND_VEC_MAX);
	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		ent[1] = bad[i];
		opaque_uvec_put(test, addr, ent, 2 * sizeof(*ent));
		KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, 2), -EINVAL);
		/* Only the entries named are looked at. */
		KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, 1), 0);
	}
	/* A range may end exactly at U64_MAX. */
	ent[1] = (struct io_uring_opaque_send_entry) {
		.handle = 1, .off = U64_MAX - 10, .len = 10,
	};
	opaque_uvec_put(test, addr, ent, 2 * sizeof(*ent));
	opaque_vec_expect_copy(test, op, addr, ent, 2);
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, 2), 0);
	/* The body fits INT_MAX, which the whole frame must too. */
	ent[0].len = 1U << 30;
	ent[1] = (struct io_uring_opaque_send_entry) { .handle = 1, .off = 0, .len = 1U << 30 };
	opaque_uvec_put(test, addr, ent, 2 * sizeof(*ent));
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, 2), -EINVAL);
	ent[1].len--;
	opaque_uvec_put(test, addr, ent, 2 * sizeof(*ent));
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, 2), 0);
	KUNIT_EXPECT_EQ(test, op->length, (u32)INT_MAX);
	KUNIT_EXPECT_EQ(test, op->vec->max_len, 1U << 30);
	/* A handle, even a range, may repeat. */
	ent[0] = (struct io_uring_opaque_send_entry) { .handle = 1, .off = 0, .len = 100 };
	ent[1] = ent[0];
	ent[2] = (struct io_uring_opaque_send_entry) { .handle = 2, .off = 5, .len = 300 };
	ent[3] = ent[0];
	opaque_uvec_put(test, addr, ent, 4 * sizeof(*ent));
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, 4), 0);
	KUNIT_EXPECT_EQ(test, op->length, 600U);
	KUNIT_EXPECT_EQ(test, op->vec->max_len, 300U);
}

/*
 * What is copied in is the vector: every field of every entry, the body of
 * their sum and the largest of them, and the request holds nothing yet. Vectors
 * of every size, with a body that just fits, entries that end at U64_MAX and
 * repeated handles and ranges.
 */
static void opaque_vec_copy_in(struct kunit *test)
{
	static const u32 sizes[] = { 1, 2, 3, 7, 8, 9, 63, 64, 255, 256, 1023,
				     IORING_OPAQUE_SEND_VEC_MAX };
	struct io_uring_opaque_send_entry *ent = kunit_kcalloc(test, IORING_OPAQUE_SEND_VEC_MAX,
							       sizeof(*ent), GFP_KERNEL);
	struct io_opaque_req *op = opaque_vec_blank(test, opaque_test_store(test),
						    IORING_OPAQUE_SEND_VEC_MAX);
	unsigned long addr = opaque_uvec_map(test);
	unsigned int k;
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, ent);
	for (k = 0; k < ARRAY_SIZE(sizes); k++) {
		/* Different in every field, so that a field read twice, or not at all, shows. */
		for (i = 0; i < sizes[k]; i++)
			ent[i] = (struct io_uring_opaque_send_entry) {
				.handle = 0x0123456789abcdefULL + i * 0x1000001ULL,
				.off = 0x123456789aULL + i,
				.len = 1 + (i * 37) % 5000,
			};
		opaque_uvec_put(test, addr, ent, sizes[k] * sizeof(*ent));
		opaque_vec_expect_copy(test, op, addr, ent, sizes[k]);
		KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, sizes[k]), 0);
	}
	/* The largest body there is: the most entries, and a sum of exactly INT_MAX. */
	for (i = 0; i < IORING_OPAQUE_SEND_VEC_MAX; i++)
		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = 1, .off = 0, .len = 1U << 21,
		};
	ent[IORING_OPAQUE_SEND_VEC_MAX - 1].len--;
	opaque_uvec_put(test, addr, ent, IORING_OPAQUE_SEND_VEC_MAX * sizeof(*ent));
	opaque_vec_expect_copy(test, op, addr, ent, IORING_OPAQUE_SEND_VEC_MAX);
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, IORING_OPAQUE_SEND_VEC_MAX), 0);
	KUNIT_EXPECT_EQ(test, op->length, (u32)INT_MAX);
	KUNIT_EXPECT_EQ(test, op->vec->max_len, 1U << 21);
	/* One byte more is too much, in the last entry as in the first. */
	ent[IORING_OPAQUE_SEND_VEC_MAX - 1].len++;
	opaque_uvec_put(test, addr, ent, IORING_OPAQUE_SEND_VEC_MAX * sizeof(*ent));
	opaque_vec_expect_copy(test, op, addr, ent, IORING_OPAQUE_SEND_VEC_MAX);
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, IORING_OPAQUE_SEND_VEC_MAX), -EINVAL);
	/* Entries that end at U64_MAX, a handle that repeats and a range that does. */
	ent[0] = (struct io_uring_opaque_send_entry) { .handle = 7, .off = U64_MAX - 9, .len = 9 };
	ent[1] = (struct io_uring_opaque_send_entry) { .handle = 7, .off = U64_MAX - 9, .len = 9 };
	ent[2] = (struct io_uring_opaque_send_entry) {
		.handle = U64_MAX, .off = U64_MAX - 1, .len = 1,
	};
	ent[3] = (struct io_uring_opaque_send_entry) { .handle = 0, .off = 0, .len = U32_MAX >> 2 };
	opaque_uvec_put(test, addr, ent, 4 * sizeof(*ent));
	opaque_vec_expect_copy(test, op, addr, ent, 4);
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, 4), 0);
	KUNIT_EXPECT_EQ(test, op->vec->max_len, U32_MAX >> 2);
}

/*
 * Each way an entry can be wrong, in the first, a middle and the last place of
 * a vector: a reserved field in use, no bytes, an end past U64_MAX, and a sum
 * past INT_MAX however it is reached. All are -EINVAL, as they were when the
 * copy came before the check, and the answer is the model's.
 */
static void opaque_vec_copy_in_invalid(struct kunit *test)
{
	static const struct io_uring_opaque_send_entry bad[] = {
		{ .handle = 1, .off = 0, .len = 10, .__resv = 1 },
		{ .handle = 1, .off = 0, .len = 10, .__resv = 0x80000000 },
		{ .handle = 1, .off = 0, .len = 10, .__resv = U32_MAX },
		{ .handle = 1, .off = 0, .len = 0 },
		{ .handle = 1, .off = U64_MAX, .len = 1 },
		{ .handle = 1, .off = U64_MAX - 9, .len = 11 },
		{ .handle = 1, .off = 1ULL << 63, .len = 1U << 31 },
		{ .handle = 1, .off = 0, .len = U32_MAX },
		{ .handle = 1, .off = 0, .len = (u32)INT_MAX + 1 },
	};
	static const unsigned int at[] = { 0, 3, 7 };
	struct io_opaque_req *op = opaque_vec_blank(test, opaque_test_store(test), 8);
	struct io_uring_opaque_send_entry ent[8];
	unsigned long addr = opaque_uvec_map(test);
	unsigned int i, k, j;

	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		for (k = 0; k < ARRAY_SIZE(at); k++) {
			for (j = 0; j < ARRAY_SIZE(ent); j++)
				ent[j] = (struct io_uring_opaque_send_entry) {
					.handle = 1, .off = 100 * j, .len = 10 + j,
				};
			ent[at[k]] = bad[i];
			opaque_uvec_put(test, addr, ent, sizeof(ent));
			KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, ARRAY_SIZE(ent)), -EINVAL);
			opaque_vec_expect_copy(test, op, addr, ent, ARRAY_SIZE(ent));
		}
	}
	/* Lengths that add up to a multiple of 2^32, and to one more: the sum is not 32 bits. */
	ent[0] = (struct io_uring_opaque_send_entry) { .handle = 1, .off = 0, .len = 1U << 31 };
	ent[1] = ent[0];
	opaque_uvec_put(test, addr, ent, 2 * sizeof(*ent));
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, 2), -EINVAL);
	ent[0].len = U32_MAX;
	ent[1].len = 2;
	opaque_uvec_put(test, addr, ent, 2 * sizeof(*ent));
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, 2), -EINVAL);
	/*
	 * A sum past INT_MAX from entries that are each fine: eight of 2^28 are a
	 * byte too many, and a byte less in the first, a middle or the last of
	 * them makes INT_MAX exactly.
	 */
	for (j = 0; j < ARRAY_SIZE(ent); j++)
		ent[j] = (struct io_uring_opaque_send_entry) {
			.handle = 1, .off = 0, .len = 1U << 28,
		};
	opaque_uvec_put(test, addr, ent, sizeof(ent));
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, ARRAY_SIZE(ent)), -EINVAL);
	opaque_vec_expect_copy(test, op, addr, ent, ARRAY_SIZE(ent));
	for (k = 0; k < ARRAY_SIZE(at); k++) {
		ent[at[k]].len--;
		opaque_uvec_put(test, addr, ent, sizeof(ent));
		KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, ARRAY_SIZE(ent)), 0);
		KUNIT_EXPECT_EQ(test, op->length, (u32)INT_MAX);
		opaque_vec_expect_copy(test, op, addr, ent, ARRAY_SIZE(ent));
		ent[at[k]].len++;
	}
}

/*
 * A copy-in that reaches memory that is not there says -EFAULT, whatever it has
 * read: the entries before the hole are fine, and so is a vector that ends where
 * the hole starts, an entry that the hole cuts after any number of its bytes
 * (the reserved field is the last four, and is read), an entry that is not valid
 * and is followed by the hole (the copy came first, so the fault was said first),
 * and an address that is not user memory at all. The block is good for the next
 * vector.
 */
static void opaque_vec_copy_in_fault(struct kunit *test)
{
	struct io_opaque_req *op = opaque_vec_blank(test, opaque_test_store(test), 4);
	const struct io_uring_opaque_send_entry good = { .handle = 1, .off = 0, .len = 10 };
	struct io_uring_opaque_send_entry ent[4];
	unsigned long addr = opaque_uvec_map(test), hole, at;
	unsigned int k, d;

	opaque_uvec_hole(test, addr);
	hole = addr + OPAQUE_UVEC_HOLE * PAGE_SIZE;
	for (k = 0; k < ARRAY_SIZE(ent); k++)
		ent[k] = (struct io_uring_opaque_send_entry) {
			.handle = 1, .off = 100 * k, .len = 10 + k,
		};

	/* The vector ends where the hole starts: all of it is there. */
	at = hole - sizeof(ent);
	opaque_uvec_put(test, at, ent, sizeof(ent));
	opaque_vec_expect_copy(test, op, at, ent, ARRAY_SIZE(ent));
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, at, ARRAY_SIZE(ent)), 0);
	/* The hole takes the last d bytes of the last entry; the last four are __resv. */
	for (d = 1; d < sizeof(*ent); d++) {
		at = hole - sizeof(ent) + d;
		opaque_uvec_put(test, at, ent, sizeof(ent) - d);
		KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, at, ARRAY_SIZE(ent)), -EFAULT);
	}
	/* The hole is where entry k begins. */
	for (k = 0; k < ARRAY_SIZE(ent); k++) {
		at = hole - k * sizeof(*ent);
		opaque_uvec_put(test, at, ent, k * sizeof(*ent));
		KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, at, ARRAY_SIZE(ent)), -EFAULT);
	}
	/* An entry that is not valid, then the hole: -EFAULT. Without the hole, -EINVAL. */
	ent[0].len = 0;
	at = hole - 2 * sizeof(*ent);
	opaque_uvec_put(test, at, ent, 2 * sizeof(*ent));
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, at, ARRAY_SIZE(ent)), -EFAULT);
	ent[3].__resv = 1;
	at = hole - sizeof(ent);
	opaque_uvec_put(test, at, ent, sizeof(ent));
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, at, ARRAY_SIZE(ent)), -EINVAL);
	/* Not user memory: null, the end of user space, a vector running off it, kernel space. */
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, 0, 1), -EFAULT);
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, TASK_SIZE - 12, 1), -EFAULT);
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, TASK_SIZE - sizeof(*ent), 2), -EFAULT);
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, TASK_SIZE, 1), -EFAULT);
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, (unsigned long)-24, 1), -EFAULT);
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, (unsigned long)-1, ARRAY_SIZE(ent)), -EFAULT);
	/* The block is as good as it was. */
	opaque_uvec_put(test, addr, &good, sizeof(good));
	KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, addr, 1), 0);
	KUNIT_EXPECT_EQ(test, op->length, 10U);
}

/*
 * The copy-in is the model on vectors that are made up: entries whose fields lie
 * on the edges of the rules, in vectors of up to sixteen, in memory that is all
 * there or runs into the hole. Enough of them go and enough are refused.
 */
static void opaque_vec_copy_in_model(struct kunit *test)
{
	static const u64 offs[] = {
		0, 1, 4095, 1ULL << 32, U64_MAX - 20, U64_MAX - 10, U64_MAX - 9, U64_MAX - 1,
		U64_MAX,
	};
	static const u32 lens[] = {
		0, 1, 2, 10, 4096, 1U << 20, 1U << 27, 1U << 28, 1U << 30, INT_MAX,
		(u32)INT_MAX + 1, U32_MAX,
	};
	struct io_opaque_req *op = opaque_vec_blank(test, opaque_test_store(test), 16);
	struct io_uring_opaque_send_entry ent[16];
	unsigned long addr = opaque_uvec_map(test), hole, at;
	unsigned int iter, valid = 0, invalid = 0, faults = 0;
	u32 i, nr, cut, total, longest;
	struct rnd_state rs;
	bool cut_by_hole;

	opaque_uvec_hole(test, addr);
	hole = addr + OPAQUE_UVEC_HOLE * PAGE_SIZE;
	prandom_seed_state(&rs, 0x5eed1234);
	for (iter = 0; iter < 3000; iter++) {
		nr = 1 + prandom_u32_state(&rs) % ARRAY_SIZE(ent);
		for (i = 0; i < nr; i++) {
			ent[i].handle = prandom_u32_state(&rs) % 4 ?
					prandom_u32_state(&rs) : U64_MAX;
			ent[i].off = prandom_u32_state(&rs) % 5 ?
				     prandom_u32_state(&rs) % 100000 :
				     offs[prandom_u32_state(&rs) % ARRAY_SIZE(offs)];
			ent[i].len = prandom_u32_state(&rs) % 12 ?
				     1 + prandom_u32_state(&rs) % 5000 :
				     lens[prandom_u32_state(&rs) % ARRAY_SIZE(lens)];
			ent[i].__resv = prandom_u32_state(&rs) % 64 ? 0 : prandom_u32_state(&rs);
		}
		switch (prandom_u32_state(&rs) % 4) {
		case 0:		/* in the pages before the hole */
			at = addr + prandom_u32_state(&rs) % (OPAQUE_UVEC_HOLE * PAGE_SIZE -
							      nr * sizeof(*ent));
			opaque_uvec_put(test, at, ent, nr * sizeof(*ent));
			cut_by_hole = false;
			break;
		case 1:		/* the hole takes the end of it */
			cut = 1 + prandom_u32_state(&rs) % (nr * sizeof(*ent) - 1);
			at = hole - cut;
			opaque_uvec_put(test, at, ent, cut);
			cut_by_hole = true;
			break;
		default:	/* in the pages after the hole */
			at = hole + PAGE_SIZE + prandom_u32_state(&rs) % PAGE_SIZE;
			opaque_uvec_put(test, at, ent, nr * sizeof(*ent));
			cut_by_hole = false;
			break;
		}
		if (cut_by_hole) {
			KUNIT_EXPECT_EQ(test, opaque_vec_copy(op, at, nr), -EFAULT);
			faults++;
			continue;
		}
		opaque_vec_expect_copy(test, op, at, ent, nr);
		if (opaque_vec_model(ent, nr, &total, &longest))
			invalid++;
		else
			valid++;
	}
	KUNIT_EXPECT_GT(test, valid, 300U);
	KUNIT_EXPECT_GT(test, invalid, 300U);
	KUNIT_EXPECT_GT(test, faults, 300U);
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
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			ALIGN(80000, PAGE_SIZE) + opaque_records(replacement));
	io_opaque_vec_release(second);
	spin_lock(&store->tables);
	data = io_opaque_detach(store, opaque_test_slot(store, 0));
	spin_unlock(&store->tables);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

static void opaque_test_vec_segs_restore(void *limit)
{
	io_opaque_send_vec_segs = (unsigned long)limit;
}

/*
 * Admission pins every entry, or none: a vector that cannot go consumes nothing and holds
 * nothing, and a LAST vector detaches each handle once, under tables.
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
	/* Entries set at each failure: one past the limit is set, one out of range is not. */
	static const u32 held[4] = { 2, 1, 1, 3 };
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
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), a);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 1), b);
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
			KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, other), errs[i]);
			KUNIT_EXPECT_FALSE(test, other->consumed);
			/* A LAST vector pins nothing; any other leaves its pins to the cleanup. */
			opaque_vec_failed(test, other, last ? 0 : held[i]);
			KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
			KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 1);
			KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), a);
			KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 1), b);
			KUNIT_EXPECT_EQ(test, store->nr_objects, 2U);
		}
	}
	/* The limit is the number of segments that fit. */
	io_opaque_send_vec_segs = 3;
	other = opaque_vec_new(test, store, ent, 3, 0);
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, other), 0);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 3);
	io_opaque_vec_release(other);
	io_opaque_send_vec_segs = IO_OPAQUE_SEND_VEC_SEGS;
	/* A store that has stopped admits nothing. */
	other = opaque_vec_new(test, store, ent, 3, 0);
	store->dead = 1;
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, other), -ESHUTDOWN);
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
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 2), NULL);
	KUNIT_EXPECT_EQ(test, store->nr_objects, 2U);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 2 * opaque_sparse_held(a));

	/* LAST takes each handle once; A's second entry shares the reference. */
	op = opaque_vec_new(test, store, ent, 3, IORING_OPAQUE_SEND_LAST);
	KUNIT_EXPECT_EQ(test, opaque_vec_admit(op), 0);
	/* The cursor is over the pinned versions, whose handles are gone. */
	opaque_vec_expect_cursor(test, op);
	KUNIT_EXPECT_TRUE(test, op->consumed);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 1), NULL);
	KUNIT_EXPECT_EQ(test, store->nr_objects, 0U);
	KUNIT_EXPECT_FALSE(test, opaque_test_used(store, 0));
	KUNIT_EXPECT_FALSE(test, opaque_test_used(store, 1));
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[0], a);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[1], b);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[2], a);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
	KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 2 * opaque_sparse_held(a));
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	const s64 charge = opaque_sparse_held(obj[0]);
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
	opaque_test_free(store, 0);
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
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 2), NULL);
	bytes = atomic64_read(&store->bytes);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, bytes - atomic64_read(&store->bytes), charge);

	/* The one left is released as it always was. */
	opaque_test_free(store, 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
		KUNIT_EXPECT_PTR_NE(test, opaque_slot_data(store, 0), NULL);
	}
	/* LAST took the handle and, with the references, the object. */
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
	KUNIT_EXPECT_EQ(test, store->nr_objects, 0U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/*
 * A queue that fails after the check refuses the vector with every pin it took, which are the
 * request's to give back when it is cleaned up and not the join's, since the socket may be
 * owned; a LAST vector stays consumed.
 */
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
		KUNIT_EXPECT_PTR_EQ(test, v[i]->tx, NULL);
		if (!i) {
			/* A pin for each run of entries, A, B and A again, besides the table's. */
			KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 3);
			KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 2);
			KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), a);
		}
		/* The pins are held still; the cleanup of the request gives them back. */
		KUNIT_EXPECT_EQ(test, v[i]->vec->pinned, (u32)ARRAY_SIZE(ent));
		io_opaque_vec_release(v[i]);
		KUNIT_EXPECT_EQ(test, v[i]->vec->pinned, 0U);
		if (!i) {
			KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
			KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 1);
		}
	}
	/* Both handles went with the LAST vector, and the pins with the failed admission. */
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 1), NULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, head.tx->admitting, 0U);
	opaque_test_tx_done(&head, false);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&head), NULL);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
}

/*
 * Pin the object for real, let another ring free it, and fail the queue: the pin is then the last
 * reference of the version when the join refuses the frame. The put of the other ring is not the
 * request's, and the spy of the test (opaque_put_spy_start()) does not count it.
 */
static int opaque_test_acquire_free_fail(struct io_opaque_req *op)
{
	struct kunit *test = kunit_get_current_test();
	int ret;

	kunit_deactivate_static_stub(test, io_opaque_send_acquire);
	ret = io_opaque_send_acquire(op);
	kunit_deactivate_static_stub(test, io_opaque_data_put);
	opaque_test_free(op->store, 0);
	kunit_activate_static_stub(test, io_opaque_data_put, opaque_test_data_put_spy);
	opaque_race_head->tx->failed = true;
	return ret;
}

/*
 * A frame that joins a queue which fails after the frame has taken its version is refused with the
 * version still its own (io_opaque_tx_settle()). An inline issue joins with the socket owned, and
 * a send puts no version then: the last put frees the version, and a free is not for the socket
 * lock to wait out. The reference is the request's to give back when it is cleaned up, with the
 * socket released. The join runs here as that issue runs it, on a real socket that the test owns,
 * with the puts watched. In two cases: the table holds the version as well, so that the reference
 * of the frame is not the last; and another ring frees the object once the frame has taken hold,
 * so that its reference is the last, and the put that gives it back frees the version.
 */
static void opaque_tx_refused_owned(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_req head, refused;
	struct socket *sock;
	unsigned int mode;
	struct sock *sk;

	KUNIT_ASSERT_EQ(test, sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock),
			0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_sock_release, sock), 0);
	sk = sock->sk;
	for (mode = 0; mode < 2; mode++) {
		struct io_opaque_data *data = opaque_test_object(test, store);
		int ret;

		opaque_test_tx_op(&head, store);
		KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(&head, sk), 1);
		opaque_race_head = &head;
		opaque_test_tx_op(&refused, store);
		refused.handle = data->handle;
		refused.offset = 100;
		refused.length = 1000;
		if (mode)
			kunit_activate_static_stub(test, io_opaque_send_acquire,
						   opaque_test_acquire_free_fail);
		else
			kunit_activate_static_stub(test, io_opaque_send_acquire,
						   opaque_test_acquire_fail);
		/* The socket is owned, as an inline issue joins. */
		opaque_put_spy_start(test, sk);
		lock_sock(sk);
		ret = io_opaque_tx_join(&refused, sk);
		release_sock(sk);
		opaque_put_spy_stop(test);
		KUNIT_EXPECT_EQ_MSG(test, ret, -ECANCELED, "mode %u", mode);
		/* Nothing was put: the request has what it took. */
		opaque_put_spy_expect(test, 0, 0);
		KUNIT_EXPECT_PTR_EQ(test, refused.data, data);
		KUNIT_EXPECT_PTR_EQ(test, refused.tx, NULL);
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), mode ? 1 : 2);
		KUNIT_EXPECT_GT(test, atomic64_read(&store->bytes), 0LL);

		/* Its cleanup puts it, with the socket released: in mode 1 that put frees it. */
		opaque_put_spy_start(test, sk);
		io_opaque_data_put(store, refused.data);
		opaque_put_spy_stop(test);
		refused.data = NULL;
		opaque_put_spy_expect(test, 1, mode);
		head.tx->failed = false;
		opaque_test_tx_done(&head, false);
		KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(&head), NULL);
		KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
		if (!mode) {
			KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
			opaque_test_free(store, 0);
		}
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
		KUNIT_EXPECT_EQ(test, store->nr_objects, 0U);
		kvfree_rcu_barrier();
	}
	opaque_expect_lockdep(test, store);
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
	u64 walked, expect = 0, calls = 0;
	struct io_opaque_req *op;
	u32 i, total = 0;
	long seeks;

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
		walked = opaque_seek_steps(step[i].resumes ? step[i - 1].seg : 0, step[i].seg,
					   r->obj[o]->nr);
		expect += walked;
		/* A range that starts where the walk is takes no call to find. */
		calls += !!walked;
	}
	op = opaque_vec_new(test, r->store, ent, nr, 0);
	/* Room enough that admission does not grow the block and walk twice. */
	opaque_vec_room(test, op, 4 * nr + 64);
	walked = atomic_long_read(&io_opaque_vec_stepped);
	seeks = atomic_long_read(&io_opaque_vec_seeks);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	walked = atomic_long_read(&io_opaque_vec_stepped) - walked;
	seeks = atomic_long_read(&io_opaque_vec_seeks) - seeks;
	KUNIT_EXPECT_EQ(test, walked, expect);
	KUNIT_EXPECT_EQ(test, (u64)seeks, calls);
	/* Where the ranges start is where the cursor starts each of them. */
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
		opaque_test_free(r->store, i);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&r->store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&r->store->extents), 0);
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
			/*
			 * After the segments, at the next multiple of eight bytes, in
			 * their allocation: an odd count must not leave them short of it.
			 */
			KUNIT_EXPECT_PTR_EQ(test, (void *)data->anchor,
					    (void *)data->bvec +
					    ALIGN(data->nr * sizeof(*data->bvec), sizeof(u64)));
			KUNIT_EXPECT_TRUE(test,
					  IS_ALIGNED((unsigned long)data->anchor, sizeof(u64)));
			for (k = 0; k < DIV_ROUND_UP(sizes[i], IO_OPAQUE_SEEK_STRIDE); k++)
				KUNIT_EXPECT_EQ(test, data->anchor[k],
						opaque_seg_pos(data, k * IO_OPAQUE_SEEK_STRIDE));
		}
		io_opaque_data_put(store, data);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
		opaque_test_free(store, i);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/*
 * Counting the segments of the ranges stops once the limit is passed: a range
 * over a whole large object, or one that crosses the limit, counts as many as
 * it takes to know, and the vector fails having removed nothing.
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
		KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, op), -EMSGSIZE);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_vec_counted) - counted, 11L);
		opaque_vec_failed(test, op, last ? 0 : 1);
		/* Eight fit; the second range crosses the limit after three more. */
		op = opaque_vec_new(test, store, ent, 2, flags);
		counted = atomic_long_read(&io_opaque_vec_counted);
		KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, op), -EMSGSIZE);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_vec_counted) - counted, 11L);
		/* Both entries name one handle: the second shares the first's reference. */
		opaque_vec_failed(test, op, last ? 0 : 2);
		KUNIT_EXPECT_FALSE(test, op->consumed);
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), data);
	}
	/* At the limit exactly, all of it is counted and the vector goes. */
	io_opaque_send_vec_segs = 8;
	op = opaque_vec_new(test, store, ent, 1, 0);
	opaque_vec_room(test, op, 8);
	counted = atomic_long_read(&io_opaque_vec_counted);
	KUNIT_EXPECT_EQ(test, opaque_vec_admit(op), 0);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_vec_counted) - counted, 8L);
	io_opaque_vec_release(op);
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
		KUNIT_ASSERT_EQ(test, opaque_test_acquire(store, op), 0);
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
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
 * does there; and whether tables was held where the block grew.
 */
static struct {
	void (*between)(struct kunit *test, struct io_opaque_req *op);
	struct io_opaque_req *compact;
	int fail;
	unsigned int calls;
	u32 pinned, segs, cap;
	bool consumed, grow_locked;
	/* The hook is not yet reached; it takes itself out when it is. */
	bool grow_hook;
} opaque_window;

static int opaque_test_grow_window(struct io_opaque_req *op, u32 nr, u32 segs, u32 keep)
{
	struct kunit *test = kunit_get_current_test();

	kunit_deactivate_static_stub(test, io_opaque_vec_grow);
	opaque_window.grow_hook = false;
	opaque_window.calls++;
	opaque_window.grow_locked = opaque_test_held(&op->store->tables);
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

/* Watch admission from here on: the grow, with what is done in the window it leaves. */
static void opaque_window_open(struct kunit *test,
			       void (*between)(struct kunit *test, struct io_opaque_req *op))
{
	memset(&opaque_window, 0, sizeof(opaque_window));
	opaque_window.between = between;
	opaque_window.grow_hook = true;
	kunit_activate_static_stub(test, io_opaque_vec_grow, opaque_test_grow_window);
}

/* A hook that was not reached must not catch the setup of the next request. */
static void opaque_window_close(struct kunit *test)
{
	if (opaque_window.grow_hook)
		kunit_deactivate_static_stub(test, io_opaque_vec_grow);
	opaque_window.grow_hook = false;
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
	opaque_test_free(op->store, 0);
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
 * A block with too few segments for what the ranges span is grown, and the vector is
 * resolved again. Nothing is removed before that. A vector that consumes what it names
 * holds nothing meanwhile, and what the second attempt sees is what counts; any other
 * holds the versions that the first attempt pinned, and the second goes on with them.
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
	 * The attempt that finds the block too small removes nothing and leaves the block as
	 * it is, with the versions it pinned if the vector is not LAST. The retry walks the
	 * ranges again and ends as a single attempt would have.
	 */
	opaque_test_publish(store, 0, 9, a);
	opaque_grow_entries(ent, io_opaque_handle(0, 9));
	for (last = 0; last < 2; last++) {
		op = opaque_vec_new(test, store, ent, ARRAY_SIZE(ent),
				    last ? IORING_OPAQUE_SEND_LAST : 0);
		KUNIT_ASSERT_EQ(test, op->vec->cap_segs, 2U);
		before = atomic_long_read(&io_opaque_vec_stepped);
		ret = opaque_test_acquire(store, op);
		pass = atomic_long_read(&io_opaque_vec_stepped) - before;
		KUNIT_EXPECT_EQ(test, ret, 5);
		KUNIT_EXPECT_GT(test, pass, 0L);
		/* Both entries name A: one reference between them, held across the growing. */
		KUNIT_EXPECT_EQ(test, op->vec->pinned, last ? 0U : 2U);
		KUNIT_EXPECT_FALSE(test, op->consumed);
		KUNIT_EXPECT_EQ(test, op->vec->cap_segs, 2U);
		/* It stored what the block held and made no cursor of it. */
		KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 0U);
		KUNIT_EXPECT_EQ(test, iov_iter_count(&op->iter), (size_t)0);
		KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), last ? 1 : 2);
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), a);
		KUNIT_EXPECT_EQ(test, store->nr_objects, 1U);
		KUNIT_EXPECT_TRUE(test, opaque_test_used(store, 0));
		KUNIT_EXPECT_EQ(test, io_opaque_vec_finish(op, ret), 0);
		/* The second attempt walked as the first did: pass one was done again. */
		KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_vec_stepped) - before, 2 * pass);
		KUNIT_EXPECT_GE(test, op->vec->cap_segs, 5U);
		KUNIT_EXPECT_MEMEQ(test, op->vec->ent, ent, sizeof(ent));
		KUNIT_EXPECT_EQ(test, op->vec->pinned, 2U);
		KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 5U);
		KUNIT_EXPECT_EQ(test, op->consumed, (bool)last);
		/* LAST: the detached reference and the repeat's. Else: the table's and one pin. */
		KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), last ? NULL : a);
		opaque_vec_expect_cursor(test, op);
		io_opaque_vec_release(op);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);

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
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, op), -ESTALE);
	KUNIT_EXPECT_EQ(test, io_opaque_vec_finish(op, -ESTALE), -ESTALE);
	KUNIT_EXPECT_EQ(test, op->vec->cap_segs, 2U);
	/* The first entry is pinned, and is the request's to give back. */
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
	opaque_vec_failed(test, op, 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
	opaque_test_free(store, 0);

	/* Through admission, a block that fits is not grown, and the cursor is whole at the end. */
	a = opaque_sparse_data(test, store);
	opaque_test_publish(store, 0, 11, a);
	op = opaque_grow_queued(test, store, io_opaque_handle(0, 11), 0);
	opaque_vec_room(test, op, 5);
	opaque_window_open(test, NULL);
	KUNIT_ASSERT_EQ(test, io_opaque_tx_admit(op, sk), 1);
	opaque_window_close(test);
	lockdep_assert_none_held_once();
	KUNIT_EXPECT_EQ(test, opaque_window.calls, 0U);
	KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 2U);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 5U);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
	opaque_vec_expect_cursor(test, op);
	opaque_test_tx_done(op, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(op), NULL);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);

	/*
	 * A block that cannot grow ends the admission, which consumes nothing, LAST
	 * included, and leaves what a vector had pinned to the cleanup of the request;
	 * and it grows where no lock is held.
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
		KUNIT_EXPECT_EQ(test, opaque_window.segs, 5U);
		KUNIT_EXPECT_EQ(test, opaque_window.cap, 2U);
		KUNIT_EXPECT_EQ(test, opaque_window.pinned, last ? 0U : 2U);
		KUNIT_EXPECT_FALSE(test, opaque_window.consumed);
		KUNIT_EXPECT_FALSE(test, op->consumed);
		KUNIT_EXPECT_EQ(test, iov_iter_count(&op->iter), (size_t)0);
		KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
		KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), last ? 1 : 2);
		opaque_vec_failed(test, op, last ? 0 : 2);
		KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), a);
		KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	}

	/* The handle is gone when the second attempt looks: it fails and consumes nothing. */
	op = opaque_grow_queued(test, store, io_opaque_handle(0, 11), IORING_OPAQUE_SEND_LAST);
	opaque_window_open(test, opaque_window_free);
	KUNIT_EXPECT_EQ(test, io_opaque_tx_admit(op, sk), -ESTALE);
	opaque_window_close(test);
	KUNIT_EXPECT_EQ(test, opaque_window.calls, 1U);
	KUNIT_EXPECT_FALSE(test, opaque_window.grow_locked);
	KUNIT_EXPECT_FALSE(test, op->consumed);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 0U);
	KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);

	/*
	 * Nothing changes in the window, and the vector is LAST: the retry takes
	 * the handle with the references, in the hold of tables that makes the cursor.
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
	KUNIT_EXPECT_TRUE(test, op->consumed);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 5U);
	opaque_vec_expect_cursor(test, op);
	opaque_test_tx_done(op, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(op), NULL);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);

	/*
	 * The object is compacted in the window: the second attempt goes on with the
	 * version that the first pinned, and the cursor is over its five segments, not
	 * the replacement's. The replacement, which the table holds, is not referenced.
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
	replacement = opaque_slot_data(store, 0);
	KUNIT_ASSERT_PTR_NE(test, replacement, a);
	KUNIT_EXPECT_EQ(test, opaque_window.calls, 1U);
	KUNIT_EXPECT_FALSE(test, opaque_window.grow_locked);
	KUNIT_EXPECT_EQ(test, opaque_window.segs, 5U);
	KUNIT_EXPECT_EQ(test, opaque_window.cap, 2U);
	KUNIT_EXPECT_EQ(test, opaque_window.pinned, 2U);
	KUNIT_EXPECT_FALSE(test, opaque_window.consumed);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 2U);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[0], a);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[1], a);
	KUNIT_EXPECT_EQ(test, refcount_read(&replacement->refs), 1);
	/* The worker's reference and the send's; the table's went to the replacement. */
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
	opaque_grow_entries(ent, compact.handle);
	n = opaque_ref_segs(replacement, ent[0].off, ent[0].len, NULL) +
	    opaque_ref_segs(replacement, ent[1].off, ent[1].len, NULL);
	KUNIT_ASSERT_NE(test, n, 5U);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 5U);
	opaque_vec_expect_cursor(test, op);
	opaque_test_tx_done(op, true);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(op), NULL);
	io_opaque_vec_release(op);
	io_opaque_data_put(store, compact.data);
	io_opaque_compact_end(&compact);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			ALIGN(80000, PAGE_SIZE) + opaque_records(replacement));
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
}

/* A store with room for @nr objects, for a vector of many handles. */
static struct io_opaque_store *opaque_test_wide_store(struct kunit *test, u32 nr)
{
	struct io_opaque_store *store = opaque_test_big_store(test);

	opaque_test_slots(test, store, nr);
	return store;
}

/* The counters of admission, to be read before and after a vector goes through it. */
struct opaque_adm {
	u64 fast, slow;
	long seeks, counted;
};

static void opaque_adm_read(struct opaque_adm *a)
{
	a->fast = io_opaque_stat_read(IO_OPAQUE_STAT_SEND_ADMIT_FAST);
	a->slow = io_opaque_stat_read(IO_OPAQUE_STAT_SEND_ADMIT_SLOW);
	a->seeks = atomic_long_read(&io_opaque_vec_seeks);
	a->counted = atomic_long_read(&io_opaque_vec_counted);
}

/* What admission did since @before: fast and slow admissions, seek calls, segments counted. */
static void opaque_adm_expect(struct kunit *test, const struct opaque_adm *before, u64 fast,
			      u64 slow, long seeks, long counted)
{
	struct opaque_adm now;

	opaque_adm_read(&now);
	KUNIT_EXPECT_EQ(test, now.fast - before->fast, fast);
	KUNIT_EXPECT_EQ(test, now.slow - before->slow, slow);
	KUNIT_EXPECT_EQ(test, now.seeks - before->seeks, seeks);
	KUNIT_EXPECT_EQ(test, now.counted - before->counted, counted);
}

/* The bytes the cursor of an admitted vector reads are those of its ranges, one after the other. */
static void opaque_vec_expect_bytes(struct kunit *test, struct io_opaque_req *op,
				    u8 *const *source)
{
	u8 *want = kunit_kmalloc(test, op->length, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, op->length, GFP_KERNEL);
	struct iov_iter iter = op->iter;
	u32 i, done = 0;

	KUNIT_ASSERT_NOT_NULL(test, want);
	KUNIT_ASSERT_NOT_NULL(test, got);
	for (i = 0; i < op->vec->nr; i++) {
		u32 slot = op->vec->ent[i].handle & IO_OPAQUE_INDEX_MASK;

		memcpy(want + done, source[slot] + op->vec->ent[i].off, op->vec->ent[i].len);
		done += op->vec->ent[i].len;
	}
	KUNIT_EXPECT_EQ(test, done, op->length);
	KUNIT_EXPECT_EQ(test, copy_from_iter(got, done, &iter), (size_t)done);
	KUNIT_EXPECT_MEMEQ(test, got, want, done);
}

/*
 * Admission in one pass, ending with the cursor made: what it
 * stores, finds, pins and counts, for the vectors that the workload sends and
 * for those that it does not. Objects of one segment (slots 0 to 31), of three
 * (32 to 35) and of forty, which has anchors (36). The calls to find where a
 * range starts, the segments counted, the references and the fast and slow
 * admissions are those of the rules in io_opaque_vec_acquire().
 */
static void opaque_send_vec_one_pass(struct kunit *test)
{
	const u32 ones = 32, threes = 4, nobj = ones + threes + 1;
	struct io_opaque_store *store = opaque_test_wide_store(test, 40);
	struct io_uring_opaque_send_entry ent[32];
	struct io_opaque_data *obj[40];
	struct io_opaque_req *op, *single;
	u8 *source[40];
	struct opaque_adm before, after;
	u32 i, k;

	for (i = 0; i < nobj; i++) {
		obj[i] = opaque_varied_data(test, store, i < ones ? 1 : i < ones + threes ? 3 : 40);
		/* Every byte of an object differs from the bytes of the others. */
		opaque_test_fill(obj[i], i * 5);
		source[i] = kunit_kmalloc(test, obj[i]->length, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, source[i]);
		opaque_test_copy(obj[i], source[i]);
		opaque_test_publish(store, i, 3, obj[i]);
	}

	/* The workload: one handle, eight ranges, each the whole of an object of one segment. */
	for (i = 0; i < 8; i++)
		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = io_opaque_handle(0, 3), .off = 0, .len = obj[0]->length,
		};
	op = opaque_vec_new(test, store, ent, 8, 0);
	opaque_adm_read(&before);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	/* A fast admission, with no call to find a start and eight segments counted. */
	opaque_adm_expect(test, &before, 1, 0, 0, 8);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 8U);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 8U);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[0]->refs), 2);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&op->iter), (size_t)8 * obj[0]->length);
	opaque_vec_expect_cursor(test, op);
	opaque_vec_expect_bytes(test, op, source);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[0]->refs), 1);

	/* Many handles: a pin each, and no call, since the walk begins again in every object. */
	for (i = 0; i < ones; i++)
		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = io_opaque_handle(i, 3), .off = 0, .len = obj[i]->length,
		};
	op = opaque_vec_new(test, store, ent, ones, 0);
	opaque_adm_read(&before);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	opaque_adm_expect(test, &before, 1, 0, 0, ones);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, ones);
	for (i = 0; i < ones; i++) {
		KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[i], obj[i]);
		KUNIT_EXPECT_EQ(test, refcount_read(&obj[i]->refs), 2);
	}
	opaque_vec_expect_cursor(test, op);
	opaque_vec_expect_bytes(test, op, source);
	io_opaque_vec_release(op);
	for (i = 0; i < ones; i++)
		KUNIT_EXPECT_EQ(test, refcount_read(&obj[i]->refs), 1);

	/* Runs of one handle share a pin: A A B A A B B is four, two of A and two of B. */
	for (i = 0; i < 7; i++)
		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = io_opaque_handle(i == 2 || i >= 5, 3), .off = 10, .len = 50,
		};
	op = opaque_vec_new(test, store, ent, 7, 0);
	opaque_adm_read(&before);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	opaque_adm_expect(test, &before, 1, 0, 0, 7);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[0]->refs), 3);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[1]->refs), 3);
	opaque_vec_expect_cursor(test, op);
	opaque_vec_expect_bytes(test, op, source);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[0]->refs), 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[1]->refs), 1);

	/*
	 * Objects of three segments, the whole of each: twelve segments for four
	 * entries is more than the block holds. The first attempt counts them without
	 * storing the rest, the block grows and the second stores: one slow
	 * admission, and no start found with a call, in either attempt.
	 */
	for (i = 0; i < threes; i++)
		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = io_opaque_handle(ones + i, 3), .off = 0,
			.len = obj[ones + i]->length,
		};
	op = opaque_vec_new(test, store, ent, threes, 0);
	KUNIT_ASSERT_EQ(test, op->vec->cap_segs, threes);
	opaque_adm_read(&before);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	opaque_adm_expect(test, &before, 0, 1, 0, 2 * 3 * threes);
	KUNIT_EXPECT_GE(test, op->vec->cap_segs, 3 * threes);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 3 * threes);
	opaque_vec_expect_cursor(test, op);
	opaque_vec_expect_bytes(test, op, source);
	io_opaque_vec_release(op);
	/* The block is kept for the next vector of the request, which then goes at once. */
	opaque_adm_read(&before);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	opaque_adm_expect(test, &before, 1, 0, 0, 3 * threes);
	io_opaque_vec_release(op);

	/*
	 * Ranges of the object of forty segments, with anchors. A range that starts
	 * in the segment the walk is at is found without a call, and any other
	 * with one. The walk begins at the first segment, goes on from where the
	 * range before it started, and begins again for a range that starts before
	 * that segment. The cursor is that of walking every range from the start.
	 */
	{
		static const struct { u8 seg; u16 delta; u8 segs; } r[] = {
			{ 0, 0, 1 }, { 0, 5, 1 }, { 3, 0, 4 }, { 3, 7, 2 }, { 39, 0, 1 },
			{ 2, 0, 1 }, { 2, 1, 1 }, { 33, 4, 6 }, { 17, 0, 2 }, { 17, 3, 1 },
		};
		long calls = 0, counted = 0;
		u32 walk = 0;
		u64 walkpos = 0;

		k = ones + threes;
		for (i = 0; i < ARRAY_SIZE(r); i++) {
			u32 last_seg = min_t(u32, r[i].seg + r[i].segs, 40);
			u64 start = opaque_seg_pos(obj[k], r[i].seg) + r[i].delta;

			ent[i] = (struct io_uring_opaque_send_entry) {
				.handle = io_opaque_handle(k, 3), .off = start,
				.len = opaque_seg_pos(obj[k], last_seg) - start,
			};
			counted += last_seg - r[i].seg;
			if (start < walkpos) {
				walk = 0;
				walkpos = 0;
			}
			calls += walk != r[i].seg;
			walk = r[i].seg;
			walkpos = opaque_seg_pos(obj[k], walk);
		}
		op = opaque_vec_new(test, store, ent, ARRAY_SIZE(r), 0);
		opaque_vec_room(test, op, 40);
		opaque_adm_read(&before);
		KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
		opaque_adm_expect(test, &before, 1, 0, calls, counted);
		KUNIT_EXPECT_GT(test, calls, 0L);
		KUNIT_EXPECT_LT(test, calls, (long)ARRAY_SIZE(r));
		opaque_vec_expect_cursor(test, op);
		opaque_vec_expect_bytes(test, op, source);
		io_opaque_vec_release(op);
	}

	/* A LAST vector: a handle goes at its first entry, and a repeat takes a reference. */
	{
		static const u8 slots[5] = { 0, 1, 0, 1, 2 };

		for (i = 0; i < ARRAY_SIZE(slots); i++)
			ent[i] = (struct io_uring_opaque_send_entry) {
				.handle = io_opaque_handle(slots[i], 3), .off = 0, .len = 100,
			};
	}
	op = opaque_vec_new(test, store, ent, 5, IORING_OPAQUE_SEND_LAST);
	opaque_adm_read(&before);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	opaque_adm_expect(test, &before, 1, 0, 0, 5);
	KUNIT_EXPECT_TRUE(test, op->consumed);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, i), NULL);
	KUNIT_EXPECT_EQ(test, store->nr_objects, nobj - 3);
	/* The table's reference went to the send, a repeat took one: two for 0 and 1, one for 2. */
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[0]->refs), 2);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[1]->refs), 2);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[2]->refs), 1);
	opaque_vec_expect_cursor(test, op);
	opaque_vec_expect_bytes(test, op, source);
	io_opaque_vec_release(op);

	/* A send of one range, and a vector that fails, are neither fast nor slow. */
	single = kunit_kzalloc(test, sizeof(*single), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, single);
	single->store = store;
	single->handle = io_opaque_handle(5, 3);
	single->offset = 10;
	single->length = 50;
	opaque_adm_read(&before);
	KUNIT_ASSERT_EQ(test, opaque_test_acquire(store, single), 0);
	opaque_adm_read(&after);
	KUNIT_EXPECT_EQ(test, after.fast, before.fast);
	KUNIT_EXPECT_EQ(test, after.slow, before.slow);
	io_opaque_data_put(store, single->data);
	ent[0] = (struct io_uring_opaque_send_entry) {
		.handle = io_opaque_handle(5, 3), .off = 0, .len = 100,
	};
	ent[1] = (struct io_uring_opaque_send_entry) {
		.handle = io_opaque_handle(6, 2), .off = 0, .len = 100,
	};
	op = opaque_vec_new(test, store, ent, 2, 0);
	opaque_adm_read(&before);
	KUNIT_EXPECT_EQ(test, opaque_vec_admit(op), -ESTALE);
	/* The first entry was counted and is pinned, the second was not found. */
	opaque_adm_expect(test, &before, 0, 0, 0, 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[5]->refs), 2);
	opaque_vec_failed(test, op, 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&obj[5]->refs), 1);

	for (i = 0; i < nobj; i++) {
		if (i < 3 && !opaque_slot_data(store, i))
			continue;
		opaque_test_free(store, i);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/*
 * One failure of a vector of eight entries, which span two segments each: entry
 * @k is made wrong in way @kind, the vector is admitted, and it says what is wrong
 * and not that its block, which holds the segments if @room, is short, and nothing
 * has been detached, consumed, counted as an admission or made into a cursor. A
 * vector that pins keeps what it pinned, for the cleanup of the request: the entries
 * before @k, and @k too if it is the limit on segments that fails and not the
 * entry. Mended, it goes through, from a block that was cleaned up or, with @resume,
 * from the one that failed, which goes on with the versions it had pinned.
 */
static void opaque_fail_one(struct kunit *test, struct io_opaque_store *store,
			    struct io_opaque_data **a, struct io_opaque_data **b,
			    const struct io_uring_opaque_send_entry *ent, bool last, bool room,
			    unsigned int kind, unsigned int k, bool resume)
{
	static const int errs[] = { -ERANGE, -ESTALE, -ESTALE, -EMSGSIZE };
	const unsigned long limit = io_opaque_send_vec_segs;
	struct io_opaque_req *op = opaque_vec_new(test, store, ent, 8,
						  last ? IORING_OPAQUE_SEND_LAST : 0);
	/* The entries alternate between A and B, so each has a reference of its own. */
	const u32 held = last ? 0 : k + (kind == 3);
	struct io_uring_opaque_send_entry mended;
	struct opaque_adm before, after;

	if (room)
		opaque_vec_room(test, op, 16);
	mended = op->vec->ent[k];
	if (kind == 0)		/* it ends past the object */
		op->vec->ent[k].off = 79500;
	else if (kind == 1)	/* an earlier generation of the slot */
		op->vec->ent[k].handle = io_opaque_handle(1, 6);
	else if (kind == 2)	/* a slot with nothing in it */
		op->vec->ent[k].handle = io_opaque_handle(2, 0);
	else			/* entry k is one range too many */
		io_opaque_send_vec_segs = 2 * k + 1;
	opaque_adm_read(&before);
	KUNIT_ASSERT_EQ(test, opaque_test_acquire(store, op), errs[kind]);
	opaque_adm_read(&after);
	KUNIT_EXPECT_EQ(test, after.fast, before.fast);
	KUNIT_EXPECT_EQ(test, after.slow, before.slow);
	KUNIT_EXPECT_FALSE(test, op->consumed);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, held);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 0U);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&op->iter), (size_t)0);
	KUNIT_EXPECT_EQ(test, refcount_read(&(*a)->refs), 1 + (held + 1) / 2);
	KUNIT_EXPECT_EQ(test, refcount_read(&(*b)->refs), 1 + held / 2);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), *a);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 1), *b);
	KUNIT_EXPECT_EQ(test, store->nr_objects, 2U);
	KUNIT_EXPECT_TRUE(test, opaque_test_used(store, 0));
	KUNIT_EXPECT_TRUE(test, opaque_test_used(store, 1));
	if (!resume) {
		opaque_vec_failed(test, op, held);
		KUNIT_EXPECT_EQ(test, refcount_read(&(*a)->refs), 1);
		KUNIT_EXPECT_EQ(test, refcount_read(&(*b)->refs), 1);
	}
	/* Mended, it goes, however long it took to find out. */
	op->vec->ent[k] = mended;
	io_opaque_send_vec_segs = limit;
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	KUNIT_EXPECT_EQ(test, op->vec->pinned, 8U);
	KUNIT_EXPECT_EQ(test, op->vec->nr_segs, 16U);
	KUNIT_EXPECT_EQ(test, op->consumed, last);
	/* Four references on each, the table's or the repeats'; a resume holds the same. */
	KUNIT_EXPECT_EQ(test, refcount_read(&(*a)->refs), last ? 4 : 5);
	KUNIT_EXPECT_EQ(test, refcount_read(&(*b)->refs), last ? 4 : 5);
	opaque_vec_expect_cursor(test, op);
	io_opaque_vec_release(op);
	if (last) {
		/* The handles went with it, and so did the objects: new ones for the next. */
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 1), NULL);
		KUNIT_EXPECT_EQ(test, store->nr_objects, 0U);
		*a = opaque_sparse_data(test, store);
		*b = opaque_sparse_data(test, store);
		opaque_test_publish(store, 0, 9, *a);
		opaque_test_publish(store, 1, 7, *b);
	}
	KUNIT_EXPECT_EQ(test, refcount_read(&(*a)->refs), 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&(*b)->refs), 1);
}

/*
 * A vector that cannot go consumes nothing, wherever it fails: in any of its
 * eight entries, for any of the ways an entry fails (out of range, a handle of
 * another generation, a slot that holds nothing, one range too many for the
 * limit on segments), LAST or not, with a block that holds the segments of the
 * ranges and with one that is too short for them (opaque_fail_one()), and goes on
 * to be admitted when it is mended, from a clean block or from the one that failed.
 */
static void opaque_send_vec_acquire_fails(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *a = opaque_sparse_data(test, store);
	struct io_opaque_data *b = opaque_sparse_data(test, store);
	struct io_uring_opaque_send_entry ent[8];
	unsigned long limit = io_opaque_send_vec_segs;
	unsigned int last, room, kind, k, resume;

	opaque_test_publish(store, 0, 9, a);
	opaque_test_publish(store, 1, 7, b);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_vec_segs_restore,
							(void *)limit), 0);
	/* Extents are 2000 bytes: each of these spans two. */
	for (k = 0; k < ARRAY_SIZE(ent); k++)
		ent[k] = (struct io_uring_opaque_send_entry) {
			.handle = io_opaque_handle(k % 2, k % 2 ? 7 : 9),
			.off = 1500 + 4000 * k, .len = 1000,
		};
	for (last = 0; last < 2; last++)
		for (room = 0; room < 2; room++)
			for (kind = 0; kind < 4; kind++)
				for (k = 0; k < ARRAY_SIZE(ent); k++)
					for (resume = 0; resume < 2; resume++)
						opaque_fail_one(test, store, &a, &b, ent, last,
								room, kind, k, resume);
	opaque_test_free(store, 0);
	opaque_test_free(store, 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/*
 * The cursor is made as the versions are pinned, from the segments of
 * the versions it pins, so it does not matter that the slot holds another by
 * the time the bytes are read, nor that the pins are the last references of the
 * version: its segments are still there.
 */
static void opaque_send_vec_cursor_pinned(struct kunit *test)
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
	spin_lock(&store->tables);
	ret = io_opaque_send_acquire(op);
	spin_unlock(&store->tables);
	KUNIT_ASSERT_EQ(test, ret, 0);
	/* The table's reference and one pin for the three entries, and the cursor is there. */
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
	opaque_vec_expect_cursor(test, op);
	/* The object is compacted, and the compaction worker is done with the old version. */
	refcount_inc(&a->refs);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	replacement = compact.replacement;
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	io_opaque_data_put(store, compact.data);
	io_opaque_compact_end(&compact);
	KUNIT_ASSERT_PTR_EQ(test, opaque_slot_data(store, 0), replacement);
	KUNIT_EXPECT_EQ(test, refcount_read(&old->refs), 1);
	/* Only the send holds the old version now, and its cursor reads it. */
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[0], old);
	opaque_vec_expect_cursor(test, op);
	KUNIT_EXPECT_EQ(test, copy_from_iter(got, done, &op->iter), (size_t)done);
	KUNIT_EXPECT_MEMEQ(test, got, want, done);
	io_opaque_vec_release(op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			ALIGN(80000, PAGE_SIZE) + opaque_records(replacement));
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, op), 0);
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
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
}

/*
 * What prep says of a vector it cannot take, and what it leaves behind. The
 * number of entries is looked at before any is read, so a number that is wrong
 * is -EINVAL even where the entries are not there to be read, and so is an
 * offset; entries that are not there are -EFAULT; entries that are wrong are
 * -EINVAL. A request that fails holds nothing, and the one after it gets its
 * block back.
 */
static void opaque_send_vec_prep_errors(struct kunit *test)
{
	static const struct {
		u64 off;
		u32 len;
		bool nowhere;
		int result;
	} errs[] = {
		{ 0, 0, true, -EINVAL },
		{ 0, 0, false, -EINVAL },
		{ 0, IORING_OPAQUE_SEND_VEC_MAX + 1, true, -EINVAL },
		{ 0, IORING_OPAQUE_SEND_VEC_MAX + 1, false, -EINVAL },
		{ 0, U32_MAX, true, -EINVAL },
		{ 1, 3, true, -EINVAL },
		{ 1, 3, false, -EINVAL },
		{ 0, 3, true, -EFAULT },
		{ 0, IORING_OPAQUE_SEND_VEC_MAX, true, -EFAULT },
		{ 0, 3, false, 0 },
		{ 0, IORING_OPAQUE_SEND_VEC_MAX, false, 0 },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_test_object(test, store);
	struct opaque_prep *p = kunit_kzalloc(test, sizeof(*p), GFP_KERNEL);
	const u64 h = io_opaque_handle(0, 9);
	struct io_uring_opaque_send_entry *ent;
	struct io_opaque_req *op;
	unsigned long addr, where, hole;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, p);
	ent = kunit_kcalloc(test, IORING_OPAQUE_SEND_VEC_MAX, sizeof(*ent), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ent);
	for (i = 0; i < IORING_OPAQUE_SEND_VEC_MAX; i++)
		ent[i] = (struct io_uring_opaque_send_entry) { .handle = h, .off = i, .len = 1 };
	opaque_prep_init(test, p, store);
	addr = opaque_uvec_map(test);
	opaque_uvec_put(test, addr, ent, IORING_OPAQUE_SEND_VEC_MAX * sizeof(*ent));
	for (i = 0; i < ARRAY_SIZE(errs); i++) {
		where = errs[i].nowhere ? 0 : addr;
		opaque_prep_sqe(p, IORING_OPAQUE_SEND_VECTOR, where, errs[i].off, errs[i].len);
		KUNIT_EXPECT_EQ(test, opaque_prep_send(p, &op), errs[i].result);
		if (!errs[i].result) {
			KUNIT_EXPECT_EQ(test, op->length, errs[i].len);
			KUNIT_EXPECT_EQ(test, op->vec->nr, errs[i].len);
			KUNIT_EXPECT_MEMEQ(test, op->vec->ent, ent, errs[i].len * sizeof(*ent));
			KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
		}
		opaque_prep_done(p);
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	}
	/* Entries that are not there: the hole cuts the vector after two entries of three. */
	opaque_uvec_hole(test, addr);
	hole = addr + OPAQUE_UVEC_HOLE * PAGE_SIZE;
	opaque_uvec_put(test, hole - 2 * sizeof(*ent), ent, 2 * sizeof(*ent));
	opaque_prep_sqe(p, IORING_OPAQUE_SEND_VECTOR, hole - 2 * sizeof(*ent), 0, 3);
	KUNIT_EXPECT_EQ(test, opaque_prep_send(p, &op), -EFAULT);
	opaque_prep_done(p);
	/* Entries that are wrong, the last of them: the whole vector was read. */
	ent[2].len = 0;
	opaque_uvec_put(test, addr, ent, 3 * sizeof(*ent));
	opaque_prep_sqe(p, IORING_OPAQUE_SEND_VECTOR, addr, 0, 3);
	KUNIT_EXPECT_EQ(test, opaque_prep_send(p, &op), -EINVAL);
	opaque_prep_done(p);
	/* The request after the failures is a request like any other. */
	ent[2].len = 1;
	opaque_uvec_put(test, addr, ent, 3 * sizeof(*ent));
	opaque_prep_sqe(p, IORING_OPAQUE_SEND_VECTOR, addr, 0, 3);
	KUNIT_ASSERT_EQ(test, opaque_prep_send(p, &op), 0);
	KUNIT_EXPECT_EQ(test, op->length, 3U);
	KUNIT_EXPECT_EQ(test, opaque_vec_admit(op), 0);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	opaque_prep_done(p);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
}

/*
 * Count the calls that cleanup and prep make to the puts and to the growth of a
 * block, and pass them on: what they were called with, and whether that was
 * anything.
 */
static struct {
	unsigned int data, data_null, stream, stream_null, grow;
} opaque_calls;

static void opaque_test_data_put_count(struct io_opaque_store *store, struct io_opaque_data *data)
{
	struct kunit *test = kunit_get_current_test();

	if (data)
		opaque_calls.data++;
	else
		opaque_calls.data_null++;
	kunit_deactivate_static_stub(test, io_opaque_data_put);
	io_opaque_data_put(store, data);
	kunit_activate_static_stub(test, io_opaque_data_put, opaque_test_data_put_count);
}

static void opaque_test_stream_put_count(struct io_opaque_stream *stream)
{
	struct kunit *test = kunit_get_current_test();

	if (stream)
		opaque_calls.stream++;
	else
		opaque_calls.stream_null++;
	kunit_deactivate_static_stub(test, io_opaque_stream_put);
	io_opaque_stream_put(stream);
	kunit_activate_static_stub(test, io_opaque_stream_put, opaque_test_stream_put_count);
}

static int opaque_test_grow_count(struct io_opaque_req *op, u32 nr, u32 segs, u32 keep)
{
	struct kunit *test = kunit_get_current_test();
	int ret;

	opaque_calls.grow++;
	kunit_deactivate_static_stub(test, io_opaque_vec_grow);
	ret = io_opaque_vec_grow(op, nr, segs, keep);
	kunit_activate_static_stub(test, io_opaque_vec_grow, opaque_test_grow_count);
	return ret;
}

/*
 * Cleanup puts what the request holds and calls nothing for what it does not: a
 * send that was prepared and never issued holds no version and no stream, one
 * that is admitted holds its version once, a vector holds a pin for each run of
 * entries that name a handle and leaves the request's own version alone. The
 * puts are counted by what they were called with, and a put of nothing is a
 * call for nothing.
 */
static void opaque_cleanup_calls(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_test_object(test, store);
	struct opaque_prep *p = kunit_kzalloc(test, sizeof(*p), GFP_KERNEL);
	const u64 h = io_opaque_handle(0, 9);
	struct io_uring_opaque_send_entry ent[3] = {
		{ .handle = h, .off = 0, .len = 1000 },
		{ .handle = h, .off = 5, .len = 10 },
		{ .handle = h, .off = 79000, .len = 1000 },
	};
	struct io_opaque_req *op;
	unsigned long addr;

	KUNIT_ASSERT_NOT_NULL(test, p);
	opaque_prep_init(test, p, store);
	addr = opaque_uvec_map(test);
	opaque_uvec_put(test, addr, ent, sizeof(ent));
	kunit_activate_static_stub(test, io_opaque_data_put, opaque_test_data_put_count);
	kunit_activate_static_stub(test, io_opaque_stream_put, opaque_test_stream_put_count);

	/* Prepared and never issued. */
	opaque_prep_sqe(p, 0, h, 5, 100);
	KUNIT_ASSERT_EQ(test, opaque_prep_send(p, &op), 0);
	memset(&opaque_calls, 0, sizeof(opaque_calls));
	opaque_prep_done(p);
	KUNIT_EXPECT_EQ(test, opaque_calls.data, 0U);
	KUNIT_EXPECT_EQ(test, opaque_calls.data_null, 0U);
	KUNIT_EXPECT_EQ(test, opaque_calls.stream, 0U);
	KUNIT_EXPECT_EQ(test, opaque_calls.stream_null, 0U);

	/* Admitted: its version is put, once. */
	opaque_prep_sqe(p, 0, h, 5, 100);
	KUNIT_ASSERT_EQ(test, opaque_prep_send(p, &op), 0);
	KUNIT_ASSERT_EQ(test, opaque_test_acquire(store, op), 0);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	memset(&opaque_calls, 0, sizeof(opaque_calls));
	opaque_prep_done(p);
	KUNIT_EXPECT_EQ(test, opaque_calls.data, 1U);
	KUNIT_EXPECT_EQ(test, opaque_calls.data_null, 0U);
	KUNIT_EXPECT_EQ(test, opaque_calls.stream, 0U);
	KUNIT_EXPECT_EQ(test, opaque_calls.stream_null, 0U);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);

	/* A vector of three entries of one handle: one pin, put once, and nothing else. */
	opaque_prep_sqe(p, IORING_OPAQUE_SEND_VECTOR, addr, 0, ARRAY_SIZE(ent));
	KUNIT_ASSERT_EQ(test, opaque_prep_send(p, &op), 0);
	KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
	memset(&opaque_calls, 0, sizeof(opaque_calls));
	opaque_prep_done(p);
	KUNIT_EXPECT_EQ(test, opaque_calls.data, 1U);
	KUNIT_EXPECT_EQ(test, opaque_calls.data_null, 0U);
	KUNIT_EXPECT_EQ(test, opaque_calls.stream, 0U);
	KUNIT_EXPECT_EQ(test, opaque_calls.stream_null, 0U);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	kunit_deactivate_static_stub(test, io_opaque_data_put);
	kunit_deactivate_static_stub(test, io_opaque_stream_put);
}

/*
 * Prep asks for a block only when the request has none that holds its entries.
 * A request is given back the block of the last one, unless it was big, and
 * that block is not grown for entries it holds: a block for three is kept for
 * three and for two, and is grown once for five and then kept for five. Under
 * KASAN a cached request is cleared when it is taken, so none has a block and
 * each asks.
 */
static void opaque_send_vec_prep_grows(struct kunit *test)
{
	static const u32 sizes[] = { 3, 3, 2, 5, 5, 4, 3, 5 };
	/* Asks of a request with a block kept: the first, and the one that outgrows it. */
	static const bool grows[] = { true, false, false, true, false, false, false, false };
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_test_object(test, store);
	struct opaque_prep *p = kunit_kzalloc(test, sizeof(*p), GFP_KERNEL);
	const u64 h = io_opaque_handle(0, 9);
	struct io_uring_opaque_send_entry *ent;
	struct io_opaque_req *op;
	unsigned long addr;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, p);
	ent = kunit_kcalloc(test, 8, sizeof(*ent), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ent);
	for (i = 0; i < 8; i++)
		ent[i] = (struct io_uring_opaque_send_entry) { .handle = h, .off = i, .len = 1 };
	opaque_prep_init(test, p, store);
	addr = opaque_uvec_map(test);
	opaque_uvec_put(test, addr, ent, 8 * sizeof(*ent));
	memset(&opaque_calls, 0, sizeof(opaque_calls));
	kunit_activate_static_stub(test, io_opaque_vec_grow, opaque_test_grow_count);
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		unsigned int before = opaque_calls.grow;

		opaque_prep_sqe(p, IORING_OPAQUE_SEND_VECTOR, addr, 0, sizes[i]);
		KUNIT_ASSERT_EQ(test, opaque_prep_send(p, &op), 0);
		KUNIT_EXPECT_EQ_MSG(test, opaque_calls.grow - before,
				    (IS_ENABLED(CONFIG_KASAN) || grows[i]) ? 1U : 0U,
				    "prep %u of %u entries", i, sizes[i]);
		KUNIT_EXPECT_GE(test, op->vec->cap_nr, sizes[i]);
		KUNIT_EXPECT_GE(test, op->vec->cap_segs, sizes[i]);
		KUNIT_EXPECT_EQ(test, op->length, sizes[i]);
		KUNIT_EXPECT_MEMEQ(test, op->vec->ent, ent, sizes[i] * sizeof(*ent));
		opaque_prep_done(p);
	}
	kunit_deactivate_static_stub(test, io_opaque_vec_grow);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
}

/* The member checks of opaque_send_prep_fields(), which say which byte and which shape. */
#define OPAQUE_FIELD_EQ(got, want) \
	KUNIT_EXPECT_EQ_MSG(test, got, want, "byte %02x, shape %u", byte, shape)
#define OPAQUE_FIELD_PTR_EQ(got, want) \
	KUNIT_EXPECT_PTR_EQ_MSG(test, got, want, "byte %02x, shape %u", byte, shape)
#define OPAQUE_FIELD_TRUE(got) \
	KUNIT_EXPECT_TRUE_MSG(test, got, "byte %02x, shape %u", byte, shape)
#define OPAQUE_FIELD_FALSE(got) \
	KUNIT_EXPECT_FALSE_MSG(test, got, "byte %02x, shape %u", byte, shape)

/*
 * The request of a send as prep leaves it, whatever the request held before: every member that
 * the send reads before it writes it has its value, and the members that the SQE gives have what
 * it gave. The request is filled with a byte first (io_opaque_send_poison, with three of them,
 * since a member that should be zero or all ones is told from the byte only by one that is
 * neither), so that one that prep leaves alone is what the byte made it. Five shapes: a range,
 * LAST, a send with a frame to import, a reply of a frame alone, and a vector, whose block is
 * the one member that the byte does not touch.
 */
static void opaque_send_prep_fields(struct kunit *test)
{
	static const u8 bytes[] = { 0x00, 0xa5, 0x5a };
	static const struct {
		u16 ioprio;
		u64 off, addr3;
		u32 msg_flags, len;
		bool resolved;
	} shapes[] = {
		{ 0, 5, 0, 0, 100, true },
		{ IORING_OPAQUE_SEND_LAST, 0, 0, MSG_MORE, 1, true },
		{ 0, 9, 0x1000, MSG_DONTWAIT, 100, false },
		{ IORING_OPAQUE_SEND_LAST, 0, 0x1000, 0, 0, false },
		{ IORING_OPAQUE_SEND_VECTOR, 0, 0, 0, 3, true },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_prep *p = kunit_kzalloc(test, sizeof(*p), GFP_KERNEL);
	const u64 h = io_opaque_handle(0, 9);
	struct io_uring_opaque_send_entry ent[3] = {
		{ .handle = h, .off = 0, .len = 10 }, { .handle = h, .off = 10, .len = 20 },
		{ .handle = h, .off = 30, .len = 30 },
	};
	unsigned long addr;
	unsigned int shape, j;

	KUNIT_ASSERT_NOT_NULL(test, p);
	opaque_test_params_save(test);
	opaque_prep_init(test, p, store);
	addr = opaque_uvec_map(test);
	opaque_uvec_put(test, addr, ent, sizeof(ent));
	for (j = 0; j < ARRAY_SIZE(bytes); j++) {
		u8 byte = bytes[j];

		io_opaque_send_poison = byte;
		for (shape = 0; shape < ARRAY_SIZE(shapes); shape++) {
			bool vector = shapes[shape].ioprio & IORING_OPAQUE_SEND_VECTOR;
			bool resolved = shapes[shape].resolved;
			u32 len = vector ? 60 : shapes[shape].len;
			struct io_opaque_req *op;

			opaque_prep_sqe(p, shapes[shape].ioprio, vector ? addr : h,
					shapes[shape].off, shapes[shape].len);
			p->sqe.addr3 = shapes[shape].addr3;
			p->sqe.msg_flags = shapes[shape].msg_flags;
			KUNIT_ASSERT_EQ(test, opaque_prep_send(p, &op), 0);
			KUNIT_ASSERT_NOT_NULL(test, op);
			OPAQUE_FIELD_TRUE(p->req.flags & REQ_F_NEED_CLEANUP);
			OPAQUE_FIELD_PTR_EQ(op->req, &p->req);
			OPAQUE_FIELD_PTR_EQ(op->store, store);
			/* Cancellation looks at the stream, lists and phase of what waits. */
			OPAQUE_FIELD_PTR_EQ(op->stream, NULL);
			OPAQUE_FIELD_PTR_EQ(op->data, NULL);
			OPAQUE_FIELD_TRUE(list_empty(&op->wait));
			OPAQUE_FIELD_TRUE(list_empty(&op->cancel));
			OPAQUE_FIELD_EQ(op->phase, IO_OPAQUE_IDLE);
			OPAQUE_FIELD_FALSE(op->canceled);
			OPAQUE_FIELD_EQ(op->op, (u16)U16_MAX);
			OPAQUE_FIELD_FALSE(op->consumed);
			OPAQUE_FIELD_EQ(op->progress, 0ULL);
			/* The place in a queue, the frame and its charge. */
			OPAQUE_FIELD_PTR_EQ(op->tx, NULL);
			OPAQUE_FIELD_FALSE(op->tx_started);
			OPAQUE_FIELD_PTR_EQ(op->framing, NULL);
			OPAQUE_FIELD_EQ(op->frame_charge, 0U);
			OPAQUE_FIELD_EQ(op->prefix_length, 0U);
			OPAQUE_FIELD_EQ(op->suffix_length, 0U);
			/* Resolved at once, or left to the first issue, which imports the frame. */
			OPAQUE_FIELD_EQ(op->resolved, resolved);
			OPAQUE_FIELD_EQ(op->total_length, resolved ? len : 0U);
			/* And what the SQE says. */
			OPAQUE_FIELD_EQ(op->send_flags, shapes[shape].ioprio);
			OPAQUE_FIELD_EQ(op->msg_flags, shapes[shape].msg_flags);
			OPAQUE_FIELD_EQ(op->length, len);
			OPAQUE_FIELD_PTR_EQ(op->addr, u64_to_user_ptr(shapes[shape].addr3));
			OPAQUE_FIELD_EQ(op->handle, vector ? 0 : h);
			OPAQUE_FIELD_EQ(op->offset, vector ? 0 : shapes[shape].off);
			OPAQUE_FIELD_EQ(!!(p->req.flags & REQ_F_NOWAIT),
					!!(shapes[shape].msg_flags & MSG_DONTWAIT));
			if (vector) {
				KUNIT_ASSERT_NOT_NULL(test, op->vec);
				OPAQUE_FIELD_EQ(op->vec->nr, 3U);
				OPAQUE_FIELD_EQ(op->vec->pinned, 0U);
			}
			opaque_prep_done(p);
		}
	}
}

#undef OPAQUE_FIELD_EQ
#undef OPAQUE_FIELD_PTR_EQ
#undef OPAQUE_FIELD_TRUE
#undef OPAQUE_FIELD_FALSE

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
		opaque_test_publish(store, i, 9, data[i]);
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
	KUNIT_EXPECT_PTR_NE(test, opaque_slot_data(store, 1), data[1]);
	KUNIT_EXPECT_TRUE(test, opaque_slot_data(store, 1)->dense);
	KUNIT_EXPECT_FALSE(test, opaque_test_slot(store, 1)->compacting);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->compactions), 1LL);
	/* A failed inline copy keeps the original and releases the claim. */
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 3), data[3]);
	KUNIT_EXPECT_FALSE(test, opaque_test_slot(store, 3)->compacting);
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
		KUNIT_EXPECT_TRUE(test, opaque_test_slot(store, i)->compacting);
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
			3 * opaque_sparse_held(data[0]) + ALIGN(80000, PAGE_SIZE) +
			opaque_records(opaque_slot_data(store, 1)));
	/* Unwind the queued requests as request cleanup would. */
	cancel_delayed_work_sync(&store->compact_work);
	for (i = 0; i < 4; i += 2) {
		scoped_guard(spinlock, &store->wait_lock)
			list_del_init(&ops[i].wait);
		list_del_init(&ops[i].cancel);
		opaque_test_slot(store, i)->compacting = false;
		io_opaque_data_put(store, ops[i].data);
	}
	for (i = 0; i < 4; i++) {
		struct io_opaque_data *old;

		spin_lock(&store->tables);
		old = io_opaque_detach(store, opaque_test_slot(store, i));
		spin_unlock(&store->tables);
		io_opaque_data_put(store, old);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	opaque_policy_hook.tables_free &= !opaque_test_held(&store->tables);
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
		spin_lock(&store->tables);
		store->dead = 1;
		spin_unlock(&store->tables);
		break;
	case POLICY_JOIN:
		/* The publish path adds an object that the walk has not reached yet. */
		slot = opaque_test_slot(store, OPAQUE_POLICY_SLOTS - 1);
		spin_lock(&store->tables);
		if (list_empty(&slot->candidate))
			list_add_tail(&slot->candidate, &store->candidates);
		spin_unlock(&store->tables);
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
		mask |= 1U << opaque_test_slot_index(store, slot);
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

	store->config.hard_limit = SZ_1M;
	opaque_test_slots(test, store, OPAQUE_POLICY_SLOTS);
	INIT_LIST_HEAD(&store->copies);
	INIT_DELAYED_WORK(&store->compact_work, opaque_test_noop_work);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_cancel_work,
							&store->compact_work), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_policy_restore, NULL), 0);
	for (i = 0; i < OPAQUE_POLICY_SLOTS; i++) {
		if (i == 6 || i == 7)
			continue;	/* nothing published */
		data[i] = io_opaque_data_alloc();
		KUNIT_ASSERT_NOT_NULL(test, data[i]);
		data[i]->dense = i == 1 || i == 10;
		data[i]->forward_only = i == 4;
		opaque_slot_set(store, i, data[i]);
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
		list_del_init(&opaque_test_slot(store, i)->candidate);
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
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 40);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_budget_wakes) - wakes, 1L);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);

	left = io_opaque_extent_new(store, page, 0, 128, 0, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(left));
	list_add(&left->list, &both);
	right = io_opaque_extent_split(store, left, 64);
	KUNIT_ASSERT_FALSE(test, IS_ERR(right));
	list_move(&left->list, &first);
	list_move(&right->list, &second);
	wakes = atomic_long_read(&io_opaque_budget_wakes);
	io_opaque_extents_free(store, &first);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_budget_wakes) - wakes, 1L);
	/* The extent in the chunk gave back nothing: the other holds backing, page and chunk. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)PAGE_SIZE + OPAQUE_TEST_ALONE + OPAQUE_TEST_EXTENT);
	io_opaque_extents_free(store, &second);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_budget_wakes) - wakes, 2L);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	io_opaque_extents_free(store, &first);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_budget_wakes) - wakes, 2L);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/*
 * What the send finds when it asks whether the socket is plain, which it does
 * first, on either path, before it asks the security hook or takes the socket:
 * a protocol that takes the socket over at the n-th time it is asked, as a ULP
 * or a socket map would. The real answer is given. And a frame that joins the
 * queue behind the head after the head has joined and before it takes its
 * references, which is where a frame that arrives meanwhile finds it: the head
 * joins once it owns the socket, so no earlier moment will do.
 */
static struct {
	unsigned int calls, swap_at;
	struct sock *sk;
	struct io_opaque_req *follower;
	int joined;
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

static int opaque_test_settle_after_join(struct io_opaque_req *op)
{
	struct kunit *test = kunit_get_current_test();
	int ret;

	if (opaque_direct.follower)
		opaque_direct.joined = io_opaque_tx_join(opaque_direct.follower, opaque_direct.sk);
	kunit_deactivate_static_stub(test, io_opaque_tx_settle_head);
	ret = io_opaque_tx_settle_head(op);
	kunit_activate_static_stub(test, io_opaque_tx_settle_head, opaque_test_settle_after_join);
	return ret;
}

/* The security hook on a socket that is sent on directly: it refuses, or says EAGAIN, once. */
static int opaque_test_security(struct socket *sock, struct msghdr *msg,
				const struct io_opaque_req *op)
{
	int ret = opaque_test_security_probe(sock, msg, op);

	return ret ?: opaque_refuse_now();
}

/*
 * What a refusal from the security hook, or its EAGAIN, does to an issue is the
 * same on a socket that is sent on directly and on one that goes through
 * sock_sendmsg(). The direct path asks the hook before it takes the socket,
 * because a hook may take the socket lock itself, and keeps what it says until
 * the references are taken and the frame has started, which is where the first
 * sock_sendmsg() of the other path meets it. A refusal completes the send
 * with its error and LAST consumed, and fails the frame parked behind it;
 * EAGAIN returns with the place at the head, the references and the claim on
 * LAST kept, and the next issue sends the frame. Only what happens to the
 * references afterwards differs: the direct path gives them back itself, the
 * other leaves them to cleanup, which here is the test. A frame joins behind
 * the head before it settles, so that it holds its own pin when the head
 * claims LAST.
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
	opaque_hook_calls = 0;
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
	memset(&opaque_direct, 0, sizeof(opaque_direct));
	opaque_direct.sk = sk;
	opaque_direct.follower = follower;
	kunit_activate_static_stub(test, io_opaque_send_direct, opaque_test_direct);
	kunit_activate_static_stub(test, io_opaque_tx_settle_head, opaque_test_settle_after_join);

	mutex_lock(&w->ctx->uring_lock);
	ret = io_opaque_send(&w->req[0], IO_URING_F_NONBLOCK);
	mutex_unlock(&w->ctx->uring_lock);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_FALSE(test, opaque_refuse.armed);
	KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
	KUNIT_EXPECT_EQ(test, head->consumed, last);
	/* The hook is asked once per issue on the direct path, by sock_sendmsg() on the other. */
	KUNIT_EXPECT_EQ(test, opaque_hook_calls, direct ? 1U : 0U);
	KUNIT_EXPECT_EQ(test, opaque_direct.joined, IO_OPAQUE_TX_WAIT);
	KUNIT_ASSERT_NOT_NULL(test, follower->tx);
	tx = follower->tx;

	if (err == -EAGAIN) {
		KUNIT_ASSERT_EQ(test, ret, -EAGAIN);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 0);
		KUNIT_EXPECT_PTR_EQ(test, head->tx, tx);
		KUNIT_EXPECT_PTR_EQ(test,
				    list_first_entry(&tx->requests, struct io_opaque_req, send),
				    head);
		KUNIT_EXPECT_TRUE(test, head->tx_started);
		KUNIT_EXPECT_PTR_EQ(test, head->data, data);
		KUNIT_EXPECT_FALSE(test, w->req[0].flags & REQ_F_FAIL);
		KUNIT_EXPECT_EQ(test, follower->phase, IO_OPAQUE_SEND_WAIT);
		/* The next issue sends the frame, and a frame behind it is handed the queue. */
		mutex_lock(&w->ctx->uring_lock);
		ret = io_opaque_send(&w->req[0], IO_URING_F_NONBLOCK);
		mutex_unlock(&w->ctx->uring_lock);
		KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
		KUNIT_EXPECT_EQ(test, opaque_hook_calls, direct ? 2U : 0U);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, (s32)len);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.flags & IORING_CQE_F_OPAQUE_CONSUMED,
				last ? IORING_CQE_F_OPAQUE_CONSUMED : 0);
		KUNIT_EXPECT_PTR_EQ(test, head->tx, NULL);
		opaque_wire_read(test, w, got, 0, len);
		KUNIT_EXPECT_MEMEQ(test, got, source + off, len);
		KUNIT_EXPECT_EQ(test, follower->phase, IO_OPAQUE_QUEUED);
		KUNIT_EXPECT_FALSE(test, follower->canceled);
		KUNIT_EXPECT_PTR_EQ(test, mpscq_pop(&w->ctx->work_list, &w->ctx->work_head),
				    &w->req[1].io_task_work.node);
	} else {
		KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, err);
		KUNIT_EXPECT_TRUE(test, w->req[0].flags & REQ_F_FAIL);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.flags & IORING_CQE_F_OPAQUE_CONSUMED,
				last ? IORING_CQE_F_OPAQUE_CONSUMED : 0);
		KUNIT_EXPECT_PTR_EQ(test, head->tx, NULL);
		KUNIT_EXPECT_EQ(test, head->progress, 0ULL);
		/* The head had started, so its refusal fails the frame behind it. */
		KUNIT_EXPECT_TRUE(test, tx->failed);
		KUNIT_EXPECT_TRUE(test, follower->canceled);
		KUNIT_EXPECT_EQ(test, follower->phase, IO_OPAQUE_QUEUED);
		KUNIT_EXPECT_EQ(test, follower->result, -ECANCELED);
		KUNIT_EXPECT_PTR_EQ(test, mpscq_pop(&w->ctx->work_list, &w->ctx->work_head),
				    &w->req[1].io_task_work.node);
	}
	/* The references: given back by the direct path, left to cleanup by the other. */
	KUNIT_EXPECT_PTR_EQ(test, head->data, direct ? NULL : data);
	if (!direct)
		io_opaque_data_put(store, head->data);
	head->data = NULL;
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_tx_release(follower), NULL);
	KUNIT_EXPECT_FALSE(test, tx->failed);
	/* A LAST send took the table's reference, so the follower's is the last one. */
	io_opaque_data_put(store, follower->data);
	follower->data = NULL;
	if (!last) {
		opaque_test_free(store, 0);
	}
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	kunit_deactivate_static_stub(test, io_opaque_send_direct);
	kunit_deactivate_static_stub(test, io_opaque_tx_settle_head);
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

/*
 * What the joins of the issues of a test saw: how many, with the socket owned, with the ring
 * lock held, and how many times the security hook had been asked by then.
 */
static struct {
	unsigned int calls, owned, ring_locked, hooks;
} opaque_join_spy;

static int opaque_test_join_spy(struct io_opaque_req *op, struct sock *sk)
{
	struct kunit *test = kunit_get_current_test();
	int ret;

	opaque_join_spy.calls++;
	opaque_join_spy.owned += sock_owned_by_user_nocheck(sk);
	opaque_join_spy.ring_locked += mutex_is_locked(&op->req->ctx->uring_lock);
	opaque_join_spy.hooks = opaque_hook_calls;
	kunit_deactivate_static_stub(test, io_opaque_tx_join);
	ret = io_opaque_tx_join(op, sk);
	kunit_activate_static_stub(test, io_opaque_tx_join, opaque_test_join_spy);
	return ret;
}

/* The number of requests on the ring's list of those that wait for cancellation. */
static unsigned int opaque_wait_list_len(struct io_ring_ctx *ctx)
{
	struct io_opaque_req *op;
	unsigned int nr = 0;

	list_for_each_entry(op, &ctx->opaque_waits, cancel)
		nr++;
	return nr;
}

/*
 * Where a send joins its queue. An inline issue on a plain TCP socket joins once it owns the
 * socket, after it has asked the security hook, with the ring lock held; an io-wq issue, which
 * lists a request that waits under the ring lock, joins before the hook and the socket; and a
 * socket that is not plain is never owned by the issue, and joins unowned. A head finds its
 * queue free whichever way it joins, and sends the frame.
 */
static void opaque_send_join_place(struct kunit *test)
{
	static const struct {
		bool unlocked, plain;
		unsigned int owned, hooks;
	} cases[] = {
		/* inline, plain: joined owned, after the hook */
		{ false, true, 1, 1 },
		/* io-wq, plain: joined unowned, before the hook */
		{ true, true, 0, 0 },
		/* inline, replaced proto: joined unowned, and the hook is sock_sendmsg()'s */
		{ false, false, 0, 0 },
		{ true, false, 0, 0 },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 4000, GFP_KERNEL);
	struct sock *sk;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	opaque_wire_init(test, w);
	sk = w->tx->sk;
	opaque_wire_sndbuf(sk, SZ_1M);
	opaque_test_proto = tcp_prot;
	opaque_test_proto.sendmsg = opaque_test_sendmsg;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_proto_restore, sk), 0);
	kunit_activate_static_stub(test, io_opaque_send_security, opaque_test_security_probe);
	kunit_activate_static_stub(test, io_opaque_tx_join, opaque_test_join_spy);
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct io_opaque_req *op;
		int ret;

		lock_sock(sk);
		sk->sk_prot = cases[i].plain ? &tcp_prot : &opaque_test_proto;
		release_sock(sk);
		KUNIT_ASSERT_EQ(test, io_opaque_send_direct(sk), cases[i].plain);
		op = opaque_wire_req(test, w, store, 0, 0, 100, 4000);
		memset(&w->req[0].cqe, 0, sizeof(w->req[0].cqe));
		memset(&opaque_join_spy, 0, sizeof(opaque_join_spy));
		opaque_hook_calls = 0;
		ret = opaque_wire_issue(w, 0, cases[i].unlocked);
		lockdep_assert_none_held_once();
		KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 4000);
		KUNIT_EXPECT_EQ_MSG(test, opaque_join_spy.calls, 1U, "case %u", i);
		KUNIT_EXPECT_EQ_MSG(test, opaque_join_spy.owned, cases[i].owned, "case %u", i);
		/* An inline issue holds the ring lock for the join, and so does an io-wq issue. */
		KUNIT_EXPECT_EQ_MSG(test, opaque_join_spy.ring_locked, 1U, "case %u", i);
		/* The hook is asked with the socket not owned, before the join or after it. */
		KUNIT_EXPECT_EQ_MSG(test, opaque_join_spy.hooks, cases[i].hooks, "case %u", i);
		KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
		KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
		opaque_wire_read(test, w, got, 0, 4000);
		KUNIT_EXPECT_MEMEQ(test, got, source + 100, 4000);
		/* The ordinary path leaves the references to cleanup, which here is the test. */
		KUNIT_EXPECT_PTR_EQ(test, op->data, cases[i].plain ? NULL : data);
		io_opaque_data_put(store, op->data);
		op->data = NULL;
	}
	kunit_deactivate_static_stub(test, io_opaque_tx_join);
	kunit_deactivate_static_stub(test, io_opaque_send_security);
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/*
 * A request that finds a frame ahead of it, or is looked at again with the frame still
 * ahead, parks, and an inline issue has taken the socket to find that out: it lets it go
 * before it parks and before it returns, holding nothing of the socket's, and is listed once
 * for cancellation under the ring lock it still holds. It has not sent a byte. An io-wq issue
 * parks without the hook or the socket. The head is promoted when the frame ahead of it has
 * gone, and when it is issued again it sends after it.
 */
static void opaque_send_parked_under_socket(struct kunit *test)
{
	const struct io_uring_opaque_send_entry ent[6] = {
		[0 ... 5] = { .handle = io_opaque_handle(0, 9), .off = 0, .len = 80000 },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 6 * 80000 + 300, GFP_KERNEL);
	struct io_opaque_req *head, *follower;
	unsigned int done = 0, tries, hooks;
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
	opaque_wire_sndbuf(sk, SZ_32K);
	opaque_hook_calls = 0;
	kunit_activate_static_stub(test, io_opaque_send_security, opaque_test_security_probe);
	kunit_activate_static_stub(test, io_opaque_tx_join, opaque_test_join_spy);
	memset(&opaque_join_spy, 0, sizeof(opaque_join_spy));
	/* The head queues what fits, and keeps its place at the front. */
	head = opaque_vec_new(test, store, ent, ARRAY_SIZE(ent), 0);
	head->req = &w->req[0];
	head->op = U16_MAX;
	INIT_LIST_HEAD(&head->wait);
	INIT_LIST_HEAD(&head->cancel);
	INIT_LIST_HEAD(&head->send);
	io_opaque_send_resolve(head);
	w->req[0].async_data = head;
	follower = opaque_wire_req(test, w, store, 1, 0, 200, 300);
	KUNIT_ASSERT_EQ(test, opaque_wire_issue(w, 0, false), -EAGAIN);
	tx = head->tx;
	KUNIT_ASSERT_NOT_NULL(test, tx);
	KUNIT_EXPECT_EQ(test, opaque_join_spy.owned, 1U);
	KUNIT_EXPECT_EQ(test, opaque_hook_calls, 1U);

	/* An inline issue behind it: the socket is taken, the join says wait, the socket goes. */
	memset(&opaque_join_spy, 0, sizeof(opaque_join_spy));
	ret = opaque_wire_issue(w, 1, false);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_EQ(test, ret, IOU_ISSUE_SKIP_COMPLETE);
	KUNIT_EXPECT_EQ(test, opaque_join_spy.calls, 1U);
	KUNIT_EXPECT_EQ(test, opaque_join_spy.owned, 1U);
	KUNIT_EXPECT_EQ(test, opaque_join_spy.hooks, 2U);
	KUNIT_EXPECT_EQ(test, opaque_hook_calls, 2U);
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
	KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&w->ctx->uring_lock));
	KUNIT_EXPECT_EQ(test, follower->phase, IO_OPAQUE_SEND_WAIT);
	KUNIT_EXPECT_PTR_EQ(test, follower->tx, tx);
	KUNIT_EXPECT_PTR_EQ(test, follower->data, data);
	KUNIT_EXPECT_EQ(test, follower->progress, 0ULL);
	KUNIT_EXPECT_EQ(test, w->req[1].cqe.res, 0);
	KUNIT_EXPECT_EQ(test, opaque_wait_list_len(w->ctx), 1U);
	KUNIT_EXPECT_PTR_EQ(test, list_first_entry(&tx->requests, struct io_opaque_req, send),
			    head);

	/* Looked at again with the head still ahead: parks again, and is listed once. */
	memset(&opaque_join_spy, 0, sizeof(opaque_join_spy));
	ret = opaque_wire_issue(w, 1, false);
	KUNIT_ASSERT_EQ(test, ret, IOU_ISSUE_SKIP_COMPLETE);
	KUNIT_EXPECT_EQ(test, opaque_join_spy.owned, 1U);
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
	KUNIT_EXPECT_EQ(test, follower->phase, IO_OPAQUE_SEND_WAIT);
	KUNIT_EXPECT_EQ(test, opaque_wait_list_len(w->ctx), 1U);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 3);

	/* The same from io-wq: no hook, no socket, and the ring lock back as it found it. */
	hooks = opaque_hook_calls;
	memset(&opaque_join_spy, 0, sizeof(opaque_join_spy));
	ret = opaque_wire_issue(w, 1, true);
	KUNIT_ASSERT_EQ(test, ret, IOU_ISSUE_SKIP_COMPLETE);
	KUNIT_EXPECT_EQ(test, opaque_join_spy.owned, 0U);
	KUNIT_EXPECT_EQ(test, opaque_join_spy.ring_locked, 1U);
	KUNIT_EXPECT_EQ(test, opaque_hook_calls, hooks);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&w->ctx->uring_lock));
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
	KUNIT_EXPECT_EQ(test, opaque_wait_list_len(w->ctx), 1U);
	kunit_deactivate_static_stub(test, io_opaque_tx_join);

	/* The peer reads, and the head sends the rest of its frame, which frees the queue. */
	opaque_put_spy_start(test, sk);
	for (tries = 0; tries < 4000; tries++) {
		ret = opaque_wire_issue(w, 0, false);
		if (ret == IOU_COMPLETE)
			break;
		KUNIT_ASSERT_EQ(test, ret, -EAGAIN);
		done += opaque_wire_drain(w, got + done, 6 * 80000 - done);
		usleep_range(500, 1000);
	}
	opaque_put_spy_stop(test);
	KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
	KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 6 * 80000);
	KUNIT_EXPECT_PTR_EQ(test, head->tx, NULL);
	KUNIT_EXPECT_EQ(test, follower->phase, IO_OPAQUE_QUEUED);
	KUNIT_EXPECT_FALSE(test, follower->canceled);
	KUNIT_EXPECT_PTR_EQ(test, mpscq_pop(&w->ctx->work_list, &w->ctx->work_head),
			    &w->req[1].io_task_work.node);
	/* Resumed as the task work does it, it is the head, sends, and the bytes follow. */
	list_del_init(&follower->cancel);
	follower->phase = IO_OPAQUE_IDLE;
	KUNIT_EXPECT_EQ(test, opaque_wait_list_len(w->ctx), 0U);
	ret = opaque_wire_issue(w, 1, false);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
	KUNIT_EXPECT_EQ(test, w->req[1].cqe.res, 300);
	KUNIT_EXPECT_PTR_EQ(test, follower->tx, NULL);
	KUNIT_EXPECT_PTR_EQ(test, follower->data, NULL);
	opaque_wire_read(test, w, got, done, 6 * 80000 + 300);
	for (tries = 0; tries < 6; tries++)
		KUNIT_EXPECT_MEMEQ(test, got + tries * 80000, source, 80000);
	KUNIT_EXPECT_MEMEQ(test, got + 6 * 80000, source + 200, 300);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
	kunit_deactivate_static_stub(test, io_opaque_send_security);
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/* How the requests of opaque_life_run() end. */
enum {
	OPAQUE_LIFE_DONE,	/* inline, to its end under the socket */
	OPAQUE_LIFE_DONE_IOWQ,	/* the same, issued as io-wq does */
	OPAQUE_LIFE_FAILED,	/* to its end with an error, under the socket */
	OPAQUE_LIFE_ORDINARY,	/* a socket that is not plain: cleanup gives everything back */
	OPAQUE_LIFE_RETRY,	/* EAGAIN, and the issue that completes it */
	OPAQUE_LIFE_RETRY_CANCEL, /* EAGAIN, and then it fails without being issued again */
	OPAQUE_LIFE_PARKED_CANCEL, /* parked behind a head, and canceled there */
	OPAQUE_LIFE_UNISSUED,	/* prepared, and failed before it was issued */
	OPAQUE_LIFE_REFUSED,	/* the queue that it joins has failed */
	OPAQUE_LIFE_NR,
};

static void opaque_life_run(struct kunit *test, unsigned int mode, bool vector, bool last)
{
	const bool retry = mode == OPAQUE_LIFE_RETRY || mode == OPAQUE_LIFE_RETRY_CANCEL ||
			   mode == OPAQUE_LIFE_PARKED_CANCEL || mode == OPAQUE_LIFE_REFUSED;
	const u64 handle = io_opaque_handle(0, 9);
	/* Ranges that fit the socket, or too many for it while the peer does not read. */
	const u32 nr = retry ? 6 : 3, len = retry ? 80000 : 4000;
	struct io_uring_opaque_send_entry ent[6];
	u16 flags = (vector ? IORING_OPAQUE_SEND_VECTOR : 0) | (last ? IORING_OPAQUE_SEND_LAST : 0);
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *got = kunit_kmalloc(test, 6 * 80000, GFP_KERNEL);
	struct io_opaque_req *op, *other = NULL;
	unsigned long user = 0;
	unsigned int done = 0, tries;
	struct sock *sk;
	int ret, i;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, got);
	for (i = 0; i < ARRAY_SIZE(ent); i++)
		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = handle, .off = retry ? 0 : 1000 * i,
			.len = retry ? 80000 : 1000 + i,
		};
	opaque_test_publish(store, 0, 9, data);
	opaque_wire_init(test, w);
	sk = w->tx->sk;
	opaque_wire_prep_setup(test, w, store);
	opaque_wire_sndbuf(sk, retry ? SZ_32K : SZ_1M);
	if (vector) {
		user = opaque_uvec_map(test);
		opaque_uvec_put(test, user, ent, nr * sizeof(*ent));
	}
	if (mode == OPAQUE_LIFE_FAILED)
		KUNIT_ASSERT_EQ(test, kernel_sock_shutdown(w->tx, SHUT_WR), 0);
	if (mode == OPAQUE_LIFE_ORDINARY) {
		opaque_test_proto = tcp_prot;
		opaque_test_proto.sendmsg = opaque_test_sendmsg;
		KUNIT_ASSERT_EQ(test,
				kunit_add_action_or_reset(test, opaque_test_proto_restore, sk), 0);
		lock_sock(sk);
		sk->sk_prot = &opaque_test_proto;
		release_sock(sk);
	}

	op = opaque_wire_prep(test, w, 0, flags, vector ? user : handle, 0, vector ? nr : len,
			      0);

	switch (mode) {
	case OPAQUE_LIFE_DONE:
	case OPAQUE_LIFE_DONE_IOWQ:
	case OPAQUE_LIFE_FAILED:
	case OPAQUE_LIFE_ORDINARY:
		ret = opaque_wire_issue(w, 0, mode == OPAQUE_LIFE_DONE_IOWQ);
		lockdep_assert_none_held_once();
		KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
		if (mode == OPAQUE_LIFE_FAILED)
			KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, -EPIPE);
		/*
		 * A send that ended with the socket owned has given back its place in the queue
		 * and its references there; any other leaves them to cleanup.
		 */
		if (mode != OPAQUE_LIFE_ORDINARY) {
			KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
			KUNIT_EXPECT_PTR_EQ(test, op->data, NULL);
			if (vector)
				KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
		}
		break;
	case OPAQUE_LIFE_RETRY:
	case OPAQUE_LIFE_RETRY_CANCEL:
	case OPAQUE_LIFE_PARKED_CANCEL:
	case OPAQUE_LIFE_REFUSED:
		ret = opaque_wire_issue(w, 0, false);
		KUNIT_ASSERT_EQ(test, ret, -EAGAIN);
		/* Held, with its place at the head. */
		KUNIT_ASSERT_NOT_NULL(test, op->tx);
		break;
	case OPAQUE_LIFE_UNISSUED:
		break;
	}

	if (mode == OPAQUE_LIFE_PARKED_CANCEL || mode == OPAQUE_LIFE_REFUSED) {
		other = opaque_wire_prep(test, w, 1, 0, handle, 200, 300, 0);
		if (mode == OPAQUE_LIFE_REFUSED)
			op->tx->failed = true;
		ret = opaque_wire_issue(w, 1, false);
		if (mode == OPAQUE_LIFE_REFUSED) {
			/* The failed queue refuses it; that is no completion, the core fails it. */
			KUNIT_ASSERT_EQ(test, ret, -ECANCELED);
			op->tx->failed = false;
			KUNIT_EXPECT_PTR_EQ(test, other->tx, NULL);
			KUNIT_EXPECT_PTR_EQ(test, other->data, NULL);
			io_opaque_fail(&w->req[1]);
			opaque_wire_req_cleanup(w, 1);
		} else {
			KUNIT_ASSERT_EQ(test, ret, IOU_ISSUE_SKIP_COMPLETE);
			KUNIT_EXPECT_EQ(test, other->phase, IO_OPAQUE_SEND_WAIT);
			/* Canceled where it waits: task work wakes it, and cleanup is left. */
			mutex_lock(&w->ctx->uring_lock);
			KUNIT_EXPECT_TRUE(test, io_opaque_cancel_all(w->ctx, NULL, true));
			mutex_unlock(&w->ctx->uring_lock);
			KUNIT_EXPECT_TRUE(test, other->canceled);
			KUNIT_EXPECT_EQ(test, other->phase, IO_OPAQUE_QUEUED);
			KUNIT_EXPECT_EQ(test, other->result, -ECANCELED);
			KUNIT_EXPECT_PTR_EQ(test, mpscq_pop(&w->ctx->work_list, &w->ctx->work_head),
					    &w->req[1].io_task_work.node);
			list_del_init(&other->cancel);
			other->phase = IO_OPAQUE_IDLE;
			req_set_fail(&w->req[1]);
			io_opaque_fail(&w->req[1]);
			opaque_wire_req_cleanup(w, 1);
		}
		/* The head has its place still. */
		KUNIT_EXPECT_PTR_NE(test, op->tx, NULL);
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), last ? 1 : 2);
	}

	switch (mode) {
	case OPAQUE_LIFE_RETRY:
	case OPAQUE_LIFE_PARKED_CANCEL:
	case OPAQUE_LIFE_REFUSED:
		/* The peer reads, and each issue goes on from the cursor until all is queued. */
		for (tries = 0; tries < 4000; tries++) {
			ret = opaque_wire_issue(w, 0, false);
			if (ret == IOU_COMPLETE)
				break;
			KUNIT_ASSERT_EQ(test, ret, -EAGAIN);
			done += opaque_wire_drain(w, got + done, 6 * 80000 - done);
			usleep_range(500, 1000);
		}
		lockdep_assert_none_held_once();
		KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, (s32)(vector ? nr : 1) * 80000);
		break;
	case OPAQUE_LIFE_RETRY_CANCEL:
	case OPAQUE_LIFE_UNISSUED:
		/* Canceled, or failed before it ever ran. */
		req_set_fail(&w->req[0]);
		io_opaque_fail(&w->req[0]);
		break;
	default:
		break;
	}
	/* Cleanup runs once for each request, and has nothing to give back that was given. */
	opaque_wire_req_cleanup(w, 0);
	KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
	KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
	KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
	/*
	 * Whatever the send consumed or pinned is back, and the object is the table's again, or
	 * gone with a LAST send that ran. A LAST send that was canceled before it was admitted
	 * took nothing, or it ran only as far as the point at which it did.
	 */
	if (opaque_slot_data(store, 0)) {
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
		opaque_test_free(store, 0);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/*
 * A send gives back its references and its queue place exactly once, whichever way it ends: to
 * its end under the socket, which does it there, before the CQE is posted; to its end on a
 * socket that is not plain, by an error, after EAGAIN and a retry, canceled where it waits or
 * where it was left after EAGAIN, refused by a queue that failed, and never issued. Cleanup
 * does it for every one that has not, and never for one that has: a reference given back twice
 * trips the version's count, and one that nobody gives back leaves its bytes and extents
 * charged. A vector and LAST give back their references as they always did, in the same runs.
 */
static void opaque_send_release_once(struct kunit *test)
{
	unsigned int mode, vector, last, poison;

	opaque_test_params_save(test);
	/* Once with the request of each send filled with a byte first, for prep to write over. */
	for (poison = 0; poison < 2; poison++) {
		io_opaque_send_poison = poison ? 0xa5 : -1;
		for (mode = 0; mode < OPAQUE_LIFE_NR; mode++)
			for (vector = 0; vector < 2; vector++)
				for (last = 0; last < 2; last++) {
					/* A frame behind a LAST send finds its object gone. */
					if (last && (mode == OPAQUE_LIFE_PARKED_CANCEL ||
						     mode == OPAQUE_LIFE_REFUSED))
						continue;
					opaque_life_run(test, mode, vector, last);
				}
	}
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
 * handle, LAST or not, a vector whose block cannot grow, which fails with the
 * socket owned and a frame that joined meanwhile parked behind it, and a
 * vector whose object a LAST frame takes while the block grows, so that a LAST
 * vector finds nothing when it looks again, and any other, which pinned the
 * version, goes on with it. A head that fails leaves with the error, which is
 * not a completion, nothing sent and nothing consumed, and releases the socket
 * and does not fail the queue; what a vector had pinned is left for its cleanup,
 * and the frame behind it is promoted with its references and sends them.
 */
static void opaque_send_locked_admit_fails(struct kunit *test)
{
	struct io_uring_opaque_send_entry ent[2];
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 3200, GFP_KERNEL);
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
			KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
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
			KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
			KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
			KUNIT_EXPECT_FALSE(test, head->consumed);
			KUNIT_EXPECT_PTR_EQ(test, head->tx, NULL);
			KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), data);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_send_locked_calls),
					calls);
			/*
			 * The head that is not LAST still holds the version that it pinned, for its
			 * cleanup: one reference for the two entries. LAST had pinned nothing.
			 */
			KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), last ? 2 : 3);
			opaque_vec_failed(test, head, last ? 0 : 2);
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

			/*
			 * The block grows again, and the frame that joins meanwhile is LAST: it
			 * takes the object. A LAST head, which looks again, finds none. Any other
			 * has pinned the version, and goes on with it.
			 */
			calls = atomic_long_read(&io_opaque_send_locked_calls);
			opaque_grow_entries(ent, io_opaque_handle(0, 9));
			head = opaque_wire_vec(test, w, store, 0, ent, ARRAY_SIZE(ent), flags);
			follower = opaque_wire_req(test, w, store, 1, IORING_OPAQUE_SEND_LAST,
						   200, 300);
			memset(&w->req[0].cqe, 0, sizeof(w->req[0].cqe));
			memset(&w->req[1].cqe, 0, sizeof(w->req[1].cqe));
			memset(&opaque_admit_hook, 0, sizeof(opaque_admit_hook));
			opaque_admit_hook.sk = sk;
			opaque_admit_hook.follower = follower;
			opaque_window_open(test, opaque_admit_between);
			ret = opaque_wire_issue(w, 0, unlocked);
			opaque_window_close(test);
			lockdep_assert_none_held_once();
			KUNIT_EXPECT_TRUE(test, opaque_admit_hook.owned);
			KUNIT_EXPECT_EQ(test, opaque_window.calls, 1U);
			KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
			KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
			if (last) {
				/* The head consumed and holds nothing; the queue is not failed. */
				KUNIT_EXPECT_EQ(test, ret, -ESTALE);
				KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 0);
				KUNIT_EXPECT_FALSE(test, head->consumed);
				KUNIT_EXPECT_EQ(test, head->vec->pinned, 0U);
				KUNIT_EXPECT_PTR_EQ(test, head->tx, NULL);
			} else {
				/*
				 * It sends its two ranges of the version it pinned, ahead of the
				 * frame that took the object, and lets go of it.
				 */
				KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
				KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 3200);
				KUNIT_EXPECT_FALSE(test, head->consumed);
				KUNIT_EXPECT_EQ(test, head->vec->pinned, 0U);
				KUNIT_EXPECT_PTR_EQ(test, head->tx, NULL);
				opaque_wire_read(test, w, got, 0, 3200);
				KUNIT_EXPECT_MEMEQ(test, got, source + 1500, 1000);
				KUNIT_EXPECT_MEMEQ(test, got + 1000, source + 3900, 2200);
			}
			/* One part went out of the owned path, for the head that sent. */
			KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_send_locked_calls),
					calls + !last);
			calls += !last;
			/* The frame took the object, table reference and all, and is promoted. */
			KUNIT_EXPECT_EQ(test, opaque_admit_hook.joined, IO_OPAQUE_TX_WAIT);
			KUNIT_EXPECT_TRUE(test, follower->consumed);
			KUNIT_EXPECT_PTR_EQ(test, follower->data, data);
			KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
			KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
			KUNIT_EXPECT_EQ(test, follower->phase, IO_OPAQUE_QUEUED);
			KUNIT_EXPECT_FALSE(test, follower->canceled);
			KUNIT_ASSERT_NOT_NULL(test, follower->tx);
			KUNIT_EXPECT_FALSE(test, follower->tx->failed);
			KUNIT_EXPECT_PTR_EQ(test, mpscq_pop(&w->ctx->work_list, &w->ctx->work_head),
					    &w->req[1].io_task_work.node);
			/* It resumes as the head and sends the object it consumed, and frees it. */
			follower->phase = IO_OPAQUE_IDLE;
			opaque_put_spy_start(test, sk);
			ret = opaque_wire_issue(w, 1, unlocked);
			opaque_put_spy_stop(test);
			KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
			opaque_put_spy_expect(test, 1, 1);
			KUNIT_EXPECT_EQ(test, w->req[1].cqe.res, 300);
			KUNIT_EXPECT_EQ(test,
					!!(w->req[1].cqe.flags & IORING_CQE_F_OPAQUE_CONSUMED), 1);
			KUNIT_EXPECT_PTR_EQ(test, follower->data, NULL);
			KUNIT_EXPECT_PTR_EQ(test, follower->tx, NULL);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_send_locked_calls),
					calls + 1);
			opaque_wire_read(test, w, got, 0, 300);
			KUNIT_EXPECT_MEMEQ(test, got, source + 200, 300);
			KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
			KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
			/* The next round, and the cleanup, have an object again. */
			data = opaque_sparse_data(test, store);
			opaque_test_publish(store, 0, 9, data);
		}
	}
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	memset(&opaque_direct, 0, sizeof(opaque_direct));
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
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
 * sleep. The handle may be freed while it sleeps, which leaves the send's
 * reference as the last one the version has: it is put after the socket.
 */
static struct {
	struct delayed_work work;
	struct opaque_wire *w;
	struct io_opaque_store *store;
	u8 *got;
	u32 total;
	bool freed, ring_free, unowned;
	struct completion done;
} opaque_reader;

static void opaque_reader_fn(struct work_struct *work)
{
	u32 done = 0;

	/* The sender went to sleep in TCP long ago: the ring lock is free, the socket unowned. */
	opaque_reader.ring_free = !mutex_is_locked(&opaque_reader.w->ctx->uring_lock);
	opaque_reader.unowned = !sock_owned_by_user_nocheck(opaque_reader.w->tx->sk);
	if (opaque_reader.freed) {
		struct io_opaque_store *store = opaque_reader.store;

		opaque_test_free(store, 0);
	}
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

static void opaque_blocking_run(struct kunit *test, struct opaque_wire *w,
				struct io_opaque_store *store, bool freed)
{
	struct io_uring_opaque_send_entry ent[6];
	const u32 total = 6 * 80000;
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, total, GFP_KERNEL);
	long calls = atomic_long_read(&io_opaque_send_locked_calls);
	struct io_opaque_req *op;
	ktime_t start;
	unsigned int i;
	s64 ms;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	/* Far more than the buffers on both sides will take before a read. */
	opaque_wire_sndbuf(w->tx->sk, SZ_32K);
	for (i = 0; i < ARRAY_SIZE(ent); i++)
		ent[i] = (struct io_uring_opaque_send_entry) {
			.handle = io_opaque_handle(0, 9), .off = 0, .len = 80000,
		};
	op = opaque_wire_vec(test, w, store, 0, ent, ARRAY_SIZE(ent), 0);
	memset(&opaque_reader, 0, sizeof(opaque_reader));
	opaque_reader.w = w;
	opaque_reader.store = store;
	opaque_reader.freed = freed;
	opaque_reader.got = got;
	opaque_reader.total = total;
	init_completion(&opaque_reader.done);
	INIT_DELAYED_WORK(&opaque_reader.work, opaque_reader_fn);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_reader_cancel, NULL), 0);
	schedule_delayed_work(&opaque_reader.work, msecs_to_jiffies(150));

	opaque_put_spy_start(test, w->tx->sk);
	start = ktime_get();
	ret = opaque_wire_issue(w, 0, true);
	ms = ktime_ms_delta(ktime_get(), start);
	opaque_put_spy_stop(test);
	lockdep_assert_none_held_once();
	KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
	/* The version is put once, after the socket, and only if nothing else held it. */
	opaque_put_spy_expect(test, freed, freed);
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
	if (freed) {
		/* The object went with the send. */
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
	} else {
		KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
		opaque_test_free(store, 0);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

static void opaque_send_locked_blocking(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_wire_init(test, w);
	opaque_blocking_run(test, w, store, false);
	/* Again, with the handle freed from the peer's side while the sender sleeps. */
	opaque_blocking_run(test, w, store, true);
}

/* The wake-ups of a socket that waiters for budget are waiting on, counted. */
struct opaque_budget_watch {
	struct sock *sk;
	wait_queue_entry_t entry;
};

static unsigned int opaque_budget_wakes_seen;

static int opaque_budget_wake_fn(wait_queue_entry_t *entry, unsigned int mode, int flags,
				 void *key)
{
	opaque_budget_wakes_seen++;
	return 0;
}

static void opaque_budget_watch_stop(void *arg)
{
	struct opaque_budget_watch *watch = arg;

	remove_wait_queue(sk_sleep(watch->sk), &watch->entry);
}

/*
 * Freeing a version lets in every waiter for budget that the budget it gives
 * back lets go on, and not only the first batch of them: a wake takes at most
 * IO_OPAQUE_WAKE_BATCH, and the rest, left to the retry work, may wait for a
 * timer that is a long way off, as freeing the extents one by one never made
 * them. Seventy-two ready waiters are three batches, and are all woken by
 * the free itself, in three looks and no more.
 */
static void opaque_extents_free_wakes_all(struct kunit *test)
{
	const unsigned int nr = 2 * IO_OPAQUE_WAKE_BATCH + 8;
	struct opaque_budget_watch *watch = kunit_kzalloc(test, sizeof(*watch), GFP_KERNEL);
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_stream *stream = kunit_kzalloc(test, sizeof(*stream), GFP_KERNEL);
	struct io_opaque_req *ops = kunit_kcalloc(test, nr, sizeof(*ops), GFP_KERNEL);
	long looks = atomic_long_read(&io_opaque_budget_wakes);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, watch);
	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, stream);
	KUNIT_ASSERT_NOT_NULL(test, ops);
	opaque_wire_init(test, w);
	/* The batch limit arms the retry work, which is not for this test to run. */
	INIT_DELAYED_WORK(&store->retry_work, io_opaque_retry_work);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_cancel_work,
							&store->retry_work), 0);
	opaque_test_stream(stream, store);
	watch->sk = w->tx->sk;
	init_waitqueue_func_entry(&watch->entry, opaque_budget_wake_fn);
	add_wait_queue(sk_sleep(watch->sk), &watch->entry);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_budget_watch_stop, watch), 0);
	for (i = 0; i < nr; i++) {
		ops[i].store = store;
		ops[i].req = &w->req[0];
		ops[i].stream = stream;
		ops[i].phase = IO_OPAQUE_BUDGET_POLL;
		ops[i].retry_at = jiffies;
		INIT_LIST_HEAD(&ops[i].wait);
		INIT_LIST_HEAD(&ops[i].cancel);
		list_add_tail(&ops[i].wait, &store->budget_waits);
	}
	opaque_budget_wakes_seen = 0;

	/* The version's forty extents go, and the budget they held with them. */
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, opaque_budget_wakes_seen, nr);
	KUNIT_EXPECT_TRUE(test, list_empty(&store->budget_waits));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_budget_wakes) - looks, 3L);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

#ifdef CONFIG_IO_URING_ZCRX_KUNIT_TEST
/*
 * Tests of a store that binds a MAIO instance. The aids of zcrx_test.c create
 * the instance and a page pool that uses its provider, without a device. Every
 * test returns all pages before it ends, and the deferred actions of the aids
 * destroy the pool and the instance.
 */
#define OPAQUE_BATCH	PP_ALLOC_CACHE_REFILL

/*
 * A store that binds the instance, and marks its sends as io_opaque_alloc() does. The test owns
 * the reference to the instance.
 */
static struct io_opaque_store *opaque_maio_store(struct kunit *test,
						 struct io_zcrx_ifq *ifq)
{
	struct io_opaque_store *store = opaque_test_store(test);

	store->maio = ifq;
	store->send_ubuf_flags = SKBFL_SHARED_FRAG;
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
static struct sk_buff *opaque_pool_page_skb(struct kunit *test, struct page *page,
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
			     struct sk_buff *skb, u32 len)
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
	KUNIT_ASSERT_EQ(test, io_opaque_vector(stream->store, data), 0);
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
		.flags = IORING_OPAQUE_CFG_MAIO,
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
	/* The sends of a bound store are marked from the start, not by the capture's flag. */
	KUNIT_EXPECT_EQ(test, io_opaque_send_ubuf_flags(store), (u8)SKBFL_SHARED_FRAG);
	KUNIT_EXPECT_FALSE(test, store->maio_pages);
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
	KUNIT_EXPECT_EQ(test, io_opaque_send_ubuf_flags(store), (u8)0);
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
	skb = opaque_pool_page_skb(test, page, 128, 1024);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 1024, false), 1024);
	stream.next = 1024;
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, page);
	KUNIT_EXPECT_EQ(test, extent->offset, 128U);
	KUNIT_EXPECT_TRUE(test, extent->backing->area);
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_MAIO);
	KUNIT_EXPECT_FALSE(test, extent->backing->copied);
	/* A pool reference, not a page reference. */
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 2L);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE + OPAQUE_TEST_BLOCK);

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
	/* The version gave back its page, chunk and vector; the stream holds its block. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), OPAQUE_TEST_BLOCK);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * A backing that holds several pool references gives all of them back, and the
 * pool gets the page once.
 */
static void opaque_maio_pool_units(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 2 * OPAQUE_BATCH);
	struct page_pool *pp = io_zcrx_maio_test_pool(test, ifq);
	struct io_opaque_store *store = opaque_maio_store(test, ifq);
	struct io_opaque_extent *extent;
	struct page *page = opaque_pool_page(test, pp);

	/* The reference of the driver passes to the backing, and so do two more. */
	io_zcrx_maio_page_get(ifq, page);
	io_zcrx_maio_page_get(ifq, page);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 3L);
	extent = io_opaque_extent_new(store, page, 0, 128, 0, IO_OPAQUE_EXT_NO_MEMCG);
	KUNIT_ASSERT_FALSE(test, IS_ERR(extent));
	extent->backing->ref = IO_OPAQUE_REF_MAIO;
	extent->backing->units = 3;
	KUNIT_EXPECT_TRUE(test, ptr_ring_empty(&pp->ring));
	io_opaque_extent_free(store, extent);
	KUNIT_EXPECT_FALSE(test, ptr_ring_empty(&pp->ring));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 1L);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * A bound store passes on the references of the skb too: the pool reference to
 * an area page that a pool of the instance owns, and the page reference to
 * a page of the area that no pool owns. Neither takes a reference of its own, and
 * the area has the pages back when the store lets go.
 */
static void opaque_maio_steal(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 2 * OPAQUE_BATCH);
	struct page_pool *pp = io_zcrx_maio_test_pool(test, ifq);
	struct io_opaque_store *store = opaque_maio_store(test, ifq);
	struct io_zcrx_area *area = ifq->areas[0];
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct io_opaque_extent *pooled, *outsider;
	struct page *page = opaque_pool_page(test, pp), *plain;
	struct sk_buff *skb;
	u64 off, stolen = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN);
	u32 nfree;

	plain = io_zcrx_maio_copy_page(ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, plain);
	nfree = area->free_count;
	/* The skb holds a page reference to the second page. */
	get_page(plain);
	skb = opaque_pool_page_skb(test, page, 0, 1000);
	skb_add_rx_frag(skb, 1, plain, 0, 1000, PAGE_SIZE);
	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, skb->len), 2000);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (u8)0);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN), stolen + 2);
	pooled = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	outsider = list_last_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_ASSERT_PTR_NE(test, pooled, outsider);
	KUNIT_EXPECT_PTR_EQ(test, pooled->backing->page, page);
	KUNIT_EXPECT_EQ(test, pooled->backing->ref, (u8)IO_OPAQUE_REF_MAIO);
	KUNIT_EXPECT_EQ(test, pooled->backing->units, (u16)1);
	KUNIT_EXPECT_TRUE(test, pooled->backing->area);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 1L);
	KUNIT_EXPECT_PTR_EQ(test, outsider->backing->page, plain);
	KUNIT_EXPECT_EQ(test, outsider->backing->ref, (u8)IO_OPAQUE_REF_PAGE);
	KUNIT_EXPECT_EQ(test, outsider->backing->units, (u16)1);
	KUNIT_EXPECT_TRUE(test, outsider->backing->area);
	KUNIT_EXPECT_EQ(test, page_count(plain), 2);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 0LL);

	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 1L);
	KUNIT_EXPECT_EQ(test, page_count(plain), 2);
	KUNIT_EXPECT_TRUE(test, ptr_ring_empty(&pp->ring));
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_FALSE(test, ptr_ring_empty(&pp->ring));
	KUNIT_EXPECT_EQ(test, page_count(plain), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	KUNIT_EXPECT_EQ(test, area->free_count, nfree);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	io_zcrx_maio_page_put(ifq, plain);
	KUNIT_EXPECT_EQ(test, area->free_count, nfree + 1);
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
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PAGE);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&pooled->pp_ref_count), 1L);
	KUNIT_EXPECT_EQ(test, page_count(pooled), 3);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, page_count(pooled), 2);

	/* A page of the area that no pool owns, in an skb marked for recycling. */
	plain = io_zcrx_maio_copy_page(ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, plain);
	get_page(plain);
	skb = opaque_pool_page_skb(test, plain, 0, 256);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 256, false), 256);
	stream.next += 256;
	extent = list_last_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, plain);
	KUNIT_EXPECT_TRUE(test, extent->backing->area);
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PAGE);
	KUNIT_EXPECT_EQ(test, page_count(plain), 3);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, page_count(plain), 2);

	/* Neither page is counted as copied. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)2 * PAGE_SIZE + OPAQUE_TEST_BLOCK);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, page_count(pooled), 1);
	KUNIT_EXPECT_EQ(test, page_count(plain), 1);
	io_opaque_capture_release(&stream);
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
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 2);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)2 * PAGE_SIZE + OPAQUE_TEST_BLOCK);
	for (i = 0; i < 2; i++) {
		extent = i ? second : extent;
		KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_off(ifq, extent->backing->page, &off));
		KUNIT_EXPECT_TRUE(test, extent->backing->copied);
		KUNIT_EXPECT_TRUE(test, extent->backing->area);
		KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_MAIO);
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
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), OPAQUE_TEST_BLOCK);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
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
	skb = opaque_pool_page_skb(test, page, 64, 200);

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
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, area->free_count, nfree);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);

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
	io_opaque_capture_release(&stream);
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
	skb = opaque_pool_page_skb(test, fallback, 16, 300);
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
	io_opaque_capture_release(&stream);
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
		skb[i] = opaque_pool_page_skb(test, pages[i], offs[i], lens[i]);
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
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	opaque_test_scratch(test, &stream);

	/* No free page: nothing is charged and nothing is kept. */
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 100, false), -ENOMEM);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
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
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)PAGE_SIZE + OPAQUE_TEST_BLOCK);
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
	io_opaque_capture_release(&stream);
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
	opaque_test_publish(store, 0, 9, data);
	KUNIT_EXPECT_EQ(test, io_opaque_compact_start(&op), -EOPNOTSUPP);
	KUNIT_EXPECT_FALSE(test, opaque_test_slot(store, 0)->compacting);
	op.handle = 10ULL << IO_OPAQUE_INDEX_BITS;
	KUNIT_EXPECT_EQ(test, io_opaque_compact_start(&op), -ESTALE);

	sparse = opaque_sparse_data(test, plain);
	KUNIT_EXPECT_FALSE(test, sparse->mapped);

	kfree_skb(skb);
	opaque_slot_set(store, 0, NULL);
	io_opaque_data_put(store, data);
	io_opaque_data_put(plain, sparse);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * CREATE in a bound store copies into ordinary pages and not into the area:
 * the process has the bytes already, so there is nothing to make visible to
 * it. The object is not mapped and not published as MAPPED, every extent is
 * outside the area, a COMPACT is the no-op of any created object, and the area
 * neither gives a page when the object is made nor takes one when it is freed.
 */
static void opaque_maio_create(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);
	struct io_opaque_store *store = opaque_maio_store(test, ifq);
	struct io_zcrx_area *area = ifq->areas[0];
	struct io_kiocb *req = kunit_kzalloc(test, sizeof(*req), GFP_KERNEL);
	struct io_opaque_req op = {
		.store = store, .req = req, .op = IORING_OPAQUE_CREATE,
		.length = 2 * PAGE_SIZE + 100,
	};
	struct io_opaque_extent *extent;
	struct io_opaque_data *data;
	u64 flags = 0, total = 0;
	unsigned long addr;
	u8 *pattern, *after;

	KUNIT_ASSERT_NOT_NULL(test, req);
	addr = kunit_vm_mmap(test, NULL, 0, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_ANONYMOUS | MAP_PRIVATE, 0);
	if (!addr || addr >= TASK_SIZE)
		kunit_skip(test, "no userspace mapping for CREATE input");
	pattern = kunit_kmalloc(test, op.length, GFP_KERNEL);
	after = kunit_kzalloc(test, op.length, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pattern);
	KUNIT_ASSERT_NOT_NULL(test, after);
	get_random_bytes(pattern, op.length);
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)addr, pattern, op.length), 0UL);
	op.addr = (void __user *)addr;

	op.data = io_opaque_data_alloc();
	KUNIT_ASSERT_NOT_NULL(test, op.data);
	KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&op), 0);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, ALIGN(op.length, PAGE_SIZE), true));
	op.create_reserved = ALIGN(op.length, PAGE_SIZE);
	KUNIT_ASSERT_EQ(test, io_opaque_create_build(&op), 0);
	KUNIT_ASSERT_EQ(test, io_opaque_publish(&op, &flags), (int)op.length);
	KUNIT_EXPECT_EQ(test, flags, 0ULL);

	/* No page of the area is in use, and the object is not mapped. */
	KUNIT_EXPECT_EQ(test, area->free_count, 16U);
	KUNIT_EXPECT_EQ(test, opaque_slots_in(ifq, IO_ZCRX_KSLOT_OWNED), 0U);
	data = opaque_slot_data(store, op.slot);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_EXPECT_FALSE(test, data->mapped);
	KUNIT_EXPECT_TRUE(test, data->dense);
	KUNIT_EXPECT_EQ(test, data->length, (u64)op.length);
	KUNIT_EXPECT_GE(test, data->nr, 1U);
	list_for_each_entry(extent, &data->extents, list) {
		struct io_uring_opaque_extent pair = io_opaque_extent_pair(store, extent);

		KUNIT_EXPECT_FALSE(test, extent->backing->area);
		KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_UNIT);
		KUNIT_EXPECT_EQ(test, pair.area_off, IORING_OPAQUE_EXTENT_UNMAPPED);
		KUNIT_EXPECT_EQ(test, pair.len, (u64)extent->length);
		total += pair.len;
	}
	KUNIT_EXPECT_EQ(test, total, data->length);
	opaque_test_copy(data, after);
	KUNIT_EXPECT_MEMEQ(test, after, pattern, op.length);

	/* It is dense, so COMPACT does nothing, and does not refuse as for a mapped object. */
	KUNIT_EXPECT_EQ(test, io_opaque_compact_start(&op), 0);

	/* The release gives the area nothing. */
	spin_lock(&store->tables);
	data = io_opaque_detach(store, opaque_test_slot(store, op.slot));
	spin_unlock(&store->tables);
	io_opaque_data_put(store, data);
	io_opaque_create_release(&op);
	KUNIT_EXPECT_EQ(test, area->free_count, 16U);
	KUNIT_EXPECT_EQ(test, opaque_slots_in(ifq, IO_ZCRX_KSLOT_FREE), 16U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}
#endif

#if IS_ENABLED(CONFIG_PAGE_POOL)
static void opaque_test_pool_destroy(void *pp)
{
	page_pool_destroy(pp);
}

/* A page pool without a device: its pages come from the page allocator, and go back to its ring. */
static struct page_pool *opaque_sized_pool(struct kunit *test, unsigned int size)
{
	struct page_pool_params params = { .pool_size = size, .nid = NUMA_NO_NODE };
	struct page_pool *pp = page_pool_create(&params);

	KUNIT_ASSERT_FALSE(test, IS_ERR(pp));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_pool_destroy, pp), 0);
	return pp;
}

static struct page_pool *opaque_plain_pool(struct kunit *test)
{
	return opaque_sized_pool(test, 16);
}

static unsigned int opaque_ring_pages(struct page_pool *pp)
{
	unsigned int i, n = 0;

	for (i = 0; i < pp->ring.size; i++)
		n += !!pp->ring.queue[i];
	return n;
}

/* The skb of a driver: a pool page in a fragment, and the pool reference of the page. */
static struct sk_buff *opaque_pp_skb(struct kunit *test, struct page *page,
				     unsigned int offset, unsigned int length)
{
	struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_add_rx_frag(skb, 0, page, offset, length, PAGE_SIZE);
	skb_mark_for_recycle(skb);
	return skb;
}

/* Pages that the pool recycled into its ring; it counts them only with statistics. */
static u64 opaque_ring_recycled(struct page_pool *pp)
{
#ifdef CONFIG_PAGE_POOL_STATS
	struct page_pool_stats stats = { };

	page_pool_get_stats(pp, &stats);
	return stats.recycle_stats.ring;
#else
	return 0;
#endif
}

static void opaque_expect_recycled(struct kunit *test, struct page_pool *pp, u64 expected)
{
	if (IS_ENABLED(CONFIG_PAGE_POOL_STATS))
		KUNIT_EXPECT_EQ(test, opaque_ring_recycled(pp), expected);
}

/*
 * A page pool page in an skb that is marked for recycling is held by a pool
 * reference, the kind that the skb holds. The page goes back to its pool when
 * the last of the skb and the store lets go, in whichever order, and it never
 * leaves the pool: no page reference is taken, so the pool finds the page
 * unreferenced when it gets it back.
 */
static void opaque_capture_pool_page(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct page *page = page_pool_dev_alloc_pages(pp);
	u64 held = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD);
	u64 recycled = opaque_ring_recycled(pp);
	struct sk_buff *skb;
	unsigned int ring;

	KUNIT_ASSERT_NOT_NULL(test, page);
	opaque_test_stream(&stream, store);

	/* The skb goes first. */
	skb = opaque_pp_skb(test, page, 128, 1024);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 1024, false), 1024);
	stream.next = 1024;
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, page);
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PP);
	KUNIT_EXPECT_EQ(test, extent->backing->units, (u16)1);
	KUNIT_EXPECT_FALSE(test, extent->backing->area);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 2L);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 0LL);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held + 1);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 1L);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 0U);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	opaque_expect_recycled(test, pp, recycled + 1);
	/* The page is back; the block that the stream takes the next chunk from is not. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), OPAQUE_TEST_BLOCK);

	/* The store goes first, and the page stays with the skb. */
	page = page_pool_dev_alloc_pages(pp);
	KUNIT_ASSERT_NOT_NULL(test, page);
	ring = opaque_ring_pages(pp);
	stream.next = 0;
	skb = opaque_pp_skb(test, page, 0, 512);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 512, false), 512);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 2L);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 1L);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), ring);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), ring + 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	opaque_expect_recycled(test, pp, recycled + 2);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), OPAQUE_TEST_BLOCK);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * The head of an skb built on a pool page is held the same way: the page that
 * virt_to_page() finds is a pool page, and the skb holds a pool reference to it.
 */
static void opaque_capture_pool_head(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct page *page = page_pool_dev_alloc_pages(pp);
	struct sk_buff *skb;

	KUNIT_ASSERT_NOT_NULL(test, page);
	opaque_test_stream(&stream, store);
	skb = build_skb(page_address(page), PAGE_SIZE);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_reserve(skb, 64);
	memset(skb_put(skb, 600), 0x3c, 600);
	skb_mark_for_recycle(skb);
	KUNIT_EXPECT_TRUE(test, skb->head_frag);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 600, false), 600);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, page);
	KUNIT_EXPECT_EQ(test, extent->offset, 64U);
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PP);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&page->pp_ref_count), 2L);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 0LL);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 0U);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
}

/*
 * Only a page pool page of an skb marked for recycling is held by a pool
 * reference. An skb that is not marked holds page references, and so does a
 * marked one to a page that no pool owns.
 */
static void opaque_capture_pool_other(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	struct page *pooled = page_pool_dev_alloc_pages(pp);
	struct page *plain = alloc_page(GFP_KERNEL);
	struct sk_buff *skb;

	KUNIT_ASSERT_NOT_NULL(test, pooled);
	KUNIT_ASSERT_NOT_NULL(test, plain);
	opaque_test_stream(&stream, store);

	/* A pool page in an skb that holds a page reference to it. */
	get_page(pooled);
	skb = alloc_skb(0, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_add_rx_frag(skb, 0, pooled, 0, 256, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 256, false), 256);
	stream.next = 256;
	extent = list_last_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PAGE);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&pooled->pp_ref_count), 1L);
	KUNIT_EXPECT_EQ(test, page_count(pooled), 3);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, page_count(pooled), 2);

	/* An ordinary page in an skb that is marked for recycling. */
	get_page(plain);
	skb = opaque_pp_skb(test, plain, 0, 256);
	KUNIT_EXPECT_EQ(test, io_opaque_capture(&stream, skb, 0, 256, false), 256);
	extent = list_last_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PAGE);
	KUNIT_EXPECT_EQ(test, page_count(plain), 3);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, page_count(plain), 2);

	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, page_count(pooled), 1);
	KUNIT_EXPECT_EQ(test, page_count(plain), 1);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	/* The driver's reference to the pool page ends in the pool. */
	page_pool_put_unrefed_netmem(pp, page_to_netmem(pooled), -1, true);
	put_page(plain);
}

/* The skb of a driver: @nr pool pages in as many fragments of @size bytes, and their references. */
static struct sk_buff *opaque_pp_frags(struct kunit *test, struct page_pool *pp,
				       struct page **pages, unsigned int nr, unsigned int size)
{
	struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);
	unsigned int i;

	BUILD_BUG_ON(OPAQUE_TEST_FRAGS > MAX_SKB_FRAGS);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_LE(test, nr, (unsigned int)MAX_SKB_FRAGS);
	for (i = 0; i < nr; i++) {
		pages[i] = page_pool_dev_alloc_pages(pp);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
		skb_add_rx_frag(skb, i, pages[i], 0, size, PAGE_SIZE);
	}
	skb_mark_for_recycle(skb);
	return skb;
}

/* Put @child at the end of the frag_list of @top, and count it in the length of @top. */
static void opaque_pp_child(struct sk_buff *top, struct sk_buff **tail, struct sk_buff *child)
{
	if (*tail)
		(*tail)->next = child;
	else
		skb_shinfo(top)->frag_list = child;
	*tail = child;
	top->len += child->len;
	top->data_len += child->len;
	top->truesize += child->truesize;
}

static long opaque_pp_refs(struct page *page)
{
	return atomic_long_read(&page->pp_ref_count);
}

/*
 * A call that consumes an skb to its end passes the references of its fragments
 * to the backings, and the skb has none left to drop. The length of the skb and
 * its accounting stay, TCP uncharges the truesize and frees the skb at once, and
 * the pages go back to their pool when the store lets go.
 */
static void opaque_steal_whole_skb(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct io_opaque_extent *extent;
	struct page *pages[4];
	struct sk_buff *skb = opaque_pp_frags(test, pp, pages, 4, 1000);
	u32 len = skb->len, data_len = skb->data_len, truesize = skb->truesize;
	u64 stolen = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN);
	u64 held = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD);
	unsigned int i = 0;

	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, len), (int)len);
	KUNIT_EXPECT_EQ(test, desc.error, 0);
	KUNIT_EXPECT_EQ(test, stream.next, (u64)len);
	KUNIT_EXPECT_EQ(test, desc.count, (size_t)SZ_1M - len);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (u8)0);
	KUNIT_EXPECT_EQ(test, skb->len, len);
	KUNIT_EXPECT_EQ(test, skb->data_len, data_len);
	KUNIT_EXPECT_EQ(test, skb->truesize, truesize);
	list_for_each_entry(extent, &stream.extents, list) {
		KUNIT_ASSERT_LT(test, i, 4U);
		KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, pages[i]);
		KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PP);
		KUNIT_EXPECT_EQ(test, extent->backing->units, (u16)1);
		/* The reference of the skb is the only one, and nothing was taken. */
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), 1L);
		KUNIT_EXPECT_EQ(test, page_count(pages[i]), 1);
		i++;
	}
	KUNIT_EXPECT_EQ(test, i, 4U);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN), stolen + 4);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held);

	/* TCP frees the skb, and the free releases nothing. */
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 0U);
	for (i = 0; i < 4; i++)
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), 1L);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 4U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * A call that stops inside the skb leaves it as it is, and the backings take
 * references of their own. The call that consumes the rest takes over the
 * references to the fragments from its offset on, and the skb keeps those before.
 */
static void opaque_steal_partial_call(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct page *pages[4];
	struct sk_buff *skb = opaque_pp_frags(test, pp, pages, 4, 1000);
	u32 len = skb->len;
	u64 stolen = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN);
	u64 held = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD);
	unsigned int i;

	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	/* The budget lets one page in: the call stops at the second fragment. */
	store->config.hard_limit = PAGE_SIZE + OPAQUE_TEST_BLOCK;
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, len), 1000);
	KUNIT_EXPECT_EQ(test, desc.error, -ENOBUFS);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (u8)4);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[0]), 2L);
	for (i = 1; i < 4; i++)
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), 1L);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held + 1);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN), stolen);

	/* There is room again. The call begins at the third fragment... */
	store->config.hard_limit = 128 * PAGE_SIZE;
	desc.error = 0;
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 1000, len - 1000), (int)len - 1000);
	/* ...and the skb keeps the first fragment, which it still holds a reference to. */
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (u8)1);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[0]), 2L);
	for (i = 1; i < 4; i++)
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), 1L);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held + 1);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN), stolen + 3);

	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[0]), 1L);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 0U);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 4U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * A fragment in which the call began is the skb's: its earlier bytes belong to
 * a call that came before, and the bytes of this call take a reference of
 * their own. The fragments after it are passed on.
 */
static void opaque_steal_straddle(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_data *data = io_opaque_data_alloc();
	struct io_opaque_req op = {
		.store = store, .stream = &stream, .data = data,
		.op = IORING_OPAQUE_RECV_OBJECT, .offset = 0, .length = 1500,
	};
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct page *pages[3];
	struct sk_buff *skb = opaque_pp_frags(test, pp, pages, 3, 1000);

	KUNIT_ASSERT_NOT_NULL(test, data);
	opaque_test_wakeups(test);
	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	/* The object takes the first 1500 bytes, and the gate holds the rest on the socket. */
	stream.hold_undecided = true;
	KUNIT_ASSERT_EQ(test, io_opaque_range_insert(&op), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, skb->len), 1500);
	KUNIT_EXPECT_EQ(test, opaque_woken.nr, 1U);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (u8)3);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[0]), 2L);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[1]), 2L);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[2]), 1L);

	/* The next call begins in the middle of the second fragment. */
	stream.hold_undecided = false;
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 1500, skb->len - 1500), 1500);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (u8)2);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[0]), 2L);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[1]), 3L);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[2]), 1L);

	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[0]), 1L);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[1]), 2L);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 0U);
	io_opaque_data_put(store, data);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 3U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/* A cloned skb is not the call's to take apart: the bytes are copied, and the skb is as it was. */
static void opaque_steal_cloned_copies(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct io_opaque_extent *extent;
	struct page *pages[4];
	struct sk_buff *skb = opaque_pp_frags(test, pp, pages, 4, 1000), *clone;
	u64 stolen = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN);
	u64 held = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD);
	u64 copied = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_COPIED);
	u32 len = skb->len;
	unsigned int i;

	clone = skb_clone(skb, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, clone);
	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, len), (int)len);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (u8)4);
	for (i = 0; i < 4; i++)
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), 1L);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), (s64)len);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN), stolen);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_COPIED), copied + 1);
	list_for_each_entry(extent, &stream.extents, list)
		KUNIT_EXPECT_TRUE(test, extent->backing->copied);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 0U);
	kfree_skb(clone);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 4U);
	io_opaque_extents_free(store, &stream.extents);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * Fragments that a decision discards, and fragments that nobody captured, are
 * released when the call ends, as the free of the skb would release them. The
 * skb gives up all of them.
 */
static void opaque_steal_mixed_discard(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_req discard = {
		.store = store, .stream = &stream, .op = IORING_OPAQUE_DISCARD,
		.offset = 1000, .length = 2000,
	};
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct io_opaque_extent *extent;
	struct page *pages[4];
	struct sk_buff *skb = opaque_pp_frags(test, pp, pages, 4, 1000);
	u32 len = skb->len;
	unsigned int i;

	opaque_test_wakeups(test);
	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	KUNIT_ASSERT_EQ(test, io_opaque_range_insert(&discard), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, len), (int)len);
	KUNIT_EXPECT_EQ(test, discard.progress, 2000ULL);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (u8)0);
	/* The two discarded pages went back to the pool in the call. */
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 2U);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 2);
	i = 0;
	list_for_each_entry(extent, &stream.extents, list) {
		struct page *page = pages[i ? 3 : 0];

		KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, page);
		KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PP);
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(page), 1L);
		i++;
	}
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 2U);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 4U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * The skbs on the frag_list of an skb are freed with it, and each gives up its own
 * fragments. A child that the call began in keeps the fragments before the
 * offset.
 */
static void opaque_steal_fraglist(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct page *pages[6];
	struct sk_buff *top = opaque_pp_frags(test, pp, pages, 2, 1000), *tail = NULL;
	struct sk_buff *kid[2];
	unsigned int i;

	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	for (i = 0; i < 2; i++) {
		kid[i] = opaque_pp_frags(test, pp, pages + 2 + 2 * i, 2, 1000);
		opaque_pp_child(top, &tail, kid[i]);
	}
	KUNIT_ASSERT_EQ(test, top->len, 6000U);

	/* The first call stops in the first child: everything is held. */
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, top, 0, 3000), 3000);
	KUNIT_EXPECT_EQ(test, skb_shinfo(top)->nr_frags, (u8)2);
	KUNIT_EXPECT_EQ(test, skb_shinfo(kid[0])->nr_frags, (u8)2);
	KUNIT_EXPECT_EQ(test, skb_shinfo(kid[1])->nr_frags, (u8)2);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), 2L);
	for (; i < 6; i++)
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), 1L);

	/* The second begins at the second fragment of that child, and ends the skb. */
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, top, 3000, 3000), 3000);
	KUNIT_EXPECT_EQ(test, skb_shinfo(top)->nr_frags, (u8)2);
	KUNIT_EXPECT_EQ(test, skb_shinfo(kid[0])->nr_frags, (u8)1);
	KUNIT_EXPECT_EQ(test, skb_shinfo(kid[1])->nr_frags, (u8)0);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), 2L);
	for (; i < 6; i++)
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), 1L);
	KUNIT_EXPECT_EQ(test, top->len, 6000U);

	kfree_skb(top);
	for (i = 0; i < 6; i++)
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), 1L);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 0U);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 6U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
}

/* Children from the first byte on give up everything, and the free of the tree drops nothing. */
static void opaque_steal_fraglist_whole(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct page *pages[6];
	struct sk_buff *top = opaque_pp_frags(test, pp, pages, 2, 1000), *tail = NULL;
	struct sk_buff *kid[2];
	unsigned int i;

	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	for (i = 0; i < 2; i++) {
		kid[i] = opaque_pp_frags(test, pp, pages + 2 + 2 * i, 2, 1000);
		opaque_pp_child(top, &tail, kid[i]);
	}
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, top, 0, top->len), 6000);
	KUNIT_EXPECT_EQ(test, skb_shinfo(top)->nr_frags, (u8)0);
	KUNIT_EXPECT_EQ(test, skb_shinfo(kid[0])->nr_frags, (u8)0);
	KUNIT_EXPECT_EQ(test, skb_shinfo(kid[1])->nr_frags, (u8)0);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 6);
	for (i = 0; i < 6; i++)
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), 1L);
	kfree_skb(top);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 0U);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 6U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
}

/*
 * The head of an skb is never the call's to give away: the skb frees it with the
 * skb. The backing takes a pool reference of its own to the head page, and the
 * fragments are passed on.
 */
static void opaque_steal_head_pool_ref(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct io_opaque_extent *extent;
	struct page *head = page_pool_dev_alloc_pages(pp), *frag[2];
	struct sk_buff *skb;
	u64 stolen = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN);
	u64 held = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, head);
	skb = build_skb(page_address(head), PAGE_SIZE);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_reserve(skb, 64);
	memset(skb_put(skb, 600), 0x3c, 600);
	for (i = 0; i < 2; i++) {
		frag[i] = page_pool_dev_alloc_pages(pp);
		KUNIT_ASSERT_NOT_NULL(test, frag[i]);
		skb_add_rx_frag(skb, i, frag[i], 0, 1000, PAGE_SIZE);
	}
	skb_mark_for_recycle(skb);
	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, skb->len), 2600);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (u8)0);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, head);
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PP);
	KUNIT_EXPECT_EQ(test, extent->backing->units, (u16)1);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(head), 2L);
	KUNIT_EXPECT_EQ(test, page_count(head), 1);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(frag[0]), 1L);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(frag[1]), 1L);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held + 1);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN), stolen + 2);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(head), 1L);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 0U);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 3U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
}

/*
 * Two skbs carry fragments of one page. One backing holds the page for both:
 * the bytes of the second join it, the references of both skbs are its units, and
 * the release gives them back at once.
 */
static void opaque_steal_same_page_two_skbs(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct io_opaque_extent *extent;
	struct page *page = page_pool_dev_alloc_pages(pp);
	struct sk_buff *a = alloc_skb(0, GFP_KERNEL), *b = alloc_skb(0, GFP_KERNEL);
	u64 stolen = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN);
	u64 held = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD);

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	/* Each skb holds a pool reference to the page. */
	page_pool_ref_netmem(page_to_netmem(page));
	skb_add_rx_frag(a, 0, page, 0, 1000, PAGE_SIZE);
	skb_add_rx_frag(b, 0, page, 2000, 1000, PAGE_SIZE);
	skb_mark_for_recycle(a);
	skb_mark_for_recycle(b);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(page), 2L);
	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, a, 0, 1000), 1000);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, b, 0, 1000), 1000);
	KUNIT_EXPECT_EQ(test, skb_shinfo(a)->nr_frags, (u8)0);
	KUNIT_EXPECT_EQ(test, skb_shinfo(b)->nr_frags, (u8)0);
	/* The counters count the page once, though two references passed. */
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN), stolen + 1);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held);
	/* One backing, charged once, with two units; two extents name it. */
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 2);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)PAGE_SIZE + OPAQUE_TEST_BLOCK + OPAQUE_TEST_EXTENT);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, list_last_entry(&stream.extents, struct io_opaque_extent,
						  list)->backing, extent->backing);
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PP);
	KUNIT_EXPECT_EQ(test, extent->backing->units, (u16)2);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(page), 2L);
	kfree_skb(a);
	kfree_skb(b);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(page), 2L);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 0U);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 1U);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(page), 1L);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * Units of one kind only add up. A fragment of the same page whose skb holds a
 * page reference, not a pool reference, makes a backing of its own, which is
 * charged again.
 */
static void opaque_steal_kind_mismatch(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct io_opaque_extent *first, *second, *next;
	struct page *page = page_pool_dev_alloc_pages(pp);
	struct sk_buff *a = alloc_skb(0, GFP_KERNEL), *b = alloc_skb(0, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	/* A holds the pool reference of the driver, and B a page reference. */
	skb_add_rx_frag(a, 0, page, 0, 1000, PAGE_SIZE);
	skb_mark_for_recycle(a);
	get_page(page);
	skb_add_rx_frag(b, 0, page, 2000, 1000, PAGE_SIZE);
	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, a, 0, 1000), 1000);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, b, 0, 1000), 1000);
	KUNIT_EXPECT_EQ(test, skb_shinfo(a)->nr_frags, (u8)0);
	KUNIT_EXPECT_EQ(test, skb_shinfo(b)->nr_frags, (u8)0);
	first = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	second = list_last_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_ASSERT_PTR_NE(test, first, second);
	KUNIT_EXPECT_PTR_NE(test, first->backing, second->backing);
	KUNIT_EXPECT_EQ(test, first->backing->ref, (u8)IO_OPAQUE_REF_PP);
	KUNIT_EXPECT_EQ(test, second->backing->ref, (u8)IO_OPAQUE_REF_PAGE);
	KUNIT_EXPECT_EQ(test, first->backing->units, (u16)1);
	KUNIT_EXPECT_EQ(test, second->backing->units, (u16)1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)2 * PAGE_SIZE + OPAQUE_TEST_BLOCK);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(page), 1L);
	KUNIT_EXPECT_EQ(test, page_count(page), 2);
	kfree_skb(a);
	kfree_skb(b);
	/* The page reference goes first, so that the pool finds the page unreferenced. */
	list_for_each_entry_safe_reverse(second, next, &stream.extents, list)
		io_opaque_extent_free(store, second);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * A fragment that two backings name gives its reference to the first only. The
 * other takes one of its own, and the references add up when both are released.
 */
static void opaque_steal_split_fragment(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_data *data = io_opaque_data_alloc();
	struct io_opaque_req op = {
		.store = store, .stream = &stream, .data = data,
		.op = IORING_OPAQUE_RECV_OBJECT, .offset = 1000, .length = 1000,
	};
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct io_opaque_extent *extent;
	struct page *pages[1];
	struct sk_buff *skb = opaque_pp_frags(test, pp, pages, 1, 3000);
	u64 stolen = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN);
	u64 held = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD);

	KUNIT_ASSERT_NOT_NULL(test, data);
	opaque_test_wakeups(test);
	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	KUNIT_ASSERT_EQ(test, io_opaque_range_insert(&op), 0);
	/* The middle third goes to an object: a backing of its own on the same page. */
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, 3000), 3000);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (u8)0);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 3);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)2 * PAGE_SIZE + OPAQUE_TEST_BLOCK + OPAQUE_TEST_EXTENT);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_EQ(test, extent->backing->units, (u16)1);
	extent = list_first_entry(&data->extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_EQ(test, extent->backing->units, (u16)1);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[0]), 2L);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN), stolen + 1);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held + 1);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[0]), 2L);
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[0]), 1L);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 0U);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * A pass with batch left finishes its skb, and one without consumes nothing; the
 * batch is spent, never negative.
 */
static void opaque_steal_soft_batch(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = 100, .arg.data = &stream };
	struct page *pages[4];
	struct sk_buff *skb = opaque_pp_frags(test, pp, pages, 4, 1000);
	struct sk_buff *next = opaque_pp_frags(test, pp, pages, 4, 1000);

	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, skb->len), 4000);
	KUNIT_EXPECT_EQ(test, desc.count, (size_t)0);
	KUNIT_EXPECT_EQ(test, stream.next, 4000ULL);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (u8)0);
	/* TCP ends the pass: the batch is gone. A call that is made anyway takes nothing. */
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, next, 0, next->len), 0);
	KUNIT_EXPECT_EQ(test, stream.next, 4000ULL);
	KUNIT_EXPECT_EQ(test, skb_shinfo(next)->nr_frags, (u8)4);
	kfree_skb(skb);
	kfree_skb(next);
	io_opaque_extents_free(store, &stream.extents);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * The head of an skb and a fragment of it on one pool page. The head is held by a
 * reference of the backing's own, the reference of the fragment passes on: two
 * units, one page in the counters, and the page goes back to the pool once, after
 * the skb and the store.
 */
static void opaque_steal_head_frag_same_page(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct io_opaque_extent *extent;
	struct page *page = page_pool_dev_alloc_pages(pp);
	struct sk_buff *skb;
	u64 stolen = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN);
	u64 held = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD);
	unsigned int n = 0;

	KUNIT_ASSERT_NOT_NULL(test, page);
	skb = build_skb(page_address(page), PAGE_SIZE);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_reserve(skb, 64);
	memset(skb_put(skb, 600), 0x3c, 600);
	/* The fragment has a pool reference of its own to the same page. */
	page_pool_ref_netmem(page_to_netmem(page));
	skb_add_rx_frag(skb, 0, page, 2048, 1000, 1024);
	skb_mark_for_recycle(skb);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(page), 2L);
	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, skb->len), 1600);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (u8)0);
	list_for_each_entry(extent, &stream.extents, list) {
		KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, page);
		n++;
	}
	KUNIT_EXPECT_EQ(test, n, 2U);
	extent = list_first_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PP);
	KUNIT_EXPECT_EQ(test, extent->backing->units, (u16)2);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)PAGE_SIZE + OPAQUE_TEST_BLOCK + OPAQUE_TEST_EXTENT);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held + 1);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN), stolen);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(page), 3L);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(page), 2L);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 0U);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 1U);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(page), 1L);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
}

/*
 * One backing on both sides of the boundary at which the oldest entries settle.
 * Two contiguous fragments of one page are the 32nd and 33rd entries: the first
 * settles early with a reference of its own, the second passes the reference of
 * the skb on. Two units, released once, and one page in the counters.
 */
static void opaque_steal_overflow_same_backing(struct kunit *test)
{
	const unsigned int per = OPAQUE_TEST_FRAGS, kids = 3, total = (kids + 1) * per;
	struct page_pool *pp = opaque_sized_pool(test, 128);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct page *pages[(3 + 1) * OPAQUE_TEST_FRAGS];
	struct sk_buff *top, *kid[3], *tail = NULL;
	struct page *shared;
	skb_frag_t *frag;
	u64 stolen = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN);
	u64 held = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD);
	unsigned int i, ring;

	top = opaque_pp_frags(test, pp, pages, per, 100);
	for (i = 0; i < kids; i++) {
		kid[i] = opaque_pp_frags(test, pp, pages + (i + 1) * per, per, 100);
		opaque_pp_child(top, &tail, kid[i]);
	}
	/*
	 * Fragments 14 and 15 of the first child are the 32nd and 33rd entries.
	 * Put 15 on the page of 14, behind it. The page that 15 had goes back to the
	 * pool now.
	 */
	shared = skb_frag_page(&skb_shinfo(kid[0])->frags[14]);
	frag = &skb_shinfo(kid[0])->frags[15];
	page_pool_ref_netmem(page_to_netmem(shared));
	page_pool_put_full_page(pp, skb_frag_page(frag), false);
	skb_frag_fill_page_desc(frag, shared, 100, 100);
	ring = opaque_ring_pages(pp);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(shared), 2L);
	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, top, 0, top->len), (int)top->len);
	/* The fragment of the skb, the backing's own, and the one passed on. */
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(shared), 3L);
	KUNIT_EXPECT_EQ(test, skb_shinfo(kid[0])->nr_frags, (u8)15);
	/* One backing less than the fragments; 32 of them settled early. */
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held + 32);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN),
			stolen + total - 32 - 1);
	/* A page for each backing but one, and a block for every IO_OPAQUE_BLOCK_CHUNKS of them. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)(total - 1) * PAGE_SIZE +
			DIV_ROUND_UP(total - 1, IO_OPAQUE_BLOCK_CHUNKS) * OPAQUE_TEST_BLOCK);
	kfree_skb(top);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(shared), 2L);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_pp_refs(shared), 1L);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), ring + total - 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * The task work that completes a range is the ring's of its request, and the
 * store can be shared with another ring. It runs at once, on another CPU, and
 * does not take the lock of the stream: it publishes the object, and the
 * application frees it. This does that. Every backing of the range must hold
 * its references by then, and the free must give them back to the pool.
 */
static void opaque_remote_free(struct io_opaque_req *op, int result)
{
	struct kunit *test = kunit_get_current_test();
	struct io_opaque_extent *extent;

	if (op->op != IORING_OPAQUE_RECV_OBJECT || !op->data)
		return;
	list_for_each_entry(extent, &op->data->extents, list)
		KUNIT_EXPECT_NE(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PENDING);
	io_opaque_data_put(op->store, op->data);
	op->data = NULL;
}

static void opaque_remote_completion_run(struct kunit *test, unsigned int pages)
{
	struct page_pool *pp = opaque_sized_pool(test, 128);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_data *data = io_opaque_data_alloc();
	struct io_opaque_req op = {
		.store = store, .stream = &stream, .data = data,
		.op = IORING_OPAQUE_RECV_OBJECT, .offset = 0, .length = pages * 100,
	};
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct page *all[IO_OPAQUE_BLOCK_CHUNKS];
	struct sk_buff *top = NULL, *tail = NULL;
	unsigned int made = 0;

	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_LE(test, pages, (unsigned int)ARRAY_SIZE(all));
	/* One skb, and children on its frag_list for what it cannot hold. */
	while (made < pages) {
		unsigned int n = min_t(unsigned int, pages - made, OPAQUE_TEST_FRAGS);
		struct sk_buff *skb = opaque_pp_frags(test, pp, all + made, n, 100);

		if (top)
			opaque_pp_child(top, &tail, skb);
		else
			top = skb;
		made += n;
	}
	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	kunit_activate_static_stub(test, io_opaque_queue_ready, opaque_remote_free);
	KUNIT_ASSERT_EQ(test, io_opaque_range_insert(&op), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, top, 0, top->len), (int)top->len);
	/* The object was published and freed, and the call settled what it had left. */
	KUNIT_EXPECT_NULL(test, op.data);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	/* The block went with its last chunk if the call used it up, else the stream holds it. */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			pages < IO_OPAQUE_BLOCK_CHUNKS ? OPAQUE_TEST_BLOCK : 0);
	kfree_skb(top);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), pages);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/* A few fragments, and as many as fill and retire a block of chunks. */
static void opaque_steal_remote_completion(struct kunit *test)
{
	opaque_remote_completion_run(test, 4);
	opaque_remote_completion_run(test, IO_OPAQUE_BLOCK_CHUNKS);
}

/*
 * A stream without the room for a call, which only a test makes, takes its
 * references at once and leaves the skb as it is.
 */
static void opaque_steal_no_room(struct kunit *test)
{
	struct page_pool *pp = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	struct page *pages[4];
	struct sk_buff *skb = opaque_pp_frags(test, pp, pages, 4, 1000);
	u64 stolen = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN);
	u64 held = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD);
	unsigned int i;

	opaque_test_stream(&stream, store);
	KUNIT_EXPECT_NULL(test, stream.pending);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, skb->len), 4000);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, (u8)4);
	for (i = 0; i < 4; i++)
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), 2L);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held + 4);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN), stolen);
	kfree_skb(skb);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 4U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
}

/*
 * More fragments than the stream remembers settle in order: the oldest take
 * references of their own, and the skb keeps their fragments. The rest are
 * passed on. Every page has the references it needs at the end.
 */
static void opaque_steal_overflow(struct kunit *test)
{
	struct page_pool *pp = opaque_sized_pool(test, 128);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	read_descriptor_t desc = { .count = SZ_1M, .arg.data = &stream };
	const unsigned int per = OPAQUE_TEST_FRAGS, kids = 5, total = 5 * OPAQUE_TEST_FRAGS;
	struct page *pages[5 * OPAQUE_TEST_FRAGS];
	struct sk_buff *top = alloc_skb(0, GFP_KERNEL), *tail = NULL, *kid[5];
	u64 stolen = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN);
	u64 held = io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD);
	unsigned int i, early = IO_OPAQUE_PENDING_MAX / 2;

	KUNIT_ASSERT_NOT_NULL(test, top);
	KUNIT_ASSERT_GT(test, total, (unsigned int)IO_OPAQUE_PENDING_MAX);
	opaque_test_stream(&stream, store);
	opaque_test_scratch(test, &stream);
	for (i = 0; i < kids; i++) {
		kid[i] = opaque_pp_frags(test, pp, pages + i * per, per, 100);
		opaque_pp_child(top, &tail, kid[i]);
	}
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, top, 0, top->len), (int)top->len);
	/* The first IO_OPAQUE_PENDING_MAX / 2 fragments settled when the 65th came. */
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_HELD), held + early);
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_PAGES_STOLEN),
			stolen + total - early);
	for (i = 0; i < total; i++)
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), i < early ? 2L : 1L);
	/* The skb keeps what the oldest settled, up to the first fragment that was passed on. */
	for (i = 0; i < kids; i++) {
		unsigned int kept = early > i * per ? min(early - i * per, per) : 0;

		KUNIT_EXPECT_EQ(test, skb_shinfo(kid[i])->nr_frags, (u8)kept);
	}
	kfree_skb(top);
	for (i = 0; i < total; i++)
		KUNIT_EXPECT_EQ(test, opaque_pp_refs(pages[i]), 1L);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), 0U);
	io_opaque_extents_free(store, &stream.extents);
	KUNIT_EXPECT_EQ(test, opaque_ring_pages(pp), total);
	KUNIT_EXPECT_EQ(test, atomic_read(&pp->pages_state_release_cnt), 0);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}
#endif

/* The memory cgroup of a store pays for every page it holds except those of the area. */
static void opaque_hold_flags(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, io_opaque_hold_flags(IO_OPAQUE_HOLD_PAGE), 0U);
	KUNIT_EXPECT_EQ(test, io_opaque_hold_flags(IO_OPAQUE_HOLD_PP), 0U);
	KUNIT_EXPECT_EQ(test, io_opaque_hold_flags(IO_OPAQUE_HOLD_AREA),
			(unsigned int)IO_OPAQUE_EXT_NO_MEMCG);
	KUNIT_EXPECT_EQ(test, io_opaque_hold_flags(IO_OPAQUE_HOLD_POOL),
			(unsigned int)IO_OPAQUE_EXT_NO_MEMCG);
	KUNIT_EXPECT_EQ(test, io_opaque_hold_flags(IO_OPAQUE_HOLD_COPY),
			(unsigned int)IO_OPAQUE_EXT_RESERVED);
}

/*
 * Managed fragments. A body that goes out of the owned path is filled in as
 * managed fragments, and each skb that gets some has a ubuf of its own that
 * holds one reference on every distinct folio it uses. The tests look at the
 * fill on a skb, at the counters and the pages on the wire fixture, and at what
 * a consumer on the receive path does with the skb.
 */

/* The counters of managed sends, as a test sees them move. */
struct opaque_managed {
	u64 skbs, refs, fallback, copied, body;
};

static void opaque_managed_read(struct opaque_managed *m)
{
	m->skbs = io_opaque_stat_read(IO_OPAQUE_STAT_SEND_MANAGED_SKBS);
	m->refs = io_opaque_stat_read(IO_OPAQUE_STAT_SEND_FOLIO_REFS);
	m->fallback = io_opaque_stat_read(IO_OPAQUE_STAT_SEND_FRAG_FALLBACK);
	m->copied = io_opaque_stat_read(IO_OPAQUE_STAT_SEND_UBUF_COPIED);
	m->body = io_opaque_stat_read(IO_OPAQUE_STAT_BODY_COPIED);
}

/*
 * Wait for the peer to have acknowledged everything that @sk sent. That is snd_una, which is not
 * the same as the skbs of the data being gone (opaque_wire_settle()).
 */
static void opaque_wire_acked(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int i;

	for (i = 0; i < 100 && READ_ONCE(tp->snd_una) != READ_ONCE(tp->write_seq); i++)
		msleep(20);
}

/* The pages of @data have only the references of their extents, and the ubufs are as they were. */
static bool opaque_wire_back(const struct io_opaque_data *data, long live)
{
	const struct io_opaque_extent *extent;

	list_for_each_entry(extent, &data->extents, list)
		if (page_count(extent->backing->page) != 1)
			return false;
	return atomic_long_read(&io_opaque_ubufs_live) == live;
}

#define OPAQUE_WIRE_SETTLE_MS	5000

/*
 * What the skbs of the data held is not back when opaque_wire_acked() returns, and a case that
 * counts it has to wait for the counts that it expects. tcp_ack() moves snd_una and only then calls
 * tcp_clean_rtx_queue(), which frees the skbs that the ACK acknowledged, in the softirq that read
 * it on whatever CPU that is: a case that waits for snd_una on another CPU can look in between.
 * An skb without a ubuf that the peer has read can be queued for another CPU to free
 * (opaque_rx_kick()); the probe of the path MTU, which TCP makes of the fragments of the queue, is
 * one. Their page references and ubufs are put after that.
 *
 * This waits for the pages of @data to have only the references of their extents, and for the
 * ubufs made since @live was read to be gone, for five seconds at most, and raises NET_RX on every
 * CPU between its looks. It expects nothing itself: the case makes its expectations after it, as
 * it would have, so a reference that is never put fails the case, with the count that is left,
 * when the time is up.
 */
static void opaque_wire_settle(const struct io_opaque_data *data, long live)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(OPAQUE_WIRE_SETTLE_MS);

	while (!opaque_wire_back(data, live) && time_before(jiffies, timeout)) {
		on_each_cpu(opaque_rx_kick, NULL, 1);
		usleep_range(500, 2000);
	}
}

static u8 opaque_test_ubuf_no_waiver(void)
{
	return SKBFL_ZEROCOPY_ENABLE | SKBFL_DONT_ORPHAN;
}

static bool opaque_test_not_direct(const struct sock *sk)
{
	return false;
}

/* One body-only send of @len bytes of the object from @off, as request 0 of the fixture. */
static void opaque_managed_send(struct kunit *test, struct opaque_wire *w,
				struct io_opaque_store *store, u64 off, u32 len, u32 msg_flags)
{
	struct io_opaque_req *op = opaque_wire_req(test, w, store, 0, 0, off, len);

	op->msg_flags = msg_flags;
	memset(&w->req[0].cqe, 0, sizeof(w->req[0].cqe));
	KUNIT_ASSERT_EQ(test, opaque_wire_issue(w, 0, true), IOU_COMPLETE);
	KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, (s32)len);
	/* The ordinary path keeps its references until the request is cleaned up. */
	io_opaque_data_put(store, op->data);
	op->data = NULL;
}

/*
 * The sends of the wire fixture go out of the owned path as managed fragments.
 * Each skb has a ubuf, which takes a reference on a folio once however many
 * fragments use it, and the peer, whose receive path would have copied a
 * zerocopy skb, is not made to copy: the ubuf says it need not be. The
 * sparse object has an extent of 2000 bytes in each page, so a range takes
 * one reference per extent it spans.
 */
static void opaque_send_managed_loopback(struct kunit *test)
{
	static const struct {
		struct opaque_send_case c;
		u64 folios;	/* extents the ranges span */
	} cases[] = {
		{ { .unlocked = true }, 21 },	/* 4196 + 40000: extents 2 to 22 */
		{ { }, 21 },
		{ { .last = true }, 21 },
		{ { .vector = true, .unlocked = true }, 4 },	/* extents 2, 3, 35 and 0 */
		{ { .vector = true, .last = true }, 4 },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_test_params_save(test);
	io_opaque_send_managed = true;
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct opaque_managed before, after;

		opaque_managed_read(&before);
		opaque_wire_send(test, w, store, &cases[i].c);
		opaque_managed_read(&after);
		KUNIT_EXPECT_GE(test, after.skbs - before.skbs, 1ULL);
		/* A folio once for each extent, and again where an skb ends in one. */
		KUNIT_EXPECT_GE(test, after.refs - before.refs, cases[i].folios);
		KUNIT_EXPECT_LE(test, after.refs - before.refs,
				cases[i].folios + (after.skbs - before.skbs) - 1);
		KUNIT_EXPECT_EQ(test, after.fallback, before.fallback);
		/* Nobody copied the data on its way in, at the peer, or in TCP. */
		KUNIT_EXPECT_EQ(test, after.copied, before.copied);
		KUNIT_EXPECT_EQ(test, after.body, before.body);
		if (!i) {
			/* A fresh connection: skbs of as many fragments as may be, a ubuf each. */
			unsigned int limit = READ_ONCE(net_hotdata.sysctl_max_skb_frags);

			KUNIT_EXPECT_GE(test, after.skbs - before.skbs,
					(u64)DIV_ROUND_UP(21, limit));
		}
	}
}

/*
 * A unit that a send's ubuf holds is not taken from the cache, and not
 * rewritten, for as long as the peer's queue holds the skbs: the object is gone
 * with the LAST send and the sender has everything acknowledged, but the
 * ubuf is released only with the last skb of the data. A unit is taken again
 * only when the peer has read.
 */
static void opaque_send_managed_holds_unit(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req compact = {
		.store = store, .data = data, .handle = io_opaque_handle(0, 9),
	};
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 40000, GFP_KERNEL);
	struct page *units[3], *taken[2];
	u64 hits;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_params_save(test);
	io_opaque_send_managed = true;
	io_opaque_send_copy_max = 0;
	opaque_test_cache(test, store, 4);
	opaque_test_copy(data, source);
	/* Publish the compacted copy: two whole units and a tail, and the send spans the units. */
	opaque_test_publish(store, 0, 9, data);
	refcount_inc(&data->refs); /* compaction worker */
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	KUNIT_ASSERT_EQ(test, opaque_test_pages(compact.replacement, units, 3), 3U);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	io_opaque_data_put(store, compact.data);
	io_opaque_compact_end(&compact);
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	opaque_wire_req(test, w, store, 0, IORING_OPAQUE_SEND_LAST, 0, 40000);
	hits = io_opaque_stat_read(IO_OPAQUE_STAT_CACHE_COOLING_HITS);

	KUNIT_ASSERT_EQ(test, opaque_wire_issue(w, 0, true), IOU_COMPLETE);
	KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 40000);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
	/* The units left with the object, and the ubuf's references keep them cooling. */
	KUNIT_EXPECT_EQ(test, store->cache.nr_cooling, 2U);
	KUNIT_EXPECT_EQ(test, store->cache.nr_idle, 0U);
	opaque_wire_acked(w->tx->sk);
	KUNIT_EXPECT_EQ(test, READ_ONCE(tcp_sk(w->tx->sk)->snd_una),
			READ_ONCE(tcp_sk(w->tx->sk)->write_seq));
	/* The peer's queue holds the skbs, and with them the ubuf and the units. */
	KUNIT_EXPECT_GT(test, page_count(units[0]), 1);
	KUNIT_EXPECT_GT(test, page_count(units[1]), 1);
	KUNIT_EXPECT_PTR_EQ(test, io_opaque_unit_take(store), NULL);
	KUNIT_EXPECT_EQ(test, store->cache.nr_cooling, 2U);

	/* The peer reads, the last skb of the data is freed, and the ubuf lets go. */
	opaque_wire_read(test, w, got, 0, 40000);
	KUNIT_EXPECT_MEMEQ(test, got, source, 40000);
	KUNIT_EXPECT_TRUE(test, opaque_wire_unit_free(units[0]));
	KUNIT_EXPECT_TRUE(test, opaque_wire_unit_free(units[1]));
	taken[0] = io_opaque_unit_take(store);
	taken[1] = io_opaque_unit_take(store);
	KUNIT_EXPECT_TRUE(test,
			  (taken[0] == units[0] && taken[1] == units[1]) ||
			  (taken[0] == units[1] && taken[1] == units[0]));
	KUNIT_EXPECT_EQ(test, io_opaque_stat_read(IO_OPAQUE_STAT_CACHE_COOLING_HITS) - hits, 2ULL);
	if (taken[0])
		io_opaque_unit_put(store, taken[0]);
	if (taken[1])
		io_opaque_unit_put(store, taken[1]);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/*
 * Without the waiver the receive path copies the skb on loopback, as it would a
 * MSG_ZEROCOPY skb: every skb that went in comes back through
 * skb_copy_ubufs(), the counter of copies sees it, and the bytes are still
 * right. With it, the same send copies nothing, so the counter is shown to
 * count and not to be stuck.
 */
static void opaque_send_managed_copied_without_waiver(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct opaque_managed before, after;

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_test_params_save(test);
	io_opaque_send_managed = true;
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);

	kunit_activate_static_stub(test, io_opaque_ubuf_flags, opaque_test_ubuf_no_waiver);
	opaque_managed_read(&before);
	opaque_wire_send(test, w, store, &(struct opaque_send_case) { .unlocked = true });
	opaque_managed_read(&after);
	KUNIT_EXPECT_GE(test, after.skbs - before.skbs, 1ULL);
	KUNIT_EXPECT_GE(test, after.copied - before.copied, after.skbs - before.skbs);

	kunit_deactivate_static_stub(test, io_opaque_ubuf_flags);
	opaque_managed_read(&before);
	opaque_wire_send(test, w, store, &(struct opaque_send_case) { .unlocked = true });
	opaque_managed_read(&after);
	KUNIT_EXPECT_GE(test, after.skbs - before.skbs, 1ULL);
	KUNIT_EXPECT_EQ(test, after.copied, before.copied);
}

/*
 * A framed send copies its prefix into the skb first, and that skb cannot be
 * managed: the body takes a page reference for each fragment, and the fill is
 * counted. The skbs after it are managed, and the suffix, which TCP copies
 * into the last of them, makes that one hold references of its own.
 */
static void opaque_send_managed_prefix_fallback(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct opaque_managed before, after;
	unsigned int limit;

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_test_params_save(test);
	io_opaque_send_managed = true;
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	/*
	 * The body is 21 extents of 2000 bytes, and TCP ends an skb at its size goal as well as at
	 * the fragment limit: some 32 KB on this connection, which 45 fragments of these pass. The
	 * limit is to end the first skb here, so it is set to what the object can fill.
	 */
	opaque_max_frags_set(test, OPAQUE_TEST_FRAGS);
	limit = READ_ONCE(net_hotdata.sysctl_max_skb_frags);

	opaque_managed_read(&before);
	opaque_wire_send(test, w, store, &(struct opaque_send_case) {
			 .prefix = 24, .suffix = 8, .unlocked = true });
	opaque_managed_read(&after);
	/* Once: the skb the prefix started is full, and the next fill is not let into it. */
	KUNIT_EXPECT_EQ(test, after.fallback - before.fallback, 1ULL);
	/* The prefix and limit - 1 extents fill the first skb; 21 - (limit - 1) are managed. */
	KUNIT_EXPECT_EQ(test, after.refs - before.refs, (u64)(21 - (limit - 1)));
	KUNIT_EXPECT_EQ(test, after.skbs - before.skbs, 2ULL);
	KUNIT_EXPECT_EQ(test, after.copied, before.copied);
}

/*
 * A managed send, a send that TCP copies, a send that is spliced by the
 * ordinary path, and a managed one again, corked so that they are one
 * stream and share the skb that is at the tail of the queue: each appends to
 * what the one before left, the skb is downgraded by the first copy, and the
 * bytes of all four are the peer's, in order. The ubuf was made once, and what
 * the last send found was not managed.
 */
static void opaque_send_managed_mixed(struct kunit *test)
{
	static const struct { u64 off; u32 len; } r[4] = {
		{ 0, 6000 }, { 10000, 3000 }, { 20000, 6000 }, { 30000, 6000 },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 21000, GFP_KERNEL);
	struct opaque_page_refs *pages;
	struct opaque_managed before, after;
	u32 done = 0, i;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_params_save(test);
	io_opaque_send_managed = true;
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	pages = opaque_page_refs_take(test, data);

	opaque_managed_read(&before);
	io_opaque_send_copy_max = 0;
	opaque_managed_send(test, w, store, r[0].off, r[0].len, MSG_MORE);
	io_opaque_send_copy_max = SZ_1M;
	opaque_managed_send(test, w, store, r[1].off, r[1].len, MSG_MORE);
	io_opaque_send_copy_max = 0;
	kunit_activate_static_stub(test, io_opaque_send_direct, opaque_test_not_direct);
	opaque_managed_send(test, w, store, r[2].off, r[2].len, MSG_MORE);
	kunit_deactivate_static_stub(test, io_opaque_send_direct);
	opaque_managed_send(test, w, store, r[3].off, r[3].len, 0);
	opaque_managed_read(&after);

	KUNIT_EXPECT_EQ(test, after.skbs - before.skbs, 1ULL);
	KUNIT_EXPECT_EQ(test, after.fallback - before.fallback, 1ULL);
	KUNIT_EXPECT_EQ(test, after.body - before.body, (u64)r[1].len);
	KUNIT_EXPECT_EQ(test, after.copied, before.copied);
	for (i = 0; i < ARRAY_SIZE(r); i++) {
		opaque_wire_read(test, w, got, 0, r[i].len);
		KUNIT_EXPECT_MEMEQ(test, got, source + r[i].off, r[i].len);
		done += r[i].len;
	}
	KUNIT_EXPECT_EQ(test, done, 21000U);
	KUNIT_EXPECT_TRUE(test, opaque_pages_settle(pages, true));
	opaque_test_free(store, 0);
	KUNIT_EXPECT_TRUE(test, opaque_pages_settle(pages, false));
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/*
 * Frames larger than the room the socket has: the head queues what fits and
 * waits with its cursor, and no ubuf reference outlives the send. The pages of
 * the object are back to the object's reference alone once the peer has read
 * (opaque_wire_eagain() looks at them).
 */
static void opaque_send_managed_eagain(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct opaque_managed before, after;
	unsigned int action, vector;

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_test_params_save(test);
	io_opaque_send_managed = true;
	opaque_wire_init(test, w);
	opaque_managed_read(&before);
	opaque_wire_eagain(test, w, store, false, false, OPAQUE_EAGAIN_KEEP);
	opaque_wire_eagain(test, w, store, true, true, OPAQUE_EAGAIN_KEEP);
	for (action = OPAQUE_EAGAIN_FREE; action <= OPAQUE_EAGAIN_COMPACT; action++)
		for (vector = 0; vector < 2; vector++)
			opaque_wire_eagain(test, w, store, vector, false, action);
	opaque_managed_read(&after);
	KUNIT_EXPECT_GT(test, after.skbs, before.skbs);
	KUNIT_EXPECT_GT(test, after.refs, before.refs);
	KUNIT_EXPECT_EQ(test, after.copied, before.copied);
}

/*
 * A vector of eight ranges in eight folios takes eight references and puts
 * eight, and eight ranges of one folio take one, however many fragments they
 * make: the references are the ubuf's, one for a folio.
 */
static void opaque_send_managed_vec_distinct(struct kunit *test)
{
	const u64 handle = io_opaque_handle(0, 9);
	struct io_uring_opaque_send_entry ent[8];
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 8000, GFP_KERNEL);
	struct opaque_page_refs *pages;
	struct opaque_managed before, after;
	unsigned int pass, i;
	struct io_opaque_req *op;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_params_save(test);
	io_opaque_send_managed = true;
	io_opaque_send_copy_max = 0;
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	pages = opaque_page_refs_take(test, data);

	for (pass = 0; pass < 2; pass++) {
		/* Eight extents, a range from each; then eight times the range of one. */
		for (i = 0; i < ARRAY_SIZE(ent); i++)
			ent[i] = (struct io_uring_opaque_send_entry) {
				.handle = handle, .off = pass ? 100 : i * 2000 + 100, .len = 1000,
			};
		op = opaque_wire_vec(test, w, store, 0, ent, ARRAY_SIZE(ent), 0);
		memset(&w->req[0].cqe, 0, sizeof(w->req[0].cqe));
		opaque_managed_read(&before);
		KUNIT_ASSERT_EQ(test, opaque_wire_issue(w, 0, true), IOU_COMPLETE);
		opaque_managed_read(&after);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 8000);
		KUNIT_EXPECT_EQ(test, op->vec->pinned, 0U);
		/* One ubuf for the skb, and a reference for each folio in it. */
		KUNIT_EXPECT_EQ(test, after.skbs - before.skbs, 1ULL);
		KUNIT_EXPECT_EQ(test, after.refs - before.refs, pass ? 1ULL : 8ULL);
		KUNIT_EXPECT_EQ(test, after.fallback, before.fallback);
		opaque_wire_read(test, w, got, 0, 8000);
		for (i = 0; i < ARRAY_SIZE(ent); i++)
			KUNIT_EXPECT_MEMEQ(test, got + i * 1000, source + ent[i].off, 1000);
		opaque_wire_acked(w->tx->sk);
		/* All put: only the test's reference and the object's are left. */
		KUNIT_EXPECT_TRUE(test, opaque_pages_settle(pages, true));
	}
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

/*
 * Anything that is not the owned path splices, and a ubuf is not made: the
 * ordinary path, with the sock_sendmsg() a ULP would take, and the evaluation
 * parameter that turns managed fragments off. Both send the same bytes.
 */
static void opaque_send_managed_ordinary_path(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 12000, GFP_KERNEL);
	struct opaque_page_refs *pages;
	struct opaque_managed before, after;
	unsigned int mode;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_params_save(test);
	io_opaque_send_copy_max = 0;
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	pages = opaque_page_refs_take(test, data);

	for (mode = 0; mode < 3; mode++) {
		/* 0: not the owned path; 1: the owned path with the parameter off; 2: on. */
		io_opaque_send_managed = mode == 2;
		if (mode == 0)
			kunit_activate_static_stub(test, io_opaque_send_direct,
						   opaque_test_not_direct);
		opaque_managed_read(&before);
		opaque_managed_send(test, w, store, 4196, 12000, 0);
		opaque_managed_read(&after);
		if (mode == 0)
			kunit_deactivate_static_stub(test, io_opaque_send_direct);
		opaque_wire_read(test, w, got, 0, 12000);
		KUNIT_EXPECT_MEMEQ(test, got, source + 4196, 12000);
		if (mode == 2) {
			KUNIT_EXPECT_GE(test, after.skbs - before.skbs, 1ULL);
			KUNIT_EXPECT_GE(test, after.refs - before.refs, 1ULL);
		} else {
			KUNIT_EXPECT_EQ(test, after.skbs, before.skbs);
			KUNIT_EXPECT_EQ(test, after.refs, before.refs);
		}
		KUNIT_EXPECT_EQ(test, after.fallback, before.fallback);
		KUNIT_EXPECT_EQ(test, after.copied, before.copied);
		KUNIT_EXPECT_EQ(test, after.body, before.body);
		opaque_wire_acked(w->tx->sk);
		KUNIT_EXPECT_TRUE(test, opaque_pages_settle(pages, true));
	}
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

/* An skb with managed fragments of the units of a compacted object, and the ubuf it was given. */
static struct io_opaque_ubuf *opaque_managed_skb(struct kunit *test, struct opaque_splice *f)
{
	KUNIT_ASSERT_EQ(test, io_opaque_link_skb(f->skb, NULL), 0);
	KUNIT_ASSERT_NOT_NULL(test, skb_zcopy(f->skb));
	return container_of(skb_zcopy(f->skb), struct io_opaque_ubuf, uarg);
}

/*
 * A folio is one fragment and one reference of the ubuf, however many calls
 * fill from it, and the fragments are managed: the skb takes no reference
 * and weighs what it holds.
 */
static void opaque_managed_fill_runs(struct kunit *test)
{
	size_t len = OPAQUE_TEST_UNIT + 2 * PAGE_SIZE, first = 5 * PAGE_SIZE;
	struct opaque_managed before, after;
	struct io_opaque_ubuf *u;
	struct opaque_splice f;
	unsigned int truesize;
	skb_frag_t *frag;

	opaque_splice_setup(test, &f);
	truesize = f.skb->truesize;
	iov_iter_truncate(&f.iter, len);
	opaque_managed_read(&before);
	u = opaque_managed_skb(test, &f);
	KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(f.skb)->ops, &io_opaque_ubuf_ops);
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->flags & SKBFL_ALL_ZEROCOPY,
			SKBFL_ZEROCOPY_ENABLE | SKBFL_DONT_ORPHAN | SKBFL_DONT_ORPHAN_RX);
	KUNIT_EXPECT_EQ(test, u->nr, 0);
	/* The first call stops inside the first unit. */
	KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(f.skb, &f.iter, first), 0);
	KUNIT_EXPECT_TRUE(test, skb_zcopy_managed(f.skb));
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 1);
	KUNIT_EXPECT_EQ(test, u->nr, 1);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 2);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&f.iter), len - first);
	/* The rest of the unit extends the fragment, with no reference; the next starts another. */
	KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(f.skb, &f.iter, len - first), 0);
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 2);
	KUNIT_EXPECT_EQ(test, u->nr, 2);
	frag = &skb_shinfo(f.skb)->frags[0];
	KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), f.units[0]);
	KUNIT_EXPECT_EQ(test, skb_frag_off(frag), 0U);
	KUNIT_EXPECT_EQ(test, skb_frag_size(frag), (unsigned int)OPAQUE_TEST_UNIT);
	frag = &skb_shinfo(f.skb)->frags[1];
	KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), f.units[1]);
	KUNIT_EXPECT_EQ(test, skb_frag_off(frag), 0U);
	KUNIT_EXPECT_EQ(test, skb_frag_size(frag), (unsigned int)(2 * PAGE_SIZE));
	KUNIT_EXPECT_PTR_EQ(test, u->folio[0], page_folio(f.units[0]));
	KUNIT_EXPECT_PTR_EQ(test, u->folio[1], page_folio(f.units[1]));
	KUNIT_EXPECT_EQ(test, f.skb->len, (unsigned int)len);
	KUNIT_EXPECT_EQ(test, f.skb->data_len, (unsigned int)len);
	KUNIT_EXPECT_EQ(test, f.skb->truesize, truesize + (unsigned int)len);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&f.iter), 0UL);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 2);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 2);
	opaque_managed_read(&after);
	KUNIT_EXPECT_EQ(test, after.skbs - before.skbs, 1ULL);
	KUNIT_EXPECT_EQ(test, after.refs - before.refs, 2ULL);
	/* The skb let go of the data: the ubuf completes, and gives the folios back. */
	kfree_skb(f.skb);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 1);
	opaque_managed_read(&after);
	KUNIT_EXPECT_EQ(test, after.copied, before.copied);
	opaque_splice_done(&f);
}

/*
 * Ranges of one folio that touch make one fragment, and ranges that do not
 * make two, with the one reference; a range that starts inside a unit starts
 * the fragment at that offset, as a splice would.
 */
static void opaque_managed_fill_adjacent(struct kunit *test)
{
	size_t skip = PAGE_SIZE + 100;
	struct bio_vec bv[5];
	struct io_opaque_ubuf *u;
	struct opaque_splice f;
	struct iov_iter iter;
	skb_frag_t *frag;

	opaque_splice_setup(test, &f);
	u = opaque_managed_skb(test, &f);
	/* Three ranges in a row in unit 0, one after a gap, and one in unit 1. */
	bvec_set_page(&bv[0], f.units[0], 1000, skip);
	bvec_set_page(&bv[1], f.units[0], 1000, skip + 1000);
	bvec_set_page(&bv[2], f.units[0], 500, skip + 2000);
	bvec_set_page(&bv[3], f.units[0], 700, skip + 3000);
	bvec_set_page(&bv[4], f.units[1], 300, 10);
	iov_iter_bvec(&iter, ITER_SOURCE, bv, ARRAY_SIZE(bv), 3500);
	KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(f.skb, &iter, 3500), 0);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&iter), 0UL);
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 3);
	frag = &skb_shinfo(f.skb)->frags[0];
	KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), f.units[0]);
	KUNIT_EXPECT_EQ(test, skb_frag_off(frag), (unsigned int)skip);
	KUNIT_EXPECT_EQ(test, skb_frag_size(frag), 2500U);
	frag = &skb_shinfo(f.skb)->frags[1];
	KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), f.units[0]);
	KUNIT_EXPECT_EQ(test, skb_frag_off(frag), (unsigned int)(skip + 3000));
	KUNIT_EXPECT_EQ(test, skb_frag_size(frag), 700U);
	frag = &skb_shinfo(f.skb)->frags[2];
	KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), f.units[1]);
	KUNIT_EXPECT_EQ(test, skb_frag_off(frag), 10U);
	KUNIT_EXPECT_EQ(test, skb_frag_size(frag), 300U);
	/* Two folios, two references, for three fragments. */
	KUNIT_EXPECT_EQ(test, u->nr, 2);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 2);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 2);
	kfree_skb(f.skb);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 1);
	opaque_splice_done(&f);
}

/*
 * The fragment limit ends a fill with the bytes it added and the next with
 * none, and a ubuf that cannot hold another folio ends it like a full skb, for
 * an skb that shares a ubuf with a full one: the fill takes no reference it has
 * nowhere to keep. The skb that follows has a ubuf of its own and goes on.
 */
static void opaque_managed_fill_limits(struct kunit *test)
{
	const unsigned int limit = READ_ONCE(net_hotdata.sysctl_max_skb_frags);
	struct sk_buff *shared = alloc_skb(0, GFP_KERNEL), *next = alloc_skb(0, GFP_KERNEL);
	size_t room = 2000 * limit, total;
	struct opaque_managed before, after;
	struct io_opaque_ubuf *u, *v;
	struct opaque_segs *segs;
	struct opaque_splice f;
	struct iov_iter iter;

	KUNIT_ASSERT_NOT_NULL(test, shared);
	KUNIT_ASSERT_NOT_NULL(test, next);
	opaque_splice_setup(test, &f);
	/* One page, 2000 bytes each: a fragment each, four more than one skb may have. */
	segs = opaque_segs_new(test, limit + 4, 2000);
	total = 2000 * segs->nr;
	iov_iter_bvec(&iter, ITER_SOURCE, segs->bv, segs->nr, total);
	opaque_managed_read(&before);
	u = opaque_managed_skb(test, &f);
	KUNIT_EXPECT_EQ(test, io_opaque_sg_from_iter(f.skb, &iter, total), -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, (int)limit);
	KUNIT_EXPECT_EQ(test, f.skb->len, (unsigned int)room);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&iter), total - room);
	KUNIT_EXPECT_EQ(test, u->nr, limit);
	opaque_managed_read(&after);
	KUNIT_EXPECT_EQ(test, after.refs - before.refs, (u64)limit);
	/* A full skb takes nothing more, and takes no reference for it. */
	KUNIT_EXPECT_EQ(test, io_opaque_sg_from_iter(f.skb, &iter, total), -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, f.skb->len, (unsigned int)room);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&iter), total - room);
	opaque_managed_read(&after);
	KUNIT_EXPECT_EQ(test, after.refs - before.refs, (u64)limit);

	/* An skb that shares the full ubuf, as a split makes it, has no room for a folio. */
	skb_zcopy_set(shared, &u->uarg, NULL);
	if (limit == ARRAY_SIZE(u->folio)) {
		KUNIT_EXPECT_EQ(test, io_opaque_sg_from_iter(shared, &iter, 2000), -EMSGSIZE);
		KUNIT_EXPECT_EQ(test, shared->len, 0U);
		KUNIT_EXPECT_EQ(test, u->nr, limit);
		opaque_managed_read(&after);
		KUNIT_EXPECT_EQ(test, after.refs - before.refs, (u64)limit);
	}

	/* The skb after the full one has a ubuf of its own, and the rest of the bytes. */
	KUNIT_ASSERT_EQ(test, io_opaque_link_skb(next, NULL), 0);
	v = container_of(skb_zcopy(next), struct io_opaque_ubuf, uarg);
	KUNIT_EXPECT_PTR_NE(test, v, u);
	KUNIT_EXPECT_EQ(test, io_opaque_sg_from_iter(next, &iter, 6000), 0);
	KUNIT_EXPECT_EQ(test, next->len, 6000U);
	KUNIT_EXPECT_EQ(test, v->nr, 3);
	kfree_skb(f.skb);
	kfree_skb(shared);
	kfree_skb(next);
	/* Every reference was given back by the last skb of its ubuf. */
	opaque_splice_done(&f);
	KUNIT_EXPECT_EQ(test, atomic64_read(&f.store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&f.store->extents), 0);
}

/*
 * The units of a splice fixture can be given a pattern of their own, one that tells
 * every position of every unit from the others, so that a test can tell the bytes
 * that an skb points at from the bytes it should have.
 */
static u8 opaque_unit_byte(unsigned int unit, size_t pos)
{
	return (pos * 131 + pos / 257 + unit * 17) & 0xff;
}

static void opaque_unit_pattern(const struct opaque_splice *f)
{
	unsigned int i;
	size_t pos;

	for (i = 0; i < ARRAY_SIZE(f->units); i++)
		for (pos = 0; pos < page_size(f->units[i]); pos++)
			((u8 *)page_address(f->units[i]))[pos] = opaque_unit_byte(i, pos);
}

/*
 * The first @len bytes of @skb are those of the segments from @bv on, the first begun
 * @skip bytes in: what a fill was asked to add, in the order it was asked.
 */
static void opaque_expect_unit_bytes(struct kunit *test, const struct opaque_splice *f,
				     struct sk_buff *skb, const struct bio_vec *bv, size_t skip,
				     size_t len, u8 *got, u8 *want)
{
	size_t at = 0, k;

	while (at < len) {
		size_t part = min_t(size_t, bv->bv_len - skip, len - at);
		int unit = ARRAY_SIZE(f->units);

		while (--unit >= 0 && f->units[unit] != bv->bv_page)
			;
		KUNIT_ASSERT_GE(test, unit, 0);
		for (k = 0; k < part; k++)
			want[at + k] = opaque_unit_byte(unit, bv->bv_offset + skip + k);
		at += part;
		skip = 0;
		bv++;
	}
	KUNIT_ASSERT_EQ(test, skb_copy_bits(skb, 0, got, len), 0);
	KUNIT_EXPECT_MEMEQ(test, got, want, len);
}

/*
 * Ranges in several folios, in the order a send gives them. Ranges that touch in a
 * folio are one fragment. A range in a folio that the skb used before, with another
 * between, is a fragment of its own and not a reference: the folio is in the ubuf, and
 * the list is how the fill knows. The ubuf lists its folios in the order the skb first
 * used them, and the fragments point at the first page of a folio however far into it
 * the bytes are, at the bytes that the ranges name.
 */
static void opaque_managed_fill_folios(struct kunit *test)
{
	static const struct {
		int unit;
		u32 off, len;
	} r[] = {
		/* A fragment of unit 0, and the range that extends it. */
		{ 0, 100, 1000 }, { 0, 1100, 500 },
		/* A second folio. */
		{ 1, 0, 2000 },
		/* Unit 0 again, further on: a fragment, and the range that extends that. */
		{ 0, 3 * PAGE_SIZE + 17, 700 }, { 0, 3 * PAGE_SIZE + 717, 300 },
		/* A third folio. */
		{ 2, 10, 400 },
		/* Unit 1 where its first fragment ended, another folio's fragment between. */
		{ 1, 2000, 100 },
	};
	static const struct {
		int unit;
		u32 off, size;
	} frags[] = {
		{ 0, 100, 1500 }, { 1, 0, 2000 }, { 0, 3 * PAGE_SIZE + 17, 1000 },
		{ 2, 10, 400 }, { 1, 2000, 100 },
	};
	const size_t total = 5000;
	u8 *got = kunit_kmalloc(test, total, GFP_KERNEL);
	u8 *want = kunit_kmalloc(test, total, GFP_KERNEL);
	struct opaque_managed before, after;
	struct bio_vec bv[ARRAY_SIZE(r)];
	struct io_opaque_ubuf *u;
	struct opaque_splice f;
	struct iov_iter iter;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, got);
	KUNIT_ASSERT_NOT_NULL(test, want);
	opaque_splice_setup(test, &f);
	opaque_unit_pattern(&f);
	for (i = 0; i < ARRAY_SIZE(r); i++)
		bvec_set_page(&bv[i], f.units[r[i].unit], r[i].len, r[i].off);
	iov_iter_bvec(&iter, ITER_SOURCE, bv, ARRAY_SIZE(bv), total);
	u = opaque_managed_skb(test, &f);
	opaque_managed_read(&before);
	KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(f.skb, &iter, total), 0);
	opaque_managed_read(&after);

	KUNIT_EXPECT_EQ(test, iov_iter_count(&iter), 0UL);
	KUNIT_EXPECT_EQ(test, f.skb->len, (unsigned int)total);
	KUNIT_ASSERT_EQ(test, skb_shinfo(f.skb)->nr_frags, (int)ARRAY_SIZE(frags));
	for (i = 0; i < ARRAY_SIZE(frags); i++) {
		skb_frag_t *frag = &skb_shinfo(f.skb)->frags[i];

		KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(frag), f.units[frags[i].unit]);
		KUNIT_EXPECT_EQ(test, skb_frag_off(frag), frags[i].off);
		KUNIT_EXPECT_EQ(test, skb_frag_size(frag), frags[i].size);
	}
	/* Three folios, in the order they were first used, and a reference for each. */
	KUNIT_ASSERT_EQ(test, u->nr, 3);
	for (i = 0; i < 3; i++) {
		KUNIT_EXPECT_PTR_EQ(test, u->folio[i], page_folio(f.units[i]));
		KUNIT_EXPECT_EQ(test, page_count(f.units[i]), 2);
	}
	KUNIT_EXPECT_EQ(test, after.refs - before.refs, 3ULL);
	KUNIT_EXPECT_EQ(test, after.fallback, before.fallback);
	opaque_expect_unit_bytes(test, &f, f.skb, bv, 0, total, got, want);
	kfree_skb(f.skb);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_EQ(test, page_count(f.units[i]), 1);
	opaque_splice_done(&f);
}

/*
 * The cursor after a fill is where iov_iter_advance() leaves it for the bytes the fill
 * added, wherever the fill stops: inside a segment or at the end of one, with the first
 * segment already begun, and for every length up to all that is left. The skb points at
 * the bytes of the ranges.
 */
static void opaque_managed_fill_cursor(struct kunit *test)
{
	static const struct {
		int unit;
		u32 off, len;
	} r[] = {
		{ 0, 3 * PAGE_SIZE + 5, 400 }, { 0, 4 * PAGE_SIZE, 300 }, { 1, 17, 500 },
		{ 2, 0, 100 }, { 0, 5 * PAGE_SIZE + 2, 1 }, { 1, 5000, 800 },
	};
	const size_t begun = 123;
	struct bio_vec bv[ARRAY_SIZE(r)];
	struct iov_iter start;
	struct opaque_splice f;
	size_t total = 0, len;
	u8 *got, *want;
	unsigned int i;

	opaque_splice_setup(test, &f);
	opaque_unit_pattern(&f);
	for (i = 0; i < ARRAY_SIZE(r); i++) {
		bvec_set_page(&bv[i], f.units[r[i].unit], r[i].len, r[i].off);
		total += r[i].len;
	}
	got = kunit_kmalloc(test, total, GFP_KERNEL);
	want = kunit_kmalloc(test, total, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, got);
	KUNIT_ASSERT_NOT_NULL(test, want);
	/* The first segment is begun when the fill is called, as a send's second call finds it. */
	iov_iter_bvec(&start, ITER_SOURCE, bv, ARRAY_SIZE(bv), total);
	iov_iter_advance(&start, begun);
	for (len = 0; len <= total - begun; len++) {
		struct iov_iter had = start, left = start;
		struct io_opaque_ubuf *u;

		kfree_skb(f.skb);
		f.skb = alloc_skb(0, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, f.skb);
		u = opaque_managed_skb(test, &f);
		KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(f.skb, &had, len), 0);
		iov_iter_advance(&left, len);
		KUNIT_ASSERT_PTR_EQ(test, had.bvec, left.bvec);
		KUNIT_ASSERT_EQ(test, had.nr_segs, left.nr_segs);
		KUNIT_ASSERT_EQ(test, had.iov_offset, left.iov_offset);
		KUNIT_ASSERT_EQ(test, had.count, left.count);
		KUNIT_ASSERT_EQ(test, f.skb->len, (unsigned int)len);
		KUNIT_ASSERT_LE(test, u->nr, 3);
		if (len)
			opaque_expect_unit_bytes(test, &f, f.skb, bv, begun, len, got, want);
	}
	kfree_skb(f.skb);
	opaque_splice_done(&f);
}

/*
 * At the fragment limit a range that touches the last fragment still extends it, as a
 * splice's does, and the one after that, which does not, is where the fill stops. It
 * takes no reference for it and leaves the cursor at it. The limit is the sysctl's, also
 * when that is lower than the ubuf could hold.
 */
static void opaque_managed_fill_at_limit(struct kunit *test)
{
	static const int limits[] = { 0, 4, 1 };
	unsigned int c;

	for (c = 0; c < ARRAY_SIZE(limits); c++) {
		unsigned int limit = limits[c];
		struct opaque_managed before, after;
		struct io_opaque_ubuf *u;
		struct opaque_splice f;
		struct iov_iter iter;
		struct bio_vec *bv;
		size_t total = 0;
		unsigned int i;

		if (!limit)
			limit = READ_ONCE(net_hotdata.sysctl_max_skb_frags);
		else if (limit >= READ_ONCE(net_hotdata.sysctl_max_skb_frags))
			continue;
		else
			opaque_max_frags_set(test, limit);
		bv = kunit_kcalloc(test, limit + 2, sizeof(*bv), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, bv);
		opaque_splice_setup(test, &f);
		/* 100-byte fragments with gaps, a range that touches the last, one that cannot. */
		for (i = 0; i < limit; i++)
			bvec_set_page(&bv[i], f.units[0], 100, i * 200);
		bvec_set_page(&bv[limit], f.units[0], 50, (limit - 1) * 200 + 100);
		bvec_set_page(&bv[limit + 1], f.units[0], 80, (limit + 1) * 200);
		for (i = 0; i < limit + 2; i++)
			total += bv[i].bv_len;
		iov_iter_bvec(&iter, ITER_SOURCE, bv, limit + 2, total);
		u = opaque_managed_skb(test, &f);
		opaque_managed_read(&before);
		KUNIT_EXPECT_EQ(test, io_opaque_sg_from_iter(f.skb, &iter, total), -EMSGSIZE);
		opaque_managed_read(&after);
		KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, (int)limit);
		KUNIT_EXPECT_EQ(test, f.skb->len, (unsigned int)(limit * 100 + 50));
		KUNIT_EXPECT_EQ(test, skb_frag_size(&skb_shinfo(f.skb)->frags[limit - 1]), 150U);
		KUNIT_EXPECT_EQ(test, iov_iter_count(&iter), 80UL);
		KUNIT_EXPECT_EQ(test, u->nr, 1);
		KUNIT_EXPECT_EQ(test, after.refs - before.refs, 1ULL);
		/* Full: nothing more, no reference, and the cursor stays. */
		KUNIT_EXPECT_EQ(test, io_opaque_sg_from_iter(f.skb, &iter, total), -EMSGSIZE);
		KUNIT_EXPECT_EQ(test, f.skb->len, (unsigned int)(limit * 100 + 50));
		KUNIT_EXPECT_EQ(test, iov_iter_count(&iter), 80UL);
		opaque_managed_read(&after);
		KUNIT_EXPECT_EQ(test, after.refs - before.refs, 1ULL);
		KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 2);
		kfree_skb(f.skb);
		KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
		opaque_splice_done(&f);
	}
}

/*
 * What the fill relies on of a version's segments: each is the first page of its folio, and
 * ends inside it. They are what the paths that make a version make of its extents.
 */
static void opaque_expect_segments(struct kunit *test, const struct io_opaque_data *data)
{
	u32 i;

	for (i = 0; i < data->nr; i++) {
		const struct bio_vec *bv = &data->bvec[i];

		KUNIT_EXPECT_FALSE_MSG(test, PageTail(bv->bv_page), "segment %u", i);
		KUNIT_EXPECT_GT_MSG(test, bv->bv_len, 0U, "segment %u", i);
		KUNIT_EXPECT_LE_MSG(test, (size_t)bv->bv_offset + bv->bv_len,
				    (size_t)page_size(bv->bv_page), "segment %u", i);
	}
}

/*
 * The whole of @data through the managed fill, as many skbs as it takes, and the bytes of the skbs
 * are those of the version, which the caller read into @want.
 */
static void opaque_expect_object_fill(struct kunit *test, struct io_opaque_data *data,
				      const u8 *want)
{
	u8 *got = kunit_kmalloc(test, data->length, GFP_KERNEL);
	struct iov_iter iter;
	size_t done = 0;

	KUNIT_ASSERT_NOT_NULL(test, got);
	iov_iter_bvec(&iter, ITER_SOURCE, data->bvec, data->nr, data->length);
	while (iov_iter_count(&iter)) {
		struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);
		int ret;

		KUNIT_ASSERT_NOT_NULL(test, skb);
		KUNIT_ASSERT_EQ(test, io_opaque_link_skb(skb, NULL), 0);
		ret = io_opaque_sg_from_iter(skb, &iter, iov_iter_count(&iter));
		KUNIT_ASSERT_TRUE(test, ret == 0 || ret == -EMSGSIZE);
		KUNIT_ASSERT_GT(test, skb->len, 0U);
		KUNIT_ASSERT_LE(test, done + skb->len, data->length);
		KUNIT_ASSERT_EQ(test, skb_copy_bits(skb, 0, got + done, skb->len), 0);
		done += skb->len;
		kfree_skb(skb);
	}
	KUNIT_EXPECT_EQ(test, done, (size_t)data->length);
	KUNIT_EXPECT_MEMEQ(test, got, want, data->length);
}

/*
 * Versions as the paths that make them make them, sent through the managed fill: extents of
 * compound pages at odd offsets, across pages and beyond the first, and the same compacted into
 * units of every order the unit cache allows, whole units and a tail. In each the segments are
 * the first pages of their folios and end inside them, and the skbs point at the version's bytes.
 */
static void opaque_managed_fill_objects(struct kunit *test)
{
	static const unsigned int orders[] = { 0, 1, IO_OPAQUE_UNIT_ORDER, IO_OPAQUE_MAX_ORDER };
	struct io_opaque_store *store = opaque_test_store(test);
	u32 length = 2 * OPAQUE_TEST_UNIT + PAGE_SIZE + 369;
	struct io_opaque_data *runs = opaque_runs_data(test, store, length);
	u8 *want = kunit_kmalloc(test, 80000, GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, want);
	opaque_expect_segments(test, runs);
	opaque_test_copy(runs, want);
	opaque_expect_object_fill(test, runs, want);
	io_opaque_data_put(store, runs);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);

	for (i = 0; i < ARRAY_SIZE(orders); i++) {
		struct io_opaque_store *s = opaque_test_store(test);
		struct io_opaque_data *data = opaque_sparse_data(test, s);
		struct io_opaque_req op = { .store = s, .data = data };

		io_opaque_unit_cache_init(s, 4, orders[i]);
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_test_drain, s), 0);
		opaque_test_copy(data, want);
		KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
		opaque_expect_segments(test, op.replacement);
		/* The first unit is whole, and of the order of the cache. */
		KUNIT_EXPECT_EQ_MSG(test, compound_order(op.replacement->bvec[0].bv_page),
				    orders[i], "order %u", orders[i]);
		opaque_expect_object_fill(test, op.replacement, want);
		io_opaque_data_put(s, op.replacement);
		op.replacement = NULL;
		io_opaque_compact_end(&op);
		io_opaque_data_put(s, data);
	}
}

/*
 * A capture makes extents of what skbs carry, and the segments of those are the first pages of
 * their folios whichever page of the folio a frag names: a frag on a tail page, at an offset
 * that is not in its page and that crosses into the next, and on the first page itself.
 */
static void opaque_managed_fill_captured(struct kunit *test)
{
	static const struct { unsigned int page; u32 off, len; } r[] = {
		{ 2, 100, 2000 }, { 1, 3000, 4000 }, { 0, 5000, 900 }, { 3, 0, 4096 },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct page *page = alloc_pages(GFP_KERNEL | __GFP_COMP, 2);
	struct io_opaque_data *data = io_opaque_data_alloc();
	u8 *want = kunit_kmalloc(test, 16 * PAGE_SIZE, GFP_KERNEL);
	struct io_opaque_stream stream;
	u32 at = 0, i, k;

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_NOT_NULL(test, want);
	for (k = 0; k < page_size(page); k++)
		((u8 *)page_address(page))[k] = (k * 131 + k / 251) & 0xff;
	opaque_test_stream(&stream, store);
	for (i = 0; i < ARRAY_SIZE(r); i++) {
		struct sk_buff *skb = opaque_test_frag(test, page + r[i].page, r[i].off, r[i].len);

		KUNIT_ASSERT_EQ(test, io_opaque_capture(&stream, skb, 0, r[i].len, false),
				(int)r[i].len);
		kfree_skb(skb);
		for (k = 0; k < r[i].len; k++)
			want[at + k] = ((r[i].page * PAGE_SIZE + r[i].off + k) * 131 +
					(r[i].page * PAGE_SIZE + r[i].off + k) / 251) & 0xff;
		at += r[i].len;
	}
	/* The extents are the version, in the order they were captured. */
	list_splice_init(&stream.extents, &data->extents);
	data->length = at;
	KUNIT_ASSERT_EQ(test, io_opaque_vector(store, data), 0);
	KUNIT_ASSERT_EQ(test, data->nr, (u32)ARRAY_SIZE(r));
	opaque_expect_segments(test, data);
	for (i = 0; i < ARRAY_SIZE(r); i++) {
		KUNIT_EXPECT_PTR_EQ(test, data->bvec[i].bv_page, page);
		KUNIT_EXPECT_EQ(test, data->bvec[i].bv_offset, r[i].page * PAGE_SIZE + r[i].off);
		KUNIT_EXPECT_EQ(test, data->bvec[i].bv_len, r[i].len);
	}
	opaque_expect_object_fill(test, data, want);
	io_opaque_data_put(store, data);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	put_page(page);
}

/*
 * An skb that has fragments of its own cannot be managed: a framed send's
 * prefix is one. The body is then filled as a splice fills it, with a page
 * reference for each fragment and none in the ubuf, and the fill is counted.
 */
static void opaque_managed_fill_fallback(struct kunit *test)
{
	struct page *prefix = alloc_page(GFP_KERNEL);
	struct opaque_managed before, after;
	const unsigned int limit = READ_ONCE(net_hotdata.sysctl_max_skb_frags);
	struct io_opaque_ubuf *u;
	struct opaque_segs *segs;
	struct opaque_splice f;
	struct iov_iter iter;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, prefix);
	opaque_splice_setup(test, &f);
	skb_add_rx_frag(f.skb, 0, prefix, 0, 24, 24);
	u = opaque_managed_skb(test, &f);
	/* Pages for the fragments that fill the skb to its limit, and four more. */
	segs = opaque_segs_new(test, limit + 4, 2000);
	iov_iter_bvec(&iter, ITER_SOURCE, segs->bv, segs->nr, 2000 * segs->nr);
	opaque_managed_read(&before);
	KUNIT_EXPECT_EQ(test, io_opaque_sg_from_iter(f.skb, &iter, 6000), 0);
	opaque_managed_read(&after);
	KUNIT_EXPECT_EQ(test, after.fallback - before.fallback, 1ULL);
	KUNIT_EXPECT_EQ(test, after.refs, before.refs);
	KUNIT_EXPECT_FALSE(test, skb_zcopy_managed(f.skb));
	KUNIT_EXPECT_EQ(test, u->nr, 0);
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 4);
	KUNIT_EXPECT_EQ(test, f.skb->len, 6024U);
	/* A reference of the skb's own on each page. */
	for (i = 1; i < 4; i++)
		KUNIT_EXPECT_EQ(test, page_count(skb_frag_page(&skb_shinfo(f.skb)->frags[i])), 2);
	/*
	 * The skb is full when its fragments are, and then TCP starts another. Each fill adds a
	 * fragment: one that adds nothing, or an iterator that is spent, fails the test and does
	 * not leave it looping.
	 */
	for (i = 0; skb_shinfo(f.skb)->nr_frags < limit; i++) {
		KUNIT_ASSERT_LT(test, i, limit);
		KUNIT_ASSERT_GT(test, iov_iter_count(&iter), 0UL);
		KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(f.skb, &iter, 2000), 0);
	}
	KUNIT_EXPECT_EQ(test, io_opaque_sg_from_iter(f.skb, &iter, 2000), -EMSGSIZE);
	kfree_skb(f.skb);
	opaque_splice_done(&f);
	KUNIT_EXPECT_EQ(test, atomic64_read(&f.store->bytes), 0LL);
}

/* The foreign ubuf an skb of another provider carries. */
static void opaque_test_foreign_complete(struct sk_buff *skb, struct ubuf_info *uarg,
					 bool success)
{
}

static const struct ubuf_info_ops opaque_test_foreign_ops = {
	.complete = opaque_test_foreign_complete,
};

/*
 * A skb with a ubuf of ours is joined for no more than the look, another
 * provider's is a boundary, and a skb that has none gets one.
 */
static void opaque_managed_link(struct kunit *test)
{
	struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);
	struct ubuf_info foreign = {
		.ops = &opaque_test_foreign_ops, .flags = SKBFL_ZEROCOPY_ENABLE,
	};
	struct opaque_managed before, after;
	struct ubuf_info *uarg;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	opaque_managed_read(&before);
	KUNIT_ASSERT_EQ(test, io_opaque_link_skb(skb, NULL), 0);
	uarg = skb_zcopy(skb);
	KUNIT_ASSERT_NOT_NULL(test, uarg);
	KUNIT_EXPECT_EQ(test, refcount_read(&uarg->refcnt), 1U);
	/* Joined: the same ubuf, and no new object. */
	KUNIT_EXPECT_EQ(test, io_opaque_link_skb(skb, NULL), 0);
	KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(skb), uarg);
	KUNIT_EXPECT_EQ(test, refcount_read(&uarg->refcnt), 1U);
	opaque_managed_read(&after);
	KUNIT_EXPECT_EQ(test, after.skbs - before.skbs, 1ULL);
	kfree_skb(skb);

	skb = alloc_skb(0, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	refcount_set(&foreign.refcnt, 1);
	skb_zcopy_init(skb, &foreign);
	KUNIT_EXPECT_EQ(test, io_opaque_link_skb(skb, NULL), -EEXIST);
	KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(skb), &foreign);
	kfree_skb(skb);
	opaque_managed_read(&after);
	KUNIT_EXPECT_EQ(test, after.skbs - before.skbs, 1ULL);
}

/*
 * A skb of ours with all the fragments it may have is a boundary, as another
 * provider's is: -EEXIST, which TCP takes as the end of that skb, before any fill
 * is let in. One with room is joined, whatever it holds, and the limit is the
 * sysctl's, also when it is lower than the ubuf could hold. A skb that has no ubuf
 * yet gets one even at a limit of one fragment.
 */
static void opaque_managed_link_full(struct kunit *test)
{
	const int limit = READ_ONCE(net_hotdata.sysctl_max_skb_frags);
	struct opaque_managed before, after;
	struct io_opaque_ubuf *u;
	struct ubuf_info *uarg;
	struct bio_vec bv[3];
	struct opaque_splice f;
	struct iov_iter iter;
	unsigned int i;

	opaque_splice_setup(test, &f);
	for (i = 0; i < ARRAY_SIZE(bv); i++)
		bvec_set_page(&bv[i], f.units[0], 100, i * 200);
	iov_iter_bvec(&iter, ITER_SOURCE, bv, ARRAY_SIZE(bv), 300);
	u = opaque_managed_skb(test, &f);
	uarg = &u->uarg;
	KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(f.skb, &iter, 300), 0);
	KUNIT_ASSERT_EQ(test, skb_shinfo(f.skb)->nr_frags, 3);

	/* Three fragments, and room for more: joined, with no new ubuf and no new reference. */
	opaque_managed_read(&before);
	KUNIT_EXPECT_EQ(test, io_opaque_link_skb(f.skb, NULL), 0);
	KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(f.skb), uarg);
	/* As many as the limit allows, or fewer: no room, and nothing is made or changed. */
	opaque_max_frags_set(test, 4);
	KUNIT_EXPECT_EQ(test, io_opaque_link_skb(f.skb, NULL), 0);
	opaque_max_frags_set(test, 3);
	KUNIT_EXPECT_EQ(test, io_opaque_link_skb(f.skb, NULL), -EEXIST);
	opaque_max_frags_set(test, 2);
	KUNIT_EXPECT_EQ(test, io_opaque_link_skb(f.skb, NULL), -EEXIST);
	KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(f.skb), uarg);
	KUNIT_EXPECT_EQ(test, refcount_read(&uarg->refcnt), 1U);
	KUNIT_EXPECT_EQ(test, skb_shinfo(f.skb)->nr_frags, 3);
	opaque_managed_read(&after);
	KUNIT_EXPECT_EQ(test, after.skbs, before.skbs);
	/* A skb that has no ubuf gets one, however low the limit. */
	opaque_max_frags_set(test, 1);
	{
		struct sk_buff *fresh = alloc_skb(0, GFP_KERNEL);

		KUNIT_ASSERT_NOT_NULL(test, fresh);
		KUNIT_EXPECT_EQ(test, io_opaque_link_skb(fresh, NULL), 0);
		KUNIT_EXPECT_NOT_NULL(test, skb_zcopy(fresh));
		KUNIT_EXPECT_PTR_NE(test, skb_zcopy(fresh), uarg);
		/* ... and it is full once it has the one fragment, which the fill gave it. */
		iov_iter_bvec(&iter, ITER_SOURCE, bv, 1, 100);
		KUNIT_EXPECT_EQ(test, io_opaque_sg_from_iter(fresh, &iter, 100), 0);
		KUNIT_EXPECT_EQ(test, io_opaque_link_skb(fresh, NULL), -EEXIST);
		kfree_skb(fresh);
	}
	opaque_managed_read(&after);
	KUNIT_EXPECT_EQ(test, after.skbs - before.skbs, 1ULL);
	KUNIT_EXPECT_GE(test, limit, 1);
	kfree_skb(f.skb);
	opaque_splice_done(&f);
}

/* What TCP sees of the fill when a test drives tcp_sendmsg_locked() itself. */
static struct {
	unsigned int calls;
	/* Fills that said -EMSGSIZE having added nothing: each one is reverted by TCP's helper. */
	unsigned int empty;
} opaque_fill_spy;

static int opaque_test_spy_fill(struct sk_buff *skb, struct iov_iter *from, size_t length)
{
	unsigned int len = skb->len;
	int ret = io_opaque_sg_from_iter(skb, from, length);

	opaque_fill_spy.calls++;
	if (ret == -EMSGSIZE && skb->len == len)
		opaque_fill_spy.empty++;
	return ret;
}

/*
 * A send of 1000 bytes of each of 40 pages, to TCP itself: an skb gets as many
 * fragments as it may, which is well below what its size would allow, and the next
 * fill is not let into it. The skb after it has a ubuf of its own. No fill ever finds
 * its skb full: link_skb() has said so before it.
 */
static void opaque_managed_boundary(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct ubuf_info uarg = { .ops = &io_opaque_ubuf_ops };
	struct bio_vec *bv = kunit_kcalloc(test, 40, sizeof(*bv), GFP_KERNEL);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 40000, GFP_KERNEL);
	u8 *want = kunit_kmalloc(test, 40000, GFP_KERNEL);
	struct opaque_page_refs *pages;
	struct opaque_managed before, after;
	struct msghdr msg = {};
	unsigned int limit, skbs, i;
	struct sock *sk;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, bv);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	KUNIT_ASSERT_NOT_NULL(test, want);
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	/*
	 * 40 pages of 1000 bytes are 40 KB, and TCP ends an skb at its size goal as well as at the
	 * fragment limit: some 32 KB on this connection. The limit is what ends an skb here, at the
	 * number of pages that the object has to fill the skbs with, not 45 of them.
	 */
	opaque_max_frags_set(test, OPAQUE_TEST_FRAGS);
	limit = READ_ONCE(net_hotdata.sysctl_max_skb_frags);
	skbs = DIV_ROUND_UP(40, limit);
	opaque_test_copy(data, source);
	pages = opaque_page_refs_take(test, data);
	for (i = 0; i < 40; i++) {
		bvec_set_page(&bv[i], data->bvec[i].bv_page, 1000, 0);
		memcpy(want + i * 1000, source + i * 2000, 1000);
	}
	sk = w->tx->sk;
	msg.msg_flags = MSG_ZEROCOPY;
	msg.msg_ubuf = &uarg;
	msg.sg_from_iter = opaque_test_spy_fill;
	iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, bv, 40, 40000);
	memset(&opaque_fill_spy, 0, sizeof(opaque_fill_spy));

	opaque_managed_read(&before);
	lock_sock(sk);
	ret = tcp_sendmsg_locked(sk, &msg, 40000);
	release_sock(sk);
	opaque_managed_read(&after);
	KUNIT_EXPECT_EQ(test, ret, 40000);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&msg.msg_iter), 0UL);
	KUNIT_EXPECT_EQ(test, after.skbs - before.skbs, (u64)skbs);
	KUNIT_EXPECT_EQ(test, after.refs - before.refs, 40ULL);
	KUNIT_EXPECT_EQ(test, after.fallback, before.fallback);
	KUNIT_EXPECT_EQ(test, opaque_fill_spy.calls, skbs);
	KUNIT_EXPECT_EQ(test, opaque_fill_spy.empty, 0U);
	opaque_wire_read(test, w, got, 0, 40000);
	KUNIT_EXPECT_MEMEQ(test, got, want, 40000);
	opaque_wire_acked(sk);
	KUNIT_EXPECT_TRUE(test, opaque_pages_settle(pages, true));
	io_opaque_data_put(store, data);
	KUNIT_EXPECT_TRUE(test, opaque_pages_settle(pages, false));
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
}

struct opaque_atomic {
	struct tasklet_struct tasklet;
	struct completion done;
	struct sk_buff *skb;
	bool copy, softirq;
	int ret;
};

static void opaque_atomic_fn(struct tasklet_struct *t)
{
	struct opaque_atomic *a = from_tasklet(a, t, tasklet);

	a->softirq = in_serving_softirq();
	if (a->copy)
		a->ret = skb_copy_ubufs(a->skb, GFP_ATOMIC);
	kfree_skb(a->skb);
	complete(&a->done);
}

/*
 * The ubuf completes where the last skb is freed, and that is often a softirq:
 * the ACK that frees a sent skb, or a consumer that copies the skb on the
 * receive path and so completes it with the data copied. Neither may sleep or
 * take a lock that is not for interrupts; the debug configuration checks that.
 */
static void opaque_ubuf_complete_atomic(struct kunit *test)
{
	struct opaque_atomic *a = kunit_kzalloc(test, sizeof(*a), GFP_KERNEL);
	unsigned int copy;

	KUNIT_ASSERT_NOT_NULL(test, a);
	for (copy = 0; copy < 2; copy++) {
		struct opaque_managed before, after;
		struct opaque_splice f;

		opaque_splice_setup(test, &f);
		opaque_managed_skb(test, &f);
		KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(f.skb, &f.iter, 80000), 0);
		KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 2);
		KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 2);
		KUNIT_EXPECT_EQ(test, page_count(f.units[2]), 2);
		a->skb = f.skb;
		a->copy = copy;
		a->softirq = false;
		a->ret = -1;
		init_completion(&a->done);
		tasklet_setup(&a->tasklet, opaque_atomic_fn);
		opaque_managed_read(&before);
		tasklet_schedule(&a->tasklet);
		KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&a->done, 5 * HZ));
		tasklet_kill(&a->tasklet);
		opaque_managed_read(&after);
		KUNIT_EXPECT_TRUE(test, a->softirq);
		KUNIT_EXPECT_EQ(test, a->ret, copy ? 0 : -1);
		/* Copied: told so, once. Freed: not. The folios are back either way. */
		KUNIT_EXPECT_EQ(test, after.copied - before.copied, (u64)copy);
		KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
		KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 1);
		KUNIT_EXPECT_EQ(test, page_count(f.units[2]), 1);
		opaque_splice_done(&f);
	}
}

/* The msg_ubuf of a send as io_opaque_send() sets it where a body is managed: its ops. */
static struct io_opaque_msg_ubuf opaque_new_msg_ubuf(void)
{
	return (struct io_opaque_msg_ubuf) { .uarg.ops = &io_opaque_msg_ubuf_ops };
}

/*
 * The pool of ubufs that a store keeps. A test makes a pool and the msg_ubuf of a send that names
 * it, as io_opaque_send() hands one to TCP, and closes the pool as the store would unless the
 * test has done so already. The skbs of a test are linked to a ubuf through that msg_ubuf.
 */
struct opaque_pool {
	struct io_opaque_ubuf_pool *pool;
	struct io_opaque_msg_ubuf mub;
	bool closed;
};

static void opaque_pool_close_action(void *arg)
{
	struct opaque_pool *p = arg;

	if (!p->closed)
		io_opaque_ubuf_pool_close(p->pool);
}

static struct opaque_pool *opaque_pool_new(struct kunit *test)
{
	struct opaque_pool *p = kunit_kzalloc(test, sizeof(*p), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, p);
	p->pool = io_opaque_ubuf_pool_new();
	KUNIT_ASSERT_NOT_NULL(test, p->pool);
	p->mub = opaque_new_msg_ubuf();
	p->mub.pool = p->pool;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_pool_close_action, p), 0);
	return p;
}

static void opaque_pool_close(struct opaque_pool *p)
{
	p->closed = true;
	io_opaque_ubuf_pool_close(p->pool);
}

/* A skb with a ubuf, as TCP has link_skb() give it one for the msg_ubuf of a send. */
static struct sk_buff *opaque_pool_skb(struct kunit *test, struct opaque_pool *p)
{
	struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb->ip_summed = CHECKSUM_PARTIAL;
	KUNIT_ASSERT_EQ(test, io_opaque_link_skb(skb, &p->mub.uarg), 0);
	KUNIT_ASSERT_NOT_NULL(test, skb_zcopy(skb));
	return skb;
}

static struct io_opaque_ubuf *opaque_skb_ubuf(struct sk_buff *skb)
{
	return container_of(skb_zcopy(skb), struct io_opaque_ubuf, uarg);
}

/* The ubufs made and not yet freed, the pools, and the counters that say what the ubufs did. */
struct opaque_pool_snap {
	long live, pools;
	u64 skbs, reused, copied;
};

static void opaque_pool_read(struct opaque_pool_snap *m)
{
	m->live = atomic_long_read(&io_opaque_ubufs_live);
	m->pools = atomic_long_read(&io_opaque_ubuf_pools_live);
	m->skbs = io_opaque_stat_read(IO_OPAQUE_STAT_SEND_MANAGED_SKBS);
	m->reused = io_opaque_stat_read(IO_OPAQUE_STAT_SEND_UBUF_REUSED);
	m->copied = io_opaque_stat_read(IO_OPAQUE_STAT_SEND_UBUF_COPIED);
}

/* The ubufs that a pool holds on its list of those that completed, and in its stock. */
static unsigned int opaque_pool_held(struct io_opaque_ubuf_pool *pool, unsigned int *stocked)
{
	const struct llist_node *node;
	unsigned int done = 0;

	*stocked = 0;
	/* A closed pool has a sentinel for a list, which is not a ubuf. */
	for (node = READ_ONCE(pool->done.first);
	     node && node != (void *)1UL && done < 4 * IO_OPAQUE_UBUF_POOL_MAX; node = node->next)
		done++;
	for (node = pool->stock; node && *stocked < 4 * IO_OPAQUE_UBUF_POOL_MAX; node = node->next)
		(*stocked)++;
	/*
	 * A list that goes round, as one with a ubuf on it twice does, is a failure that ends
	 * here: it is cut, so that closing the pool does not go round for ever.
	 */
	if (done >= 4 * IO_OPAQUE_UBUF_POOL_MAX || *stocked >= 4 * IO_OPAQUE_UBUF_POOL_MAX) {
		KUNIT_FAIL(kunit_get_current_test(), "the list of the pool goes round");
		WRITE_ONCE(pool->done.first, NULL);
		pool->stock = NULL;
	}
	return done;
}

/* What the pool of @p holds: @on_list of the ubufs that completed, and @in_stock taken out. */
#define OPAQUE_POOL_HELD(test, p, on_list, in_stock)				\
	do {									\
		unsigned int __stock, __done = opaque_pool_held((p)->pool, &__stock);	\
										\
		KUNIT_EXPECT_EQ(test, __done, (unsigned int)(on_list));		\
		KUNIT_EXPECT_EQ(test, __stock, (unsigned int)(in_stock));	\
	} while (0)

/*
 * A skb gets the ubuf of one that completed, and gets it whole, as a ubuf made new is: one
 * reference, no folios, none of the msg_ubuf of the send that had it before, the flags and the ops
 * of one of ours. The cache is not asked while the pool has one: the ubufs made stay as many as
 * the skbs that are in use at once, however many skbs there are, and the counter says which
 * were reused.
 */
static void opaque_ubuf_pool_reuse(struct kunit *test)
{
	struct opaque_pool *p = opaque_pool_new(test);
	struct opaque_pool_snap before, after;
	struct io_opaque_ubuf *first, *u;
	struct opaque_splice f;
	struct iov_iter iter;
	struct sk_buff *skb;
	unsigned int i;

	opaque_splice_setup(test, &f);
	opaque_pool_read(&before);
	/* The pool is empty: the cache gives the first, which counts the pool among its owners. */
	skb = opaque_pool_skb(test, p);
	first = opaque_skb_ubuf(skb);
	KUNIT_EXPECT_PTR_EQ(test, first->pool, p->pool);
	KUNIT_EXPECT_EQ(test, refcount_read(&p->pool->refs), 2U);
	OPAQUE_POOL_HELD(test, p, 0, 0);
	iov_iter_bvec(&iter, ITER_SOURCE, f.op.replacement->bvec, f.op.replacement->nr, 80000);
	KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(skb, &iter, 6000), 0);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 2);
	kfree_skb(skb);
	/* It let go of its folio and went to the pool, not to the cache. */
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
	/* Under KASAN it is poisoned there, and the track of the free is where the count was. */
	if (!IS_ENABLED(CONFIG_KASAN))
		KUNIT_EXPECT_EQ(test, refcount_read(&first->uarg.refcnt), 0U);
	OPAQUE_POOL_HELD(test, p, 1, 0);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live - before.live, 1L);
	KUNIT_EXPECT_EQ(test, after.reused, before.reused);

	/* Every skb after it gets that one, and nothing else is made. */
	for (i = 0; i < 6; i++) {
		skb = opaque_pool_skb(test, p);
		u = opaque_skb_ubuf(skb);
		KUNIT_EXPECT_PTR_EQ(test, u, first);
		KUNIT_EXPECT_EQ(test, refcount_read(&u->uarg.refcnt), 1U);
		KUNIT_EXPECT_EQ(test, u->nr, 0);
		KUNIT_EXPECT_PTR_EQ(test, u->uarg.ops, &io_opaque_ubuf_ops);
		KUNIT_EXPECT_EQ(test, u->uarg.flags, io_opaque_ubuf_flags());
		KUNIT_EXPECT_PTR_EQ(test, u->pool, p->pool);
		OPAQUE_POOL_HELD(test, p, 0, 0);
		iov_iter_bvec(&iter, ITER_SOURCE, f.op.replacement->bvec, f.op.replacement->nr,
			      80000);
		KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(skb, &iter, 40000), 0);
		KUNIT_EXPECT_EQ(test, u->nr, 2);
		KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 2);
		KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 2);
		kfree_skb(skb);
		KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
		KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 1);
		OPAQUE_POOL_HELD(test, p, 1, 0);
	}
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live - before.live, 1L);
	KUNIT_EXPECT_EQ(test, after.reused - before.reused, 6ULL);
	KUNIT_EXPECT_EQ(test, after.skbs - before.skbs, 7ULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&p->pool->refs), 2U);
	opaque_splice_done(&f);
}

/*
 * A ubuf that a clone, a split or a tap holds is not the pool's: it completes when the last
 * skb that holds it is freed, not the first, nobody else gets it meanwhile, and it is whole while
 * it is held, with its count, its folios and its fragments' pages. The device's skb and a clone
 * for a tap share a shinfo, which holds one count; a split gives each skb a count on the ubuf; a
 * tap that the receive path copies for lets go of its count and keeps its copy.
 */
enum {
	OPAQUE_SHARE_CLONE,
	OPAQUE_SHARE_SPLIT,
	OPAQUE_SHARE_TAP_HELD,
	OPAQUE_SHARE_TAP_COPIED,
	OPAQUE_SHARE_NR,
};

static void opaque_ubuf_pool_share_one(struct kunit *test, unsigned int how)
{
	bool copied = how == OPAQUE_SHARE_TAP_COPIED;
	struct opaque_pool *p = opaque_pool_new(test);
	struct opaque_pool_snap before, after;
	struct sk_buff *skb, *twin, *other, *next;
	struct io_opaque_ubuf *u;
	struct opaque_splice f;

	opaque_splice_setup(test, &f);
	if (copied)
		kunit_activate_static_stub(test, io_opaque_ubuf_flags, opaque_test_ubuf_no_waiver);
	skb = opaque_pool_skb(test, p);
	u = opaque_skb_ubuf(skb);
	KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(skb, &f.iter, 80000), 0);
	KUNIT_ASSERT_EQ(test, u->nr, 3);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 2);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 2);
	KUNIT_EXPECT_EQ(test, page_count(f.units[2]), 2);
	opaque_pool_read(&before);

	if (how == OPAQUE_SHARE_SPLIT) {
		twin = alloc_skb(0, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, twin);
		twin->ip_summed = CHECKSUM_PARTIAL;
		skb_split(skb, twin, 40000);
		KUNIT_EXPECT_EQ(test, refcount_read(&u->uarg.refcnt), 2U);
	} else {
		twin = skb_clone(skb, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, twin);
		/* A clone shares the shinfo, which holds the one count. */
		KUNIT_EXPECT_EQ(test, refcount_read(&u->uarg.refcnt), 1U);
		/* What the receive path does with it for a tap: hands it on, or copies it. */
		if (how != OPAQUE_SHARE_CLONE)
			KUNIT_EXPECT_EQ(test, skb_orphan_frags_rx(twin, GFP_KERNEL), 0);
	}
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.copied - before.copied, copied ? 1ULL : 0ULL);
	if (copied) {
		/* The tap has bytes of its own and no ubuf; the device's skb has the one count. */
		KUNIT_EXPECT_NULL(test, skb_zcopy(twin));
		KUNIT_EXPECT_EQ(test, refcount_read(&u->uarg.refcnt), 1U);
	} else {
		KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(twin), &u->uarg);
	}
	KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(skb), &u->uarg);

	/* A skb that wants a ubuf while this one is held gets another, from the cache. */
	other = opaque_pool_skb(test, p);
	KUNIT_EXPECT_PTR_NE(test, opaque_skb_ubuf(other), u);
	OPAQUE_POOL_HELD(test, p, 0, 0);

	/* The device's skb goes. The twin still holds the ubuf and its folios, unless it copied. */
	kfree_skb(skb);
	/* The ubuf of a tap that copied is the pool's by now, and poisoned under KASAN. */
	if (!copied || !IS_ENABLED(CONFIG_KASAN))
		KUNIT_EXPECT_EQ(test, u->nr, 3);
	if (copied) {
		OPAQUE_POOL_HELD(test, p, 1, 0);
		KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
	} else {
		OPAQUE_POOL_HELD(test, p, 0, 0);
		KUNIT_EXPECT_EQ(test, refcount_read(&u->uarg.refcnt), 1U);
		KUNIT_EXPECT_GE(test, page_count(f.units[0]), 2);
		KUNIT_EXPECT_GE(test, page_count(f.units[1]), 2);
		KUNIT_EXPECT_GE(test, page_count(f.units[2]), 2);
	}
	/* The last one goes and the ubuf is the pool's, once, with every page given back. */
	kfree_skb(twin);
	OPAQUE_POOL_HELD(test, p, 1, 0);
	if (!IS_ENABLED(CONFIG_KASAN))
		KUNIT_EXPECT_EQ(test, refcount_read(&u->uarg.refcnt), 0U);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
	KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 1);
	KUNIT_EXPECT_EQ(test, page_count(f.units[2]), 1);

	/* The next skb gets that ubuf, and the other is not disturbed. */
	next = opaque_pool_skb(test, p);
	KUNIT_EXPECT_PTR_EQ(test, opaque_skb_ubuf(next), u);
	KUNIT_EXPECT_EQ(test, refcount_read(&u->uarg.refcnt), 1U);
	KUNIT_EXPECT_EQ(test, u->nr, 0);
	OPAQUE_POOL_HELD(test, p, 0, 0);
	kfree_skb(next);
	kfree_skb(other);
	OPAQUE_POOL_HELD(test, p, 2, 0);
	if (copied)
		kunit_deactivate_static_stub(test, io_opaque_ubuf_flags);
	opaque_splice_done(&f);
}

static void opaque_ubuf_pool_share(struct kunit *test)
{
	unsigned int how;

	for (how = 0; how < OPAQUE_SHARE_NR; how++)
		opaque_ubuf_pool_share_one(test, how);
}

/*
 * The pool holds as many ubufs as it has room for, and refuses the rest, which the cache gets
 * back, so none is lost and the cache is asked only when the pool is empty. The send side takes
 * all that has completed at once into a stock that it hands out of, and what the pool holds is
 * the stock and the list, which may each be as long as the bound. Every skb in use has a ubuf of
 * its own: no two are the same, which a ubuf kept twice would not be.
 */
static void opaque_ubuf_pool_bound(struct kunit *test)
{
	const unsigned int max = IO_OPAQUE_UBUF_POOL_MAX, many = 2 * max + 5;
	struct sk_buff **skb = kunit_kcalloc(test, many, sizeof(*skb), GFP_KERNEL);
	struct opaque_pool *p = opaque_pool_new(test);
	struct opaque_pool_snap before, after;
	unsigned int i, j, done, stocked;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	opaque_pool_read(&before);
	for (i = 0; i < max + 7; i++)
		skb[i] = opaque_pool_skb(test, p);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live - before.live, (long)max + 7);
	KUNIT_EXPECT_EQ(test, refcount_read(&p->pool->refs), 1 + max + 7);
	for (i = 0; i < max + 7; i++)
		kfree_skb(skb[i]);
	/* The pool took the first of those that completed and the cache took the rest back. */
	OPAQUE_POOL_HELD(test, p, max, 0);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live - before.live, (long)max);
	KUNIT_EXPECT_EQ(test, refcount_read(&p->pool->refs), 1 + max);

	/* They are given out before the cache is asked, each to one skb. */
	for (i = 0; i < max; i++) {
		skb[i] = opaque_pool_skb(test, p);
		for (j = 0; j < i; j++)
			KUNIT_EXPECT_PTR_NE(test, opaque_skb_ubuf(skb[i]), opaque_skb_ubuf(skb[j]));
	}
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live - before.live, (long)max);
	KUNIT_EXPECT_EQ(test, after.reused - before.reused, (u64)max);
	OPAQUE_POOL_HELD(test, p, 0, 0);
	/* And the next is the cache's. */
	skb[max] = opaque_pool_skb(test, p);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live - before.live, (long)max + 1);
	for (i = 0; i <= max; i++)
		kfree_skb(skb[i]);
	OPAQUE_POOL_HELD(test, p, max, 0);

	/*
	 * The list full, one skb takes it all into the stock, and the skbs still in use complete
	 * onto the list again: the pool holds the stock and a full list, and no more.
	 */
	for (i = 0; i < many; i++)
		skb[i] = opaque_pool_skb(test, p);
	/* The first max came from the pool, the rest are the cache's. */
	for (i = 0; i < max; i++)
		kfree_skb(skb[i]);
	OPAQUE_POOL_HELD(test, p, max, 0);
	skb[0] = opaque_pool_skb(test, p);
	OPAQUE_POOL_HELD(test, p, 0, max - 1);
	for (i = max; i < many; i++)
		kfree_skb(skb[i]);
	done = opaque_pool_held(p->pool, &stocked);
	KUNIT_EXPECT_EQ(test, done, max);
	KUNIT_EXPECT_EQ(test, stocked, max - 1);
	kfree_skb(skb[0]);
	done = opaque_pool_held(p->pool, &stocked);
	KUNIT_EXPECT_LE(test, done, max);
	KUNIT_EXPECT_LE(test, done + stocked, 2 * max);
	KUNIT_EXPECT_EQ(test, refcount_read(&p->pool->refs), 1 + done + stocked);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live - before.live, (long)(done + stocked));
	opaque_pool_close(p);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live, before.live);
	KUNIT_EXPECT_EQ(test, after.pools, before.pools - 1);
}

/*
 * A store goes with skbs still holding its ubufs: it frees the ubufs the pool holds, those on the
 * list and those in the stock, refuses the ones that complete later, which the cache gets, and
 * the pool itself is freed with the last of them, whenever that is. The folios of the skbs
 * that outlive the store are put back as they always were.
 */
static void opaque_ubuf_pool_teardown(struct kunit *test)
{
	struct opaque_pool *p = opaque_pool_new(test);
	struct opaque_pool_snap before, after;
	struct sk_buff *skb[10], *held;
	struct opaque_splice f;
	struct iov_iter iter;
	unsigned int i;

	opaque_splice_setup(test, &f);
	opaque_pool_read(&before);
	for (i = 0; i < ARRAY_SIZE(skb); i++) {
		skb[i] = opaque_pool_skb(test, p);
		iov_iter_bvec(&iter, ITER_SOURCE, f.op.replacement->bvec, f.op.replacement->nr,
			      80000);
		KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(skb[i], &iter, 1000), 0);
	}
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1 + (int)ARRAY_SIZE(skb));
	/* Four complete and are the pool's, and a skb takes them out of the list into the stock. */
	for (i = 0; i < 4; i++)
		kfree_skb(skb[i]);
	OPAQUE_POOL_HELD(test, p, 4, 0);
	held = opaque_pool_skb(test, p);
	OPAQUE_POOL_HELD(test, p, 0, 3);
	/* Two more complete after that and are on the list. */
	kfree_skb(skb[4]);
	kfree_skb(skb[5]);
	OPAQUE_POOL_HELD(test, p, 2, 3);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live - before.live, (long)ARRAY_SIZE(skb));
	KUNIT_EXPECT_EQ(test, refcount_read(&p->pool->refs), 1U + ARRAY_SIZE(skb));
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1 + 4);

	/* The store goes: what the pool held is freed, which is five ubufs. */
	opaque_pool_close(p);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live - before.live, (long)ARRAY_SIZE(skb) - 5);
	KUNIT_EXPECT_EQ(test, after.pools, before.pools);
	KUNIT_EXPECT_EQ(test, refcount_read(&p->pool->refs), 5U);
	OPAQUE_POOL_HELD(test, p, 0, 0);
	/* The pool refuses what completes now, and its folios go back as before. */
	for (i = 6; i < ARRAY_SIZE(skb); i++) {
		kfree_skb(skb[i]);
		OPAQUE_POOL_HELD(test, p, 0, 0);
		KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1 + (int)(ARRAY_SIZE(skb) - 1 - i));
	}
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live - before.live, 1L);
	KUNIT_EXPECT_EQ(test, after.pools, before.pools);
	KUNIT_EXPECT_EQ(test, refcount_read(&p->pool->refs), 1U);
	/* The ubuf of the last skb is the pool's last reference, and goes with it. */
	kfree_skb(held);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live, before.live);
	KUNIT_EXPECT_EQ(test, after.pools, before.pools - 1);
	KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
	opaque_splice_done(&f);
}

/*
 * A real store has a pool from the moment it is made, and closes it when its last reference goes:
 * a ubuf that completed is freed with it, one that an skb still holds is freed when that skb is,
 * and the pool, which the store's going does not free while it has one, goes with the last.
 */
static void opaque_ubuf_pool_store(struct kunit *test)
{
	struct io_ring_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct io_uring_opaque_config cfg = {
		.hard_limit = 64 * PAGE_SIZE, .compact_headroom = 4 * OPAQUE_TEST_UNIT,
		.max_object_size = SZ_64K, .max_objects = 4, .max_streams = 1,
		.max_extents = 64,
		/* Ignored: any value registers, which earlier versions of the interface refused. */
		.max_requests = U32_MAX,
	};
	struct io_opaque_msg_ubuf mub = opaque_new_msg_ubuf();
	struct opaque_pool_snap before, after;
	struct sk_buff *skb[3];
	struct io_opaque_store *store;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	opaque_test_params_save(test);
	io_opaque_cache_bytes = 0;
	ctx->user = get_current_user();
	opaque_pool_read(&before);
	store = io_opaque_alloc(ctx, &cfg);
	KUNIT_ASSERT_FALSE(test, IS_ERR(store));
	KUNIT_ASSERT_NOT_NULL(test, store->ubuf_pool);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.pools - before.pools, 1L);
	/* The msg_ubuf of a send of this store, as io_opaque_send() sets it. */
	mub.pool = READ_ONCE(store->ubuf_pool);
	for (i = 0; i < ARRAY_SIZE(skb); i++) {
		skb[i] = alloc_skb(0, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, skb[i]);
		KUNIT_ASSERT_EQ(test, io_opaque_link_skb(skb[i], &mub.uarg), 0);
	}
	/* One of three completes and is the pool's; the store goes with the other two. */
	kfree_skb(skb[0]);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live - before.live, 3L);
	io_opaque_put(store);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live - before.live, 2L);
	KUNIT_EXPECT_EQ(test, after.pools - before.pools, 1L);
	kfree_skb(skb[1]);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live - before.live, 1L);
	kfree_skb(skb[2]);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live, before.live);
	KUNIT_EXPECT_EQ(test, after.pools, before.pools);
	free_uid(ctx->user);
}

/*
 * The ubuf of a pool completes where the last skb is freed, and that is often a softirq: the ACK
 * that frees a sent skb, a consumer that copies. It is put on the list there and taken by the
 * next skb in the task, with only atomics in between.
 */
static void opaque_ubuf_pool_atomic(struct kunit *test)
{
	struct opaque_atomic *a = kunit_kzalloc(test, sizeof(*a), GFP_KERNEL);
	unsigned int copy;

	KUNIT_ASSERT_NOT_NULL(test, a);
	for (copy = 0; copy < 2; copy++) {
		struct opaque_pool *p = opaque_pool_new(test);
		struct opaque_pool_snap before, after;
		struct io_opaque_ubuf *u;
		struct opaque_splice f;
		struct sk_buff *skb;

		opaque_splice_setup(test, &f);
		skb = opaque_pool_skb(test, p);
		u = opaque_skb_ubuf(skb);
		KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(skb, &f.iter, 80000), 0);
		a->skb = skb;
		a->copy = copy;
		a->softirq = false;
		a->ret = -1;
		init_completion(&a->done);
		tasklet_setup(&a->tasklet, opaque_atomic_fn);
		opaque_pool_read(&before);
		tasklet_schedule(&a->tasklet);
		KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&a->done, 5 * HZ));
		tasklet_kill(&a->tasklet);
		opaque_pool_read(&after);
		KUNIT_EXPECT_TRUE(test, a->softirq);
		KUNIT_EXPECT_EQ(test, a->ret, copy ? 0 : -1);
		KUNIT_EXPECT_EQ(test, after.copied - before.copied, (u64)copy);
		KUNIT_EXPECT_EQ(test, page_count(f.units[0]), 1);
		KUNIT_EXPECT_EQ(test, page_count(f.units[1]), 1);
		KUNIT_EXPECT_EQ(test, page_count(f.units[2]), 1);
		OPAQUE_POOL_HELD(test, p, 1, 0);
		skb = opaque_pool_skb(test, p);
		KUNIT_EXPECT_PTR_EQ(test, opaque_skb_ubuf(skb), u);
		kfree_skb(skb);
		opaque_splice_done(&f);
	}
}

/*
 * Over TCP: the sends of a store that has a pool take their ubufs from it. The ACKs free the skbs
 * and the ubufs go to the pool, in whatever context they come, and the next sends have them: the
 * cache makes only the ubufs that the sends could not take, and the store's going gives the
 * rest back.
 */
static void opaque_send_managed_pool(struct kunit *test)
{
	static const struct opaque_send_case cases[] = {
		{ .unlocked = true },
		{ },
		{ .vector = true },
		{ .prefix = 24, .suffix = 8 },
		{ .vector = true, .last = true },
		{ .prefix = 24, .suffix = 8, .prep = true },
	};
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct opaque_pool *p = opaque_pool_new(test);
	struct opaque_pool_snap before, after;
	unsigned int round, i, tries, done, stocked;

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_test_params_save(test);
	io_opaque_send_managed = true;
	store->ubuf_pool = p->pool;
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	opaque_pool_read(&before);
	for (round = 0; round < 3; round++) {
		for (i = 0; i < ARRAY_SIZE(cases); i++) {
			opaque_wire_send(test, w, store, &cases[i]);
			opaque_wire_acked(w->tx->sk);
		}
	}
	/* The skbs are freed as the ACKs and the reads come; then every ubuf is in the pool. */
	for (tries = 0; tries < 100; tries++) {
		done = opaque_pool_held(p->pool, &stocked);
		opaque_pool_read(&after);
		if (after.live - before.live == (long)(done + stocked))
			break;
		msleep(20);
	}
	KUNIT_EXPECT_EQ(test, after.live - before.live, (long)(done + stocked));
	KUNIT_EXPECT_GT(test, after.reused - before.reused, 0ULL);
	/* Every skb had its ubuf from the pool or from the cache, and none was refused. */
	KUNIT_EXPECT_EQ(test, (u64)(after.live - before.live) + after.reused - before.reused,
			after.skbs - before.skbs);
	KUNIT_EXPECT_EQ(test, after.copied, before.copied);
	store->ubuf_pool = NULL;
	opaque_pool_close(p);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live, before.live);
	KUNIT_EXPECT_EQ(test, after.pools, before.pools - 1);
}

/*
 * Which skbs say that their fragments may change. The stack takes SKBFL_SHARED_FRAG to mean
 * that someone else may write them while the skb is queued, and copies them before it sums
 * them in software or changes them in place. The pages of a store that binds a MAIO instance
 * are the area's, which the process that maps it can write, and so are those that a store
 * without an instance has kept from a queue that an instance owns. A body that was spliced
 * from them was marked by TCP. So the skbs of a managed send of such a store are marked, and
 * so is every skb that is made from one. The pages of an ordinary store cannot change, and
 * its skbs are not marked. The receive path copies nothing for either: the holder of a clone,
 * as a tap is, is left with it.
 */
#define OPAQUE_SHARED_HDR	8
#define OPAQUE_SHARED_PAGES	6
#define OPAQUE_SHARED_MSS	3000

/*
 * Whether the skbs of the managed sends of @store are to be marked: it binds an instance, or holds
 * a page of one. Not asked of io_opaque_send_ubuf_flags(), which the tests are to hold to it.
 */
static bool opaque_store_marked(const struct io_opaque_store *store)
{
	return store->maio || store->maio_pages;
}

/*
 * An skb of a managed send of @store, as TCP has the send's fill leave it. The msg_ubuf is set
 * from the store, link_skb() gives the skb its ubuf, and the fill adds the segments of @segs.
 * There is a header in the linear part and a gso_size, for skb_segment().
 */
static struct sk_buff *opaque_shared_skb(struct kunit *test, struct io_opaque_store *store,
					 const struct opaque_segs *segs)
{
	struct io_opaque_msg_ubuf mub = opaque_new_msg_ubuf();
	struct sk_buff *skb = alloc_skb(OPAQUE_SHARED_HDR, GFP_KERNEL);
	struct iov_iter iter;
	size_t len = 0;
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	__skb_put(skb, OPAQUE_SHARED_HDR);
	skb_reset_mac_header(skb);
	memset(skb_mac_header(skb), 0xa5, OPAQUE_SHARED_HDR);
	skb_pull(skb, OPAQUE_SHARED_HDR);
	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);
	skb->protocol = htons(ETH_P_ATALK);
	skb->ip_summed = CHECKSUM_PARTIAL;
	skb_shinfo(skb)->gso_size = OPAQUE_SHARED_MSS;
	mub.uarg.flags = io_opaque_send_ubuf_flags(store);
	KUNIT_ASSERT_EQ(test, io_opaque_link_skb(skb, &mub.uarg), 0);
	for (i = 0; i < segs->nr; i++)
		len += segs->bv[i].bv_len;
	iov_iter_bvec(&iter, ITER_SOURCE, segs->bv, segs->nr, len);
	KUNIT_ASSERT_EQ(test, io_opaque_sg_from_iter(skb, &iter, len), 0);
	KUNIT_ASSERT_EQ(test, skb->len, (unsigned int)len);
	return skb;
}

/*
 * The skb of a send of @store, and the skbs that the stack derives from it: a clone, which a tap
 * holds; a copy of the head, as a retransmission of a cloned skb makes; the second half of a
 * split, as TCP cuts an skb at the window; an extract of a range; and the segments, with the
 * checksum offloaded and without, where the stack copies the fragments of an skb that is marked
 * before it sums them.
 */
static void opaque_shared_frag_one(struct kunit *test, struct io_opaque_store *store)
{
	const bool marked = opaque_store_marked(store);
	const u8 flags = SKBFL_ZEROCOPY_ENABLE | SKBFL_DONT_ORPHAN | SKBFL_DONT_ORPHAN_RX |
			 (marked ? SKBFL_SHARED_FRAG : 0);
	struct opaque_segs *segs = opaque_segs_new(test, OPAQUE_SHARED_PAGES, 2000);
	struct sk_buff *skb, *clone, *copy, *twin, *ext, *list, *seg;
	struct opaque_pool_snap before, mid, after;
	struct page *prefix = alloc_page(GFP_KERNEL);
	struct io_opaque_msg_ubuf mub = opaque_new_msg_ubuf();
	struct iov_iter iter;
	unsigned int n, i;
	u64 copies;

	KUNIT_ASSERT_NOT_NULL(test, prefix);
	opaque_pool_read(&before);

	/* The ubuf has the flags, and so has the skb linked to it, whose shinfo adds one. */
	skb = opaque_shared_skb(test, store, segs);
	KUNIT_EXPECT_EQ(test, skb_zcopy(skb)->flags, flags);
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->flags & SKBFL_ALL_ZEROCOPY,
			flags | SKBFL_MANAGED_FRAG_REFS);
	KUNIT_EXPECT_EQ(test, skb_has_shared_frag(skb), marked);

	/* A clone shares the shinfo. The receive path leaves it to its holder, copying nothing. */
	clone = skb_clone(skb, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, clone);
	KUNIT_EXPECT_EQ(test, skb_has_shared_frag(clone), marked);
	KUNIT_EXPECT_EQ(test, skb_orphan_frags_rx(clone, GFP_KERNEL), 0);
	KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(clone), skb_zcopy(skb));
	KUNIT_EXPECT_EQ(test, skb_has_shared_frag(clone), marked);

	/* A copy of the head has a shinfo of its own, with references of its own on the pages. */
	copy = pskb_copy(skb, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	KUNIT_EXPECT_EQ(test, skb_has_shared_frag(copy), marked);
	KUNIT_EXPECT_EQ(test, skb_shinfo(copy)->flags & SKBFL_ALL_ZEROCOPY, flags);
	consume_skb(copy);
	consume_skb(clone);
	kfree_skb(skb);

	/* A split cuts inside a fragment, and each half is marked. */
	skb = opaque_shared_skb(test, store, segs);
	twin = alloc_skb(0, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, twin);
	twin->ip_summed = CHECKSUM_PARTIAL;
	skb_split(skb, twin, 7000);
	KUNIT_EXPECT_EQ(test, skb->len, 7000U);
	KUNIT_EXPECT_EQ(test, twin->len, 5000U);
	KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(twin), skb_zcopy(skb));
	KUNIT_EXPECT_EQ(test, skb_has_shared_frag(skb), marked);
	KUNIT_EXPECT_EQ(test, skb_has_shared_frag(twin), marked);
	KUNIT_EXPECT_EQ(test, skb_shinfo(twin)->flags & SKBFL_ALL_ZEROCOPY, flags);
	kfree_skb(twin);
	kfree_skb(skb);

	/* An extract carves a clone, which gets a shinfo that is a copy of the shared one. */
	skb = opaque_shared_skb(test, store, segs);
	ext = pskb_extract(skb, 4000, 5000, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ext);
	KUNIT_EXPECT_EQ(test, ext->len, 5000U);
	KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(ext), skb_zcopy(skb));
	KUNIT_EXPECT_EQ(test, skb_has_shared_frag(ext), marked);
	KUNIT_EXPECT_EQ(test, skb_shinfo(ext)->flags & SKBFL_ALL_ZEROCOPY, flags);
	kfree_skb(ext);
	kfree_skb(skb);

	/* Segments of a device that sums them keep the fragments, and the mark. */
	skb = opaque_shared_skb(test, store, segs);
	list = skb_segment(skb, NETIF_F_SG | NETIF_F_HW_CSUM);
	KUNIT_ASSERT_FALSE(test, IS_ERR_OR_NULL(list));
	for (seg = list, n = 0; seg; seg = seg->next, n++) {
		KUNIT_EXPECT_TRUE(test, skb_is_nonlinear(seg));
		KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(seg), skb_zcopy(skb));
		KUNIT_EXPECT_EQ(test, skb_has_shared_frag(seg), marked);
		KUNIT_EXPECT_EQ(test, skb_shinfo(seg)->flags & SKBFL_ALL_ZEROCOPY, flags);
	}
	KUNIT_EXPECT_EQ(test, n, 4U);
	kfree_skb_list(list);
	kfree_skb(skb);
	/* Nothing has copied the data yet: not the tap, the copy, the split or the segments. */
	opaque_pool_read(&mid);
	KUNIT_EXPECT_EQ(test, mid.copied, before.copied);

	/*
	 * Where the stack sums in software it copies the fragments of a marked skb first: the
	 * segments of a marked store's skb are linear, and an ordinary store's keep the pages.
	 * A segment with no bytes left in fragments lets go of its ubuf as a copy
	 * (__pskb_pull_tail() says so), which is what the counter of copies sees.
	 */
	skb = opaque_shared_skb(test, store, segs);
	list = skb_segment(skb, NETIF_F_SG);
	KUNIT_ASSERT_FALSE(test, IS_ERR_OR_NULL(list));
	for (seg = list, n = 0; seg; seg = seg->next, n++) {
		KUNIT_EXPECT_EQ(test, skb_is_nonlinear(seg), !marked);
		KUNIT_EXPECT_EQ(test, seg->len, OPAQUE_SHARED_HDR + 3000U);
	}
	KUNIT_EXPECT_EQ(test, n, 4U);
	kfree_skb_list(list);
	kfree_skb(skb);
	opaque_pool_read(&after);
	copies = after.copied - mid.copied;
	KUNIT_EXPECT_EQ(test, copies, marked ? (u64)n : 0ULL);

	/*
	 * The skb that has fragments of its own, a framed send's prefix, takes the body as a splice
	 * does, with a page reference for each fragment, and is marked the same.
	 */
	skb = alloc_skb(0, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_add_rx_frag(skb, 0, prefix, 0, 24, 24);
	mub.uarg.flags = io_opaque_send_ubuf_flags(store);
	KUNIT_ASSERT_EQ(test, io_opaque_link_skb(skb, &mub.uarg), 0);
	iov_iter_bvec(&iter, ITER_SOURCE, segs->bv, segs->nr, 2 * 2000);
	KUNIT_EXPECT_EQ(test, io_opaque_sg_from_iter(skb, &iter, 2 * 2000), 0);
	KUNIT_EXPECT_FALSE(test, skb_zcopy_managed(skb));
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, 3);
	KUNIT_EXPECT_EQ(test, skb_has_shared_frag(skb), marked);
	KUNIT_EXPECT_EQ(test, page_count(segs->bv[0].bv_page), 2);
	kfree_skb(skb);

	/* The fallback copied nothing, and every ubuf and page is given back. */
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live, before.live);
	KUNIT_EXPECT_EQ(test, after.copied - mid.copied, copies);
	for (i = 0; i < segs->nr; i++)
		KUNIT_EXPECT_EQ(test, page_count(segs->bv[i].bv_page), 1);
}

static void opaque_managed_shared_frag(struct kunit *test)
{
	opaque_shared_frag_one(test, opaque_test_store(test));
}

#define OPAQUE_SHARED_CSUM_AT	6

/*
 * The skb of opaque_shared_skb() as a single segment for a device without checksum offload: the
 * header stays in the head, with the field of the sum at the end of it, and the sum is left to
 * skb_checksum_help().
 */
static struct sk_buff *opaque_shared_csum_skb(struct kunit *test, struct io_opaque_store *store,
					      const struct opaque_segs *segs)
{
	struct sk_buff *skb = opaque_shared_skb(test, store, segs);

	skb_push(skb, OPAQUE_SHARED_HDR);
	skb_shinfo(skb)->gso_size = 0;
	skb->csum_start = skb_headroom(skb);
	skb->csum_offset = OPAQUE_SHARED_CSUM_AT;
	memset(skb->data + OPAQUE_SHARED_CSUM_AT, 0, sizeof(__sum16));
	return skb;
}

/*
 * What TCP hands to a device that cannot sum a segment: a clone of the skb in its write queue, and
 * a copy of its head if the skb is cloned already, as for a retransmission while the first clone is
 * still below. skb_checksum_help() sums it there. For a marked skb it copies the fragments into the
 * head of the clone first, so that the sum is that of the bytes that go out, and a write to the
 * pages after that cannot make it wrong. The skb in the queue keeps its fragments and its ubuf
 * for the next transmission, and every reference taken on the way is given back, in whichever
 * order the skbs go.
 */
static void opaque_shared_csum_one(struct kunit *test, struct io_opaque_store *store)
{
	const bool marked = opaque_store_marked(store);
	const u32 len = OPAQUE_SHARED_HDR + OPAQUE_SHARED_PAGES * 2000;
	struct opaque_segs *segs = opaque_segs_new(test, OPAQUE_SHARED_PAGES, 2000);
	u8 *want = kunit_kmalloc(test, len, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, len, GFP_KERNEL);
	struct opaque_pool_snap before, after;
	struct sk_buff *skb, *derived[2];
	struct ubuf_info *uarg;
	unsigned int order, i, j;
	__sum16 sum;
	void *mapped;

	KUNIT_ASSERT_NOT_NULL(test, want);
	KUNIT_ASSERT_NOT_NULL(test, got);
	for (order = 0; order < 2; order++) {
		opaque_pool_read(&before);
		/* Every byte of a page differs from its neighbours, and a pass from the last. */
		for (i = 0; i < segs->nr; i++) {
			mapped = kmap_local_page(segs->bv[i].bv_page);
			for (j = 0; j < 2000; j++)
				((u8 *)mapped)[j] = i * 31 + j * 7 + order;
			kunmap_local(mapped);
		}
		skb = opaque_shared_csum_skb(test, store, segs);
		KUNIT_ASSERT_EQ(test, skb->len, len);
		uarg = skb_zcopy(skb);
		KUNIT_ASSERT_NOT_NULL(test, uarg);

		/* What should go out: the packet, with the sum of it in its field. */
		KUNIT_ASSERT_EQ(test, skb_copy_bits(skb, 0, want, len), 0);
		sum = csum_fold(csum_partial(want, len, 0)) ?: CSUM_MANGLED_0;
		memcpy(want + OPAQUE_SHARED_CSUM_AT, &sum, sizeof(sum));

		derived[0] = skb_clone(skb, GFP_KERNEL);
		derived[1] = pskb_copy(skb, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, derived[0]);
		KUNIT_ASSERT_NOT_NULL(test, derived[1]);
		for (i = 0; i < 2; i++) {
			struct sk_buff *d = derived[i];

			KUNIT_EXPECT_EQ(test, skb_checksum_help(d), 0);
			KUNIT_EXPECT_TRUE(test, d->ip_summed == CHECKSUM_NONE);
			KUNIT_ASSERT_EQ(test, d->len, len);
			KUNIT_ASSERT_EQ(test, skb_copy_bits(d, 0, got, len), 0);
			KUNIT_EXPECT_MEMEQ_MSG(test, got, want, len, "skb %u", i);
			/* A marked skb is copied into its head, and lets go of its ubuf. */
			KUNIT_EXPECT_EQ(test, skb_is_nonlinear(d), !marked);
			KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(d), marked ? NULL : uarg);
		}

		/* The skb in the queue is as it was, and the ubuf has a reference per holder. */
		KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->nr_frags, OPAQUE_SHARED_PAGES);
		KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(skb), uarg);
		KUNIT_EXPECT_EQ(test, skb_has_shared_frag(skb), marked);
		KUNIT_EXPECT_EQ(test, refcount_read(&uarg->refcnt), marked ? 1U : 3U);

		if (marked) {
			/* The process writes the pages after the sum. The copy is not hit. */
			for (i = 0; i < segs->nr; i++) {
				mapped = kmap_local_page(segs->bv[i].bv_page);
				memset(mapped, 0xee, 2000);
				kunmap_local(mapped);
			}
			for (i = 0; i < 2; i++) {
				KUNIT_ASSERT_EQ(test, skb_copy_bits(derived[i], 0, got, len), 0);
				KUNIT_EXPECT_MEMEQ_MSG(test, got, want, len, "skb %u", i);
				/* The sum of a packet with its own sum in it is zero. */
				sum = csum_fold(csum_partial(got, len, 0));
				KUNIT_EXPECT_EQ(test, (__force u16)sum, (u16)0);
			}
		}

		if (order) {
			kfree_skb(skb);
			consume_skb(derived[0]);
			consume_skb(derived[1]);
		} else {
			consume_skb(derived[0]);
			consume_skb(derived[1]);
			kfree_skb(skb);
		}
		/* Balanced: ubufs and pages are back, and the counter saw a copy for each skb. */
		opaque_pool_read(&after);
		KUNIT_EXPECT_EQ(test, after.live, before.live);
		KUNIT_EXPECT_EQ(test, after.copied - before.copied, marked ? 2ULL : 0ULL);
		for (i = 0; i < segs->nr; i++)
			KUNIT_EXPECT_EQ_MSG(test, page_count(segs->bv[i].bv_page), 1,
					    "order %u page %u", order, i);
	}
}

static void opaque_managed_shared_csum(struct kunit *test)
{
	opaque_shared_csum_one(test, opaque_test_store(test));
}

/*
 * A send joins the skb at the tail of a socket's queue only if it would have given it the flags it
 * has. Two stores can send on one socket: were a bound store's fragments added to an skb that an
 * ordinary store began they would not be marked, and were an ordinary store's added to a bound
 * store's they would be marked for nothing. -EEXIST makes TCP start another skb, as it does for
 * another provider's. A ubuf that the pool kept is made whole again, with the flags of the send
 * that has it now and not of the one that had it last.
 */
static void opaque_managed_link_binding(struct kunit *test)
{
	struct io_opaque_msg_ubuf ordinary = opaque_new_msg_ubuf(), bound = opaque_new_msg_ubuf();
	struct io_opaque_msg_ubuf *send[2] = { &ordinary, &bound }, *first, *other;
	struct opaque_pool *p = opaque_pool_new(test);
	struct io_opaque_ubuf *u;
	struct ubuf_info *uarg;
	struct sk_buff *skb;
	unsigned int i;

	bound.uarg.flags = SKBFL_SHARED_FRAG;
	for (i = 0; i < ARRAY_SIZE(send); i++) {
		first = send[i];
		other = send[!i];
		skb = alloc_skb(0, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, skb);
		KUNIT_ASSERT_EQ(test, io_opaque_link_skb(skb, &first->uarg), 0);
		uarg = skb_zcopy(skb);
		KUNIT_ASSERT_NOT_NULL(test, uarg);
		/* The same flags join the skb, whatever else it holds. */
		KUNIT_EXPECT_EQ(test, io_opaque_link_skb(skb, &first->uarg), 0);
		KUNIT_EXPECT_EQ(test, io_opaque_link_skb(skb, NULL), i ? -EEXIST : 0);
		/* The others do not, and change nothing of it. */
		KUNIT_EXPECT_EQ(test, io_opaque_link_skb(skb, &other->uarg), -EEXIST);
		KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(skb), uarg);
		KUNIT_EXPECT_EQ(test, refcount_read(&uarg->refcnt), 1U);
		KUNIT_EXPECT_EQ(test, !!(uarg->flags & SKBFL_SHARED_FRAG), !!first->uarg.flags);
		KUNIT_EXPECT_EQ(test, !!(skb_shinfo(skb)->flags & SKBFL_SHARED_FRAG),
				!!first->uarg.flags);
		kfree_skb(skb);
	}

	p->mub.uarg.flags = SKBFL_SHARED_FRAG;
	skb = opaque_pool_skb(test, p);
	u = opaque_skb_ubuf(skb);
	KUNIT_EXPECT_EQ(test, u->uarg.flags, io_opaque_ubuf_flags() | SKBFL_SHARED_FRAG);
	kfree_skb(skb);
	/* The ubuf is the pool's now, and the next skb has it, for a send that adds no flag. */
	p->mub.uarg.flags = 0;
	skb = opaque_pool_skb(test, p);
	KUNIT_EXPECT_PTR_EQ(test, opaque_skb_ubuf(skb), u);
	KUNIT_EXPECT_EQ(test, u->uarg.flags, io_opaque_ubuf_flags());
	KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->flags & SKBFL_SHARED_FRAG, 0);
	kfree_skb(skb);
	/* And again for a send that does, which takes it from the pool too. */
	p->mub.uarg.flags = SKBFL_SHARED_FRAG;
	skb = opaque_pool_skb(test, p);
	KUNIT_EXPECT_PTR_EQ(test, opaque_skb_ubuf(skb), u);
	KUNIT_EXPECT_EQ(test, u->uarg.flags, io_opaque_ubuf_flags() | SKBFL_SHARED_FRAG);
	kfree_skb(skb);
}

/*
 * The skbs in the queue of the peer, which are the clones of what the send made, as a tap on
 * the way would hold them: those that have fragments, how many of them say that the fragments may
 * change, and how many have a ubuf of ours.
 */
static void opaque_wire_queue_frags(struct opaque_wire *w, unsigned int *skbs,
				    unsigned int *shared, unsigned int *ours)
{
	struct sk_buff_head *queue = &w->rx->sk->sk_receive_queue;
	struct sk_buff *skb;

	*skbs = *shared = *ours = 0;
	spin_lock_bh(&queue->lock);
	skb_queue_walk(queue, skb) {
		struct ubuf_info *uarg = skb_zcopy(skb);

		if (!skb_shinfo(skb)->nr_frags)
			continue;
		(*skbs)++;
		*shared += skb_has_shared_frag(skb);
		*ours += uarg && uarg->ops == &io_opaque_ubuf_ops;
	}
	spin_unlock_bh(&queue->lock);
}

/*
 * Over TCP on loopback, sends of @store through the owned path: four with managed fragments, so
 * that the store's pool has ubufs to give to the later ones, and one spliced. The peer's queue
 * holds the skbs, as a tap on the way would: those of a marked store, bound or not, are marked,
 * and those of an ordinary store are not. A spliced body is marked by TCP whichever store it is
 * from. The receive path copies nothing, and the bytes are the object's.
 */
static void opaque_wire_shared_frag(struct kunit *test, struct io_opaque_store *store)
{
	const bool marked = opaque_store_marked(store);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct opaque_pool *p = opaque_pool_new(test);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 40000, GFP_KERNEL);
	struct opaque_pool_snap before, after;
	unsigned int round;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_params_save(test);
	io_opaque_send_copy_max = 0;
	store->ubuf_pool = p->pool;
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	opaque_pool_read(&before);

	for (round = 0; round < 5; round++) {
		bool managed = round < 4;
		unsigned int skbs, shared, ours;

		io_opaque_send_managed = managed;
		opaque_managed_send(test, w, store, 4196, 40000, 0);
		opaque_wire_acked(w->tx->sk);
		opaque_wire_queue_frags(w, &skbs, &shared, &ours);
		KUNIT_EXPECT_GE_MSG(test, skbs, 1U, "round %u", round);
		if (managed) {
			KUNIT_EXPECT_EQ_MSG(test, ours, skbs, "round %u", round);
			KUNIT_EXPECT_EQ_MSG(test, shared, marked ? skbs : 0U, "round %u", round);
		} else {
			KUNIT_EXPECT_EQ_MSG(test, ours, 0U, "round %u", round);
			KUNIT_EXPECT_EQ_MSG(test, shared, skbs, "round %u", round);
		}
		opaque_wire_read(test, w, got, 0, 40000);
		KUNIT_EXPECT_MEMEQ_MSG(test, got, source + 4196, 40000, "round %u", round);
	}
	opaque_pool_read(&after);
	KUNIT_EXPECT_GT(test, after.skbs, before.skbs);
	KUNIT_EXPECT_GT(test, after.reused, before.reused);
	KUNIT_EXPECT_EQ(test, after.copied, before.copied);
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	store->ubuf_pool = NULL;
}

static void opaque_send_managed_shared_frag(struct kunit *test)
{
	opaque_wire_shared_frag(test, opaque_test_store(test));
}

/*
 * The skbs in the queue of the peer that have fragments and no ubuf of ours, and how many of them
 * say that the fragments may change. A managed send leaves none: they are the skbs that TCP made
 * of its own, as the probe of the path MTU is.
 */
static void opaque_wire_queue_bare(struct opaque_wire *w, unsigned int *skbs,
				   unsigned int *shared)
{
	struct sk_buff_head *queue = &w->rx->sk->sk_receive_queue;
	struct sk_buff *skb;

	*skbs = *shared = 0;
	spin_lock_bh(&queue->lock);
	skb_queue_walk(queue, skb) {
		if (!skb_shinfo(skb)->nr_frags || skb_zcopy(skb))
			continue;
		(*skbs)++;
		*shared += skb_has_shared_frag(skb);
	}
	spin_unlock_bh(&queue->lock);
}

static void opaque_probing_restore(void *arg)
{
	WRITE_ONCE(init_net.ipv4.sysctl_tcp_mtu_probing, (unsigned long)arg);
}

/*
 * The probe of the path MTU. With probing on, a connection begins with a small MSS, and when its
 * window of congestion has grown and it has more queued than a probe needs, TCP takes the payload
 * of the skbs at the head of its write queue into one skb of fragments that point where theirs
 * do (tcp_clone_payload()). That skb has a page reference of its own for each, no ubuf, and the
 * mark of theirs: were the mark lost on the way, the pages of a marked store would be summed in
 * software as the pages of an ordinary one are. The first send of the connection grows the window,
 * a later one finds a probe due. The peer's queue holds the probe, as a tap on the way would; the
 * bytes are the object's, and every page reference is given back when the peer has read and the
 * skbs of the sender are freed (opaque_wire_settle()).
 */
static void opaque_wire_probe(struct kunit *test, struct io_opaque_store *store)
{
	const bool marked = opaque_store_marked(store);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 40000, GFP_KERNEL);
	unsigned int round, skbs, shared, probes = 0, marks = 0;
	struct opaque_pool_snap before, after;
	struct io_opaque_extent *extent;
	struct tcp_sock *tp;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_params_save(test);
	io_opaque_send_copy_max = 0;
	io_opaque_send_managed = true;
	/* tcp_mtup_init() reads the setting when the connection is made. */
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_probing_restore,
			(void *)(unsigned long)READ_ONCE(init_net.ipv4.sysctl_tcp_mtu_probing)), 0);
	WRITE_ONCE(init_net.ipv4.sysctl_tcp_mtu_probing, 2);
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	KUNIT_ASSERT_TRUE(test, inet_csk(w->tx->sk)->icsk_mtup.enabled);
	tp = tcp_sk(w->tx->sk);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	opaque_pool_read(&before);

	for (round = 0; round < 4 && !probes; round++) {
		opaque_managed_send(test, w, store, 4196, 40000, 0);
		opaque_wire_acked(w->tx->sk);
		opaque_wire_queue_bare(w, &skbs, &shared);
		probes += skbs;
		marks += shared;
		opaque_wire_read(test, w, got, 0, 40000);
		KUNIT_EXPECT_MEMEQ_MSG(test, got, source + 4196, 40000, "round %u", round);
	}
	KUNIT_EXPECT_GE_MSG(test, probes, 1U, "no probe in %u rounds, window %u", round,
			    tcp_snd_cwnd(tp));
	KUNIT_EXPECT_NE(test, tp->mtu_probe.probe_seq_end, 0U);
	KUNIT_EXPECT_EQ(test, marks, marked ? probes : 0U);

	/*
	 * The peer has read everything, and the sender has everything acknowledged. The skbs go a
	 * moment after that, and with them the page references and the ubufs.
	 */
	opaque_wire_acked(w->tx->sk);
	opaque_wire_settle(data, before.live);
	list_for_each_entry(extent, &data->extents, list)
		KUNIT_EXPECT_EQ(test, page_count(extent->backing->page), 1);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live, before.live);
	opaque_test_free(store, 0);
}

static void opaque_send_managed_probe(struct kunit *test)
{
	opaque_wire_probe(test, opaque_test_store(test));
}

#ifdef CONFIG_IO_URING_ZCRX_KUNIT_TEST
/* The same for a store that binds an instance: see opaque_maio_store(). */
static void opaque_maio_managed_shared_frag(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);

	opaque_shared_frag_one(test, opaque_maio_store(test, ifq));
}

static void opaque_maio_send_managed_shared_frag(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);

	opaque_wire_shared_frag(test, opaque_maio_store(test, ifq));
}

/*
 * A store that binds an instance and one that does not send on one socket, corked so that
 * their bodies would share the skb at the tail of the queue. They do not: the peer's queue has
 * an skb for each send, and only the bound store's is marked. Each send finds the skb of the one
 * before it with the other flags and starts another.
 */
static void opaque_maio_send_managed_two_stores(struct kunit *test)
{
	static const struct { bool bound; u64 off; } sends[] = {
		{ false, 0 }, { true, 10000 }, { false, 20000 },
	};
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);
	struct io_opaque_store *store[2] = {
		opaque_test_store(test), opaque_maio_store(test, ifq),
	};
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 6000, GFP_KERNEL);
	unsigned int i, skbs, shared, ours;
	struct opaque_managed before, after;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_params_save(test);
	io_opaque_send_managed = true;
	io_opaque_send_copy_max = 0;
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	for (i = 0; i < ARRAY_SIZE(store); i++)
		opaque_test_publish(store[i], 0, 9, opaque_sparse_data(test, store[i]));
	opaque_test_copy(opaque_slot_data(store[0], 0), source);

	opaque_managed_read(&before);
	for (i = 0; i < ARRAY_SIZE(sends); i++)
		opaque_managed_send(test, w, store[sends[i].bound], sends[i].off, 6000,
				    i + 1 < ARRAY_SIZE(sends) ? MSG_MORE : 0);
	opaque_wire_acked(w->tx->sk);
	opaque_managed_read(&after);
	opaque_wire_queue_frags(w, &skbs, &shared, &ours);
	KUNIT_EXPECT_EQ(test, skbs, 3U);
	KUNIT_EXPECT_EQ(test, ours, 3U);
	KUNIT_EXPECT_EQ(test, shared, 1U);
	KUNIT_EXPECT_EQ(test, after.skbs - before.skbs, 3ULL);
	KUNIT_EXPECT_EQ(test, after.copied, before.copied);
	for (i = 0; i < ARRAY_SIZE(sends); i++) {
		opaque_wire_read(test, w, got, 0, 6000);
		KUNIT_EXPECT_MEMEQ_MSG(test, got, source + sends[i].off, 6000, "send %u", i);
	}
	for (i = 0; i < ARRAY_SIZE(store); i++) {
		opaque_test_free(store[i], 0);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store[i]->bytes), 0LL);
	}
}

/* The same for a bound store: its skbs are summed in software as a copy, and are not hit. */
static void opaque_maio_managed_shared_csum(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);

	opaque_shared_csum_one(test, opaque_maio_store(test, ifq));
}

static void opaque_maio_send_managed_probe(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);

	opaque_wire_probe(test, opaque_maio_store(test, ifq));
}

/*
 * @len bytes of @fill in a page of the area that the pool owns, as the driver of a queue of the
 * instance gets it, in the skb that carries them, which the store of @stream captures. The skb is
 * gone and the store holds the page by a pool reference.
 */
static struct page *opaque_pool_capture(struct kunit *test, struct io_opaque_stream *stream,
					struct page_pool *pp, u32 len, u8 fill)
{
	struct page *page = opaque_pool_page(test, pp);
	struct sk_buff *skb;
	void *mapped = kmap_local_page(page);

	memset(mapped, fill, len);
	kunmap_local(mapped);
	skb = opaque_pool_page_skb(test, page, 0, len);
	opaque_maio_take(test, stream, skb, len);
	kfree_skb(skb);
	return page;
}

/*
 * A store that binds no instance and holds a page of one: it captures an skb from a queue that
 * the instance owns, as a store does that captures on the device of an instance, and then lets the
 * object go. The mark stays.
 */
static struct io_opaque_store *opaque_maio_unbound_store(struct kunit *test,
							 struct io_zcrx_ifq *ifq)
{
	struct page_pool *pp = io_zcrx_maio_test_pool(test, ifq);
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;

	opaque_test_stream(&stream, store);
	opaque_pool_capture(test, &stream, pp, 1000, 0x5c);
	io_opaque_extents_free(store, &stream.extents);
	io_opaque_capture_release(&stream);
	KUNIT_ASSERT_TRUE(test, store->maio_pages);
	KUNIT_ASSERT_EQ(test, io_opaque_send_ubuf_flags(store), (u8)SKBFL_SHARED_FRAG);
	KUNIT_ASSERT_EQ(test, atomic64_read(&store->bytes), 0LL);
	return store;
}

/*
 * A store that binds no instance notes that it holds a page of one when it keeps, by a pool
 * reference, a page in the area that a pool of an instance owns, and not otherwise. A plain page
 * does not mark it, nor a page of an ordinary pool, nor a fallback page of the pool of the
 * instance: it is not in the area, and nobody maps it. The mark stays when the page is let go.
 */
static void opaque_maio_unbound_flag(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, OPAQUE_BATCH + 1);
	struct page_pool *pp = io_zcrx_maio_test_pool(test, ifq);
	struct page_pool *ordinary = opaque_plain_pool(test);
	struct io_opaque_store *store = opaque_test_store(test);
	struct page *held[OPAQUE_BATCH], *copy, *fallback, *plain, *pooled;
	struct sk_buff *skb[4];
	struct io_opaque_stream stream;
	struct io_opaque_extent *extent;
	unsigned int i;
	u64 off;

	/* One page for a copy, a batch for the pool, and then nothing: the pool falls back. */
	copy = io_zcrx_maio_copy_page(ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	for (i = 0; i < OPAQUE_BATCH; i++)
		held[i] = opaque_pool_page(test, pp);
	fallback = opaque_pool_page(test, pp);
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_owned(ifq, fallback));
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_off(ifq, fallback, &off));
	plain = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, plain);
	pooled = page_pool_dev_alloc_pages(ordinary);
	KUNIT_ASSERT_NOT_NULL(test, pooled);

	KUNIT_EXPECT_FALSE(test, store->maio_pages);
	KUNIT_EXPECT_EQ(test, io_opaque_send_ubuf_flags(store), (u8)0);
	opaque_test_stream(&stream, store);

	/* A plain page, a page of an ordinary pool, and a fallback page of the instance's pool. */
	skb[0] = opaque_test_frag(test, plain, 0, 300);
	opaque_maio_take(test, &stream, skb[0], 300);
	KUNIT_EXPECT_FALSE(test, store->maio_pages);
	skb[1] = opaque_pp_skb(test, pooled, 0, 400);
	opaque_maio_take(test, &stream, skb[1], 400);
	extent = list_last_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PP);
	KUNIT_EXPECT_FALSE(test, store->maio_pages);
	skb[2] = opaque_pool_page_skb(test, fallback, 0, 500);
	opaque_maio_take(test, &stream, skb[2], 500);
	extent = list_last_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, fallback);
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PP);
	KUNIT_EXPECT_FALSE(test, store->maio_pages);
	KUNIT_EXPECT_EQ(test, io_opaque_send_ubuf_flags(store), (u8)0);

	/* A page of the area: held the same way, and the store holds a page of the instance. */
	skb[3] = opaque_pool_page_skb(test, held[0], 0, 600);
	opaque_maio_take(test, &stream, skb[3], 600);
	extent = list_last_entry(&stream.extents, struct io_opaque_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, held[0]);
	KUNIT_EXPECT_EQ(test, extent->backing->ref, (u8)IO_OPAQUE_REF_PP);
	KUNIT_EXPECT_FALSE(test, extent->backing->area);
	KUNIT_EXPECT_TRUE(test, store->maio_pages);
	KUNIT_EXPECT_EQ(test, io_opaque_send_ubuf_flags(store), (u8)SKBFL_SHARED_FRAG);

	/* The pages go back, and the mark stays. */
	for (i = 0; i < ARRAY_SIZE(skb); i++)
		kfree_skb(skb[i]);
	io_opaque_extents_free(store, &stream.extents);
	io_opaque_capture_release(&stream);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_TRUE(test, store->maio_pages);
	KUNIT_EXPECT_EQ(test, io_opaque_send_ubuf_flags(store), (u8)SKBFL_SHARED_FRAG);
	for (i = 1; i < OPAQUE_BATCH; i++)
		opaque_pool_free(pp, held[i]);
	io_zcrx_maio_page_put(ifq, copy);
	__free_page(plain);
}

/* The skbs that a store with a page of an instance makes are marked, and so are theirs. */
static void opaque_maio_unbound_managed_shared_frag(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);

	opaque_shared_frag_one(test, opaque_maio_unbound_store(test, ifq));
}

static void opaque_maio_unbound_send_managed_shared_frag(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);

	opaque_wire_shared_frag(test, opaque_maio_unbound_store(test, ifq));
}

static void opaque_maio_unbound_managed_shared_csum(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);

	opaque_shared_csum_one(test, opaque_maio_unbound_store(test, ifq));
}

static void opaque_maio_unbound_send_managed_probe(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);

	opaque_wire_probe(test, opaque_maio_unbound_store(test, ifq));
}

/*
 * A store that binds no instance sends on one socket before it holds a page of an instance and
 * after. The socket is corked, so that what TCP has not sent stays at the tail of the queue. The
 * first send makes an skb that is not marked. The store then captures three pages of the area,
 * from a queue that the instance owns, and publishes them as an object. The sends of that object
 * are marked, and so would be the sends of any other. The first of them finds the unmarked skb at
 * the tail of the queue and starts another, and the second joins the one that is marked. The
 * peer's queue has two skbs, one of them marked, and the bytes are those of the first object and
 * then those of the area pages, which the skbs carry without a copy.
 */
static void opaque_maio_unbound_send_managed_flip(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = io_zcrx_maio_test_ifq(test, 16);
	struct page_pool *pp = io_zcrx_maio_test_pool(test, ifq);
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_pool *ubufs = opaque_pool_new(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *area = kunit_kmalloc(test, 3 * 2000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 16000, GFP_KERNEL);
	unsigned int i, skbs, shared, ours;
	struct opaque_managed before, after;
	struct io_opaque_stream stream;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, area);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_test_params_save(test);
	io_opaque_send_managed = true;
	io_opaque_send_copy_max = 0;
	store->ubuf_pool = ubufs->pool;
	opaque_wire_init(test, w);
	opaque_wire_sndbuf(w->tx->sk, SZ_1M);
	tcp_sock_set_cork(w->tx->sk, true);
	opaque_test_publish(store, 0, 9, data);
	opaque_test_copy(data, source);
	KUNIT_ASSERT_FALSE(test, store->maio_pages);
	KUNIT_ASSERT_EQ(test, io_opaque_send_ubuf_flags(store), (u8)0);
	opaque_managed_read(&before);

	/* Before: an ordinary object, and an skb that is not marked and not sent. */
	opaque_managed_send(test, w, store, 0, 6000, 0);
	KUNIT_EXPECT_EQ(test, (int)(tcp_sk(w->tx->sk)->write_seq - tcp_sk(w->tx->sk)->snd_nxt),
			6000);

	/* The store captures three pages of the area. Their object takes the slot of the first. */
	opaque_test_stream(&stream, store);
	for (i = 0; i < 3; i++) {
		opaque_pool_capture(test, &stream, pp, 2000, 0x41 + i);
		memset(area + i * 2000, 0x41 + i, 2000);
	}
	KUNIT_ASSERT_TRUE(test, store->maio_pages);
	KUNIT_ASSERT_EQ(test, io_opaque_send_ubuf_flags(store), (u8)SKBFL_SHARED_FRAG);
	opaque_test_free(store, 0);
	data = opaque_maio_object(test, &stream);
	KUNIT_EXPECT_FALSE(test, data->mapped);
	opaque_test_publish(store, 0, 9, data);
	io_opaque_capture_release(&stream);

	/* After: the first send of it starts a skb, and the second joins that skb. */
	opaque_managed_send(test, w, store, 0, 6000, 0);
	opaque_managed_send(test, w, store, 2000, 4000, 0);
	tcp_sock_set_cork(w->tx->sk, false);
	opaque_wire_acked(w->tx->sk);
	opaque_managed_read(&after);
	opaque_wire_queue_frags(w, &skbs, &shared, &ours);
	KUNIT_EXPECT_EQ(test, skbs, 2U);
	KUNIT_EXPECT_EQ(test, ours, 2U);
	KUNIT_EXPECT_EQ(test, shared, 1U);
	KUNIT_EXPECT_EQ(test, after.skbs - before.skbs, 2ULL);
	KUNIT_EXPECT_EQ(test, after.copied, before.copied);
	opaque_wire_read(test, w, got, 0, 16000);
	KUNIT_EXPECT_MEMEQ(test, got, source, 6000);
	KUNIT_EXPECT_MEMEQ(test, got + 6000, area, 6000);
	KUNIT_EXPECT_MEMEQ(test, got + 12000, area + 2000, 4000);

	/* The reader has let go of every skb, and the pages of the area go back to the pool. */
	opaque_wire_acked(w->tx->sk);
	opaque_test_free(store, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	store->ubuf_pool = NULL;
}
#endif

/*
 * The pool on several CPUs. OPAQUE_RACE_TAKERS tasks make skbs with ubufs through
 * io_opaque_link_skb(), as sends do, and OPAQUE_RACE_FREERS tasks free them in the order they were
 * made, every other one with bottom halves off as an ACK frees a skb, so that a ubuf completes on
 * one CPU and is taken on another: the takers have the lock of the pool between them, the
 * completions a compare-and-exchange. Each skb in use is entered under the address of its ubuf, and
 * a second entry for one address is a ubuf that two skbs have at once: a duplicate. A ubuf comes
 * out whole, as a new one is, whichever skb had it before.
 */
#define OPAQUE_RACE_TAKERS	3
#define OPAQUE_RACE_FREERS	3
#define OPAQUE_RACE_TASKS	(OPAQUE_RACE_TAKERS + OPAQUE_RACE_FREERS)
/* The takers wait while this many skbs are in use, so that they do not run away from the freers. */
#define OPAQUE_RACE_QUEUE	512

struct opaque_race;

struct opaque_race_task {
	struct opaque_race *r;
	struct completion done;
};

struct opaque_race {
	struct opaque_pool *p;
	/* The skbs in use, in the order they were made, and by the address of their ubuf. */
	struct sk_buff_head queue;
	struct xarray inuse;
	/* The takers stop first, the freers when the queue is empty; they wait while held. */
	atomic_t stop_take, stop_free, hold;
	atomic_long_t taken, freed, dup, bad;
	struct opaque_race_task task[OPAQUE_RACE_TASKS];
};

static int opaque_race_taker(void *data)
{
	struct opaque_race_task *t = data;
	struct opaque_race *r = t->r;
	unsigned long n = 0;

	while (!atomic_read(&r->stop_take)) {
		struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);
		struct io_opaque_ubuf *u;

		if (!skb) {
			cond_resched();
			continue;
		}
		skb->ip_summed = CHECKSUM_PARTIAL;
		if (io_opaque_link_skb(skb, &r->p->mub.uarg)) {
			kfree_skb(skb);
			atomic_long_inc(&r->bad);
			continue;
		}
		u = opaque_skb_ubuf(skb);
		if (u->nr || u->pool != r->p->pool ||
		    u->uarg.ops != &io_opaque_ubuf_ops || refcount_read(&u->uarg.refcnt) != 1)
			atomic_long_inc(&r->bad);
		switch (xa_insert(&r->inuse, (unsigned long)u >> 3, skb, GFP_KERNEL)) {
		case 0:
			break;
		case -EBUSY:
			atomic_long_inc(&r->dup);
			break;
		default:
			atomic_long_inc(&r->bad);
		}
		while (skb_queue_len(&r->queue) > OPAQUE_RACE_QUEUE && !atomic_read(&r->stop_take))
			cond_resched();
		skb_queue_tail(&r->queue, skb);
		atomic_long_inc(&r->taken);
		if (!(++n & 31))
			cond_resched();
	}
	complete(&t->done);
	return 0;
}

static int opaque_race_freer(void *data)
{
	struct opaque_race_task *t = data;
	struct opaque_race *r = t->r;
	unsigned long n = 0;

	for (;;) {
		struct sk_buff *skb;

		if (atomic_read(&r->hold)) {
			cond_resched();
			continue;
		}
		skb = skb_dequeue(&r->queue);
		if (!skb) {
			if (atomic_read(&r->stop_free))
				break;
			cond_resched();
			continue;
		}
		xa_erase(&r->inuse, (unsigned long)opaque_skb_ubuf(skb) >> 3);
		if (n++ & 1) {
			local_bh_disable();
			kfree_skb(skb);
			local_bh_enable();
		} else {
			kfree_skb(skb);
		}
		atomic_long_inc(&r->freed);
		if (!(n & 31))
			cond_resched();
	}
	complete(&t->done);
	return 0;
}

/*
 * Make a pool and start the takers and the freers on it. The tasks are made before any is woken, so
 * that one that cannot be made takes the others down before they have run, as they use what is made
 * here. The freers wait while @hold.
 */
static struct opaque_race *opaque_race_start(struct kunit *test, bool hold)
{
	struct opaque_race *r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	struct task_struct *task[OPAQUE_RACE_TASKS];
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, r);
	r->p = opaque_pool_new(test);
	skb_queue_head_init(&r->queue);
	xa_init(&r->inuse);
	atomic_set(&r->hold, hold);
	for (i = 0; i < OPAQUE_RACE_TASKS; i++) {
		r->task[i].r = r;
		init_completion(&r->task[i].done);
		task[i] = kthread_create(i < OPAQUE_RACE_TAKERS ? opaque_race_taker :
					 opaque_race_freer, &r->task[i], "opaque_race%u", i);
		if (IS_ERR(task[i])) {
			while (i--)
				kthread_stop(task[i]);
			return NULL;
		}
	}
	for (i = 0; i < OPAQUE_RACE_TASKS; i++)
		wake_up_process(task[i]);
	return r;
}

/* The takers stop and are waited for: every skb they made is queued. */
static void opaque_race_stop_taking(struct opaque_race *r)
{
	unsigned int i;

	atomic_set(&r->stop_take, 1);
	for (i = 0; i < OPAQUE_RACE_TAKERS; i++)
		wait_for_completion(&r->task[i].done);
}

/* The freers stop when the queue is empty, and are waited for: every skb is freed. */
static void opaque_race_finish(struct opaque_race *r)
{
	unsigned int i;

	atomic_set(&r->stop_free, 1);
	for (i = OPAQUE_RACE_TAKERS; i < OPAQUE_RACE_TASKS; i++)
		wait_for_completion(&r->task[i].done);
}

/*
 * Takers and freers for a few seconds, and then the freers drain what is queued. Nobody gets a
 * ubuf that another skb has, every ubuf comes whole, and every skb made is freed. What the cache
 * made and has not got back is what the pool holds, on its list and in its stock, within its bound,
 * give or take the ubufs of the freers that were in the middle of a push when the list was taken:
 * each can leave one on the new list uncounted, and have one refused that was counted before the
 * count was reset. The references of the pool are the store's and one for each ubuf it holds, and
 * closing it gives them all back. It needs two CPUs to race on.
 */
static void opaque_ubuf_pool_smp_stress(struct kunit *test)
{
	struct opaque_pool_snap before, after;
	unsigned int done, stocked;
	struct opaque_race *r;

	opaque_race_needs_cpus(test);
	opaque_pool_read(&before);
	r = opaque_race_start(test, false);
	KUNIT_ASSERT_NOT_NULL(test, r);
	/* The pool that the race made. */
	before.pools++;
	msleep(4000);
	opaque_race_stop_taking(r);
	opaque_race_finish(r);
	done = opaque_pool_held(r->p->pool, &stocked);
	opaque_pool_read(&after);
	kunit_info(test, "cpus %u taken %ld freed %ld reused %llu live %ld done %u stock %u",
		   num_online_cpus(), atomic_long_read(&r->taken), atomic_long_read(&r->freed),
		   after.reused - before.reused, after.live - before.live, done, stocked);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&r->dup), 0L);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&r->bad), 0L);
	KUNIT_EXPECT_EQ(test, skb_queue_len(&r->queue), 0U);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&r->taken), atomic_long_read(&r->freed));
	KUNIT_EXPECT_EQ(test, after.skbs - before.skbs, (u64)atomic_long_read(&r->taken));
	KUNIT_EXPECT_GT(test, after.reused - before.reused, 0ULL);
	KUNIT_EXPECT_EQ(test, after.live - before.live, (long)(done + stocked));
	KUNIT_EXPECT_LE(test, done, IO_OPAQUE_UBUF_POOL_MAX + 2 * OPAQUE_RACE_FREERS);
	KUNIT_EXPECT_LE(test, done + stocked, 2 * IO_OPAQUE_UBUF_POOL_MAX + 4 * OPAQUE_RACE_FREERS);
	KUNIT_EXPECT_EQ(test, refcount_read(&r->p->pool->refs), 1 + done + stocked);
	xa_destroy(&r->inuse);
	opaque_pool_close(r->p);
	opaque_pool_read(&after);
	KUNIT_EXPECT_EQ(test, after.live, before.live);
	KUNIT_EXPECT_EQ(test, after.pools, before.pools - 1);
}

/*
 * The store goes while ubufs of its pool complete on other CPUs. In each round the takers fill the
 * queue with skbs whose ubufs the pool made and stop, the freers are let go, and the pool is closed
 * a little later than that, from not at all to nearly a millisecond: before the first completion,
 * among them and after the last. Whatever the order, no ubuf is freed twice or left on a list that
 * nobody takes, every one is back with the cache, and the pool is freed with the last of them.
 */
static void opaque_ubuf_pool_smp_close(struct kunit *test)
{
	unsigned int round;

	opaque_race_needs_cpus(test);
	for (round = 0; round < 20; round++) {
		struct opaque_pool_snap before, after;
		unsigned long wait_until;
		struct opaque_race *r;

		opaque_pool_read(&before);
		r = opaque_race_start(test, true);
		KUNIT_ASSERT_NOT_NULL(test, r);
		before.pools++;
		wait_until = jiffies + 10 * HZ;
		while (skb_queue_len(&r->queue) < 400 && time_before(jiffies, wait_until))
			msleep(1);
		KUNIT_EXPECT_GE(test, skb_queue_len(&r->queue), 400U);
		opaque_race_stop_taking(r);
		atomic_set(&r->hold, 0);
		udelay(50 * round);
		opaque_pool_close(r->p);
		opaque_race_finish(r);
		opaque_pool_read(&after);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&r->dup), 0L);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&r->bad), 0L);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&r->taken), atomic_long_read(&r->freed));
		KUNIT_EXPECT_EQ(test, after.live, before.live);
		KUNIT_EXPECT_EQ(test, after.pools, before.pools - 1);
		xa_destroy(&r->inuse);
	}
}

/*
 * A send that does not consume its object takes its reference on the version in RCU, with no
 * lock (io_opaque_pin()): it reads the slot, and takes the reference if the count is not zero.
 * The tests below change what the slot holds in the window between the two, which
 * io_opaque_lookup_between lets them reach, and then do the same from several CPUs. A hook runs
 * in the RCU section of the lookup, so it may take a spinlock and put, and it may not sleep
 * or make an expectation, which allocates when it fails: it records, and the test looks after.
 * io_opaque_lookup_before is reached before the slot is read, and io_opaque_lookup_after by a
 * lookup that holds its reference: both outside RCU and with no lock, where a hook may do what a
 * context of its own does, such as another ring's FREE, or the stop of the store.
 */
enum {
	OPAQUE_LK_BEFORE,
	OPAQUE_LK_BETWEEN,
	OPAQUE_LK_AFTER,
};

static void opaque_lookup_hook_reset(void *unused)
{
	WRITE_ONCE(io_opaque_lookup_before, NULL);
	WRITE_ONCE(io_opaque_lookup_between, NULL);
	WRITE_ONCE(io_opaque_lookup_after, NULL);
}

/* Run @hook at @where in every lookup, until the end of the test. */
static void opaque_lookup_hook_at(struct kunit *test, unsigned int where,
				  void (*hook)(struct io_opaque_store *store))
{
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, opaque_lookup_hook_reset, NULL), 0);
	if (where == OPAQUE_LK_BEFORE)
		WRITE_ONCE(io_opaque_lookup_before, hook);
	else if (where == OPAQUE_LK_AFTER)
		WRITE_ONCE(io_opaque_lookup_after, hook);
	else
		WRITE_ONCE(io_opaque_lookup_between, hook);
}

/* Run @hook in the window of every lookup, until the end of the test. */
static void opaque_lookup_hook(struct kunit *test, void (*hook)(struct io_opaque_store *store))
{
	opaque_lookup_hook_at(test, OPAQUE_LK_BETWEEN, hook);
}

/* What the window of the tests that make it do something does, and what it saw. */
static struct {
	void (*act)(void);
	/* It acts in the lookup with this number, counting from zero, and in no other. */
	unsigned int at, calls;
	struct io_opaque_store *store;
	/* A compaction that has been built, which the window publishes. */
	struct io_opaque_req *compact;
	/* Whether it also puts the reference that the compaction holds on the old version. */
	bool drop;
	/* A request that takes the slot over with a new object. */
	struct io_opaque_req *reuse;
	/* A socket, to say whether the lookup had it owned. */
	struct sock *sk;
	bool owned;
	/* Whether the window ran in an RCU read-side section, as it must. */
	bool rcu;
	/* What the act got, and the count of @watch before it and after. */
	int ret, reuse_ret;
	u64 flags;
	struct io_opaque_data *watch;
	int before, refs;
} opaque_lk;

static void opaque_lk_window(struct io_opaque_store *store)
{
	if (opaque_lk.calls++ != opaque_lk.at || !opaque_lk.act)
		return;
	opaque_lk.rcu = rcu_read_lock_held();
	if (opaque_lk.sk)
		opaque_lk.owned = sock_owned_by_user_nocheck(opaque_lk.sk);
	if (opaque_lk.watch)
		opaque_lk.before = refcount_read(&opaque_lk.watch->refs);
	opaque_lk.act();
	if (opaque_lk.watch)
		opaque_lk.refs = refcount_read(&opaque_lk.watch->refs);
}

/*
 * Start the window: run @act at @where in lookup number @at of this test. Lookups are counted
 * where the window is, so a window after the reference does not see the lookups that fail, and
 * one before the read sees the ones that the stop of a store leaves nothing to read for.
 */
static void opaque_lk_open_at(struct kunit *test, struct io_opaque_store *store,
			      unsigned int where, void (*act)(void), unsigned int at)
{
	memset(&opaque_lk, 0, sizeof(opaque_lk));
	opaque_lk.store = store;
	opaque_lk.act = act;
	opaque_lk.at = at;
	opaque_lookup_hook_at(test, where, opaque_lk_window);
}

/* Start the window between the read of the slot and the reference. */
static void opaque_lk_open(struct kunit *test, struct io_opaque_store *store, void (*act)(void),
			   unsigned int at)
{
	opaque_lk_open_at(test, store, OPAQUE_LK_BETWEEN, act, at);
}

static void opaque_lk_close(void)
{
	opaque_lookup_hook_reset(NULL);
}

/* The object is freed, and the put of the table's reference may be the last. */
static void opaque_lk_free(void)
{
	opaque_test_free(opaque_lk.store, 0);
}

/*
 * The object is freed by another ring: the put of the table's reference is not the send's, and
 * the spy of the test (opaque_put_spy_start()) does not count it.
 */
static void opaque_lk_free_other(void)
{
	struct kunit *test = kunit_get_current_test();

	kunit_deactivate_static_stub(test, io_opaque_data_put);
	opaque_test_free(opaque_lk.store, 0);
	kunit_activate_static_stub(test, io_opaque_data_put, opaque_test_data_put_spy);
}

/*
 * What io_opaque_stop() does, as far as a window in RCU can do it: dead is set under tables, and
 * the objects are detached and put afterwards, one at a time.
 */
static void opaque_lk_stop_like(void)
{
	struct io_opaque_store *store = opaque_lk.store;
	u32 i;

	spin_lock(&store->tables);
	WRITE_ONCE(store->dead, 1);
	spin_unlock(&store->tables);
	for (i = 0; i < store->config.max_objects; i++)
		opaque_test_free(store, i);
}

/* The store is stopped, as the ring that owns it does when its last user goes. */
static void opaque_lk_stop(void)
{
	io_opaque_stop(opaque_lk.store);
}

/* The compaction is published; with @drop the old version loses the compaction's reference too. */
static void opaque_lk_compact(void)
{
	opaque_lk.ret = io_opaque_compact_publish(opaque_lk.compact);
	if (opaque_lk.drop)
		io_opaque_data_put(opaque_lk.store, opaque_lk.compact->data);
}

/* The object is freed and its slot taken by a new one, under the next generation. */
static void opaque_lk_reuse(void)
{
	opaque_test_free(opaque_lk.store, 0);
	opaque_lk.ret = io_opaque_slot_reserve(opaque_lk.reuse);
	if (!opaque_lk.ret)
		opaque_lk.ret = io_opaque_publish(opaque_lk.reuse, &opaque_lk.flags);
}

/*
 * The compaction is published and its old version loses the references of the compaction and of
 * the table, so that no one holds it; the replacement is then freed too, and with @reuse the
 * slot is taken by a new object. Nothing in the table is left for the handle of the old version.
 */
static void opaque_lk_compact_free(void)
{
	opaque_lk.ret = io_opaque_compact_publish(opaque_lk.compact);
	io_opaque_data_put(opaque_lk.store, opaque_lk.compact->data);
	opaque_lk.compact->data = NULL;
	opaque_test_free(opaque_lk.store, 0);
	if (opaque_lk.reuse) {
		opaque_lk.reuse_ret = io_opaque_slot_reserve(opaque_lk.reuse);
		if (!opaque_lk.reuse_ret)
			opaque_lk.reuse_ret = io_opaque_publish(opaque_lk.reuse, &opaque_lk.flags);
	}
}

/*
 * What a lookup returns for the handles that it must refuse, and what it takes for the ones it
 * takes: the version is the one the handle names and knows its handle, a slot that was freed and
 * taken by another object is not found by the handle of the first, and a lookup that finds the
 * wrong object, or a range that the version does not have, takes no reference. None of it needs
 * the window, so none of it takes tables.
 */
static void opaque_pin_handles(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *old = opaque_sparse_data(test, store);
	struct io_opaque_data *new = opaque_sparse_data(test, store);
	struct io_opaque_req reuse = { .store = store, .data = new, .length = new->length };
	struct io_opaque_req send = { .store = store, .offset = 100, .length = 1000 };
	const u64 first = io_opaque_handle(0, 9), second = io_opaque_handle(0, 10);
	long slow = atomic_long_read(&io_opaque_pin_slow);
	u64 flags = 0, name[5];
	unsigned int i;

	opaque_test_publish(store, 0, 9, old);
	KUNIT_EXPECT_EQ(test, old->handle, first);

	/* The version the handle names, with a reference of the send's own. */
	send.handle = first;
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &send), 0);
	KUNIT_EXPECT_PTR_EQ(test, send.data, old);
	KUNIT_EXPECT_FALSE(test, send.consumed);
	KUNIT_EXPECT_EQ(test, refcount_read(&old->refs), 2);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&send.iter), (size_t)1000);
	io_opaque_data_put(store, send.data);
	KUNIT_EXPECT_EQ(test, refcount_read(&old->refs), 1);

	/* Names of nothing: no handle, no such slot, a slot that holds none, two generations. */
	name[0] = 0;
	name[1] = io_opaque_handle(store->config.max_objects, 9);
	name[2] = io_opaque_handle(1, 9);
	name[3] = io_opaque_handle(0, 10);
	name[4] = io_opaque_handle(0, 8);
	for (i = 0; i < ARRAY_SIZE(name); i++) {
		send.data = NULL;
		send.handle = name[i];
		KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &send), -ESTALE);
		KUNIT_EXPECT_PTR_EQ(test, send.data, NULL);
		KUNIT_EXPECT_EQ(test, refcount_read(&old->refs), 1);
	}

	/* A range outside the version is refused, and no reference is taken. */
	send.handle = first;
	send.offset = old->length - 10;
	send.length = 20;
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &send), -ERANGE);
	KUNIT_EXPECT_PTR_EQ(test, send.data, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&old->refs), 1);
	send.offset = 100;
	send.length = 1000;

	/*
	 * The object is freed and the slot taken by another, as RECV_OBJECT does it. The old handle
	 * reaches the new version, which is not what it names: it finds that, and gives the
	 * reference back. The new handle gets it. Publishing writes the handle the version knows.
	 */
	opaque_test_free(store, 0);
	KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&reuse), 0);
	KUNIT_ASSERT_EQ(test, io_opaque_publish(&reuse, &flags), (int)new->length);
	KUNIT_EXPECT_EQ(test, reuse.handle, second);
	KUNIT_EXPECT_EQ(test, new->handle, second);
	send.handle = first;
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &send), -ESTALE);
	KUNIT_EXPECT_PTR_EQ(test, send.data, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&new->refs), 1);
	send.handle = second;
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &send), 0);
	KUNIT_EXPECT_PTR_EQ(test, send.data, new);
	KUNIT_EXPECT_EQ(test, refcount_read(&new->refs), 2);
	io_opaque_data_put(store, send.data);

	/* A store that is stopping lets nothing in. */
	send.data = NULL;
	WRITE_ONCE(store->dead, 1);
	KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &send), -ESHUTDOWN);
	KUNIT_EXPECT_PTR_EQ(test, send.data, NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&new->refs), 1);
	WRITE_ONCE(store->dead, 0);

	/*
	 * A lookup that found no version of its handle to take was asked of the table: the empty
	 * slot, the two generations, and the slot that another object had taken. The names that no
	 * slot can have, none and out of range, were refused at once.
	 */
	KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_pin_slow), slow + 4);

	/*
	 * A send that consumes its object takes tables itself, for it removes the handle, and
	 * the table says so if it does not hold it: that would be a warning, which is counted.
	 */
	send.handle = second;
	send.send_flags = IORING_OPAQUE_SEND_LAST;
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_EQ(test, opaque_test_acquire(store, &send), 0);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 0);
	}
	KUNIT_EXPECT_TRUE(test, send.consumed);
	KUNIT_EXPECT_PTR_EQ(test, send.data, new);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&new->refs), 1);
	io_opaque_data_put(store, send.data);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	kvfree_rcu_barrier();
	opaque_expect_lockdep(test, store);
}

/*
 * The version that a lookup has read is put for the last time before the lookup takes its
 * reference. Its count is zero then, and the lookup finds that in the struct, which is still
 * there; it asks the table again, finds the slot empty, and returns ESTALE without a version.
 * One put frees it, and the extents are gone at once. If an earlier send still holds the
 * version, the lookup takes its reference as it should: the send is ordered before the free.
 */
static void opaque_pin_retired(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	unsigned int held;

	for (held = 0; held < 2; held++) {
		struct io_opaque_data *data = opaque_sparse_data(test, store);
		const u64 generation = 9 + held;
		struct io_opaque_req send = {
			.store = store, .handle = io_opaque_handle(0, generation),
			.offset = 100, .length = 1000,
		};
		long slow = atomic_long_read(&io_opaque_pin_slow);
		int ret;

		opaque_test_publish(store, 0, generation, data);
		if (held)
			refcount_inc(&data->refs); /* an earlier send */
		opaque_lk_open(test, store, opaque_lk_free, 0);
		opaque_lk.watch = data;
		ret = opaque_test_acquire(store, &send);
		opaque_lk_close();
		KUNIT_EXPECT_EQ(test, opaque_lk.calls, 1U);
		KUNIT_EXPECT_TRUE(test, opaque_lk.rcu);
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
		if (!held) {
			KUNIT_EXPECT_EQ(test, ret, -ESTALE);
			KUNIT_EXPECT_PTR_EQ(test, send.data, NULL);
			KUNIT_EXPECT_FALSE(test, send.consumed);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_pin_slow) - slow, 1L);
			/* The put in the window was the last, and the struct was still there. */
			KUNIT_EXPECT_EQ(test, opaque_lk.before, 1);
			KUNIT_EXPECT_EQ(test, opaque_lk.refs, 0);
			KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
		} else {
			KUNIT_EXPECT_EQ(test, ret, 0);
			KUNIT_EXPECT_PTR_EQ(test, send.data, data);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_pin_slow) - slow, 0L);
			KUNIT_EXPECT_EQ(test, opaque_lk.before, 2);
			KUNIT_EXPECT_EQ(test, opaque_lk.refs, 1);
			KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
			KUNIT_EXPECT_EQ(test, data->handle, send.handle);
			io_opaque_data_put(store, send.data);
			io_opaque_data_put(store, data);
			KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
		}
		kvfree_rcu_barrier();
	}
	opaque_expect_lockdep(test, store);
}

/*
 * The same in a send that the ring issues: the head owns the socket when it takes its
 * references, and the lookup that finds the version gone asks the table again with it owned,
 * which lockdep checks against the order of the locks. The send ends with ESTALE and holds
 * nothing; no byte is sent and nobody is left waiting.
 */
static void opaque_pin_retired_send(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	struct sock *sk;
	unsigned int mode;

	KUNIT_ASSERT_NOT_NULL(test, w);
	opaque_wire_init(test, w);
	sk = w->tx->sk;
	opaque_wire_sndbuf(sk, SZ_1M);
	for (mode = 0; mode < 2; mode++) {
		struct io_opaque_data *data = opaque_sparse_data(test, store);
		const u64 generation = 9 + mode;
		struct io_opaque_req *head;
		long calls = atomic_long_read(&io_opaque_send_locked_calls);
		long slow = atomic_long_read(&io_opaque_pin_slow);
		int ret;

		opaque_test_publish(store, 0, generation, data);
		head = opaque_wire_req(test, w, store, 0, 0, 100, 1000);
		head->handle = io_opaque_handle(0, generation);
		memset(&w->req[0].cqe, 0, sizeof(w->req[0].cqe));
		opaque_lk_open(test, store, opaque_lk_free, 0);
		opaque_lk.sk = sk;
		ret = opaque_wire_issue(w, 0, !mode);
		opaque_lk_close();
		lockdep_assert_none_held_once();
		KUNIT_EXPECT_EQ(test, ret, -ESTALE);
		KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 0);
		KUNIT_EXPECT_EQ(test, opaque_lk.calls, 1U);
		KUNIT_EXPECT_TRUE(test, opaque_lk.rcu);
		/* The lookup ran with the socket owned, as a head's does. */
		KUNIT_EXPECT_TRUE(test, opaque_lk.owned);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_pin_slow) - slow, 1L);
		KUNIT_EXPECT_FALSE(test, head->consumed);
		KUNIT_EXPECT_PTR_EQ(test, head->data, NULL);
		KUNIT_EXPECT_PTR_EQ(test, head->tx, NULL);
		KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
		KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
		KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
		KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_send_locked_calls), calls);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
		kvfree_rcu_barrier();
	}
	opaque_expect_lockdep(test, store);
}

/*
 * A store stops by setting dead under tables and detaching its objects afterwards, and a send
 * that has looked at dead, found the store alive and not yet read its slot may meet the stop
 * anywhere in that. It is told that the store has stopped, as a send that came after is, and not
 * that its handle is stale: no handle was freed. That holds wherever the stop lands, and for
 * each of the three ways that a lookup in RCU finds nothing to take, which are all asked of the
 * table: after the slot was read and the version put by all (the lookup finds a count of zero);
 * after the slot was read and found to hold an object of the next generation (another handle);
 * and before the slot was read (it finds nothing); for a single range and for the second entry
 * of a vector, whose first has pinned already. A send that took its reference before the stop
 * was ordered before it, and takes the version: here an earlier send holds the version, so the
 * count is not zero when the lookup raises it.
 */
static void opaque_pin_stop_window(struct kunit *test)
{
	static const struct {
		unsigned int where, at;
		void (*act)(void);
		int ret;
	} cases[] = {
		/* 0: the version is put by all of its holders while the lookup has it. */
		{ OPAQUE_LK_BETWEEN, 0, opaque_lk_stop_like, -ESHUTDOWN },
		/* 1: an earlier send holds it, and the lookup takes it. */
		{ OPAQUE_LK_BETWEEN, 0, opaque_lk_stop_like, 0 },
		/* 2: the store is stopped before the slot is read. */
		{ OPAQUE_LK_BEFORE, 0, opaque_lk_stop, -ESHUTDOWN },
		/* 3: the same, in the lookup of the second entry of a vector. */
		{ OPAQUE_LK_BEFORE, 1, opaque_lk_stop, -ESHUTDOWN },
		/* 4: the slot holds the object of the next generation when the lookup reads it. */
		{ OPAQUE_LK_BETWEEN, 0, opaque_lk_stop_like, -ESHUTDOWN },
	};
	unsigned int kind;

	for (kind = 0; kind < ARRAY_SIZE(cases); kind++) {
		struct io_opaque_store *store = opaque_test_store(test);
		struct io_opaque_data *a = opaque_sparse_data(test, store);
		struct io_opaque_data *b = opaque_sparse_data(test, store);
		struct io_opaque_data *c = kind == 4 ? opaque_sparse_data(test, store) : NULL;
		struct io_uring_opaque_send_entry ent[2] = {
			{ .handle = io_opaque_handle(0, 9), .off = 100, .len = 1000 },
			{ .handle = io_opaque_handle(1, 7), .off = 100, .len = 1000 },
		};
		struct io_opaque_req send = {
			.store = store, .handle = ent[0].handle, .offset = 100, .length = 1000,
		};
		struct io_opaque_req *op = NULL;
		long slow = atomic_long_read(&io_opaque_pin_slow);
		int ret;

		opaque_test_stoppable(test, store);
		opaque_test_publish(store, 0, 9, a);
		opaque_test_publish(store, 1, 7, b);
		if (kind == 3)
			op = opaque_vec_new(test, store, ent, 2, 0);
		if (kind == 1)
			refcount_inc(&a->refs); /* an earlier send */
		if (kind == 4) {
			struct io_opaque_req next = {
				.store = store, .data = c, .length = c->length,
			};
			u64 flags = 0;

			/* The slot is C's, under the next generation, and the send asks for A's. */
			opaque_test_free(store, 0);
			store->object_cursor = 0;
			KUNIT_ASSERT_EQ(test, io_opaque_slot_reserve(&next), 0);
			KUNIT_ASSERT_EQ(test, io_opaque_publish(&next, &flags), (int)c->length);
		}
		opaque_lk_open_at(test, store, cases[kind].where, cases[kind].act, cases[kind].at);
		ret = op ? opaque_vec_admit(op) : opaque_test_acquire(store, &send);
		opaque_lk_close();
		KUNIT_EXPECT_EQ(test, ret, cases[kind].ret);
		KUNIT_EXPECT_EQ(test, READ_ONCE(store->dead), 1);
		KUNIT_EXPECT_EQ(test, opaque_lk.calls, kind == 3 ? 2U : 1U);
		KUNIT_EXPECT_EQ(test, opaque_lk.rcu, cases[kind].where == OPAQUE_LK_BETWEEN);
		KUNIT_EXPECT_EQ(test, store->nr_objects, 0U);
		/* One that meets the stop is settled by the table; one with its reference, not. */
		slow = atomic_long_read(&io_opaque_pin_slow) - slow;
		KUNIT_EXPECT_EQ(test, slow, kind == 1 ? 0L : 1L);
		if (kind == 1) {
			KUNIT_ASSERT_PTR_EQ(test, send.data, a);
			KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
			io_opaque_data_put(store, send.data);
			io_opaque_data_put(store, a);
		} else if (kind == 3) {
			/* The pin of the first entry is the request's to give back. */
			KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[0], a);
			KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
			opaque_vec_failed(test, op, 1);
		} else {
			KUNIT_EXPECT_PTR_EQ(test, send.data, NULL);
		}
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
		kvfree_rcu_barrier();
		opaque_expect_lockdep(test, store);
	}
}

/*
 * A compaction is published in the window. If the old version is gone as well (its last
 * reference put there) the lookup that read it asks the table again, finds the replacement
 * and sends that: the handle was never freed, and ESTALE for it would be wrong. If the old
 * version is still held, the lookup takes it, as the send that it is ordered before the
 * publication; both have the bytes and the handle that was asked for.
 */
static void opaque_pin_compacted(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	unsigned int gone;

	for (gone = 0; gone < 2; gone++) {
		struct io_opaque_data *data = opaque_sparse_data(test, store), *replacement;
		const u64 generation = 9 + gone;
		struct io_opaque_req compact = {
			.store = store, .data = data, .handle = io_opaque_handle(0, generation),
		};
		struct io_opaque_req send = {
			.store = store, .handle = compact.handle, .offset = 100, .length = 1000,
		};
		long slow = atomic_long_read(&io_opaque_pin_slow);
		u8 *source = kunit_kmalloc(test, data->length, GFP_KERNEL);
		u8 *got = kunit_kmalloc(test, send.length, GFP_KERNEL);
		int ret;

		KUNIT_ASSERT_NOT_NULL(test, source);
		KUNIT_ASSERT_NOT_NULL(test, got);
		opaque_test_publish(store, 0, generation, data);
		opaque_test_copy(data, source);
		refcount_inc(&data->refs); /* compaction worker */
		KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
		replacement = compact.replacement;
		opaque_lk_open(test, store, opaque_lk_compact, 0);
		opaque_lk.compact = &compact;
		opaque_lk.drop = gone;
		ret = opaque_test_acquire(store, &send);
		opaque_lk_close();
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, opaque_lk.calls, 1U);
		KUNIT_EXPECT_TRUE(test, opaque_lk.rcu);
		KUNIT_EXPECT_EQ(test, opaque_lk.ret, 0);
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), replacement);
		KUNIT_EXPECT_EQ(test, replacement->handle, compact.handle);
		KUNIT_EXPECT_EQ(test, send.data->handle, compact.handle);
		KUNIT_EXPECT_EQ(test, copy_from_iter(got, send.length, &send.iter),
				(size_t)send.length);
		KUNIT_EXPECT_MEMEQ(test, got, source + send.offset, send.length);
		if (gone) {
			KUNIT_EXPECT_PTR_EQ(test, send.data, replacement);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_pin_slow) - slow, 1L);
			KUNIT_EXPECT_EQ(test, refcount_read(&replacement->refs), 2);
		} else {
			KUNIT_EXPECT_PTR_EQ(test, send.data, data);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_pin_slow) - slow, 0L);
			/* The worker's reference and the send's; the table's went with the old. */
			KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 2);
			io_opaque_data_put(store, compact.data);
		}
		io_opaque_compact_end(&compact);
		io_opaque_data_put(store, send.data);
		opaque_test_free(store, 0);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
		kvfree_rcu_barrier();
	}
	opaque_expect_lockdep(test, store);
}

/*
 * The slot is freed and taken by another object in the window, as the last reference of the old
 * one goes or while an earlier send holds it. Asking for the old handle never gets the new
 * version, whatever order those happen in: the version that the lookup read is the old one,
 * which it takes if it can, and then it is the right one for the handle; or it cannot, and the
 * table, asked again, has a new generation in the slot.
 */
static void opaque_pin_reused(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	unsigned int held;

	for (held = 0; held < 2; held++) {
		struct io_opaque_data *old = opaque_sparse_data(test, store);
		struct io_opaque_data *new = opaque_sparse_data(test, store);
		const u64 generation = 9 + 2 * held;
		struct io_opaque_req reuse = { .store = store, .data = new, .length = new->length };
		struct io_opaque_req send = {
			.store = store, .handle = io_opaque_handle(0, generation),
			.offset = 100, .length = 1000,
		};
		long slow = atomic_long_read(&io_opaque_pin_slow);
		int ret;

		opaque_test_publish(store, 0, generation, old);
		store->object_cursor = 0;
		if (held)
			refcount_inc(&old->refs); /* an earlier send */
		opaque_lk_open(test, store, opaque_lk_reuse, 0);
		opaque_lk.reuse = &reuse;
		ret = opaque_test_acquire(store, &send);
		opaque_lk_close();
		KUNIT_EXPECT_EQ(test, opaque_lk.calls, 1U);
		KUNIT_EXPECT_TRUE(test, opaque_lk.rcu);
		KUNIT_ASSERT_EQ(test, opaque_lk.ret, (int)new->length);
		KUNIT_EXPECT_EQ(test, reuse.handle, io_opaque_handle(0, generation + 1));
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), new);
		KUNIT_EXPECT_EQ(test, refcount_read(&new->refs), 1);
		if (held) {
			KUNIT_ASSERT_EQ(test, ret, 0);
			KUNIT_EXPECT_PTR_EQ(test, send.data, old);
			KUNIT_EXPECT_EQ(test, send.data->handle, send.handle);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_pin_slow) - slow, 0L);
			io_opaque_data_put(store, send.data);
			io_opaque_data_put(store, old);
		} else {
			KUNIT_EXPECT_EQ(test, ret, -ESTALE);
			KUNIT_EXPECT_PTR_EQ(test, send.data, NULL);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_pin_slow) - slow, 1L);
		}
		opaque_test_free(store, 0);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
		kvfree_rcu_barrier();
	}
	opaque_expect_lockdep(test, store);
}

/*
 * A compaction is published in the window, the old version is put by everyone, and the
 * replacement is freed as well, and the slot taken by another object in one case: the lookup
 * read a version that has gone with all it was replaced by. It is told by the table that the
 * handle is stale, and takes nothing; the other object, which has the slot under the next
 * generation, is not looked at, and nothing is left behind: not the extents, not the charge that
 * the compaction made.
 */
static void opaque_pin_slow_free(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	unsigned int reuse;

	for (reuse = 0; reuse < 2; reuse++) {
		struct io_opaque_data *data = opaque_sparse_data(test, store);
		struct io_opaque_data *other = reuse ? opaque_sparse_data(test, store) : NULL;
		const u64 generation = 9 + 2 * reuse;
		struct io_opaque_req compact = {
			.store = store, .data = data, .handle = io_opaque_handle(0, generation),
		};
		struct io_opaque_req nreq = {
			.store = store, .data = other, .length = other ? other->length : 0,
		};
		struct io_opaque_req send = {
			.store = store, .handle = compact.handle, .offset = 100, .length = 1000,
		};
		long slow = atomic_long_read(&io_opaque_pin_slow);
		int ret;

		opaque_test_publish(store, 0, generation, data);
		store->object_cursor = 0;
		refcount_inc(&data->refs); /* compaction worker */
		KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
		opaque_lk_open(test, store, opaque_lk_compact_free, 0);
		opaque_lk.compact = &compact;
		opaque_lk.reuse = reuse ? &nreq : NULL;
		ret = opaque_test_acquire(store, &send);
		opaque_lk_close();
		KUNIT_EXPECT_EQ(test, opaque_lk.calls, 1U);
		KUNIT_EXPECT_TRUE(test, opaque_lk.rcu);
		KUNIT_EXPECT_EQ(test, opaque_lk.ret, 0);
		if (reuse)
			KUNIT_EXPECT_EQ(test, opaque_lk.reuse_ret, (int)other->length);
		KUNIT_EXPECT_EQ(test, ret, -ESTALE);
		KUNIT_EXPECT_PTR_EQ(test, send.data, NULL);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&io_opaque_pin_slow) - slow, 1L);
		io_opaque_compact_end(&compact);
		if (reuse) {
			/* The slot is the other object's, which no one has looked at. */
			KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), other);
			KUNIT_EXPECT_EQ(test, refcount_read(&other->refs), 1);
			opaque_test_free(store, 0);
		}
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
		kvfree_rcu_barrier();
	}
	opaque_expect_lockdep(test, store);
}

/*
 * The entries of a vector that name one handle are a run: the first pins the version, and the
 * rest use that pin and do not read the slot, which may hold another version by then. Here a
 * compaction is published in the window of the first lookup, so the slot holds the
 * replacement while the second entry is resolved; both entries have the old version, with a
 * single reference between them, and the slot was read once. A handle that comes back after
 * another is looked up again, and gets the replacement, in a vector for which the compaction is
 * published in the window of the second lookup. The cursor reads the same bytes from either
 * version.
 */
static void opaque_pin_vec_run(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	unsigned int again;

	for (again = 0; again < 2; again++) {
		struct io_opaque_data *a = opaque_sparse_data(test, store);
		struct io_opaque_data *b = opaque_sparse_data(test, store), *replacement;
		const u64 ha = io_opaque_handle(0, 9 + again), hb = io_opaque_handle(1, 7 + again);
		/* Single segments: the extents are 2000 bytes, and 4000 to 6000 is the third. */
		struct io_uring_opaque_send_entry ent[3] = {
			{ .handle = ha, .off = 100, .len = 1000 },
			{ .handle = again ? hb : ha, .off = again ? 0 : 5000,
			  .len = again ? 500 : 1000 },
			{ .handle = ha, .off = 9000, .len = 300 },
		};
		struct io_opaque_req compact = { .store = store, .data = a, .handle = ha };
		const u32 nr = again ? 3 : 2;
		struct io_opaque_req *op;
		u8 *source[2];

		source[0] = kunit_kmalloc(test, a->length, GFP_KERNEL);
		source[1] = kunit_kmalloc(test, b->length, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, source[0]);
		KUNIT_ASSERT_NOT_NULL(test, source[1]);
		opaque_test_publish(store, 0, 9 + again, a);
		opaque_test_publish(store, 1, 7 + again, b);
		opaque_test_copy(a, source[0]);
		opaque_test_copy(b, source[1]);
		refcount_inc(&a->refs); /* compaction worker */
		KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
		replacement = compact.replacement;
		op = opaque_vec_new(test, store, ent, nr, 0);
		KUNIT_ASSERT_GE(test, op->vec->cap_segs, nr);
		/* The second case also takes the worker's reference off the old version there. */
		opaque_lk_open(test, store, opaque_lk_compact, again);
		opaque_lk.compact = &compact;
		opaque_lk.drop = again;
		KUNIT_ASSERT_EQ(test, opaque_vec_admit(op), 0);
		opaque_lk_close();
		/* One lookup for the run of A; three for A, B and A again. */
		KUNIT_EXPECT_EQ(test, opaque_lk.calls, again ? 3U : 1U);
		KUNIT_EXPECT_TRUE(test, opaque_lk.rcu);
		KUNIT_EXPECT_EQ(test, opaque_lk.ret, 0);
		KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), replacement);
		KUNIT_EXPECT_EQ(test, op->vec->pinned, nr);
		KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[0], a);
		if (!again) {
			/* One reference for the run, and the worker's; the table's is gone. */
			KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[1], a);
			KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 2);
			KUNIT_EXPECT_EQ(test, refcount_read(&replacement->refs), 1);
		} else {
			KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[1], opaque_slot_data(store, 1));
			KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[2], replacement);
			KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
			KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 2);
			KUNIT_EXPECT_EQ(test, refcount_read(&replacement->refs), 2);
		}
		opaque_vec_expect_cursor(test, op);
		opaque_vec_expect_bytes(test, op, source);
		io_opaque_vec_release(op);
		if (!again) {
			KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
			io_opaque_data_put(store, compact.data);
		}
		io_opaque_compact_end(&compact);
		opaque_test_free(store, 0);
		opaque_test_free(store, 1);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
		KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
		kvfree_rcu_barrier();
	}
	opaque_expect_lockdep(test, store);
}

/*
 * A vector that fails after it has pinned keeps its references, for the cleanup of the request:
 * admission may own the socket, and a version that was freed meanwhile makes the pin of an entry
 * the last reference, which is not to be put there. Here the second entry names a range that is
 * not there, and the first object is freed in the window of the second lookup, so that the pin
 * of the first entry is all that is left of it when the vector fails, and the cleanup puts it.
 * The entry that failed took no reference.
 */
static void opaque_pin_vec_failed(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *a = opaque_sparse_data(test, store);
	struct io_opaque_data *b = opaque_sparse_data(test, store);
	struct io_uring_opaque_send_entry ent[2] = {
		{ .handle = io_opaque_handle(0, 9), .off = 100, .len = 1000 },
		{ .handle = io_opaque_handle(1, 7), .off = b->length - 10, .len = 20 },
	};
	struct io_opaque_req *op = opaque_vec_new(test, store, ent, 2, 0);
	int ret;

	opaque_test_publish(store, 0, 9, a);
	opaque_test_publish(store, 1, 7, b);
	opaque_lk_open(test, store, opaque_lk_free, 1);
	opaque_lk.watch = a;
	ret = opaque_vec_admit(op);
	opaque_lk_close();
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
	KUNIT_EXPECT_EQ(test, opaque_lk.calls, 2U);
	KUNIT_EXPECT_TRUE(test, opaque_lk.rcu);
	/* The pin of the first entry was all that was left of A. */
	KUNIT_EXPECT_EQ(test, opaque_lk.before, 2);
	KUNIT_EXPECT_EQ(test, opaque_lk.refs, 1);
	KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
	KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[0], a);
	KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
	KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 2 * opaque_sparse_held(b));
	opaque_vec_failed(test, op, 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), opaque_sparse_held(b));
	KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 1);
	opaque_test_free(store, 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	kvfree_rcu_barrier();
	opaque_expect_lockdep(test, store);
}

/*
 * A head is admitted with the socket owned, inline or from io-wq, and the last reference of the
 * version it has pinned is its own: another ring frees the object once the head's lookup holds
 * it. A free is not for the socket lock to wait out, so the admission puts no version, whatever
 * becomes of the vector: when an entry after the first is refused (ERANGE), the pin of the
 * first stays with the request and the cleanup puts it, with the socket released; and when the
 * block is too short for the ranges and grows, the pins go across the growing and the vector
 * is sent from the versions it pinned, bytes of the freed object included, and the last
 * reference is put by the send after it has let go of the socket. The spy sees every put the
 * request makes, and none may have the socket owned.
 */
static void opaque_pin_vec_owned(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	u8 *source[2] = { kunit_kmalloc(test, 80000, GFP_KERNEL),
			  kunit_kmalloc(test, 80000, GFP_KERNEL) };
	u8 *got = kunit_kmalloc(test, 2000, GFP_KERNEL);
	unsigned long user;
	unsigned int kind, mode;
	struct sock *sk;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source[0]);
	KUNIT_ASSERT_NOT_NULL(test, source[1]);
	KUNIT_ASSERT_NOT_NULL(test, got);
	user = opaque_uvec_map(test);
	opaque_wire_init(test, w);
	sk = w->tx->sk;
	opaque_wire_prep_setup(test, w, store);
	opaque_wire_sndbuf(sk, SZ_1M);
	/* kind 0: the second range is out of B. kind 1: a range in each, over two extents. */
	for (kind = 0; kind < 2; kind++) {
		for (mode = 0; mode < 2; mode++) {
			struct io_opaque_data *a = opaque_sparse_data(test, store);
			struct io_opaque_data *b = opaque_sparse_data(test, store);
			struct io_uring_opaque_send_entry ent[2] = {
				{ .handle = io_opaque_handle(0, 9), .off = kind ? 1500 : 100,
				  .len = 1000 },
				{ .handle = io_opaque_handle(1, 7),
				  .off = kind ? 1500 : b->length - 10, .len = kind ? 1000 : 20 },
			};
			struct io_opaque_req *op;
			int ret;

			opaque_test_publish(store, 0, 9, a);
			opaque_test_publish(store, 1, 7, b);
			opaque_test_copy(a, source[0]);
			opaque_test_copy(b, source[1]);
			opaque_uvec_put(test, user, ent, sizeof(ent));
			op = opaque_wire_prep(test, w, 0, IORING_OPAQUE_SEND_VECTOR, user, 0,
					      ARRAY_SIZE(ent), 0);
			/* The ring recycles a block that was grown: make it short again. */
			kvfree(op->vec);
			op->vec = NULL;
			KUNIT_ASSERT_EQ(test, io_opaque_vec_grow(op, ARRAY_SIZE(ent), 2, 0), 0);
			KUNIT_ASSERT_EQ(test, io_opaque_vec_copy_in(op, (const void __user *)user,
								    ARRAY_SIZE(ent)), 0);
			KUNIT_ASSERT_EQ(test, op->vec->cap_segs, 2U);
			/* The lookup of the first entry holds its reference: free A now. */
			opaque_lk_open_at(test, store, OPAQUE_LK_AFTER, opaque_lk_free_other, 0);
			opaque_lk.sk = sk;
			opaque_put_spy_start(test, sk);
			ret = opaque_wire_issue(w, 0, !mode);
			opaque_lk_close();
			lockdep_assert_none_held_once();
			/* The admission ran with the socket owned, outside RCU. */
			KUNIT_EXPECT_TRUE(test, opaque_lk.owned);
			KUNIT_EXPECT_FALSE(test, opaque_lk.rcu);
			KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
			KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
			KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
			KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
			KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
			if (!kind) {
				KUNIT_EXPECT_EQ(test, ret, -ERANGE);
				KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 0);
				/* Nothing has been put: the request keeps the pin of A. */
				KUNIT_EXPECT_EQ(test, opaque_lk.calls, 1U);
				KUNIT_EXPECT_EQ(test, op->vec->pinned, 1U);
				KUNIT_EXPECT_PTR_EQ(test, op->vec->pin[0], a);
				KUNIT_EXPECT_EQ(test, refcount_read(&a->refs), 1);
				KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 1);
				opaque_put_spy_expect(test, 0, 0);
				KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
						2 * opaque_sparse_held(b));
				/* What the core does with a send that failed. */
				req_set_fail(&w->req[0]);
				io_opaque_fail(&w->req[0]);
			} else {
				KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
				KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 2000);
				/* Both lookups held their references, and neither put one. */
				KUNIT_EXPECT_EQ(test, opaque_lk.calls, 2U);
				/* A's last reference was the send's, put after the socket. */
				opaque_put_spy_expect(test, 1, 1);
				KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
						opaque_sparse_held(b));
				opaque_wire_read(test, w, got, 0, 2000);
				KUNIT_EXPECT_MEMEQ(test, got, source[0] + 1500, 1000);
				KUNIT_EXPECT_MEMEQ(test, got + 1000, source[1] + 1500, 1000);
			}
			/* Cleanup has the rest to give back, with the socket released. */
			opaque_wire_req_cleanup(w, 0);
			opaque_put_spy_stop(test);
			opaque_put_spy_expect(test, 1, 1);
			KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), opaque_sparse_held(b));
			KUNIT_EXPECT_EQ(test, refcount_read(&b->refs), 1);
			opaque_test_free(store, 1);
			KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
			kvfree_rcu_barrier();
		}
	}
	opaque_expect_lockdep(test, store);
}

/*
 * A single range, admitted with the socket owned, takes a reference only if it keeps it: a
 * handle that a slot has since given to another object, and a range that the version does not
 * have, are refused in the lookup, with no reference taken, and so nothing is given back and
 * nothing put. The lookup that does take one is told, once it holds it, that another ring frees
 * the object, which makes the send's pin the last reference, and that send puts it after the
 * socket is released. The other two would otherwise have put it under the socket.
 */
static void opaque_pin_send_owned(struct kunit *test)
{
	static const int errs[] = { -ESTALE, -ERANGE, 0 };
	struct io_opaque_store *store = opaque_test_store(test);
	struct opaque_wire *w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	u8 *source = kunit_kmalloc(test, 80000, GFP_KERNEL);
	u8 *got = kunit_kmalloc(test, 1000, GFP_KERNEL);
	unsigned int kind, mode;
	struct sock *sk;

	KUNIT_ASSERT_NOT_NULL(test, w);
	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, got);
	opaque_wire_init(test, w);
	sk = w->tx->sk;
	opaque_wire_prep_setup(test, w, store);
	opaque_wire_sndbuf(sk, SZ_1M);
	/* kind 0: the slot holds an object of the next generation. 1: out of range. 2: a send. */
	for (kind = 0; kind < ARRAY_SIZE(errs); kind++) {
		for (mode = 0; mode < 2; mode++) {
			struct io_opaque_data *data = opaque_sparse_data(test, store);
			struct io_opaque_req *op;
			int ret;

			opaque_test_publish(store, 0, kind ? 9 : 10, data);
			opaque_test_copy(data, source);
			op = opaque_wire_prep(test, w, 0, 0, io_opaque_handle(0, 9),
					      kind == 1 ? data->length - 10 : 100,
					      kind == 1 ? 20 : 1000, 0);
			opaque_lk_open_at(test, store, OPAQUE_LK_AFTER, opaque_lk_free_other, 0);
			opaque_lk.sk = sk;
			opaque_put_spy_start(test, sk);
			ret = opaque_wire_issue(w, 0, !mode);
			opaque_lk_close();
			lockdep_assert_none_held_once();
			KUNIT_EXPECT_FALSE(test, opaque_test_held(&store->tables));
			KUNIT_EXPECT_FALSE(test, sock_owned_by_user_nocheck(sk));
			KUNIT_EXPECT_PTR_EQ(test, op->tx, NULL);
			KUNIT_EXPECT_EQ(test, opaque_test_txs_busy(store), 0U);
			if (errs[kind]) {
				KUNIT_EXPECT_EQ(test, ret, errs[kind]);
				KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 0);
				/* The lookup took no reference, so it was not told of the FREE. */
				KUNIT_EXPECT_EQ(test, opaque_lk.calls, 0U);
				KUNIT_EXPECT_PTR_EQ(test, op->data, NULL);
				KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), data);
				KUNIT_EXPECT_EQ(test, refcount_read(&data->refs), 1);
				opaque_put_spy_expect(test, 0, 0);
				req_set_fail(&w->req[0]);
				io_opaque_fail(&w->req[0]);
				opaque_wire_req_cleanup(w, 0);
				opaque_put_spy_stop(test);
				opaque_put_spy_expect(test, 0, 0);
				opaque_test_free(store, 0);
			} else {
				KUNIT_ASSERT_EQ(test, ret, IOU_COMPLETE);
				KUNIT_EXPECT_EQ(test, w->req[0].cqe.res, 1000);
				KUNIT_EXPECT_TRUE(test, opaque_lk.owned);
				KUNIT_EXPECT_FALSE(test, opaque_lk.rcu);
				KUNIT_EXPECT_EQ(test, opaque_lk.calls, 1U);
				KUNIT_EXPECT_PTR_EQ(test, opaque_slot_data(store, 0), NULL);
				opaque_wire_req_cleanup(w, 0);
				opaque_put_spy_stop(test);
				/* The send's reference was the last, and went after the socket. */
				opaque_put_spy_expect(test, 1, 1);
				opaque_wire_read(test, w, got, 0, 1000);
				KUNIT_EXPECT_MEMEQ(test, got, source + 100, 1000);
			}
			KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
			KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
			kvfree_rcu_barrier();
		}
	}
	opaque_expect_lockdep(test, store);
}

/*
 * The struct of a version outlives its last reference by a grace period: a reader that has
 * the version from the table when the last reference is put can still read the count, and
 * find it zero. The extents go with the put, as they always did. Under KASAN, a struct that
 * was freed with the put is reported here.
 */
static void opaque_pin_grace(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store), *seen;
	int refs;

	opaque_test_publish(store, 0, 9, data);
	rcu_read_lock();
	seen = rcu_dereference(opaque_test_slot(store, 0)->data);
	opaque_test_free(store, 0);
	refs = refcount_read(&seen->refs);
	rcu_read_unlock();
	KUNIT_EXPECT_PTR_EQ(test, seen, data);
	KUNIT_EXPECT_EQ(test, refs, 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	kvfree_rcu_barrier();
	opaque_expect_lockdep(test, store);
}

#define OPAQUE_PIN_READERS	2
#define OPAQUE_PIN_TASKS	(OPAQUE_PIN_READERS + 1)
#define OPAQUE_PIN_ROUNDS	20000
#define OPAQUE_PIN_BYTES	4000

enum opaque_pin_mode {
	/* A new object each round, which the writer sends LAST. */
	OPAQUE_PIN_LAST,
	/* The same, freed. */
	OPAQUE_PIN_FREE,
	/* One object, whose version the writer replaces by compaction. */
	OPAQUE_PIN_COMPACT,
	/* Four objects, until the store stops. */
	OPAQUE_PIN_STOP,
	/* Vectors read two objects, a new one each round, the one before it freed. */
	OPAQUE_PIN_VEC_FREE,
	/* Vectors read two objects, whose versions the writer replaces by compaction. */
	OPAQUE_PIN_VEC_COMPACT,
};

struct opaque_pin_race;

struct opaque_pin_task {
	struct opaque_pin_race *r;
	unsigned int id;
	/* The lookup in progress was held by the test (opaque_pin_hold()). */
	bool held;
};

struct opaque_pin_race {
	struct io_opaque_store *store;
	enum opaque_pin_mode mode;
	/* The newest handle and the one before it; the readers ask for both. */
	u64 handle, prev;
	unsigned int nr;
	atomic_t ready, go, stop, stopped;
	/*
	 * The test holds the lookups of the readers before they read the table (hold), counts those
	 * it held (parked), and lets them go when the store has stopped (release).
	 */
	atomic_t hold, parked, release;
	/* Lookups that returned a version, found the handle stale, or the store stopped. */
	atomic_long_t ok, stale, shutdown;
	/* Those of the shutdown that had been held, and were told as a lookup that began after. */
	atomic_long_t window;
	/* Anything else, or a version that was not the one asked for. */
	atomic_long_t bad;
	/* Rounds the writer got through. */
	unsigned int rounds;
	struct opaque_pin_task task[OPAQUE_PIN_TASKS];
	struct completion done[OPAQUE_PIN_TASKS];
	/* The requests of the readers of the vector modes. */
	struct io_opaque_req vreq[OPAQUE_PIN_READERS];
};

/*
 * An object of two extents of 2000 bytes, made where an assertion cannot be used: NULL if
 * there was no memory or no room.
 */
static struct io_opaque_data *opaque_pin_object(struct io_opaque_store *store)
{
	struct io_opaque_data *data = io_opaque_data_alloc();
	u32 i;

	if (!data)
		return NULL;
	for (i = 0; i < 2; i++) {
		struct page *page = alloc_page(GFP_KERNEL);
		struct io_opaque_extent *extent;

		if (!page)
			goto fail;
		extent = io_opaque_extent_new(store, page, 0, 2000, i * 2000, 0);
		if (IS_ERR(extent)) {
			__free_page(page);
			goto fail;
		}
		list_add_tail(&extent->list, &data->extents);
		data->length += 2000;
	}
	if (io_opaque_vector(store, data))
		goto fail;
	return data;
fail:
	io_opaque_data_put(store, data);
	return NULL;
}

/* Publish @data in a free slot as RECV_OBJECT does. Returns its handle, or zero. */
static u64 opaque_pin_publish(struct io_opaque_store *store, struct io_opaque_data *data)
{
	struct io_opaque_req op = { .store = store, .data = data, .length = data->length };
	u64 flags = 0;

	if (io_opaque_slot_reserve(&op))
		return 0;
	if (io_opaque_publish(&op, &flags) < 0)
		return 0;
	return op.handle;
}

/* Everything waits for the others, so that no task is over when another starts. */
static void opaque_pin_start(struct opaque_pin_race *r)
{
	atomic_inc(&r->ready);
	while (!atomic_read(&r->go))
		cond_resched();
}

/*
 * The window of the lookups, a little longer in some of them than it takes to get from the read
 * to the reference, so that what the writer does is often done in it.
 */
static atomic_t opaque_pin_bare;

static void opaque_pin_jitter(struct io_opaque_store *store)
{
	u32 r = get_random_u32();

	if (!rcu_read_lock_held())
		atomic_inc(&opaque_pin_bare);

	if (!(r & 7))
		udelay(1 + ((r >> 3) & 3));
}

/*
 * Lookups of a send that does not consume what it names, as fast as they go: of the newest
 * handle and of the one before it, which names a slot the writer has taken over since. A lookup
 * that returns a version has the one it asked for, in full, and the range it asked for; one
 * that does not says that the handle is stale. When the store stops no handle is stale, for
 * none is freed: a lookup is told that the store has stopped, or takes a version.
 */
static int opaque_pin_reader(void *arg)
{
	struct opaque_pin_task *t = arg;
	struct opaque_pin_race *r = t->r;
	unsigned long n = 0;

	opaque_pin_start(r);
	while (!atomic_read(&r->stop)) {
		/* If the store had stopped before this lookup began, it may only fail. */
		bool after = atomic_read(&r->stopped), held;
		u64 h = n & 1 ? READ_ONCE(r->prev) : READ_ONCE(r->handle);
		struct io_opaque_req send = {
			.store = r->store, .offset = 500, .length = 1000,
		};
		int ret;

		if (r->mode == OPAQUE_PIN_STOP)
			h = io_opaque_handle(n % 4, 9);
		send.handle = h;
		ret = io_opaque_send_acquire(&send);
		held = t->held;
		t->held = false;
		if (!ret) {
			/* A lookup that was held while the store stopped has nothing to take. */
			if (after || held || send.consumed || send.data->handle != h ||
			    send.data->length != OPAQUE_PIN_BYTES ||
			    iov_iter_count(&send.iter) != send.length)
				atomic_long_inc(&r->bad);
			io_opaque_data_put(r->store, send.data);
			atomic_long_inc(&r->ok);
		} else if (ret == -ESTALE && r->mode != OPAQUE_PIN_STOP) {
			atomic_long_inc(&r->stale);
		} else if (ret == -ESHUTDOWN && r->mode == OPAQUE_PIN_STOP) {
			atomic_long_inc(&r->shutdown);
			if (held)
				atomic_long_inc(&r->window);
		} else {
			atomic_long_inc(&r->bad);
		}
		if (!(++n & 63))
			cond_resched();
	}
	complete(&r->done[t->id]);
	return 0;
}

/*
 * Hold the lookup of a reader here when the test has asked for it, once: the reader has looked at
 * dead and found the store alive, and has not read its table. It goes on when the test says that
 * the store has stopped, and then finds its slot empty, which it must not call a stale handle.
 */
static void opaque_pin_hold(struct io_opaque_store *store)
{
	struct opaque_pin_task *t;
	struct opaque_pin_race *r;

	/* Only the readers, which the test made with opaque_pin_reader(). */
	if (kthread_func(current) != opaque_pin_reader)
		return;
	t = kthread_data(current);
	r = t->r;
	if (!atomic_read(&r->hold) || t->held || READ_ONCE(store->dead))
		return;
	t->held = true;
	atomic_inc(&r->parked);
	while (!atomic_read(&r->release))
		usleep_range(1000, 2000);
}

/*
 * Objects made, published, told to the readers and then taken away, in the slot that the one
 * before had: sent LAST, which takes the table's reference and puts it, or freed.
 */
static int opaque_pin_writer(void *arg)
{
	struct opaque_pin_task *t = arg;
	struct opaque_pin_race *r = t->r;
	struct io_opaque_store *store = r->store;
	unsigned int n;

	opaque_pin_start(r);
	for (n = 0; n < OPAQUE_PIN_ROUNDS && !atomic_read(&r->stop); n++) {
		struct io_opaque_data *data = opaque_pin_object(store);
		struct io_opaque_req last = {
			.store = store, .length = OPAQUE_PIN_BYTES,
			.send_flags = IORING_OPAQUE_SEND_LAST,
		};
		u64 h;

		if (!data) {
			atomic_long_inc(&r->bad);
			break;
		}
		h = opaque_pin_publish(store, data);
		if (!h) {
			io_opaque_data_put(store, data);
			atomic_long_inc(&r->bad);
			break;
		}
		WRITE_ONCE(r->prev, READ_ONCE(r->handle));
		WRITE_ONCE(r->handle, h);
		udelay(get_random_u32_below(6));
		if (r->mode == OPAQUE_PIN_LAST) {
			last.handle = h;
			if (io_opaque_send_acquire(&last) || !last.consumed ||
			    last.data->handle != h)
				atomic_long_inc(&r->bad);
			else
				io_opaque_data_put(store, last.data);
		} else {
			opaque_test_free(store, 0);
		}
		r->rounds = n + 1;
	}
	complete(&r->done[t->id]);
	return 0;
}

/*
 * One object, which keeps its handle, and a version of it that is replaced over and over: the
 * way compaction does it, with a reference on the version it copies, a copy published in its
 * place and the references put after. The readers never see it go.
 */
static int opaque_pin_compactor(void *arg)
{
	struct opaque_pin_task *t = arg;
	struct opaque_pin_race *r = t->r;
	struct io_opaque_store *store = r->store;
	unsigned int n;

	opaque_pin_start(r);
	for (n = 0; n < OPAQUE_PIN_ROUNDS / 3 && !atomic_read(&r->stop); n++) {
		/* Two objects take turns, if there are two. */
		const bool other = r->mode == OPAQUE_PIN_VEC_COMPACT && (n & 1);
		struct io_opaque_req compact = {
			.store = store, .handle = other ? r->prev : r->handle,
		};
		struct io_opaque_slot *slot;
		int ret;

		spin_lock(&store->tables);
		slot = io_opaque_lookup(store, compact.handle);
		compact.data = slot ? io_opaque_slot_data(store, slot) : NULL;
		if (compact.data)
			refcount_inc(&compact.data->refs);
		spin_unlock(&store->tables);
		if (!compact.data) {
			atomic_long_inc(&r->bad);
			break;
		}
		ret = io_opaque_compact_build(&compact);
		if (!ret)
			ret = io_opaque_compact_publish(&compact);
		if (ret)
			atomic_long_inc(&r->bad);
		io_opaque_data_put(store, compact.data);
		io_opaque_data_put(store, compact.replacement);
		io_opaque_compact_end(&compact);
		udelay(get_random_u32_below(4));
		r->rounds = n + 1;
	}
	complete(&r->done[t->id]);
	return 0;
}

/*
 * What the readers of the vector modes ask for: the newest handle (0) and the one before it (1).
 * The second entry names the handle of the first, and runs on its reference; the third names
 * the other, and the fourth names the first again, which is looked up afresh, and may find the
 * replacement of a compaction that was published since. The second spans two extents, and
 * the vector five segments for its four entries, so that every vector grows its block once.
 */
#define OPAQUE_PIN_VEC_NR	4
#define OPAQUE_PIN_VEC_SEGS	5
#define OPAQUE_PIN_VEC_BYTES	3000

static const struct {
	u8 handle;
	u32 off, len;
} opaque_pin_vec_shape[OPAQUE_PIN_VEC_NR] = {
	{ 0, 100, 1000 }, { 0, 1500, 1000 }, { 1, 0, 500 }, { 0, 2500, 500 },
};

/*
 * An admitted vector is what it was asked to be: each entry has the version of its handle, and the
 * segments are the bytes of the ranges. How many there are depends on the versions, five when
 * all are as they were made and fewer where a compaction has made one dense.
 */
static bool opaque_pin_vec_whole(const struct io_opaque_req *op)
{
	const struct io_opaque_vec *vec = op->vec;
	u32 i, bytes = 0;

	if (op->consumed || vec->pinned != OPAQUE_PIN_VEC_NR ||
	    vec->nr_segs < OPAQUE_PIN_VEC_NR || vec->nr_segs > OPAQUE_PIN_VEC_SEGS ||
	    iov_iter_count(&op->iter) != op->length)
		return false;
	for (i = 0; i < OPAQUE_PIN_VEC_NR; i++)
		if (vec->pin[i]->handle != vec->ent[i].handle ||
		    vec->pin[i]->length != OPAQUE_PIN_BYTES)
			return false;
	for (i = 0; i < vec->nr_segs; i++)
		bytes += vec->bv[i].bv_len;
	return bytes == OPAQUE_PIN_VEC_BYTES && vec->pin[1] == vec->pin[0];
}

/*
 * Admission of vectors, as fast as it goes, over the newest handle and the one before it, in a
 * block that is made short each time. A vector that is admitted is whole, and holds references
 * that its cleanup gives back; one that is not has an ESTALE for a handle that was freed or
 * whose slot was taken, and holds the references it had taken, which its cleanup gives back
 * as well. When no handle is ever freed, no vector fails.
 */
static int opaque_pin_vec_reader(void *arg)
{
	struct opaque_pin_task *t = arg;
	struct opaque_pin_race *r = t->r;
	struct io_opaque_req *op = &r->vreq[t->id];
	unsigned long n = 0;

	op->store = r->store;
	op->send_flags = IORING_OPAQUE_SEND_VECTOR;
	opaque_pin_start(r);
	while (!atomic_read(&r->stop)) {
		const u64 handle[2] = { READ_ONCE(r->handle), READ_ONCE(r->prev) };
		bool whole;
		int ret;
		u32 i;

		kvfree(op->vec);
		op->vec = NULL;
		if (io_opaque_vec_grow(op, OPAQUE_PIN_VEC_NR, OPAQUE_PIN_VEC_NR, 0)) {
			atomic_long_inc(&r->bad);
			break;
		}
		for (i = 0; i < OPAQUE_PIN_VEC_NR; i++)
			op->vec->ent[i] = (struct io_uring_opaque_send_entry) {
				.handle = handle[opaque_pin_vec_shape[i].handle],
				.off = opaque_pin_vec_shape[i].off,
				.len = opaque_pin_vec_shape[i].len,
			};
		io_opaque_vec_commit(op, OPAQUE_PIN_VEC_NR, OPAQUE_PIN_VEC_BYTES, 1000);
		op->consumed = false;
		ret = io_opaque_vec_finish(op, io_opaque_send_acquire(op));
		whole = !ret && opaque_pin_vec_whole(op);
		if (ret == -ESTALE && r->mode == OPAQUE_PIN_VEC_FREE)
			atomic_long_inc(&r->stale);
		else if (whole)
			atomic_long_inc(&r->ok);
		else
			atomic_long_inc(&r->bad);
		/* Request cleanup, which also gives back what a vector that failed had taken. */
		io_opaque_vec_release(op);
		if (op->vec->pinned)
			atomic_long_inc(&r->bad);
		if (!(++n & 63))
			cond_resched();
	}
	kvfree(op->vec);
	op->vec = NULL;
	complete(&r->done[t->id]);
	return 0;
}

/*
 * Two objects at a time: a new one is published and told to the readers, the one before it is
 * freed a moment later, in the slot that the one before that had left. The readers ask for the
 * newest and the previous, so a vector can have both for a while, and its entries are freed or
 * have their slots taken while it is admitted.
 */
static int opaque_pin_vec_writer(void *arg)
{
	struct opaque_pin_task *t = arg;
	struct opaque_pin_race *r = t->r;
	struct io_opaque_store *store = r->store;
	u64 old = READ_ONCE(r->handle);
	unsigned int n;

	opaque_pin_start(r);
	for (n = 0; n < OPAQUE_PIN_ROUNDS && !atomic_read(&r->stop); n++) {
		struct io_opaque_data *data = opaque_pin_object(store);
		u64 h;

		if (!data) {
			atomic_long_inc(&r->bad);
			break;
		}
		h = opaque_pin_publish(store, data);
		if (!h) {
			io_opaque_data_put(store, data);
			atomic_long_inc(&r->bad);
			break;
		}
		WRITE_ONCE(r->prev, old);
		WRITE_ONCE(r->handle, h);
		/* Long enough for a vector to have both, and sometimes not. */
		udelay(get_random_u32_below(6) * 6);
		opaque_test_free(store, old & IO_OPAQUE_INDEX_MASK);
		old = h;
		r->rounds = n + 1;
	}
	opaque_test_free(store, old & IO_OPAQUE_INDEX_MASK);
	complete(&r->done[t->id]);
	return 0;
}

/*
 * Make the readers and, if there is one, the @writer, and let them go when all are running. They
 * are made before any is woken, so that one that cannot be made takes the others down before
 * they have run.
 */
static bool opaque_pin_launch(struct opaque_pin_race *r, int (*writer)(void *))
{
	struct task_struct *task[OPAQUE_PIN_TASKS];
	unsigned int i;

	r->nr = writer ? OPAQUE_PIN_TASKS : OPAQUE_PIN_READERS;
	for (i = 0; i < r->nr; i++) {
		r->task[i] = (struct opaque_pin_task) { .r = r, .id = i };
		init_completion(&r->done[i]);
		task[i] = kthread_create(i >= OPAQUE_PIN_READERS ? writer :
					 r->mode >= OPAQUE_PIN_VEC_FREE ? opaque_pin_vec_reader :
					 opaque_pin_reader, &r->task[i], "opaque_pin%u", i);
		if (IS_ERR(task[i])) {
			while (i--)
				kthread_stop(task[i]);
			return false;
		}
	}
	for (i = 0; i < r->nr; i++)
		wake_up_process(task[i]);
	while (atomic_read(&r->ready) < r->nr)
		cond_resched();
	atomic_set(&r->go, 1);
	return true;
}

/*
 * Wait for the writer, then stop the readers and wait for them. Returns whether the writer was
 * in time; if it was not, the readers stop it too, and it is waited for after them.
 */
static bool opaque_pin_finish(struct opaque_pin_race *r)
{
	bool timely = true;
	unsigned int i;

	if (r->nr > OPAQUE_PIN_READERS &&
	    !wait_for_completion_timeout(&r->done[OPAQUE_PIN_READERS], 60 * HZ))
		timely = false;
	atomic_set(&r->stop, 1);
	for (i = 0; i < OPAQUE_PIN_READERS; i++)
		wait_for_completion(&r->done[i]);
	if (!timely)
		wait_for_completion(&r->done[OPAQUE_PIN_READERS]);
	return timely;
}

/* The hook of the lookups is the jitter, and goes with the test. */
static struct opaque_pin_race *opaque_pin_race_new(struct kunit *test, enum opaque_pin_mode mode)
{
	struct opaque_pin_race *r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, r);
	r->store = opaque_test_store(test);
	/* One slot, so that every round takes over the one the round before had. */
	r->store->config.max_objects = 1;
	r->mode = mode;
	atomic_set(&opaque_pin_bare, 0);
	opaque_lookup_hook(test, opaque_pin_jitter);
	return r;
}

/* What every race expects at its end: nothing wrong, everything given back, the race run. */
static void opaque_pin_expect(struct kunit *test, struct opaque_pin_race *r, long slow)
{
	struct io_opaque_store *store = r->store;

	kunit_info(test,
		   "%u rounds on %u CPUs: %ld lookups took a version, %ld found it stale, %ld were refused by the store, %ld were settled by the table",
		   r->rounds, num_online_cpus(), atomic_long_read(&r->ok),
		   atomic_long_read(&r->stale), atomic_long_read(&r->shutdown),
		   atomic_long_read(&io_opaque_pin_slow) - slow);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&r->bad), 0L);
	/* Every lookup was in an RCU read-side section between the read and the reference. */
	KUNIT_EXPECT_EQ(test, atomic_read(&opaque_pin_bare), 0);
	KUNIT_EXPECT_GT(test, atomic_long_read(&r->ok), 0L);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&store->extents), 0);
	KUNIT_EXPECT_EQ(test, store->nr_objects, 0U);
	opaque_expect_lockdep(test, store);
	kvfree_rcu_barrier();
}

/*
 * Lookups of a handle that is being consumed by a LAST send, which takes the table's reference
 * with tables held, and of the one before it, whose slot has been taken over by the next object.
 * Every lookup gets the version of the handle it asked for, whole, or ESTALE; the objects are
 * all gone, bytes and extents, at the end. It needs two CPUs to race on.
 */
static void opaque_pin_smp_last(struct kunit *test)
{
	struct opaque_pin_race *r;
	long slow = atomic_long_read(&io_opaque_pin_slow);

	opaque_race_needs_cpus(test);
	r = opaque_pin_race_new(test, OPAQUE_PIN_LAST);
	KUNIT_ASSERT_TRUE(test, opaque_pin_launch(r, opaque_pin_writer));
	KUNIT_EXPECT_TRUE(test, opaque_pin_finish(r));
	KUNIT_EXPECT_EQ(test, r->rounds, (unsigned int)OPAQUE_PIN_ROUNDS);
	KUNIT_EXPECT_GT(test, atomic_long_read(&r->stale), 0L);
	KUNIT_EXPECT_GT(test, atomic_long_read(&io_opaque_pin_slow), slow);
	opaque_pin_expect(test, r, slow);
}

/* The same against FREE, which detaches under tables and puts after. It needs two CPUs. */
static void opaque_pin_smp_free(struct kunit *test)
{
	struct opaque_pin_race *r;
	long slow = atomic_long_read(&io_opaque_pin_slow);

	opaque_race_needs_cpus(test);
	r = opaque_pin_race_new(test, OPAQUE_PIN_FREE);
	KUNIT_ASSERT_TRUE(test, opaque_pin_launch(r, opaque_pin_writer));
	KUNIT_EXPECT_TRUE(test, opaque_pin_finish(r));
	KUNIT_EXPECT_EQ(test, r->rounds, (unsigned int)OPAQUE_PIN_ROUNDS);
	KUNIT_EXPECT_GT(test, atomic_long_read(&r->stale), 0L);
	KUNIT_EXPECT_GT(test, atomic_long_read(&io_opaque_pin_slow), slow);
	opaque_pin_expect(test, r, slow);
}

/*
 * Lookups of a handle whose version is replaced by compaction over and over. The handle is
 * never freed, so no lookup may be told it is stale, whatever version it read; and whichever it
 * pins, it is for that handle. Both the old and the new version end at a count of zero. It needs
 * two CPUs to race on.
 */
static void opaque_pin_smp_compact(struct kunit *test)
{
	struct opaque_pin_race *r;
	long slow = atomic_long_read(&io_opaque_pin_slow);

	opaque_race_needs_cpus(test);
	r = opaque_pin_race_new(test, OPAQUE_PIN_COMPACT);
	{
		struct io_opaque_data *data = opaque_pin_object(r->store);

		KUNIT_ASSERT_NOT_NULL(test, data);
		r->handle = opaque_pin_publish(r->store, data);
		KUNIT_ASSERT_NE(test, r->handle, 0ULL);
		/* The readers ask for the newest handle and the one before: here they are one. */
		r->prev = r->handle;
	}
	KUNIT_ASSERT_TRUE(test, opaque_pin_launch(r, opaque_pin_compactor));
	KUNIT_EXPECT_TRUE(test, opaque_pin_finish(r));
	KUNIT_EXPECT_EQ(test, r->rounds, (unsigned int)OPAQUE_PIN_ROUNDS / 3);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&r->stale), 0L);
	KUNIT_EXPECT_EQ(test, atomic64_read(&r->store->compactions), (s64)r->rounds);
	opaque_test_free(r->store, 0);
	opaque_pin_expect(test, r, slow);
	KUNIT_EXPECT_EQ(test, atomic64_read(&r->store->transient), 0LL);
}

/*
 * The store stops while lookups go on: its objects are detached and put, the free of a version
 * left to whichever lookup holds the last reference. A lookup that began after the stop was done
 * fails; the others take a version or are told that the store has stopped, and none gets a
 * version that is not what it asked for or is told that its handle is stale, for none is freed.
 * A stop that lands where a lookup is the most likely to meet it is hard to hit by chance, since
 * dead is set long before the first object is detached, and a reader looks at dead just before
 * it reads the slot: so the test holds the readers there, between the one and the other, stops
 * the store, and lets them go on to find their slots empty (opaque_pin_hold()). Both must have
 * been held, and be told that the store stopped by the table. It needs two CPUs to race on.
 */
static void opaque_pin_smp_stop(struct kunit *test)
{
	struct opaque_pin_race *r;
	long slow = atomic_long_read(&io_opaque_pin_slow), seen;
	unsigned long until;
	u32 i;

	opaque_race_needs_cpus(test);
	r = opaque_pin_race_new(test, OPAQUE_PIN_STOP);
	r->store->config.max_objects = 4;
	opaque_test_stoppable(test, r->store);
	for (i = 0; i < 4; i++) {
		struct io_opaque_data *data = opaque_pin_object(r->store);

		KUNIT_ASSERT_NOT_NULL(test, data);
		opaque_test_publish(r->store, i, 9, data);
	}
	opaque_lookup_hook_at(test, OPAQUE_LK_BEFORE, opaque_pin_hold);
	KUNIT_ASSERT_TRUE(test, opaque_pin_launch(r, NULL));
	until = jiffies + 10 * HZ;
	while (atomic_long_read(&r->ok) < 2000 && time_before(jiffies, until))
		usleep_range(100, 200);
	/* Each reader that comes to its next lookup stays where it has found the store alive. */
	atomic_set(&r->hold, 1);
	until = jiffies + 10 * HZ;
	while (atomic_read(&r->parked) < OPAQUE_PIN_READERS && time_before(jiffies, until))
		usleep_range(100, 200);
	KUNIT_EXPECT_EQ(test, atomic_read(&r->parked), OPAQUE_PIN_READERS);
	io_opaque_stop(r->store);
	atomic_set(&r->stopped, 1);
	atomic_set(&r->release, 1);
	seen = atomic_long_read(&r->shutdown);
	until = jiffies + 10 * HZ;
	while (atomic_long_read(&r->shutdown) < seen + 2000 && time_before(jiffies, until))
		usleep_range(100, 200);
	KUNIT_EXPECT_TRUE(test, opaque_pin_finish(r));
	KUNIT_EXPECT_GE(test, atomic_long_read(&r->shutdown), seen + 2000);
	/* The window was hit: the lookups that were held were told by the table, and only so. */
	KUNIT_EXPECT_EQ(test, atomic_long_read(&r->window), (long)OPAQUE_PIN_READERS);
	KUNIT_EXPECT_GE(test, atomic_long_read(&io_opaque_pin_slow) - slow,
			(long)OPAQUE_PIN_READERS);
	opaque_pin_expect(test, r, slow);
}

/*
 * Vectors against objects that are freed and whose slots are taken by the next: two at a time,
 * the one before the newest freed a moment after the newest is published, and the vectors name
 * both. A vector whose entries are all there is whole, with the version that each handle names
 * and a reference that its cleanup gives back, never a version of the object that took a slot
 * over; any other is told ESTALE and leaves what it had taken for its cleanup. At the end
 * nothing is charged. It needs two CPUs to race on.
 */
static void opaque_pin_smp_vec_free(struct kunit *test)
{
	struct opaque_pin_race *r;
	long slow = atomic_long_read(&io_opaque_pin_slow);
	struct io_opaque_data *data;

	opaque_race_needs_cpus(test);
	r = opaque_pin_race_new(test, OPAQUE_PIN_VEC_FREE);
	r->store->config.max_objects = 2;
	data = opaque_pin_object(r->store);
	KUNIT_ASSERT_NOT_NULL(test, data);
	r->handle = opaque_pin_publish(r->store, data);
	KUNIT_ASSERT_NE(test, r->handle, 0ULL);
	r->prev = r->handle;
	KUNIT_ASSERT_TRUE(test, opaque_pin_launch(r, opaque_pin_vec_writer));
	KUNIT_EXPECT_TRUE(test, opaque_pin_finish(r));
	KUNIT_EXPECT_EQ(test, r->rounds, (unsigned int)OPAQUE_PIN_ROUNDS);
	KUNIT_EXPECT_GT(test, atomic_long_read(&r->stale), 0L);
	KUNIT_EXPECT_GT(test, atomic_long_read(&io_opaque_pin_slow), slow);
	opaque_pin_expect(test, r, slow);
}

/*
 * Vectors against two objects whose versions are replaced by compaction over and over, one and
 * then the other. Neither handle is freed, so no vector may fail, whichever versions its entries
 * find: all four are the versions of the handles they name, and the entry that returns to a handle
 * after the other may find a replacement that the run before it did not. Every version, old and
 * new, ends at a count of zero. It needs two CPUs to race on.
 */
static void opaque_pin_smp_vec_compact(struct kunit *test)
{
	struct opaque_pin_race *r;
	long slow = atomic_long_read(&io_opaque_pin_slow);
	u64 handle[2];
	u32 i;

	opaque_race_needs_cpus(test);
	r = opaque_pin_race_new(test, OPAQUE_PIN_VEC_COMPACT);
	r->store->config.max_objects = 2;
	for (i = 0; i < ARRAY_SIZE(handle); i++) {
		struct io_opaque_data *data = opaque_pin_object(r->store);

		KUNIT_ASSERT_NOT_NULL(test, data);
		handle[i] = opaque_pin_publish(r->store, data);
		KUNIT_ASSERT_NE(test, handle[i], 0ULL);
	}
	r->handle = handle[0];
	r->prev = handle[1];
	KUNIT_ASSERT_TRUE(test, opaque_pin_launch(r, opaque_pin_compactor));
	KUNIT_EXPECT_TRUE(test, opaque_pin_finish(r));
	KUNIT_EXPECT_EQ(test, r->rounds, (unsigned int)OPAQUE_PIN_ROUNDS / 3);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&r->stale), 0L);
	KUNIT_EXPECT_EQ(test, atomic64_read(&r->store->compactions), (s64)r->rounds);
	opaque_test_free(r->store, 0);
	opaque_test_free(r->store, 1);
	opaque_pin_expect(test, r, slow);
	KUNIT_EXPECT_EQ(test, atomic64_read(&r->store->transient), 0LL);
}

/*
 * SEND_ZC's notification and IORING_SEND_ZC_NO_RX_COPY, the counterpart of the managed sends
 * above that a registered-buffer send gets by asking for it. The notifications are built by
 * hand, as io_alloc_notif() and io_send_zc_prep() leave them, with no ring behind them, and
 * every test keeps the reference that the request holds: no completion drops the last one, so
 * no task work is queued and no cqe is posted.
 */
#define SENDZC_TEST_FRAGS	2
#define SENDZC_TEST_FRAG_LEN	100

struct sendzc_test_ring {
	struct io_ring_ctx	*ctx;
	struct io_uring_task	*tctx;
};

/* What the notifications of one test share: only compared, never used. */
static struct sendzc_test_ring *sendzc_test_ring(struct kunit *test)
{
	struct sendzc_test_ring *ring = kunit_kzalloc(test, sizeof(*ring), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ring);
	ring->ctx = kunit_kzalloc(test, sizeof(*ring->ctx), GFP_KERNEL);
	ring->tctx = kunit_kzalloc(test, sizeof(*ring->tctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ring->ctx);
	KUNIT_ASSERT_NOT_NULL(test, ring->tctx);
	return ring;
}

/* A notification as io_send_zc_prep() leaves it, the usage report on. */
static struct io_notif_data *sendzc_test_notif(struct kunit *test,
					       const struct sendzc_test_ring *ring,
					       bool rx_waiver)
{
	struct io_kiocb *notif = kunit_kzalloc(test, sizeof(*notif), GFP_KERNEL);
	struct io_notif_data *nd;

	KUNIT_ASSERT_NOT_NULL(test, notif);
	notif->ctx = ring->ctx;
	notif->tctx = ring->tctx;
	nd = io_notif_to_data(notif);
	nd->head = nd;
	nd->zc_report = true;
	nd->uarg.flags = IO_NOTIF_UBUF_FLAGS | (rx_waiver ? SKBFL_DONT_ORPHAN_RX : 0);
	nd->uarg.ops = &io_ubuf_ops;
	refcount_set(&nd->uarg.refcnt, 1);
	return nd;
}

static void sendzc_test_free_page(void *page)
{
	__free_page(page);
}

/*
 * Fragments of a registered buffer, as io_sg_from_iter() adds them: managed, so that the skb
 * takes no reference on the pages, which the test owns. Frag i is filled with the byte i + 1.
 */
static void sendzc_test_fill(struct kunit *test, struct sk_buff *skb, struct page **pages)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int i;

	shinfo->flags |= SKBFL_MANAGED_FRAG_REFS;
	for (i = 0; i < SENDZC_TEST_FRAGS; i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, pages[i]);
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, sendzc_test_free_page,
								pages[i]), 0);
		memset(page_address(pages[i]), i + 1, PAGE_SIZE);
		__skb_fill_page_desc_noacc(shinfo, i, pages[i], 0, SENDZC_TEST_FRAG_LEN);
	}
	shinfo->nr_frags = SENDZC_TEST_FRAGS;
	skb->len = SENDZC_TEST_FRAGS * SENDZC_TEST_FRAG_LEN;
	skb->data_len = skb->len;
	skb->truesize += SENDZC_TEST_FRAGS * PAGE_SIZE;
}

/*
 * A notification joins the skb of another only if both asked for the same of the receive path:
 * an skb is held by it or copied for it as a whole.
 */
static void sendzc_link_waiver(struct kunit *test)
{
	const struct sendzc_test_ring *ring = sendzc_test_ring(test);
	int prev, cur;

	for (prev = 0; prev < 2; prev++) {
		for (cur = 0; cur < 2; cur++) {
			struct io_notif_data *a = sendzc_test_notif(test, ring, prev);
			struct io_notif_data *b = sendzc_test_notif(test, ring, cur);
			struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);
			u8 waived;

			KUNIT_ASSERT_NOT_NULL(test, skb);
			/* The first ubuf is the skb's, and gives it its bit. */
			KUNIT_ASSERT_EQ(test, io_link_skb(skb, &a->uarg), 0);
			KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(skb), &a->uarg);
			waived = skb_shinfo(skb)->flags & SKBFL_DONT_ORPHAN_RX;
			KUNIT_EXPECT_EQ(test, !!waived, prev);
			KUNIT_EXPECT_EQ(test, refcount_read(&a->uarg.refcnt), 2U);
			/* The same notification joins itself, and takes nothing. */
			KUNIT_EXPECT_EQ(test, io_link_skb(skb, &a->uarg), 0);
			KUNIT_EXPECT_EQ(test, refcount_read(&a->uarg.refcnt), 2U);

			if (prev == cur) {
				/* Chained behind the first, which the skb keeps for both. */
				KUNIT_EXPECT_EQ(test, io_link_skb(skb, &b->uarg), 0);
				KUNIT_EXPECT_PTR_EQ(test, b->head, a);
				KUNIT_EXPECT_PTR_EQ(test, a->next, b);
				KUNIT_EXPECT_EQ(test, refcount_read(&a->uarg.refcnt), 3U);
			} else {
				/* Refused and nothing changed: TCP starts a new skb. */
				KUNIT_EXPECT_EQ(test, io_link_skb(skb, &b->uarg), -EEXIST);
				KUNIT_EXPECT_PTR_EQ(test, b->head, b);
				KUNIT_EXPECT_NULL(test, a->next);
				KUNIT_EXPECT_NULL(test, b->next);
				KUNIT_EXPECT_EQ(test, refcount_read(&a->uarg.refcnt), 2U);
				KUNIT_EXPECT_EQ(test, refcount_read(&b->uarg.refcnt), 1U);
			}
			KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(skb), &a->uarg);
			KUNIT_EXPECT_EQ(test, skb_shinfo(skb)->flags & SKBFL_DONT_ORPHAN_RX,
					waived);

			/* The skb's release is the one completion the test lets run. */
			consume_skb(skb);
			KUNIT_EXPECT_EQ(test, refcount_read(&a->uarg.refcnt),
					prev == cur ? 2U : 1U);
		}
	}
}

struct sendzc_rx_case {
	const char	*name;
	bool		waiver;		/* IORING_SEND_ZC_NO_RX_COPY */
};

static const struct sendzc_rx_case sendzc_rx_cases[] = {
	{ "copied", false },
	{ "held", true },
};

static void sendzc_rx_case_to_desc(const struct sendzc_rx_case *c, char *desc)
{
	strscpy(desc, c->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(sendzc_rx, sendzc_rx_cases, sendzc_rx_case_to_desc);

/*
 * What a packet socket does to the skb of a registered-buffer send: dev_queue_xmit_nit()
 * clones it for the taps and skb_orphan_frags_rx() copies the clone, unless the request waived
 * the copy. Then the tap holds the clone after the device is done with its skb.
 */
static void sendzc_rx_copy(struct kunit *test)
{
	const struct sendzc_rx_case *c = test->param_value;
	struct io_notif_data *nd = sendzc_test_notif(test, sendzc_test_ring(test), c->waiver);
	struct sk_buff *skb = alloc_skb(0, GFP_KERNEL), *tap;
	u8 want[SENDZC_TEST_FRAGS * SENDZC_TEST_FRAG_LEN], data[sizeof(want)];
	struct page *pages[SENDZC_TEST_FRAGS];
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	for (i = 0; i < sizeof(want); i++)
		want[i] = i / SENDZC_TEST_FRAG_LEN + 1;
	/* TCP links the ubuf before it fills the skb, io_sg_from_iter() adds the frags. */
	KUNIT_ASSERT_EQ(test, io_link_skb(skb, &nd->uarg), 0);
	sendzc_test_fill(test, skb, pages);
	KUNIT_ASSERT_EQ(test, refcount_read(&nd->uarg.refcnt), 2U);

	tap = skb_clone(skb, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, tap);
	KUNIT_ASSERT_PTR_EQ(test, skb_zcopy(tap), &nd->uarg);
	KUNIT_EXPECT_EQ(test, skb_orphan_frags_rx(tap, GFP_KERNEL), 0);

	if (c->waiver) {
		/* The tap holds the clone as it is: the buffer's pages, no reference of its own. */
		KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(tap), &nd->uarg);
		KUNIT_ASSERT_EQ(test, skb_shinfo(tap)->nr_frags, SENDZC_TEST_FRAGS);
		for (i = 0; i < SENDZC_TEST_FRAGS; i++) {
			KUNIT_EXPECT_PTR_EQ(test, skb_frag_page(&skb_shinfo(tap)->frags[i]),
					    pages[i]);
			KUNIT_EXPECT_EQ(test, page_count(pages[i]), 1);
		}
		KUNIT_EXPECT_FALSE(test, READ_ONCE(nd->zc_copied));
		/* The device is done with its skb. The clone shares the shinfo, which holds it. */
		consume_skb(skb);
		KUNIT_EXPECT_EQ(test, refcount_read(&nd->uarg.refcnt), 2U);
		KUNIT_EXPECT_FALSE(test, READ_ONCE(nd->zc_used));
		/* The tap reads the buffer, which is unchanged. */
		KUNIT_ASSERT_EQ(test, skb_copy_bits(tap, 0, data, sizeof(data)), 0);
		KUNIT_EXPECT_MEMEQ(test, data, want, sizeof(want));
		/* It lets go: the notification is due now, and says that nothing was copied. */
		consume_skb(tap);
		KUNIT_EXPECT_EQ(test, refcount_read(&nd->uarg.refcnt), 1U);
		KUNIT_EXPECT_TRUE(test, READ_ONCE(nd->zc_used));
		KUNIT_EXPECT_FALSE(test, READ_ONCE(nd->zc_copied));
	} else {
		/* The tap holds a copy: pages of its own, the bytes, and the notification told. */
		KUNIT_EXPECT_NULL(test, skb_zcopy(tap));
		KUNIT_EXPECT_EQ(test, skb_shinfo(tap)->flags & SKBFL_ALL_ZEROCOPY, 0);
		KUNIT_ASSERT_EQ(test, skb_shinfo(tap)->nr_frags, 1);
		for (i = 0; i < SENDZC_TEST_FRAGS; i++)
			KUNIT_EXPECT_PTR_NE(test, skb_frag_page(&skb_shinfo(tap)->frags[0]),
					    pages[i]);
		KUNIT_ASSERT_EQ(test, skb_copy_bits(tap, 0, data, sizeof(data)), 0);
		KUNIT_EXPECT_MEMEQ(test, data, want, sizeof(want));
		KUNIT_EXPECT_TRUE(test, READ_ONCE(nd->zc_copied));
		/* The device's skb still holds the notification, and the tap no longer does. */
		KUNIT_EXPECT_EQ(test, refcount_read(&nd->uarg.refcnt), 2U);
		consume_skb(tap);
		KUNIT_EXPECT_EQ(test, refcount_read(&nd->uarg.refcnt), 2U);
		consume_skb(skb);
		KUNIT_EXPECT_EQ(test, refcount_read(&nd->uarg.refcnt), 1U);
		KUNIT_EXPECT_TRUE(test, READ_ONCE(nd->zc_copied));
	}
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
	KUNIT_CASE(opaque_block_lifetime),
	KUNIT_CASE(opaque_block_charge),
	KUNIT_CASE(opaque_block_charge_limit),
	KUNIT_CASE(opaque_split_charge),
	KUNIT_CASE(opaque_chunk_alone_charge),
	KUNIT_CASE(opaque_second_extent_charge),
	KUNIT_CASE(opaque_vector_charge),
	KUNIT_CASE(opaque_stat_extents_clamp),
	KUNIT_CASE(opaque_release_pending),
	KUNIT_CASE(opaque_hold_flags),
#if IS_ENABLED(CONFIG_PAGE_POOL)
	KUNIT_CASE(opaque_capture_pool_page),
	KUNIT_CASE(opaque_capture_pool_head),
	KUNIT_CASE(opaque_capture_pool_other),
	KUNIT_CASE(opaque_steal_whole_skb),
	KUNIT_CASE(opaque_steal_partial_call),
	KUNIT_CASE(opaque_steal_straddle),
	KUNIT_CASE(opaque_steal_cloned_copies),
	KUNIT_CASE(opaque_steal_mixed_discard),
	KUNIT_CASE(opaque_steal_fraglist),
	KUNIT_CASE(opaque_steal_fraglist_whole),
	KUNIT_CASE(opaque_steal_head_pool_ref),
	KUNIT_CASE(opaque_steal_same_page_two_skbs),
	KUNIT_CASE(opaque_steal_kind_mismatch),
	KUNIT_CASE(opaque_steal_split_fragment),
	KUNIT_CASE(opaque_steal_soft_batch),
	KUNIT_CASE(opaque_steal_overflow),
	KUNIT_CASE(opaque_steal_no_room),
	KUNIT_CASE(opaque_steal_head_frag_same_page),
	KUNIT_CASE(opaque_steal_overflow_same_backing),
	KUNIT_CASE(opaque_steal_remote_completion),
#endif
	KUNIT_CASE(opaque_release_units),
	KUNIT_CASE(opaque_capture_page_stats),
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
	KUNIT_CASE(opaque_tx_failed_long),
	KUNIT_CASE_SLOW(opaque_tx_failed_smp),
	KUNIT_CASE(opaque_tx_idle_cache),
	KUNIT_CASE(opaque_lock_held_check),
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
	KUNIT_CASE(opaque_send_prep_fields),
	KUNIT_CASE(opaque_send_copy_policy),
	KUNIT_CASE(opaque_send_loopback),
	KUNIT_CASE(opaque_send_locked_path),
	KUNIT_CASE(opaque_send_msghdr_parts),
	KUNIT_CASE(opaque_send_locked_eagain),
	KUNIT_CASE(opaque_send_locked_short),
	KUNIT_CASE(opaque_send_locked_error),
	KUNIT_CASE(opaque_send_locked_last_versions),
	KUNIT_CASE(opaque_send_fallback),
	KUNIT_CASE(opaque_send_security_refusal),
	KUNIT_CASE(opaque_send_join_place),
	KUNIT_CASE(opaque_send_parked_under_socket),
	KUNIT_CASE(opaque_send_release_once),
	KUNIT_CASE(opaque_send_locked_admit_fails),
	KUNIT_CASE(opaque_send_direct_switch),
	KUNIT_CASE(opaque_send_locked_blocking),
	KUNIT_CASE(opaque_attach_lock_order),
	KUNIT_CASE(opaque_send_vec_check),
	KUNIT_CASE(opaque_vec_copy_in),
	KUNIT_CASE(opaque_vec_copy_in_invalid),
	KUNIT_CASE(opaque_vec_copy_in_fault),
	KUNIT_CASE(opaque_vec_copy_in_model),
	KUNIT_CASE(opaque_send_vec_acquire),
	KUNIT_CASE(opaque_send_vec_pin_once),
	KUNIT_CASE(opaque_send_vec_iter),
	KUNIT_CASE(opaque_send_vec_loopback),
	KUNIT_CASE(opaque_send_vec_admit_fail),
	KUNIT_CASE(opaque_tx_refused_owned),
	KUNIT_CASE(opaque_send_vec_resume),
	KUNIT_CASE(opaque_vector_anchors),
	KUNIT_CASE(opaque_send_vec_seek_bound),
	KUNIT_CASE(opaque_send_vec_count_cap),
	KUNIT_CASE(opaque_send_single_seek),
	KUNIT_CASE(opaque_send_vec_grow_retry),
	KUNIT_CASE(opaque_send_vec_cursor_pinned),
	KUNIT_CASE(opaque_send_vec_one_pass),
	KUNIT_CASE(opaque_send_vec_acquire_fails),
	KUNIT_CASE(opaque_pin_handles),
	KUNIT_CASE(opaque_pin_retired),
	KUNIT_CASE(opaque_pin_retired_send),
	KUNIT_CASE(opaque_pin_stop_window),
	KUNIT_CASE(opaque_pin_compacted),
	KUNIT_CASE(opaque_pin_reused),
	KUNIT_CASE(opaque_pin_slow_free),
	KUNIT_CASE(opaque_pin_vec_run),
	KUNIT_CASE(opaque_pin_vec_failed),
	KUNIT_CASE(opaque_pin_vec_owned),
	KUNIT_CASE(opaque_pin_send_owned),
	KUNIT_CASE(opaque_pin_grace),
	KUNIT_CASE(opaque_send_vec_prep),
	KUNIT_CASE(opaque_send_vec_prep_errors),
	KUNIT_CASE(opaque_cleanup_calls),
	KUNIT_CASE(opaque_send_vec_prep_grows),
	KUNIT_CASE(opaque_policy_chunks),
	KUNIT_CASE(opaque_extents_free_wakes_once),
	KUNIT_CASE(opaque_extents_free_wakes_all),
	KUNIT_CASE(opaque_params_sysfs),
#ifdef CONFIG_IO_URING_ZCRX_KUNIT_TEST
	KUNIT_CASE(opaque_maio_bind),
	KUNIT_CASE(opaque_maio_pool_ref),
	KUNIT_CASE(opaque_maio_pool_units),
	KUNIT_CASE(opaque_maio_steal),
	KUNIT_CASE(opaque_maio_page_ref),
	KUNIT_CASE(opaque_maio_copy),
	KUNIT_CASE(opaque_maio_shared_copy),
	KUNIT_CASE(opaque_maio_fallback_copy),
	KUNIT_CASE(opaque_maio_extents),
	KUNIT_CASE(opaque_maio_exhausted),
	KUNIT_CASE(opaque_maio_compact),
	KUNIT_CASE(opaque_maio_create),
	KUNIT_CASE(opaque_maio_managed_shared_frag),
	KUNIT_CASE(opaque_maio_send_managed_shared_frag),
	KUNIT_CASE(opaque_maio_send_managed_two_stores),
	KUNIT_CASE(opaque_maio_managed_shared_csum),
	KUNIT_CASE(opaque_maio_send_managed_probe),
	KUNIT_CASE(opaque_maio_unbound_flag),
	KUNIT_CASE(opaque_maio_unbound_managed_shared_frag),
	KUNIT_CASE(opaque_maio_unbound_send_managed_shared_frag),
	KUNIT_CASE(opaque_maio_unbound_managed_shared_csum),
	KUNIT_CASE(opaque_maio_unbound_send_managed_probe),
	KUNIT_CASE(opaque_maio_unbound_send_managed_flip),
#endif
	KUNIT_CASE(opaque_send_managed_loopback),
	KUNIT_CASE(opaque_send_managed_holds_unit),
	KUNIT_CASE(opaque_send_managed_copied_without_waiver),
	KUNIT_CASE(opaque_send_managed_prefix_fallback),
	KUNIT_CASE(opaque_send_managed_mixed),
	KUNIT_CASE(opaque_send_managed_eagain),
	KUNIT_CASE(opaque_send_managed_vec_distinct),
	KUNIT_CASE(opaque_send_managed_ordinary_path),
	KUNIT_CASE(opaque_managed_fill_runs),
	KUNIT_CASE(opaque_managed_fill_adjacent),
	KUNIT_CASE(opaque_managed_fill_limits),
	KUNIT_CASE(opaque_managed_fill_folios),
	KUNIT_CASE(opaque_managed_fill_cursor),
	KUNIT_CASE(opaque_managed_fill_at_limit),
	KUNIT_CASE(opaque_managed_fill_objects),
	KUNIT_CASE(opaque_managed_fill_captured),
	KUNIT_CASE(opaque_managed_fill_fallback),
	KUNIT_CASE(opaque_managed_link),
	KUNIT_CASE(opaque_managed_link_full),
	KUNIT_CASE(opaque_managed_boundary),
	KUNIT_CASE(opaque_managed_shared_frag),
	KUNIT_CASE(opaque_managed_shared_csum),
	KUNIT_CASE(opaque_managed_link_binding),
	KUNIT_CASE(opaque_send_managed_shared_frag),
	KUNIT_CASE(opaque_send_managed_probe),
	KUNIT_CASE(opaque_ubuf_complete_atomic),
	KUNIT_CASE(opaque_ubuf_pool_reuse),
	KUNIT_CASE(opaque_ubuf_pool_share),
	KUNIT_CASE(opaque_ubuf_pool_bound),
	KUNIT_CASE(opaque_ubuf_pool_teardown),
	KUNIT_CASE(opaque_ubuf_pool_store),
	KUNIT_CASE(opaque_ubuf_pool_atomic),
	KUNIT_CASE(opaque_send_managed_pool),
	KUNIT_CASE_SLOW(opaque_ubuf_pool_smp_stress),
	KUNIT_CASE_SLOW(opaque_ubuf_pool_smp_close),
	KUNIT_CASE_SLOW(opaque_pin_smp_last),
	KUNIT_CASE_SLOW(opaque_pin_smp_free),
	KUNIT_CASE_SLOW(opaque_pin_smp_compact),
	KUNIT_CASE_SLOW(opaque_pin_smp_stop),
	KUNIT_CASE_SLOW(opaque_pin_smp_vec_free),
	KUNIT_CASE_SLOW(opaque_pin_smp_vec_compact),
	KUNIT_CASE(sendzc_link_waiver),
	KUNIT_CASE_PARAM(sendzc_rx_copy, sendzc_rx_gen_params),
	{}
};

static struct kunit_suite opaque_suite = {
	.name = "io_uring-opaque",
	.test_cases = opaque_cases,
};

kunit_test_suite(opaque_suite);
