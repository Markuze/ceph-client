/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IO_URING_OPAQUE_INTERNAL_H
#define IO_URING_OPAQUE_INTERNAL_H

#include <linux/bitmap.h>
#include <linux/bvec.h>
#include <linux/llist.h>
#include <linux/net.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/sizes.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <kunit/visibility.h>
#include <uapi/linux/io_uring/opaque.h>

#include "opaque.h"

#define IO_OPAQUE_READ_MAX	SZ_64K
#define IO_OPAQUE_RX_BATCH	SZ_256K
#define IO_OPAQUE_MAX_ORDER	(PAGE_SHIFT < 16 ? 16 - PAGE_SHIFT : 0)
#define IO_OPAQUE_ALLOC_RETRY_MS	50
/* Waiters looked at in one hold of wait_lock, or followers of a failed queue in one of tx_lock. */
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
/* Unused per-socket TX queues kept per store; the oldest is freed beyond this. */
#define IO_OPAQUE_TX_IDLE_MAX	64
/* Backing segments all ranges of one vectored send may span; more is EMSGSIZE. */
#define IO_OPAQUE_SEND_VEC_SEGS	4096
/*
 * A version of more segments than this records where every this-many-th of
 * them starts, so that a range is found with a walk of about this many steps
 * and a binary search, however large the version is and wherever the walk
 * began. A smaller one is walked, which is as short.
 */
#define IO_OPAQUE_SEEK_STRIDE	16
/* Slots of the table of handles that a policy change walks while it holds tables. */
#define IO_OPAQUE_POLICY_CHUNK	256
/* Backings that one call of the actor leaves undecided; more settle at once. */
#define IO_OPAQUE_PENDING_MAX	64

/* What io_opaque_tx_join() returns besides an error. */
/* Queued behind another frame and parked, with its references taken. */
#define IO_OPAQUE_TX_WAIT	0
/* The head of its queue, with its references taken. */
#define IO_OPAQUE_TX_HEAD	1
/* The head of its queue, still to take its references. */
#define IO_OPAQUE_TX_SETTLE	2

struct io_zcrx_ifq;

/*
 * What a backing holds of its page, which is how the last release gives it
 * back. The unit is one reference of that kind.
 */
enum io_opaque_ref {
	/*
	 * Nothing yet: capture has not decided whether the skb passes its
	 * reference on or the backing takes one. No backing is released in
	 * this state.
	 */
	IO_OPAQUE_REF_PENDING,
	/* A page reference, dropped with folio_put_refs(). */
	IO_OPAQUE_REF_PAGE,
	/*
	 * A page pool fragment reference, as the stack takes for an skb marked
	 * for recycling. The last one hands the page back to its pool.
	 */
	IO_OPAQUE_REF_PP,
	/*
	 * A pool reference of the MAIO instance that the store binds. The
	 * last one goes through io_zcrx_maio_page_put().
	 */
	IO_OPAQUE_REF_MAIO,
	/* A destination unit of compaction or CREATE, which the unit cache may keep. */
	IO_OPAQUE_REF_UNIT,
};

struct io_opaque_backing {
	refcount_t refs;
	u32 filled;
	struct page *page;
	u64 charge;
	/* How many references of kind @ref the backing holds on the page. */
	u16 units;
	/* An enum io_opaque_ref. */
	u8 ref;
	/* While @ref is pending, the kind of reference that settles it. */
	u8 want;
	bool copied;
	bool accounted;
	/* The page is in the area of the MAIO instance that the store binds. */
	bool area;
};

struct io_opaque_extent {
	struct list_head list;
	struct io_opaque_backing *backing;
	u64 start;
	u32 offset;
	u32 length;
	bool embedded;
};

struct io_opaque_block;

/* The common capture: a backing and its first extent. A split allocates only an extent. */
struct io_opaque_chunk {
	struct io_opaque_backing backing;
	struct io_opaque_extent extent;
	struct io_opaque_block *block;
};

/*
 * Chunks are allocated in blocks, so that a stream that keeps a page per
 * fragment allocates once per IO_OPAQUE_BLOCK_CHUNKS pages and not once per
 * page. @live counts what keeps the block: a chunk until its backing is
 * released, and in the current block of a stream the chunks that are still to
 * be handed out and the hold of the stream. The last to let go frees the block.
 * A chunk of CREATE or compaction is a block of its own.
 */
struct io_opaque_block {
	refcount_t live;
	u32 nr;
	/* Chunks handed out; changed by the stream that owns the block, under its lock. */
	u32 used;
	struct io_opaque_chunk chunk[];
};

/*
 * Chunks, and so backings, that a stream allocates at a time: as many as fit in
 * an object of 4 KiB with the header of the block. A smaller block leaves part of
 * the object unused, and the next size class is twice as large.
 */
#define IO_OPAQUE_BLOCK_CHUNKS \
	((int)((SZ_4K - sizeof(struct io_opaque_block)) / sizeof(struct io_opaque_chunk)))

struct io_opaque_data {
	refcount_t refs;
	/*
	 * The handle that names this version. Set before the version is published and
	 * never changed, so that a lookup that holds neither tables nor a reference can
	 * tell whether the slot it read the version from still holds what the handle
	 * names. The lookup (io_opaque_pin()) reads it, and the length, in an RCU
	 * read-side section before it takes a reference. Neither changes after
	 * publication, and the struct outlives its last reference by a grace period
	 * (@rcu), so what it reads is what the version was, even if it was put by
	 * everyone since.
	 */
	u64 handle;
	/* The struct outlives its last reference by a grace period, see io_opaque_data_put(). */
	struct rcu_head rcu;
	struct list_head extents;
	struct bio_vec *bvec;
	/*
	 * Where segments 0, IO_OPAQUE_SEEK_STRIDE, twice that and so on start,
	 * as bytes into the object, when it has more than that many segments;
	 * else NULL. It is allocated with bvec and freed with it.
	 */
	u64 *anchor;
	u64 length;
	u64 charge;
	u32 nr;
	bool dense;
	bool forward_only;
	/*
	 * Every extent is in the area of the MAIO instance of the store. Set
	 * when the vector is built. A dense copy would move the bytes out of
	 * the area, so compaction skips these objects.
	 */
	bool mapped;
};

struct io_opaque_slot {
	/*
	 * The version that the handle names. It is changed with tables held, and read with
	 * it, or in RCU by the lookup of a send that does not consume the object
	 * (io_opaque_pin()).
	 */
	struct io_opaque_data __rcu *data;
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
	/* Admitted requests that have not queued their whole frame; the head sends. */
	struct list_head requests;
	/* On store->txs_idle while no request is queued or arriving. */
	struct list_head idle;
	/* Requests between lookup and enqueue; the queue stays while any. */
	unsigned int admitting;
	bool failed;
};

/*
 * What a vectored send needs besides the request: its entries, copied at prep,
 * the version each pins and the segments of the combined cursor, which
 * admission stores as it finds the ranges. One block holds the header and the
 * three arrays, so that growing it is one allocation.
 */
struct io_opaque_vec {
	/* Entries, from the SQE. */
	u32 nr;
	/* Segments of the combined cursor, from admission. */
	u32 nr_segs;
	/*
	 * Entries of pin[] that admission has set. The version of each is kept
	 * alive by the send, by a reference of the entry's own or, unless the
	 * vector is LAST, by that of the entry before it when both name the
	 * same handle. They stay set when admission fails, for the cleanup of the
	 * request, and when it needs a bigger block, which takes them over.
	 */
	u32 pinned;
	/* Entries of pin[] that are borrowed from the ring, see borrowed[]. */
	u32 nr_borrowed;
	u32 cap_nr;
	u32 cap_segs;
	/* The largest entry; the copy threshold applies to it. */
	u32 max_len;
	struct io_uring_opaque_send_entry *ent;
	struct io_opaque_data **pin;
	struct bio_vec *bv;
	/*
	 * Whether the version of an entry that owns its pin (io_opaque_vec_owns()), among the
	 * first @pinned, is held by a borrow from the ring (io_opaque_pin()) and not by a
	 * reference of the send's own. Read only while @nr_borrowed is not zero.
	 */
	u8 *borrowed;
};

/*
 * Where the bytes of a capture come from, when the pass that captures them may
 * take over the reference of the skb to the page: the skb, which is the one
 * that TCP handed to the actor or a child on its frag_list, the index of the
 * fragment in it, or -1 for its head, and whether the fragment lies wholly at or
 * after the offset at which the pass began.
 */
struct io_opaque_src {
	struct sk_buff *skb;
	s16 frag;
	bool from_entry;
};

/*
 * A backing that a pass created or joined, and the fragment it did so for. The
 * pass settles them when it has consumed what it came for, see
 * io_opaque_actor().
 */
struct io_opaque_pending {
	struct io_opaque_backing *backing;
	struct io_opaque_src src;
	/* Settled: the reference of the skb to the fragment passed to the backing. */
	bool moved;
};

struct io_opaque_stream {
	refcount_t refs;
	/* Extents, range decisions and collector state. */
	struct mutex lock;
	struct io_opaque_store *store;
	struct file *file;
	struct sock_rx_owner owner;
	struct list_head extents;
	/* The block that capture takes its next chunk from, or NULL. */
	struct io_opaque_block *block;
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
	/*
	 * The call of the actor that is under way, if any: the references that
	 * its captures have still to settle. Attaching the stream allocates the
	 * room for IO_OPAQUE_PENDING_MAX of them, and the last put frees it. Under
	 * lock, as everything else of the capture.
	 */
	bool in_call;
	/* The offset in the skb at which the call began. */
	u32 call_entry;
	u32 nr_pending;
	struct io_opaque_pending *pending;
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
	IO_OPAQUE_STAT_FRAMING_COPIED,
	IO_OPAQUE_STAT_BODY_COPIED,
	/* Pages that capture got by the reference of the skb, by its own reference, by a copy. */
	IO_OPAQUE_STAT_PAGES_STOLEN,
	IO_OPAQUE_STAT_PAGES_HELD,
	IO_OPAQUE_STAT_PAGES_COPIED,
	/* Blocks of chunks that streams allocated. */
	IO_OPAQUE_STAT_CHUNK_BLOCKS,
	IO_OPAQUE_STAT_SEND_MANAGED_SKBS,
	IO_OPAQUE_STAT_SEND_FOLIO_REFS,
	IO_OPAQUE_STAT_SEND_FRAG_FALLBACK,
	IO_OPAQUE_STAT_SEND_UBUF_COPIED,
	IO_OPAQUE_STAT_SEND_ADMIT_FAST,
	IO_OPAQUE_STAT_SEND_ADMIT_SLOW,
	IO_OPAQUE_STAT_SEND_UBUF_REUSED,
	/*
	 * Versions that sends hold, counted when they are acquired: a reference that the send
	 * took on the version, or the table's own that a send with LAST took over.
	 */
	IO_OPAQUE_STAT_SEND_REFS_TAKEN,
	/* Versions that a send borrowed from its ring, taking no reference (io_opaque_pin()). */
	IO_OPAQUE_STAT_SEND_REFS_BORROWED,
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

/*
 * The ubuf_info of the skbs that carry a send's managed fragments (see
 * io_opaque_link_skb()): one per skb, shared by the skbs that are derived from it.
 */
struct io_opaque_ubuf {
	struct ubuf_info uarg;
	/* Entries of folio[]. */
	u8 nr;
	/*
	 * Link in the pool of a ubuf that has completed and is waiting for a skb.
	 * KASAN poisons a ubuf in the pool but for this (io_opaque_ubuf_poison()).
	 */
	struct llist_node node;
	/* Where the ubuf goes when its last skb is freed; none for one that is only freed. */
	struct io_opaque_ubuf_pool *pool;
	/* The distinct folios of the fragments of the skbs that share the ubuf. */
	struct folio *folio[MAX_SKB_FRAGS];
};

/*
 * The ubufs of a store that have completed, kept for the next skbs (see
 * io_opaque_ubuf_pool_get()). Completions, which run in any context on any CPU,
 * push to done with a compare-and-exchange; the send side, which runs under the
 * socket lock but for many sockets and rings at a time, takes all of done at once
 * into stock, and hands out of that, under lock. nr counts what was pushed since
 * done was last taken, so that done holds IO_OPAQUE_UBUF_POOL_MAX at most.
 *
 * refs is one for the store and one for every ubuf of the pool that has not been
 * freed to the cache, wherever it is (in an skb, in done, in stock): the pool
 * outlives the store and the ring for as long as an skb waits for an ACK.
 */
#define IO_OPAQUE_UBUF_POOL_MAX	128

struct io_opaque_ubuf_pool {
	refcount_t refs;
	struct llist_head done ____cacheline_aligned_in_smp;
	atomic_t nr;
	spinlock_t lock ____cacheline_aligned_in_smp;
	struct llist_node *stock;
};

/*
 * What a managed send hands to link_skb() through msg_ubuf: the ops, which
 * skb_zerocopy_iter_stream() finds link_skb() through, and what the send's store
 * gives the ubufs of the skbs that link_skb() makes: its pool and its flags. It
 * lives on the stack of io_opaque_send() for one tcp_sendmsg_locked(). No skb is
 * ever linked to it, and its own ubuf_info has no flags.
 */
struct io_opaque_msg_ubuf {
	struct ubuf_info uarg;
	/* The pool of the send's store, for the ubufs of its skbs; or none. */
	struct io_opaque_ubuf_pool *pool;
	/*
	 * What the store adds to the flags of every ubuf (io_opaque_ubuf_flags()):
	 * SKBFL_SHARED_FRAG if it binds a MAIO instance or holds a page of one, else none.
	 * It is not the pool's, because a store may have no pool.
	 */
	u8 flags;
};

/* Evaluation tunables; module parameters, not ABI. */
extern unsigned long io_opaque_cache_bytes;
extern unsigned int io_opaque_unit_order;
extern unsigned int io_opaque_inline_compact_max;
extern bool io_opaque_copy_per_page;
extern unsigned int io_opaque_send_copy_max;
extern bool io_opaque_send_managed;

struct io_opaque_store {
	refcount_t refs;
	refcount_t users;
	struct io_uring_opaque_config config;
	/*
	 * Handles and candidates. A spinlock: nothing under it sleeps or allocates, and
	 * a version is put, a copy made and a socket locked only after it is dropped. It
	 * is held for a time that the request bounds: what the admission of a send that
	 * consumes its object does under it is a lookup per entry, the segments its
	 * ranges span up to the limit, which it counts and stores in the request's
	 * block, and for each range a walk that the version's anchors keep to about two
	 * strides and a binary search. Attach and that admission take it with the socket
	 * owned, so a socket lock is never taken while holding it. Every change of a
	 * slot's version is made with it held, and a send that does not consume its
	 * object does not take it to read one (see io_opaque_pin()), but for the lookup
	 * that finds no version to take.
	 */
	spinlock_t tables;
	struct io_opaque_slot *objects;
	struct io_opaque_stream_slot *streams;
	unsigned long *object_used;
	unsigned long *stream_used;
	u32 object_cursor;
	u32 stream_cursor;
	u32 nr_objects;
	u32 nr_streams;
	struct rb_root txs;
	struct list_head txs_idle;
	u32 nr_txs_idle;
	struct user_struct *user;
	struct mm_struct *mm;
	struct mem_cgroup *memcg;
	struct obj_cgroup *objcg;
	/*
	 * The MAIO instance that the store binds, or NULL. A bound store keeps
	 * every byte in area pages. The store holds one reference to the
	 * instance until io_opaque_free(), which runs after the last backing is
	 * gone, so the area outlives all pages that a backing holds.
	 */
	struct io_zcrx_ifq *maio;
	/*
	 * Set once and never cleared: this store, which binds no instance, has kept a page
	 * of the area of a MAIO instance (io_opaque_retain()), because it captured from a
	 * queue that an instance owns. The process that maps the area can write that page,
	 * as it can write the pages of a bound store, so the managed sends of this store
	 * are marked as theirs are (io_opaque_send_ubuf_flags()). The mark is the store's,
	 * not the object's, as the binding is. Set by the capture, read by the sends.
	 */
	bool maio_pages;
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
	atomic_t memory_mode;
	/* Completed ubufs of managed sends, for the next skbs; none if there was no memory. */
	struct io_opaque_ubuf_pool *ubuf_pool;
	/* Budget waiters and remote wakeup/cancellation arbitration. */
	spinlock_t wait_lock;
	/*
	 * TX queues: the tree, every queue's requests and failed flag. Never with tables;
	 * after the socket, which an inline send owns when it joins.
	 */
	spinlock_t tx_lock;
	struct list_head budget_waits;
	struct list_head copies;
	/* One dispatcher serializes manual and automatic copies. */
	struct delayed_work compact_work;
	struct delayed_work retry_work;
	struct list_head candidates;
	struct io_uring_opaque_policy policy;
	/* Counts policy changes; a walk of the table that is overtaken by another stops. */
	u32 policy_seq;
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

/* Flags of io_opaque_extent_new(). */
#define IO_OPAQUE_EXT_RESERVED	BIT(0)	/* the caller charged the store already */
#define IO_OPAQUE_EXT_NO_MEMCG	BIT(1)	/* another owner accounts for the memory */

/*
 * How the capture holds the page of a new backing. AREA and POOL exist only for
 * a store that binds a MAIO instance, and PP only for one that does not.
 */
enum io_opaque_hold {
	/* A page reference, taken here. */
	IO_OPAQUE_HOLD_PAGE,
	/*
	 * A page pool fragment reference, taken here, to a page that a page
	 * pool owns, in an skb that is marked for recycling.
	 */
	IO_OPAQUE_HOLD_PP,
	/*
	 * A page reference to an area page, taken here, when no pool reference
	 * is possible. Either no pool of the instance owns the page, or the skb
	 * holds only a page reference to it. The page is an outsider page for
	 * the area. For example, the process may have passed it to a send as
	 * user memory, and the send came back over loopback. The bytes are
	 * visible through the mapping, so the backing counts as in the area.
	 * If a pool owns the page when it returns, the instance parks the page
	 * in limbo until this reference ends.
	 */
	IO_OPAQUE_HOLD_AREA,
	/*
	 * A pool reference to an area page, taken here. A pool of the instance
	 * owns the page.
	 */
	IO_OPAQUE_HOLD_POOL,
	/*
	 * A copy page. The reference of the caller passes to the backing. The
	 * page is a page of the area if the store binds an instance.
	 */
	IO_OPAQUE_HOLD_COPY,
};

struct io_opaque_extent *io_opaque_extent_new(struct io_opaque_store *store,
					      struct page *page, u32 offset,
					      u32 length, u64 start,
					      unsigned int flags);
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

/* The version that @slot holds, for a caller that holds tables, which is what changes it. */
static inline struct io_opaque_data *io_opaque_slot_data(struct io_opaque_store *store,
							 const struct io_opaque_slot *slot)
{
	return rcu_dereference_protected(slot->data, lockdep_is_held(&store->tables));
}

void io_opaque_wait(struct io_opaque_req *op, enum io_opaque_phase phase);
void io_opaque_queue_ready(struct io_opaque_req *op, int result);
int io_opaque_compact_build(struct io_opaque_req *op);
int io_opaque_compact_publish(struct io_opaque_req *op);
void io_opaque_compact_end(struct io_opaque_req *op);
void io_opaque_compact_work(struct work_struct *work);
int io_opaque_compact_start(struct io_opaque_req *op);
int io_opaque_set_policy(struct io_opaque_req *op);

#if IS_ENABLED(CONFIG_KUNIT)
unsigned int io_opaque_hold_flags(enum io_opaque_hold hold);
struct io_opaque_extent *io_opaque_stream_extent_new(struct io_opaque_stream *stream,
						     struct page *page, u32 offset,
						     u32 length, u64 start,
						     unsigned int flags);
void io_opaque_block_retire(struct io_opaque_stream *stream);
void io_opaque_capture_release(struct io_opaque_stream *stream);
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
void io_opaque_stream_put(struct io_opaque_stream *stream);
int io_opaque_attach(struct io_opaque_req *op);
int io_opaque_capture(struct io_opaque_stream *stream, struct sk_buff *skb,
		      u32 offset, u32 len, bool shared);
int io_opaque_actor(read_descriptor_t *desc, struct sk_buff *skb,
		    unsigned int offset, size_t length);
bool io_opaque_collect_demand(struct io_opaque_stream *stream);
void io_opaque_send_resolve(struct io_opaque_req *op);
int io_opaque_send_acquire(struct io_opaque_req *op);
void io_opaque_send_return(struct io_opaque_req *op);
int io_opaque_vec_grow(struct io_opaque_req *op, u32 nr, u32 segs, u32 keep);
void io_opaque_vec_commit(struct io_opaque_req *op, u32 nr, u32 total, u32 longest);
int io_opaque_vec_copy_in(struct io_opaque_req *op,
			  const struct io_uring_opaque_send_entry __user *uent, u32 nr);
void io_opaque_vec_release(struct io_opaque_req *op);
int io_opaque_vec_finish(struct io_opaque_req *op, int ret);
int io_opaque_apply_policy(struct io_opaque_store *store,
			   const struct io_uring_opaque_policy *policy);
extern unsigned int io_opaque_policy_chunk;
extern void (*io_opaque_policy_between)(struct io_opaque_store *store);
extern void (*io_opaque_lookup_before)(struct io_opaque_store *store);
extern void (*io_opaque_lookup_between)(struct io_opaque_store *store);
extern void (*io_opaque_lookup_after)(struct io_opaque_store *store);
extern atomic_long_t io_opaque_pin_slow;
extern void (*io_opaque_borrows_exit_hook)(struct io_ring_ctx *ctx);
extern unsigned int io_opaque_send_vec_segs;
extern atomic_long_t io_opaque_vec_seeks;
extern atomic_long_t io_opaque_vec_stepped;
extern atomic_long_t io_opaque_vec_counted;
extern atomic_long_t io_opaque_budget_wakes;
int io_opaque_tx_join(struct io_opaque_req *op, struct sock *sk);
int io_opaque_tx_settle_head(struct io_opaque_req *op);
int io_opaque_tx_admit(struct io_opaque_req *op, struct sock *sk);
struct io_opaque_req *io_opaque_tx_release(struct io_opaque_req *op);
void io_opaque_txs_free(struct io_opaque_store *store);
bool io_opaque_send_splices(const struct io_opaque_req *op);
bool io_opaque_send_direct(const struct sock *sk);
extern const struct ubuf_info_ops io_opaque_ubuf_ops;
extern const struct ubuf_info_ops io_opaque_msg_ubuf_ops;
extern atomic_long_t io_opaque_ubufs_live;
extern atomic_long_t io_opaque_ubuf_pools_live;
struct io_opaque_ubuf_pool *io_opaque_ubuf_pool_new(void);
void io_opaque_ubuf_pool_close(struct io_opaque_ubuf_pool *pool);
struct io_opaque_ubuf *io_opaque_ubuf_pool_get(struct io_opaque_ubuf_pool *pool);
u8 io_opaque_ubuf_flags(void);
u8 io_opaque_send_ubuf_flags(const struct io_opaque_store *store);
int io_opaque_link_skb(struct sk_buff *skb, struct ubuf_info *uarg);
int io_opaque_sg_from_iter(struct sk_buff *skb, struct iov_iter *from, size_t length);
int io_opaque_send_security(struct socket *sock, struct msghdr *msg,
			    const struct io_opaque_req *op);
extern atomic_long_t io_opaque_send_locked_calls;
extern void (*io_opaque_send_part)(const struct msghdr *msg,
				   const struct io_opaque_msg_ubuf *mub,
				   bool owned);
extern int io_opaque_send_poison;
int io_opaque_create_build(struct io_opaque_req *op);
bool io_opaque_unit_retire(struct io_opaque_store *store, struct page *page);
struct page *io_opaque_unit_take(struct io_opaque_store *store);
struct io_uring_opaque_extent
io_opaque_extent_pair(struct io_opaque_store *store, const struct io_opaque_extent *extent);
#endif
#endif
