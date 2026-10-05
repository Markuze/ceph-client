// SPDX-License-Identifier: GPL-2.0
/* Included after the RX tests to reuse their skb/page fixtures. */

struct kpass_stream_test_ctx {
	struct kpass_session session;
	struct kpass_sock sock;
};

static void kpass_test_stream_cleanup(void *ptr)
{
	struct kpass_stream_test_ctx *ctx = ptr;
	unsigned int i;

	kpass_extents_free(&ctx->session, &ctx->sock.stream);
	for (i = 0; i < KPASS_MAX_OBJECTS; i++)
		kpass_object_put(&ctx->session, ctx->session.objects[i]);
}

static int kpass_stream_test_init(struct kunit *test)
{
	struct kpass_stream_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	INIT_LIST_HEAD(&ctx->session.requests);
	mutex_init(&ctx->session.stream_lock);
	ctx->session.backing_limit = KPASS_DEFAULT_LIMIT;
	ctx->sock.session = &ctx->session;
	INIT_LIST_HEAD(&ctx->sock.stream);
	test->priv = ctx;
	return kunit_add_action_or_reset(test, kpass_test_stream_cleanup, ctx);
}

static void kpass_test_request_cleanup(void *ptr)
{
	struct kpass_request *req = ptr;

	list_del_init(&req->list);
	kpass_object_put(req->session, req->object);
}

static struct kpass_request *kpass_test_request(struct kunit *test, u8 op,
						u64 offset, u64 length)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	struct kpass_request *req;

	req = kunit_kzalloc(test, sizeof(*req), GFP_KERNEL);
	if (!req)
		return NULL;
	req->session = &ctx->session;
	req->sock = &ctx->sock;
	req->cmd.op = op;
	req->cmd.stream.offset = offset;
	req->cmd.stream.length = length;
	if (op != KPASS_OP_DISCARD) {
		req->object = kpass_object_alloc();
		if (!req->object)
			return NULL;
	}
	INIT_LIST_HEAD(&req->list);
	if (kunit_add_action_or_reset(test, kpass_test_request_cleanup, req))
		return NULL;
	list_add_tail(&req->list, &ctx->session.requests);
	return req;
}

static int kpass_test_stream_capture(struct kunit *test, struct sk_buff *skb)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	read_descriptor_t desc = { .count = skb->len, .arg.data = &ctx->sock };

	return kpass_stream_actor(&desc, skb, 0, skb->len);
}

static void kpass_stream_assembly_test(struct kunit *test)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	struct sk_buff *first = kpass_test_page_skb(test, 0, 64, 8);
	struct sk_buff *second = kpass_test_page_skb(test, 0, 96, 8);
	struct kpass_request *keep, *header;
	struct kpass_extent *extent;
	struct page *page;
	u64 handle;

	KUNIT_ASSERT_NOT_NULL(test, first);
	KUNIT_ASSERT_NOT_NULL(test, second);
	page = virt_to_page(first->data);
	KUNIT_ASSERT_EQ(test, kpass_test_stream_capture(test, first), 8);
	keep = kpass_test_request(test, KPASS_OP_KEEP, 2, 10);
	header = kpass_test_request(test, KPASS_OP_READ_STREAM, 12, 4);
	KUNIT_ASSERT_NOT_NULL(test, keep);
	KUNIT_ASSERT_NOT_NULL(test, header);
	kpass_stream_process(&ctx->session);
	KUNIT_EXPECT_FALSE(test, keep->ready);
	KUNIT_EXPECT_FALSE(test, header->ready);
	KUNIT_EXPECT_EQ(test, keep->object->length, 6ULL);
	KUNIT_EXPECT_EQ(test, ctx->session.object_count, 0U);
	extent = list_first_entry(&keep->object->extents,
				  struct kpass_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, extent->backing->page, page);
	KUNIT_EXPECT_EQ(test, extent->offset, 66U);
	KUNIT_EXPECT_EQ(test, extent->length, 6U);
	kunit_release_action(test, kunit_action_kfree_skb, first);
	KUNIT_ASSERT_EQ(test, kpass_test_stream_capture(test, second), 8);
	kpass_stream_process(&ctx->session);
	KUNIT_EXPECT_TRUE(test, keep->ready);
	KUNIT_EXPECT_TRUE(test, header->ready);
	KUNIT_EXPECT_EQ(test, keep->result, 10);
	KUNIT_EXPECT_EQ(test, header->result, 4);
	KUNIT_EXPECT_EQ(test, keep->object->length, 10ULL);
	KUNIT_EXPECT_EQ(test, ctx->session.copied_bytes, 0ULL);
	KUNIT_EXPECT_EQ(test, ctx->session.object_count, 0U);
	handle = kpass_object_publish(&ctx->session, keep->object);
	KUNIT_ASSERT_NE(test, handle, 0ULL);
	KUNIT_EXPECT_PTR_EQ(test, kpass_object_lookup(&ctx->session, handle),
			    keep->object);
	keep->object = NULL;
}

static void kpass_stream_discard_test(struct kunit *test)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	struct sk_buff *skb = kpass_test_page_skb(test, 0, 64, 16);
	struct kpass_request *discard, *keep;
	struct kpass_extent *left, *right;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_EQ(test, kpass_test_stream_capture(test, skb), 16);
	discard = kpass_test_request(test, KPASS_OP_DISCARD, 4, 8);
	KUNIT_ASSERT_NOT_NULL(test, discard);
	kpass_stream_process(&ctx->session);
	KUNIT_EXPECT_EQ(test, discard->result, 8);
	KUNIT_ASSERT_EQ(test, ctx->session.extent_count, 2U);
	left = list_first_entry(&ctx->sock.stream, struct kpass_extent, list);
	right = list_last_entry(&ctx->sock.stream, struct kpass_extent, list);
	KUNIT_EXPECT_PTR_EQ(test, left->backing, right->backing);
	KUNIT_EXPECT_EQ(test, refcount_read(&left->backing->refs), 2);
	KUNIT_EXPECT_EQ(test, ctx->session.backing_bytes, (u64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, kpass_stream_available(&ctx->sock, 0, 16), -ENODATA);
	KUNIT_EXPECT_EQ(test, kpass_stream_available(&ctx->sock, 12, 4), 1);
	keep = kpass_test_request(test, KPASS_OP_KEEP, 12, 4);
	KUNIT_ASSERT_NOT_NULL(test, keep);
	kpass_stream_process(&ctx->session);
	KUNIT_EXPECT_EQ(test, keep->result, 4);
	kpass_extents_free(&ctx->session, &ctx->sock.stream);
	KUNIT_EXPECT_EQ(test, ctx->session.backing_bytes, (u64)PAGE_SIZE);
	kpass_object_put(&ctx->session, keep->object);
	keep->object = NULL;
	KUNIT_EXPECT_EQ(test, ctx->session.backing_bytes, 0ULL);
	KUNIT_EXPECT_EQ(test, ctx->session.extent_count, 0U);
}

static void kpass_stream_budget_test(struct kunit *test)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	struct sk_buff *skb = kpass_test_page_skb(test, 0, 64, 8);
	struct kpass_request *discard;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	ctx->session.backing_limit = PAGE_SIZE;
	KUNIT_ASSERT_EQ(test, kpass_test_stream_capture(test, skb), 8);
	KUNIT_ASSERT_EQ(test, kpass_test_stream_capture(test, skb), -ENOBUFS);
	KUNIT_EXPECT_EQ(test, ctx->sock.rx_next, 8ULL);
	discard = kpass_test_request(test, KPASS_OP_DISCARD, 8, 8);
	KUNIT_ASSERT_NOT_NULL(test, discard);
	KUNIT_ASSERT_EQ(test, kpass_test_stream_capture(test, skb), 8);
	kpass_stream_process(&ctx->session);
	KUNIT_EXPECT_EQ(test, discard->result, 8);
	KUNIT_EXPECT_EQ(test, ctx->sock.rx_next, 16ULL);
	KUNIT_EXPECT_EQ(test, ctx->session.backing_bytes, (u64)PAGE_SIZE);
	kpass_extents_free(&ctx->session, &ctx->sock.stream);
	KUNIT_ASSERT_EQ(test, kpass_test_stream_capture(test, skb), 8);
	KUNIT_EXPECT_EQ(test, ctx->sock.rx_next, 24ULL);
}

static void kpass_stream_eof_test(struct kunit *test)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	struct sk_buff *skb = kpass_test_slab_skb(test, 8);
	struct kpass_request *keep;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_EQ(test, kpass_test_stream_capture(test, skb), 8);
	keep = kpass_test_request(test, KPASS_OP_KEEP, 0, 16);
	KUNIT_ASSERT_NOT_NULL(test, keep);
	kpass_stream_process(&ctx->session);
	KUNIT_EXPECT_FALSE(test, keep->ready);
	ctx->sock.rx_eof = true;
	kpass_stream_process(&ctx->session);
	KUNIT_EXPECT_TRUE(test, keep->ready);
	KUNIT_EXPECT_EQ(test, keep->result, -ENODATA);
	KUNIT_EXPECT_EQ(test, ctx->session.object_count, 0U);
	KUNIT_EXPECT_EQ(test, ctx->session.copied_bytes, 8ULL);
	kpass_object_put(&ctx->session, keep->object);
	keep->object = NULL;
	KUNIT_EXPECT_EQ(test, ctx->session.backing_bytes, 0ULL);
}

static void kpass_object_generation_test(struct kunit *test)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	struct kpass_object *object = kpass_object_alloc();
	u64 first, second;

	KUNIT_ASSERT_NOT_NULL(test, object);
	first = kpass_object_publish(&ctx->session, object);
	KUNIT_ASSERT_NE(test, first, 0ULL);
	ctx->session.objects[0] = NULL;
	ctx->session.object_count--;
	second = kpass_object_publish(&ctx->session, object);
	KUNIT_EXPECT_NE(test, first, second);
	KUNIT_EXPECT_EQ(test, (u32)first, (u32)second);
	KUNIT_EXPECT_PTR_EQ(test, kpass_object_lookup(&ctx->session, first), NULL);
	KUNIT_EXPECT_PTR_EQ(test, kpass_object_lookup(&ctx->session, second), object);
	ctx->session.objects[0] = NULL;
	ctx->session.object_count--;
	ctx->session.generations[0] = U32_MAX;
	second = kpass_object_publish(&ctx->session, object);
	KUNIT_EXPECT_EQ(test, (u32)second, 2U);
	KUNIT_EXPECT_EQ(test, ctx->session.generations[0], U32_MAX);
}

static void kpass_stream_callback_restore_test(struct kunit *test)
{
	struct kpass_stream_test_ctx *ctx = test->priv;
	struct socket *original = kpass_test_socket(test);
	struct socket *accepted = kpass_test_socket(test);
	void (*data_ready)(struct sock *sk);
	void (*write_space)(struct sock *sk);
	void (*state_change)(struct sock *sk);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, original);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, accepted);
	data_ready = original->sk->sk_data_ready;
	write_space = original->sk->sk_write_space;
	state_change = original->sk->sk_state_change;
	ctx->sock.sock = original;
	kpass_sock_hook(&ctx->sock);
	KUNIT_EXPECT_TRUE(test, sk_user_data_is_nocopy(original->sk));
	kpass_sock_unhook(&ctx->sock);

	/* A TCP child inherits its listener's callbacks before kernel_accept. */
	accepted->sk->sk_data_ready = kpass_sk_data_ready;
	accepted->sk->sk_write_space = kpass_sk_write_space;
	accepted->sk->sk_state_change = kpass_sk_state_change;
	ctx->sock.sock = accepted;
	kpass_sock_hook(&ctx->sock);
	kpass_sock_unhook(&ctx->sock);
	KUNIT_EXPECT_PTR_EQ(test, accepted->sk->sk_user_data, NULL);
	KUNIT_EXPECT_PTR_EQ(test, accepted->sk->sk_data_ready, data_ready);
	KUNIT_EXPECT_PTR_EQ(test, accepted->sk->sk_write_space, write_space);
	KUNIT_EXPECT_PTR_EQ(test, accepted->sk->sk_state_change, state_change);
}

static struct kunit_case kpass_stream_test_cases[] = {
	KUNIT_CASE(kpass_stream_assembly_test),
	KUNIT_CASE(kpass_stream_discard_test),
	KUNIT_CASE(kpass_stream_budget_test),
	KUNIT_CASE(kpass_stream_eof_test),
	KUNIT_CASE(kpass_object_generation_test),
	KUNIT_CASE(kpass_stream_callback_restore_test),
	{}
};

static struct kunit_suite kpass_stream_test_suite = {
	.name = "kpass-stream",
	.init = kpass_stream_test_init,
	.test_cases = kpass_stream_test_cases,
};

kunit_test_suite(kpass_stream_test_suite);
