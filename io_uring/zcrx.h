// SPDX-License-Identifier: GPL-2.0
#ifndef IOU_ZC_RX_H
#define IOU_ZC_RX_H

#include <linux/io_uring_types.h>
#include <linux/dma-buf.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <net/page_pool/types.h>
#include <net/net_trackers.h>

#define ZCRX_SUPPORTED_REG_FLAGS	(ZCRX_REG_IMPORT | ZCRX_REG_NODEV |\
					 ZCRX_REG_MAIO)
#define ZCRX_FEATURES			(ZCRX_FEATURE_RX_PAGE_SIZE |\
					 ZCRX_FEATURE_EVENT)
#define ZCRX_EVENT_TYPE_MASK		((1U << ZCRX_EVENT_ALLOC_FAIL) |\
					 (1U << ZCRX_EVENT_COPY))

#define IO_DMA_ATTR (DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_WEAK_ORDERING)

struct io_zcrx_mem {
	unsigned long			size;
	bool				is_dmabuf;
	/* order-0 pages that the kernel allocated, see IORING_ZCRX_AREA_KERNEL */
	bool				is_kernel;

	struct page			**pages;
	unsigned long			nr_folios;
	struct sg_table			page_sg_table;
	unsigned long			account_pages;
	struct sg_table			*sgt;

	struct dma_buf_attachment	*attach;
	struct dma_buf			*dmabuf;
};

/* Provider state of a slot of a kernel area. Written under ->alloc_lock. */
enum {
	IO_ZCRX_KSLOT_FREE,	/* in the freelist */
	IO_ZCRX_KSLOT_OWNED,	/* handed out: a page pool owns it, or it is a copy page */
	IO_ZCRX_KSLOT_LIMBO,	/* in the limbo list: an outsider holds a reference */
	IO_ZCRX_KSLOT_GONE,	/* released after the close, the area holds no reference */
};

/* A run of pages with consecutive page frame numbers in consecutive slots. */
struct io_zcrx_kchunk {
	unsigned long		base_pfn;
	u32			nr_pages;
	u32			first_slot;
};

/*
 * Bookkeeping of a kernel area. The pages are in area->mem.pages and are
 * indexed by slot.
 */
struct io_zcrx_kmem {
	/* Sorted by base_pfn. Never changes after the area is created. */
	struct io_zcrx_kchunk	*chunks;
	u32			nr_chunks;
	/* Per slot, IO_ZCRX_KSLOT_*. Protected by ->alloc_lock. */
	u8			*state;
	/* FIFO ring of slots in limbo, one entry per slot at most. ->alloc_lock */
	u32			*limbo;
	u32			limbo_head;
	u32			limbo_count;
	/*
	 * The instance has no user left, see io_zcrx_kmem_close(). A slot that
	 * comes back releases its page. ->alloc_lock
	 */
	bool			closed;
};

struct io_zcrx_area {
	struct net_iov_area	nia;
	struct io_zcrx_ifq	*ifq;
	atomic_t		*user_refs;
	/* only for a kernel area, then nia.niovs is NULL and nia.num_niovs counts the slots */
	struct io_zcrx_kmem	*kmem;

	bool			is_mapped;
	u16			area_id;

	/* freelist */
	u32			free_count;
	u32			*freelist;

	struct io_zcrx_mem	mem;
};

/*
 * A slot is the unit that the refill ring and the CQE offsets address. It is
 * the idx-th net_iov of the area, or the idx-th page of a kernel area.
 */
struct io_zcrx_slot {
	struct io_zcrx_area	*area;
	u32			idx;
};

struct zcrx_rq_hdr {
	u32		head ____cacheline_aligned_in_smp;
	u32		tail ____cacheline_aligned_in_smp;
};

struct zcrx_rq {
	spinlock_t			lock;
	struct zcrx_rq_hdr		*ring;
	struct io_uring_zcrx_rqe	*rqes;
	u32				cached_head;
	u32				cached_tail;
	u32				nr_entries;
};

struct io_zcrx_ifq {
	/* read-protected by any of: ->pp_lock, ->alloc_lock, ->rq.lock */
	struct io_zcrx_area		**areas;
	unsigned			nr_areas;

	unsigned			niov_shift;
	struct user_struct		*user;
	struct mm_struct		*mm_account;
	bool				kern_readable;
	/*
	 * MAIO instance: one area of kernel pages. A slot of the area is a
	 * page, not a net_iov.
	 */
	bool				maio;
	/* the process maps the area of a MAIO instance through this region */
	struct io_mapped_region		area_region;
	/* MAIO: receive queues of the device that have the provider */
	u32				nr_rxq;
	/* MAIO: the area had no free page at the last allocation. ->alloc_lock */
	bool				maio_exhausted;
	/*
	 * MAIO: the users of the area. The rings and files of user_refs count
	 * as one, and each store that binds the instance counts as one. The
	 * last one closes the area, see zcrx_maio_close().
	 */
	refcount_t			maio_users;

	struct zcrx_rq			rq ____cacheline_aligned_in_smp;
	spinlock_t			alloc_lock ____cacheline_aligned_in_smp;

	u32				if_rxq;
	struct device			*dev;
	struct net_device		*netdev;
	netdevice_tracker		netdev_tracker;
	refcount_t			refs;
	/* counts userspace facing users like io_uring */
	refcount_t			user_refs;

	/*
	 * Page pool and net configuration lock, can be taken deeper in the
	 * net stack.
	 */
	struct mutex			pp_lock;
	struct io_mapped_region		rq_region;

	spinlock_t			ctx_lock;
	struct io_ring_ctx		*master_ctx;
	u32				allowed_notif_mask;
	u32				fired_notifs;
	u64				notif_data;
	struct zcrx_stats		*notif_stats;
};

static inline void zcrx_stat_add(__u64 *p, s64 v)
{
	WRITE_ONCE(*p, READ_ONCE(*p) + v);
}

static inline u64 zcrx_area_id_to_token(u32 area_id)
{
	return (u64)area_id << IORING_ZCRX_AREA_SHIFT;
}

static inline struct io_zcrx_ifq *io_pp_to_ifq(struct page_pool *pp)
{
	return pp->mp_priv;
}

/*
 * Return true if no one outside the area holds a reference to the page. The
 * area holds one reference. Each PTE of a user mapping holds one reference
 * and one map count. Every other reference belongs to an outsider, for
 * example a socket buffer in a transmit queue or a pinned user buffer. The
 * provider must not hand the page to a device while an outsider holds it.
 *
 * The function reads the map count, the reference count, the map count and
 * the reference count again. It returns true only if both pairs agree and the
 * reference count is one above the map count. The kernel takes the reference
 * of a PTE before it raises the map count, and it lowers the map count before
 * it drops the reference. The reads are ordered to match. A single mmap() or
 * munmap() step of the area between the reads changes one of the four values
 * in a way that the checks see, so the function returns false. A false
 * result is harmless: the caller keeps the page in limbo and asks again
 * later. A false "quiet" would let a device write a page that an outsider
 * still reads, and the checks exclude it.
 *
 * Every PTE of an area page belongs to a mapping of the area, so the count is
 * exact. The kernel maps an area page nowhere else: TCP_ZEROCOPY_RECEIVE
 * copies it, see net_mp_page_nomap(), and fork() does not copy the mapping. A
 * process that maps the area through an imported instance maps the area too.
 */
static inline bool io_zcrx_page_is_quiet(struct page *page)
{
	struct folio *folio = page_folio(page);
	int mapped, refs;

	mapped = folio_mapcount(folio);
	/* Read the map count before the reference count, see above. */
	smp_rmb();
	refs = folio_ref_count(folio);
	/* Read the reference count before the second map count. */
	smp_rmb();
	if (mapped != folio_mapcount(folio))
		return false;
	/* Read the second map count before the second reference count. */
	smp_rmb();
	if (refs != folio_ref_count(folio))
		return false;
	return refs == 1 + mapped;
}

#if defined(CONFIG_IO_URING_ZCRX)
int io_zcrx_ctrl(struct io_ring_ctx *ctx, void __user *arg, unsigned nr_arg);
int io_register_zcrx(struct io_ring_ctx *ctx,
			 struct io_uring_zcrx_ifq_reg __user *arg);
void io_unregister_zcrx(struct io_ring_ctx *ctx);
void io_terminate_zcrx(struct io_ring_ctx *ctx);
int io_zcrx_recv(struct io_kiocb *req, struct io_zcrx_ifq *ifq,
		 struct socket *sock, unsigned int flags,
		 unsigned issue_flags, unsigned int *len);
struct io_mapped_region *io_zcrx_get_region(struct io_ring_ctx *ctx,
					    unsigned int id);
struct io_mapped_region *io_zcrx_get_area_region(struct io_ring_ctx *ctx,
						 unsigned int id);

/* zcrx.c, used by zcrx_maio.c */
unsigned int io_zcrx_ring_refill(struct page_pool *pp, struct io_zcrx_ifq *ifq,
				 netmem_ref *netmems, unsigned int to_alloc);
void zcrx_send_notif(struct io_zcrx_ifq *ifq, unsigned int type);
void io_put_zcrx_ifq(struct io_zcrx_ifq *ifq);
void io_zcrx_maio_drop_user(struct io_zcrx_ifq *ifq);
extern const struct memory_provider_ops io_uring_pp_maio_ops;

/* zcrx_maio.c */
int io_zcrx_kmem_import(struct io_zcrx_ifq *ifq, struct io_zcrx_mem *mem,
			const struct io_uring_zcrx_area_reg *area_reg);
void io_zcrx_kmem_release(struct io_zcrx_area *area);
int io_zcrx_kmem_init_area(struct io_zcrx_area *area);
void io_zcrx_kmem_free_area(struct io_zcrx_area *area);
void io_zcrx_kmem_close(struct io_zcrx_area *area);
int io_zcrx_kmem_map(struct io_zcrx_ifq *ifq, struct device *dev);
void io_zcrx_kmem_unmap(struct io_zcrx_ifq *ifq, struct io_zcrx_area *area);
void io_zcrx_kmem_return_slot(struct io_zcrx_area *area, u32 idx);
void io_zcrx_kmem_park_slot(struct page_pool *pp, struct io_zcrx_area *area,
			    u32 idx);
bool io_zcrx_kmem_lookup(const struct io_zcrx_area *area,
			 const struct page *page, u32 *slot);
netmem_ref io_pp_maio_alloc_netmems(struct page_pool *pp, gfp_t gfp);
bool io_pp_maio_release_netmem(struct page_pool *pp, netmem_ref netmem);
int io_pp_maio_init(struct page_pool *pp);

/*
 * The interface of a MAIO instance for in-kernel users, for example the
 * opaque stores. The contracts are in zcrx_maio.c.
 */
struct io_zcrx_ifq *io_zcrx_maio_get(struct io_ring_ctx *ctx, u32 zcrx_id);
void io_zcrx_maio_put(struct io_zcrx_ifq *ifq);
bool io_zcrx_maio_page_off(struct io_zcrx_ifq *ifq, struct page *page,
			   u64 *area_off);
void io_zcrx_maio_page_get(struct io_zcrx_ifq *ifq, struct page *page);
void io_zcrx_maio_page_put(struct io_zcrx_ifq *ifq, struct page *page);
struct page *io_zcrx_maio_copy_page(struct io_zcrx_ifq *ifq, u64 *area_off);
bool io_zcrx_maio_page_owned(struct io_zcrx_ifq *ifq, struct page *page);

#if IS_ENABLED(CONFIG_IO_URING_ZCRX_KUNIT_TEST)
/*
 * Aids for the KUnit tests of other io_uring code, see zcrx_test.c. They need
 * no device.
 */
struct kunit;
struct io_zcrx_ifq *io_zcrx_maio_test_ifq(struct kunit *test, unsigned int pages);
struct page_pool *io_zcrx_maio_test_pool(struct kunit *test,
					 struct io_zcrx_ifq *ifq);
#endif
#else
static inline struct io_zcrx_ifq *io_zcrx_maio_get(struct io_ring_ctx *ctx,
						   u32 zcrx_id)
{
	return NULL;
}

static inline void io_zcrx_maio_put(struct io_zcrx_ifq *ifq)
{
}

static inline bool io_zcrx_maio_page_off(struct io_zcrx_ifq *ifq,
					 struct page *page, u64 *area_off)
{
	return false;
}

static inline void io_zcrx_maio_page_get(struct io_zcrx_ifq *ifq,
					 struct page *page)
{
}

static inline void io_zcrx_maio_page_put(struct io_zcrx_ifq *ifq,
					 struct page *page)
{
}

static inline struct page *io_zcrx_maio_copy_page(struct io_zcrx_ifq *ifq,
						  u64 *area_off)
{
	return NULL;
}

static inline bool io_zcrx_maio_page_owned(struct io_zcrx_ifq *ifq,
					   struct page *page)
{
	return false;
}

static inline int io_register_zcrx(struct io_ring_ctx *ctx,
				   struct io_uring_zcrx_ifq_reg __user *arg)
{
	return -EOPNOTSUPP;
}

static inline void io_unregister_zcrx(struct io_ring_ctx *ctx)
{
}

static inline void io_terminate_zcrx(struct io_ring_ctx *ctx)
{
}

static inline int io_zcrx_recv(struct io_kiocb *req, struct io_zcrx_ifq *ifq,
			       struct socket *sock, unsigned int flags,
			       unsigned issue_flags, unsigned int *len)
{
	return -EOPNOTSUPP;
}

static inline struct io_mapped_region *io_zcrx_get_region(struct io_ring_ctx *ctx,
							  unsigned int id)
{
	return NULL;
}

static inline struct io_mapped_region *io_zcrx_get_area_region(struct io_ring_ctx *ctx,
							       unsigned int id)
{
	return NULL;
}

static inline int io_zcrx_ctrl(struct io_ring_ctx *ctx,
				void __user *arg, unsigned nr_arg)
{
	return -EOPNOTSUPP;
}
#endif

int io_recvzc(struct io_kiocb *req, unsigned int issue_flags);
int io_recvzc_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);

#endif
