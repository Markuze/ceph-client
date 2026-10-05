// SPDX-License-Identifier: GPL-2.0
/* Included here so the tests exercise the actual TCP capture actor. */

#include <kunit/test.h>
#include <kunit/skbuff.h>
#include <net/page_pool/helpers.h>

struct kpass_rx_test_ctx {
	struct kpass_session session;
	struct kpass_sock sock;
	struct kpass_buf *buf;
};

static void kpass_test_destroy_pool(void *ptr)
{
	kpass_destroy_buffer_pool(ptr);
}

static void kpass_test_put_page(void *ptr)
{
	put_page(ptr);
}

static void kpass_test_watch_page(struct kunit *test, struct page *page)
{
	get_page(page);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, kpass_test_put_page,
						     page), 0);
}

static int kpass_rx_test_init(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	spin_lock_init(&ctx->session.buf_lock);
	ret = kpass_init_buffer_pool(&ctx->session, 2, 2 * PAGE_SIZE + 17);
	if (ret)
		return ret;
	ret = kunit_add_action_or_reset(test, kpass_test_destroy_pool,
				       &ctx->session);
	if (ret)
		return ret;
	ctx->sock.session = &ctx->session;
	ctx->buf = kpass_buf_alloc(&ctx->session);
	ctx->buf->sock = &ctx->sock;
	test->priv = ctx;
	return 0;
}

static struct sk_buff *kpass_test_page_skb(struct kunit *test,
					unsigned int order,
					unsigned int reserve,
					unsigned int len)
{
	struct sk_buff *skb;
	struct page *page;

	page = alloc_pages(GFP_KERNEL | __GFP_COMP, order);
	if (!page)
		return NULL;
	skb = build_skb(page_address(page), PAGE_SIZE << order);
	if (!skb) {
		put_page(page);
		return NULL;
	}
	if (kunit_add_action_or_reset(test, kunit_action_kfree_skb, skb))
		return NULL;
	skb_reserve(skb, reserve);
	memset(skb_put(skb, len), 0x5a, len);
	return skb;
}

static struct sk_buff *kpass_test_slab_skb(struct kunit *test, unsigned int len)
{
	struct sk_buff *skb = kunit_zalloc_skb(test, len, GFP_KERNEL);

	if (skb)
		memset(skb_put(skb, len), 0x36, len);
	return skb;
}

static struct page *kpass_test_add_frag(struct sk_buff *skb, unsigned int order,
					unsigned int offset, unsigned int len)
{
	struct page *page = alloc_pages(GFP_KERNEL | __GFP_COMP, order);

	if (!page)
		return NULL;
	memset(page_address(page) + offset, 0xa7, len);
	skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags, page, offset, len,
			PAGE_SIZE << order);
	return page;
}

static void kpass_test_add_child(struct sk_buff *parent, struct sk_buff *child)
{
	struct sk_buff **tail = &skb_shinfo(parent)->frag_list;

	while (*tail)
		tail = &(*tail)->next;
	/* The test and parent each own a reference to the child. */
	*tail = skb_get(child);
	parent->len += child->len;
	parent->data_len += child->len;
	parent->truesize += child->truesize;
}

static void kpass_test_expect_bytes(struct kunit *test, struct kpass_buf *buf,
				    const void *expected, unsigned int len)
{
	unsigned int i, offset = 0;

	KUNIT_ASSERT_EQ(test, buf->total_len, len);
	for (i = 0; i < buf->sg_count; i++) {
		struct kpass_sg_entry *sg = &buf->sgvec[i];
		void *data;

		KUNIT_ASSERT_LT(test, sg->offset, (u32)PAGE_SIZE);
		KUNIT_ASSERT_GT(test, sg->length, 0U);
		KUNIT_ASSERT_LE(test, sg->length, (u32)PAGE_SIZE - sg->offset);
		KUNIT_ASSERT_LE(test, sg->length, len - offset);
		data = kmap_local_page(sg->page);
		KUNIT_EXPECT_MEMEQ(test, data + sg->offset, expected + offset,
				   sg->length);
		kunmap_local(data);
		offset += sg->length;
	}
	KUNIT_EXPECT_EQ(test, offset, len);
}

static void kpass_rx_page_head_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct sk_buff *skb = kpass_test_page_skb(test, 0, 128, 256);
	read_descriptor_t desc = { .count = 64, .arg.data = ctx->buf };
	struct page *page;
	u8 expected[64];

	KUNIT_ASSERT_NOT_NULL(test, skb);
	page = virt_to_page(skb->data);
	kpass_test_watch_page(test, page);
	memcpy(expected, skb->data + 7, sizeof(expected));
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 7, 249), 64);
	KUNIT_EXPECT_EQ(test, ctx->buf->sg_count, 1U);
	KUNIT_EXPECT_PTR_EQ(test, ctx->buf->sgvec[0].page, page);
	KUNIT_EXPECT_EQ(test, ctx->buf->sgvec[0].offset, 135U);
	KUNIT_EXPECT_EQ(test, page_count(page), 3);
	KUNIT_EXPECT_EQ(test, ctx->buf->copied_len, 0U);
	KUNIT_EXPECT_EQ(test, desc.count, (size_t)0);
	KUNIT_EXPECT_EQ(test, desc.written, (size_t)64);
	kunit_kfree_skb(test, skb);
	KUNIT_EXPECT_EQ(test, page_count(page), 2);
	kpass_test_expect_bytes(test, ctx->buf, expected, sizeof(expected));
	kpass_buf_free(&ctx->session, ctx->buf);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	KUNIT_EXPECT_EQ(test, ctx->buf->sg_count, 0U);
	KUNIT_EXPECT_FALSE(test, ctx->buf->captured);
}

static void kpass_rx_cloned_head_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct sk_buff *skb = kpass_test_page_skb(test, 0, 128, 64);
	read_descriptor_t desc = { .count = 64, .arg.data = ctx->buf };
	struct sk_buff *clone;
	struct page *pool_page = ctx->buf->pages[0];
	struct page *copy_page;
	int refs = page_count(pool_page);
	u8 expected[64];

	KUNIT_ASSERT_NOT_NULL(test, skb);
	clone = skb_clone(skb, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, clone);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
				kunit_action_kfree_skb, clone), 0);
	KUNIT_ASSERT_TRUE(test, skb_head_is_locked(skb));
	memcpy(expected, skb->data, sizeof(expected));
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 0, 64), 64);
	KUNIT_EXPECT_EQ(test, ctx->buf->copied_len, 64U);
	copy_page = ctx->buf->sgvec[0].page;
	KUNIT_EXPECT_PTR_NE(test, copy_page, virt_to_page(skb->data));
	KUNIT_EXPECT_PTR_NE(test, copy_page, pool_page);
	KUNIT_EXPECT_EQ(test, page_count(copy_page), 1);
	kpass_test_watch_page(test, copy_page);
	KUNIT_EXPECT_EQ(test, page_count(pool_page), refs);
	memset(clone->data, 0xcc, 64);
	kunit_kfree_skb(test, clone);
	kunit_kfree_skb(test, skb);
	kpass_test_expect_bytes(test, ctx->buf, expected, sizeof(expected));
	kpass_buf_release_capture(ctx->buf);
	KUNIT_EXPECT_EQ(test, page_count(copy_page), 1);
	KUNIT_EXPECT_EQ(test, page_count(pool_page), refs);
}

static void kpass_rx_slab_copy_pages_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	unsigned int len = PAGE_SIZE + 300;
	struct sk_buff *skb = kpass_test_slab_skb(test, len);
	struct kpass_buf *buf = kpass_buf_alloc(&ctx->session);
	read_descriptor_t desc = { .count = len, .arg.data = buf };
	u8 *expected = kunit_kmalloc(test, len, GFP_KERNEL);
	struct page *first, *second;
	void *pool;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	KUNIT_ASSERT_NOT_NULL(test, expected);
	KUNIT_ASSERT_FALSE(test, skb->head_frag);
	buf->sock = &ctx->sock;
	/* This buffer's pool slot has an unaligned stride. RX must ignore it. */
	pool = kpass_buf_kaddr(&ctx->session, buf);
	memset(pool, 0xee, len);
	memcpy(expected, skb->data, len);
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 0, len), (int)len);
	KUNIT_EXPECT_EQ(test, buf->copied_len, len);
	KUNIT_ASSERT_EQ(test, buf->sg_count, 2U);
	first = buf->sgvec[0].page;
	second = buf->sgvec[1].page;
	KUNIT_EXPECT_EQ(test, buf->sgvec[0].offset, 0U);
	KUNIT_EXPECT_EQ(test, buf->sgvec[1].offset, 0U);
	KUNIT_EXPECT_EQ(test, buf->sgvec[0].length, (u32)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, buf->sgvec[1].length, 300U);
	KUNIT_EXPECT_PTR_NE(test, first, second);
	KUNIT_EXPECT_PTR_NE(test, first, vmalloc_to_page(pool));
	KUNIT_EXPECT_PTR_NE(test, second, vmalloc_to_page(pool + PAGE_SIZE));
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(pool, 0xee, len), NULL);
	KUNIT_EXPECT_EQ(test, page_count(first), 1);
	KUNIT_EXPECT_EQ(test, page_count(second), 1);
	kpass_test_watch_page(test, first);
	kpass_test_watch_page(test, second);
	memset(skb->data, 0xcc, len);
	memset(pool, 0xcc, len);
	kunit_kfree_skb(test, skb);
	kpass_test_expect_bytes(test, buf, expected, len);
	kpass_buf_release_capture(buf);
	KUNIT_EXPECT_EQ(test, page_count(first), 1);
	KUNIT_EXPECT_EQ(test, page_count(second), 1);
}

static void kpass_rx_cross_page_head_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct sk_buff *skb = kpass_test_page_skb(test, 1, PAGE_SIZE - 32, 128);
	read_descriptor_t desc = { .count = 96, .arg.data = ctx->buf };
	struct page *page;
	u8 expected[96];

	KUNIT_ASSERT_NOT_NULL(test, skb);
	page = virt_to_page(skb->head);
	kpass_test_watch_page(test, page);
	memcpy(expected, skb->data + 8, sizeof(expected));
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 8, 120), 96);
	KUNIT_EXPECT_EQ(test, ctx->buf->sg_count, 2U);
	KUNIT_EXPECT_EQ(test, ctx->buf->sgvec[0].length, 24U);
	KUNIT_EXPECT_PTR_EQ(test, ctx->buf->sgvec[1].page, page + 1);
	KUNIT_EXPECT_EQ(test, ctx->buf->copied_len, 0U);
	KUNIT_EXPECT_EQ(test, page_count(page), 4);
	kunit_kfree_skb(test, skb);
	kpass_test_expect_bytes(test, ctx->buf, expected, sizeof(expected));
	kpass_buf_release_capture(ctx->buf);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
}

static void kpass_rx_cross_page_frag_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct sk_buff *skb = kpass_test_slab_skb(test, 0);
	read_descriptor_t desc = { .count = 96, .arg.data = ctx->buf };
	struct page *page;
	u8 expected[96];

	KUNIT_ASSERT_NOT_NULL(test, skb);
	page = kpass_test_add_frag(skb, 1, PAGE_SIZE - 32, 128);
	KUNIT_ASSERT_NOT_NULL(test, page);
	kpass_test_watch_page(test, page);
	KUNIT_ASSERT_EQ(test, skb_copy_bits(skb, 8, expected, sizeof(expected)), 0);
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 8, 120), 96);
	KUNIT_EXPECT_EQ(test, ctx->buf->sg_count, 2U);
	KUNIT_EXPECT_EQ(test, ctx->buf->sgvec[0].length, 24U);
	KUNIT_EXPECT_PTR_EQ(test, ctx->buf->sgvec[1].page, page + 1);
	KUNIT_EXPECT_EQ(test, ctx->buf->copied_len, 0U);
	KUNIT_EXPECT_EQ(test, page_count(page), 4);
	kunit_kfree_skb(test, skb);
	kpass_test_expect_bytes(test, ctx->buf, expected, sizeof(expected));
	kpass_buf_release_capture(ctx->buf);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
}

static void kpass_rx_frag_list_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct sk_buff *skb = kpass_test_page_skb(test, 0, 128, 32);
	struct sk_buff *child = kpass_test_slab_skb(test, 23);
	struct sk_buff *grandchild = kpass_test_page_skb(test, 0, 128, 37);
	struct sk_buff *next = kpass_test_page_skb(test, 0, 128, 55);
	read_descriptor_t desc = { .count = 150, .arg.data = ctx->buf };
	u8 expected[150];

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_NOT_NULL(test, child);
	KUNIT_ASSERT_NOT_NULL(test, grandchild);
	KUNIT_ASSERT_NOT_NULL(test, next);
	KUNIT_ASSERT_NOT_NULL(test, kpass_test_add_frag(skb, 0, 64, 64));
	kpass_test_add_child(child, grandchild);
	kpass_test_add_child(skb, child);
	kpass_test_add_child(skb, next);
	KUNIT_ASSERT_EQ(test, skb_copy_bits(skb, 16, expected, 150), 0);
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 16, 195), 150);
	KUNIT_EXPECT_EQ(test, ctx->buf->copied_len, 23U);
	kpass_test_expect_bytes(test, ctx->buf, expected, 150);
	kpass_buf_release_capture(ctx->buf);
	/* Start within the nested child, skipping the head and frags. */
	desc.count = 200;
	desc.written = 0;
	KUNIT_ASSERT_EQ(test, skb_copy_bits(skb, 129, expected, 82), 0);
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 129, 82), 82);
	KUNIT_EXPECT_EQ(test, ctx->buf->copied_len, 0U);
	KUNIT_EXPECT_PTR_EQ(test, ctx->buf->sgvec[0].page,
			    virt_to_page(grandchild->data));
	kunit_kfree_skb(test, skb);
	kunit_kfree_skb(test, child);
	kunit_kfree_skb(test, grandchild);
	kunit_kfree_skb(test, next);
	kpass_test_expect_bytes(test, ctx->buf, expected, 82);
}

static void kpass_rx_shared_frag_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct sk_buff *skb = kpass_test_page_skb(test, 0, 128, 32);
	struct sk_buff *child = kpass_test_page_skb(test, 0, 128, 48);
	read_descriptor_t desc = { .count = 176, .arg.data = ctx->buf };
	struct page *frag;
	u8 expected[176];

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_NOT_NULL(test, child);
	frag = kpass_test_add_frag(skb, 0, 64, 96);
	KUNIT_ASSERT_NOT_NULL(test, frag);
	kpass_test_add_child(skb, child);
	skb_shinfo(skb)->flags |= SKBFL_SHARED_FRAG;
	KUNIT_ASSERT_EQ(test, skb_copy_bits(skb, 0, expected, sizeof(expected)), 0);
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 0, 176), 176);
	KUNIT_EXPECT_EQ(test, ctx->buf->copied_len, 144U);
	KUNIT_EXPECT_PTR_EQ(test, ctx->buf->sgvec[0].page, virt_to_page(skb->data));
	memset(page_address(frag) + 64, 0xcc, 96);
	memset(child->data, 0xcc, 48);
	kunit_kfree_skb(test, skb);
	kunit_kfree_skb(test, child);
	kpass_test_expect_bytes(test, ctx->buf, expected, sizeof(expected));
}

static void kpass_rx_sg_limit_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct sk_buff *skb = kpass_test_page_skb(test, 1, PAGE_SIZE - 32, 128);
	read_descriptor_t desc = { .count = 96, .arg.data = ctx->buf };

	KUNIT_ASSERT_NOT_NULL(test, skb);
	ctx->buf->sg_max = 1;
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 0, 128), 32);
	KUNIT_EXPECT_EQ(test, desc.count, (size_t)64);
	KUNIT_EXPECT_EQ(test, desc.written, (size_t)32);
	KUNIT_EXPECT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 32, 96), -ENOSPC);
	KUNIT_EXPECT_EQ(test, ctx->buf->total_len, 32U);
	KUNIT_EXPECT_EQ(test, ctx->buf->copied_len, 0U);
	KUNIT_EXPECT_EQ(test, desc.count, (size_t)64);
	kpass_test_expect_bytes(test, ctx->buf, skb->data, 32);
}

static void kpass_rx_buffer_limit_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct sk_buff *skb = kpass_test_slab_skb(test, 0);
	read_descriptor_t desc = { .count = 3 * PAGE_SIZE, .arg.data = ctx->buf };
	struct page *page;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	page = kpass_test_add_frag(skb, 2, 0, 3 * PAGE_SIZE);
	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 0, skb->len),
			(int)ctx->session.buf_size);
	KUNIT_EXPECT_EQ(test, desc.count, (size_t)PAGE_SIZE - 17);
	KUNIT_EXPECT_EQ(test, kpass_tcp_recv_actor(&desc, skb, ctx->buf->total_len,
						 desc.count), -ENOSPC);
	kpass_test_expect_bytes(test, ctx->buf, page_address(page),
				ctx->session.buf_size);
}

static void kpass_rx_unreadable_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct sk_buff *skb = kpass_test_page_skb(test, 0, 128, 32);
	struct sk_buff *child = kpass_test_page_skb(test, 0, 128, 32);
	read_descriptor_t desc = { .count = 64, .arg.data = ctx->buf };

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_NOT_NULL(test, child);
	kpass_test_add_child(skb, child);
	skb->unreadable = true;
	KUNIT_EXPECT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 0, 64), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->buf->sg_count, 0U);
	KUNIT_EXPECT_EQ(test, desc.count, (size_t)64);
	skb->unreadable = false;
	child->unreadable = true;
	KUNIT_EXPECT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 0, 64), 32);
	KUNIT_EXPECT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 32, 32), -EIO);
	KUNIT_EXPECT_EQ(test, desc.count, (size_t)32);
	kpass_test_expect_bytes(test, ctx->buf, skb->data, 32);
}

static void kpass_rx_reuse_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct sk_buff *skb = kpass_test_page_skb(test, 0, 128, 64);
	struct sk_buff *slab = kpass_test_slab_skb(test, 80);
	read_descriptor_t desc = { .count = 144, .arg.data = ctx->buf };
	struct page *page, *copy_page;
	u8 expected[144];

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_NOT_NULL(test, slab);
	page = virt_to_page(skb->data);
	kpass_test_watch_page(test, page);
	memcpy(expected, skb->data, 64);
	memcpy(expected + 64, slab->data, 80);
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 0, 64), 64);
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, slab, 0, 80), 80);
	KUNIT_EXPECT_EQ(test, ctx->buf->copied_len, 80U);
	KUNIT_EXPECT_EQ(test, ctx->buf->sgvec[1].offset, 0U);
	copy_page = ctx->buf->sgvec[1].page;
	KUNIT_EXPECT_EQ(test, page_count(copy_page), 1);
	kpass_test_watch_page(test, copy_page);
	kpass_test_expect_bytes(test, ctx->buf, expected, sizeof(expected));
	/* This is the release used when RECV replaces a previous capture. */
	kpass_buf_release_capture(ctx->buf);
	KUNIT_EXPECT_EQ(test, page_count(page), 2);
	KUNIT_EXPECT_EQ(test, page_count(copy_page), 1);
	KUNIT_EXPECT_EQ(test, ctx->buf->copied_len, 0U);
	desc.count = 64;
	desc.written = 0;
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 0, 64), 64);
	kunit_kfree_skb(test, skb);
	/* Session teardown must also release a still-allocated capture. */
	kunit_release_action(test, kpass_test_destroy_pool, &ctx->session);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
}

static int kpass_test_splice_capture(struct sk_buff *skb, struct kpass_buf *buf)
{
	unsigned int i;
	int total = 0;

	for (i = 0; i < buf->sg_count; i++) {
		struct kpass_sg_entry *sg = &buf->sgvec[i];
		struct iov_iter iter;
		struct bio_vec bvec;
		int ret;

		bvec_set_page(&bvec, sg->page, sg->length, sg->offset);
		iov_iter_bvec(&iter, ITER_SOURCE, &bvec, 1, sg->length);
		ret = skb_splice_from_iter(skb, &iter, sg->length);
		if (ret != sg->length)
			return ret < 0 ? ret : -EIO;
		total += ret;
	}
	return total;
}

static void kpass_rx_copy_reuse_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct sk_buff *src = kpass_test_slab_skb(test, 64);
	struct sk_buff *tx = kpass_test_slab_skb(test, 0);
	read_descriptor_t desc = { .count = 64, .arg.data = ctx->buf };
	u32 id = ctx->buf->id;
	u8 expected[128], actual[128];

	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_NOT_NULL(test, tx);
	/* Occupy the other handle so free/alloc below returns this same ID. */
	KUNIT_ASSERT_NOT_NULL(test, kpass_buf_alloc(&ctx->session));
	memcpy(expected, src->data, 64);
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, src, 0, 64), 64);
	KUNIT_ASSERT_EQ(test, kpass_test_splice_capture(tx, ctx->buf), 64);
	KUNIT_ASSERT_PTR_EQ(test, skb_frag_page(&skb_shinfo(tx)->frags[0]),
			    ctx->buf->sgvec[0].page);

	/* A new receive replaces the capture while TX retains its pages. */
	kpass_buf_release_capture(ctx->buf);
	memset(src->data, 0x7b, 64);
	memcpy(expected + 64, src->data, 64);
	desc.count = 64;
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, src, 0, 64), 64);
	KUNIT_ASSERT_EQ(test, skb_copy_bits(tx, 0, actual, 64), 0);
	KUNIT_EXPECT_MEMEQ(test, actual, expected, 64);
	KUNIT_ASSERT_EQ(test, kpass_test_splice_capture(tx, ctx->buf), 64);

	/* Free/reallocate the handle and copy different bytes once more. */
	kpass_buf_free(&ctx->session, ctx->buf);
	ctx->buf = kpass_buf_alloc(&ctx->session);
	KUNIT_ASSERT_NOT_NULL(test, ctx->buf);
	KUNIT_ASSERT_EQ(test, ctx->buf->id, id);
	ctx->buf->sock = &ctx->sock;
	desc.arg.data = ctx->buf;
	desc.count = 64;
	memset(src->data, 0xcc, 64);
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, src, 0, 64), 64);
	KUNIT_ASSERT_EQ(test, skb_copy_bits(tx, 0, actual, sizeof(actual)), 0);
	KUNIT_EXPECT_MEMEQ(test, actual, expected, sizeof(expected));

	/* The already-spliced bytes must also outlive the session. */
	kunit_release_action(test, kpass_test_destroy_pool, &ctx->session);
	KUNIT_ASSERT_EQ(test, skb_copy_bits(tx, 0, actual, sizeof(actual)), 0);
	KUNIT_EXPECT_MEMEQ(test, actual, expected, sizeof(expected));
}

static void kpass_test_destroy_page_pool(void *ptr)
{
	page_pool_destroy(ptr);
}

static void kpass_rx_page_pool_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct page_pool_params params = { .pool_size = 8, .nid = NUMA_NO_NODE };
	read_descriptor_t desc = { .count = 64, .arg.data = ctx->buf };
	struct page_pool *pool;
	struct sk_buff *skb;
	struct page *page;
	u8 expected[64];

	pool = page_pool_create(&params);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pool);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
				kpass_test_destroy_page_pool, pool), 0);
	page = page_pool_alloc_pages(pool, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);
	skb = build_skb(page_address(page), PAGE_SIZE);
	if (!skb)
		page_pool_put_full_page(pool, page, false);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_mark_for_recycle(skb);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
				kunit_action_kfree_skb, skb), 0);
	skb_reserve(skb, 128);
	memset(skb_put(skb, 64), 0x5a, 64);
	memcpy(expected, skb->data, sizeof(expected));
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, skb, 0, 64), 64);
	KUNIT_EXPECT_PTR_EQ(test, ctx->buf->sgvec[0].page, page);
	KUNIT_EXPECT_EQ(test, page_count(page), 2);
	KUNIT_EXPECT_EQ(test, ctx->buf->copied_len, 0U);
	/* The capture reference alone must prevent page-pool recycling. */
	kunit_kfree_skb(test, skb);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	kunit_release_action(test, kpass_test_destroy_page_pool, pool);
	kpass_test_expect_bytes(test, ctx->buf, expected, sizeof(expected));
}

static void kpass_test_release_socket(void *ptr)
{
	sock_release(ptr);
}

static struct socket *kpass_test_socket(struct kunit *test)
{
	struct socket *sock;
	int ret;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (ret)
		return ERR_PTR(ret);
	ret = kunit_add_action_or_reset(test, kpass_test_release_socket, sock);
	return ret ? ERR_PTR(ret) : sock;
}

static void kpass_rx_tcp_stream_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct socket *listener = kpass_test_socket(test);
	struct socket *client = kpass_test_socket(test);
	read_descriptor_t desc = { .count = 17, .arg.data = ctx->buf };
	unsigned int len = PAGE_SIZE + 128, i;
	u8 *expected = kunit_kmalloc(test, len, GFP_KERNEL);
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT };
	struct socket *server;
	struct kvec vec;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, listener);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, client);
	KUNIT_ASSERT_NOT_NULL(test, expected);
	KUNIT_ASSERT_EQ(test, kernel_bind(listener, (struct sockaddr_unsized *)&addr,
					 sizeof(addr)), 0);
	KUNIT_ASSERT_EQ(test, kernel_listen(listener, 1), 0);
	KUNIT_ASSERT_GT(test, kernel_getsockname(listener, (struct sockaddr *)&addr), 0);
	KUNIT_ASSERT_EQ(test, kernel_connect(client, (struct sockaddr_unsized *)&addr,
					    sizeof(addr), 0), 0);
	KUNIT_ASSERT_EQ(test, kernel_accept(listener, &server, O_NONBLOCK), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
				kpass_test_release_socket, server), 0);
	for (i = 0; i < len; i++)
		expected[i] = i * 31 + 7;
	vec.iov_base = expected;
	vec.iov_len = len;
	KUNIT_ASSERT_EQ(test, kernel_sendmsg(client, &msg, &vec, 1, len), (int)len);

	lock_sock(server->sk);
	ret = tcp_read_sock(server->sk, &desc, kpass_tcp_recv_actor);
	release_sock(server->sk);
	KUNIT_ASSERT_EQ(test, ret, 17);
	KUNIT_EXPECT_EQ(test, desc.count, (size_t)0);
	kpass_test_expect_bytes(test, ctx->buf, expected, 17);

	desc.count = len - 17;
	lock_sock(server->sk);
	ret = tcp_read_sock(server->sk, &desc, kpass_tcp_recv_actor);
	release_sock(server->sk);
	KUNIT_ASSERT_EQ(test, ret, (int)len - 17);
	KUNIT_EXPECT_EQ(test, desc.count, (size_t)0);
	KUNIT_EXPECT_TRUE(test, skb_queue_empty(&server->sk->sk_receive_queue));
	kunit_release_action(test, kpass_test_release_socket, server);
	kunit_release_action(test, kpass_test_release_socket, client);
	kpass_test_expect_bytes(test, ctx->buf, expected, len);
	kunit_info(test, "TCP RX: %u bytes retained, %u bytes copied\n",
		   ctx->buf->total_len - ctx->buf->copied_len, ctx->buf->copied_len);
}

static void kpass_rx_copy_tcp_test(struct kunit *test)
{
	struct kpass_rx_test_ctx *ctx = test->priv;
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct socket *listener = kpass_test_socket(test);
	struct socket *client = kpass_test_socket(test);
	struct sk_buff *src = kpass_test_slab_skb(test, 64);
	read_descriptor_t desc = { .count = 64, .arg.data = ctx->buf };
	struct msghdr msg = {};
	struct socket *server;
	u8 expected[128], actual[128];
	struct kvec vec = { .iov_base = actual, .iov_len = sizeof(actual) };
	unsigned int i;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, listener);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, client);
	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_EQ(test, kernel_bind(listener, (struct sockaddr_unsized *)&addr,
					 sizeof(addr)), 0);
	KUNIT_ASSERT_EQ(test, kernel_listen(listener, 1), 0);
	KUNIT_ASSERT_GT(test, kernel_getsockname(listener, (struct sockaddr *)&addr), 0);
	KUNIT_ASSERT_EQ(test, kernel_connect(client, (struct sockaddr_unsized *)&addr,
					    sizeof(addr), 0), 0);
	KUNIT_ASSERT_EQ(test, kernel_accept(listener, &server, O_NONBLOCK), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
				kpass_test_release_socket, server), 0);
	server->sk->sk_rcvtimeo = HZ;

	ctx->sock.sock = client;
	spin_lock_init(&ctx->sock.tx_lock);
	INIT_LIST_HEAD(&ctx->sock.tx_queue);
	INIT_WORK(&ctx->sock.tx_work, kpass_tx_work_fn);
	memcpy(expected, src->data, 64);
	memcpy(expected + 64, src->data, 64);
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, src, 0, 64), 64);
	/* Send the same captured bytes twice through the actual TX worker. */
	for (i = 0; i < 2; i++) {
		ctx->buf->offset = 0;
		ctx->buf->length = 64;
		ctx->buf->state = BUF_TX_QUEUED;
		list_add_tail(&ctx->buf->list, &ctx->sock.tx_queue);
		kpass_tx_work_fn(&ctx->sock.tx_work);
		KUNIT_ASSERT_TRUE(test, list_empty(&ctx->sock.tx_queue));
		KUNIT_ASSERT_EQ(test, ctx->buf->state, BUF_ALLOCATED);
	}

	/* Repost and release before the peer reads either copy of the data. */
	kpass_buf_release_capture(ctx->buf);
	memset(src->data, 0xcc, 64);
	desc.count = 64;
	KUNIT_ASSERT_EQ(test, kpass_tcp_recv_actor(&desc, src, 0, 64), 64);
	kpass_buf_free(&ctx->session, ctx->buf);
	KUNIT_ASSERT_EQ(test, kernel_recvmsg(server, &msg, &vec, 1, sizeof(actual),
					    MSG_WAITALL), (int)sizeof(actual));
	KUNIT_EXPECT_MEMEQ(test, actual, expected, sizeof(expected));
}

static struct kunit_case kpass_rx_test_cases[] = {
	KUNIT_CASE(kpass_rx_page_head_test),
	KUNIT_CASE(kpass_rx_cloned_head_test),
	KUNIT_CASE(kpass_rx_slab_copy_pages_test),
	KUNIT_CASE(kpass_rx_cross_page_head_test),
	KUNIT_CASE(kpass_rx_cross_page_frag_test),
	KUNIT_CASE(kpass_rx_frag_list_test),
	KUNIT_CASE(kpass_rx_shared_frag_test),
	KUNIT_CASE(kpass_rx_sg_limit_test),
	KUNIT_CASE(kpass_rx_buffer_limit_test),
	KUNIT_CASE(kpass_rx_unreadable_test),
	KUNIT_CASE(kpass_rx_reuse_test),
	KUNIT_CASE(kpass_rx_copy_reuse_test),
	KUNIT_CASE(kpass_rx_page_pool_test),
	KUNIT_CASE(kpass_rx_tcp_stream_test),
	KUNIT_CASE(kpass_rx_copy_tcp_test),
	{}
};

static struct kunit_suite kpass_rx_test_suite = {
	.name = "kpass-rx",
	.init = kpass_rx_test_init,
	.test_cases = kpass_rx_test_cases,
};

kunit_test_suite(kpass_rx_test_suite);
