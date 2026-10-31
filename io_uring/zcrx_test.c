// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the MAIO mode of zcrx: the page states of the provider, the
 * page lookup, the refill ring, the fallback pages, the teardown and the
 * conversion of the receive queues of a device. The file is included at the
 * end of zcrx.c, so that it reaches the static functions.
 *
 * The tests need no network hardware. A page pool binds the provider through
 * the same gate as for a device, with a net_device that is not registered.
 */
#include <kunit/test.h>
#include <kunit/device.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/random.h>
#include <linux/xarray.h>

#define ZT_RQ_ENTRIES	16

struct zt_env {
	struct io_zcrx_ifq	*ifq;
	struct io_zcrx_area	*area;
	struct zcrx_stats	*stats;
	struct net_device	*dev;
	struct page_pool	*pp[2];
};

static struct io_zcrx_ifq *zt_ifq(struct kunit *test, unsigned int pages)
{
	struct io_uring_zcrx_area_reg area_reg = {
		.len	= (u64)pages << PAGE_SHIFT,
		.flags	= IORING_ZCRX_AREA_KERNEL,
	};
	struct io_uring_zcrx_ifq_reg reg = {};
	struct io_zcrx_ifq *ifq;

	ifq = io_zcrx_ifq_alloc(NULL);
	KUNIT_ASSERT_NOT_NULL(test, ifq);
	ifq->maio = true;
	ifq->kern_readable = true;
	ifq->rq.nr_entries = ZT_RQ_ENTRIES;
	ifq->rq.ring = kunit_kzalloc(test, sizeof(*ifq->rq.ring), GFP_KERNEL);
	ifq->rq.rqes = kunit_kcalloc(test, ZT_RQ_ENTRIES, sizeof(*ifq->rq.rqes),
				     GFP_KERNEL);
	ifq->notif_stats = kunit_kzalloc(test, sizeof(*ifq->notif_stats),
					 GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ifq->rq.ring);
	KUNIT_ASSERT_NOT_NULL(test, ifq->rq.rqes);
	KUNIT_ASSERT_NOT_NULL(test, ifq->notif_stats);
	KUNIT_ASSERT_EQ(test, io_zcrx_create_area(ifq, &area_reg, &reg), 0);
	return ifq;
}

static struct net_device *zt_netdev(struct kunit *test, unsigned int queues)
{
	struct net_device *dev;

	dev = alloc_netdev_mqs(0, "zcrxt%d", NET_NAME_UNKNOWN, ether_setup,
			       queues, queues);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	return dev;
}

static void zt_unbind(struct net_device *dev, unsigned int queue)
{
	struct pp_memory_provider_params *p = &__netif_get_rx_queue(dev, queue)->mp_params;

	memset(p, 0, sizeof(*p));
}

static void zt_bind(struct net_device *dev, unsigned int queue,
		    struct io_zcrx_ifq *ifq)
{
	struct pp_memory_provider_params *p = &__netif_get_rx_queue(dev, queue)->mp_params;

	p->mp_ops = &io_uring_pp_maio_ops;
	p->mp_priv = ifq;
	p->flags = PP_MP_F_READABLE;
}

static struct page_pool *zt_pool(struct kunit *test, struct net_device *dev,
				 unsigned int queue, unsigned int flags)
{
	struct page_pool_params params = {
		.pool_size	= 64,
		.nid		= NUMA_NO_NODE,
		.netdev		= dev,
		.queue_idx	= queue,
		.flags		= flags,
	};
	struct page_pool *pp;

	netdev_lock(dev);
	pp = page_pool_create(&params);
	netdev_unlock(dev);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pp));
	return pp;
}

static void zt_env_init(struct kunit *test, struct zt_env *env,
			unsigned int pages, unsigned int pools)
{
	unsigned int i;

	memset(env, 0, sizeof(*env));
	env->ifq = zt_ifq(test, pages);
	env->area = env->ifq->areas[0];
	env->stats = env->ifq->notif_stats;
	env->dev = zt_netdev(test, 2);
	for (i = 0; i < pools; i++) {
		zt_bind(env->dev, i, env->ifq);
		env->pp[i] = zt_pool(test, env->dev, i, PP_FLAG_PRIVATE_HEADERS);
	}
}

/* Destroy the pools, check that the area is whole, and free everything. */
static void zt_env_exit(struct kunit *test, struct zt_env *env)
{
	struct io_zcrx_kmem *kmem = env->area->kmem;
	u32 i, owned = 0, nr = env->area->nia.num_niovs;
	unsigned int p;

	for (p = 0; p < 2; p++)
		if (env->pp[p])
			page_pool_destroy(env->pp[p]);
	for (i = 0; i < nr; i++)
		owned += kmem->state[i] == IO_ZCRX_KSLOT_OWNED;
	KUNIT_EXPECT_EQ(test, owned, 0U);
	io_put_zcrx_ifq(env->ifq);
	free_netdev(env->dev);
}

static u32 zt_count(struct io_zcrx_area *area, int state)
{
	u32 i, n = 0;

	for (i = 0; i < area->nia.num_niovs; i++)
		n += area->kmem->state[i] == state;
	return n;
}

static u32 zt_slot(struct kunit *test, struct io_zcrx_ifq *ifq, struct page *page)
{
	u32 idx = 0;

	KUNIT_ASSERT_TRUE(test, io_zcrx_kmem_lookup(ifq->areas[0], page, &idx));
	return idx;
}

static struct page *zt_alloc(struct page_pool *pp)
{
	netmem_ref netmem = page_pool_alloc_netmems(pp, GFP_KERNEL);

	return netmem ? netmem_to_page(netmem) : NULL;
}

/* Hand the page back to its pool, as the stack does when it frees the skb. */
static void zt_free(struct page_pool *pp, struct page *page)
{
	page_pool_put_unrefed_netmem(pp, page_to_netmem(page), -1, true);
}

/* The page pool takes the provider through the same gate as for a device. */
static void zcrx_maio_gate(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = zt_ifq(test, 16);
	struct net_device *dev = zt_netdev(test, 2);
	struct page_pool *pp;

	/* A readable provider binds under PP_FLAG_PRIVATE_HEADERS. */
	zt_bind(dev, 0, ifq);
	pp = zt_pool(test, dev, 0, PP_FLAG_PRIVATE_HEADERS);
	KUNIT_EXPECT_PTR_EQ(test, pp->mp_ops, &io_uring_pp_maio_ops);
	KUNIT_EXPECT_PTR_EQ(test, pp->mp_priv, ifq);
	KUNIT_EXPECT_TRUE(test, pp->mp_readable);
	KUNIT_EXPECT_FALSE(test, page_pool_is_unreadable(pp));
	KUNIT_EXPECT_FALSE(test, netif_rxq_has_unreadable_mp(dev, 0));
	page_pool_destroy(pp);

	/* Without a flag, the pool stays ordinary. */
	pp = zt_pool(test, dev, 0, 0);
	KUNIT_EXPECT_NULL(test, pp->mp_ops);
	page_pool_destroy(pp);

	/* The header split flag binds a readable provider as well. */
	pp = zt_pool(test, dev, 0, PP_FLAG_ALLOW_UNREADABLE_NETMEM);
	KUNIT_EXPECT_PTR_EQ(test, pp->mp_ops, &io_uring_pp_maio_ops);
	KUNIT_EXPECT_FALSE(test, page_pool_is_unreadable(pp));
	page_pool_destroy(pp);

	/* A queue without the provider gives an ordinary pool. */
	pp = zt_pool(test, dev, 1, PP_FLAG_PRIVATE_HEADERS);
	KUNIT_EXPECT_NULL(test, pp->mp_ops);
	page_pool_destroy(pp);

	/*
	 * A provider of unreadable memory does not bind under
	 * PP_FLAG_PRIVATE_HEADERS, because the flag does not say that the
	 * driver handles such memory.
	 */
	zt_unbind(dev, 0);
	__netif_get_rx_queue(dev, 0)->mp_params.mp_ops = &io_uring_pp_zc_ops;
	__netif_get_rx_queue(dev, 0)->mp_params.mp_priv = ifq;
	KUNIT_EXPECT_TRUE(test, netif_rxq_has_unreadable_mp(dev, 0));
	pp = zt_pool(test, dev, 0, PP_FLAG_PRIVATE_HEADERS);
	KUNIT_EXPECT_NULL(test, pp->mp_ops);
	KUNIT_EXPECT_FALSE(test, page_pool_is_unreadable(pp));
	page_pool_destroy(pp);
	zt_unbind(dev, 0);

	free_netdev(dev);
	io_put_zcrx_ifq(ifq);
}

/* The lookup agrees with a scan, inside the area and around it. */
static void zcrx_maio_lookup(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = zt_ifq(test, 1024);
	struct io_zcrx_area *area = ifq->areas[0];
	struct io_zcrx_kmem *kmem = area->kmem;
	struct page *other = alloc_page(GFP_KERNEL);
	u32 i, c, idx;
	unsigned long pfn;
	int n;

	KUNIT_ASSERT_NOT_NULL(test, other);

	/* Every page maps to its own slot. */
	for (i = 0; i < area->nia.num_niovs; i++) {
		KUNIT_ASSERT_TRUE(test, io_zcrx_kmem_lookup(area, area->mem.pages[i], &idx));
		KUNIT_ASSERT_EQ(test, idx, i);
	}

	/* The chunks are sorted and do not overlap. */
	for (c = 1; c < kmem->nr_chunks; c++)
		KUNIT_ASSERT_TRUE(test, kmem->chunks[c - 1].base_pfn +
				  kmem->chunks[c - 1].nr_pages <= kmem->chunks[c].base_pfn);

	/* A page that the area does not own is not found. */
	KUNIT_EXPECT_FALSE(test, io_zcrx_kmem_lookup(area, other, &idx));

	/* Probe the neighbours of every chunk against a scan. */
	for (c = 0; c < kmem->nr_chunks; c++) {
		unsigned long probes[] = {
			kmem->chunks[c].base_pfn - 1,
			kmem->chunks[c].base_pfn,
			kmem->chunks[c].base_pfn + kmem->chunks[c].nr_pages - 1,
			kmem->chunks[c].base_pfn + kmem->chunks[c].nr_pages,
		};

		for (n = 0; n < ARRAY_SIZE(probes); n++) {
			bool found = false;

			pfn = probes[n];
			for (i = 0; i < area->nia.num_niovs; i++)
				found |= page_to_pfn(area->mem.pages[i]) == pfn;
			KUNIT_EXPECT_EQ(test, io_zcrx_kmem_lookup(area, pfn_to_page(pfn), &idx),
					found);
		}
	}
	__free_page(other);
	io_put_zcrx_ifq(ifq);
}

#define ZT_STATES_PAGES	256
#define ZT_BATCH	PP_ALLOC_CACHE_REFILL

/* Take every page that the cache of the pool holds, so the next call reaches the provider. */
static unsigned int zt_drain_cache(struct kunit *test, struct page_pool *pp,
				   struct page **out, unsigned int max)
{
	unsigned int n = 0;

	while (pp->alloc.count) {
		KUNIT_ASSERT_LT(test, n, max);
		out[n] = zt_alloc(pp);
		KUNIT_ASSERT_NOT_NULL(test, out[n]);
		n++;
	}
	return n;
}

/* The states of a page: owned, limbo, free, and the reclaim from limbo. */
static void zcrx_maio_states(struct kunit *test)
{
	struct page **held = kunit_kcalloc(test, ZT_STATES_PAGES, sizeof(*held), GFP_KERNEL);
	struct page **fb = kunit_kcalloc(test, 4 * ZT_BATCH, sizeof(*fb), GFP_KERNEL);
	unsigned int nfb = 0, nheld = 0, i;
	struct page *p, *stranger;
	struct zt_env env;
	u64 off;
	u32 idx;

	KUNIT_ASSERT_NOT_NULL(test, held);
	KUNIT_ASSERT_NOT_NULL(test, fb);
	zt_env_init(test, &env, ZT_STATES_PAGES, 1);
	KUNIT_EXPECT_EQ(test, env.area->free_count, (u32)ZT_STATES_PAGES);
	KUNIT_EXPECT_EQ(test, zt_count(env.area, IO_ZCRX_KSLOT_FREE), (u32)ZT_STATES_PAGES);

	/* The first allocation takes a batch from the freelist. */
	stranger = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, stranger);
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_off(env.ifq, stranger, &off));
	idx = zt_slot(test, env.ifq, stranger);
	KUNIT_EXPECT_EQ(test, off, (u64)idx << PAGE_SHIFT);
	KUNIT_EXPECT_EQ(test, env.area->free_count, (u32)(ZT_STATES_PAGES - ZT_BATCH));
	KUNIT_EXPECT_EQ(test, zt_count(env.area, IO_ZCRX_KSLOT_OWNED), (u32)ZT_BATCH);
	KUNIT_EXPECT_PTR_EQ(test, stranger->pp, env.pp[0]);
	KUNIT_EXPECT_EQ(test, page_count(stranger), 1);

	/* An outsider holds the page when the pool returns it: limbo. */
	get_page(stranger);
	zt_free(env.pp[0], stranger);
	KUNIT_EXPECT_EQ(test, env.area->kmem->state[idx], IO_ZCRX_KSLOT_LIMBO);
	KUNIT_EXPECT_EQ(test, env.area->kmem->limbo_count, 1U);
	KUNIT_EXPECT_EQ(test, env.stats->limbo_enter, 1ULL);
	KUNIT_EXPECT_NULL(test, stranger->pp);
	KUNIT_EXPECT_EQ(test, stranger->pp_magic, 0UL);

	/* Take all the other pages. The area is empty after that. */
	for (i = 0; i < ZT_STATES_PAGES - 1; i++) {
		held[nheld] = zt_alloc(env.pp[0]);
		KUNIT_ASSERT_NOT_NULL(test, held[nheld]);
		KUNIT_ASSERT_TRUE(test, io_zcrx_maio_page_off(env.ifq, held[nheld], &off));
		nheld++;
	}
	KUNIT_EXPECT_EQ(test, env.area->free_count, 0U);
	KUNIT_EXPECT_EQ(test, env.stats->limbo_leave, 0ULL);
	KUNIT_EXPECT_EQ(test, env.stats->exhaust_count, 0ULL);

	/* The outsider leaves, and the next allocation finds the page by reclaim. */
	put_page(stranger);
	p = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_PTR_EQ(test, p, stranger);
	KUNIT_EXPECT_EQ(test, env.stats->limbo_leave, 1ULL);
	KUNIT_EXPECT_EQ(test, env.area->kmem->limbo_count, 0U);
	KUNIT_EXPECT_EQ(test, env.stats->exhaust_count, 0ULL);
	held[nheld++] = p;

	/*
	 * The area has no page. The pool gets ordinary pages, and the first
	 * call that finds the area empty counts one episode.
	 */
	p = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_off(env.ifq, p, &off));
	KUNIT_EXPECT_PTR_EQ(test, p->pp, env.pp[0]);
	KUNIT_EXPECT_EQ(test, env.stats->exhaust_count, 1ULL);
	KUNIT_EXPECT_EQ(test, env.stats->fallback_pages, (u64)ZT_BATCH);
	KUNIT_EXPECT_TRUE(test, env.ifq->maio_exhausted);
	fb[nfb++] = p;

	/* The pool serves the rest of the batch without the provider. */
	nfb += zt_drain_cache(test, env.pp[0], fb + nfb, 4 * ZT_BATCH - nfb);
	KUNIT_EXPECT_EQ(test, nfb, (unsigned int)ZT_BATCH);
	KUNIT_EXPECT_EQ(test, env.stats->exhaust_count, 1ULL);

	/* The next batch is the same episode. */
	p = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, p);
	fb[nfb++] = p;
	nfb += zt_drain_cache(test, env.pp[0], fb + nfb, 4 * ZT_BATCH - nfb);
	KUNIT_EXPECT_EQ(test, env.stats->exhaust_count, 1ULL);
	KUNIT_EXPECT_EQ(test, env.stats->fallback_pages, 2ULL * ZT_BATCH);

	/*
	 * A fallback page leaves through release_netmem, which returns true.
	 * Hold the page to look at it after the pool released it.
	 */
	p = fb[--nfb];
	get_page(p);
	zt_free(env.pp[0], p);
	KUNIT_EXPECT_NULL(test, p->pp);
	KUNIT_EXPECT_EQ(test, p->pp_magic, 0UL);
	KUNIT_EXPECT_EQ(test, page_count(p), 1);
	put_page(p);

	/*
	 * An area page returns through limbo, and the episode ends when the
	 * pool gets it again. Another exhaustion is a new episode.
	 */
	p = held[0];
	get_page(p);
	zt_free(env.pp[0], p);
	put_page(p);
	KUNIT_EXPECT_EQ(test, env.stats->limbo_enter, 2ULL);
	nfb += zt_drain_cache(test, env.pp[0], fb + nfb, 4 * ZT_BATCH - nfb);
	held[0] = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_PTR_EQ(test, held[0], p);
	KUNIT_EXPECT_FALSE(test, env.ifq->maio_exhausted);
	KUNIT_EXPECT_EQ(test, env.stats->limbo_leave, 2ULL);
	nfb += zt_drain_cache(test, env.pp[0], fb + nfb, 4 * ZT_BATCH - nfb);
	fb[nfb++] = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, fb[nfb - 1]);
	KUNIT_EXPECT_EQ(test, env.stats->exhaust_count, 2ULL);

	/* Return every page. */
	for (i = 0; i < nheld; i++)
		zt_free(env.pp[0], held[i]);
	for (i = 0; i < nfb; i++)
		zt_free(env.pp[0], fb[i]);
	zt_env_exit(test, &env);
}

static void zt_hold(struct kunit *test, struct zt_env *env, struct page *p)
{
	u32 idx = zt_slot(test, env->ifq, p);

	/* What the receive path does for a page that it passes to the user. */
	page_pool_ref_netmem(page_to_netmem(p));
	io_zcrx_get_uref((struct io_zcrx_slot){ .area = env->area, .idx = idx });
	KUNIT_EXPECT_FALSE(test, page_pool_unref_and_test(page_to_netmem(p)));
	KUNIT_EXPECT_EQ(test, atomic_read(&env->area->user_refs[idx]), 1);
}

static void zt_rqe(struct zt_env *env, u64 off)
{
	struct zcrx_rq *rq = &env->ifq->rq;
	u32 tail = rq->ring->tail;

	rq->rqes[tail & (rq->nr_entries - 1)] =
		(struct io_uring_zcrx_rqe){ .off = off, .len = PAGE_SIZE };
	/* Publish the entry before the tail, as the application does. */
	smp_store_release(&rq->ring->tail, tail + 1);
}

/* A page that the user returns through the refill ring goes back to its pool. */
static void zcrx_maio_refill_ring(struct kunit *test)
{
	struct page *drain[ZT_BATCH], *p, *q;
	struct zt_env env;
	unsigned int n;
	u32 idx;
	u64 off;

	/* A pool of another queue hands the page to its owner. */
	zt_env_init(test, &env, 256, 2);
	p = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, p);
	idx = zt_slot(test, env.ifq, p);
	zt_hold(test, &env, p);
	KUNIT_ASSERT_TRUE(test, io_zcrx_maio_page_off(env.ifq, p, &off));
	zt_rqe(&env, off);
	q = zt_alloc(env.pp[1]);
	KUNIT_ASSERT_NOT_NULL(test, q);
	KUNIT_EXPECT_PTR_NE(test, q, p);
	KUNIT_EXPECT_EQ(test, atomic_read(&env.area->user_refs[idx]), 0);
	KUNIT_EXPECT_EQ(test, env.ifq->rq.ring->head, 1U);
	KUNIT_EXPECT_FALSE(test, ptr_ring_empty(&env.pp[0]->ring));
	zt_free(env.pp[1], q);
	zt_env_exit(test, &env);

	/* Entries of a slot without a user reference, or behind the area, do nothing. */
	zt_env_init(test, &env, 256, 1);
	zt_rqe(&env, 0);
	zt_rqe(&env, 256ULL << PAGE_SHIFT);
	zt_rqe(&env, 1ULL << IORING_ZCRX_AREA_SHIFT);
	p = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, env.ifq->rq.ring->head, 3U);
	KUNIT_EXPECT_TRUE(test, ptr_ring_empty(&env.pp[0]->ring));
	zt_free(env.pp[0], p);
	zt_env_exit(test, &env);

	/* The pool of the page gets its own page first. */
	zt_env_init(test, &env, 256, 1);
	p = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, p);
	zt_hold(test, &env, p);
	KUNIT_ASSERT_TRUE(test, io_zcrx_maio_page_off(env.ifq, p, &off));
	zt_rqe(&env, off);
	n = zt_drain_cache(test, env.pp[0], drain, ARRAY_SIZE(drain));
	q = zt_alloc(env.pp[0]);
	KUNIT_EXPECT_PTR_EQ(test, q, p);
	zt_free(env.pp[0], q);
	while (n--)
		zt_free(env.pp[0], drain[n]);
	zt_env_exit(test, &env);
}

/*
 * A slot that the user returns through the refill ring goes back to its pool
 * only if no outsider holds its page. A pipe after a splice of another socket
 * that shares the page is such an outsider, and so is a transmit queue after a
 * send of the slot. The PTE of the mapping of the area is not.
 */
static void zcrx_maio_refill_outsider(struct kunit *test)
{
	struct page **drain = kunit_kcalloc(test, 2 * ZT_BATCH, sizeof(*drain), GFP_KERNEL);
	struct page *p, *q;
	struct zt_env env;
	unsigned int n;
	u64 off;
	u32 idx;

	KUNIT_ASSERT_NOT_NULL(test, drain);

	/* A page that only the area and its mapping hold returns at once. */
	zt_env_init(test, &env, 2 * ZT_BATCH, 1);
	p = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, p);
	get_page(p);
	atomic_inc(&p->_mapcount);
	zt_hold(test, &env, p);
	KUNIT_ASSERT_TRUE(test, io_zcrx_maio_page_off(env.ifq, p, &off));
	zt_rqe(&env, off);
	n = zt_drain_cache(test, env.pp[0], drain, 2 * ZT_BATCH);
	q = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, q);
	KUNIT_EXPECT_PTR_EQ(test, q, p);
	KUNIT_EXPECT_EQ(test, env.stats->limbo_enter, 0ULL);
	atomic_dec(&p->_mapcount);
	put_page(p);
	zt_free(env.pp[0], q);
	while (n--)
		zt_free(env.pp[0], drain[n]);
	zt_env_exit(test, &env);

	/* With an outsider, the refill takes the entry and parks the slot. */
	zt_env_init(test, &env, 2 * ZT_BATCH, 1);
	p = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, p);
	idx = zt_slot(test, env.ifq, p);
	get_page(p);
	atomic_inc(&p->_mapcount);
	zt_hold(test, &env, p);
	get_page(p);
	KUNIT_ASSERT_TRUE(test, io_zcrx_maio_page_off(env.ifq, p, &off));
	zt_rqe(&env, off);
	n = zt_drain_cache(test, env.pp[0], drain, 2 * ZT_BATCH);
	q = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, q);
	drain[n++] = q;
	KUNIT_EXPECT_PTR_NE(test, q, p);
	KUNIT_EXPECT_EQ(test, env.ifq->rq.ring->head, 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&env.area->user_refs[idx]), 0);
	KUNIT_EXPECT_EQ(test, env.area->kmem->state[idx], IO_ZCRX_KSLOT_LIMBO);
	KUNIT_EXPECT_EQ(test, env.stats->limbo_enter, 1ULL);
	KUNIT_EXPECT_NULL(test, p->pp);

	/* The outsider leaves. The reclaim finds the page when the freelist runs low. */
	n += zt_drain_cache(test, env.pp[0], drain + n, 2 * ZT_BATCH - n);
	put_page(p);
	q = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, q);
	KUNIT_EXPECT_PTR_EQ(test, q, p);
	KUNIT_EXPECT_EQ(test, env.stats->limbo_leave, 1ULL);
	KUNIT_EXPECT_EQ(test, env.stats->exhaust_count, 0ULL);

	/* Undo the imitation of the mapping and return every page. */
	atomic_dec(&p->_mapcount);
	put_page(p);
	zt_free(env.pp[0], q);
	while (n--)
		zt_free(env.pp[0], drain[n]);
	zt_env_exit(test, &env);
}

/* Copy pages come from the freelist, have no pool and obey the same test. */
static void zcrx_maio_copy_page(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = zt_ifq(test, 16);
	struct io_zcrx_area *area = ifq->areas[0];
	struct page *p, *q, *all[16];
	u64 off, off2;
	u32 idx, i;

	p = io_zcrx_maio_copy_page(ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, p);
	idx = zt_slot(test, ifq, p);
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_off(ifq, p, &off2));
	KUNIT_EXPECT_EQ(test, off, off2);
	KUNIT_EXPECT_EQ(test, area->kmem->state[idx], IO_ZCRX_KSLOT_OWNED);
	KUNIT_EXPECT_NULL(test, p->pp);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&p->pp_ref_count), 1L);
	KUNIT_EXPECT_EQ(test, area->free_count, 15U);

	/* Two holders: the page returns with the last one. */
	io_zcrx_maio_page_get(ifq, p);
	io_zcrx_maio_page_put(ifq, p);
	KUNIT_EXPECT_EQ(test, area->kmem->state[idx], IO_ZCRX_KSLOT_OWNED);
	io_zcrx_maio_page_put(ifq, p);
	KUNIT_EXPECT_EQ(test, area->kmem->state[idx], IO_ZCRX_KSLOT_FREE);
	KUNIT_EXPECT_EQ(test, area->free_count, 16U);

	/* An outsider keeps the page in limbo, and the reclaim finds it later. */
	p = io_zcrx_maio_copy_page(ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, p);
	idx = zt_slot(test, ifq, p);
	get_page(p);
	io_zcrx_maio_page_put(ifq, p);
	KUNIT_EXPECT_EQ(test, area->kmem->state[idx], IO_ZCRX_KSLOT_LIMBO);
	for (i = 0; i < 15; i++) {
		all[i] = io_zcrx_maio_copy_page(ifq, &off);
		KUNIT_ASSERT_NOT_NULL(test, all[i]);
		KUNIT_ASSERT_PTR_NE(test, all[i], p);
	}
	/* Nothing is free while the outsider holds the page. */
	KUNIT_EXPECT_NULL(test, io_zcrx_maio_copy_page(ifq, &off));
	put_page(p);
	q = io_zcrx_maio_copy_page(ifq, &off);
	KUNIT_EXPECT_PTR_EQ(test, q, p);
	KUNIT_EXPECT_NULL(test, io_zcrx_maio_copy_page(ifq, &off));
	io_zcrx_maio_page_put(ifq, q);
	for (i = 0; i < 15; i++)
		io_zcrx_maio_page_put(ifq, all[i]);
	KUNIT_EXPECT_EQ(test, area->free_count, 16U);
	io_put_zcrx_ifq(ifq);
}

/* An instance owns a page when a page pool that uses its provider owns the page. */
static void zcrx_maio_owned(struct kunit *test)
{
	struct page *held[ZT_BATCH], *own, *copy, *fallback, *plain, *pooled;
	struct io_zcrx_ifq *other = zt_ifq(test, 16);
	struct page_pool *ordinary;
	struct zt_env env;
	unsigned int n;
	u64 off;

	/* One batch for the pool and one page for a copy page. */
	zt_env_init(test, &env, ZT_BATCH + 1, 1);
	own = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, own);
	copy = io_zcrx_maio_copy_page(env.ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	n = zt_drain_cache(test, env.pp[0], held, ARRAY_SIZE(held));

	/* An area page of the pool. Another instance does not own it. */
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_off(env.ifq, own, &off));
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_owned(env.ifq, own));
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_owned(other, own));

	/* A copy page has no pool. */
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_off(env.ifq, copy, &off));
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_owned(env.ifq, copy));

	/* The area is empty, so the pool gets a page that is not in the area. */
	fallback = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, fallback);
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_off(env.ifq, fallback, &off));
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_owned(env.ifq, fallback));

	/* A page that no pool owns, and a page of a pool without the provider. */
	plain = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, plain);
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_owned(env.ifq, plain));
	ordinary = zt_pool(test, env.dev, 1, PP_FLAG_PRIVATE_HEADERS);
	KUNIT_EXPECT_NULL(test, ordinary->mp_ops);
	pooled = zt_alloc(ordinary);
	KUNIT_ASSERT_NOT_NULL(test, pooled);
	KUNIT_EXPECT_TRUE(test, page_pool_page_is_pp(pooled));
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_owned(env.ifq, pooled));

	zt_free(ordinary, pooled);
	page_pool_destroy(ordinary);
	__free_page(plain);
	zt_free(env.pp[0], fallback);
	io_zcrx_maio_page_put(env.ifq, copy);
	while (n--)
		zt_free(env.pp[0], held[n]);
	zt_free(env.pp[0], own);
	zt_env_exit(test, &env);
	io_put_zcrx_ifq(other);
}

/*
 * A PTE of a user mapping holds one page reference and one map count. The
 * test imitates a PTE by hand. A page with only PTE references is ready, and
 * a page with an outsider on top of them is not.
 */
static void zcrx_maio_mapped(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = zt_ifq(test, 16);
	struct io_zcrx_area *area = ifq->areas[0];
	struct page *p, *q, *others[16];
	unsigned int n = 0;
	u64 off;
	u32 idx;

	p = io_zcrx_maio_copy_page(ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, p);
	idx = zt_slot(test, ifq, p);

	/* Two mappings, no outsider. */
	get_page(p);
	get_page(p);
	atomic_inc(&p->_mapcount);
	atomic_inc(&p->_mapcount);
	io_zcrx_maio_page_put(ifq, p);
	KUNIT_EXPECT_EQ(test, area->kmem->state[idx], IO_ZCRX_KSLOT_FREE);

	/* The same page with an outsider: limbo. */
	q = io_zcrx_maio_copy_page(ifq, &off);
	KUNIT_ASSERT_PTR_EQ(test, q, p);
	get_page(p);
	io_zcrx_maio_page_put(ifq, p);
	KUNIT_EXPECT_EQ(test, area->kmem->state[idx], IO_ZCRX_KSLOT_LIMBO);

	/* A mapping leaves while the outsider stays: still limbo. */
	atomic_dec(&p->_mapcount);
	put_page(p);
	while ((q = io_zcrx_maio_copy_page(ifq, &off))) {
		KUNIT_ASSERT_PTR_NE(test, q, p);
		KUNIT_ASSERT_LT(test, n, ARRAY_SIZE(others));
		others[n++] = q;
	}
	KUNIT_EXPECT_EQ(test, area->kmem->state[idx], IO_ZCRX_KSLOT_LIMBO);

	/* The outsider leaves: the page is ready with one mapping left. */
	put_page(p);
	q = io_zcrx_maio_copy_page(ifq, &off);
	KUNIT_EXPECT_PTR_EQ(test, q, p);

	/* Undo the imitation, so that the page frees cleanly. */
	atomic_dec(&p->_mapcount);
	put_page(p);
	io_zcrx_maio_page_put(ifq, p);
	while (n--)
		io_zcrx_maio_page_put(ifq, others[n]);
	KUNIT_EXPECT_EQ(test, area->free_count, 16U);
	io_put_zcrx_ifq(ifq);
}

/* Freeing the instance leaves a page alive for the outsider that holds it. */
static void zcrx_maio_teardown(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = zt_ifq(test, 16);
	struct page *p;
	u64 off;

	p = io_zcrx_maio_copy_page(ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, p);
	get_page(p);
	io_zcrx_maio_page_put(ifq, p);
	KUNIT_EXPECT_EQ(test, ifq->areas[0]->kmem->limbo_count, 1U);

	io_put_zcrx_ifq(ifq);
	/* The reference of the area is gone, the outsider's remains. */
	KUNIT_EXPECT_EQ(test, page_count(p), 1);
	put_page(p);
}

/* The instance of a ring is found by its id and kept by a reference. */
static void zcrx_maio_get_put(struct kunit *test)
{
	struct io_ring_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct io_zcrx_ifq *ifq = zt_ifq(test, 16), *found;
	struct io_zcrx_ifq *plain = io_zcrx_ifq_alloc(NULL);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_NOT_NULL(test, plain);
	mutex_init(&ctx->uring_lock);
	xa_init_flags(&ctx->zcrx_ctxs, XA_FLAGS_ALLOC);
	KUNIT_ASSERT_EQ(test, xa_err(xa_store(&ctx->zcrx_ctxs, 0, ifq, GFP_KERNEL)), 0);
	KUNIT_ASSERT_EQ(test, xa_err(xa_store(&ctx->zcrx_ctxs, 1, plain, GFP_KERNEL)), 0);

	mutex_lock(&ctx->uring_lock);
	found = io_zcrx_maio_get(ctx, 0);
	KUNIT_EXPECT_PTR_EQ(test, found, ifq);
	KUNIT_EXPECT_EQ(test, refcount_read(&ifq->refs), 2U);
	/* Not found, or not a MAIO instance. */
	KUNIT_EXPECT_NULL(test, io_zcrx_maio_get(ctx, 5));
	KUNIT_EXPECT_NULL(test, io_zcrx_maio_get(ctx, 1));
	mutex_unlock(&ctx->uring_lock);

	io_zcrx_maio_put(found);
	KUNIT_EXPECT_EQ(test, refcount_read(&ifq->refs), 1U);
	xa_destroy(&ctx->zcrx_ctxs);
	io_put_zcrx_ifq(plain);
	io_put_zcrx_ifq(ifq);
}

/*
 * The offset of a CQE is the area token in the top bits, the slot index shifted
 * by the slot size, and the byte offset. It is the same for the slot of a page
 * and for the slot of a net_iov.
 */
static void zcrx_maio_slot_off(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = kunit_kzalloc(test, sizeof(*ifq), GFP_KERNEL);
	struct io_zcrx_area *area = kunit_kzalloc(test, sizeof(*area), GFP_KERNEL);
	struct net_iov *niovs = kunit_kcalloc(test, 8, sizeof(*niovs), GFP_KERNEL);
	const u64 token = (u64)3 << IORING_ZCRX_AREA_SHIFT;
	struct io_zcrx_slot slot;
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, ifq);
	KUNIT_ASSERT_NOT_NULL(test, area);
	KUNIT_ASSERT_NOT_NULL(test, niovs);
	area->ifq = ifq;
	area->area_id = 3;
	area->nia.niovs = niovs;
	area->nia.num_niovs = 8;
	for (i = 0; i < 8; i++)
		net_iov_init(&niovs[i], &area->nia, NET_IOV_IOURING);
	KUNIT_EXPECT_EQ(test, zcrx_area_id_to_token(3), token);

	/* A slot of a kernel area is a page. */
	ifq->niov_shift = PAGE_SHIFT;
	slot = (struct io_zcrx_slot){ .area = area, .idx = 5 };
	KUNIT_EXPECT_EQ(test, io_zcrx_slot_off(ifq, slot, 0), token + 5 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, io_zcrx_slot_off(ifq, slot, 100), token + 5 * PAGE_SIZE + 100);
	KUNIT_EXPECT_EQ(test, io_zcrx_slot_off(ifq, slot, PAGE_SIZE - 1),
			token + 6 * PAGE_SIZE - 1);

	/* An area of more than 4 GiB: the index shifts in 64 bits. */
	slot.idx = (1U << (32 - PAGE_SHIFT)) + 7;
	KUNIT_EXPECT_EQ(test, io_zcrx_slot_off(ifq, slot, 9),
			token + (1ULL << 32) + 7 * PAGE_SIZE + 9);

	/* A net_iov slot has the size of the niov of the instance. */
	ifq->niov_shift = 14;
	slot = io_zcrx_niov_slot(&niovs[6]);
	KUNIT_EXPECT_PTR_EQ(test, slot.area, area);
	KUNIT_EXPECT_EQ(test, slot.idx, 6U);
	KUNIT_EXPECT_EQ(test, io_zcrx_slot_off(ifq, slot, 300), token + 6 * 16384ULL + 300);
	slot.idx = (1U << 18) + 1;
	KUNIT_EXPECT_EQ(test, io_zcrx_slot_off(ifq, slot, 0), token + (1ULL << 32) + 16384);
}

/*
 * A copy page that the process returns through the refill ring goes back to the
 * area. The pool that reads the ring does not own it, so the page takes the way
 * of every page without a pool: it passes the test for outsiders.
 */
static void zcrx_maio_copy_refill(struct kunit *test)
{
	struct page *copy, *p, *q, *cached[ZT_BATCH];
	struct io_zcrx_slot slot;
	struct zt_env env;
	unsigned int n;
	u64 off;
	u32 idx;

	/* No outsider: the page is free, and the pool takes it from the freelist. */
	zt_env_init(test, &env, 16, 1);
	copy = io_zcrx_maio_copy_page(env.ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	idx = zt_slot(test, env.ifq, copy);
	slot = (struct io_zcrx_slot){ .area = env.area, .idx = idx };
	/* What the copy path does after it posts the CQE. */
	io_zcrx_get_uref(slot);
	zt_rqe(&env, off);
	p = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, env.ifq->rq.ring->head, 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&env.area->user_refs[idx]), 0);
	KUNIT_EXPECT_EQ(test, env.stats->limbo_enter, 0ULL);
	/* The area has 16 pages. The pool took all, also the returned page. */
	KUNIT_EXPECT_EQ(test, env.area->free_count, 0U);
	KUNIT_EXPECT_PTR_EQ(test, copy->pp, env.pp[0]);
	KUNIT_EXPECT_EQ(test, env.area->kmem->state[idx], IO_ZCRX_KSLOT_OWNED);
	n = zt_drain_cache(test, env.pp[0], cached, ARRAY_SIZE(cached));
	zt_free(env.pp[0], p);
	while (n--)
		zt_free(env.pp[0], cached[n]);
	zt_env_exit(test, &env);

	/* An outsider holds the page: it waits in limbo until the outsider leaves. */
	zt_env_init(test, &env, 16, 1);
	copy = io_zcrx_maio_copy_page(env.ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	idx = zt_slot(test, env.ifq, copy);
	slot = (struct io_zcrx_slot){ .area = env.area, .idx = idx };
	get_page(copy);
	io_zcrx_get_uref(slot);
	zt_rqe(&env, off);
	p = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, atomic_read(&env.area->user_refs[idx]), 0);
	KUNIT_EXPECT_EQ(test, env.area->kmem->state[idx], IO_ZCRX_KSLOT_LIMBO);
	KUNIT_EXPECT_EQ(test, env.stats->limbo_enter, 1ULL);
	KUNIT_EXPECT_NULL(test, copy->pp);
	put_page(copy);
	/* The next call of the provider reclaims the page. */
	n = zt_drain_cache(test, env.pp[0], cached, ARRAY_SIZE(cached));
	q = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_PTR_EQ(test, q, copy);
	KUNIT_EXPECT_EQ(test, env.stats->limbo_leave, 1ULL);
	zt_free(env.pp[0], q);
	zt_free(env.pp[0], p);
	while (n--)
		zt_free(env.pp[0], cached[n]);
	zt_env_exit(test, &env);
}

/*
 * The receive path passes a reference only to a page that a pool of the
 * instance owns as a slot of the area, in an skb marked for recycling, with
 * the bytes inside the page.
 */
static void zcrx_maio_page_slot(struct kunit *test)
{
	struct page *fb[ZT_BATCH], *copies[4], *copy, *p, *po, *plain, *f;
	struct zt_env env, other, small;
	struct io_zcrx_slot slot = {};
	unsigned int i, n;
	u64 off;

	zt_env_init(test, &env, 32, 1);
	zt_env_init(test, &other, 32, 1);
	zt_env_init(test, &small, ARRAY_SIZE(copies), 1);

	/* The pool takes the free pages of the area, so take a copy page first. */
	copy = io_zcrx_maio_copy_page(env.ifq, &off);
	KUNIT_ASSERT_NOT_NULL(test, copy);

	/* A page of a pool of the instance, with bytes inside the page. */
	p = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_slot(env.ifq, p, 0, 100, true, &slot));
	KUNIT_EXPECT_PTR_EQ(test, slot.area, env.area);
	KUNIT_EXPECT_EQ(test, slot.idx, zt_slot(test, env.ifq, p));
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_slot(env.ifq, p, 0, PAGE_SIZE, true, &slot));
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_slot(env.ifq, p, PAGE_SIZE - 100, 100, true,
						       &slot));
	/* The bytes span two pages, or the skb has no recycle mark: copy. */
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_slot(env.ifq, p, PAGE_SIZE - 99, 100, true,
							&slot));
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_slot(env.ifq, p, 0, PAGE_SIZE + 1, true,
							&slot));
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_slot(env.ifq, p, 0, 100, false, &slot));

	/* A page of a pool of another instance. */
	po = zt_alloc(other.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, po);
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_slot(env.ifq, po, 0, 100, true, &slot));
	KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_slot(other.ifq, po, 0, 100, true, &slot));

	/* A page of the area that no pool owns, and an ordinary page. */
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_slot(env.ifq, copy, 0, 100, true, &slot));
	plain = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, plain);
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_slot(env.ifq, plain, 0, 100, true, &slot));
	__free_page(plain);

	/* A fallback page of a pool of the instance is no slot. */
	for (i = 0; i < ARRAY_SIZE(copies); i++) {
		copies[i] = io_zcrx_maio_copy_page(small.ifq, &off);
		KUNIT_ASSERT_NOT_NULL(test, copies[i]);
		KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_slot(small.ifq, copies[i], 0, 100, true,
								&slot));
	}
	f = zt_alloc(small.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_EXPECT_TRUE(test, page_pool_page_is_pp(f));
	KUNIT_EXPECT_PTR_EQ(test, f->pp, small.pp[0]);
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_slot(small.ifq, f, 0, 100, true, &slot));
	n = zt_drain_cache(test, small.pp[0], fb, ARRAY_SIZE(fb));
	zt_free(small.pp[0], f);
	while (n--)
		zt_free(small.pp[0], fb[n]);
	for (i = 0; i < ARRAY_SIZE(copies); i++)
		io_zcrx_maio_page_put(small.ifq, copies[i]);

	zt_free(other.pp[0], po);
	zt_free(env.pp[0], p);
	io_zcrx_maio_page_put(env.ifq, copy);
	zt_env_exit(test, &small);
	zt_env_exit(test, &other);
	zt_env_exit(test, &env);
}

/*
 * A net_iov belongs to another provider. A MAIO instance cannot copy it, and
 * it owns none, so the fragment fails the request. The function never reaches
 * the request for this case.
 */
static void zcrx_maio_net_iov_frag(struct kunit *test)
{
	struct net_iov_area nia = {};
	struct net_iov niov = {};
	skb_frag_t frag = {};
	struct zt_env env;

	zt_env_init(test, &env, 16, 1);
	net_iov_init(&niov, &nia, NET_IOV_IOURING);
	frag.netmem = net_iov_to_netmem(&niov);
	frag.len = 100;

	KUNIT_EXPECT_EQ(test, io_zcrx_recv_frag(NULL, env.ifq, &frag, 0, 100, true), -EFAULT);
	/* A net_iov of a pool of the MAIO provider does not make it ours. */
	niov.desc.pp = env.pp[0];
	KUNIT_EXPECT_EQ(test, io_zcrx_recv_frag(NULL, env.ifq, &frag, 0, 100, true), -EFAULT);
	KUNIT_EXPECT_EQ(test, io_zcrx_recv_frag(NULL, env.ifq, &frag, 0, 100, false), -EFAULT);
	zt_env_exit(test, &env);
}

/*
 * A pool and a copy share one exhaustion episode. The first allocation that
 * finds the area empty counts it, whoever it is. A retry does not count again.
 * An allocation that gets a page ends the episode.
 */
static void zcrx_maio_copy_episode(struct kunit *test)
{
	struct page *pg[16], *fb[ZT_BATCH], *p;
	struct zt_env env;
	unsigned int i, n;
	u64 off;

	zt_env_init(test, &env, ARRAY_SIZE(pg), 1);
	for (i = 0; i < ARRAY_SIZE(pg); i++) {
		pg[i] = io_zcrx_maio_copy_page(env.ifq, &off);
		KUNIT_ASSERT_NOT_NULL(test, pg[i]);
	}
	KUNIT_EXPECT_FALSE(test, env.ifq->maio_exhausted);
	KUNIT_EXPECT_EQ(test, env.stats->exhaust_count, 0ULL);

	/* A copy finds the area empty: one episode. A retry is the same one. */
	KUNIT_EXPECT_NULL(test, io_zcrx_maio_copy_page(env.ifq, &off));
	KUNIT_EXPECT_TRUE(test, env.ifq->maio_exhausted);
	KUNIT_EXPECT_EQ(test, env.stats->exhaust_count, 1ULL);
	KUNIT_EXPECT_NULL(test, io_zcrx_maio_copy_page(env.ifq, &off));
	KUNIT_EXPECT_EQ(test, env.stats->exhaust_count, 1ULL);

	/* The pool finds the area empty in the same episode, and gets ordinary pages. */
	p = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, env.stats->exhaust_count, 1ULL);
	KUNIT_EXPECT_EQ(test, env.stats->fallback_pages, (u64)ZT_BATCH);
	n = zt_drain_cache(test, env.pp[0], fb, ARRAY_SIZE(fb));

	/* A page returns, and the pool takes it. That ends the episode. */
	io_zcrx_maio_page_put(env.ifq, pg[0]);
	KUNIT_EXPECT_TRUE(test, env.ifq->maio_exhausted);
	zt_free(env.pp[0], p);
	n += zt_drain_cache(test, env.pp[0], fb + n, ARRAY_SIZE(fb) - n);
	p = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_PTR_EQ(test, p, pg[0]);
	KUNIT_EXPECT_FALSE(test, env.ifq->maio_exhausted);

	/* The next empty result starts a new episode, a copy that gets a page ends it. */
	KUNIT_EXPECT_NULL(test, io_zcrx_maio_copy_page(env.ifq, &off));
	KUNIT_EXPECT_EQ(test, env.stats->exhaust_count, 2ULL);
	io_zcrx_maio_page_put(env.ifq, pg[1]);
	KUNIT_EXPECT_PTR_EQ(test, io_zcrx_maio_copy_page(env.ifq, &off), pg[1]);
	KUNIT_EXPECT_FALSE(test, env.ifq->maio_exhausted);

	zt_free(env.pp[0], p);
	while (n--)
		zt_free(env.pp[0], fb[n]);
	for (i = 1; i < ARRAY_SIZE(pg); i++)
		io_zcrx_maio_page_put(env.ifq, pg[i]);
	zt_env_exit(test, &env);
}

/* A fake queue API makes the conversion of the queues of a device testable. */
struct zt_qmem {
	struct page_pool	*pp;
};

static struct {
	int		fail_at;
	int		fail_err;	/* error of the allocation at fail_at, -ENOMEM if 0 */
	int		close_err;	/* error of the restart that removes the provider */
	unsigned int	n;
	int		idx[32];
	bool		mp[32];
} zt_log;

static int zt_queue_mem_alloc(struct net_device *dev,
			      struct netdev_queue_config *qcfg, void *mem, int idx)
{
	struct page_pool_params params = {
		.pool_size	= 16,
		.nid		= NUMA_NO_NODE,
		.netdev		= dev,
		.queue_idx	= idx,
		.flags		= PP_FLAG_PRIVATE_HEADERS,
	};
	bool mp = !!__netif_get_rx_queue(dev, idx)->mp_params.mp_ops;
	struct zt_qmem *q = mem;
	struct page_pool *pp;

	if (zt_log.n < ARRAY_SIZE(zt_log.idx)) {
		zt_log.idx[zt_log.n] = idx;
		zt_log.mp[zt_log.n] = mp;
		zt_log.n++;
	}
	if (mp && idx == zt_log.fail_at)
		return zt_log.fail_err ?: -ENOMEM;
	if (!mp && zt_log.close_err)
		return zt_log.close_err;
	pp = page_pool_create(&params);
	if (IS_ERR(pp))
		return PTR_ERR(pp);
	q->pp = pp;
	return 0;
}

static void zt_queue_mem_free(struct net_device *dev, void *mem)
{
	page_pool_destroy(((struct zt_qmem *)mem)->pp);
}

static int zt_queue_start(struct net_device *dev, struct netdev_queue_config *qcfg,
			  void *mem, int idx)
{
	return 0;
}

static int zt_queue_stop(struct net_device *dev, void *mem, int idx)
{
	return 0;
}

static const struct net_device_ops zt_ndo = {};

static const struct netdev_queue_mgmt_ops zt_qops = {
	.ndo_queue_mem_size	= sizeof(struct zt_qmem),
	.ndo_queue_mem_alloc	= zt_queue_mem_alloc,
	.ndo_queue_mem_free	= zt_queue_mem_free,
	.ndo_queue_start	= zt_queue_start,
	.ndo_queue_stop		= zt_queue_stop,
};

static bool zt_all_clear(struct net_device *dev)
{
	unsigned int i;

	for (i = 0; i < dev->real_num_rx_queues; i++)
		if (__netif_get_rx_queue(dev, i)->mp_params.mp_ops ||
		    __netif_get_rx_queue(dev, i)->mp_params.mp_priv ||
		    __netif_get_rx_queue(dev, i)->mp_params.flags)
			return false;
	return true;
}

/*
 * The core restarts the queues in index order. A failure undoes the queues
 * before it in reverse order and leaves the device as it was. The removal runs
 * in index order.
 */
static void zcrx_maio_queues(struct kunit *test)
{
	struct pp_memory_provider_params p;
	struct io_zcrx_ifq *ifq = zt_ifq(test, 16);
	struct net_device *dev = zt_netdev(test, 4);
	unsigned int i;

	dev->netdev_ops = &zt_ndo;
	dev->queue_mgmt_ops = &zt_qops;
	netdev_lock(dev);

	/* All queues convert, in index order. */
	memset(&zt_log, 0, sizeof(zt_log));
	zt_log.fail_at = -1;
	KUNIT_ASSERT_EQ(test, zcrx_open_maio_queues(dev, ifq), 0);
	KUNIT_EXPECT_EQ(test, ifq->nr_rxq, 4U);
	KUNIT_EXPECT_EQ(test, zt_log.n, 4U);
	for (i = 0; i < 4; i++) {
		KUNIT_EXPECT_EQ(test, zt_log.idx[i], (int)i);
		KUNIT_EXPECT_TRUE(test, zt_log.mp[i]);
		KUNIT_EXPECT_PTR_EQ(test, __netif_get_rx_queue(dev, i)->mp_params.mp_ops,
				    &io_uring_pp_maio_ops);
		KUNIT_EXPECT_PTR_EQ(test, __netif_get_rx_queue(dev, i)->mp_params.mp_priv, ifq);
		KUNIT_EXPECT_EQ(test, __netif_get_rx_queue(dev, i)->mp_params.flags,
				(u32)PP_MP_F_READABLE);
	}
	/* A second instance cannot take the queues. */
	KUNIT_EXPECT_EQ(test, netif_mp_open_rxq(dev, 0, &(struct pp_memory_provider_params){
				.mp_ops = &io_uring_pp_maio_ops, .mp_priv = ifq,
				.flags = PP_MP_F_READABLE }, NULL), -EEXIST);

	/* The removal runs in index order and clears every queue. */
	memset(&p, 0, sizeof(p));
	p.mp_ops = &io_uring_pp_maio_ops;
	p.mp_priv = ifq;
	memset(&zt_log, 0, sizeof(zt_log));
	zt_log.fail_at = -1;
	zcrx_close_maio_queues(dev, ifq, &p);
	KUNIT_EXPECT_EQ(test, ifq->nr_rxq, 0U);
	KUNIT_EXPECT_EQ(test, zt_log.n, 4U);
	for (i = 0; i < 4; i++) {
		KUNIT_EXPECT_EQ(test, zt_log.idx[i], (int)i);
		KUNIT_EXPECT_FALSE(test, zt_log.mp[i]);
	}
	KUNIT_EXPECT_TRUE(test, zt_all_clear(dev));

	/* Queue 2 fails: queues 1 and 0 are undone, in reverse order. */
	memset(&zt_log, 0, sizeof(zt_log));
	zt_log.fail_at = 2;
	KUNIT_EXPECT_EQ(test, zcrx_open_maio_queues(dev, ifq), -ENOMEM);
	KUNIT_EXPECT_EQ(test, ifq->nr_rxq, 0U);
	KUNIT_ASSERT_EQ(test, zt_log.n, 5U);
	KUNIT_EXPECT_EQ(test, zt_log.idx[0], 0);
	KUNIT_EXPECT_EQ(test, zt_log.idx[1], 1);
	KUNIT_EXPECT_EQ(test, zt_log.idx[2], 2);
	KUNIT_EXPECT_TRUE(test, zt_log.mp[2]);
	KUNIT_EXPECT_EQ(test, zt_log.idx[3], 1);
	KUNIT_EXPECT_FALSE(test, zt_log.mp[3]);
	KUNIT_EXPECT_EQ(test, zt_log.idx[4], 0);
	KUNIT_EXPECT_FALSE(test, zt_log.mp[4]);
	KUNIT_EXPECT_TRUE(test, zt_all_clear(dev));

	/* Queue 0 fails: there is nothing to undo. */
	memset(&zt_log, 0, sizeof(zt_log));
	zt_log.fail_at = 0;
	KUNIT_EXPECT_EQ(test, zcrx_open_maio_queues(dev, ifq), -ENOMEM);
	KUNIT_EXPECT_EQ(test, zt_log.n, 1U);
	KUNIT_EXPECT_TRUE(test, zt_all_clear(dev));

	/* The last queue fails: all the others are undone. */
	memset(&zt_log, 0, sizeof(zt_log));
	zt_log.fail_at = 3;
	KUNIT_EXPECT_EQ(test, zcrx_open_maio_queues(dev, ifq), -ENOMEM);
	KUNIT_EXPECT_EQ(test, zt_log.n, 7U);
	KUNIT_EXPECT_EQ(test, zt_log.idx[4], 2);
	KUNIT_EXPECT_EQ(test, zt_log.idx[5], 1);
	KUNIT_EXPECT_EQ(test, zt_log.idx[6], 0);
	KUNIT_EXPECT_TRUE(test, zt_all_clear(dev));

	netdev_unlock(dev);
	free_netdev(dev);
	KUNIT_EXPECT_EQ(test, refcount_read(&ifq->refs), 1U);
	io_put_zcrx_ifq(ifq);
}

/*
 * A driver can fail the allocation of queue memory with -ENETDOWN, as
 * virtio_net does for an interface that is down. The install then fails with
 * that error and leaves the device as it was. The removal accepts the same
 * error from the restart of a queue.
 */
static void zcrx_maio_queues_down(struct kunit *test)
{
	struct pp_memory_provider_params p = {
		.mp_ops	= &io_uring_pp_maio_ops,
		.flags	= PP_MP_F_READABLE,
	};
	struct io_zcrx_ifq *ifq = zt_ifq(test, 16);
	struct net_device *dev = zt_netdev(test, 4);

	p.mp_priv = ifq;
	dev->netdev_ops = &zt_ndo;
	dev->queue_mgmt_ops = &zt_qops;
	netdev_lock(dev);

	/* The first queue fails, so there is nothing to undo. */
	memset(&zt_log, 0, sizeof(zt_log));
	zt_log.fail_at = 0;
	zt_log.fail_err = -ENETDOWN;
	KUNIT_EXPECT_EQ(test, zcrx_open_maio_queues(dev, ifq), -ENETDOWN);
	KUNIT_EXPECT_EQ(test, ifq->nr_rxq, 0U);
	KUNIT_EXPECT_EQ(test, zt_log.n, 1U);
	KUNIT_EXPECT_TRUE(test, zt_all_clear(dev));

	/* The device goes down after queue 1. The undo meets the error too. */
	memset(&zt_log, 0, sizeof(zt_log));
	zt_log.fail_at = 2;
	zt_log.fail_err = -ENETDOWN;
	zt_log.close_err = -ENETDOWN;
	KUNIT_EXPECT_EQ(test, zcrx_open_maio_queues(dev, ifq), -ENETDOWN);
	KUNIT_EXPECT_EQ(test, ifq->nr_rxq, 0U);
	KUNIT_EXPECT_EQ(test, zt_log.n, 5U);
	KUNIT_EXPECT_TRUE(test, zt_all_clear(dev));

	/* The removal on a device that went down clears every queue. */
	memset(&zt_log, 0, sizeof(zt_log));
	zt_log.fail_at = -1;
	KUNIT_ASSERT_EQ(test, zcrx_open_maio_queues(dev, ifq), 0);
	zt_log.close_err = -ENETDOWN;
	zcrx_close_maio_queues(dev, ifq, &p);
	KUNIT_EXPECT_EQ(test, ifq->nr_rxq, 0U);
	KUNIT_EXPECT_TRUE(test, zt_all_clear(dev));

	netdev_unlock(dev);
	free_netdev(dev);
	KUNIT_EXPECT_EQ(test, refcount_read(&ifq->refs), 1U);
	io_put_zcrx_ifq(ifq);
}

/*
 * The core calls uninstall when the device unregisters. A pool of the
 * instance can live on, for example when the driver could not stop a queue
 * and destroys its pool later. The ring can unregister the instance in the
 * meantime. The destroy of the pool then drops the last reference. It needs
 * no device, and the area stays whole until then.
 */
static void zcrx_maio_destroy_after_uninstall(struct kunit *test)
{
	struct netdev_rx_queue *rxq;
	struct zt_env env;
	struct page *page;

	zt_env_init(test, &env, 16, 1);
	rxq = __netif_get_rx_queue(env.dev, 0);
	page = zt_alloc(env.pp[0]);
	KUNIT_ASSERT_NOT_NULL(test, page);
	/* The ring holds one reference and the pool holds one. */
	KUNIT_EXPECT_EQ(test, refcount_read(&env.ifq->refs), 2U);

	/* The device unregisters. The instance drops its hold on the device. */
	netdev_hold(env.dev, &env.ifq->netdev_tracker, GFP_KERNEL);
	env.ifq->netdev = env.dev;
	rxq->mp_params.mp_ops->uninstall(rxq->mp_params.mp_priv, rxq);
	KUNIT_EXPECT_NULL(test, env.ifq->netdev);
	KUNIT_EXPECT_TRUE(test, zt_all_clear(env.dev));

	/* The ring unregisters the instance and finds no device. */
	zcrx_unregister(env.ifq, NULL);
	KUNIT_EXPECT_EQ(test, env.ifq->nr_rxq, 0U);
	KUNIT_EXPECT_EQ(test, refcount_read(&env.ifq->refs), 1U);

	/* The pool gets its page back and dies. This frees the instance. */
	zt_free(env.pp[0], page);
	page_pool_destroy(env.pp[0]);
	free_netdev(env.dev);
}

struct zt_stress {
	struct io_zcrx_ifq	*ifq;
	struct page_pool	*pp;
	struct completion	done;
	atomic_t		busy;
};

/* Take copy pages, and let an outsider hold some of them for a while. */
static int zt_stress_copy(void *data)
{
	struct zt_stress *st = data;
	u32 i;

	for (i = 0; i < 20000; i++) {
		struct page *p;
		u64 off;

		p = io_zcrx_maio_copy_page(st->ifq, &off);
		if (!p) {
			cond_resched();
			continue;
		}
		if (!(i % 3)) {
			get_page(p);
			io_zcrx_maio_page_put(st->ifq, p);
			cond_resched();
			put_page(p);
		} else {
			io_zcrx_maio_page_put(st->ifq, p);
		}
	}
	if (atomic_dec_and_test(&st->busy))
		complete(&st->done);
	return 0;
}

/* Cycle pages through the pool, so that they take the release path. */
static int zt_stress_pool(void *data)
{
	struct zt_stress *st = data;
	u32 i;

	for (i = 0; i < 20000; i++) {
		netmem_ref netmem = page_pool_alloc_netmems(st->pp, GFP_KERNEL);
		struct page *p;

		if (!netmem) {
			cond_resched();
			continue;
		}
		p = netmem_to_page(netmem);
		if (!(i % 4)) {
			get_page(p);
			zt_free(st->pp, p);
			cond_resched();
			put_page(p);
		} else {
			zt_free(st->pp, p);
		}
	}
	if (atomic_dec_and_test(&st->busy))
		complete(&st->done);
	return 0;
}

/*
 * Two threads take copy pages and one thread cycles pages through a pool, all
 * on one area, while outsiders hold references. Lockdep and the state checks
 * watch the locks and the page states.
 */
static void zcrx_maio_stress(struct kunit *test)
{
	struct zt_env env;
	struct zt_stress st = { .busy = ATOMIC_INIT(3) };
	struct io_zcrx_area *area;
	u32 free = 0, limbo = 0;
	u32 i;

	zt_env_init(test, &env, 192, 1);
	area = env.area;
	st.ifq = env.ifq;
	st.pp = env.pp[0];
	init_completion(&st.done);
	KUNIT_ASSERT_FALSE(test, IS_ERR(kthread_run(zt_stress_copy, &st, "zcrx-st0")));
	KUNIT_ASSERT_FALSE(test, IS_ERR(kthread_run(zt_stress_copy, &st, "zcrx-st1")));
	KUNIT_ASSERT_FALSE(test, IS_ERR(kthread_run(zt_stress_pool, &st, "zcrx-st2")));
	wait_for_completion(&st.done);

	/* Empty the cache of the pool, so that every page has left it. */
	page_pool_destroy(env.pp[0]);
	env.pp[0] = NULL;
	/* The threads are done, so no state changes now. */
	for (i = 0; i < area->nia.num_niovs; i++) {
		free += area->kmem->state[i] == IO_ZCRX_KSLOT_FREE;
		limbo += area->kmem->state[i] == IO_ZCRX_KSLOT_LIMBO;
	}
	KUNIT_EXPECT_EQ(test, zt_count(area, IO_ZCRX_KSLOT_OWNED), 0U);
	KUNIT_EXPECT_EQ(test, area->free_count, free);
	KUNIT_EXPECT_EQ(test, area->kmem->limbo_count, limbo);
	KUNIT_EXPECT_EQ(test, free + limbo, (u32)area->nia.num_niovs);
	zt_env_exit(test, &env);
}

#ifdef CONFIG_HAS_DMA
static struct device *zt_dma_device(struct kunit *test, const char *name)
{
	struct device *dev = kunit_device_register(test, name);

	KUNIT_ASSERT_FALSE(test, IS_ERR(dev));
	dev->dma_mask = &dev->coherent_dma_mask;
	KUNIT_ASSERT_EQ(test, dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64)), 0);
	return dev;
}

static struct page_pool *zt_dma_pool(struct net_device *ndev, struct device *dev,
				     unsigned int queue, enum dma_data_direction dir)
{
	struct page_pool_params params = {
		.pool_size	= 64,
		.nid		= NUMA_NO_NODE,
		.dev		= dev,
		.netdev		= ndev,
		.queue_idx	= queue,
		.dma_dir	= dir,
		.max_len	= PAGE_SIZE,
		.flags		= PP_FLAG_PRIVATE_HEADERS | PP_FLAG_DMA_MAP |
				  PP_FLAG_DMA_SYNC_DEV,
	};
	struct page_pool *pp;

	netdev_lock(ndev);
	pp = page_pool_create(&params);
	netdev_unlock(ndev);
	return pp;
}

static dma_addr_t zt_dma_addr(struct page *page)
{
	return page_pool_get_dma_addr_netmem(page_to_netmem(page));
}

/*
 * A pool that maps DMA gets the area mapped, once, for its device. Fallback
 * pages are mapped by the pool and unmapped when the pool releases them. The
 * DMA API debug code of the kernel watches the mappings and the syncs.
 */
static void zcrx_maio_dma(struct kunit *test)
{
	struct io_zcrx_ifq *ifq = zt_ifq(test, 128);
	struct io_zcrx_area *area = ifq->areas[0];
	struct device *dev = zt_dma_device(test, "zcrx-dma0");
	struct device *other = zt_dma_device(test, "zcrx-dma1");
	struct net_device *ndev = zt_netdev(test, 2);
	struct page_pool *a, *b, *c;
	struct page *p, *q, *held[128];
	unsigned int i, j, n = 0;
	u64 off;

	zt_bind(ndev, 0, ifq);
	zt_bind(ndev, 1, ifq);

	/* A pool that uses DMA in both directions cannot share the mapping. */
	c = zt_dma_pool(ndev, dev, 0, DMA_BIDIRECTIONAL);
	KUNIT_EXPECT_TRUE(test, IS_ERR(c));
	KUNIT_EXPECT_FALSE(test, area->is_mapped);

	a = zt_dma_pool(ndev, dev, 0, DMA_FROM_DEVICE);
	KUNIT_ASSERT_FALSE(test, IS_ERR(a));
	KUNIT_EXPECT_TRUE(test, area->is_mapped);
	KUNIT_EXPECT_PTR_EQ(test, ifq->dev, dev);
	for (i = 0; i < area->nia.num_niovs; i++) {
		KUNIT_ASSERT_NE(test, zt_dma_addr(area->mem.pages[i]), (dma_addr_t)0);
		for (j = 0; j < i; j++)
			KUNIT_ASSERT_NE(test, zt_dma_addr(area->mem.pages[i]),
					zt_dma_addr(area->mem.pages[j]));
	}

	/* The pool of the second queue uses the same device and the same mapping. */
	b = zt_dma_pool(ndev, dev, 1, DMA_FROM_DEVICE);
	KUNIT_ASSERT_FALSE(test, IS_ERR(b));
	/* Another device cannot use a mapping for the first device. */
	c = zt_dma_pool(ndev, other, 1, DMA_FROM_DEVICE);
	KUNIT_EXPECT_TRUE(test, IS_ERR(c));

	/* Take the whole area from pool a, and then fallback pages. */
	for (i = 0; i < 128; i++) {
		p = zt_alloc(a);
		KUNIT_ASSERT_NOT_NULL(test, p);
		KUNIT_EXPECT_TRUE(test, io_zcrx_maio_page_off(ifq, p, &off));
		held[n++] = p;
	}
	q = zt_alloc(a);
	KUNIT_ASSERT_NOT_NULL(test, q);
	KUNIT_EXPECT_FALSE(test, io_zcrx_maio_page_off(ifq, q, &off));
	/* The pool mapped the fallback page itself. */
	KUNIT_EXPECT_NE(test, zt_dma_addr(q), (dma_addr_t)0);
	/* An outsider holds it, so that it leaves through the release path. */
	get_page(q);
	zt_free(a, q);
	KUNIT_EXPECT_EQ(test, zt_dma_addr(q), (dma_addr_t)0);
	put_page(q);

	/* Return the area pages, and let the pools go. */
	while (n--)
		zt_free(a, held[n]);
	page_pool_destroy(b);
	page_pool_destroy(a);

	/* The area stays mapped until the instance closes or the device goes. */
	KUNIT_EXPECT_TRUE(test, area->is_mapped);
	scoped_guard(mutex, &ifq->pp_lock)
		io_zcrx_unmap_areas(ifq);
	KUNIT_EXPECT_FALSE(test, area->is_mapped);
	for (i = 0; i < area->nia.num_niovs; i++)
		KUNIT_ASSERT_EQ(test, zt_dma_addr(area->mem.pages[i]), (dma_addr_t)0);

	zt_unbind(ndev, 0);
	zt_unbind(ndev, 1);
	free_netdev(ndev);
	io_put_zcrx_ifq(ifq);
}
#endif

/* Compare the lookup in a chunk table with an xarray keyed by page number. */
static void zcrx_maio_lookup_bench(struct kunit *test)
{
	static const unsigned int sizes[] = { 16, 512, 4096 };
	const unsigned int run = 64, loops = 100000;
	unsigned int s, i, c;
	u64 t0, t1, t2;
	unsigned long sum;
	u32 slot;

	for (s = 0; s < ARRAY_SIZE(sizes); s++) {
		struct io_zcrx_kchunk *chunks = kunit_kcalloc(test, sizes[s], sizeof(*chunks),
							     GFP_KERNEL);
		struct io_zcrx_kmem kmem = { .chunks = chunks, .nr_chunks = sizes[s] };
		struct io_zcrx_area area = { .kmem = &kmem };
		unsigned long *pfns = kunit_kcalloc(test, loops, sizeof(*pfns), GFP_KERNEL);
		struct xarray xa;

		KUNIT_ASSERT_NOT_NULL(test, chunks);
		KUNIT_ASSERT_NOT_NULL(test, pfns);
		xa_init(&xa);
		/* Chunks of 64 pages with a gap of 64 pages between them. */
		for (c = 0; c < sizes[s]; c++) {
			chunks[c].base_pfn = 4096 + (unsigned long)c * 2 * run;
			chunks[c].nr_pages = run;
			chunks[c].first_slot = c * run;
			for (i = 0; i < run; i++) {
				void *old = xa_store(&xa, chunks[c].base_pfn + i,
						     xa_mk_value(c * run + i),
						     GFP_KERNEL);

				KUNIT_ASSERT_EQ(test, xa_err(old), 0);
			}
		}
		for (i = 0; i < loops; i++)
			pfns[i] = 4096 + (get_random_u32() % (sizes[s] * run)) +
				  (get_random_u32() % sizes[s]) * run;

		sum = 0;
		t0 = ktime_get_ns();
		for (i = 0; i < loops; i++)
			if (io_zcrx_kmem_lookup(&area, pfn_to_page(pfns[i]), &slot))
				sum += slot;
		t1 = ktime_get_ns();
		for (i = 0; i < loops; i++) {
			void *v = xa_load(&xa, pfns[i]);

			if (v)
				sum += xa_to_value(v);
		}
		t2 = ktime_get_ns();
		kunit_info(test, "lookup, %5u chunks of %u pages: table %llu ns, xarray %llu ns per call (sum %lu)",
			   sizes[s], run, div_u64(t1 - t0, loops), div_u64(t2 - t1, loops), sum);
		xa_destroy(&xa);
	}
}

static struct kunit_case zcrx_maio_cases[] = {
	KUNIT_CASE(zcrx_maio_gate),
	KUNIT_CASE(zcrx_maio_lookup),
	KUNIT_CASE(zcrx_maio_states),
	KUNIT_CASE(zcrx_maio_refill_ring),
	KUNIT_CASE(zcrx_maio_refill_outsider),
	KUNIT_CASE(zcrx_maio_copy_page),
	KUNIT_CASE(zcrx_maio_owned),
	KUNIT_CASE(zcrx_maio_mapped),
	KUNIT_CASE(zcrx_maio_teardown),
	KUNIT_CASE(zcrx_maio_get_put),
	KUNIT_CASE(zcrx_maio_slot_off),
	KUNIT_CASE(zcrx_maio_copy_refill),
	KUNIT_CASE(zcrx_maio_page_slot),
	KUNIT_CASE(zcrx_maio_net_iov_frag),
	KUNIT_CASE(zcrx_maio_copy_episode),
	KUNIT_CASE(zcrx_maio_queues),
	KUNIT_CASE(zcrx_maio_queues_down),
	KUNIT_CASE(zcrx_maio_destroy_after_uninstall),
	KUNIT_CASE_SLOW(zcrx_maio_stress),
#ifdef CONFIG_HAS_DMA
	KUNIT_CASE(zcrx_maio_dma),
#endif
	KUNIT_CASE_SLOW(zcrx_maio_lookup_bench),
	{}
};

static struct kunit_suite zcrx_maio_suite = {
	.name		= "io_uring-zcrx-maio",
	.test_cases	= zcrx_maio_cases,
};

kunit_test_suite(zcrx_maio_suite);

/*
 * Aids for the KUnit tests of other io_uring code. Those tests cannot reach
 * the static functions of zcrx.c, which includes this file. An aid creates a
 * resource and registers a deferred action that frees it when the test ends.
 * KUnit runs the actions in the reverse order of their registration, so a test
 * must create a pool after the instance that it uses.
 */
static void zt_aid_put_ifq(void *ifq)
{
	io_put_zcrx_ifq(ifq);
}

static void zt_aid_free_netdev(void *dev)
{
	free_netdev(dev);
}

static void zt_aid_destroy_pool(void *pp)
{
	page_pool_destroy(pp);
}

/*
 * Create an instance without a device. Its area has @pages pages. The test owns
 * one reference. The deferred action drops it, and the other holders of the
 * instance must be gone by then.
 */
struct io_zcrx_ifq *io_zcrx_maio_test_ifq(struct kunit *test, unsigned int pages)
{
	struct io_zcrx_ifq *ifq = zt_ifq(test, pages);

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, zt_aid_put_ifq, ifq), 0);
	return ifq;
}

/*
 * Create a page pool that binds the provider of @ifq, through the same gate as
 * the pool of a driver. The pool belongs to queue 0 of a net_device that is not
 * registered. The test must return every page of the pool before it ends,
 * because the pool finishes only when all its pages are back.
 */
struct page_pool *io_zcrx_maio_test_pool(struct kunit *test,
					 struct io_zcrx_ifq *ifq)
{
	struct net_device *dev = zt_netdev(test, 1);
	struct page_pool *pp;

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, zt_aid_free_netdev, dev), 0);
	zt_bind(dev, 0, ifq);
	pp = zt_pool(test, dev, 0, PP_FLAG_PRIVATE_HEADERS);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, zt_aid_destroy_pool, pp), 0);
	return pp;
}
