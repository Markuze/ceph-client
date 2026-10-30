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
#include <asm/shmparam.h>

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
