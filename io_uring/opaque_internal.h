/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IO_URING_OPAQUE_INTERNAL_H
#define IO_URING_OPAQUE_INTERNAL_H

#include <linux/bitmap.h>
#include <linux/bvec.h>
#include <linux/net.h>
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

struct io_opaque_backing {
	refcount_t refs;
	struct page *page;
	u64 charge;
	u32 filled;
	bool copied;
	bool accounted;
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
};

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
	atomic_t extents;
	atomic_t requests;
	/* Budget waiters and remote wakeup/cancellation arbitration. */
	spinlock_t wait_lock;
	struct list_head budget_waits;
	struct list_head copies;
	/* Serialize manual and automatic copies outside the submission lock. */
	struct mutex copy_lock;
	struct work_struct copy_work;
	struct delayed_work retry_work;
	struct delayed_work auto_work;
	struct list_head candidates;
	struct io_uring_opaque_policy policy;
	u64 tokens;
	unsigned long token_time;
	int dead;
};

bool io_opaque_charge(struct io_opaque_store *store, u64 bytes, bool rx);
void io_opaque_uncharge(struct io_opaque_store *store, u64 bytes);
struct io_opaque_extent *io_opaque_extent_new(struct io_opaque_store *store,
					      struct page *page, u32 offset,
					      u32 length, u64 start,
					      bool reserved);
struct io_opaque_data *io_opaque_data_alloc(void);
void io_opaque_data_put(struct io_opaque_store *store,
			struct io_opaque_data *data);
int io_opaque_vector(struct io_opaque_data *data);
u64 io_opaque_handle(u32 index, u64 generation);
struct io_opaque_slot *io_opaque_lookup(struct io_opaque_store *store,
					u64 handle);
void io_opaque_wait(struct io_opaque_req *op, enum io_opaque_phase phase);
void io_opaque_queue_ready(struct io_opaque_req *op, int result);
int io_opaque_compact_build(struct io_opaque_req *op);
int io_opaque_compact_publish(struct io_opaque_req *op);
void io_opaque_compact_work(struct work_struct *work);
int io_opaque_compact_start(struct io_opaque_req *op);
int io_opaque_set_policy(struct io_opaque_req *op);
void io_opaque_auto_work(struct work_struct *work);

#if IS_ENABLED(CONFIG_KUNIT)
void io_opaque_release_slot(unsigned long *used, u32 index, u64 generation);
void io_opaque_extent_free(struct io_opaque_store *store,
			   struct io_opaque_extent *extent);
void io_opaque_extents_free(struct io_opaque_store *store,
			    struct list_head *list);
struct io_opaque_data *io_opaque_detach(struct io_opaque_store *store,
					struct io_opaque_slot *slot);
int io_opaque_slot_reserve(struct io_opaque_req *op);
u32 io_opaque_find_slot(unsigned long *used, u32 count, u32 *cursor);
struct io_opaque_extent *io_opaque_extent_split(struct io_opaque_store *store,
						struct io_opaque_extent *extent,
						u64 at);
int io_opaque_available(struct io_opaque_stream *stream, u64 off, u64 len);
int io_opaque_take(struct io_opaque_req *op);
void io_opaque_restore(struct io_opaque_req *op);
int io_opaque_range_insert(struct io_opaque_req *op);
int io_opaque_capture(struct io_opaque_stream *stream,
		      const struct sk_buff *skb, u32 offset, u32 len,
		      bool shared);
int io_opaque_actor(read_descriptor_t *desc, struct sk_buff *skb,
		    unsigned int offset, size_t length);
int io_opaque_send_acquire(struct io_opaque_req *op);
#endif
#endif
