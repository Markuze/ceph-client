// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <linux/io_uring/opaque.h>
#include <linux/io_uring/zcrx.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../kselftest.h"

/* No liburing dependency; exercise the 64-byte SQE and 32-byte CQE ABI. */
struct event {
	uint64_t tag;
	int32_t res;
	uint32_t flags;
	uint64_t extra[2];
};

struct ring {
	int fd;
	struct io_uring_params p;
	void *sq, *cq;
	struct io_uring_sqe *sqes;
	unsigned int *sq_head, *sq_tail, *sq_mask, *sq_array;
	unsigned int *cq_head, *cq_tail, *cq_mask;
	struct event *cqes;
	size_t sq_size, cq_size;
	struct event saved[64];
	unsigned int nr_saved;
};

static uint64_t next_tag = 1000;

static void require(bool ok, const char *what)
{
	if (!ok)
		ksft_exit_fail_msg("%s: errno=%d (%s)\n", what, errno, strerror(errno));
}

static int ring_init(struct ring *r, unsigned int flags)
{
	memset(r, 0, sizeof(*r));
	r->p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | flags;
	r->fd = syscall(__NR_io_uring_setup, 64, &r->p);
	if (r->fd < 0)
		return -errno;
	r->sq_size = r->p.sq_off.array + r->p.sq_entries * sizeof(unsigned int);
	r->cq_size = r->p.cq_off.cqes + r->p.cq_entries *
		(flags & IORING_SETUP_CQE32 ? sizeof(struct event) : sizeof(struct io_uring_cqe));
	if (r->p.features & IORING_FEAT_SINGLE_MMAP) {
		r->sq_size = r->sq_size > r->cq_size ? r->sq_size : r->cq_size;
		r->cq_size = r->sq_size;
	}
	r->sq = mmap(NULL, r->sq_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		     r->fd, IORING_OFF_SQ_RING);
	r->cq = r->p.features & IORING_FEAT_SINGLE_MMAP ? r->sq :
		mmap(NULL, r->cq_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		     r->fd, IORING_OFF_CQ_RING);
	r->sqes = mmap(NULL, r->p.sq_entries * sizeof(*r->sqes), PROT_READ | PROT_WRITE,
		       MAP_SHARED, r->fd, IORING_OFF_SQES);
	require(r->sq != MAP_FAILED && r->cq != MAP_FAILED && r->sqes != MAP_FAILED,
		"ring mappings");
	r->sq_head = r->sq + r->p.sq_off.head;
	r->sq_tail = r->sq + r->p.sq_off.tail;
	r->sq_mask = r->sq + r->p.sq_off.ring_mask;
	r->sq_array = r->sq + r->p.sq_off.array;
	r->cq_head = r->cq + r->p.cq_off.head;
	r->cq_tail = r->cq + r->p.cq_off.tail;
	r->cq_mask = r->cq + r->p.cq_off.ring_mask;
	r->cqes = r->cq + r->p.cq_off.cqes;
	return 0;
}

static void ring_exit(struct ring *r)
{
	munmap(r->sqes, r->p.sq_entries * sizeof(*r->sqes));
	if (r->cq != r->sq)
		munmap(r->cq, r->cq_size);
	munmap(r->sq, r->sq_size);
	close(r->fd);
}

static void stage(struct ring *r, struct io_uring_sqe sqe)
{
	unsigned int tail = __atomic_load_n(r->sq_tail, __ATOMIC_RELAXED);
	unsigned int index = tail & *r->sq_mask;

	require(tail - *r->sq_head < r->p.sq_entries, "SQ space");
	r->sqes[index] = sqe;
	r->sq_array[index] = index;
	__atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
}

static void enter(struct ring *r, unsigned int minimum)
{
	unsigned int pending = *r->sq_tail - *r->sq_head;
	int ret = syscall(__NR_io_uring_enter, r->fd, pending, minimum,
			  IORING_ENTER_GETEVENTS, NULL, 0);

	require(ret >= 0, "io_uring_enter");
}

static bool peek(struct ring *r, struct event *event)
{
	unsigned int head = *r->cq_head;
	struct io_uring_cqe *cqe;

again:
	if (head == __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE))
		return false;
	if (r->p.flags & IORING_SETUP_CQE32) {
		*event = r->cqes[head & *r->cq_mask];
		head++;
	} else {
		cqe = (void *)r->cqes + (head & *r->cq_mask) * sizeof(*cqe);
		memset(event, 0, sizeof(*event));
		memcpy(event, cqe, sizeof(*cqe));
		head++;
		if (cqe->flags & IORING_CQE_F_SKIP) {
			__atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);
			goto again;
		}
		if (cqe->flags & IORING_CQE_F_32) {
			memcpy(event->extra, cqe + 1, sizeof(event->extra));
			head++;
		}
	}
	__atomic_store_n(r->cq_head, head, __ATOMIC_RELEASE);
	return true;
}

static struct event wait_tag(struct ring *r, uint64_t tag)
{
	struct event event;
	unsigned int i;

	for (;;) {
		for (i = 0; i < r->nr_saved; i++) {
			if (r->saved[i].tag != tag)
				continue;
			event = r->saved[i];
			r->saved[i] = r->saved[--r->nr_saved];
			return event;
		}
		enter(r, 1);
		while (peek(r, &event)) {
			if (event.tag == tag)
				return event;
			require(r->nr_saved < 64, "completion stash");
			r->saved[r->nr_saved++] = event;
		}
	}
}

static uint64_t cmd_stage(struct ring *r, uint32_t context, unsigned int op,
			  uint64_t handle, uint64_t offset, unsigned int length, void *ptr)
{
	struct io_uring_sqe sqe = {
		.opcode = IORING_OP_OPAQUE_OBJ,
		.fd = -1,
		.ioprio = op,
		.zcrx_ifq_idx = context,
		.addr = handle,
		.off = offset,
		.len = length,
		.addr3 = (uintptr_t)ptr,
		.user_data = next_tag++,
	};

	stage(r, sqe);
	return sqe.user_data;
}

static struct event command(struct ring *r, uint32_t context, unsigned int op,
			    uint64_t handle, uint64_t offset, unsigned int length, void *ptr)
{
	return wait_tag(r, cmd_stage(r, context, op, handle, offset, length, ptr));
}

static int register_store(struct ring *r, struct io_uring_opaque_config *cfg,
			  uint32_t *context)
{
	struct io_uring_zcrx_ifq_reg reg = {
		.flags = ZCRX_REG_OPAQUE_OBJ,
		.opaque_config = (uintptr_t)cfg,
	};
	int ret = syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_ZCRX_IFQ, &reg, 1);

	*context = reg.zcrx_id;
	return ret < 0 ? -errno : ret;
}

static void tcp_pair(int pair[2])
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t len = sizeof(addr);
	int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;

	require(listener >= 0, "listen socket");
	require(!bind(listener, (void *)&addr, sizeof(addr)), "bind");
	require(!getsockname(listener, (void *)&addr, &len), "getsockname");
	require(!listen(listener, 1), "listen");
	pair[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	require(pair[0] >= 0, "connect socket");
	require(!connect(pair[0], (void *)&addr, sizeof(addr)), "connect");
	pair[1] = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
	require(pair[1] >= 0, "accept");
	require(!setsockopt(pair[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)), "TCP_NODELAY");
	close(listener);
}

static uint64_t attach(struct ring *r, uint32_t context, int fd, uint64_t tag)
{
	struct io_uring_sqe sqe = {
		.opcode = IORING_OP_RECV_ZC,
		.fd = fd,
		.ioprio = IORING_RECV_MULTISHOT,
		.zcrx_ifq_idx = context,
		.user_data = tag,
	};
	struct event event;

	stage(r, sqe);
	event = wait_tag(r, tag);
	require(!event.res && (event.flags & IORING_CQE_F_MORE) && event.extra[0], "stream token");
	return event.extra[0];
}

static void write_all(int fd, const void *data, size_t length)
{
	size_t done = 0;

	while (done < length) {
		ssize_t n = write(fd, data + done, length - done);

		require(n > 0, "socket write");
		done += n;
	}
}

static void read_all(int fd, void *data, size_t length)
{
	size_t done = 0;

	while (done < length) {
		ssize_t n = read(fd, data + done, length - done);

		require(n > 0, "socket read");
		done += n;
	}
}

static uint64_t send_stage_flags(struct ring *r, uint32_t context, int fd, uint64_t handle,
				 uint64_t offset, unsigned int length, unsigned int flags,
				 unsigned int msg_flags, unsigned int sqe_flags)
{
	struct io_uring_sqe sqe = {
		.opcode = IORING_OP_OPAQUE_OBJ_SEND,
		.fd = fd,
		.ioprio = flags,
		.flags = sqe_flags,
		.zcrx_ifq_idx = context,
		.addr = handle,
		.off = offset,
		.len = length,
		.msg_flags = msg_flags,
		.user_data = next_tag++,
	};

	stage(r, sqe);
	return sqe.user_data;
}

static uint64_t send_stage(struct ring *r, uint32_t context, int fd, uint64_t handle,
			   uint64_t offset, unsigned int length)
{
	return send_stage_flags(r, context, fd, handle, offset, length, 0, 0, 0);
}

struct transfer {
	int fd;
	const unsigned char *data;
	size_t length;
	unsigned int repeats;
	bool ok;
};

static void *writer(void *arg)
{
	struct transfer *tx = arg;

	write_all(tx->fd, tx->data, tx->length);
	return NULL;
}

static void *reader(void *arg)
{
	struct transfer *rx = arg;
	unsigned char buf[16384];
	size_t done = 0, total = rx->length * rx->repeats;

	rx->ok = true;
	while (done < total) {
		size_t length = total - done > sizeof(buf) ? sizeof(buf) : total - done;
		ssize_t n = read(rx->fd, buf, length);
		size_t i;

		if (n <= 0) {
			rx->ok = false;
			break;
		}
		for (i = 0; i < (size_t)n; i++)
			if (buf[i] != rx->data[(done + i) % rx->length])
				rx->ok = false;
		done += n;
	}
	return NULL;
}

struct bursts {
	int fd;
	size_t length;
	unsigned int count;
	unsigned int delay;
};

static void *burst_writer(void *arg)
{
	struct bursts *tx = arg;
	unsigned char *data = calloc(1, tx->length);
	unsigned int i;

	require(data, "burst allocation");
	for (i = 0; i < tx->count; i++) {
		write_all(tx->fd, data, tx->length);
		if (tx->delay)
			usleep(tx->delay);
	}
	free(data);
	return NULL;
}

static void test_collector_progress(struct io_uring_opaque_config *cfg,
				    size_t burst, unsigned int count, unsigned int delay)
{
	struct ring r;
	struct bursts tx = { .length = burst, .count = count, .delay = delay };
	struct event event;
	uint32_t context;
	uint64_t stream, tag;
	pthread_t producer;
	int pair[2];

	require(!ring_init(&r, IORING_SETUP_CQE32), "collector progress ring");
	require(!register_store(&r, cfg, &context), "collector progress store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], 700);
	tag = cmd_stage(&r, context, IORING_OPAQUE_DISCARD, stream, 0, burst * count, NULL);
	enter(&r, 0);
	tx.fd = pair[0];
	require(!pthread_create(&producer, NULL, burst_writer, &tx), "burst producer");
	event = wait_tag(&r, tag);
	require(event.res == (int)(burst * count), "collector survives all receive batches");
	pthread_join(producer, NULL);
	require(!r.nr_saved && !peek(&r, &event), "collector remains armed before EOF");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"collector progress close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("collector drains %u bursts of %zu bytes\n", count, burst);
}

static void test_reset(struct io_uring_opaque_config *cfg)
{
	struct ring r;
	struct io_uring_opaque_stream_stat stream_stat;
	struct io_uring_opaque_stat stat;
	struct linger linger = { .l_onoff = 1 };
	unsigned char buf[4096];
	uint32_t context;
	uint64_t stream, keep, read;
	int pair[2], i;

	require(!ring_init(&r, IORING_SETUP_CQE32), "reset ring");
	require(!register_store(&r, cfg, &context), "reset store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], 701);
	keep = cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, 0, sizeof(buf), NULL);
	memset(buf, 0xab, sizeof(buf));
	read = cmd_stage(&r, context, IORING_OPAQUE_READ_STREAM, stream, 4096, 64, buf);
	write_all(pair[0], buf, 1000);
	for (i = 0; i < 100; i++) {
		require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, stream, 0,
				 sizeof(stream_stat), &stream_stat).res, "reset stream stat");
		if (stream_stat.rx_next == 1000)
			break;
		usleep(1000);
	}
	require(i < 100, "capture prefix before reset");
	require(!setsockopt(pair[0], SOL_SOCKET, SO_LINGER, &linger, sizeof(linger)),
		"reset linger");
	close(pair[0]);
	require(wait_tag(&r, keep).res == -ECONNRESET, "reset rejects partial KEEP");
	require(wait_tag(&r, read).res == -ECONNRESET && buf[0] == 0xab,
		"reset rejects unavailable read without copying");
	require(wait_tag(&r, 701).res == -ECONNRESET, "collector reports negative socket error");
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		!stat.objects, "reset publishes no truncated object");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"reset stream close");
	ring_exit(&r);
	close(pair[1]);
	ksft_test_result_pass("RST rejects partial objects and unavailable reads\n");
}

static void test_mixed_cqe(struct io_uring_opaque_config *cfg)
{
	struct ring r;
	struct event event;
	uint32_t context;
	uint64_t stream, tag;
	int pair[2];

	require(!ring_init(&r, IORING_SETUP_CQE_MIXED), "mixed CQE ring");
	require(!register_store(&r, cfg, &context), "mixed CQE store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], 702);
	require(*r.cq_head == *r.cq_tail, "token consumes two mixed CQ slots");
	tag = cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, 0, 3, NULL);
	write_all(pair[0], "abc", 3);
	event = wait_tag(&r, tag);
	require(event.res == 3 && event.extra[0] && (event.flags & IORING_CQE_F_32),
		"mixed KEEP handle completion");
	require(!command(&r, context, IORING_OPAQUE_FREE, event.extra[0], 0, 0, NULL).res,
		"mixed CQE free");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"mixed CQE close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("mixed CQE tokens and handles carry the 32-byte flag\n");
}

static void test_send_last(struct io_uring_opaque_config *cfg)
{
	struct ring r, imported;
	struct event event;
	unsigned char buf[16];
	uint32_t context, imported_context;
	uint64_t stream, handle, tag, cursor = 0;
	int source[2], target[2], udp, i;

	require(!ring_init(&r, IORING_SETUP_CQE32), "SEND_LAST ring");
	require(!register_store(&r, cfg, &context), "SEND_LAST store");
	tcp_pair(source);
	tcp_pair(target);
	stream = attach(&r, context, source[1], next_tag++);
	tag = cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, cursor, 8, NULL);
	write_all(source[0], "abcdefgh", 8);
	event = wait_tag(&r, tag);
	require(event.res == 8 && event.extra[0], "SEND_LAST fixture object");
	handle = event.extra[0];
	cursor += 8;
	udp = socket(AF_INET, SOCK_DGRAM, 0);
	require(udp >= 0, "SEND_LAST invalid protocol fixture");
	{
		struct {
			int fd;
			uint64_t offset;
			unsigned int length, flags, sqe_flags;
			int error;
		} cases[] = {
			{ -1, 0, 8, IORING_OPAQUE_SEND_LAST, 0, -EBADF },
			{ udp, 0, 8, IORING_OPAQUE_SEND_LAST, 0, -EOPNOTSUPP },
			{ target[0], 0, 9, IORING_OPAQUE_SEND_LAST, 0, -ERANGE },
			{ target[0], 0, 0, IORING_OPAQUE_SEND_LAST, 0, -EINVAL },
			{ target[0], 0, 8, 2, 0, -EINVAL },
			{ target[0], 0, 8, IORING_OPAQUE_SEND_LAST,
				IOSQE_CQE_SKIP_SUCCESS, -EINVAL },
			{ target[0], UINT64_MAX, 8, IORING_OPAQUE_SEND_LAST, 0, -EINVAL },
		};

		for (i = 0; i < (int)ARRAY_SIZE(cases); i++) {
			tag = send_stage_flags(&r, context, cases[i].fd, handle,
					       cases[i].offset, cases[i].length,
					       cases[i].flags, 0, cases[i].sqe_flags);
			event = wait_tag(&r, tag);
			require(event.res == cases[i].error &&
				!(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
				"rejected SEND_LAST must not report consumption");
			event = command(&r, context, IORING_OPAQUE_READ, handle, 0, 8, buf);
			require(event.res == 8 && !memcmp(buf, "abcdefgh", 8),
				"rejected SEND_LAST preserves the object");
		}
	}
	close(udp);
	ksft_test_result_pass("SEND_LAST validation preserves ownership\n");
	{
		struct io_uring_sqe head = {
			.opcode = IORING_OP_OPAQUE_OBJ, .fd = -1,
			.ioprio = IORING_OPAQUE_READ_STREAM, .flags = IOSQE_IO_LINK,
			.zcrx_ifq_idx = context, .addr = stream, .off = cursor,
			.len = 1, .addr3 = (uintptr_t)buf, .user_data = next_tag++,
		};
		struct io_uring_sqe cancel = {
			.opcode = IORING_OP_ASYNC_CANCEL, .fd = -1,
			.addr = head.user_data, .user_data = next_tag++,
		};

		stage(&r, head);
		tag = send_stage_flags(&r, context, target[0], handle, 0, 8,
				       IORING_OPAQUE_SEND_LAST, 0, 0);
		enter(&r, 0);
		stage(&r, cancel);
		require(!wait_tag(&r, cancel.user_data).res, "cancel SEND_LAST predecessor");
		require(wait_tag(&r, head.user_data).res == -ECANCELED, "canceled predecessor");
		event = wait_tag(&r, tag);
		require(event.res == -ECANCELED && !(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
			"SEND_LAST cancellation before issue must not consume");
		event = command(&r, context, IORING_OPAQUE_READ, handle, 0, 8, buf);
		require(event.res == 8 && !memcmp(buf, "abcdefgh", 8), "unissued object survives");
	}
	ksft_test_result_pass("canceling an unissued SEND_LAST retains the cached object\n");
	for (i = 0; i < 2; i++) {
		tag = send_stage(&r, context, target[0], handle, 0, 8);
		event = wait_tag(&r, tag);
		require(event.res == 8 && !(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
			"ordinary SEND retains cache ownership");
		read_all(target[1], buf, 8);
		require(!memcmp(buf, "abcdefgh", 8), "repeated cached bytes");
	}
	tag = send_stage_flags(&r, context, target[0], handle, 2, 4,
			       IORING_OPAQUE_SEND_LAST, 0, 0);
	event = wait_tag(&r, tag);
	require(event.res == 4 && (event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
		"SEND_LAST subrange completion");
	read_all(target[1], buf, 4);
	require(!memcmp(buf, "cdef", 4), "consuming subrange bytes");
	require(command(&r, context, IORING_OPAQUE_READ, handle, 0, 1, buf).res == -ESTALE,
		"SEND_LAST consumes the entire object");
	require(command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res == -ESTALE,
		"no FREE required after SEND_LAST");
	ksft_test_result_pass("cached resends and consuming ranges share one send ABI\n");
	{
		struct zcrx_ctrl export = { .zcrx_id = context, .op = ZCRX_CTRL_EXPORT };
		struct io_uring_zcrx_ifq_reg reg = {
			.flags = ZCRX_REG_IMPORT | ZCRX_REG_OPAQUE_OBJ,
		};
		struct ring *rings[2] = { &r, &imported };
		uint32_t contexts[2];
		uint64_t heads[2], sends[2];

		require(!syscall(__NR_io_uring_register, r.fd,
				 IORING_REGISTER_ZCRX_CTRL, &export, 0), "SEND_LAST export");
		require(!ring_init(&imported, IORING_SETUP_CQE32), "SEND_LAST import ring");
		reg.if_idx = export.zc_export.zcrx_fd;
		require(!syscall(__NR_io_uring_register, imported.fd,
				 IORING_REGISTER_ZCRX_IFQ, &reg, 1), "SEND_LAST import");
		imported_context = reg.zcrx_id;
		close(export.zc_export.zcrx_fd);
		contexts[0] = context;
		contexts[1] = imported_context;
		tag = cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, cursor, 8, NULL);
		write_all(source[0], "abcdefgh", 8);
		event = wait_tag(&r, tag);
		require(event.res == 8, "competing SEND_LAST object");
		handle = event.extra[0];
		cursor += 8;
		/* Prepare both LAST requests before either can take the handle. */
		for (i = 0; i < 2; i++) {
			struct io_uring_sqe head = {
				.opcode = IORING_OP_OPAQUE_OBJ, .fd = -1,
				.ioprio = IORING_OPAQUE_READ_STREAM, .flags = IOSQE_IO_LINK,
				.zcrx_ifq_idx = contexts[i], .addr = stream, .off = cursor,
				.len = 1, .addr3 = (uintptr_t)&buf[i], .user_data = next_tag++,
			};

			heads[i] = head.user_data;
			stage(rings[i], head);
			sends[i] = send_stage_flags(rings[i], contexts[i], target[0], handle, 0, 8,
						    IORING_OPAQUE_SEND_LAST, 0, 0);
			enter(rings[i], 0);
		}
		write_all(source[0], "!", 1);
		event = wait_tag(&r, sends[0]);
		require(event.res == 8 && (event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
			"first prepared LAST wins");
		event = wait_tag(&imported, sends[1]);
		require(event.res == -ESTALE && !(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
			"second prepared LAST loses without consuming");
		for (i = 0; i < 2; i++)
			require(wait_tag(rings[i], heads[i]).res == 1 && buf[i] == '!',
				"shared stream read before LAST");
		read_all(target[1], buf, 8);
		require(!memcmp(buf, "abcdefgh", 8), "exactly one LAST sends bytes");
		event = command(&imported, imported_context, IORING_OPAQUE_READ,
				handle, 0, 1, buf);
		require(event.res == -ESTALE, "consumption spans imported rings");
		event = command(&r, context, IORING_OPAQUE_DISCARD, stream, cursor++, 1, NULL);
		require(event.res == 1, "discard shared framing byte");
		ring_exit(&imported);
	}
	ksft_test_result_pass("prepared SEND_LAST requests have one winner across rings\n");
	{
		size_t length = 4U << 20, j;
		unsigned char *data = malloc(length), *received = malloc(length);
		struct transfer tx = { .fd = source[0], .data = data, .length = length };
		struct transfer rx = { .fd = target[1], .data = data, .length = length };
		pthread_t producer, consumer;
		int small = 4096, window = 65536;

		require(data && received, "SEND_LAST large payload");
		for (j = 0; j < length; j++)
			data[j] = j % 251;
		require(!setsockopt(target[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)),
			"SEND_LAST small send queue");
		require(!setsockopt(target[1], SOL_SOCKET, SO_RCVBUF, &window, sizeof(window)),
			"SEND_LAST receive window");
		for (i = 0; i < 4; i++) {
			uint64_t first = 0, last;

			tag = cmd_stage(&r, context, IORING_OPAQUE_KEEP,
					stream, cursor, length, NULL);
			require(!pthread_create(&producer, NULL, writer, &tx), "producer thread");
			event = wait_tag(&r, tag);
			require(event.res == (int)length, "SEND_LAST complete large object");
			handle = event.extra[0];
			cursor += length;
			pthread_join(producer, NULL);
			if (i < 2)
				first = send_stage(&r, context, target[0], handle, 0, length);
			if (!i) {
				event = command(&r, context, IORING_OPAQUE_COMPACT,
						handle, 0, 0, NULL);
				require(!event.res, "compact behind cached send");
			}
			last = send_stage_flags(&r, context, target[0], handle, 0, length,
						IORING_OPAQUE_SEND_LAST,
						i == 3 ? MSG_DONTWAIT : 0, 0);
			if (i < 3) {
				require(command(&r, context, IORING_OPAQUE_READ,
						handle, 0, 1, buf).res == -ESTALE,
					"queued LAST invalidates the handle before completion");
				if (i) {
					struct io_uring_sqe cancel = {
						.opcode = IORING_OP_ASYNC_CANCEL, .fd = -1,
						.addr = last, .user_data = next_tag++,
					};

					stage(&r, cancel);
					require(!wait_tag(&r, cancel.user_data).res, "cancel LAST");
				}
			}
			if (i < 2) {
				if (i == 1) {
					event = wait_tag(&r, last);
					require(event.res == -ECANCELED &&
						(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
						"canceled queued LAST reports consumption without progress");
				}
				rx.repeats = i ? 1 : 2;
				require(!pthread_create(&consumer, NULL, reader, &rx),
					"consumer thread");
				event = wait_tag(&r, first);
				require(event.res == (int)length &&
					!(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
					"pinned cached send survives LAST consumption");
				if (!i) {
					event = wait_tag(&r, last);
					require(event.res == (int)length &&
						(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
						"compacted LAST completion");
				}
				pthread_join(consumer, NULL);
				require(rx.ok, "SEND and SEND_LAST ordered payload bytes");
			} else {
				event = wait_tag(&r, last);
				require((event.flags & IORING_CQE_F_OPAQUE_CONSUMED) &&
					((event.res > 0 && event.res < (int)length) ||
					 (i == 3 && event.res == -EAGAIN)),
					"short LAST result preserves consumption status");
				if (event.res > 0) {
					read_all(target[1], received, event.res);
					require(!memcmp(received, data, event.res), "prefix bytes");
				}
			}
			event = command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL);
			require(event.res == -ESTALE, "completed LAST remains consumed");
			if (!i)
				ksft_test_result_pass("compacted LAST follows old SEND\n");
			else if (i == 1)
				ksft_test_result_pass("queued LAST cancellation retains SEND\n");
			else if (i == 2)
				ksft_test_result_pass("poll cancellation reports consumption\n");
			else
				ksft_test_result_pass("MSG_DONTWAIT short LAST consumes\n");
		}
		free(received);
		free(data);
	}
	tag = cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, cursor, 8, NULL);
	write_all(source[0], "abcdefgh", 8);
	event = wait_tag(&r, tag);
	require(event.res == 8, "SEND_LAST I/O error object");
	handle = event.extra[0];
	require(!shutdown(target[0], SHUT_WR), "SEND_LAST shutdown destination");
	tag = send_stage_flags(&r, context, target[0], handle, 0, 8,
			       IORING_OPAQUE_SEND_LAST, 0, IOSQE_ASYNC);
	event = wait_tag(&r, tag);
	require(event.res == -EPIPE && (event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
		"I/O error after claim reports consumption");
	require(command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res == -ESTALE,
		"failed claimed LAST is not restored");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"SEND_LAST close collector");
	{
		struct io_uring_opaque_stat stat;

		require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
			!stat.objects && !stat.backing_bytes && !stat.extents && !stat.streams,
			"SEND_LAST store references drain");
	}
	ring_exit(&r);
	close(source[0]);
	close(source[1]);
	close(target[0]);
	close(target[1]);
	ksft_test_result_pass("failed SEND_LAST reports ownership and drains backing\n");
}

static void test_budget(struct io_uring_opaque_config cfg)
{
	struct ring r;
	uint32_t context;
	uint64_t stream, objects[4], pending;
	struct event event;
	struct io_uring_opaque_stat stat;
	int pair[2], i;
	unsigned char byte = 7;

	cfg.hard_limit = 3 * sysconf(_SC_PAGESIZE);
	cfg.compact_headroom = 0;
	cfg.max_object_size = 8U << 20;
	cfg.max_requests = 2;
	require(!ring_init(&r, IORING_SETUP_CQE32), "budget ring");
	require(!register_store(&r, &cfg, &context), "budget store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], 200);
	require(command(&r, context, IORING_OPAQUE_KEEP, stream, 0,
			cfg.hard_limit + 1, NULL).res == -EMSGSIZE,
		"reject KEEP larger than the entire capture allowance");
	for (i = 0; i < 3; i++) {
		ksft_print_msg("budget capture %d of 3\n", i + 1);
		pending = cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, i, 1, NULL);
		write_all(pair[0], &byte, 1);
		event = wait_tag(&r, pending);
		require(event.res == 1, "fill store budget");
		objects[i] = event.extra[0];
	}
	require(command(&r, context, IORING_OPAQUE_COMPACT, objects[0], 0, 0, NULL).res == -ENOBUFS,
		"source and destination must both fit");
	ksft_print_msg("budget filled; verifying release and discard\n");
	pending = cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, 3, 1, NULL);
	write_all(pair[0], &byte, 1);
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res,
		"STAT admitted at pending limit");
	require(stat.backing_bytes == cfg.hard_limit, "budget full");
	{
		uint64_t last = send_stage_flags(&r, context, pair[0], objects[0], 0, 1,
						 IORING_OPAQUE_SEND_LAST, 0, 0);

		event = wait_tag(&r, last);
		require(event.res == -EAGAIN && !(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
			"request admission rejects LAST without consuming");
	}
	require(!command(&r, context, IORING_OPAQUE_FREE, objects[0], 0, 0, NULL).res,
		"FREE admitted at pending limit");
	event = wait_tag(&r, pending);
	require(event.res == 1, "budget release resumes collector");
	objects[3] = event.extra[0];
	pending = cmd_stage(&r, context, IORING_OPAQUE_DISCARD, stream, 4, 1, NULL);
	write_all(pair[0], &byte, 1);
	require(wait_tag(&r, pending).res == 1, "discard when backing is full");
	for (i = 1; i < 4; i++)
		require(!command(&r, context, IORING_OPAQUE_FREE, objects[i], 0, 0, NULL).res,
			"release budget fixture");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"budget stream close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("budget stall/resume, control admission and discard at capacity\n");
}

struct blocked_read {
	int fd;
	ssize_t result;
	int error;
};

static void *blocking_reader(void *arg)
{
	struct blocked_read *read = arg;
	unsigned char byte;

	read->result = recv(read->fd, &byte, 1, 0);
	read->error = errno;
	return NULL;
}

static void test_waiting_reader(struct io_uring_opaque_config *cfg)
{
	struct ring r;
	uint32_t context;
	uint64_t stream;
	int pair[2];
	struct blocked_read blocked;
	pthread_t thread;

	require(!ring_init(&r, IORING_SETUP_CQE32), "ownership ring");
	require(!register_store(&r, cfg, &context), "ownership store");
	tcp_pair(pair);
	blocked.fd = pair[1];
	require(!pthread_create(&thread, NULL, blocking_reader, &blocked), "waiting reader");
	usleep(20000);
	stream = attach(&r, context, pair[1], 300);
	pthread_join(thread, NULL);
	require(blocked.result == -1 && blocked.error == EBUSY, "wake existing reader on claim");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"ownership stream close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("receive ownership excludes a previously sleeping reader\n");
}

static void test_eof(struct io_uring_opaque_config *cfg)
{
	struct ring r;
	uint32_t context;
	uint64_t stream, pending;
	struct io_uring_opaque_stat stat;
	int pair[2];

	require(!ring_init(&r, IORING_SETUP_CQE32), "EOF ring");
	require(!register_store(&r, cfg, &context), "EOF store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], 400);
	pending = cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, 0, 8, NULL);
	write_all(pair[0], "abc", 3);
	require(!shutdown(pair[0], SHUT_WR), "source EOF");
	require(wait_tag(&r, pending).res == -ENODATA, "EOF rejects incomplete object");
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res,
		"EOF store stat");
	require(!stat.objects && !stat.backing_bytes, "partial object references drain");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"EOF stream close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("EOF releases private assembly without publishing an object\n");
}

static void test_urgent(struct io_uring_opaque_config *cfg)
{
	struct ring r;
	uint32_t context;
	uint64_t stream, pending;
	int pair[2];
	unsigned char byte;

	require(!ring_init(&r, IORING_SETUP_CQE32), "urgent data ring");
	require(!register_store(&r, cfg, &context), "urgent data store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], 600);
	pending = cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, 0, 8, NULL);
	require(send(pair[0], "!", 1, MSG_OOB) == 1, "send urgent data");
	require(wait_tag(&r, pending).res == -EOPNOTSUPP, "urgent data rejects KEEP");
	require(wait_tag(&r, 600).res == -EOPNOTSUPP, "urgent data ends collector");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"urgent stream close");
	require(recv(pair[1], &byte, 1, MSG_OOB | MSG_DONTWAIT) == 1 && byte == '!',
		"urgent byte remains available after ownership release");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("urgent TCP data terminates unsupported collection\n");
}

static void test_teardown(struct io_uring_opaque_config *cfg)
{
	struct ring r;
	uint32_t context;
	uint64_t stream;
	int pair[2], i;
	unsigned char byte;

	require(!ring_init(&r, IORING_SETUP_CQE32), "teardown ring");
	require(!register_store(&r, cfg, &context), "teardown store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], 500);
	cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, 0, 1024, NULL);
	enter(&r, 0);
	ring_exit(&r);
	for (i = 0; i < 200; i++) {
		if (recv(pair[1], &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN)
			break;
		usleep(10000);
	}
	require(i < 200, "ring teardown releases receive claim");
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("ring teardown cancels an active collector and future KEEP\n");
}

static void test_unprivileged(struct io_uring_opaque_config cfg)
{
	pid_t child;
	int status;

	if (geteuid()) {
		ksft_test_result_skip("registration without capabilities requires root fixture\n");
		return;
	}
	fflush(stdout);
	child = fork();
	require(child >= 0, "unprivileged fixture fork");
	if (!child) {
		struct ring r;
		uint32_t context;
		struct rlimit limit = { .rlim_cur = 65536, .rlim_max = 65536 };

		cfg.hard_limit = sysconf(_SC_PAGESIZE);
		cfg.compact_headroom = 0;
		cfg.max_object_size = 4096;
		if (setrlimit(RLIMIT_MEMLOCK, &limit) || setgid(1000) || setuid(1000))
			_exit(1);
		if (ring_init(&r, IORING_SETUP_CQE32) || register_store(&r, &cfg, &context))
			_exit(2);
		ring_exit(&r);
		_exit(0);
	}
	require(waitpid(child, &status, 0) == child, "unprivileged fixture wait");
	require(WIFEXITED(status) && !WEXITSTATUS(status), "register without CAP_NET_ADMIN");
	ksft_test_result_pass("unprivileged registration respects locked-memory accounting\n");
}

int main(void)
{
	struct ring r, imported;
	struct io_uring_opaque_config cfg = {
		.hard_limit = 64U << 20,
		.compact_headroom = 16U << 20,
		.max_object_size = 8U << 20,
		.max_objects = 128,
		.max_streams = 16,
		.max_extents = 65536,
		.max_requests = 256,
	};
	struct io_uring_opaque_object_stat object_stat;
	struct io_uring_opaque_stat stat;
	struct io_uring_opaque_policy policy = {
		.flags = IORING_OPAQUE_AUTO_COMPACT,
		.min_age_ms = 1,
		.min_extents = 1,
		.bytes_per_second = 64U << 20,
		.burst_bytes = 8U << 20,
		.max_temporary_bytes = 8U << 20,
	};
	struct event event;
	uint32_t context, imported_context;
	uint64_t stream, handle, old_handle, tag, cursor = 8;
	unsigned char buf[32];
	int source[2], target[2], ret;

	alarm(90);
	ksft_print_header();
	ret = ring_init(&r, IORING_SETUP_CQE32);
	if (ret)
		ksft_exit_skip("io_uring unavailable: %s\n", strerror(-ret));
	ret = register_store(&r, &cfg, &context);
	if (ret == -EINVAL || ret == -EOPNOTSUPP)
		ksft_exit_skip("OPAQUE_OBJ unavailable\n");
	require(!ret, "opaque registration");
	ksft_set_plan(34);
	ksft_test_result_pass("register opaque store without RX device or mappings\n");
	tcp_pair(source);
	tcp_pair(target);
	stream = attach(&r, context, source[1], 100);
	errno = 0;
	require(recv(source[1], buf, sizeof(buf), MSG_DONTWAIT) == -1 && errno == EBUSY,
		"ordinary receive ownership guard");
	{
		int pipefd[2], repair = 1;
		struct io_uring_sqe second = { .opcode = IORING_OP_RECV_ZC, .fd = source[1],
			.ioprio = IORING_RECV_MULTISHOT, .zcrx_ifq_idx = context,
			.user_data = next_tag++ };

		require(!pipe(pipefd), "splice fixture");
		require(splice(source[1], NULL, pipefd[1], NULL, 1, SPLICE_F_NONBLOCK) < 0 &&
			errno == EBUSY, "competing splice guard");
		close(pipefd[0]);
		close(pipefd[1]);
		require(setsockopt(source[1], IPPROTO_TCP, TCP_ULP, "tls", 3) < 0 && errno == EBUSY,
			"ULP replacement guard");
		require(setsockopt(source[1], IPPROTO_TCP, TCP_REPAIR,
				   &repair, sizeof(repair)) < 0 && errno == EBUSY,
			"TCP_REPAIR ownership guard");
		stage(&r, second);
		require(wait_tag(&r, second.user_data).res == -EBUSY, "second collector guard");
	}
	ksft_test_result_pass("claim ordinary TCP socket and exclude competing receive\n");
	tag = cmd_stage(&r, context, IORING_OPAQUE_READ_STREAM, stream, 0, 3, buf);
	write_all(source[0], "a", 1);
	enter(&r, 0);
	require(!peek(&r, &event), "incomplete header read must wait");
	write_all(source[0], "bc", 2);
	event = wait_tag(&r, tag);
	require(event.res == 3 && !memcmp(buf, "abc", 3), "bounded header read");
	ksft_test_result_pass("read split stream framing without consuming it\n");
	tag = cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, 0, 8, NULL);
	enter(&r, 0);
	require(!peek(&r, &event), "incomplete KEEP must not publish");
	write_all(source[0], "defgh", 5);
	event = wait_tag(&r, tag);
	require(event.res == 8 && event.extra[0], "completed KEEP");
	handle = event.extra[0];
	event = command(&r, context, IORING_OPAQUE_READ, handle, 1, 5, buf);
	require(event.res == 5 && !memcmp(buf, "bcdef", 5), "object range read");
	ksft_test_result_pass("publish complete object and read a subrange\n");
	tag = send_stage(&r, context, target[0], handle, 2, 4);
	require(wait_tag(&r, tag).res == 4, "object range send");
	read_all(target[1], buf, 4);
	require(!memcmp(buf, "cdef", 4), "sent bytes");
	ksft_test_result_pass("send object subrange with one completion\n");
	event = command(&r, context, IORING_OPAQUE_COMPACT, handle, 0, 0, NULL);
	require(!event.res, "manual compaction");
	event = command(&r, context, IORING_OPAQUE_OBJECT_STAT, handle, 0,
			sizeof(object_stat), &object_stat);
	require(!event.res && (object_stat.flags & IORING_OPAQUE_COMPACTED), "compacted flag");
	event = command(&r, context, IORING_OPAQUE_READ, handle, 0, 8, buf);
	require(event.res == 8 && !memcmp(buf, "abcdefgh", 8), "stable compacted handle");
	ksft_test_result_pass("compact backing while preserving handle and bytes\n");
	{
		struct zcrx_ctrl export = { .zcrx_id = context, .op = ZCRX_CTRL_EXPORT };
		struct io_uring_zcrx_ifq_reg reg = {
			.flags = ZCRX_REG_IMPORT | ZCRX_REG_OPAQUE_OBJ,
		};

		require(!syscall(__NR_io_uring_register, r.fd,
				 IORING_REGISTER_ZCRX_CTRL, &export, 0),
			"export store");
		require(!ring_init(&imported, IORING_SETUP_CQE32), "import ring");
		reg.if_idx = export.zc_export.zcrx_fd;
		require(!syscall(__NR_io_uring_register, imported.fd,
				 IORING_REGISTER_ZCRX_IFQ, &reg, 1),
			"import store");
		imported_context = reg.zcrx_id;
		close(export.zc_export.zcrx_fd);
		event = command(&imported, imported_context, IORING_OPAQUE_READ, handle, 0, 8, buf);
		require(event.res == 8 && !memcmp(buf, "abcdefgh", 8), "shared object handle");
		ring_exit(&imported);
	}
	ksft_test_result_pass("export/import preserves shared handles and object lifetime\n");
	old_handle = handle;
	require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res, "FREE");
	require(command(&r, context, IORING_OPAQUE_READ, handle, 0, 1, buf).res == -ESTALE,
		"stale object rejection");
	ksft_test_result_pass("FREE invalidates the generation handle\n");
	tag = cmd_stage(&r, context, IORING_OPAQUE_DISCARD, stream, cursor, 4, NULL);
	write_all(source[0], "skip", 4);
	require(wait_tag(&r, tag).res == 4, "future discard");
	cursor += 4;
	ksft_test_result_pass("discard future bytes without publishing an object\n");
	{
		uint64_t pending = cmd_stage(&r, context, IORING_OPAQUE_KEEP,
					     stream, cursor, 4, NULL);
		struct io_uring_sqe cancel = { .opcode = IORING_OP_ASYNC_CANCEL, .fd = -1,
			.addr = pending, .user_data = next_tag++ };

		stage(&r, cancel);
		require(wait_tag(&r, cancel.user_data).res >= 0, "cancel future KEEP");
		require(wait_tag(&r, pending).res == -ECANCELED, "canceled KEEP completion");
	}
	ksft_test_result_pass("ASYNC_CANCEL terminates an incomplete KEEP\n");
	{
		uint64_t pending = cmd_stage(&r, context, IORING_OPAQUE_KEEP,
					     stream, cursor, 1, NULL);
		struct __kernel_timespec timeout = { .tv_nsec = 20000000 };
		unsigned int index = (*r.sq_tail - 1) & *r.sq_mask;
		struct io_uring_sqe link_timeout = { .opcode = IORING_OP_LINK_TIMEOUT, .fd = -1,
			.addr = (uintptr_t)&timeout, .len = 1, .user_data = next_tag++ };

		r.sqes[index].flags |= IOSQE_IO_LINK;
		stage(&r, link_timeout);
		require(wait_tag(&r, pending).res == -ECANCELED, "linked timeout cancels KEEP");
		require(wait_tag(&r, link_timeout.user_data).res == -ETIME,
			"linked timeout completion");
	}
	ksft_test_result_pass("linked timeout uses native cancellation\n");
	{
		unsigned char byte = 42;

		tag = cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, cursor, 1, NULL);
		write_all(source[0], &byte, 1);
		event = wait_tag(&r, tag);
		handle = event.extra[0];
		cursor++;
		require(event.res == 1 && handle != old_handle, "generation reuse");
		require(!command(&r, context, IORING_OPAQUE_SET_POLICY,
				 0, 0, sizeof(policy), &policy).res,
			"automatic compaction policy");
		for (ret = 0; ret < 100; ret++) {
			require(!command(&r, context, IORING_OPAQUE_OBJECT_STAT, handle, 0,
					 sizeof(object_stat), &object_stat).res,
				"auto object stat");
			if (object_stat.flags & IORING_OPAQUE_COMPACTED)
				break;
			usleep(10000);
		}
		require(ret < 100, "automatic compaction progress");
		policy.flags = 0;
		require(!command(&r, context, IORING_OPAQUE_SET_POLICY,
				 0, 0, sizeof(policy), &policy).res,
			"disable automatic compaction");
		require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res,
			"free auto object");
	}
	ksft_test_result_pass("automatic compaction preserves a reused slot's identity\n");
	{
		size_t length = 4U << 20, i;
		unsigned char *data = malloc(length);
		struct transfer tx = { .fd = source[0], .data = data, .length = length };
		struct transfer rx = {
			.fd = target[1], .data = data, .length = length, .repeats = 2,
		};
		pthread_t producer, consumer;
		uint64_t first, second;
		int small = 4096, window = 65536;

		require(data, "large payload allocation");
		for (i = 0; i < length; i++)
			data[i] = i % 251;
		tag = cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, cursor, length, NULL);
		require(!pthread_create(&producer, NULL, writer, &tx), "producer thread");
		event = wait_tag(&r, tag);
		require(event.res == (int)length && event.extra[0], "large complete object");
		handle = event.extra[0];
		cursor += length;
		pthread_join(producer, NULL);
		require(!setsockopt(target[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)),
			"small send queue");
		require(!setsockopt(target[1], SOL_SOCKET, SO_RCVBUF, &window, sizeof(window)),
			"receive window");
		first = send_stage(&r, context, target[0], handle, 0, length);
		require(!command(&r, context, IORING_OPAQUE_COMPACT, handle, 0, 0, NULL).res,
			"compaction while old send is blocked");
		second = send_stage(&r, context, target[0], handle, 0, length);
		require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res,
			"FREE with old and new backing in flight");
		require(!pthread_create(&consumer, NULL, reader, &rx), "consumer thread");
		require(wait_tag(&r, first).res == (int)length, "old backing send completion");
		require(wait_tag(&r, second).res == (int)length, "new backing send completion");
		pthread_join(consumer, NULL);
		require(rx.ok, "old and new backing payload bytes");
		free(data);
	}
	ksft_test_result_pass("blocked SEND, compaction, FIFO resend and FREE preserve bytes\n");
	{
		struct io_uring_sqe cancel;
		uint64_t compact;

		tag = cmd_stage(&r, context, IORING_OPAQUE_KEEP, stream, cursor, 3, NULL);
		write_all(source[0], "xyz", 3);
		event = wait_tag(&r, tag);
		require(event.res == 3, "cancel compaction object");
		handle = event.extra[0];
		compact = cmd_stage(&r, context, IORING_OPAQUE_COMPACT, handle, 0, 0, NULL);
		cancel = (struct io_uring_sqe) { .opcode = IORING_OP_ASYNC_CANCEL, .fd = -1,
			.addr = compact, .user_data = next_tag++ };
		stage(&r, cancel);
		require(!wait_tag(&r, cancel.user_data).res, "cancel compaction worker");
		require(wait_tag(&r, compact).res == -ECANCELED,
			"compaction cancellation completion");
		require(command(&r, context, IORING_OPAQUE_READ, handle, 0, 3, buf).res == 3 &&
			!memcmp(buf, "xyz", 3), "original survives canceled compaction");
		require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res,
			"free canceled compaction object");
	}
	ksft_test_result_pass("cancel compaction without publishing replacement backing\n");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"stream close");
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res,
		"store stat");
	require(!stat.backing_bytes && !stat.extents && !stat.objects && !stat.streams,
		"store references drain");
	ring_exit(&r);
	close(source[0]);
	close(source[1]);
	close(target[0]);
	close(target[1]);
	ksft_test_result_pass("close and teardown release all store backing and metadata\n");
	test_send_last(&cfg);
	test_budget(cfg);
	test_waiting_reader(&cfg);
	test_eof(&cfg);
	test_urgent(&cfg);
	test_teardown(&cfg);
	test_unprivileged(cfg);
	test_collector_progress(&cfg, 100, 300, 3000);
	test_collector_progress(&cfg, 1U << 20, 64, 0);
	test_reset(&cfg);
	test_mixed_cqe(&cfg);
	ksft_finished();
}
