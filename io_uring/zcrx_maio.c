// SPDX-License-Identifier: GPL-2.0
/*
 * MAIO mode of io_uring zero-copy receive.
 *
 * A MAIO instance owns one area of kernel pages. The registering process
 * maps the area read-write. The pages are order-0 pages with independent
 * reference counts. The area holds one reference to each page from the
 * allocation until the area is freed, so a page never reaches the page
 * allocator while the area exists.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/sort.h>
#include <linux/scatterlist.h>
#include <linux/dma-map-ops.h>
#include <asm/shmparam.h>

#include <net/page_pool/helpers.h>
#include <net/page_pool/memory_provider.h>

#include <uapi/linux/io_uring.h>

#include "io_uring.h"
#include "rsrc.h"
#include "zcrx.h"

/*
 * The allocator asks for chunks of this size first. It halves a chunk that
 * the page allocator cannot give, down to one page, and splits each chunk
 * into order-0 pages.
 */
#define IO_ZCRX_KMEM_CHUNK_SIZE		SZ_2M

static void io_zcrx_kmem_free_pages(struct page **pages, unsigned long nr_pages)
{
	unsigned long i;

	for (i = 0; i < nr_pages; i++)
		put_page(pages[i]);
}

static int io_zcrx_kmem_alloc_pages(struct page **pages, unsigned long nr_pages)
{
	const unsigned int max_order = min_t(unsigned int,
					     get_order(IO_ZCRX_KMEM_CHUNK_SIZE),
					     MAX_PAGE_ORDER);
	unsigned long done = 0;

	while (done < nr_pages) {
		unsigned int order = min_t(unsigned int, max_order,
					   ilog2(nr_pages - done));
		struct page *page;
		unsigned long i;

		for (;;) {
			gfp_t gfp = GFP_KERNEL_ACCOUNT | __GFP_ZERO | __GFP_NOWARN;

			/* Do not stall on a chunk that smaller pages can fill. */
			if (order)
				gfp |= __GFP_NORETRY;
			page = alloc_pages(gfp, order);
			if (page || !order)
				break;
			order--;
		}
		if (!page) {
			io_zcrx_kmem_free_pages(pages, done);
			return -ENOMEM;
		}

		/* Each page gets its own reference count. */
		if (order)
			split_page(page, order);
		for (i = 0; i < 1UL << order; i++)
			pages[done + i] = page + i;
		done += 1UL << order;
		cond_resched();
	}
	return 0;
}

/*
 * The process maps pages that are not contiguous in the kernel. A cache that
 * aliases (SHM_COLOUR) would need a user mapping that matches a kernel alias
 * of the same colour, and io_uring cannot align the pages one by one.
 */
static bool io_zcrx_kmem_supported(void)
{
#ifdef SHM_COLOUR
	return false;
#else
	return IS_ENABLED(CONFIG_MMU);
#endif
}

/*
 * Allocate the pages of a kernel area and account them against the locked
 * memory limit of the user, as a pinned user area is accounted.
 */
int io_zcrx_kmem_import(struct io_zcrx_ifq *ifq, struct io_zcrx_mem *mem,
			const struct io_uring_zcrx_area_reg *area_reg)
{
	unsigned long nr_pages = area_reg->len >> PAGE_SHIFT;
	struct page **pages;
	unsigned long i;
	int ret;

	if (!io_zcrx_kmem_supported())
		return -EOPNOTSUPP;

	/* Account first, so that the limit bounds the allocation. */
	ret = io_account_mem(ifq->user, ifq->mm_account, nr_pages);
	if (ret)
		return ret;

	pages = kvmalloc_array(nr_pages, sizeof(*pages), GFP_KERNEL_ACCOUNT);
	if (!pages) {
		ret = -ENOMEM;
		goto err_unaccount;
	}

	ret = io_zcrx_kmem_alloc_pages(pages, nr_pages);
	if (ret)
		goto err_free_array;

	/*
	 * The allocator leaves list poison where a page pool keeps its pool
	 * pointer. A page that no pool owns must show a null pointer.
	 */
	for (i = 0; i < nr_pages; i++)
		net_mp_page_clear_page_pool(pages[i]);

	mem->pages = pages;
	mem->size = area_reg->len;
	mem->account_pages = nr_pages;
	mem->is_kernel = true;
	return 0;

err_free_array:
	kvfree(pages);
err_unaccount:
	io_unaccount_mem(ifq->user, ifq->mm_account, nr_pages);
	return ret;
}

/*
 * Drop the reference of the area to each page. A page that someone else still
 * references, for example a socket buffer or a pinned user buffer, lives on
 * until the last holder drops it. The caller undoes the accounting.
 */
void io_zcrx_kmem_release(struct io_zcrx_mem *mem)
{
	io_zcrx_kmem_free_pages(mem->pages, mem->size >> PAGE_SHIFT);
	kvfree(mem->pages);
}

/*
 * Most pages that a call looks at in the limbo list before it gives up. The
 * list is a FIFO and the pages leave it in roughly the order in which they
 * entered, so a short scan finds the pages that are ready. A page that is
 * not ready goes to the tail, and the next call looks at other pages. The
 * bound keeps the time under ->alloc_lock short in softirq context. It is
 * twice the number of pages that one call hands to a pool.
 */
#define IO_ZCRX_LIMBO_SCAN	(2 * PP_ALLOC_CACHE_REFILL)

/*
 * Add to a counter of the stats area of the application, if it has one. A
 * counter has one writer at a time, because the writers hold ->alloc_lock.
 */
static void io_zcrx_maio_stat_add(struct io_zcrx_ifq *ifq, size_t offset,
				  u64 n)
{
	lockdep_assert_held(&ifq->alloc_lock);

	if (ifq->notif_stats)
		zcrx_stat_add((u64 *)((char *)ifq->notif_stats + offset), n);
}

static inline struct io_zcrx_area *io_zcrx_maio_area(struct io_zcrx_ifq *ifq)
{
	/* The only area is created before the first pool binds. It never changes. */
	return ifq->areas[0];
}

static int io_zcrx_kchunk_cmp(const void *a, const void *b)
{
	const struct io_zcrx_kchunk *ca = a, *cb = b;

	return (ca->base_pfn > cb->base_pfn) - (ca->base_pfn < cb->base_pfn);
}

/*
 * Build the bookkeeping of a kernel area. The chunk table maps a page to its
 * slot. A chunk is a run of pages with consecutive page frame numbers in
 * consecutive slots. The page allocator tends to return big blocks, so the
 * table has few entries, and a lookup is a binary search over a table that
 * fits in a few cache lines per thousand pages.
 */
int io_zcrx_kmem_init_area(struct io_zcrx_area *area)
{
	struct page **pages = area->mem.pages;
	u32 nr = area->nia.num_niovs;
	struct io_zcrx_kmem *kmem;
	u32 i, nr_chunks = 0;

	kmem = kzalloc_obj(*kmem);
	if (!kmem)
		return -ENOMEM;
	/* From here io_zcrx_kmem_free_area() frees what is allocated. */
	area->kmem = kmem;

	for (i = 0; i < nr; i++)
		if (!i || page_to_pfn(pages[i]) != page_to_pfn(pages[i - 1]) + 1)
			nr_chunks++;

	kmem->chunks = kvmalloc_array(nr_chunks, sizeof(kmem->chunks[0]),
				      GFP_KERNEL_ACCOUNT);
	kmem->state = kvzalloc(nr, GFP_KERNEL_ACCOUNT);
	kmem->limbo = kvmalloc_array(nr, sizeof(kmem->limbo[0]),
				     GFP_KERNEL_ACCOUNT);
	if (!kmem->chunks || !kmem->state || !kmem->limbo)
		return -ENOMEM;

	kmem->nr_chunks = 0;
	for (i = 0; i < nr; i++) {
		struct io_zcrx_kchunk *c;

		if (i && page_to_pfn(pages[i]) == page_to_pfn(pages[i - 1]) + 1) {
			kmem->chunks[kmem->nr_chunks - 1].nr_pages++;
			continue;
		}
		c = &kmem->chunks[kmem->nr_chunks++];
		c->base_pfn = page_to_pfn(pages[i]);
		c->nr_pages = 1;
		c->first_slot = i;
	}
	sort(kmem->chunks, kmem->nr_chunks, sizeof(kmem->chunks[0]),
	     io_zcrx_kchunk_cmp, NULL);
	return 0;
}

void io_zcrx_kmem_free_area(struct io_zcrx_area *area)
{
	struct io_zcrx_kmem *kmem = area->kmem;
	u32 i;

	/*
	 * Every pool is gone and every holder has returned its pages, so each
	 * slot is in the freelist or in limbo.
	 */
	if (kmem->state)
		for (i = 0; i < area->nia.num_niovs; i++)
			WARN_ON_ONCE(kmem->state[i] == IO_ZCRX_KSLOT_OWNED);

	kvfree(kmem->limbo);
	kvfree(kmem->state);
	kvfree(kmem->chunks);
	kfree(kmem);
	area->kmem = NULL;
}

/*
 * Find the slot of a page. Return false if the page is not in the area. The
 * chunk table never changes after the area is created, so the lookup takes no
 * lock.
 */
bool io_zcrx_kmem_lookup(const struct io_zcrx_area *area,
			 const struct page *page, u32 *slot)
{
	const struct io_zcrx_kchunk *chunks = area->kmem->chunks;
	unsigned long pfn = page_to_pfn(page);
	u32 lo = 0, hi = area->kmem->nr_chunks;

	/* Find the first chunk that starts above the page. */
	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;

		if (chunks[mid].base_pfn <= pfn)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo)
		return false;

	/* The chunk before it is the only one that can hold the page. */
	chunks += lo - 1;
	if (pfn - chunks->base_pfn >= chunks->nr_pages)
		return false;
	*slot = chunks->first_slot + (pfn - chunks->base_pfn);
	return true;
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
 * A mapping of the page by another process counts as a mapping of the area.
 * The caller cannot tell the two apart.
 */
static bool io_zcrx_page_is_quiet(struct page *page)
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

static void io_zcrx_limbo_push(struct io_zcrx_area *area, u32 idx)
{
	struct io_zcrx_kmem *kmem = area->kmem;
	u32 tail = kmem->limbo_head + kmem->limbo_count;

	if (tail >= area->nia.num_niovs)
		tail -= area->nia.num_niovs;
	kmem->limbo[tail] = idx;
	kmem->limbo_count++;
}

static u32 io_zcrx_limbo_pop(struct io_zcrx_area *area)
{
	struct io_zcrx_kmem *kmem = area->kmem;
	u32 idx = kmem->limbo[kmem->limbo_head];

	if (++kmem->limbo_head == area->nia.num_niovs)
		kmem->limbo_head = 0;
	kmem->limbo_count--;
	return idx;
}

/*
 * Give a slot back to the area. It goes to the freelist if no outsider holds
 * its page, and to limbo otherwise. The slot must be owned: a pool owned it,
 * or it was a copy page.
 */
void io_zcrx_kmem_return_slot(struct io_zcrx_area *area, u32 idx)
{
	struct io_zcrx_kmem *kmem = area->kmem;

	guard(spinlock_bh)(&area->ifq->alloc_lock);

	if (WARN_ON_ONCE(kmem->state[idx] != IO_ZCRX_KSLOT_OWNED))
		return;
	if (io_zcrx_page_is_quiet(area->mem.pages[idx])) {
		kmem->state[idx] = IO_ZCRX_KSLOT_FREE;
		area->freelist[area->free_count++] = idx;
	} else {
		kmem->state[idx] = IO_ZCRX_KSLOT_LIMBO;
		io_zcrx_limbo_push(area, idx);
		io_zcrx_maio_stat_add(area->ifq,
				      offsetof(struct zcrx_stats, limbo_enter), 1);
	}
}

/*
 * Move pages from limbo to the freelist, up to @want of them. Look at no more
 * than IO_ZCRX_LIMBO_SCAN pages.
 */
static void io_zcrx_kmem_reclaim(struct io_zcrx_area *area, unsigned int want)
{
	struct io_zcrx_kmem *kmem = area->kmem;
	unsigned int scan = min_t(u32, kmem->limbo_count, IO_ZCRX_LIMBO_SCAN);

	lockdep_assert_held(&area->ifq->alloc_lock);

	while (scan-- && want) {
		u32 idx = io_zcrx_limbo_pop(area);

		if (io_zcrx_page_is_quiet(area->mem.pages[idx])) {
			kmem->state[idx] = IO_ZCRX_KSLOT_FREE;
			area->freelist[area->free_count++] = idx;
			io_zcrx_maio_stat_add(area->ifq,
					      offsetof(struct zcrx_stats, limbo_leave), 1);
			want--;
		} else {
			io_zcrx_limbo_push(area, idx);
		}
	}
}

/*
 * Map the area for DMA for the device of the first pool that maps DMA. A
 * later pool must use the same device. A page keeps its DMA address until
 * the area is unmapped, so the pools of all queues share one mapping.
 */
int io_zcrx_kmem_map(struct io_zcrx_ifq *ifq, struct device *dev)
{
	struct io_zcrx_area *area = io_zcrx_maio_area(ifq);
	struct sg_table *sgt = &area->mem.page_sg_table;
	u32 nr = area->nia.num_niovs;
	struct scatterlist *sg;
	unsigned int max_seg;
	u32 idx = 0;
	int i, ret;

	guard(mutex)(&ifq->pp_lock);

	if (area->is_mapped)
		return ifq->dev == dev ? 0 : -EOPNOTSUPP;

	/* A segment must not be longer than the device can handle. */
	max_seg = ALIGN_DOWN(dma_get_max_seg_size(dev), PAGE_SIZE);
	if (!max_seg)
		return -EINVAL;
	ret = sg_alloc_table_from_pages_segment(sgt, area->mem.pages, nr, 0,
						(unsigned long)nr << PAGE_SHIFT,
						max_seg, GFP_KERNEL_ACCOUNT);
	if (ret)
		return ret;
	ret = dma_map_sgtable(dev, sgt, DMA_FROM_DEVICE, IO_DMA_ATTR);
	if (ret < 0)
		goto err_free_table;

	for_each_sgtable_dma_sg(sgt, sg, i) {
		dma_addr_t dma = sg_dma_address(sg);
		unsigned long sg_len = sg_dma_len(sg);

		if (WARN_ON_ONCE(sg_len % PAGE_SIZE)) {
			ret = -EINVAL;
			goto err_clear;
		}
		while (sg_len && idx < nr) {
			if (net_mp_page_set_dma_addr(area->mem.pages[idx], dma)) {
				ret = -EFAULT;
				goto err_clear;
			}
			sg_len -= PAGE_SIZE;
			dma += PAGE_SIZE;
			idx++;
		}
	}
	if (WARN_ON_ONCE(idx != nr)) {
		ret = -EFAULT;
		goto err_clear;
	}

	area->mem.sgt = sgt;
	get_device(dev);
	ifq->dev = dev;
	area->is_mapped = true;
	return 0;

err_clear:
	while (idx--)
		net_mp_page_set_dma_addr(area->mem.pages[idx], 0);
	dma_unmap_sgtable(dev, sgt, DMA_FROM_DEVICE, IO_DMA_ATTR);
err_free_table:
	sg_free_table(sgt);
	return ret;
}

void io_zcrx_kmem_unmap(struct io_zcrx_ifq *ifq, struct io_zcrx_area *area)
{
	u32 i;

	lockdep_assert_held(&ifq->pp_lock);

	area->is_mapped = false;
	for (i = 0; i < area->nia.num_niovs; i++)
		net_mp_page_set_dma_addr(area->mem.pages[i], 0);
	dma_unmap_sgtable(ifq->dev, &area->mem.page_sg_table,
			  DMA_FROM_DEVICE, IO_DMA_ATTR);
	sg_free_table(&area->mem.page_sg_table);
	area->mem.sgt = NULL;
}

static void io_zcrx_kmem_sync_for_device(struct page_pool *pp,
					 netmem_ref *netmems, unsigned int nr)
{
#if defined(CONFIG_HAS_DMA) && defined(CONFIG_DMA_NEED_SYNC)
	struct device *dev = pp->p.dev;
	unsigned int i;

	if (!pp->dma_sync || !dma_dev_need_sync(dev))
		return;

	for (i = 0; i < nr; i++) {
		dma_addr_t dma_addr = page_pool_get_dma_addr_netmem(netmems[i]);

		__dma_sync_single_for_device(dev, dma_addr + pp->p.offset,
					     pp->p.max_len, pp->p.dma_dir);
	}
#endif
}

/*
 * Record the result of an allocation from the area, by a pool or by a copy.
 * The first allocation that finds the area empty starts an exhaustion
 * episode. It counts the episode and sets *exhausted, so that the caller
 * can tell the application when it has dropped the lock. Any allocation that
 * gets a page ends the episode.
 */
static void io_zcrx_kmem_episode(struct io_zcrx_ifq *ifq, bool got_page,
				 bool *exhausted)
{
	lockdep_assert_held(&ifq->alloc_lock);

	if (got_page) {
		ifq->maio_exhausted = false;
	} else if (!ifq->maio_exhausted) {
		ifq->maio_exhausted = true;
		*exhausted = true;
		io_zcrx_maio_stat_add(ifq,
				      offsetof(struct zcrx_stats, exhaust_count), 1);
	}
}

/*
 * Take pages from the freelist, after it took back the pages in limbo that
 * are ready. Give each page to the pool. Return the number of pages.
 * Set *exhausted if this call found the area empty and the previous call
 * did not.
 */
static unsigned int io_zcrx_kmem_refill(struct page_pool *pp,
					struct io_zcrx_ifq *ifq,
					netmem_ref *netmems,
					unsigned int to_alloc, bool *exhausted)
{
	struct io_zcrx_area *area = io_zcrx_maio_area(ifq);
	struct io_zcrx_kmem *kmem = area->kmem;
	unsigned int allocated = 0;

	guard(spinlock_bh)(&ifq->alloc_lock);

	if (area->free_count < to_alloc)
		io_zcrx_kmem_reclaim(area, to_alloc - area->free_count);

	while (allocated < to_alloc && area->free_count) {
		u32 idx = area->freelist[--area->free_count];
		struct page *page = area->mem.pages[idx];

		kmem->state[idx] = IO_ZCRX_KSLOT_OWNED;
		net_mp_page_set_page_pool(pp, page);
		netmems[allocated++] = page_to_netmem(page);
	}

	io_zcrx_kmem_episode(ifq, allocated, exhausted);
	return allocated;
}

/*
 * Give the queue ordinary pages, because the area has none. The pool may fill
 * its cache with more pages than the one that it returns.
 */
static netmem_ref io_zcrx_maio_fallback(struct page_pool *pp,
					struct io_zcrx_ifq *ifq, gfp_t gfp)
{
	netmem_ref netmem = net_mp_alloc_fallback(pp, gfp);

	if (netmem) {
		guard(spinlock_bh)(&ifq->alloc_lock);
		io_zcrx_maio_stat_add(ifq,
				      offsetof(struct zcrx_stats, fallback_pages),
				      1 + pp->alloc.count);
	}
	return netmem;
}

/*
 * The refill ring gave pages. If the previous call found the area empty, that
 * episode ends here.
 */
static void io_zcrx_maio_end_exhaustion(struct io_zcrx_ifq *ifq)
{
	guard(spinlock_bh)(&ifq->alloc_lock);
	ifq->maio_exhausted = false;
}

netmem_ref io_pp_maio_alloc_netmems(struct page_pool *pp, gfp_t gfp)
{
	struct io_zcrx_ifq *ifq = io_pp_to_ifq(pp);
	netmem_ref *netmems = pp->alloc.cache;
	unsigned int to_alloc = PP_ALLOC_CACHE_REFILL;
	bool exhausted = false;
	unsigned int allocated;

	/* pp should already be ensuring that */
	if (WARN_ON_ONCE(pp->alloc.count))
		return 0;

	allocated = io_zcrx_ring_refill(pp, ifq, netmems, to_alloc);
	if (allocated) {
		if (unlikely(READ_ONCE(ifq->maio_exhausted)))
			io_zcrx_maio_end_exhaustion(ifq);
	} else {
		allocated = io_zcrx_kmem_refill(pp, ifq, netmems, to_alloc,
						&exhausted);
		/* Tell the application once per episode. */
		if (exhausted)
			zcrx_send_notif(ifq, ZCRX_EVENT_ALLOC_FAIL);
		/*
		 * The host must keep receiving, so give the queue ordinary
		 * pages. Packets in them take the copy path.
		 */
		if (!allocated)
			return io_zcrx_maio_fallback(pp, ifq, gfp);
	}

	io_zcrx_kmem_sync_for_device(pp, netmems, allocated);
	allocated--;
	pp->alloc.count += allocated;
	return netmems[allocated];
}

bool io_pp_maio_release_netmem(struct page_pool *pp, netmem_ref netmem)
{
	struct io_zcrx_ifq *ifq = io_pp_to_ifq(pp);
	struct io_zcrx_area *area = io_zcrx_maio_area(ifq);
	struct page *page;
	u32 idx;

	if (WARN_ON_ONCE(netmem_is_net_iov(netmem)))
		return false;

	page = netmem_to_page(netmem);
	/* An ordinary fallback page. The pool unmaps and frees it. */
	if (!io_zcrx_kmem_lookup(area, page, &idx))
		return true;

	net_mp_page_clear_page_pool(page);
	io_zcrx_kmem_return_slot(area, idx);
	return false;
}

int io_pp_maio_init(struct page_pool *pp)
{
	struct io_zcrx_ifq *ifq = io_pp_to_ifq(pp);
	int ret;

	if (WARN_ON_ONCE(!ifq || !ifq->maio))
		return -EINVAL;
	/* A slot is one page. */
	if (pp->p.order)
		return -EOPNOTSUPP;

	if (pp->dma_map) {
		if (pp->p.dma_dir != DMA_FROM_DEVICE)
			return -EOPNOTSUPP;
		ret = io_zcrx_kmem_map(ifq, pp->p.dev);
		if (ret)
			return ret;
	}

	refcount_inc(&ifq->refs);
	return 0;
}

/**
 * io_zcrx_maio_get() - find a MAIO instance of a ring
 * @ctx:	the ring, locked by the caller with ->uring_lock
 * @zcrx_id:	the zcrx id that registration or import returned for the ring
 *
 * The reference keeps the area and its pages alive. It does not keep the
 * provider installed: the application can unregister the instance. Drop the
 * reference with io_zcrx_maio_put().
 *
 * Return: the instance with a reference, or NULL if the ring has no MAIO
 * instance with this id.
 */
struct io_zcrx_ifq *io_zcrx_maio_get(struct io_ring_ctx *ctx, u32 zcrx_id)
{
	struct io_zcrx_ifq *ifq;

	lockdep_assert_held(&ctx->uring_lock);

	/* The entry holds a reference of the ring while it exists. */
	ifq = xa_load(&ctx->zcrx_ctxs, zcrx_id);
	if (!ifq || !ifq->maio || !refcount_inc_not_zero(&ifq->refs))
		return NULL;
	return ifq;
}

/** io_zcrx_maio_put() - drop the reference of io_zcrx_maio_get() */
void io_zcrx_maio_put(struct io_zcrx_ifq *ifq)
{
	io_put_zcrx_ifq(ifq);
}

/**
 * io_zcrx_maio_page_off() - tell if a page is in the area
 * @ifq:	a MAIO instance
 * @page:	any page
 * @area_off:	output: the offset of the page in the zcrx offset format, that is
 *		the area id in the top bits and the byte offset in the area below.
 *		The offset names the first byte of the page.
 *
 * A page that a page pool got as a fallback is outside the area. The function
 * takes no lock and runs in any context.
 *
 * Return: true if @page is in the area, and then @area_off holds its offset.
 * False for a page outside the area.
 */
bool io_zcrx_maio_page_off(struct io_zcrx_ifq *ifq, struct page *page,
			   u64 *area_off)
{
	struct io_zcrx_area *area = io_zcrx_maio_area(ifq);
	u32 idx;

	if (!io_zcrx_kmem_lookup(area, page, &idx))
		return false;
	*area_off = zcrx_area_id_to_token(area->area_id) + ((u64)idx << PAGE_SHIFT);
	return true;
}

/**
 * io_zcrx_maio_page_get() - take a pool reference to an area page
 * @ifq:	a MAIO instance
 * @page:	a page of the area that a page pool owns, or a copy page
 *
 * A pool reference keeps the page out of the pool until the holder drops it.
 * Other holders of a pool reference are socket buffers of the receive path.
 * The caller must already hold a pool reference or know that the page is
 * owned. The function runs in any context.
 */
void io_zcrx_maio_page_get(struct io_zcrx_ifq *ifq, struct page *page)
{
	page_pool_ref_netmem(page_to_netmem(page));
}

/**
 * io_zcrx_maio_page_put() - drop a pool reference to an area page
 * @ifq:	a MAIO instance
 * @page:	a page of the area
 *
 * The last reference returns the page to its pool. A copy page has no pool,
 * and it returns to the freelist of the area. A page that an outsider still
 * references waits in limbo until the outsider is gone. The function runs in
 * any context in which the caller holds neither ->alloc_lock nor the lock of
 * the refill ring.
 */
void io_zcrx_maio_page_put(struct io_zcrx_ifq *ifq, struct page *page)
{
	netmem_ref netmem = page_to_netmem(page);
	struct io_zcrx_area *area = io_zcrx_maio_area(ifq);
	struct page_pool *pp;
	u32 idx;

	if (page_pool_unref_netmem(netmem, 1))
		return;

	pp = netmem_get_pp(netmem);
	if (pp) {
		page_pool_put_unrefed_netmem(pp, netmem, -1, false);
		return;
	}

	/* A copy page: no pool owns it. */
	if (!WARN_ON_ONCE(!io_zcrx_kmem_lookup(area, page, &idx)))
		io_zcrx_kmem_return_slot(area, idx);
}

/**
 * io_zcrx_maio_copy_page() - take a free page of the area
 * @ifq:	a MAIO instance
 * @area_off:	output: the offset of the page, see io_zcrx_maio_page_off()
 *
 * The page has no pool, and it holds one pool reference. The caller fills it.
 * The caller gives the page back with io_zcrx_maio_page_put(). The content of
 * the page is not defined.
 *
 * A call that finds the area empty, when the last allocation from the area
 * got a page, starts an exhaustion episode as a pool does: it counts the
 * episode in exhaust_count and sends ZCRX_EVENT_ALLOC_FAIL. Any allocation
 * that gets a page, also this one, ends the episode.
 *
 * The function runs in process and softirq context. The caller must not hold
 * ->alloc_lock.
 *
 * Return: the page, or NULL if the area has no free page.
 */
struct page *io_zcrx_maio_copy_page(struct io_zcrx_ifq *ifq, u64 *area_off)
{
	struct io_zcrx_area *area = io_zcrx_maio_area(ifq);
	struct io_zcrx_kmem *kmem = area->kmem;
	bool exhausted = false;
	struct page *page;
	bool got_page;
	u32 idx = 0;

	scoped_guard(spinlock_bh, &ifq->alloc_lock) {
		if (!area->free_count)
			io_zcrx_kmem_reclaim(area, 1);
		got_page = !!area->free_count;
		if (got_page) {
			idx = area->freelist[--area->free_count];
			kmem->state[idx] = IO_ZCRX_KSLOT_OWNED;
		}
		io_zcrx_kmem_episode(ifq, got_page, &exhausted);
	}

	/* The application hears of the episode once the lock is gone. */
	if (exhausted)
		zcrx_send_notif(ifq, ZCRX_EVENT_ALLOC_FAIL);
	if (!got_page)
		return NULL;

	page = area->mem.pages[idx];
	/* With one reference, the last put returns the slot. */
	page_pool_fragment_netmem(page_to_netmem(page), 1);
	*area_off = zcrx_area_id_to_token(area->area_id) + ((u64)idx << PAGE_SHIFT);
	return page;
}

/**
 * io_zcrx_maio_page_owned() - tell if a page pool of an instance owns a page
 * @ifq:	a MAIO instance
 * @page:	any page
 *
 * Tell whether @page belongs to a page pool that uses the provider of @ifq.
 * Two kinds of page qualify: an area page that a pool owns, and a fallback
 * page. Use io_zcrx_maio_page_off() to tell them apart. A page that no pool
 * owns does not qualify. Neither does a page of a pool without the provider of
 * @ifq. A copy page is not owned by a pool. Neither is an area page that a
 * process passed as user memory.
 *
 * The caller must hold a pool reference of the page. Only an skb marked for
 * page pool recycling (skb->pp_recycle) holds one, for each pool page that it
 * carries. An skb without the mark holds a plain page reference, which does
 * not pin the pool fields of the page. Without a pool reference, the pool can
 * release the page, and even free itself, while the function reads the page
 * and the pool. With one, the answer does not change, and
 * io_zcrx_maio_page_get() is valid. The function takes no lock and runs in
 * any context.
 *
 * Return: true if a page pool that uses the provider of @ifq owns @page,
 * false otherwise.
 */
bool io_zcrx_maio_page_owned(struct io_zcrx_ifq *ifq, struct page *page)
{
	struct page_pool *pp;

	if (!page_pool_page_is_pp(page))
		return false;
	pp = netmem_get_pp(page_to_netmem(page));
	return pp->mp_ops == &io_uring_pp_maio_ops && io_pp_to_ifq(pp) == ifq;
}
