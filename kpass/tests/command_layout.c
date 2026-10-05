// SPDX-License-Identifier: GPL-2.0
/* Exercise the library's command preparation without a running module. */
#include "../lib/ceph_kpass.c"
#include <assert.h>

int main(void)
{
	struct {
		struct io_uring_sqe entries[4];
		unsigned char guard[16];
	} storage;
	struct ceph_kpass_ctx ctx = { .fd = 37 };
	struct io_uring_sqe *first, *second;
	struct kpass_sqe_cmd *cmd;
	unsigned char untouched[128];
	unsigned int head = 0;
	size_t i;

	memset(&storage, 0xa5, sizeof(storage));
	memset(untouched, 0xa5, sizeof(untouched));
	ctx.ring.flags = IORING_SETUP_SQE128;
	ctx.ring.sq.sqes = storage.entries;
	ctx.ring.sq.khead = &head;
	ctx.ring.sq.ring_entries = 2;
	ctx.ring.sq.ring_mask = 1;

	first = io_uring_get_sqe(&ctx.ring);
	assert(first == &storage.entries[0]);
	cmd = prep_kpass_cmd(&ctx, first, KPASS_OP_INIT, 3, 17,
			     0x12345678, UDATA_OP_INIT);
	assert(cmd);
	assert(first->opcode == IORING_OP_URING_CMD);
	assert(first->fd == ctx.fd);
	assert(first->cmd_op == 0);
	assert(cmd->op == KPASS_OP_INIT);
	assert(cmd->sock_id == 3);
	assert(cmd->buf_id == 17);
	assert(cmd->tag == 0x12345678);
	assert(udata_op(first->user_data) == UDATA_OP_INIT);
	assert(udata_tag(first->user_data) == 0x12345678);
	cmd->init.num_buffers = 64;
	cmd->init.buf_size = 128 * 1024;
	assert(!memcmp(&storage.entries[2], untouched, sizeof(untouched)));

	second = io_uring_get_sqe(&ctx.ring);
	assert(second == &storage.entries[2]);
	cmd = prep_kpass_cmd(&ctx, second, KPASS_OP_SEND, 4, 18,
			     0x87654321, UDATA_OP_SEND);
	assert(cmd);
	memset(cmd->_pad, 0x5a, sizeof(cmd->_pad));
	cmd = (struct kpass_sqe_cmd *)first->cmd;
	assert(cmd->init.num_buffers == 64);
	assert(cmd->init.buf_size == 128 * 1024);
	for (i = 0; i < sizeof(storage.guard); i++)
		assert(storage.guard[i] == 0xa5);
	assert(!io_uring_get_sqe(&ctx.ring));

	/* Reject a standard SQE before writing past its 16-byte cmd area. */
	ctx.ring.flags = 0;
	memset(first, 0xa5, sizeof(*first));
	errno = 0;
	assert(!prep_kpass_cmd(&ctx, first, KPASS_OP_INIT, 0, 0, 0,
			       UDATA_OP_INIT));
	assert(errno == EINVAL);
	assert(!memcmp(first, untouched, sizeof(*first)));

	puts("PASS: SQE128 command encoding, fd, entry isolation, SQE64 rejection");
	return 0;
}
