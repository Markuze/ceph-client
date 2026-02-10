/*
 * Ceph Zero-Overhead Kernel Passthrough (kpass) - Userspace Library
 *
 * This library communicates with the ceph_kpass kernel module.
 * Uses ioctl for control operations, io_uring for async I/O.
 *
 * Zero-Overhead Architecture:
 * - Buffer pool stays in kernel (no mmap, no userspace access)
 * - Userspace operates on handles (buffer IDs, socket IDs) only
 * - Optional peek API for explicit data copy when needed
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <liburing.h>

#include "ceph_kpass.h"
#include "../include/uapi/ceph_kpass.h"

/*
 * Buffer tracking (state only, no data)
 */
enum buf_state {
	BUF_FREE = 0,
	BUF_ALLOCATED,
	BUF_IO_PENDING,
};

struct buf_desc {
	kpass_buf_id id;
	enum buf_state state;
	struct buf_desc *next;
};

/*
 * Socket tracking
 */
struct sock_desc {
	kpass_sock_id id;
	int fd;
	bool connected;
	bool listening;
};

struct pending_cqe {
	uint64_t user_data;
	int res;
};

/*
 * User data encoding for io_uring
 */
#define UDATA_OP_SHIFT      56
#define UDATA_SOCK_SHIFT    48
#define UDATA_BUF_SHIFT     32
#define UDATA_TAG_MASK      0xFFFFFFFFULL

#define UDATA_OP_RECV         1
#define UDATA_OP_SEND         2
#define UDATA_OP_CONNECT      3
#define UDATA_OP_ACCEPT       4
#define UDATA_OP_SOCK_CREATE  5
#define UDATA_OP_SOCK_LISTEN  6
#define UDATA_OP_SOCK_CLOSE   7
#define UDATA_OP_BUF_ALLOC    8
#define UDATA_OP_BUF_FREE     9
#define UDATA_OP_INIT         10

static inline uint64_t make_udata(uint8_t op, kpass_sock_id sock, kpass_buf_id buf, uint64_t tag)
{
	return ((uint64_t)op << UDATA_OP_SHIFT) |
	       ((uint64_t)sock << UDATA_SOCK_SHIFT) |
	       ((uint64_t)(buf & 0xFFFF) << UDATA_BUF_SHIFT) |
	       (tag & UDATA_TAG_MASK);
}

static inline uint8_t udata_op(uint64_t udata) { return (udata >> UDATA_OP_SHIFT) & 0xFF; }
static inline kpass_sock_id udata_sock(uint64_t udata) { return (udata >> UDATA_SOCK_SHIFT) & 0xFF; }
static inline kpass_buf_id udata_buf(uint64_t udata) { return (udata >> UDATA_BUF_SHIFT) & 0xFFFF; }
static inline uint64_t udata_tag(uint64_t udata) { return udata & UDATA_TAG_MASK; }

/*
 * Context - Zero-Overhead: NO buffer pool in userspace
 */
struct ceph_kpass_ctx {
	int fd;                    /* /dev/ceph_kpass */
	struct io_uring ring;

	/*
	 * Buffer handle tracking (NO data, just state)
	 * Actual buffer data is in kernel only
	 */
	uint32_t num_bufs;
	uint32_t buf_size;
	struct buf_desc *buf_descs;
	struct buf_desc *free_list;

	/* Auto-peek configuration */
	uint32_t auto_peek_size;

	/* Sockets */
	struct sock_desc sockets[CEPH_KPASS_MAX_SOCKETS];
	uint64_t sock_bitmap[(CEPH_KPASS_MAX_SOCKETS + 63) / 64];

	/* Pending accept storage */
	struct sockaddr_storage accept_addr;
	socklen_t accept_addr_len;

	/* Deferred CQEs captured during sync waits */
	struct pending_cqe *deferred;
	size_t deferred_count;
	size_t deferred_cap;

	/* Sequence for sync commands */
	uint64_t sync_seq;

	int pending;
	bool use_kernel_module;
};

/*
 * Bitmap helpers
 */
static inline bool bitmap_test(uint64_t *bm, int bit) { return bm[bit/64] & (1ULL << (bit%64)); }
static inline void bitmap_set(uint64_t *bm, int bit) { bm[bit/64] |= (1ULL << (bit%64)); }
static inline void bitmap_clear(uint64_t *bm, int bit) { bm[bit/64] &= ~(1ULL << (bit%64)); }

static kpass_sock_id alloc_sock_id(struct ceph_kpass_ctx *ctx)
{
	for (int i = 0; i < CEPH_KPASS_MAX_SOCKETS; i++) {
		if (!bitmap_test(ctx->sock_bitmap, i)) {
			bitmap_set(ctx->sock_bitmap, i);
			return i;
		}
	}
	return KPASS_SOCK_INVALID;
}

static void free_sock_id(struct ceph_kpass_ctx *ctx, kpass_sock_id id)
{
	if (id < CEPH_KPASS_MAX_SOCKETS)
		bitmap_clear(ctx->sock_bitmap, id);
}

static struct sock_desc *get_sock(struct ceph_kpass_ctx *ctx, kpass_sock_id id)
{
	if (id >= CEPH_KPASS_MAX_SOCKETS || !bitmap_test(ctx->sock_bitmap, id))
		return NULL;
	return &ctx->sockets[id];
}

static int defer_cqe(struct ceph_kpass_ctx *ctx, uint64_t user_data, int res)
{
	if (ctx->deferred_count == ctx->deferred_cap) {
		size_t new_cap = ctx->deferred_cap ? ctx->deferred_cap * 2 : 16;
		struct pending_cqe *next = realloc(ctx->deferred, new_cap * sizeof(*next));
		if (!next)
			return -1;
		ctx->deferred = next;
		ctx->deferred_cap = new_cap;
	}

	ctx->deferred[ctx->deferred_count].user_data = user_data;
	ctx->deferred[ctx->deferred_count].res = res;
	ctx->deferred_count++;
	return 0;
}

#ifdef IORING_OP_URING_CMD
static struct kpass_sqe_cmd *prep_kpass_cmd(struct ceph_kpass_ctx *ctx,
					    struct io_uring_sqe *sqe,
					    uint8_t op, kpass_sock_id sock,
					    kpass_buf_id buf, uint64_t tag,
					    uint8_t udata_op)
{
	struct kpass_sqe_cmd *cmd = (struct kpass_sqe_cmd *)sqe->cmd;

	memset(cmd, 0, sizeof(*cmd));
	cmd->op = op;
	cmd->sock_id = sock;
	cmd->buf_id = buf;
	cmd->tag = tag;

	io_uring_prep_uring_cmd(sqe, ctx->fd, 0);
	sqe->user_data = make_udata(udata_op, sock, buf, tag);

	return cmd;
}
#else
static struct kpass_sqe_cmd *prep_kpass_cmd(struct ceph_kpass_ctx *ctx,
					    struct io_uring_sqe *sqe,
					    uint8_t op, kpass_sock_id sock,
					    kpass_buf_id buf, uint64_t tag,
					    uint8_t udata_op)
{
	(void)ctx;
	(void)sqe;
	(void)op;
	(void)sock;
	(void)buf;
	(void)tag;
	(void)udata_op;
	errno = ENOSYS;
	return NULL;
}
#endif

static int wait_for_sync_cmd(struct ceph_kpass_ctx *ctx, uint8_t udata_opcode,
			     uint64_t tag, int *res_out)
{
	struct io_uring_cqe *cqe;

	for (;;) {
		int ret = io_uring_wait_cqe(&ctx->ring, &cqe);
		if (ret < 0) {
			errno = -ret;
			return -1;
		}

		uint64_t udata = cqe->user_data;
		int res = cqe->res;
		io_uring_cqe_seen(&ctx->ring, cqe);

		if (udata_op(udata) == udata_opcode && udata_tag(udata) == tag) {
			*res_out = res;
			return 0;
		}

		if (defer_cqe(ctx, udata, res) < 0) {
			errno = ENOMEM;
			return -1;
		}
	}
}

/*
 * ============================================================================
 * Lifecycle
 * ============================================================================
 */

struct ceph_kpass_ctx *ceph_kpass_create(uint32_t num_buffers, uint32_t ring_depth)
{
	struct ceph_kpass_ctx *ctx;
	struct kpass_info info;
	int ret;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->fd = -1;
	ctx->use_kernel_module = false;

	/* Open kernel device */
	ctx->fd = open(CEPH_KPASS_DEVICE, O_RDWR | O_CLOEXEC);
	if (ctx->fd >= 0) {
		ctx->use_kernel_module = true;

		/* Get device info */
		if (ioctl(ctx->fd, KPASS_IOC_GET_INFO, &info) < 0) {
			close(ctx->fd);
			free(ctx);
			return NULL;
		}

		/* Set buffer parameters */
		if (num_buffers == 0)
			num_buffers = 64;
		if (num_buffers > info.max_buffers)
			num_buffers = info.max_buffers;

		ctx->num_bufs = num_buffers;
		ctx->buf_size = info.default_buf_size;
	} else {
		/* Fallback: no kernel module, use userspace sockets */
		ctx->use_kernel_module = false;
		ctx->fd = -1;

		if (num_buffers == 0)
			num_buffers = 64;
		if (num_buffers > CEPH_KPASS_MAX_BUFFERS)
			num_buffers = CEPH_KPASS_MAX_BUFFERS;

		ctx->num_bufs = num_buffers;
		ctx->buf_size = CEPH_KPASS_BUF_SIZE;
	}

	/*
	 * ZERO-OVERHEAD: NO buffer pool allocation in userspace
	 * We only track buffer HANDLES, not data
	 */
	ctx->buf_descs = calloc(ctx->num_bufs, sizeof(struct buf_desc));
	if (!ctx->buf_descs) {
		if (ctx->fd >= 0)
			close(ctx->fd);
		free(ctx);
		return NULL;
	}

	/* Initialize free list (handle tracking only) */
	ctx->free_list = NULL;
	for (int i = ctx->num_bufs - 1; i >= 0; i--) {
		ctx->buf_descs[i].id = i;
		ctx->buf_descs[i].state = BUF_FREE;
		ctx->buf_descs[i].next = ctx->free_list;
		ctx->free_list = &ctx->buf_descs[i];
	}

	/* Initialize sockets */
	for (int i = 0; i < CEPH_KPASS_MAX_SOCKETS; i++) {
		ctx->sockets[i].id = i;
		ctx->sockets[i].fd = -1;
	}

	/* Initialize io_uring */
	if (ring_depth == 0)
		ring_depth = 128;

	ret = io_uring_queue_init(ring_depth, &ctx->ring, 0);
	if (ret < 0) {
		free(ctx->buf_descs);
		if (ctx->fd >= 0)
			close(ctx->fd);
		free(ctx);
		errno = -ret;
		return NULL;
	}

	if (ctx->use_kernel_module) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
		if (!sqe) {
			io_uring_queue_exit(&ctx->ring);
			free(ctx->buf_descs);
			if (ctx->fd >= 0)
				close(ctx->fd);
			free(ctx);
			errno = EAGAIN;
			return NULL;
		}

		uint64_t tag = ++ctx->sync_seq;
		struct kpass_sqe_cmd *cmd = prep_kpass_cmd(ctx, sqe, KPASS_OP_INIT,
							   0, 0, tag, UDATA_OP_INIT);
		if (!cmd) {
			io_uring_queue_exit(&ctx->ring);
			free(ctx->buf_descs);
			if (ctx->fd >= 0)
				close(ctx->fd);
			free(ctx);
			return NULL;
		}
		cmd->init.num_buffers = ctx->num_bufs;
		cmd->init.buf_size = ctx->buf_size;

		ret = io_uring_submit(&ctx->ring);
		if (ret < 0) {
			io_uring_queue_exit(&ctx->ring);
			free(ctx->buf_descs);
			if (ctx->fd >= 0)
				close(ctx->fd);
			free(ctx);
			errno = -ret;
			return NULL;
		}
		ctx->pending = 0;

		int res = 0;
		if (wait_for_sync_cmd(ctx, UDATA_OP_INIT, tag, &res) < 0 || res < 0) {
			io_uring_queue_exit(&ctx->ring);
			free(ctx->buf_descs);
			if (ctx->fd >= 0)
				close(ctx->fd);
			free(ctx);
			if (res < 0)
				errno = -res;
			return NULL;
		}
	}

	return ctx;
}

void ceph_kpass_destroy(struct ceph_kpass_ctx *ctx)
{
	if (!ctx)
		return;

	for (int i = 0; i < CEPH_KPASS_MAX_SOCKETS; i++) {
		if (bitmap_test(ctx->sock_bitmap, i) && ctx->sockets[i].fd >= 0)
			close(ctx->sockets[i].fd);
	}

	io_uring_queue_exit(&ctx->ring);

	if (ctx->buf_descs)
		free(ctx->buf_descs);
	if (ctx->deferred)
		free(ctx->deferred);
	if (ctx->fd >= 0)
		close(ctx->fd);

	free(ctx);
}

int ceph_kpass_fd(struct ceph_kpass_ctx *ctx)
{
	return ctx->ring.ring_fd;
}

/*
 * ============================================================================
 * Socket Management
 * ============================================================================
 */

kpass_sock_id ceph_kpass_socket_create(struct ceph_kpass_ctx *ctx)
{
	if (ctx->use_kernel_module && ctx->fd >= 0) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
		if (!sqe) {
			errno = EAGAIN;
			return KPASS_SOCK_INVALID;
		}

		uint64_t tag = ++ctx->sync_seq;
		if (!prep_kpass_cmd(ctx, sqe, KPASS_OP_SOCK_CREATE, 0, 0, tag, UDATA_OP_SOCK_CREATE))
			return KPASS_SOCK_INVALID;

		int ret = io_uring_submit(&ctx->ring);
		if (ret < 0) {
			errno = -ret;
			return KPASS_SOCK_INVALID;
		}
		ctx->pending = 0;

		int res = 0;
		if (wait_for_sync_cmd(ctx, UDATA_OP_SOCK_CREATE, tag, &res) < 0 || res < 0) {
			if (res < 0)
				errno = -res;
			return KPASS_SOCK_INVALID;
		}

		kpass_sock_id id = (kpass_sock_id)res;
		if (id >= CEPH_KPASS_MAX_SOCKETS || bitmap_test(ctx->sock_bitmap, id)) {
			errno = EINVAL;
			return KPASS_SOCK_INVALID;
		}

		bitmap_set(ctx->sock_bitmap, id);
		ctx->sockets[id].fd = -1;
		ctx->sockets[id].connected = false;
		ctx->sockets[id].listening = false;
		return id;
	}

	kpass_sock_id id = alloc_sock_id(ctx);
	if (id == KPASS_SOCK_INVALID) {
		errno = EMFILE;
		return KPASS_SOCK_INVALID;
	}

	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		free_sock_id(ctx, id);
		return KPASS_SOCK_INVALID;
	}

	int opt = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	ctx->sockets[id].fd = fd;
	ctx->sockets[id].connected = false;
	ctx->sockets[id].listening = false;

	return id;
}

int ceph_kpass_socket_listen(struct ceph_kpass_ctx *ctx, kpass_sock_id sock,
			    uint16_t port, int backlog)
{
	struct sock_desc *sd = get_sock(ctx, sock);
	if (!sd) {
		errno = EBADF;
		return -1;
	}

	if (ctx->use_kernel_module && ctx->fd >= 0) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
		if (!sqe) {
			errno = EAGAIN;
			return -1;
		}

		uint64_t tag = ++ctx->sync_seq;
		struct kpass_sqe_cmd *cmd = prep_kpass_cmd(ctx, sqe, KPASS_OP_SOCK_LISTEN,
							   sock, 0, tag, UDATA_OP_SOCK_LISTEN);
		if (!cmd)
			return -1;
		cmd->listen.family = AF_INET;
		cmd->listen.port = port;
		cmd->listen.backlog = backlog;

		int ret = io_uring_submit(&ctx->ring);
		if (ret < 0) {
			errno = -ret;
			return -1;
		}
		ctx->pending = 0;

		int res = 0;
		if (wait_for_sync_cmd(ctx, UDATA_OP_SOCK_LISTEN, tag, &res) < 0 || res < 0) {
			if (res < 0)
				errno = -res;
			return -1;
		}

		sd->listening = true;
		return 0;
	}

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = INADDR_ANY,
	};

	if (bind(sd->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		return -1;

	if (listen(sd->fd, backlog) < 0)
		return -1;

	sd->listening = true;
	return 0;
}

int ceph_kpass_socket_accept(struct ceph_kpass_ctx *ctx, kpass_sock_id sock, uint64_t tag)
{
	struct sock_desc *sd = get_sock(ctx, sock);
	if (!sd || !sd->listening) {
		errno = EINVAL;
		return -1;
	}

	if (ctx->use_kernel_module && ctx->fd >= 0) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
		if (!sqe) {
			errno = EAGAIN;
			return -1;
		}

		if (!prep_kpass_cmd(ctx, sqe, KPASS_OP_SOCK_ACCEPT, sock, 0, tag, UDATA_OP_ACCEPT))
			return -1;
		ctx->pending++;
		return 0;
	}

	struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
	if (!sqe) {
		errno = EAGAIN;
		return -1;
	}

	ctx->accept_addr_len = sizeof(ctx->accept_addr);
	io_uring_prep_accept(sqe, sd->fd,
			     (struct sockaddr *)&ctx->accept_addr,
			     &ctx->accept_addr_len,
			     SOCK_NONBLOCK | SOCK_CLOEXEC);
	sqe->user_data = make_udata(UDATA_OP_ACCEPT, sock, KPASS_BUF_INVALID, tag);

	ctx->pending++;
	return 0;
}

int ceph_kpass_socket_connect(struct ceph_kpass_ctx *ctx, kpass_sock_id sock,
			     const char *host, uint16_t port, uint64_t tag)
{
	struct sock_desc *sd = get_sock(ctx, sock);
	if (!sd) {
		errno = EBADF;
		return -1;
	}

	if (ctx->use_kernel_module && ctx->fd >= 0) {
		struct sockaddr_in addr = {
			.sin_family = AF_INET,
			.sin_port = htons(port),
		};

		if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
			errno = EINVAL;
			return -1;
		}

		struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
		if (!sqe) {
			errno = EAGAIN;
			return -1;
		}

		struct kpass_sqe_cmd *cmd = prep_kpass_cmd(ctx, sqe, KPASS_OP_SOCK_CONNECT,
							   sock, 0, tag, UDATA_OP_CONNECT);
		if (!cmd)
			return -1;
		cmd->connect.family = AF_INET;
		cmd->connect.port = port;
		memset(cmd->connect.addr, 0, sizeof(cmd->connect.addr));
		memcpy(cmd->connect.addr, &addr.sin_addr.s_addr, 4);

		ctx->pending++;
		return 0;
	}

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
	};

	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		errno = EINVAL;
		return -1;
	}

	struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
	if (!sqe) {
		errno = EAGAIN;
		return -1;
	}

	io_uring_prep_connect(sqe, sd->fd, (struct sockaddr *)&addr, sizeof(addr));
	sqe->user_data = make_udata(UDATA_OP_CONNECT, sock, KPASS_BUF_INVALID, tag);

	ctx->pending++;
	return 0;
}

void ceph_kpass_socket_close(struct ceph_kpass_ctx *ctx, kpass_sock_id sock)
{
	struct sock_desc *sd = get_sock(ctx, sock);
	if (!sd)
		return;

	if (ctx->use_kernel_module && ctx->fd >= 0) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
		if (!sqe)
			return;

		uint64_t tag = ++ctx->sync_seq;
		if (!prep_kpass_cmd(ctx, sqe, KPASS_OP_SOCK_CLOSE, sock, 0, tag, UDATA_OP_SOCK_CLOSE))
			return;

		int ret = io_uring_submit(&ctx->ring);
		if (ret < 0)
			return;
		ctx->pending = 0;

		int res = 0;
		if (wait_for_sync_cmd(ctx, UDATA_OP_SOCK_CLOSE, tag, &res) < 0)
			return;

		sd->fd = -1;
		sd->connected = false;
		sd->listening = false;
		free_sock_id(ctx, sock);
		return;
	}

	if (sd->fd >= 0) {
		close(sd->fd);
		sd->fd = -1;
	}
	sd->connected = false;
	sd->listening = false;
	free_sock_id(ctx, sock);
}

/*
 * ============================================================================
 * Buffer Management - Handle-based, NO data access
 * ============================================================================
 */

kpass_buf_id ceph_kpass_buf_alloc(struct ceph_kpass_ctx *ctx)
{
	if (ctx->use_kernel_module && ctx->fd >= 0) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
		if (!sqe) {
			errno = EAGAIN;
			return KPASS_BUF_INVALID;
		}

		uint64_t tag = ++ctx->sync_seq;
		if (!prep_kpass_cmd(ctx, sqe, KPASS_OP_BUF_ALLOC, 0, 0, tag, UDATA_OP_BUF_ALLOC))
			return KPASS_BUF_INVALID;

		int ret = io_uring_submit(&ctx->ring);
		if (ret < 0) {
			errno = -ret;
			return KPASS_BUF_INVALID;
		}
		ctx->pending = 0;

		int res = 0;
		if (wait_for_sync_cmd(ctx, UDATA_OP_BUF_ALLOC, tag, &res) < 0 || res < 0) {
			if (res < 0)
				errno = -res;
			return KPASS_BUF_INVALID;
		}

		if ((uint32_t)res < ctx->num_bufs)
			ctx->buf_descs[res].state = BUF_ALLOCATED;

		return (kpass_buf_id)res;
	}

	/* Fallback: local handle tracking */
	struct buf_desc *buf = ctx->free_list;
	if (!buf)
		return KPASS_BUF_INVALID;

	ctx->free_list = buf->next;
	buf->next = NULL;
	buf->state = BUF_ALLOCATED;

	return buf->id;
}

void ceph_kpass_buf_free(struct ceph_kpass_ctx *ctx, kpass_buf_id id)
{
	if (id >= ctx->num_bufs)
		return;

	if (ctx->use_kernel_module && ctx->fd >= 0) {
		struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
		if (!sqe)
			return;

		uint64_t tag = ++ctx->sync_seq;
		if (!prep_kpass_cmd(ctx, sqe, KPASS_OP_BUF_FREE, 0, id, tag, UDATA_OP_BUF_FREE))
			return;

		int ret = io_uring_submit(&ctx->ring);
		if (ret < 0)
			return;
		ctx->pending = 0;

		int res = 0;
		if (wait_for_sync_cmd(ctx, UDATA_OP_BUF_FREE, tag, &res) < 0)
			return;
	}

	/* Update local state */
	struct buf_desc *buf = &ctx->buf_descs[id];
	if (buf->state == BUF_FREE)
		return;

	buf->state = BUF_FREE;
	buf->next = ctx->free_list;
	ctx->free_list = buf;
}

/*
 * REMOVED: ceph_kpass_buf_ptr()
 *
 * Zero-Overhead Architecture: No userspace data access.
 * Use ceph_kpass_peek() for explicit data copy when needed.
 */

size_t ceph_kpass_buf_size(struct ceph_kpass_ctx *ctx)
{
	return ctx->buf_size;
}

uint32_t ceph_kpass_buf_count(struct ceph_kpass_ctx *ctx)
{
	return ctx->num_bufs;
}

/*
 * ============================================================================
 * Peek API - Explicit data copy from kernel
 * ============================================================================
 */

int ceph_kpass_peek(struct ceph_kpass_ctx *ctx, kpass_buf_id buf,
		   uint32_t offset, uint32_t length, void *dest)
{
	if (!ctx->use_kernel_module || ctx->fd < 0) {
		/* No kernel module - peek not available in fallback mode */
		errno = ENOSYS;
		return -1;
	}

	struct kpass_peek_args args = {
		.buf_id = buf,
		.offset = offset,
		.length = length,
		.bytes_copied = 0,
		.dest_addr = (uint64_t)(uintptr_t)dest,
	};

	if (ioctl(ctx->fd, KPASS_IOC_PEEK, &args) < 0)
		return -1;

	return args.bytes_copied;
}

int ceph_kpass_poke(struct ceph_kpass_ctx *ctx, kpass_buf_id buf,
		   uint32_t offset, uint32_t length, const void *src)
{
	if (!ctx->use_kernel_module || ctx->fd < 0) {
		/* No kernel module - poke not available in fallback mode */
		errno = ENOSYS;
		return -1;
	}

	struct kpass_peek_args args = {
		.buf_id = buf,
		.offset = offset,
		.length = length,
		.bytes_copied = 0,
		.dest_addr = (uint64_t)(uintptr_t)src,
	};

	if (ioctl(ctx->fd, KPASS_IOC_POKE, &args) < 0)
		return -1;

	return args.bytes_copied;
}

int ceph_kpass_set_auto_peek(struct ceph_kpass_ctx *ctx, uint32_t size)
{
	if (size > KPASS_AUTO_PEEK_MAX)
		size = KPASS_AUTO_PEEK_MAX;

	ctx->auto_peek_size = size;

	if (ctx->use_kernel_module && ctx->fd >= 0) {
		struct kpass_auto_peek_args args = {
			.size = size,
			._reserved = 0,
		};
		if (ioctl(ctx->fd, KPASS_IOC_SET_AUTO_PEEK, &args) < 0)
			return -1;
	}

	return 0;
}

uint32_t ceph_kpass_get_auto_peek(struct ceph_kpass_ctx *ctx)
{
	return ctx->auto_peek_size;
}

/*
 * ============================================================================
 * Network I/O - Handle-based
 *
 * Kernel mode: io_uring_cmd posts handle-only ops to the kernel module.
 * Fallback mode: recv uses a temporary buffer; send is unsupported without kernel.
 * ============================================================================
 */

/* Temporary buffer for fallback mode (when no kernel module) */
static __thread char *fallback_buffer = NULL;
static __thread size_t fallback_buffer_size = 0;

static char *get_fallback_buffer(struct ceph_kpass_ctx *ctx)
{
	if (!fallback_buffer || fallback_buffer_size < ctx->buf_size) {
		free(fallback_buffer);
		fallback_buffer = malloc(ctx->buf_size);
		fallback_buffer_size = ctx->buf_size;
	}
	return fallback_buffer;
}

int ceph_kpass_send(struct ceph_kpass_ctx *ctx, kpass_sock_id sock,
		   kpass_buf_id buf, uint32_t offset, uint32_t len, uint64_t tag)
{
	struct sock_desc *sd = get_sock(ctx, sock);
	if (!sd || (!ctx->use_kernel_module && sd->fd < 0)) {
		errno = EBADF;
		return -1;
	}

	if (buf >= ctx->num_bufs || ctx->buf_descs[buf].state != BUF_ALLOCATED) {
		errno = EINVAL;
		return -1;
	}

	struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
	if (!sqe) {
		errno = EAGAIN;
		return -1;
	}

	if (ctx->use_kernel_module && ctx->fd >= 0) {
		struct kpass_sqe_cmd *cmd = prep_kpass_cmd(ctx, sqe, KPASS_OP_SEND,
							   sock, buf, tag, UDATA_OP_SEND);
		if (!cmd)
			return -1;
		cmd->send.offset = offset;
		cmd->send.len = len;
		cmd->send.flags = 0;
	} else {
		/* Fallback: use temporary buffer */
		char *data = get_fallback_buffer(ctx);
		if (!data) {
			errno = ENOMEM;
			return -1;
		}
		/* In fallback mode without kernel, we can't send - data not accessible */
		errno = ENOSYS;
		return -1;
	}

	ctx->buf_descs[buf].state = BUF_IO_PENDING;
	ctx->pending++;
	return 0;
}

int ceph_kpass_recv(struct ceph_kpass_ctx *ctx, kpass_sock_id sock,
		   kpass_buf_id buf, uint32_t offset, uint32_t max_len, uint64_t tag)
{
	struct sock_desc *sd = get_sock(ctx, sock);
	if (!sd || (!ctx->use_kernel_module && sd->fd < 0)) {
		errno = EBADF;
		return -1;
	}

	if (buf >= ctx->num_bufs || ctx->buf_descs[buf].state != BUF_ALLOCATED) {
		errno = EINVAL;
		return -1;
	}

	struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
	if (!sqe) {
		errno = EAGAIN;
		return -1;
	}

	if (ctx->use_kernel_module && ctx->fd >= 0) {
		struct kpass_sqe_cmd *cmd = prep_kpass_cmd(ctx, sqe, KPASS_OP_RECV,
							   sock, buf, tag, UDATA_OP_RECV);
		if (!cmd)
			return -1;
		cmd->recv.offset = offset;
		cmd->recv.len = max_len;
	} else {
		/* Fallback: recv into temporary buffer */
		char *data = get_fallback_buffer(ctx);
		if (!data) {
			errno = ENOMEM;
			return -1;
		}

		io_uring_prep_recv(sqe, sd->fd, data, max_len, 0);
		sqe->user_data = make_udata(UDATA_OP_RECV, sock, buf, tag);
	}

	ctx->buf_descs[buf].state = BUF_IO_PENDING;
	ctx->pending++;
	return 0;
}

int ceph_kpass_flush(struct ceph_kpass_ctx *ctx)
{
	if (ctx->pending == 0)
		return 0;

	int ret = io_uring_submit(&ctx->ring);
	if (ret < 0) {
		errno = -ret;
		return -1;
	}

	ctx->pending = 0;
	return ret;
}

static void fill_event_from_cqe(struct ceph_kpass_ctx *ctx, struct kpass_event *ev,
				uint64_t user_data, int res)
{
	uint8_t op = udata_op(user_data);
	kpass_sock_id sock = udata_sock(user_data);
	kpass_buf_id buf = udata_buf(user_data);

	memset(ev, 0, sizeof(*ev));
	ev->tag = udata_tag(user_data);
	ev->sock = sock;
	ev->buf = buf;

	switch (op) {
	case UDATA_OP_RECV:
		if (res < 0) {
			ev->type = KPASS_EV_ERROR;
			ev->err.error = -res;
		} else if (res == 0) {
			ev->type = KPASS_EV_CLOSED;
			ev->err.error = 0;
		} else {
			ev->type = KPASS_EV_RECV_DONE;
			ev->recvd.bytes = res;
			ev->recvd.peek_len = 0;

			if (ctx->auto_peek_size > 0) {
				uint32_t peek_len = ctx->auto_peek_size;
				if (peek_len > (uint32_t)res)
					peek_len = res;
				if (peek_len > KPASS_AUTO_PEEK_MAX)
					peek_len = KPASS_AUTO_PEEK_MAX;

				if (ctx->use_kernel_module && ctx->fd >= 0) {
					int copied = ceph_kpass_peek(ctx, buf, 0, peek_len, ev->recvd.peek);
					if (copied > 0)
						ev->recvd.peek_len = copied;
				} else if (fallback_buffer) {
					memcpy(ev->recvd.peek, fallback_buffer, peek_len);
					ev->recvd.peek_len = peek_len;
				}
			}
		}
		if (buf < ctx->num_bufs)
			ctx->buf_descs[buf].state = BUF_ALLOCATED;
		break;

	case UDATA_OP_SEND:
		if (res < 0) {
			ev->type = KPASS_EV_ERROR;
			ev->err.error = -res;
		} else {
			ev->type = KPASS_EV_SEND_DONE;
			ev->sent.bytes = res;
		}
		if (buf < ctx->num_bufs)
			ctx->buf_descs[buf].state = BUF_ALLOCATED;
		break;

	case UDATA_OP_CONNECT:
		if (res < 0) {
			ev->type = KPASS_EV_ERROR;
			ev->err.error = -res;
		} else {
			ev->type = KPASS_EV_CONNECTED;
			struct sock_desc *sd = get_sock(ctx, sock);
			if (sd)
				sd->connected = true;
		}
		break;

	case UDATA_OP_ACCEPT:
		if (res < 0) {
			ev->type = KPASS_EV_ERROR;
			ev->err.error = -res;
			ev->accepted.new_sock = KPASS_SOCK_INVALID;
		} else if (ctx->use_kernel_module && ctx->fd >= 0) {
			kpass_sock_id new_id = (kpass_sock_id)res;
			if (new_id >= CEPH_KPASS_MAX_SOCKETS || bitmap_test(ctx->sock_bitmap, new_id)) {
				ev->type = KPASS_EV_ERROR;
				ev->err.error = EINVAL;
			} else {
				bitmap_set(ctx->sock_bitmap, new_id);
				ctx->sockets[new_id].fd = -1;
				ctx->sockets[new_id].connected = true;
				ctx->sockets[new_id].listening = false;
				ev->type = KPASS_EV_ACCEPTED;
				ev->accepted.new_sock = new_id;
			}
		} else {
			kpass_sock_id new_id = alloc_sock_id(ctx);
			if (new_id == KPASS_SOCK_INVALID) {
				close(res);
				ev->type = KPASS_EV_ERROR;
				ev->err.error = EMFILE;
			} else {
				ctx->sockets[new_id].fd = res;
				ctx->sockets[new_id].connected = true;
				ctx->sockets[new_id].listening = false;
				ev->type = KPASS_EV_ACCEPTED;
				ev->accepted.new_sock = new_id;
			}
		}
		break;

	default:
		ev->type = KPASS_EV_ERROR;
		ev->err.error = EINVAL;
		break;
	}
}

/*
 * ============================================================================
 * Event Handling
 * ============================================================================
 */

int ceph_kpass_poll(struct ceph_kpass_ctx *ctx, struct kpass_event *events,
		   int max_events, int timeout_ms)
{
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts;
	int count = 0;
	int ret;

	if (ctx->pending > 0) {
		ret = io_uring_submit(&ctx->ring);
		if (ret < 0) {
			errno = -ret;
			return -1;
		}
		ctx->pending = 0;
	}

	while (count < max_events && ctx->deferred_count > 0) {
		struct pending_cqe pending = ctx->deferred[0];
		memmove(ctx->deferred, ctx->deferred + 1,
			(ctx->deferred_count - 1) * sizeof(*ctx->deferred));
		ctx->deferred_count--;
		fill_event_from_cqe(ctx, &events[count], pending.user_data, pending.res);
		count++;
	}

	if (count >= max_events)
		return count;

	if (timeout_ms >= 0) {
		ts.tv_sec = timeout_ms / 1000;
		ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
		ret = io_uring_wait_cqe_timeout(&ctx->ring, &cqe, &ts);
	} else {
		ret = io_uring_wait_cqe(&ctx->ring, &cqe);
	}

	if (ret < 0) {
		if (ret == -ETIME || ret == -EINTR)
			return 0;
		errno = -ret;
		return -1;
	}

	do {
		fill_event_from_cqe(ctx, &events[count], cqe->user_data, cqe->res);
		io_uring_cqe_seen(&ctx->ring, cqe);
		count++;

		if (count >= max_events)
			break;

	} while (io_uring_peek_cqe(&ctx->ring, &cqe) == 0);

	return count;
}

/*
 * ============================================================================
 * FUSE Integration
 * ============================================================================
 */

struct io_uring *ceph_kpass_get_ring(struct ceph_kpass_ctx *ctx)
{
	return &ctx->ring;
}
