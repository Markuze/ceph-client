// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <linux/io_uring/opaque.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
		.opaque_store_id = context,
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
	struct io_uring_opaque_register reg = {
		.size = sizeof(reg), .fd = -1,
		.config = (uintptr_t)cfg, .config_size = sizeof(*cfg),
	};
	int ret = syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_OPAQUE_STORE, &reg, 1);

	*context = reg.store_id;
	return ret < 0 ? -errno : ret;
}

static int export_store(struct ring *r, uint32_t context)
{
	struct io_uring_opaque_register reg = {
		.size = sizeof(reg), .fd = -1,
		.flags = IORING_OPAQUE_REG_EXPORT, .store_id = context,
	};
	int ret = syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_OPAQUE_STORE, &reg, 1);

	return ret < 0 ? -errno : reg.fd;
}

static int import_store(struct ring *r, int fd, uint32_t *context)
{
	struct io_uring_opaque_register reg = {
		.size = sizeof(reg), .fd = fd, .flags = IORING_OPAQUE_REG_IMPORT,
	};
	int ret = syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_OPAQUE_STORE, &reg, 1);

	*context = reg.store_id;
	return ret < 0 ? -errno : ret;
}

static bool tcp_pair_family(int pair[2], int family)
{
	union {
		struct sockaddr_in v4;
		struct sockaddr_in6 v6;
	} addr = {};
	socklen_t len;
	int listener = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;

	if (listener < 0 && family == AF_INET6 && errno == EAFNOSUPPORT)
		return false;
	require(listener >= 0, "listen socket");
	if (family == AF_INET6) {
		addr.v6.sin6_family = family;
		addr.v6.sin6_addr = in6addr_loopback;
		len = sizeof(addr.v6);
	} else {
		addr.v4.sin_family = family;
		addr.v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		len = sizeof(addr.v4);
	}
	require(!bind(listener, (void *)&addr, len), "bind");
	require(!getsockname(listener, (void *)&addr, &len), "getsockname");
	require(!listen(listener, 1), "listen");
	pair[0] = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
	require(pair[0] >= 0, "connect socket");
	require(!connect(pair[0], (void *)&addr, sizeof(addr)), "connect");
	pair[1] = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
	require(pair[1] >= 0, "accept");
	require(!setsockopt(pair[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)), "TCP_NODELAY");
	close(listener);
	return true;
}

static void tcp_pair(int pair[2])
{
	require(tcp_pair_family(pair, AF_INET), "IPv4 socket pair");
}

static uint64_t attach(struct ring *r, uint32_t context, int fd, uint64_t tag)
{
	struct io_uring_sqe sqe = {
		.opcode = IORING_OP_OPAQUE_COLLECT,
		.fd = fd,
		.ioprio = 0,
		.opaque_store_id = context,
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
		.opaque_store_id = context,
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

static uint64_t frame_stage(struct ring *r, uint32_t context, int fd, uint64_t handle,
			    unsigned int length, struct io_uring_opaque_frame *frame,
			    unsigned int flags, unsigned int msg_flags)
{
	struct io_uring_sqe sqe = {
		.opcode = IORING_OP_OPAQUE_OBJ_SEND, .fd = fd, .ioprio = flags,
		.opaque_store_id = context, .addr = handle, .len = length,
		.addr3 = (uintptr_t)frame, .msg_flags = msg_flags, .user_data = next_tag++,
	};

	stage(r, sqe);
	return sqe.user_data;
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

static void test_collector_edges(struct io_uring_opaque_config *cfg)
{
	struct ring r;
	struct event event;
	unsigned char data[100] = {};
	uint32_t context;
	uint64_t stream, tag;
	int pair[2], i;

	require(!ring_init(&r, IORING_SETUP_CQE32), "collector progress ring");
	require(!register_store(&r, cfg, &context), "collector progress store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], 700);
	for (i = 0; i < 10000; i++) {
		tag = cmd_stage(&r, context, IORING_OPAQUE_DISCARD, stream,
				(uint64_t)i * sizeof(data), sizeof(data), NULL);
		enter(&r, 0);
		write_all(pair[0], data, sizeof(data));
		require(wait_tag(&r, tag).res == (int)sizeof(data),
			"collector survives receive edge");
	}
	require(!r.nr_saved && !peek(&r, &event), "collector remains armed before EOF");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"collector progress close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("collector survives 10000 separate arrival/drain cycles\n");
}

static void test_collector_bulk(struct io_uring_opaque_config *cfg)
{
	struct ring r;
	struct io_uring_opaque_stream_stat stat;
	struct bursts tx = { .length = 1U << 20, .count = 4096 };
	struct event event;
	uint32_t context;
	uint64_t stream, tags[4];
	pthread_t producer;
	int pair[2], i;

	require(!ring_init(&r, IORING_SETUP_CQE32), "continuous collector ring");
	require(!register_store(&r, cfg, &context), "continuous collector store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], 703);
	for (i = 0; i < 4; i++)
		tags[i] = cmd_stage(&r, context, IORING_OPAQUE_DISCARD, stream,
				    (uint64_t)i << 30, 1U << 30, NULL);
	enter(&r, 0);
	tx.fd = pair[0];
	require(!pthread_create(&producer, NULL, burst_writer, &tx), "continuous producer");
	for (i = 0; i < 4; i++)
		require(wait_tag(&r, tags[i]).res == 1U << 30, "collector drains full GiB range");
	pthread_join(producer, NULL);
	require(!r.nr_saved && !peek(&r, &event), "continuous collector remains armed");
	require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, stream, 0,
			 sizeof(stat), &stat).res && stat.rx_next == 4ULL << 30 &&
		!stat.undecided_bytes, "stream offset survives TCP sequence wrap");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"continuous collector close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("collector drains 4 GiB without a new readability edge\n");
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
	keep = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, sizeof(buf), NULL);
	memset(buf, 0xab, sizeof(buf));
	read = cmd_stage(&r, context, IORING_OPAQUE_INSPECT, stream, 4096, 64, buf);
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
	tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, 3, NULL);
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

static void tcp_pair_small(int pair[2])
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t len = sizeof(addr);
	int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	int small = 4096, one = 1;

	require(listener >= 0, "small-window listen socket");
	require(!setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)),
		"small window before SYN");
	require(!bind(listener, (void *)&addr, sizeof(addr)), "small-window bind");
	require(!getsockname(listener, (void *)&addr, &len), "small-window address");
	require(!listen(listener, 1), "small-window listen");
	pair[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	require(pair[0] >= 0, "small-window connect socket");
	require(!setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)),
		"small send queue before connect");
	require(!connect(pair[0], (void *)&addr, sizeof(addr)), "small-window connect");
	pair[1] = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
	require(pair[1] >= 0, "small-window accept");
	require(!setsockopt(pair[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)),
		"small-window TCP_NODELAY");
	close(listener);
}

struct slow_transfer {
	int fd;
	unsigned char *data;
	size_t length;
	size_t done;
};

static void *slow_reader(void *arg)
{
	struct slow_transfer *rx = arg;

	while (rx->done < rx->length) {
		size_t length = rx->length - rx->done;
		ssize_t n = read(rx->fd, rx->data + rx->done, length > 4096 ? 4096 : length);

		if (n <= 0)
			break;
		rx->done += n;
		usleep(500);
	}
	return NULL;
}

static void test_send_fifo(struct io_uring_opaque_config *cfg)
{
	const size_t xlen = 2U << 20, ylen = 64U << 10, total = xlen + ylen;
	struct ring r;
	struct event event, first, second;
	unsigned char *data = malloc(total), *seen = malloc(total);
	struct transfer tx = { .data = data, .length = total };
	struct slow_transfer rx = { .data = seen, .length = total };
	uint32_t context;
	uint64_t stream, keep[2], handles[2], sends[2];
	pthread_t producer, consumer;
	int source[2], target[2];

	require(data && seen, "FIFO payload allocations");
	memset(data, 'A', xlen);
	memset(data + xlen, 'B', ylen);
	require(!ring_init(&r, IORING_SETUP_CQE32), "FIFO ring");
	require(!register_store(&r, cfg, &context), "FIFO store");
	tcp_pair(source);
	tcp_pair_small(target);
	stream = attach(&r, context, source[1], 704);
	keep[0] = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, xlen, NULL);
	keep[1] = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, xlen, ylen, NULL);
	tx.fd = source[0];
	require(!pthread_create(&producer, NULL, writer, &tx), "FIFO producer");
	event = wait_tag(&r, keep[0]);
	require(event.res == (int)xlen, "FIFO first KEEP");
	handles[0] = event.extra[0];
	event = wait_tag(&r, keep[1]);
	require(event.res == (int)ylen, "FIFO second KEEP");
	handles[1] = event.extra[0];
	pthread_join(producer, NULL);
	rx.fd = target[1];
	sends[0] = send_stage(&r, context, target[0], handles[0], 0, xlen);
	sends[1] = send_stage(&r, context, target[0], handles[1], 0, ylen);
	require(!pthread_create(&consumer, NULL, slow_reader, &rx), "FIFO consumer");
	require(wait_tag(&r, sends[0]).res == (int)xlen, "blocking SEND survives 128 polls");
	require(wait_tag(&r, sends[1]).res == (int)ylen, "FIFO second SEND");
	require(!shutdown(target[0], SHUT_WR), "FIFO peer EOF");
	pthread_join(consumer, NULL);
	require(rx.done == total && !memcmp(data, seen, total), "FIFO complete ordered wire bytes");
	close(target[0]);
	close(target[1]);
	ksft_test_result_pass("slow 4 KiB peer window preserves full SEND and FIFO wire order\n");

	tcp_pair_small(target);
	sends[0] = send_stage_flags(&r, context, target[0], handles[0], 0, xlen,
				    0, MSG_DONTWAIT, IOSQE_IO_LINK);
	sends[1] = send_stage_flags(&r, context, target[0], handles[1], 0, ylen,
				    IORING_OPAQUE_SEND_LAST, 0, 0);
	first = wait_tag(&r, sends[0]);
	second = wait_tag(&r, sends[1]);
	require(first.res > 0 && first.res < (int)xlen, "linked head returns a positive prefix");
	require(second.res == -ECANCELED && !(second.flags & IORING_CQE_F_OPAQUE_CONSUMED),
		"short linked send cancels a final-send follower before ownership transfer");
	require(command(&r, context, IORING_OPAQUE_READ_OBJECT, handles[1], 0, 1, seen).res == 1 &&
		seen[0] == 'B', "unissued linked final send preserves its object");
	require(!shutdown(target[0], SHUT_WR), "linked prefix peer EOF");
	rx = (struct slow_transfer) { .fd = target[1], .data = seen, .length = total };
	slow_reader(&rx);
	require(rx.done == (size_t)first.res && !memcmp(data, seen, rx.done),
		"linked cancellation emits only the head prefix");
	close(target[0]);
	close(target[1]);
	ksft_test_result_pass("short SEND cancels linked SEND_LAST without consuming its object\n");

	tcp_pair_small(target);
	sends[0] = send_stage(&r, context, target[0], handles[0], 0, xlen);
	sends[1] = send_stage_flags(&r, context, target[0], handles[1], 0, ylen,
				    IORING_OPAQUE_SEND_LAST, 0, 0);
	enter(&r, 0);
	{
		struct io_uring_sqe cancel = {
			.opcode = IORING_OP_ASYNC_CANCEL, .fd = -1,
			.addr = sends[0], .user_data = next_tag++,
		};

		stage(&r, cancel);
		require(!wait_tag(&r, cancel.user_data).res, "cancel FIFO head");
	}
	first = wait_tag(&r, sends[0]);
	second = wait_tag(&r, sends[1]);
	require(first.res > 0 && first.res < (int)xlen, "canceled head reports queued prefix");
	require(second.res == -ECANCELED && (second.flags & IORING_CQE_F_OPAQUE_CONSUMED),
		"broken FIFO cancels claimed final-send follower");
	require(!shutdown(target[0], SHUT_WR), "canceled FIFO peer EOF");
	rx = (struct slow_transfer) { .fd = target[1], .data = seen, .length = total };
	slow_reader(&rx);
	require(rx.done == (size_t)first.res && !memcmp(data, seen, rx.done),
		"broken FIFO emits only the head prefix");
	require(!command(&r, context, IORING_OPAQUE_FREE, handles[0], 0, 0, NULL).res,
		"FIFO reusable object free");
	require(command(&r, context, IORING_OPAQUE_FREE, handles[1], 0, 0, NULL).res == -ESTALE,
		"FIFO final-send follower stays consumed");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"FIFO stream close");
	ring_exit(&r);
	close(source[0]);
	close(source[1]);
	close(target[0]);
	close(target[1]);
	free(data);
	free(seen);
	ksft_test_result_pass("a partial SEND cancels queued followers without splicing\n");
}

static void test_link_order(struct io_uring_opaque_config *cfg)
{
	struct ring r;
	struct event event;
	unsigned char buf[8];
	uint32_t context;
	uint64_t stream, handle, free_tag, next;
	int source[2], target[2], i;

	require(!ring_init(&r, IORING_SETUP_CQE32), "link order ring");
	require(!register_store(&r, cfg, &context), "link order store");
	tcp_pair(source);
	tcp_pair(target);
	stream = attach(&r, context, source[1], 705);
	for (i = 0; i < 2; i++) {
		uint64_t keep = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT,
					 stream, i * sizeof(buf), sizeof(buf), NULL);
		unsigned int index;

		write_all(source[0], "abcdefgh", sizeof(buf));
		event = wait_tag(&r, keep);
		require(event.res == (int)sizeof(buf), "link order KEEP");
		handle = event.extra[0];
		free_tag = cmd_stage(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL);
		index = (*r.sq_tail - 1) & *r.sq_mask;
		r.sqes[index].flags |= i ? IOSQE_IO_HARDLINK : IOSQE_IO_LINK;
		if (i)
			next = send_stage(&r, context, target[0], handle, 0, sizeof(buf));
		else
			next = cmd_stage(&r, context, IORING_OPAQUE_READ_OBJECT,
					 handle, 0, sizeof(buf), buf);
		require(!wait_tag(&r, free_tag).res, "linked FREE succeeds");
		require(wait_tag(&r, next).res == -ESTALE, "linked operation observes FREE");
	}
	{
		struct io_uring_sqe gate = {
			.opcode = IORING_OP_OPAQUE_OBJ, .fd = -1,
			.ioprio = IORING_OPAQUE_INSPECT, .flags = IOSQE_IO_LINK,
			.opaque_store_id = context, .addr = stream, .off = 24,
			.len = 1, .addr3 = (uintptr_t)buf, .user_data = next_tag++,
		};
		uint64_t keep = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT,
					 stream, 16, sizeof(buf), NULL);

		write_all(source[0], "abcdefgh", sizeof(buf));
		event = wait_tag(&r, keep);
		require(event.res == (int)sizeof(buf), "gated SEND KEEP");
		handle = event.extra[0];
		stage(&r, gate);
		next = send_stage(&r, context, target[0], handle, 0, sizeof(buf));
		enter(&r, 0);
		require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res,
			"FREE before delayed SEND issues");
		write_all(source[0], "!", 1);
		require(wait_tag(&r, gate.user_data).res == 1, "release delayed SEND");
		require(wait_tag(&r, next).res == -ESTALE, "unissued SEND has no backing pin");
	}
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"link order stream close");
	ring_exit(&r);
	close(source[0]);
	close(source[1]);
	close(target[0]);
	close(target[1]);
	ksft_test_result_pass("linked READ and SEND resolve handles when execution starts\n");
}

static void test_keep_restore(struct io_uring_opaque_config *cfg)
{
	int i;

	for (i = 0; i < 2; i++) {
		struct ring r;
		struct event event;
		struct io_uring_opaque_stream_stat stat;
		struct __kernel_timespec timeout = { .tv_nsec = 200000000 };
		struct io_uring_sqe end = {
			.opcode = i ? IORING_OP_LINK_TIMEOUT : IORING_OP_ASYNC_CANCEL,
			.fd = -1, .user_data = next_tag++,
		};
		unsigned char buf[8];
		uint32_t context;
		uint64_t stream, keep, inspect = 0;
		int pair[2], attempt;

		require(!ring_init(&r, IORING_SETUP_CQE32), "KEEP restore ring");
		require(!register_store(&r, cfg, &context), "KEEP restore store");
		tcp_pair(pair);
		stream = attach(&r, context, pair[1], 706);
		keep = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0,
				 sizeof(buf), NULL);
		if (i) {
			r.sqes[(*r.sq_tail - 1) & *r.sq_mask].flags |= IOSQE_IO_LINK;
			end.addr = (uintptr_t)&timeout;
			end.len = 1;
			stage(&r, end);
		}
		write_all(pair[0], "abc", 3);
		for (attempt = 0; attempt < 100; attempt++) {
			require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, stream, 0,
					 sizeof(stat), &stat).res, "KEEP restore stream stat");
			if (stat.rx_next == 3)
				break;
			usleep(1000);
		}
		require(attempt < 100, "private KEEP prefix was captured");
		if (!i) {
			end.addr = keep;
			stage(&r, end);
			/* Inspect in the same batch, before the failed receive's task work. */
			inspect = cmd_stage(&r, context, IORING_OPAQUE_INSPECT,
					    stream, 0, 3, buf);
		}
		if (inspect)
			require(wait_tag(&r, inspect).res == 3 && !memcmp(buf, "abc", 3),
				"cancel restores prefix before the next SQE inspects it");
		require(wait_tag(&r, keep).res == -ECANCELED, "partial KEEP canceled");
		require(wait_tag(&r, end.user_data).res == (i ? -ETIME : 0),
			"KEEP cancellation trigger");
		require(command(&r, context, IORING_OPAQUE_INSPECT,
				stream, 0, 3, buf).res == 3 && !memcmp(buf, "abc", 3),
			"unsuccessful KEEP restores readable prefix");
		if (i)
			require(command(&r, context, IORING_OPAQUE_DISCARD,
					stream, 0, 3, NULL).res == 3, "discard restored prefix");
		keep = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT,
				 stream, i ? 3 : 0, i ? 5 : 8, NULL);
		write_all(pair[0], "defgh", 5);
		event = wait_tag(&r, keep);
		require(event.res == (i ? 5 : 8), "KEEP succeeds after prefix recovery");
		require(command(&r, context, IORING_OPAQUE_READ_OBJECT, event.extra[0], 0,
				i ? 5 : 8, buf).res == (i ? 5 : 8) &&
			!memcmp(buf, i ? "defgh" : "abcdefgh", i ? 5 : 8),
			"recovered object bytes");
		require(!command(&r, context, IORING_OPAQUE_FREE, event.extra[0], 0, 0, NULL).res,
			"recovered object free");
		require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
			"KEEP restore close");
		ring_exit(&r);
		close(pair[0]);
		close(pair[1]);
		ksft_test_result_pass("partial KEEP survives %s and supports %s\n",
				      i ? "timeout" : "cancellation", i ? "DISCARD" : "retry");
	}
}

struct interleaved {
	int fd[2];
	const unsigned char *data;
};

static void *interleaved_writer(void *arg)
{
	struct interleaved *tx = arg;
	int i;

	for (i = 0; i < 32; i++) {
		write_all(tx->fd[0], tx->data + i * 1024, 1024);
		write_all(tx->fd[1], tx->data + i * 1024, 1024);
		usleep(1000);
	}
	return NULL;
}

static void test_interleaved(struct io_uring_opaque_config cfg)
{
	unsigned char data[32 * 1024], seen[sizeof(data)];
	int i;

	for (i = 0; i < (int)sizeof(data); i++)
		data[i] = i % 251;
	for (i = 0; i < 2; i++) {
		struct ring r;
		struct event event;
		struct io_uring_opaque_stat stat;
		struct __kernel_timespec timeout = { .tv_sec = 5 };
		struct io_uring_sqe deadline = {
			.opcode = IORING_OP_LINK_TIMEOUT, .fd = -1, .len = 1,
			.addr = (uintptr_t)&timeout, .user_data = next_tag++,
		};
		struct interleaved tx = { .data = data };
		uint32_t context;
		uint64_t stream, keep;
		pthread_t producer;
		int source[2], other[2];

		cfg.hard_limit = i ? sizeof(data) : 256 * 1024;
		cfg.compact_headroom = 0;
		require(!ring_init(&r, IORING_SETUP_CQE32), "interleaved ring");
		require(!register_store(&r, &cfg, &context), "interleaved store");
		tcp_pair(source);
		tcp_pair(other);
		stream = attach(&r, context, source[1], 707);
		keep = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0,
				 sizeof(data), NULL);
		r.sqes[(*r.sq_tail - 1) & *r.sq_mask].flags |= IOSQE_IO_LINK;
		stage(&r, deadline);
		enter(&r, 0);
		tx.fd[0] = source[0];
		tx.fd[1] = other[0];
		require(!pthread_create(&producer, NULL, interleaved_writer, &tx),
			"interleaved producer");
		event = wait_tag(&r, keep);
		require(event.res == (int)sizeof(data), "interleaved KEEP fits physical quota");
		require(wait_tag(&r, deadline.user_data).res == -ECANCELED,
			"interleaved KEEP beats deadline");
		pthread_join(producer, NULL);
		require(command(&r, context, IORING_OPAQUE_READ_OBJECT, event.extra[0], 0,
				sizeof(seen), seen).res == (int)sizeof(seen) &&
			!memcmp(data, seen, sizeof(data)), "interleaved complete object bytes");
		require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
			stat.backing_bytes <= cfg.hard_limit, "interleaved quota bound");
		require(!command(&r, context, IORING_OPAQUE_FREE, event.extra[0], 0, 0, NULL).res,
			"interleaved FREE");
		require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
			"interleaved close");
		ring_exit(&r);
		close(source[0]);
		close(source[1]);
		close(other[0]);
		close(other[1]);
		ksft_test_result_pass("32 KiB interleaved KEEP completes within %llu-byte quota\n",
				      (unsigned long long)cfg.hard_limit);
	}
}

static void test_compact_queue(struct io_uring_opaque_config *cfg)
{
	struct ring r;
	struct event event;
	struct io_uring_opaque_stat stat;
	unsigned char data[65536], seen[sizeof(data)];
	uint32_t context;
	uint64_t stream, handles[16], copies[16], keep;
	int pair[2], i;

	memset(data, 0x73, sizeof(data));
	require(!ring_init(&r, IORING_SETUP_CQE32), "compaction queue ring");
	require(!register_store(&r, cfg, &context), "compaction queue store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], 708);
	for (i = 0; i < 16; i++) {
		keep = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream,
				 (uint64_t)i * sizeof(data), sizeof(data), NULL);
		write_all(pair[0], data, sizeof(data));
		event = wait_tag(&r, keep);
		require(event.res == (int)sizeof(data), "compaction queue KEEP");
		handles[i] = event.extra[0];
	}
	for (i = 0; i < 16; i++)
		copies[i] = cmd_stage(&r, context, IORING_OPAQUE_COMPACT, handles[i], 0, 0, NULL);
	{
		struct io_uring_sqe cancel = {
			.opcode = IORING_OP_ASYNC_CANCEL, .fd = -1,
			.addr = copies[15], .user_data = next_tag++,
		};

		stage(&r, cancel);
		require(!wait_tag(&r, cancel.user_data).res, "cancel queued compaction");
	}
	for (i = 0; i < 16; i++) {
		require(wait_tag(&r, copies[i]).res == (i == 15 ? -ECANCELED : 0),
			"shared compaction dispatcher completion");
		require(command(&r, context, IORING_OPAQUE_READ_OBJECT, handles[i], 0,
				sizeof(seen), seen).res == (int)sizeof(seen) &&
			!memcmp(data, seen, sizeof(data)), "queued compaction preserves bytes");
		require(!command(&r, context, IORING_OPAQUE_FREE, handles[i], 0, 0, NULL).res,
			"queued compaction FREE");
	}
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"compaction queue close");
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		!stat.backing_bytes && !stat.extents, "compaction dispatcher drains reservations");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("shared compaction worker drains batches and canceled requests\n");
}

static void test_budget_error(struct io_uring_opaque_config cfg)
{
	int i;

	cfg.hard_limit = sysconf(_SC_PAGESIZE);
	cfg.compact_headroom = 0;
	for (i = 0; i < 2; i++) {
		struct ring r;
		struct event event;
		struct io_uring_opaque_stream_stat stat;
		struct linger linger = { .l_onoff = 1 };
		unsigned char byte;
		uint32_t context;
		uint64_t stream, handle, keep;
		int pair[2];

		require(!ring_init(&r, IORING_SETUP_CQE32), "quota error ring");
		require(!register_store(&r, &cfg, &context), "quota error store");
		tcp_pair(pair);
		stream = attach(&r, context, pair[1], 709);
		keep = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, 1, NULL);
		write_all(pair[0], "x", 1);
		event = wait_tag(&r, keep);
		require(event.res == 1, "fill error-test quota");
		handle = event.extra[0];
		keep = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 1, 4, NULL);
		write_all(pair[0], "abcd", 4);
		enter(&r, 0);
		require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, stream, 0,
				 sizeof(stat), &stat).res && stat.rx_next == 1,
			"collector is stalled by retained backing");
		event = wait_tag(&r, 709);
		require(event.res == -ENOBUFS && (event.flags & IORING_CQE_F_MORE) &&
			event.extra[0] == stream && event.extra[1] == 1,
			"quota-stalled collector reports pressure before socket errors");
		if (i) {
			require(send(pair[0], "!", 1, MSG_OOB) == 1, "urgent data at full quota");
		} else {
			require(!setsockopt(pair[0], SOL_SOCKET, SO_LINGER,
					    &linger, sizeof(linger)),
				"quota error reset linger");
			close(pair[0]);
		}
		require(wait_tag(&r, keep).res == (i ? -EOPNOTSUPP : -ECONNRESET),
			"quota-stalled KEEP observes socket condition");
		require(wait_tag(&r, 709).res == (i ? -EOPNOTSUPP : -ECONNRESET),
			"quota-stalled collector observes socket condition");
		require(command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0, 1,
				&byte).res == 1 &&
			byte == 'x', "cached object survives source error");
		require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res,
			"error-test object FREE");
		require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
			"quota error close");
		ring_exit(&r);
		if (i)
			close(pair[0]);
		close(pair[1]);
		ksft_test_result_pass("full quota still observes %s\n", i ? "urgent data" : "RST");
	}
}

#ifndef SEND_CANCEL_ROUNDS
#define SEND_CANCEL_ROUNDS 10000
#endif
#define SEND_CANCEL_LENGTH (256 << 10)

struct send_cancel_race {
	pthread_barrier_t begin, admitted, finished, drained;
	int store_fd, target[2], cpu;
	uint64_t handle;
	struct event follower;
	unsigned char *wire;
	size_t expected;
	unsigned int expired;
	bool head_done;
};

static void barrier_wait(pthread_barrier_t *barrier)
{
	int ret = pthread_barrier_wait(barrier);

	require(!ret || ret == PTHREAD_BARRIER_SERIAL_THREAD, "race barrier");
}

static void *send_cancel_follower(void *arg)
{
	struct send_cancel_race *race = arg;
	struct ring r;
	uint32_t context;
	cpu_set_t cpus;
	unsigned int i;

	CPU_ZERO(&cpus);
	CPU_SET(race->cpu, &cpus);
	require(!pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus),
		"follower CPU");
	require(!ring_init(&r, IORING_SETUP_CQE32), "follower ring");
	require(!import_store(&r, race->store_fd, &context), "follower shared store");
	for (i = 0; i < SEND_CANCEL_ROUNDS; i++) {
		struct __kernel_timespec ts = { .tv_nsec = 1 + (i % 8) * 1000 };
		struct io_uring_sqe end = { .fd = -1 };
		struct event event;
		uint64_t tag;
		bool timeout = i % 4 >= 2;

		barrier_wait(&race->begin);
		/* Only this thread allocates tags until the admission barrier. */
		end.user_data = next_tag++;
		tag = send_stage_flags(&r, context, race->target[0], race->handle, 0, 8,
				       IORING_OPAQUE_SEND_LAST, 0, timeout ? IOSQE_IO_LINK : 0);
		if (timeout) {
			end.opcode = IORING_OP_LINK_TIMEOUT;
			end.addr = (uintptr_t)&ts;
			end.len = 1;
			stage(&r, end);
		}
		enter(&r, 0);
		barrier_wait(&race->admitted);
		if (!timeout) {
			if (i % 8 == 4) {
				while (!__atomic_load_n(&race->head_done, __ATOMIC_ACQUIRE))
					sched_yield();
				enter(&r, 0);
			}
			end.opcode = IORING_OP_ASYNC_CANCEL;
			end.addr = tag;
			stage(&r, end);
		}
		event = wait_tag(&r, end.user_data);
		if (timeout) {
			/* Remote completion/failure can win before timeout task work cancels. */
			require(event.res == -ETIME || event.res == -ECANCELED ||
				event.res == -ENOENT || event.res == -EALREADY,
				"queued follower timeout completion");
			race->expired += event.res == -ETIME;
		} else {
			require(!event.res || event.res == -ENOENT || event.res == -EALREADY,
				"queued follower cancel completion");
		}
		race->follower = wait_tag(&r, tag);
		require((race->follower.res == 8 || race->follower.res == -ECANCELED) &&
			(race->follower.flags & IORING_CQE_F_OPAQUE_CONSUMED),
			"admitted follower completes once with ownership reported");
		enter(&r, 0);
		require(!r.nr_saved && !peek(&r, &event), "no duplicate follower CQE");
		barrier_wait(&race->finished);
		barrier_wait(&race->drained);
	}
	ring_exit(&r);
	return NULL;
}

static void *send_cancel_drain(void *arg)
{
	struct send_cancel_race *race = arg;
	unsigned int i;

	for (i = 0; i < SEND_CANCEL_ROUNDS; i++) {
		size_t done = 0;

		barrier_wait(&race->begin);
		barrier_wait(&race->admitted);
		for (;;) {
			size_t expected = __atomic_load_n(&race->expected, __ATOMIC_ACQUIRE);
			ssize_t n;

			if (expected != SIZE_MAX) {
				require(done <= expected, "no unexpected follower bytes");
				if (done == expected)
					break;
			}
			if (done == SEND_CANCEL_LENGTH + 8) {
				sched_yield();
				continue;
			}
			n = recv(race->target[1], race->wire + done,
				 SEND_CANCEL_LENGTH + 8 - done, MSG_DONTWAIT);
			if (n > 0) {
				int one = 1;

				done += n;
				require(!setsockopt(race->target[1], IPPROTO_TCP, TCP_QUICKACK,
						    &one, sizeof(one)), "race immediate ACK");
			} else {
				require(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
					"race wire receive");
				usleep(10);
			}
		}
		barrier_wait(&race->drained);
	}
	return NULL;
}

static void test_send_cancel_race(struct io_uring_opaque_config *cfg)
{
	struct send_cancel_race race = {};
	struct io_uring_opaque_stat stat;
	struct sockaddr_in addr = { .sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t addr_length = sizeof(addr);
	cpu_set_t original, cpus;
	struct ring r;
	struct event event;
	struct transfer tx;
	pthread_t follower, drain, producer;
	unsigned char *data = malloc(SEND_CANCEL_LENGTH), byte;
	uint32_t context;
	uint64_t stream, handle, tag;
	unsigned int i, j, complete = 0, partial = 0, canceled = 0, sent = 0;
	int source[2], listener, small = 4096, receive = 32768, one = 1, first_cpu = -1;

	require(!pthread_getaffinity_np(pthread_self(), sizeof(original), &original),
		"race CPU set");
	race.cpu = -1;
	for (i = 0; i < CPU_SETSIZE; i++) {
		if (!CPU_ISSET(i, &original))
			continue;
		if (first_cpu < 0) {
			first_cpu = i;
		} else {
			race.cpu = i;
			break;
		}
	}
	if (race.cpu < 0) {
		free(data);
		ksft_test_result_skip("cross-ring SEND cancellation requires two CPUs\n");
		return;
	}
	CPU_ZERO(&cpus);
	CPU_SET(first_cpu, &cpus);
	require(!pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus), "head CPU");
	require(data && !ring_init(&r, IORING_SETUP_CQE32), "race head ring");
	require(!register_store(&r, cfg, &context), "race store");
	race.store_fd = export_store(&r, context);
	require(race.store_fd >= 0, "race store export");
	race.wire = malloc(SEND_CANCEL_LENGTH + 8);
	require(race.wire, "race wire buffer");
	tcp_pair(source);
	/* Set the receive window before connect so the head must wait for a reader. */
	listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	require(listener >= 0 &&
		!setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &receive, sizeof(receive)) &&
		!bind(listener, (void *)&addr, addr_length) &&
		!getsockname(listener, (void *)&addr, &addr_length) && !listen(listener, 1),
		"race listener");
	race.target[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	require(race.target[0] >= 0 &&
		!setsockopt(race.target[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) &&
		!connect(race.target[0], (void *)&addr, addr_length), "race connect");
	race.target[1] = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
	require(race.target[1] >= 0 &&
		!setsockopt(race.target[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)),
		"race destination");
	close(listener);
	stream = attach(&r, context, source[1], next_tag++);
	memset(data, 'A', SEND_CANCEL_LENGTH);
	tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0,
			SEND_CANCEL_LENGTH, NULL);
	tx = (struct transfer) { .fd = source[0], .data = data,
		.length = SEND_CANCEL_LENGTH };
	require(!pthread_create(&producer, NULL, writer, &tx), "race object producer");
	event = wait_tag(&r, tag);
	pthread_join(producer, NULL);
	require(event.res == SEND_CANCEL_LENGTH, "race head object");
	handle = event.extra[0];
	require(!pthread_barrier_init(&race.begin, NULL, 3) &&
		!pthread_barrier_init(&race.admitted, NULL, 3) &&
		!pthread_barrier_init(&race.finished, NULL, 2) &&
		!pthread_barrier_init(&race.drained, NULL, 3), "race barriers");
	require(!pthread_create(&follower, NULL, send_cancel_follower, &race) &&
		!pthread_create(&drain, NULL, send_cancel_drain, &race), "race threads");
	for (i = 0; i < SEND_CANCEL_ROUNDS; i++) {
		struct io_uring_sqe cancel = { .opcode = IORING_OP_ASYNC_CANCEL,
			.fd = -1, .user_data = next_tag++ };
		int progress;

		tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream,
				SEND_CANCEL_LENGTH + (uint64_t)i * 8, 8, NULL);
		write_all(source[0], "BBBBBBBB", 8);
		event = wait_tag(&r, tag);
		require(event.res == 8, "race follower object");
		race.handle = event.extra[0];
		__atomic_store_n(&race.expected, SIZE_MAX, __ATOMIC_RELEASE);
		__atomic_store_n(&race.head_done, false, __ATOMIC_RELEASE);
		tag = send_stage(&r, context, race.target[0], handle, 0, SEND_CANCEL_LENGTH);
		enter(&r, 0);
		require(!peek(&r, &event), "head is pending before follower admission");
		barrier_wait(&race.begin);
		barrier_wait(&race.admitted);
		if (i % 2) {
			cancel.addr = tag;
			stage(&r, cancel);
			event = wait_tag(&r, cancel.user_data);
			require(!event.res || event.res == -ENOENT || event.res == -EALREADY,
				"race head cancellation");
		}
		event = wait_tag(&r, tag);
		require((event.res >= 0 && event.res <= SEND_CANCEL_LENGTH) ||
			event.res == -ECANCELED, "race head progress");
		progress = event.res > 0 ? event.res : 0;
		__atomic_store_n(&race.head_done, true, __ATOMIC_RELEASE);
		barrier_wait(&race.finished);
		if (progress < SEND_CANCEL_LENGTH) {
			partial++;
			require(race.follower.res == -ECANCELED, "broken head emits no follower");
		} else {
			complete++;
		}
		canceled += race.follower.res == -ECANCELED;
		sent += race.follower.res == 8;
		__atomic_store_n(&race.expected, progress + (race.follower.res > 0 ? 8 : 0),
				 __ATOMIC_RELEASE);
		barrier_wait(&race.drained);
		for (j = 0; j < race.expected; j++)
			require(race.wire[j] == (j < (unsigned int)progress ? 'A' : 'B'),
				"race wire ordering");
		require(command(&r, context, IORING_OPAQUE_READ_OBJECT,
				race.handle, 0, 1, &byte).res == -ESTALE,
			"race LAST handle remains consumed");
		enter(&r, 0);
		require(!r.nr_saved && !peek(&r, &event), "no duplicate head CQE");
		if (!((i + 1) % 100))
			ksft_print_msg("SEND cancellation races completed: %u\n", i + 1);
	}
	pthread_join(follower, NULL);
	pthread_join(drain, NULL);
	require(complete && partial && canceled && sent && race.expired,
		"all race paths exercised");
	ksft_print_msg("SEND races: full=%u partial=%u canceled=%u sent=%u expired=%u\n",
		       complete, partial, canceled, sent, race.expired);
	close(race.store_fd);
	require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		!stat.backing_bytes && !stat.extents && !stat.objects && !stat.streams,
		"race references drain");
	ring_exit(&r);
	close(source[0]);
	close(source[1]);
	close(race.target[0]);
	close(race.target[1]);
	pthread_barrier_destroy(&race.begin);
	pthread_barrier_destroy(&race.admitted);
	pthread_barrier_destroy(&race.finished);
	pthread_barrier_destroy(&race.drained);
	free(race.wire);
	free(data);
	require(!pthread_setaffinity_np(pthread_self(), sizeof(original), &original),
		"restore CPU set");
	ksft_test_result_pass("%u cross-ring SEND cancellation/promotion races on SMP\n",
			      (unsigned int)SEND_CANCEL_ROUNDS);
}

struct disconnect_sender {
	int fd;
	const unsigned char *data;
	size_t length;
	ssize_t result;
	bool done;
};

static void *disconnect_send(void *arg)
{
	struct disconnect_sender *tx = arg;

	tx->result = send(tx->fd, tx->data, tx->length, MSG_NOSIGNAL);
	__atomic_store_n(&tx->done, true, __ATOMIC_RELEASE);
	return NULL;
}

static void test_disconnect(struct io_uring_opaque_config *cfg)
{
	int families[] = { AF_INET, AF_INET6 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(families); i++) {
		struct sockaddr addr = { .sa_family = AF_UNSPEC };
		struct sockaddr_storage peer;
		socklen_t peer_length = sizeof(peer);
		const size_t length = 16U << 20;
		unsigned char *data = calloc(1, length);
		struct disconnect_sender tx = { .data = data, .length = length };
		struct transfer rx = { .data = data, .length = length, .repeats = 1 };
		struct ring r;
		pthread_t sender, receiver;
		uint32_t context;
		uint64_t stream, discard;
		int pair[2], alias, queued = 0, attempt, ret, error, small = 4096;

		if (!tcp_pair_family(pair, families[i])) {
			free(data);
			ksft_test_result_skip("IPv6 disconnect ownership requires IPv6 support\n");
			continue;
		}
		require(!ring_init(&r, IORING_SETUP_CQE32), "disconnect ring");
		require(!register_store(&r, cfg, &context), "disconnect store");
		alias = dup(pair[1]);
		require(alias >= 0, "duplicate claimed descriptor");
		stream = attach(&r, context, pair[1], 711);
		require(data && !getpeername(pair[1], (void *)&peer, &peer_length),
			"disconnect sender and peer");
		require(!setsockopt(pair[1], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)),
			"disconnect small send buffer");
		tx.fd = pair[1];
		rx.fd = pair[0];
		require(!pthread_create(&sender, NULL, disconnect_send, &tx),
			"disconnect blocked sender");
		for (attempt = 0; attempt < 100; attempt++) {
			require(!ioctl(pair[1], TIOCOUTQ, &queued), "disconnect queued bytes");
			if (queued)
				break;
			usleep(1000);
		}
		usleep(100000);
		require(queued && !__atomic_load_n(&tx.done, __ATOMIC_ACQUIRE),
			"sender is blocked before disconnect rejection");
		require(connect(alias, &addr, sizeof(addr)) == -1 && errno == EBUSY,
			"AF_UNSPEC cannot reset a claimed TCP connection through an alias");
		require(!pthread_create(&receiver, NULL, reader, &rx), "disconnect drain");
		pthread_join(sender, NULL);
		ret = connect(alias, (void *)&peer, peer_length);
		error = errno;
		require(!shutdown(pair[1], SHUT_WR), "disconnect sender EOF");
		pthread_join(receiver, NULL);
		ksft_print_msg("IPv%d rejected disconnect: send=%zd/%zu, connect errno=%d\n",
			       i ? 6 : 4, tx.result, length, error);
		require(tx.result == (ssize_t)length && rx.ok && ret == -1 && error == EISCONN,
			"rejected disconnect preserves blocked TX and connected state");
		discard = cmd_stage(&r, context, IORING_OPAQUE_DISCARD, stream, 0, 4, NULL);
		write_all(pair[0], "live", 4);
		require(wait_tag(&r, discard).res == 4, "rejected disconnect preserves TCP");
		require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
			"release disconnect claim");
		require(!connect(alias, &addr, sizeof(addr)), "disconnect after STREAM_CLOSE");
		ring_exit(&r);
		close(alias);
		close(pair[0]);
		close(pair[1]);
		free(data);
		ksft_test_result_pass("IPv%d disconnect preserves ownership through aliases\n",
				      i ? 6 : 4);
	}
}

static void test_socket_lifetime(struct io_uring_opaque_config *cfg)
{
	struct io_uring_opaque_stream_stat stat;
	struct timeval timeout = { .tv_sec = 2 };
	struct ring r;
	uint32_t context;
	uint64_t stream;
	unsigned char byte;
	int pair[2];

	require(!ring_init(&r, IORING_SETUP_CQE32), "socket lifetime ring");
	require(!register_store(&r, cfg, &context), "socket lifetime store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], 712);
	close(pair[1]);
	require(recv(pair[0], &byte, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN,
		"closing descriptor leaves the stream's connection alive");
	require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, stream, 0,
			 sizeof(stat), &stat).res, "token survives descriptor close");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"explicit close releases stream ownership");
	require(wait_tag(&r, 712).res == -ECANCELED, "collector terminates after stream close");
	require(!setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)),
		"lifetime peer receive timeout");
	require(recv(pair[0], &byte, 1, 0) == 0, "stream close releases the socket file");
	ring_exit(&r);
	close(pair[0]);
	ksft_test_result_pass("STREAM_CLOSE releases the connection after its descriptor closes\n");
}

static void test_poll_first(struct io_uring_opaque_config *cfg)
{
	struct io_uring_sqe sqe = {
		.opcode = IORING_OP_OPAQUE_COLLECT,
		.ioprio = IORING_RECVSEND_POLL_FIRST,
		.user_data = 713,
	};
	struct ring r;
	uint32_t context;
	struct event event;
	uint64_t stream;
	unsigned char byte;
	int pair[2];

	require(!ring_init(&r, IORING_SETUP_CQE32), "POLL_FIRST ring");
	require(!register_store(&r, cfg, &context), "POLL_FIRST store");
	tcp_pair(pair);
	sqe.fd = pair[1];
	sqe.opaque_store_id = context;
	stage(&r, sqe);
	enter(&r, 0);
	require(!peek(&r, &event), "POLL_FIRST delays attach until readability");
	require(recv(pair[1], &byte, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN,
		"POLL_FIRST has not acquired an empty socket");
	write_all(pair[0], "p", 1);
	event = wait_tag(&r, sqe.user_data);
	require(!event.res && event.flags & IORING_CQE_F_MORE, "POLL_FIRST attach token");
	stream = event.extra[0];
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, 1, &byte).res == 1 &&
		byte == 'p', "POLL_FIRST payload");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"POLL_FIRST close stream");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("POLL_FIRST waits for readability before acquiring the stream\n");
}

static void test_full_cq(struct io_uring_opaque_config *cfg)
{
	struct io_uring_sqe sqe = { .opcode = IORING_OP_NOP };
	struct ring r;
	uint32_t context;
	struct event event;
	uint64_t stream;
	unsigned int i;
	unsigned char bytes[3];
	int pair[2];

	require(!ring_init(&r, IORING_SETUP_CQE32), "full CQ ring");
	require(!register_store(&r, cfg, &context), "full CQ store");
	tcp_pair(pair);
	for (i = 0; i < r.p.cq_entries; i++) {
		sqe.user_data = next_tag++;
		stage(&r, sqe);
		if ((i + 1) % r.p.sq_entries == 0)
			enter(&r, 0);
	}
	enter(&r, 0);
	sqe = (struct io_uring_sqe) {
		.opcode = IORING_OP_OPAQUE_COLLECT, .fd = pair[1],
		.ioprio = 0, .opaque_store_id = context,
		.user_data = 714,
	};
	stage(&r, sqe);
	write_all(pair[0], "cq!", 3);
	enter(&r, 0);
	require(recv(pair[1], bytes, sizeof(bytes), MSG_DONTWAIT) == 3 &&
		!memcmp(bytes, "cq!", 3), "full CQ attach neither consumes nor claims RX");
	for (i = 0; i < r.p.cq_entries; i++)
		require(peek(&r, &event) && !event.res && event.tag != sqe.user_data,
			"drain original full CQ");
	event = wait_tag(&r, sqe.user_data);
	require(event.res == -ENOSPC && !(event.flags & IORING_CQE_F_MORE),
		"full CQ reports attach failure");
	stream = attach(&r, context, pair[1], 715);
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"retry attach after CQ drain");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("full CQ attach preserves TCP bytes and permits retry\n");
}

static uint64_t read_number(const char *path)
{
	unsigned long long value;
	FILE *file = fopen(path, "r");

	require(file && fscanf(file, "%llu", &value) == 1, "read numeric fixture");
	fclose(file);
	return value;
}

static void write_setting(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	require(fd >= 0, "open fixture setting");
	write_all(fd, value, strlen(value));
	close(fd);
}

static const char * const fault_names[] = {
	"require-start", "require-end", "reject-start", "reject-end",
	"stacktrace-depth", "ignore-gfp-wait", "cache-filter", "verbose",
};

struct fault_settings {
	char saved[8][64];
};

static bool symbol_range(const char *name, uint64_t *start, uint64_t *end)
{
	char line[512], symbol[256], type;
	unsigned long long address;
	FILE *file = fopen("/proc/kallsyms", "r");

	if (!file)
		return false;
	*start = 0;
	*end = 0;
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "%llx %c %255s", &address, &type, symbol) != 3)
			continue;
		if (*start && address > *start) {
			*end = address;
			break;
		}
		if (!strncmp(symbol, name, strlen(name)) &&
		    (!symbol[strlen(name)] || symbol[strlen(name)] == '.'))
			*start = address;
	}
	fclose(file);
	return *start && *end;
}

static bool fault_begin(struct fault_settings *settings)
{
	char path[128];
	FILE *file;
	unsigned int i;

	if (geteuid() || access("/sys/kernel/debug/failslab/probability", W_OK) ||
	    access("/proc/self/fail-nth", W_OK) ||
	    read_number("/sys/kernel/debug/failslab/probability"))
		return false;
	for (i = 0; i < ARRAY_SIZE(fault_names); i++) {
		snprintf(path, sizeof(path), "/sys/kernel/debug/failslab/%s", fault_names[i]);
		if (access(path, W_OK))
			return false;
		file = fopen(path, "r");
		require(file && fgets(settings->saved[i], sizeof(settings->saved[i]), file),
			"save fault-injection settings");
		fclose(file);
	}
	write_setting("/sys/kernel/debug/failslab/reject-start", "0");
	write_setting("/sys/kernel/debug/failslab/reject-end", "0");
	write_setting("/sys/kernel/debug/failslab/stacktrace-depth", "32");
	write_setting("/sys/kernel/debug/failslab/ignore-gfp-wait", "0");
	write_setting("/sys/kernel/debug/failslab/cache-filter", "0");
	write_setting("/sys/kernel/debug/failslab/verbose", "0");
	return true;
}

static void fault_arm(uint64_t start, uint64_t end)
{
	char value[32];

	snprintf(value, sizeof(value), "0x%llx", (unsigned long long)start);
	write_setting("/sys/kernel/debug/failslab/require-start", value);
	snprintf(value, sizeof(value), "0x%llx", (unsigned long long)end);
	write_setting("/sys/kernel/debug/failslab/require-end", value);
	write_setting("/proc/self/fail-nth", "1");
}

static void fault_end(struct fault_settings *settings)
{
	char path[128];
	unsigned int i;
	bool injected;

	/* A zero counter proves the requested allocation actually failed. */
	injected = read_number("/proc/self/fail-nth") == 0;
	write_setting("/proc/self/fail-nth", "0");
	for (i = 0; i < ARRAY_SIZE(fault_names); i++) {
		snprintf(path, sizeof(path), "/sys/kernel/debug/failslab/%s", fault_names[i]);
		write_setting(path, settings->saved[i]);
	}
	require(injected, "targeted allocation fault was injected");
}

static void test_allocation_failure(struct io_uring_opaque_config *cfg)
{
	static const char * const names[] = { "io_opaque_capture", "io_opaque_ready" };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct io_uring_opaque_stream_stat stat;
		struct fault_settings settings;
		unsigned char payload[2048], readback[2048];
		struct ring r;
		struct event event;
		uint32_t context;
		uint64_t start, end, stream, keep, handle;
		unsigned int attempts;
		int pair[2], other[2];

		if (!symbol_range(names[i], &start, &end) || !fault_begin(&settings)) {
			ksft_test_result_skip("%s ENOMEM needs failslab and stack filters\n",
					      names[i]);
			continue;
		}
		memset(payload, 0x61 + i, sizeof(payload));
		require(!ring_init(&r, IORING_SETUP_CQE32), "allocation-failure ring");
		require(!register_store(&r, cfg, &context), "allocation-failure store");
		tcp_pair(pair);
		tcp_pair(other);
		stream = attach(&r, context, pair[1], 716);
		keep = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0,
				 sizeof(payload), NULL);
		write_all(pair[0], payload, sizeof(payload) / 2);
		for (attempts = 0; attempts < 5000; attempts++) {
			require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, stream, 0,
					 sizeof(stat), &stat).res, "allocation prefix statistics");
			if (stat.rx_next == sizeof(payload) / 2)
				break;
			usleep(1000);
		}
		require(attempts < 5000, "private prefix is captured before allocation failure");
		/* Force a new extent rather than an allocation-free contiguous merge. */
		write_all(other[0], payload, sizeof(payload) / 2);
		fault_arm(start, end);
		write_all(pair[0], payload + sizeof(payload) / 2, sizeof(payload) / 2);
		enter(&r, 0);
		if (!i) {
			for (attempts = 0; attempts < 5000; attempts++) {
				if (!read_number("/proc/self/fail-nth"))
					break;
				enter(&r, 0);
				usleep(1000);
			}
			fault_end(&settings);
			require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, stream, 0,
					 sizeof(stat), &stat).res && !stat.error,
				"capture ENOMEM does not poison the stream");
			event = wait_tag(&r, keep);
		} else {
			event = wait_tag(&r, keep);
			fault_end(&settings);
			require(event.res == -ENOMEM, "vector allocation rejects publication");
			require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0,
					sizeof(payload), readback).res == sizeof(payload) &&
				!memcmp(payload, readback, sizeof(payload)),
				"vector ENOMEM restores the entire prefix");
			event = command(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0,
					sizeof(payload), NULL);
		}
		require(event.res == sizeof(payload) && event.extra[0],
			"retry publishes complete KEEP");
		handle = event.extra[0];
		require(command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0,
				sizeof(payload), readback).res == sizeof(payload) &&
			!memcmp(payload, readback, sizeof(payload)),
			"allocation retry preserves bytes");
		require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res,
			"free allocation-failure object");
		require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
			"close allocation-failure stream");
		ring_exit(&r);
		close(pair[0]);
		close(pair[1]);
		close(other[0]);
		close(other[1]);
		ksft_test_result_pass("%s ENOMEM preserves bytes and permits KEEP retry\n",
				      names[i]);
	}
}

static void test_memcg(struct io_uring_opaque_config *cfg)
{
	const char *root = "/sys/fs/cgroup";
	char directory[128], path[160], pid[32];
	unsigned char *payload;
	int pair[2], signal[2], status;
	pid_t child;

	snprintf(directory, sizeof(directory), "%s/opaque-test-%d", root, getpid());
	if (geteuid() || mkdir(directory, 0755)) {
		ksft_test_result_skip("retained RX memcg test needs a writable cgroup2 fixture\n");
		return;
	}
	snprintf(path, sizeof(path), "%s/memory.current", directory);
	if (access(path, R_OK)) {
		rmdir(directory);
		ksft_test_result_skip("memory controller is not enabled in the fixture\n");
		return;
	}
	/* Both TCP sockets and the sending pages belong to the parent cgroup. */
	tcp_pair(pair);
	require(!pipe(signal), "memcg synchronization pipe");
	payload = malloc(4U << 20);
	require(payload, "memcg parent payload");
	memset(payload, 0x57, 4U << 20);
	fflush(stdout);
	child = fork();
	require(child >= 0, "memcg fixture fork");
	if (!child) {
		struct io_uring_opaque_object_stat object;
		struct ring r;
		struct event event;
		uint32_t context;
		uint64_t stream, keep, before, after, released;

		close(signal[0]);
		snprintf(path, sizeof(path), "%s/cgroup.procs", directory);
		snprintf(pid, sizeof(pid), "%d", getpid());
		write_setting(path, pid);
		require(!ring_init(&r, IORING_SETUP_CQE32), "memcg ring");
		require(!register_store(&r, cfg, &context), "memcg store");
		stream = attach(&r, context, pair[1], 710);
		keep = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, 4U << 20, NULL);
		enter(&r, 0);
		snprintf(path, sizeof(path), "%s/memory.current", directory);
		before = read_number(path);
		write_all(signal[1], "!", 1);
		event = wait_tag(&r, keep);
		require(event.res == 4U << 20, "memcg retained object");
		require(!command(&r, context, IORING_OPAQUE_OBJECT_STAT, event.extra[0], 0,
				 sizeof(object), &object).res, "memcg object statistics");
		after = read_number(path);
		ksft_print_msg("memcg before=%llu after=%llu backing=%llu\n",
			       (unsigned long long)before, (unsigned long long)after,
			       (unsigned long long)object.backing_bytes);
		/* Per-CPU precharged stocks make memory.current a batched counter. */
		require(after + (1U << 20) >= before + object.backing_bytes,
			"retained pages are charged to the store owner's memcg");
		require(!command(&r, context, IORING_OPAQUE_FREE, event.extra[0], 0, 0, NULL).res,
			"memcg free backing");
		released = read_number(path);
		require(released + object.backing_bytes <= after + (1U << 20),
			"FREE releases retained memcg charges");
		require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
			"memcg close stream");
		ring_exit(&r);
		_exit(0);
	}
	close(signal[1]);
	read_all(signal[0], pid, 1);
	write_all(pair[0], payload, 4U << 20);
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status),
		"retained RX memory-cgroup accounting");
	free(payload);
	close(signal[0]);
	close(pair[0]);
	close(pair[1]);
	require(!rmdir(directory), "remove memcg fixture");
	ksft_test_result_pass("retained RX pages charge the store owner and uncharge on FREE\n");
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
	tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, cursor, 8, NULL);
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
			event = command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0, 8, buf);
			require(event.res == 8 && !memcmp(buf, "abcdefgh", 8),
				"rejected SEND_LAST preserves the object");
		}
	}
	close(udp);
	ksft_test_result_pass("SEND_LAST validation preserves ownership\n");
	{
		struct io_uring_sqe head = {
			.opcode = IORING_OP_OPAQUE_OBJ, .fd = -1,
			.ioprio = IORING_OPAQUE_INSPECT, .flags = IOSQE_IO_LINK,
			.opaque_store_id = context, .addr = stream, .off = cursor,
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
		event = command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0, 8, buf);
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
	require(command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0, 1, buf).res == -ESTALE,
		"SEND_LAST consumes the entire object");
	require(command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res == -ESTALE,
		"no FREE required after SEND_LAST");
	ksft_test_result_pass("cached resends and consuming ranges share one send ABI\n");
	{
		int store_fd = export_store(&r, context);
		struct ring *rings[2] = { &r, &imported };
		uint32_t contexts[2];
		uint64_t heads[2], sends[2];

		require(store_fd >= 0, "SEND_LAST export");
		require(!ring_init(&imported, IORING_SETUP_CQE32), "SEND_LAST import ring");
		require(!import_store(&imported, store_fd, &imported_context), "SEND_LAST import");
		close(store_fd);
		contexts[0] = context;
		contexts[1] = imported_context;
		tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, cursor, 8, NULL);
		write_all(source[0], "abcdefgh", 8);
		event = wait_tag(&r, tag);
		require(event.res == 8, "competing SEND_LAST object");
		handle = event.extra[0];
		cursor += 8;
		/* Prepare both LAST requests before either can take the handle. */
		for (i = 0; i < 2; i++) {
			struct io_uring_sqe head = {
				.opcode = IORING_OP_OPAQUE_OBJ, .fd = -1,
				.ioprio = IORING_OPAQUE_INSPECT, .flags = IOSQE_IO_LINK,
				.opaque_store_id = contexts[i], .addr = stream, .off = cursor,
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
		event = command(&imported, imported_context, IORING_OPAQUE_READ_OBJECT,
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

			tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT,
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
				require(command(&r, context, IORING_OPAQUE_READ_OBJECT,
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
	tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, cursor, 8, NULL);
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
	require(command(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0,
			cfg.hard_limit + 1, NULL).res == -EMSGSIZE,
		"reject KEEP larger than the entire capture allowance");
	for (i = 0; i < 3; i++) {
		ksft_print_msg("budget capture %d of 3\n", i + 1);
		pending = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, i, 1, NULL);
		write_all(pair[0], &byte, 1);
		event = wait_tag(&r, pending);
		require(event.res == 1, "fill store budget");
		objects[i] = event.extra[0];
	}
	require(command(&r, context, IORING_OPAQUE_COMPACT, objects[0], 0, 0, NULL).res == -ENOBUFS,
		"source and destination must both fit");
	ksft_print_msg("budget filled; verifying release and discard\n");
	pending = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 3, 1, NULL);
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

static void wait_unread(int fd, unsigned int length)
{
	unsigned int attempt;
	int unread;

	for (attempt = 0; attempt < 5000; attempt++) {
		require(!ioctl(fd, FIONREAD, &unread), "TCP unread count");
		if (unread == (int)length)
			return;
		usleep(1000);
	}
	require(false, "unadmitted bytes remain on TCP");
}

static void test_budget_notifications(struct io_uring_opaque_config cfg)
{
	static const char request[] = "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n";
	static const unsigned int flags[] = { IORING_SETUP_CQE32, IORING_SETUP_CQE_MIXED };
	unsigned int i;

	cfg.hard_limit = 2 * sysconf(_SC_PAGESIZE);
	cfg.compact_headroom = sysconf(_SC_PAGESIZE);
	for (i = 0; i < ARRAY_SIZE(flags); i++) {
		struct ring r, release;
		struct io_uring_opaque_stream_stat stream_stat;
		struct io_uring_opaque_stat stat;
		struct event event;
		uint32_t context, released_context;
		uint64_t cached_stream, stream, handle, tag, collector;
		unsigned char seen[512];
		unsigned int attempt, length = sizeof(request) - 1;
		int cached[2], source[2], store_fd;

		require(!ring_init(&r, flags[i]), "pressure notification ring");
		require(!ring_init(&release, IORING_SETUP_CQE32), "pressure release ring");
		require(!register_store(&r, &cfg, &context), "pressure notification store");
		store_fd = export_store(&r, context);
		require(store_fd >= 0 && !import_store(&release, store_fd, &released_context),
			"pressure release shared store");
		close(store_fd);
		tcp_pair(cached);
		tcp_pair(source);
		cached_stream = attach(&r, context, cached[1], next_tag++);
		tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT,
				cached_stream, 0, 8, NULL);
		write_all(cached[0], "quota123", 8);
		event = wait_tag(&r, tag);
		require(event.res == 8, "pressure cached object");
		handle = event.extra[0];
		collector = next_tag++;
		stream = attach(&r, context, source[1], collector);
		tag = cmd_stage(&r, context, IORING_OPAQUE_INSPECT,
				stream, 0, sizeof(seen), seen);
		write_all(source[0], request, length);
		event = wait_tag(&r, collector);
		require(event.res == -ENOBUFS && (event.flags & IORING_CQE_F_MORE) &&
			event.extra[0] == stream && !event.extra[1],
			"quota CQE identifies the live stream and unread offset");
		if (flags[i] == IORING_SETUP_CQE_MIXED)
			require(event.flags & IORING_CQE_F_32, "pressure CQE carries mixed extras");
		wait_unread(source[1], length);
		for (attempt = 0; attempt < 8; attempt++) {
			write_all(source[0], "x", 1);
			wait_unread(source[1], length + attempt + 1);
			require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, stream, 0,
					 sizeof(stream_stat), &stream_stat).res &&
				!stream_stat.rx_next && !stream_stat.undecided_bytes &&
				!stream_stat.error && !r.nr_saved,
				"repeated arrivals do not consume bytes or duplicate quota CQEs");
		}
		length += attempt;
		require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
			stat.backing_bytes == cfg.hard_limit - cfg.compact_headroom,
			"pressure stays within capture allowance");
		require(!command(&release, released_context, IORING_OPAQUE_FREE,
				 handle, 0, 0, NULL).res, "another ring releases receive capacity");
		event = wait_tag(&r, tag);
		require(event.res == (int)length && !memcmp(seen, request, sizeof(request) - 1),
			"release resumes pending inspection without rearming collection");
		for (attempt = sizeof(request) - 1; attempt < length; attempt++)
			require(seen[attempt] == 'x', "resumed receive preserves queued bytes");
		require(command(&r, context, IORING_OPAQUE_DISCARD,
				stream, 0, length, NULL).res == (int)length,
			"discard inspected request");
		tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT,
				cached_stream, 8, 8, NULL);
		write_all(cached[0], "quota456", 8);
		event = wait_tag(&r, tag);
		require(event.res == 8, "refill after pressure recovery");
		handle = event.extra[0];
		tag = cmd_stage(&r, context, IORING_OPAQUE_INSPECT,
				stream, length, sizeof(seen), seen);
		write_all(source[0], request, sizeof(request) - 1);
		event = wait_tag(&r, collector);
		require(event.res == -ENOBUFS && (event.flags & IORING_CQE_F_MORE) &&
			event.extra[0] == stream && event.extra[1] == length,
			"new unread offset reports a new quota episode");
		wait_unread(source[1], sizeof(request) - 1);
		require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE,
				 stream, 0, 0, NULL).res, "close a quota-paused stream");
		require(wait_tag(&r, tag).res == -ECANCELED, "close cancels paused inspection");
		event = wait_tag(&r, collector);
		require(event.res == -ECANCELED && !(event.flags & IORING_CQE_F_MORE),
			"quota-paused collector has one terminal completion");
		read_all(source[1], seen, sizeof(request) - 1);
		require(!memcmp(seen, request, sizeof(request) - 1),
			"closing the claim exposes bytes never taken from TCP");
		require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res &&
			!command(&r, context, IORING_OPAQUE_STREAM_CLOSE,
				 cached_stream, 0, 0, NULL).res, "pressure fixture teardown");
		require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
			!stat.backing_bytes && !stat.extents && !stat.objects && !stat.streams,
			"pressure recovery drains backing and metadata");
		ring_exit(&release);
		ring_exit(&r);
		close(cached[0]);
		close(cached[1]);
		close(source[0]);
		close(source[1]);
		ksft_test_result_pass("%s quota CQE retains TCP bytes and resumes after FREE\n",
				      i ? "mixed" : "32-byte");
	}
}

static void test_budget_full_cq(struct io_uring_opaque_config cfg)
{
	struct io_uring_sqe nop = { .opcode = IORING_OP_NOP };
	struct io_uring_opaque_stream_stat stream_stat;
	struct io_uring_opaque_stat stat;
	struct ring r;
	struct event event;
	uint32_t context;
	uint64_t cached_stream, stream, handle, tag, collector;
	unsigned char seen[3];
	unsigned int i;
	int cached[2], source[2];

	cfg.hard_limit = sysconf(_SC_PAGESIZE);
	cfg.compact_headroom = 0;
	require(!ring_init(&r, IORING_SETUP_CQE32), "pressure full CQ ring");
	require(!register_store(&r, &cfg, &context), "pressure full CQ store");
	tcp_pair(cached);
	tcp_pair(source);
	cached_stream = attach(&r, context, cached[1], next_tag++);
	tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, cached_stream, 0, 8, NULL);
	write_all(cached[0], "quota123", 8);
	event = wait_tag(&r, tag);
	require(event.res == 8, "pressure full CQ cached object");
	handle = event.extra[0];
	collector = next_tag++;
	stream = attach(&r, context, source[1], collector);
	for (i = 0; i < r.p.cq_entries; i++) {
		nop.user_data = next_tag++;
		stage(&r, nop);
		if ((i + 1) % r.p.sq_entries == 0)
			enter(&r, 0);
	}
	enter(&r, 0);
	write_all(source[0], "cq!", sizeof(seen));
	wait_unread(source[1], sizeof(seen));
	enter(&r, 0);
	for (i = 0; i < r.p.cq_entries; i++)
		require(peek(&r, &event) && !event.res && event.tag != collector,
			"drain pressure notification full CQ");
	event = wait_tag(&r, collector);
	require(event.res == -ENOSPC && !(event.flags & IORING_CQE_F_MORE),
		"pressure CQ overflow ends collection explicitly");
	require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, stream, 0,
			 sizeof(stream_stat), &stream_stat).res && !stream_stat.rx_next &&
		!stream_stat.undecided_bytes && stream_stat.error == -ENOSPC,
		"pressure CQ overflow preserves the token without consuming TCP");
	wait_unread(source[1], sizeof(seen));
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"release pressure CQ overflow claim");
	read_all(source[1], seen, sizeof(seen));
	require(!memcmp(seen, "cq!", sizeof(seen)), "CQ overflow leaves socket bytes intact");
	require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, cached_stream, 0, 0, NULL).res,
		"pressure full CQ fixture teardown");
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		!stat.backing_bytes && !stat.extents && !stat.objects && !stat.streams,
		"pressure full CQ drains backing and metadata");
	ring_exit(&r);
	close(cached[0]);
	close(cached[1]);
	close(source[0]);
	close(source[1]);
	ksft_test_result_pass("full CQ quota failure preserves the token and unread TCP bytes\n");
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
	unsigned char buf[3];
	int pair[2];

	require(!ring_init(&r, IORING_SETUP_CQE32), "EOF ring");
	require(!register_store(&r, cfg, &context), "EOF store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], 400);
	pending = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, 8, NULL);
	write_all(pair[0], "abc", 3);
	require(!shutdown(pair[0], SHUT_WR), "source EOF");
	require(wait_tag(&r, pending).res == -ENODATA, "EOF rejects incomplete object");
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res,
		"EOF store stat");
	require(!stat.objects && stat.backing_bytes, "EOF preserves unpublished prefix");
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, 3, buf).res == 3 &&
		!memcmp(buf, "abc", 3), "EOF prefix remains readable");
	require(command(&r, context, IORING_OPAQUE_DISCARD, stream, 0, 3, NULL).res == 3,
		"EOF prefix can be discarded");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"EOF stream close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("EOF restores private assembly without publishing an object\n");
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
	pending = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, 8, NULL);
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
	cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, 1024, NULL);
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

static int register_request(struct ring *r, struct io_uring_opaque_register *reg)
{
	int ret = syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_OPAQUE_STORE, reg, 1);

	return ret < 0 ? -errno : ret;
}

static void test_registration_abi(struct io_uring_opaque_config cfg)
{
	struct {
		struct io_uring_opaque_config config;
		uint64_t extra[2];
	} cfg_ext = { .config = cfg };
	struct {
		struct io_uring_opaque_register reg;
		uint64_t extra[2];
	} reg_ext = {};
	struct {
		struct io_uring_opaque_stat stat;
		uint64_t extra[2];
	} stat_ext;
	struct {
		struct io_uring_opaque_stream_stat stat;
		uint64_t extra;
	} stream_ext;
	struct {
		struct io_uring_opaque_object_stat stat;
		uint64_t extra;
	} object_ext;
	struct {
		struct io_uring_opaque_policy policy;
		uint64_t extra;
	} policy_ext = {};
	struct { uint32_t size, guard; } prefix = { .guard = 0x12345678 };
	struct io_uring_opaque_register create, reg;
	struct io_uring_opaque_register *readonly;
	struct io_uring_sqe collect = { .opcode = IORING_OP_OPAQUE_COLLECT };
	struct ring r, imported;
	struct event event;
	uint32_t context, other, last;
	uint64_t stream, handle, tag;
	unsigned char bytes[3];
	int pair[2], fd;
	size_t page_size = sysconf(_SC_PAGESIZE);

	cfg.hard_limit = 16 * page_size;
	cfg.compact_headroom = 4 * page_size;
	create = (struct io_uring_opaque_register) {
		.size = sizeof(create), .fd = -1,
		.config = (uintptr_t)&cfg, .config_size = sizeof(cfg),
	};
	require(!ring_init(&r, IORING_SETUP_CQE32), "versioned registration ring");
	reg = create;
	reg.size = 32;
	reg.config_size = 40;
	reg.__resv[0] = 0x12345678;
	require(!register_request(&r, &reg) && reg.size == sizeof(reg) &&
		reg.__resv[0] == 0x12345678, "short registration does not overwrite its tail");
	context = reg.store_id;
	ksft_test_result_pass("registration prefixes preserve caller buffer bounds\n");

	cfg_ext.config = cfg;
	reg_ext.reg = create;
	reg_ext.reg.size = sizeof(reg_ext);
	reg_ext.reg.config = (uintptr_t)&cfg_ext;
	reg_ext.reg.config_size = sizeof(cfg_ext);
	require(!register_request(&r, &reg_ext.reg) &&
		reg_ext.reg.size == sizeof(reg_ext.reg) && !reg_ext.extra[0] && !reg_ext.extra[1],
		"zero registration/configuration extensions");
	last = reg_ext.reg.store_id;
	reg_ext.reg.store_id = 0;
	cfg_ext.extra[0] = 1;
	require(register_request(&r, &reg_ext.reg) == -E2BIG, "unknown configuration extension");
	cfg_ext.extra[0] = 0;
	reg_ext.reg.size = sizeof(reg_ext);
	reg_ext.extra[0] = 1;
	require(register_request(&r, &reg_ext.reg) == -E2BIG, "unknown registration extension");
	ksft_test_result_pass("zero input extensions work and nonzero unknown extensions fail\n");

	reg = create;
	reg.flags = 1U << 31;
	require(register_request(&r, &reg) == -EINVAL, "unknown registration flag");
	reg.flags = IORING_OPAQUE_REG_IMPORT | IORING_OPAQUE_REG_EXPORT;
	require(register_request(&r, &reg) == -EINVAL, "conflicting registration flags");
	reg = create;
	reg.__resv0 = 1;
	require(register_request(&r, &reg) == -EINVAL, "registration reserved field");
	reg = create;
	cfg.flags = 1;
	require(register_request(&r, &reg) == -EINVAL, "configuration flags");
	cfg.flags = 0;
	cfg.__resv[0] = 1;
	require(register_request(&r, &reg) == -EINVAL, "configuration reserved field");
	cfg.__resv[0] = 0;
	reg.config = 1;
	require(register_request(&r, &reg) == -EFAULT, "invalid configuration pointer");
	ksft_test_result_pass("registration rejects invalid flags, reserved fields and pointers\n");

	readonly = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	require(readonly != MAP_FAILED, "registration rollback mapping");
	*readonly = create;
	require(!mprotect(readonly, page_size, PROT_READ), "read-only registration output");
	require(register_request(&r, readonly) == -EFAULT, "creation copyout rollback");
	reg = create;
	require(!register_request(&r, &reg) && reg.store_id == last + 1,
		"failed copyout releases the allocated registration ID");
	ksft_test_result_pass("creation copyout faults release registration and quota\n");

	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(prefix.size), &prefix).res &&
		prefix.size == sizeof(stat_ext.stat) && prefix.guard == 0x12345678,
		"short store statistics");
	memset(&stat_ext, 0xa5, sizeof(stat_ext));
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat_ext), &stat_ext).res &&
		stat_ext.stat.size == sizeof(stat_ext.stat) && !stat_ext.extra[0] &&
		!stat_ext.extra[1] && stat_ext.stat.hard_limit == cfg.hard_limit,
		"extended store statistics");
	require(command(&r, context, IORING_OPAQUE_STAT, 0, 0, 3, &prefix).res == -EINVAL,
		"statistics require a size word");
	require(command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat_ext),
			(void *)1).res == -EFAULT, "statistics output fault");
	ksft_test_result_pass("statistics report sizes and respect output bounds\n");

	tcp_pair(pair);
	collect.fd = pair[1];
	collect.opaque_store_id = context;
	collect.ioprio = IORING_RECV_MULTISHOT;
	collect.user_data = next_tag++;
	stage(&r, collect);
	require(wait_tag(&r, collect.user_data).res == -EINVAL,
		"collector rejects the unrelated RECV_MULTISHOT flag");
	collect.ioprio = 0;
	collect.len = 1;
	collect.user_data = next_tag++;
	stage(&r, collect);
	require(wait_tag(&r, collect.user_data).res == -EINVAL, "collector rejects payload length");
	stream = attach(&r, context, pair[1], next_tag++);
	memset(&stream_ext, 0xa5, sizeof(stream_ext));
	require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, stream, 0,
			 sizeof(stream_ext), &stream_ext).res &&
		stream_ext.stat.size == sizeof(stream_ext.stat) && !stream_ext.extra,
		"extended stream statistics");
	tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, 3, NULL);
	write_all(pair[0], "abi", 3);
	event = wait_tag(&r, tag);
	require(event.res == 3, "versioned statistics object");
	handle = event.extra[0];
	memset(&object_ext, 0xa5, sizeof(object_ext));
	require(!command(&r, context, IORING_OPAQUE_OBJECT_STAT, handle, 0,
			 sizeof(object_ext), &object_ext).res &&
		object_ext.stat.size == sizeof(object_ext.stat) && !object_ext.extra &&
		object_ext.stat.length == 3, "extended object statistics");
	ksft_test_result_pass("dedicated collection and versioned object/stream queries\n");

	policy_ext.policy = (struct io_uring_opaque_policy) {
		.flags = IORING_OPAQUE_POLICY_F_AUTO_COMPACT,
		.bytes_per_second = cfg.hard_limit, .burst_bytes = cfg.hard_limit,
		.max_temporary_bytes = cfg.compact_headroom,
	};
	require(!command(&r, context, IORING_OPAQUE_SET_POLICY, 0, 0, 32, &policy_ext).res,
		"compaction policy prefix");
	require(!command(&r, context, IORING_OPAQUE_SET_POLICY, 0, 0,
			 sizeof(policy_ext), &policy_ext).res, "compaction policy zero extension");
	policy_ext.extra = 1;
	require(command(&r, context, IORING_OPAQUE_SET_POLICY, 0, 0,
			sizeof(policy_ext), &policy_ext).res == -E2BIG,
		"compaction policy unknown extension");
	policy_ext.extra = 0;
	policy_ext.policy.__resv0 = 1;
	require(command(&r, context, IORING_OPAQUE_SET_POLICY, 0, 0,
			sizeof(policy_ext), &policy_ext).res == -EINVAL,
		"compaction policy reserved field");
	policy_ext.policy.__resv0 = 0;
	policy_ext.policy.bytes_per_second = 0;
	require(command(&r, context, IORING_OPAQUE_SET_POLICY, 0, 0,
			sizeof(policy_ext), &policy_ext).res == -EINVAL,
		"compaction policy missing budget");
	policy_ext.policy.flags = 0;
	require(!command(&r, context, IORING_OPAQUE_SET_POLICY, 0, 0,
			 sizeof(policy_ext), &policy_ext).res,
		"disable versioned compaction policy");
	ksft_test_result_pass("automatic compaction uses versioned copy budgets\n");

	fd = export_store(&r, context);
	require(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC), "opaque export close-on-exec");
	require(import_store(&r, pair[0], &other) == -EBADF, "reject non-store import descriptor");
	require(!mprotect(readonly, page_size, PROT_READ | PROT_WRITE), "export fault setup");
	*readonly = (struct io_uring_opaque_register) {
		.size = sizeof(*readonly), .flags = IORING_OPAQUE_REG_EXPORT,
		.fd = -1, .store_id = context,
	};
	require(!mprotect(readonly, page_size, PROT_READ), "export fault output");
	require(register_request(&r, readonly) == -EFAULT, "export copyout rollback");
	require(!mprotect(readonly, page_size, PROT_READ | PROT_WRITE), "import fault setup");
	*readonly = (struct io_uring_opaque_register) {
		.size = sizeof(*readonly), .flags = IORING_OPAQUE_REG_IMPORT, .fd = fd,
	};
	require(!mprotect(readonly, page_size, PROT_READ), "import fault output");
	require(register_request(&r, readonly) == -EFAULT, "import copyout rollback");
	munmap(readonly, page_size);
	ksft_test_result_pass("store FD validation and export/import copyout rollback\n");

	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"export-only lifetime stream close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	require(!ring_init(&imported, IORING_SETUP_CQE32), "descriptor-only import ring");
	require(!import_store(&imported, fd, &context), "import after the creating ring closes");
	close(fd);
	require(command(&imported, context, IORING_OPAQUE_READ_OBJECT, handle, 0, 3,
			bytes).res == 3 &&
		!memcmp(bytes, "abi", 3), "export descriptor preserves object backing");
	ksft_test_result_pass("exported FD preserves the store after creator ring exit\n");

	fd = export_store(&imported, context);
	require(fd >= 0 && !import_store(&imported, fd, &other) && other != context,
		"two registrations of the same store");
	close(fd);
	require(!command(&imported, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res,
		"free through one registration");
	require(command(&imported, other, IORING_OPAQUE_READ_OBJECT, handle, 0, 3,
			bytes).res == -ESTALE,
		"shared handle invalidation through duplicate registration");
	require(!command(&imported, other, IORING_OPAQUE_STAT, 0, 0,
			 sizeof(stat_ext.stat), &stat_ext.stat).res &&
		!stat_ext.stat.objects && !stat_ext.stat.streams && !stat_ext.stat.backing_bytes,
		"constant-time counters drain through shared registration");
	ring_exit(&imported);
	ksft_test_result_pass("duplicate registrations share invalidation and drain references\n");
}

static void test_async_objects(struct io_uring_opaque_config *cfg)
{
	static const char get[] = "*2\r\n$3\r\nGET\r\n$3\r\nhot\r\n";
	struct ring r;
	struct event event;
	struct io_uring_opaque_stat stat;
	unsigned char buf[512];
	uint32_t context;
	uint64_t stream, inspect, object, following, handle, cursor = sizeof(get) - 1;
	int pair[2], i;

	require(!ring_init(&r, IORING_SETUP_CQE32), "async object ring");
	require(!register_store(&r, cfg, &context), "async object store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	memset(buf, 0xa5, sizeof(buf));
	inspect = cmd_stage(&r, context, IORING_OPAQUE_INSPECT, stream, 0, sizeof(buf), buf);
	write_all(pair[0], get, sizeof(get) - 1);
	event = wait_tag(&r, inspect);
	require(event.res == (int)sizeof(get) - 1 && !event.extra[0] &&
		!memcmp(buf, get, sizeof(get) - 1) && buf[sizeof(get) - 1] == 0xa5,
		"short GET completes with actual bytes and no object handle");
	require(command(&r, context, IORING_OPAQUE_DISCARD, stream, 0, cursor,
			NULL).res == (int)cursor,
		"inspection preserves short GET");
	ksft_test_result_pass("short GET completes in a larger inspection window\n");

	for (i = 0; i < 128; i++, cursor++) {
		unsigned char byte = i;

		if (i & 1)
			write_all(pair[0], &byte, 1);
		inspect = cmd_stage(&r, context, IORING_OPAQUE_INSPECT, stream,
				    cursor, sizeof(buf), buf);
		enter(&r, 0);
		if (!(i & 1))
			write_all(pair[0], &byte, 1);
		event = wait_tag(&r, inspect);
		require(event.res == 1 && buf[0] == byte, "arrival versus inspection arming");
		require(command(&r, context, IORING_OPAQUE_DISCARD,
				stream, cursor, 1, NULL).res == 1, "armed inspection drain");
	}
	ksft_test_result_pass("inspection handles arrival before and after arming\n");

	inspect = cmd_stage(&r, context, IORING_OPAQUE_INSPECT, stream, cursor, sizeof(buf), buf);
	object = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, cursor + 4, 8, NULL);
	following = cmd_stage(&r, context, IORING_OPAQUE_INSPECT,
			      stream, cursor + 12, sizeof(buf) - 32, buf + 32);
	enter(&r, 0);
	write_all(pair[0], "HDR:abc", 7);
	event = wait_tag(&r, inspect);
	require(event.res >= 4 && !memcmp(buf, "HDR:", 4),
		"inspection capacity does not reserve the future object range");
	enter(&r, 0);
	require(!peek(&r, &event), "declared object and next header remain asynchronous");
	write_all(pair[0], "defghGET\r\n", 10);
	event = wait_tag(&r, object);
	require(event.res == 8 && event.extra[0], "one complete object-ready CQE");
	handle = event.extra[0];
	event = wait_tag(&r, following);
	require(event.res == 5 && !memcmp(buf + 32, "GET\r\n", 5), "next header at known offset");
	require(command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0, 8, buf).res == 8 &&
		!memcmp(buf, "abcdefgh", 8), "READ_OBJECT uses complete immutable bytes");
	require(command(&r, context, IORING_OPAQUE_INSPECT,
			stream, cursor + 4, sizeof(buf), buf).res == -ENODATA,
		"consumed object range is not stream framing");
	require(command(&r, context, IORING_OPAQUE_DISCARD, stream, cursor, 4, NULL).res == 4 &&
		command(&r, context, IORING_OPAQUE_DISCARD, stream, cursor + 12, 5,
			NULL).res == 5 &&
		!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res,
		"async object fixture retirement");
	cursor += 17;
	ksft_test_result_pass("header capacity, declared object and following header coexist\n");

	inspect = cmd_stage(&r, context, IORING_OPAQUE_INSPECT, stream, cursor, sizeof(buf), buf);
	enter(&r, 0);
	{
		struct io_uring_sqe cancel = {
			.opcode = IORING_OP_ASYNC_CANCEL, .addr = inspect, .user_data = next_tag++,
		};

		stage(&r, cancel);
		require(!wait_tag(&r, cancel.user_data).res &&
			wait_tag(&r, inspect).res == -ECANCELED, "cancel unavailable inspection");
	}
	write_all(pair[0], "END", 3);
	require(!shutdown(pair[0], SHUT_WR), "inspection EOF");
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, cursor,
			sizeof(buf), buf).res == 3 && !memcmp(buf, "END", 3),
		"buffered framing returns before EOF");
	require(!command(&r, context, IORING_OPAQUE_INSPECT,
			 stream, cursor + 3, sizeof(buf), buf).res, "inspection EOF returns zero");
	require(command(&r, context, IORING_OPAQUE_DISCARD, stream, cursor, 3, NULL).res == 3 &&
		!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"inspection EOF teardown");
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		!stat.backing_bytes && !stat.extents && !stat.objects && !stat.streams,
		"inspection references drain");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("unavailable inspection cancels and EOF preserves final bytes\n");
}

static void test_framed_sends(struct io_uring_opaque_config *cfg)
{
	static const char value[] = "opaque-cache-hit";
	static const char expected[] = "$16\r\nopaque-cache-hit\r\n";
	struct io_uring_opaque_frame frame = {
		.size = sizeof(frame), .prefix = (uintptr_t)"$16\r\n", .prefix_length = 5,
		.suffix = (uintptr_t)"\r\n", .suffix_length = 2,
	};
	struct io_uring_opaque_frame generated = {
		.size = sizeof(generated), .prefix = (uintptr_t)"$-1\r\n", .prefix_length = 5,
	};
	struct io_uring_opaque_stat stat;
	struct ring r;
	struct event event;
	uint32_t context;
	uint64_t stream, tag, handle, sends[4];
	unsigned char buf[64];
	int source[2], target[2];
	unsigned int i;

	require(!ring_init(&r, IORING_SETUP_CQE32), "framed send ring");
	require(!register_store(&r, cfg, &context), "framed send store");
	tcp_pair(source);
	tcp_pair(target);
	stream = attach(&r, context, source[1], next_tag++);
	tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, 16, NULL);
	write_all(source[0], value, 16);
	event = wait_tag(&r, tag);
	require(event.res == 16, "framed send object");
	handle = event.extra[0];
	for (i = 0; i < 3; i++)
		sends[i] = frame_stage(&r, context, target[0], handle, 16, &frame,
				       i == 2 ? IORING_OPAQUE_SEND_LAST : 0, 0);
	sends[3] = frame_stage(&r, context, target[0], 0, 0, &generated, 0, 0);
	for (i = 0; i < 3; i++) {
		event = wait_tag(&r, sends[i]);
		require(event.res == (int)sizeof(expected) - 1 &&
			!!(event.flags & IORING_CQE_F_OPAQUE_CONSUMED) == (i == 2),
			"one framed result and independent LAST ownership");
		read_all(target[1], buf, sizeof(expected) - 1);
		require(!memcmp(buf, expected, sizeof(expected) - 1),
			"complete framed response bytes");
	}
	require(wait_tag(&r, sends[3]).res == 5, "generated-only reply shares frame queue");
	read_all(target[1], buf, 5);
	require(!memcmp(buf, "$-1\r\n", 5) &&
		command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0, 1, buf).res == -ESTALE,
		"generated reply follows consuming frame");
	ksft_test_result_pass("one send orders prefix, reusable or LAST body and suffix\n");

	tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 16, 16, NULL);
	write_all(source[0], value, 16);
	event = wait_tag(&r, tag);
	require(event.res == 16, "framing validation object");
	handle = event.extra[0];
	{
		struct io_uring_opaque_frame cases[] = {
			{ .size = 4 },
			{ .size = sizeof(frame), .flags = 1 },
			{ .size = sizeof(frame), .__resv = { 1 } },
			{ .size = sizeof(frame), .prefix_length = 1, .prefix = 1 },
			{ .size = sizeof(frame), .suffix_length = 1, .suffix = 1 },
			{ .size = sizeof(frame), .prefix_length = IORING_OPAQUE_FRAME_MAX + 1 },
			{ .size = sizeof(frame), .prefix_length = UINT32_MAX, .suffix_length = 1 },
			{ .size = sizeof(frame), .prefix = 1 },
		};
		struct {
			struct io_uring_opaque_frame frame;
			uint64_t extra;
		} extended = { .frame.size = sizeof(extended), .extra = 1 };

		for (i = 0; i < ARRAY_SIZE(cases); i++) {
			tag = frame_stage(&r, context, target[0], handle, 16, &cases[i],
					  IORING_OPAQUE_SEND_LAST, 0);
			event = wait_tag(&r, tag);
			require(event.res == (i == 3 || i == 4 ? -EFAULT : -EINVAL) &&
				!(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
					"framing rejected before LAST");
			require(command(&r, context, IORING_OPAQUE_READ_OBJECT,
					handle, 0, 16, buf).res == 16 && !memcmp(buf, value, 16),
				"bad framing preserves handle");
		}
		tag = frame_stage(&r, context, target[0], handle, 16, &extended.frame,
				  IORING_OPAQUE_SEND_LAST, 0);
		event = wait_tag(&r, tag);
		require(event.res == -E2BIG && !(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
			"nonzero framing extension rejected before consumption");
		extended.extra = 0;
		extended.frame.prefix = (uintptr_t)"[";
		extended.frame.prefix_length = 1;
		extended.frame.suffix = (uintptr_t)"]";
		extended.frame.suffix_length = 1;
		tag = frame_stage(&r, context, target[0], handle, 16, &extended.frame, 0, 0);
		require(wait_tag(&r, tag).res == 18, "zero framing extension accepted");
		read_all(target[1], buf, 18);
		require(buf[0] == '[' && !memcmp(buf + 1, value, 16) && buf[17] == ']',
			"extended frame bytes");
		extended.frame.size = offsetof(struct io_uring_opaque_frame, __resv);
		tag = frame_stage(&r, context, target[0], handle, 16, &extended.frame, 0, 0);
		require(wait_tag(&r, tag).res == 18, "minimum framing prefix accepted");
		read_all(target[1], buf, 18);
		require(buf[0] == '[' && !memcmp(buf + 1, value, 16) && buf[17] == ']',
			"minimum frame bytes");
		extended.frame.prefix = (uintptr_t)"$4\r\n";
		extended.frame.prefix_length = 4;
		extended.frame.suffix = (uintptr_t)"\r\n";
		extended.frame.suffix_length = 2;
		{
			struct io_uring_sqe range = {
				.opcode = IORING_OP_OPAQUE_OBJ_SEND, .fd = target[0],
				.opaque_store_id = context, .addr = handle, .off = 2, .len = 4,
				.addr3 = (uintptr_t)&extended.frame, .user_data = next_tag++,
			};

			stage(&r, range);
			require(wait_tag(&r, range.user_data).res == 10,
				"framed object subrange result");
			read_all(target[1], buf, 10);
			require(!memcmp(buf, "$4\r\naque\r\n", 10), "framed object subrange bytes");
		}
		tag = frame_stage(&r, context, target[0], 0, 0, &generated,
				  IORING_OPAQUE_SEND_LAST, 0);
		event = wait_tag(&r, tag);
		require(event.res == -EINVAL && !(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
			"generated-only frame cannot consume an object");
	}
	require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"framing validation teardown");
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		!stat.backing_bytes && !stat.framing_bytes && stat.framing_copied_bytes == 41,
		"framing charge and copies accounted separately");
	ring_exit(&r);
	close(source[0]);
	close(source[1]);
	close(target[0]);
	close(target[1]);
	ksft_test_result_pass("framing faults and version validation precede object consumption\n");
}

static void test_framed_backpressure(struct io_uring_opaque_config *cfg)
{
	const unsigned int length = 1U << 20;
	const unsigned int prefix_length = IORING_OPAQUE_FRAME_MAX - 8;
	const unsigned int total = prefix_length + length + 8;
	unsigned int mode, i;

	for (mode = 0; mode < 3; mode++) {
		struct ring r;
		struct event event;
		struct io_uring_opaque_stat stat;
		unsigned char *data = malloc(length), *expected = malloc(2 * total + 5);
		unsigned char *framing = mmap(NULL, IORING_OPAQUE_FRAME_MAX,
					     PROT_READ | PROT_WRITE,
						MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		struct io_uring_opaque_frame frame = {
			.size = sizeof(frame), .prefix = (uintptr_t)framing,
			.prefix_length = prefix_length,
			.suffix = (uintptr_t)(framing + prefix_length), .suffix_length = 8,
		};
		struct io_uring_opaque_frame generated = {
			.size = sizeof(generated), .prefix = (uintptr_t)"DONE\n",
				.prefix_length = 5,
		};
		struct transfer tx, rx;
		uint32_t context;
		uint64_t stream, object, handle, first, second = 0, last = 0;
		pthread_t producer, consumer;
		unsigned char byte;
		int source[2], target[2], queue_size = 4096;

		require(data && expected && framing != MAP_FAILED, "framed pressure memory");
		for (i = 0; i < length; i++)
			data[i] = i * 31;
		memset(framing, 'H', prefix_length);
		memcpy(framing + prefix_length, "TRAILER\n", 8);
		memcpy(expected, framing, prefix_length);
		memcpy(expected + prefix_length, data, length);
		memcpy(expected + prefix_length + length, framing + prefix_length, 8);
		memcpy(expected + total, expected, total);
		memcpy(expected + 2 * total, "DONE\n", 5);
		require(!ring_init(&r, IORING_SETUP_CQE32), "framed pressure ring");
		require(!register_store(&r, cfg, &context), "framed pressure store");
		tcp_pair(source);
		tcp_pair(target);
		require(!setsockopt(target[0], SOL_SOCKET, SO_SNDBUF, &queue_size,
				    sizeof(queue_size)) &&
			!setsockopt(target[1], SOL_SOCKET, SO_RCVBUF, &queue_size,
				sizeof(queue_size)),
			"framed pressure socket buffers");
		stream = attach(&r, context, source[1], next_tag++);
		object = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, length, NULL);
		tx = (struct transfer) { .fd = source[0], .data = data, .length = length };
		require(!pthread_create(&producer, NULL, writer, &tx), "framed pressure producer");
		event = wait_tag(&r, object);
		pthread_join(producer, NULL);
		require(event.res == (int)length, "framed pressure complete object");
		handle = event.extra[0];
		first = frame_stage(&r, context, target[0], handle, length, &frame,
				    mode == 2 ? IORING_OPAQUE_SEND_LAST : 0,
				    mode == 2 ? MSG_DONTWAIT : 0);
		enter(&r, 0);
		if (mode != 2) {
			if (!mode)
				require(!command(&r, context, IORING_OPAQUE_COMPACT, handle,
						 0, 0, NULL).res,
					"compact behind framed send pin");
			second = frame_stage(&r, context, target[0], handle, length, &frame,
					     IORING_OPAQUE_SEND_LAST, 0);
			last = frame_stage(&r, context, target[0], 0, 0, &generated, 0, 0);
			enter(&r, 0);
			require(command(&r, context, IORING_OPAQUE_READ_OBJECT,
					handle, 0, 1, &byte).res == -ESTALE,
				"queued framed LAST admitted");
			require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0,
					 sizeof(stat), &stat).res &&
				stat.framing_bytes >= 2 * IORING_OPAQUE_FRAME_MAX + 5 &&
				stat.framing_bytes < 2 * IORING_OPAQUE_FRAME_MAX +
						     (uint64_t)sysconf(_SC_PAGESIZE),
				"small generated follower uses a subpage framing charge");
		}
		if (!mode) {
			memset(framing, 'X', IORING_OPAQUE_FRAME_MAX);
			memset(&frame, 0, sizeof(frame));
			require(!munmap(framing, IORING_OPAQUE_FRAME_MAX),
				"release admitted user framing");
			framing = MAP_FAILED;
			rx = (struct transfer) { .fd = target[1], .data = expected,
				.length = 2 * total + 5, .repeats = 1 };
			require(!pthread_create(&consumer, NULL, reader, &rx),
				"framed pressure reader");
			event = wait_tag(&r, first);
			require(event.res == (int)total &&
				!(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
				"reusable whole frame completes");
			event = wait_tag(&r, second);
			require(event.res == (int)total &&
				(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
				"queued consuming whole frame completes");
			require(wait_tag(&r, last).res == 5, "generated follower completes");
			pthread_join(consumer, NULL);
			require(rx.ok, "frame snapshots and complete responses stay ordered");
		} else {
			if (mode == 1) {
				struct io_uring_sqe cancel = {
					.opcode = IORING_OP_ASYNC_CANCEL, .addr = first,
						.user_data = next_tag++,
				};

				stage(&r, cancel);
				require(wait_tag(&r, cancel.user_data).res >= 0,
					"cancel partial framed head");
			}
			event = wait_tag(&r, first);
			require(event.res > 0 && event.res < (int)total &&
				!!(event.flags & IORING_CQE_F_OPAQUE_CONSUMED) == (mode == 2),
				"partial frame counts all progress and independent ownership");
			rx = (struct transfer) { .fd = target[1], .data = expected,
				.length = event.res, .repeats = 1 };
			reader(&rx);
			require(rx.ok, "partial frame is exact prefix of response");
			if (mode == 1) {
				event = wait_tag(&r, second);
				require(event.res == -ECANCELED &&
					(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
					"partial head cancels admitted LAST follower");
				event = wait_tag(&r, last);
				require(event.res == -ECANCELED &&
					!(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
					"partial head cancels generated follower");
			}
			require(command(&r, context, IORING_OPAQUE_READ_OBJECT,
					handle, 0, 1, &byte).res == -ESTALE,
						"consumed frame stays retired");
		}
		require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
			"framed pressure stream close");
		require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
			!stat.backing_bytes && !stat.framing_bytes && !stat.objects &&
				!stat.streams,
			"framed pressure references drain");
		ring_exit(&r);
		close(source[0]);
		close(source[1]);
		close(target[0]);
		close(target[1]);
		if (framing != MAP_FAILED)
			munmap(framing, IORING_OPAQUE_FRAME_MAX);
		free(expected);
		free(data);
		ksft_test_result_pass("framed backpressure: %s\n", mode == 0 ?
			"snapshots, compaction and complete ordering" : mode == 1 ?
			"partial cancellation stops both kinds of follower" :
			"nonblocking total progress and LAST");
	}
}

static void test_frame_quota(struct io_uring_opaque_config *cfg)
{
	struct io_uring_opaque_config small = *cfg;
	struct io_uring_opaque_frame frame = {
		.size = sizeof(frame), .prefix = (uintptr_t)"[", .prefix_length = 1,
	};
	struct io_uring_opaque_stat stat;
	struct event event;
	struct ring r;
	uint32_t context;
	uint64_t stream, tag, handle;
	unsigned char buf[8];
	int source[2], target[2];

	small.hard_limit = 2 * sysconf(_SC_PAGESIZE);
	small.compact_headroom = sysconf(_SC_PAGESIZE);
	require(!ring_init(&r, IORING_SETUP_CQE32), "frame quota ring");
	require(!register_store(&r, &small, &context), "frame quota store");
	tcp_pair(source);
	tcp_pair(target);
	stream = attach(&r, context, source[1], next_tag++);
	tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, 8, NULL);
	write_all(source[0], "quota123", 8);
	event = wait_tag(&r, tag);
	require(event.res == 8, "frame quota object");
	handle = event.extra[0];
	tag = frame_stage(&r, context, target[0], handle, 8, &frame, IORING_OPAQUE_SEND_LAST, 0);
	event = wait_tag(&r, tag);
	require(event.res == -ENOBUFS && !(event.flags & IORING_CQE_F_OPAQUE_CONSUMED) &&
		command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0, 8, buf).res == 8 &&
		!memcmp(buf, "quota123", 8), "frame quota failure preserves ownership");
	require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		!stat.backing_bytes && !stat.framing_bytes, "frame quota drains");
	ring_exit(&r);
	close(source[0]);
	close(source[1]);
	close(target[0]);
	close(target[1]);
	ksft_test_result_pass("framing quota failure leaves LAST handle reusable\n");
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
		.flags = IORING_OPAQUE_POLICY_F_AUTO_COMPACT,
		.bytes_per_second = 64U << 20,
		.burst_bytes = 8U << 20,
		.max_temporary_bytes = 8U << 20,
	};
	struct event event;
	uint32_t context, imported_context;
	uint64_t stream, handle, old_handle, tag, cursor = 8;
	unsigned char buf[32];
	int source[2], target[2], ret;

	alarm(600);
	ksft_print_header();
	ret = ring_init(&r, IORING_SETUP_CQE32);
	if (ret)
		ksft_exit_skip("io_uring unavailable: %s\n", strerror(-ret));
	ret = register_store(&r, &cfg, &context);
	if (ret == -EINVAL || ret == -EOPNOTSUPP)
		ksft_exit_skip("OPAQUE_OBJ unavailable\n");
	require(!ret, "opaque registration");
	ksft_set_plan(77);
	ksft_test_result_pass("register opaque store without RX device or mappings\n");
	tcp_pair(source);
	tcp_pair(target);
	stream = attach(&r, context, source[1], 100);
	errno = 0;
	require(recv(source[1], buf, sizeof(buf), MSG_DONTWAIT) == -1 && errno == EBUSY,
		"ordinary receive ownership guard");
	{
		int pipefd[2], repair = 1;
		struct io_uring_sqe second = { .opcode = IORING_OP_OPAQUE_COLLECT, .fd = source[1],
			.ioprio = 0, .opaque_store_id = context,
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
	tag = cmd_stage(&r, context, IORING_OPAQUE_INSPECT, stream, 0, 3, buf);
	write_all(source[0], "a", 1);
	event = wait_tag(&r, tag);
	require(event.res == 1 && buf[0] == 'a', "header capacity is not a wait threshold");
	write_all(source[0], "bc", 2);
	ksft_test_result_pass("inspect split stream framing without an exact-length wait\n");
	tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, 8, NULL);
	enter(&r, 0);
	require(!peek(&r, &event), "incomplete KEEP must not publish");
	write_all(source[0], "defgh", 5);
	event = wait_tag(&r, tag);
	require(event.res == 8 && event.extra[0], "completed KEEP");
	handle = event.extra[0];
	event = command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 1, 5, buf);
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
	require(!event.res && (object_stat.flags & IORING_OPAQUE_OBJECT_F_COMPACTED),
		"compacted flag");
	event = command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0, 8, buf);
	require(event.res == 8 && !memcmp(buf, "abcdefgh", 8), "stable compacted handle");
	ksft_test_result_pass("compact backing while preserving handle and bytes\n");
	{
		int store_fd = export_store(&r, context);

		require(store_fd >= 0, "export store");
		require(!ring_init(&imported, IORING_SETUP_CQE32), "import ring");
		require(!import_store(&imported, store_fd, &imported_context), "import store");
		close(store_fd);
		event = command(&imported, imported_context, IORING_OPAQUE_READ_OBJECT,
				handle, 0, 8, buf);
		require(event.res == 8 && !memcmp(buf, "abcdefgh", 8), "shared object handle");
		ring_exit(&imported);
	}
	ksft_test_result_pass("export/import preserves shared handles and object lifetime\n");
	old_handle = handle;
	require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res, "FREE");
	require(command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0, 1, buf).res == -ESTALE,
		"stale object rejection");
	ksft_test_result_pass("FREE invalidates the generation handle\n");
	tag = cmd_stage(&r, context, IORING_OPAQUE_DISCARD, stream, cursor, 4, NULL);
	write_all(source[0], "skip", 4);
	require(wait_tag(&r, tag).res == 4, "future discard");
	cursor += 4;
	ksft_test_result_pass("discard future bytes without publishing an object\n");
	{
		uint64_t pending = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT,
					     stream, cursor, 4, NULL);
		struct io_uring_sqe cancel = { .opcode = IORING_OP_ASYNC_CANCEL, .fd = -1,
			.addr = pending, .user_data = next_tag++ };

		stage(&r, cancel);
		require(wait_tag(&r, cancel.user_data).res >= 0, "cancel future KEEP");
		require(wait_tag(&r, pending).res == -ECANCELED, "canceled KEEP completion");
	}
	ksft_test_result_pass("ASYNC_CANCEL terminates an incomplete KEEP\n");
	{
		uint64_t pending = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT,
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
		unsigned char bytes[8193], actual[sizeof(bytes)];

		memset(bytes, 42, sizeof(bytes));
		tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, cursor,
				sizeof(bytes), NULL);
		write_all(source[0], bytes, sizeof(bytes));
		event = wait_tag(&r, tag);
		handle = event.extra[0];
		cursor += sizeof(bytes);
		require(event.res == (int)sizeof(bytes) && handle != old_handle,
			"new object identity");
		require(!command(&r, context, IORING_OPAQUE_SET_POLICY,
				 0, 0, sizeof(policy), &policy).res,
			"automatic compaction policy");
		for (ret = 0; ret < 100; ret++) {
			require(!command(&r, context, IORING_OPAQUE_OBJECT_STAT, handle, 0,
					 sizeof(object_stat), &object_stat).res,
				"auto object stat");
			if (object_stat.flags & IORING_OPAQUE_OBJECT_F_COMPACTED)
				break;
			usleep(10000);
		}
		require(ret < 100, "automatic compaction progress");
		require(command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0,
				sizeof(actual), actual).res == (int)sizeof(actual) &&
			!memcmp(actual, bytes, sizeof(actual)), "automatic compaction bytes");
		policy.flags = 0;
		require(!command(&r, context, IORING_OPAQUE_SET_POLICY,
				 0, 0, sizeof(policy), &policy).res,
			"disable automatic compaction");
		require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res,
			"free auto object");
	}
	ksft_test_result_pass("automatic compaction preserves object identity and payload bytes\n");
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
		tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, cursor,
				length, NULL);
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

		tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, cursor, 3, NULL);
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
		require(command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0, 3,
				buf).res == 3 &&
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
	test_registration_abi(cfg);
	test_send_last(&cfg);
	test_async_objects(&cfg);
	test_framed_sends(&cfg);
	test_framed_backpressure(&cfg);
	test_frame_quota(&cfg);
	test_budget(cfg);
	test_budget_notifications(cfg);
	test_budget_full_cq(cfg);
	test_waiting_reader(&cfg);
	test_eof(&cfg);
	test_urgent(&cfg);
	test_teardown(&cfg);
	test_unprivileged(cfg);
	test_collector_edges(&cfg);
	test_collector_bulk(&cfg);
	test_reset(&cfg);
	test_mixed_cqe(&cfg);
	test_send_fifo(&cfg);
	test_send_cancel_race(&cfg);
	test_link_order(&cfg);
	test_keep_restore(&cfg);
	test_interleaved(cfg);
	test_compact_queue(&cfg);
	test_budget_error(cfg);
	test_memcg(&cfg);
	test_disconnect(&cfg);
	test_socket_lifetime(&cfg);
	test_poll_first(&cfg);
	test_full_cq(&cfg);
	test_allocation_failure(&cfg);
	ksft_finished();
}
