/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IO_URING_OPAQUE_INTERNAL_H
#define IO_URING_OPAQUE_INTERNAL_H

#include <linux/bitmap.h>
#include <linux/bvec.h>
#include <linux/net.h>
#include <linux/percpu.h>
#include <linux/sizes.h>
#include <linux/workqueue.h>
#include <kunit/visibility.h>
#include <uapi/linux/io_uring/opaque.h>

#include "opaque.h"

#define IO_OPAQUE_READ_MAX	SZ_64K
#define IO_OPAQUE_RX_BATCH	SZ_256K
#define IO_OPAQUE_MAX_ORDER	(PAGE_SHIFT < 16 ? 16 - PAGE_SHIFT : 0)
#define IO_OPAQUE_ALLOC_RETRY_MS	50
#define IO_OPAQUE_WAKE_BATCH	32
#define IO_OPAQUE_INDEX_BITS	14
#define IO_OPAQUE_INDEX_MASK	((1U << IO_OPAQUE_INDEX_BITS) - 1)
#define IO_OPAQUE_GENERATION_MAX	(U64_MAX >> IO_OPAQUE_INDEX_BITS)
/* Default destination unit: 32 KiB, the largest per-CPU cached buddy order. */
#define IO_OPAQUE_UNIT_ORDER	(PAGE_SHIFT < 15 ? 15 - PAGE_SHIFT : 0)
/* Cooling units examined per take after the idle list runs dry. */
#define IO_OPAQUE_UNIT_PROBES	4
/* A cooling unit still referenced after this long is released, not reused. */
#define IO_OPAQUE_UNIT_COOL_MAX	HZ

struct io_opaque_backing {
	refcount_t refs;
	struct page *page;
	u64 charge;
	u32 filled;
	bool copied;
	bool accounted;
	/* Compaction or CREATE destination the unit cache may keep. */
	bool owned;
};

struct io_opaque_extent {
	struct list_head list;
	struct io_opaque_backing *backing;
	u64 start;
	u32 offset;
	u32 length;
	bool embedded;
};

/* One allocation for the common capture; splits allocate only an extent. */
struct io_opaque_chunk {
	struct io_opaque_backing backing;
	struct io_opaque_extent extent;
};

struct io_opaque_data {
	refcount_t refs;
	struct list_head extents;
	struct bio_vec *bvec;
	u64 length;
	u64 charge;
	u32 nr;
	bool dense;
	bool forward_only;
};

struct io_opaque_slot {
	struct io_opaque_data *data;
	u64 generation;
	bool reserved;
	bool compacting;
	struct list_head candidate;
	unsigned long born;
	unsigned long retry;
};

struct io_opaque_stream_slot {
	struct io_opaque_stream *stream;
	u64 generation;
};

struct io_opaque_tx {
	struct rb_node node;
	struct sock *sk;
	struct list_head requests;
	bool failed;
};

struct io_opaque_stream {
	refcount_t refs;
	/* Extents, range decisions and collector state. */
	struct mutex lock;
	struct io_opaque_store *store;
	struct file *file;
	struct sock_rx_owner owner;
	struct list_head extents;
	struct list_head reads;
	struct rb_root_cached decisions;
	struct io_opaque_req *collector;
	u64 token;
	u64 next;
	u64 undecided;
	u64 need_bytes;
	int error;
	bool eof;
	bool closed;
	/* Collection pass may not take undecided bytes off the socket. */
	bool hold_undecided;
};

/*
 * Whole destination units whose last extent reference has dropped. Units
 * are linked through page->lru of the compound head; page->private holds the
 * retirement time of a cooling unit. Idle units have no other reference and
 * are ready to write. Cooling units are still referenced elsewhere (TCP
 * fragments, clones, retransmissions) and become idle once only the cache
 * holds them. The cache keeps its units charged to the store owner's memcg
 * but outside the hard limit.
 */
struct io_opaque_unit_cache {
	spinlock_t lock;
	struct list_head idle;		/* LIFO: most recently written first */
	struct list_head cooling;	/* FIFO by retirement time */
	u32 nr_idle;
	u32 nr_cooling;
	/* Fixed at store creation; a zero limit disables the cache. */
	u32 limit;
	u32 order;
	/* Statistics, under lock. */
	u64 hits;
	u64 misses;
	u64 cooled;
	u64 cooling_hits;
	u64 probes_failed;
	u64 expired;
	u64 trimmed;
};

/* Global counters, exported read-only under /sys/module/io_uring_opaque/. */
enum io_opaque_stat_item {
	IO_OPAQUE_STAT_CACHE_HITS,
	IO_OPAQUE_STAT_CACHE_MISSES,
	IO_OPAQUE_STAT_CACHE_FALLBACKS,
	IO_OPAQUE_STAT_CACHE_REMOTE,
	IO_OPAQUE_STAT_CACHE_RETIRED,
	IO_OPAQUE_STAT_CACHE_COOLED,
	IO_OPAQUE_STAT_CACHE_COOLING_HITS,
	IO_OPAQUE_STAT_CACHE_PROBES_FAILED,
	IO_OPAQUE_STAT_CACHE_EXPIRED,
	IO_OPAQUE_STAT_CACHE_TRIMMED,
	IO_OPAQUE_STAT_CACHE_DRAINED,
	IO_OPAQUE_STAT_CACHED_BYTES,
	IO_OPAQUE_STAT_COMPACT_INLINE,
	IO_OPAQUE_STAT_COMPACT_WORKER,
	IO_OPAQUE_STAT_COMPACT_AUTO,
	IO_OPAQUE_STAT_COMPACT_BYTES,
	IO_OPAQUE_STAT_NR,
};

struct io_opaque_stats {
	u64 item[IO_OPAQUE_STAT_NR];
};

DECLARE_PER_CPU(struct io_opaque_stats, io_opaque_stats);

static inline void io_opaque_count(enum io_opaque_stat_item item, u64 delta)
{
	this_cpu_add(io_opaque_stats.item[item], delta);
}

u64 io_opaque_stat_read(enum io_opaque_stat_item item);

/* Evaluation tunables; module parameters, not ABI. */
extern unsigned long io_opaque_cache_bytes;
extern unsigned int io_opaque_unit_order;
extern unsigned int io_opaque_inline_compact_max;
extern bool io_opaque_copy_per_page;

struct io_opaque_store {
	refcount_t refs;
	refcount_t users;
	struct io_uring_opaque_config config;
	/* Handles, candidates and TX queues; never held during copies or I/O. */
	struct mutex tables;
	struct io_opaque_slot *objects;
	struct io_opaque_stream_slot *streams;
	unsigned long *object_used;
	unsigned long *stream_used;
	u32 object_cursor;
	u32 stream_cursor;
	u32 nr_objects;
	u32 nr_streams;
	struct rb_root txs;
	struct user_struct *user;
	struct mm_struct *mm;
	struct mem_cgroup *memcg;
	struct obj_cgroup *objcg;
	atomic64_t bytes;
	atomic64_t copied;
	atomic64_t compacted;
	atomic64_t compactions;
	atomic64_t framing;
	atomic64_t framing_copied;
	/* Framing held in the reply reserve, outside bytes. */
	atomic64_t reserve_framing;
	/*
	 * Charged in bytes but not payload held: unpublished compaction copies
	 * and framing in ordinary capacity. Watermarks subtract it. Changes
	 * that move bytes and transient together are one seqcount write.
	 */
	atomic64_t transient;
	/* Serializes transient_seq writers. */
	spinlock_t transient_lock;
	seqcount_spinlock_t transient_seq;
	atomic_t extents;
	atomic_t requests;
	atomic_t memory_mode;
	/* Budget waiters and remote wakeup/cancellation arbitration. */
	spinlock_t wait_lock;
	struct list_head budget_waits;
	struct list_head copies;
	/* One dispatcher serializes manual and automatic copies. */
	struct delayed_work compact_work;
	struct delayed_work retry_work;
	struct list_head candidates;
	struct io_uring_opaque_policy policy;
	u64 tokens;
	unsigned long token_time;
	unsigned long auto_at;
	int dead;
	struct io_opaque_unit_cache cache;
};

/* Locked-memory pages reserved for the unit cache at registration. */
static inline unsigned long io_opaque_cache_pages(struct io_opaque_store *store)
{
	return (unsigned long)store->cache.limit << store->cache.order;
}

bool io_opaque_charge(struct io_opaque_store *store, u64 bytes, bool rx);
void io_opaque_uncharge(struct io_opaque_store *store, u64 bytes);
bool io_opaque_charge_transient(struct io_opaque_store *store, u64 bytes, u64 limit);
void io_opaque_release_transient(struct io_opaque_store *store, u64 uncharge,
				 u64 transient);
bool io_opaque_charge_frame(struct io_opaque_store *store, u64 bytes, bool *reserved);
void io_opaque_uncharge_frame(struct io_opaque_store *store, u64 bytes, bool reserved);
int io_opaque_memory_mode(struct io_opaque_store *store);

static inline u64 io_opaque_rx_limit(struct io_opaque_store *store)
{
	return store->config.hard_limit - store->config.compact_headroom -
	       store->config.reply_reserve;
}

struct io_opaque_extent *io_opaque_extent_new(struct io_opaque_store *store,
					      struct page *page, u32 offset,
					      u32 length, u64 start,
					      bool reserved);
struct io_opaque_data *io_opaque_data_alloc(void);
void io_opaque_data_put(struct io_opaque_store *store,
			struct io_opaque_data *data);
int io_opaque_vector(struct io_opaque_data *data);
struct page *io_opaque_owned_page(unsigned int *order);
void io_opaque_unit_cache_init(struct io_opaque_store *store, u32 limit, u32 order);
void io_opaque_unit_cache_setup(struct io_opaque_store *store);
void io_opaque_unit_put(struct io_opaque_store *store, struct page *page);
void io_opaque_unit_drain(struct io_opaque_store *store);
struct page *io_opaque_dest_page(struct io_opaque_store *store, u64 pages_left,
				 unsigned int *order);
u64 io_opaque_handle(u32 index, u64 generation);
struct io_opaque_slot *io_opaque_lookup(struct io_opaque_store *store,
					u64 handle);
void io_opaque_wait(struct io_opaque_req *op, enum io_opaque_phase phase);
void io_opaque_queue_ready(struct io_opaque_req *op, int result);
int io_opaque_compact_build(struct io_opaque_req *op);
int io_opaque_compact_publish(struct io_opaque_req *op);
void io_opaque_compact_end(struct io_opaque_req *op);
void io_opaque_compact_work(struct work_struct *work);
int io_opaque_compact_start(struct io_opaque_req *op);
int io_opaque_set_policy(struct io_opaque_req *op);

#if IS_ENABLED(CONFIG_KUNIT)
void io_opaque_release_slot(unsigned long *used, u32 index, u64 generation);
void io_opaque_extent_free(struct io_opaque_store *store,
			   struct io_opaque_extent *extent);
void io_opaque_extents_free(struct io_opaque_store *store,
			    struct list_head *list);
struct io_opaque_data *io_opaque_detach(struct io_opaque_store *store,
					struct io_opaque_slot *slot);
int io_opaque_slot_reserve(struct io_opaque_req *op);
int io_opaque_publish(struct io_opaque_req *op, u64 *object_flags);
void io_opaque_create_release(struct io_opaque_req *op);
u32 io_opaque_find_slot(unsigned long *used, u32 count, u32 *cursor);
struct io_opaque_extent *io_opaque_extent_split(struct io_opaque_store *store,
						struct io_opaque_extent *extent,
						u64 at);
int io_opaque_available(struct io_opaque_stream *stream, u64 off, u64 len);
int io_opaque_take(struct io_opaque_req *op);
void io_opaque_restore(struct io_opaque_req *op);
int io_opaque_range_insert(struct io_opaque_req *op);
int io_opaque_range_start(struct io_opaque_req *op);
void io_opaque_retry_work(struct work_struct *work);
void io_opaque_budget_wait(struct io_opaque_req *op, int error);
void io_opaque_stream_close(struct io_opaque_stream *stream);
int io_opaque_capture(struct io_opaque_stream *stream,
		      const struct sk_buff *skb, u32 offset, u32 len,
		      bool shared);
int io_opaque_actor(read_descriptor_t *desc, struct sk_buff *skb,
		    unsigned int offset, size_t length);
bool io_opaque_collect_demand(struct io_opaque_stream *stream);
int io_opaque_send_acquire(struct io_opaque_req *op);
int io_opaque_create_build(struct io_opaque_req *op);
bool io_opaque_unit_retire(struct io_opaque_store *store, struct page *page);
struct page *io_opaque_unit_take(struct io_opaque_store *store);
#endif
#endif
