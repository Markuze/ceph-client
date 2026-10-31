// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/ethtool.h>
#include <linux/io_uring.h>
#include <linux/io_uring/opaque.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
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
#include <time.h>
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

static void tcp_pair_buffers(int pair[2], int sndbuf, int rcvbuf)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t len = sizeof(addr);
	int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	int one = 1;

	require(listener >= 0, "small-window listen socket");
	require(!setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)),
		"small window before SYN");
	require(!bind(listener, (void *)&addr, sizeof(addr)), "small-window bind");
	require(!getsockname(listener, (void *)&addr, &len), "small-window address");
	require(!listen(listener, 1), "small-window listen");
	pair[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	require(pair[0] >= 0, "small-window connect socket");
	require(!setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)),
		"small send queue before connect");
	require(!connect(pair[0], (void *)&addr, sizeof(addr)), "small-window connect");
	pair[1] = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
	require(pair[1] >= 0, "small-window accept");
	require(!setsockopt(pair[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)),
		"small-window TCP_NODELAY");
	close(listener);
}

static void tcp_pair_small(int pair[2])
{
	tcp_pair_buffers(pair, 4096, 4096);
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

static void test_compact_queue(struct io_uring_opaque_config *cfg, int mode)
{
	struct ring r, imported;
	struct event event;
	struct io_uring_opaque_stat stat;
	struct io_uring_opaque_policy policy = {
		.flags = IORING_OPAQUE_POLICY_F_AUTO_COMPACT,
		.bytes_per_second = mode == 1 ? 1 : 64U << 20,
		.burst_bytes = mode == 1 ? 1 : 8U << 20,
		.max_temporary_bytes = mode == 1 ? sysconf(_SC_PAGESIZE) : 8U << 20,
	};
	unsigned char data[65536], seen[sizeof(data)];
	uint32_t context, other = 0;
	uint64_t stream, handles[16], copies[16], keep, disabled = 0;
	int pair[2], i, fd;

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
	if (mode == 2) {
		fd = export_store(&r, context);
		require(fd >= 0 && !ring_init(&imported, IORING_SETUP_CQE32) &&
			!import_store(&imported, fd, &other), "shared compaction queue");
		close(fd);
	}
	if (mode)
		require(!command(&r, context, IORING_OPAQUE_SET_POLICY, 0, 0,
				 sizeof(policy), &policy).res, "compaction queue auto policy");
	for (i = 0; i < 16; i++) {
		bool remote = mode == 2 && (i & 1);

		copies[i] = cmd_stage(remote ? &imported : &r, remote ? other : context,
				      IORING_OPAQUE_COMPACT, handles[i], 0, 0, NULL);
	}
	if (mode == 1) {
		policy.flags = 0;
		disabled = cmd_stage(&r, context, IORING_OPAQUE_SET_POLICY, 0, 0,
				     sizeof(policy), &policy);
	}
	if (mode != 2) {
		struct io_uring_sqe cancel = {
			.opcode = IORING_OP_ASYNC_CANCEL, .fd = -1,
			.addr = copies[15], .user_data = next_tag++,
		};

		stage(&r, cancel);
		require(!wait_tag(&r, cancel.user_data).res, "cancel queued compaction");
	} else {
		enter(&r, 0);
		enter(&imported, 0);
	}
	if (disabled)
		require(!wait_tag(&r, disabled).res, "disable auto with accepted manual copies");
	for (i = 0; i < 16; i++) {
		event = wait_tag(mode == 2 && (i & 1) ? &imported : &r, copies[i]);
		require(mode == 2 ? event.res == 0 || event.res == -EBUSY :
			event.res == (i == 15 ? -ECANCELED : 0),
			"shared compaction dispatcher completion");
		if (mode == 2) {
			struct io_uring_opaque_object_stat object;
			int attempt;

			for (attempt = 0; attempt < 300; attempt++) {
				require(!command(&r, context, IORING_OPAQUE_OBJECT_STAT, handles[i],
						 0, sizeof(object), &object).res,
					"shared compact stat");
				if (object.flags & IORING_OPAQUE_OBJECT_F_COMPACTED)
					break;
				usleep(10000);
			}
			require(attempt < 300, "manual and automatic compaction progress");
		}
		require(command(&r, context, IORING_OPAQUE_READ_OBJECT, handles[i], 0,
				sizeof(seen), seen).res == (int)sizeof(seen) &&
			!memcmp(data, seen, sizeof(data)), "queued compaction preserves bytes");
		require(!command(&r, context, IORING_OPAQUE_FREE, handles[i], 0, 0, NULL).res,
			"queued compaction FREE");
	}
	if (mode == 2) {
		policy.flags = 0;
		require(!command(&imported, other, IORING_OPAQUE_SET_POLICY, 0, 0,
				 sizeof(policy), &policy).res, "disable shared auto compaction");
		ring_exit(&imported);
	}
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"compaction queue close");
	if (mode == 2) {
		int attempt;

		/* Automatic publication precedes release of its worker's source pin. */
		for (attempt = 0; attempt < 300; attempt++) {
			require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0,
					 sizeof(stat), &stat).res, "automatic copy cleanup stat");
			if (!stat.backing_bytes && !stat.extents)
				break;
			usleep(10000);
		}
		require(attempt < 300, "automatic copy releases its final source pin");
	}
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		!stat.backing_bytes && !stat.extents, "compaction dispatcher drains reservations");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("%s\n", mode == 1 ?
		"disabling automatic policy preserves accepted manual compactions" : mode == 2 ?
		"shared-ring manual and automatic compaction preserves bytes and drains backing" :
		"shared compaction worker drains batches and canceled requests");
}

static void test_compact_teardown(struct io_uring_opaque_config *cfg)
{
	unsigned char data[65536], byte;
	int round;

	memset(data, 0x64, sizeof(data));
	for (round = 0; round < 8; round++) {
		struct io_uring_opaque_policy policy = {
			.flags = IORING_OPAQUE_POLICY_F_AUTO_COMPACT,
			.bytes_per_second = 64U << 20, .burst_bytes = 8U << 20,
			.max_temporary_bytes = 8U << 20,
		};
		struct ring r;
		uint64_t handles[16], stream, keep;
		uint32_t context;
		int pair[2], i;

		require(!ring_init(&r, IORING_SETUP_CQE32) &&
			!register_store(&r, cfg, &context), "compaction teardown store");
		tcp_pair(pair);
		stream = attach(&r, context, pair[1], 713);
		for (i = 0; i < 16; i++) {
			struct event event;

			keep = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream,
					 (uint64_t)i * sizeof(data), sizeof(data), NULL);
			write_all(pair[0], data, sizeof(data));
			event = wait_tag(&r, keep);
			require(event.res == (int)sizeof(data), "compaction teardown capture");
			handles[i] = event.extra[0];
		}
		require(!command(&r, context, IORING_OPAQUE_SET_POLICY, 0, 0,
				 sizeof(policy), &policy).res, "compaction teardown auto timer");
		for (i = 0; i < 16; i++)
			cmd_stage(&r, context, IORING_OPAQUE_COMPACT, handles[i], 0, 0, NULL);
		enter(&r, 0);
		ring_exit(&r);
		for (i = 0; i < 300; i++) {
			if (recv(pair[1], &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN)
				break;
			usleep(10000);
		}
		require(i < 300, "compaction teardown releases the receive claim");
		close(pair[0]);
		close(pair[1]);
	}
	ksft_test_result_pass("queued compactions drain during ring teardown with auto policy\n");
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
			{ target[0], 0, 8, 4, 0, -EINVAL },
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
	cfg.flags = 1U << 31;
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

static void submit_only(struct ring *r)
{
	unsigned int pending = *r->sq_tail - *r->sq_head;

	require(syscall(__NR_io_uring_enter, r->fd, pending, 0, 0, NULL, 0) >= 0,
		"submit without deferred task work");
}

static struct event wait_tag_ms(struct ring *r, uint64_t tag, unsigned int ms)
{
		struct timespec start, now;
	struct event event;
	unsigned int i;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		struct __kernel_timespec ts = { .tv_nsec = 10000000 };
		struct io_uring_getevents_arg arg = { .ts = (uintptr_t)&ts };
		unsigned int pending = *r->sq_tail - *r->sq_head;
		int ret;

		for (i = 0; i < r->nr_saved; i++) {
			if (r->saved[i].tag == tag) {
				event = r->saved[i];
				r->saved[i] = r->saved[--r->nr_saved];
				return event;
			}
		}
		ret = syscall(__NR_io_uring_enter, r->fd, pending, 1,
			      IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg, sizeof(arg));
		require(ret >= 0 || errno == ETIME || errno == EINTR, "bounded CQ wait");
		while (peek(r, &event)) {
			if (event.tag == tag)
				return event;
			require(r->nr_saved < 64, "bounded wait completion stash");
			r->saved[r->nr_saved++] = event;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		require((now.tv_sec - start.tv_sec) * 1000 +
			(now.tv_nsec - start.tv_nsec) / 1000000 < ms, "CQ wait deadline");
	}
}

static struct event receive_complete(struct ring *r, uint32_t context, uint64_t stream,
				     int fd, uint64_t offset, const void *data, size_t length)
{
	struct transfer tx = { .fd = fd, .data = data, .length = length };
	uint64_t tag = cmd_stage(r, context, IORING_OPAQUE_RECV_OBJECT,
				stream, offset, length, NULL);
	struct event event;
	pthread_t producer;

	submit_only(r);
	require(!pthread_create(&producer, NULL, writer, &tx), "complete object producer");
	event = wait_tag_ms(r, tag, 30000);
	pthread_join(producer, NULL);
	require(event.res == (int)length, "complete object receive");
	return event;
}

static int unread_bytes(int fd)
{
	int unread;

	require(!ioctl(fd, FIONREAD, &unread), "TCP unread count");
	return unread;
}

static uint64_t stream_undecided(struct ring *r, uint32_t context, uint64_t stream)
{
	struct io_uring_opaque_stream_stat stat;

	require(!command(r, context, IORING_OPAQUE_STREAM_STAT, stream, 0,
			 sizeof(stat), &stat).res, "stream stat");
	return stat.undecided_bytes;
}

/* Let the collector's task work run (DEFER_TASKRUN needs a GETEVENTS enter). */
static void settle(struct ring *r)
{
	usleep(100000);
	enter(r, 0);
}

/* G: undecided bytes are retained uncopied and hold further collection. */
static void test_capture_gate(struct io_uring_opaque_config cfg)
{
	const size_t length = 16U << 10;
	unsigned char *data = calloc(1, length), header[128];
	struct io_uring_opaque_stat stat;
	struct ring r;
	uint32_t context;
	uint64_t stream;
	int pair[2];

	require(data && !ring_init(&r, IORING_SETUP_CQE32) &&
		!register_store(&r, &cfg, &context), "capture gate store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	write_all(pair[0], data, length);
	settle(&r);
	require(!unread_bytes(pair[1]) && stream_undecided(&r, context, stream) == length,
		"first arrival collected while nothing was undecided");
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, sizeof(header),
			header).res == sizeof(header), "inspect first arrival");
	write_all(pair[0], data, length);
	settle(&r);
	require(unread_bytes(pair[1]) == (int)length &&
		stream_undecided(&r, context, stream) == length,
		"second arrival stays on TCP while bytes are undecided");
	require(command(&r, context, IORING_OPAQUE_DISCARD, stream, 0, length,
			NULL).res == (int)length, "decide first arrival");
	settle(&r);
	require(!unread_bytes(pair[1]) && stream_undecided(&r, context, stream) == length,
		"deciding the first arrival lets the collector take the second");
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		!stat.copied_bytes, "undecided bytes are retained, not copied");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"capture gate close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	free(data);
	ksft_test_result_pass("undecided bytes are retained and hold further collection\n");
}

/* S: a header split across arrivals; a waiting INSPECT pulls the second part. */
static void test_split_header(struct io_uring_opaque_config cfg)
{
	unsigned char part[100], buf[512];
	struct ring r;
	uint32_t context;
	uint64_t stream, tag;
	int pair[2];

	memset(part, 'h', sizeof(part));
	require(!ring_init(&r, IORING_SETUP_CQE32) && !register_store(&r, &cfg, &context),
		"split header store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	write_all(pair[0], part, sizeof(part));
	settle(&r);
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, sizeof(buf),
			buf).res == sizeof(part), "inspect first header part");
	write_all(pair[0], part, sizeof(part));
	settle(&r);
	require(unread_bytes(pair[1]) == sizeof(part), "second part stays on TCP");
	tag = cmd_stage(&r, context, IORING_OPAQUE_INSPECT, stream, sizeof(part),
			sizeof(buf), buf);
	require(wait_tag_ms(&r, tag, 5000).res == sizeof(part),
		"waiting INSPECT collects the second part");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"split header close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("a waiting INSPECT collects a split header\n");
}

/* N1 with a zero memory policy: a flooding stream cannot refill freed capacity. */
static void test_greedy_default_config(struct io_uring_opaque_config cfg)
{
	struct io_uring_opaque_frame frame = { .size = sizeof(frame),
		.prefix = (uintptr_t)"HDR:", .prefix_length = 4 };
	unsigned char *flood = calloc(1, 256U << 10);
	struct ring r;
	struct event event;
	uint32_t context;
	uint64_t greedy, responder, request, cached, before;
	int a[2], b[2], target[2];

	cfg.hard_limit = 256U << 10;
	cfg.compact_headroom = 64U << 10;
	cfg.reply_reserve = 0;
	cfg.high_watermark = 0;
	cfg.low_watermark = 0;
	require(flood && !ring_init(&r, IORING_SETUP_CQE32) &&
		!register_store(&r, &cfg, &context), "default policy store");
	tcp_pair(a);
	tcp_pair(b);
	tcp_pair(target);
	responder = attach(&r, context, b[1], next_tag++);
	request = receive_complete(&r, context, responder, b[0], 0, "request!", 8).extra[0];
	cached = receive_complete(&r, context, responder, b[0], 8, "cached!!", 8).extra[0];
	greedy = attach(&r, context, a[1], next_tag++);
	write_all(a[0], flood, 256U << 10);
	settle(&r);
	before = stream_undecided(&r, context, greedy);
	require(!command(&r, context, IORING_OPAQUE_FREE, cached, 0, 0, NULL).res,
		"evict cached object");
	settle(&r);
	require(stream_undecided(&r, context, greedy) == before && unread_bytes(a[1]) > 0,
		"flooding stream does not refill freed capacity");
	event = wait_tag(&r, frame_stage(&r, context, target[0], request, 8, &frame,
					 IORING_OPAQUE_SEND_LAST, 0));
	require(event.res == 12 && (event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
		"framed reply after eviction");
	event = wait_tag(&r, frame_stage(&r, context, target[0], 0, 0, &frame, 0, 0));
	require(event.res == 4, "generated-only reply after eviction");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, greedy, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, responder, 0, 0, NULL).res,
		"default policy close");
	ring_exit(&r);
	close(a[0]); close(a[1]); close(b[0]); close(b[1]);
	close(target[0]); close(target[1]);
	free(flood);
	ksft_test_result_pass("default policy: flooding stream cannot starve replies\n");
}

/* A header longer than the old 4 KiB window is fully visible to INSPECT. */
static void test_header_larger_than_old_window(struct io_uring_opaque_config cfg)
{
	unsigned char header[6000], buf[8192];
	struct ring r;
	uint32_t context;
	uint64_t stream;
	int pair[2];

	memset(header, 'H', sizeof(header));
	require(!ring_init(&r, IORING_SETUP_CQE32) && !register_store(&r, &cfg, &context),
		"large header store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	write_all(pair[0], header, sizeof(header));
	settle(&r);
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, sizeof(buf),
			buf).res == sizeof(header), "inspect a 6000-byte header");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"large header close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("INSPECT sees a header larger than one page\n");
}

/* A decision that becomes the earliest during a held pass collects the gap before it. */
static void test_gap_after_decision(struct io_uring_opaque_config cfg)
{
	unsigned char header[100], wire[2100], buf[128];
	struct ring r;
	uint32_t context;
	uint64_t stream, first, second;
	int pair[2];

	memset(header, 'H', sizeof(header));
	memset(wire, 'B', sizeof(wire));
	require(!ring_init(&r, IORING_SETUP_CQE32) && !register_store(&r, &cfg, &context),
		"gap decision store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	write_all(pair[0], header, sizeof(header));
	settle(&r);
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, sizeof(buf),
			buf).res == sizeof(header), "inspect undecided header");
	/* Two bodies with an uncollected header between them; all bytes arrive at once. */
	first = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 100, 1000, NULL);
	second = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 1200, 1000, NULL);
	submit_only(&r);
	write_all(pair[0], wire, sizeof(wire));
	settle(&r);
	require(wait_tag_ms(&r, first, 5000).res == 1000, "first declared body");
	require(wait_tag_ms(&r, second, 5000).res == 1000,
		"later body completes without another arrival");
	require(!unread_bytes(pair[1]) && stream_undecided(&r, context, stream) == 200,
		"gap collected as undecided");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"gap decision close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("a decision past an uncollected gap completes in a held pass\n");
}

/* Re-inspecting an offset of a held stream returns only bytes already collected. */
static void test_reinspect_same_offset(struct io_uring_opaque_config cfg)
{
	unsigned char part[100], buf[512];
	struct ring r;
	uint32_t context;
	uint64_t stream, tag;
	int pair[2];

	memset(part, 'h', sizeof(part));
	require(!ring_init(&r, IORING_SETUP_CQE32) && !register_store(&r, &cfg, &context),
		"same-offset inspect store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	write_all(pair[0], part, sizeof(part));
	settle(&r);
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, sizeof(buf),
			buf).res == sizeof(part), "inspect the first part");
	write_all(pair[0], part, sizeof(part));
	settle(&r);
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, sizeof(buf),
			buf).res == sizeof(part) && unread_bytes(pair[1]) == sizeof(part),
		"same-offset INSPECT leaves held bytes on TCP");
	tag = cmd_stage(&r, context, IORING_OPAQUE_INSPECT, stream, sizeof(part),
			sizeof(buf), buf);
	require(wait_tag_ms(&r, tag, 5000).res == sizeof(part),
		"end-offset INSPECT collects the held bytes");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"same-offset inspect close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("re-inspecting an offset returns only collected bytes\n");
}

/* Compaction copies do not count toward the watermarks. */
static void test_compact_watermarks(struct io_uring_opaque_config cfg)
{
	const size_t big = 1U << 20, step = 128U << 10;
	unsigned char *data = calloc(1, big);
	struct io_uring_opaque_stat before, after;
	struct event event;
	struct ring r;
	uint32_t context;
	uint64_t stream, large, offset = 0;
	int pair[2];

	cfg.hard_limit = 8U << 20;
	cfg.compact_headroom = 2U << 20;
	cfg.reply_reserve = 256U << 10;
	cfg.high_watermark = 4U << 20;
	cfg.low_watermark = 2U << 20;
	require(data && !ring_init(&r, IORING_SETUP_CQE32) &&
		!register_store(&r, &cfg, &context), "compaction watermark store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	large = receive_complete(&r, context, stream, pair[0], offset, data, big).extra[0];
	offset += big;
	for (;;) {
		require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(before),
				 &before).res, "compaction watermark stat");
		if (before.backing_bytes + big >= cfg.high_watermark + step)
			break;
		event = receive_complete(&r, context, stream, pair[0], offset, data, step);
		require(!event.extra[1], "cacheable object below the high watermark");
		offset += step;
	}
	require(before.memory_mode == IORING_OPAQUE_MEMORY_NORMAL, "normal before compaction");
	require(!command(&r, context, IORING_OPAQUE_COMPACT, large, 0, 0, NULL).res,
		"compact a large object near the high watermark");
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(after), &after).res &&
		after.backing_bytes <= before.backing_bytes &&
		after.memory_mode == IORING_OPAQUE_MEMORY_NORMAL,
		"compaction copy does not switch the store to lowmem");
	event = receive_complete(&r, context, stream, pair[0], offset, "after!!!", 8);
	require(!event.extra[1], "object received after compaction is cacheable");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"compaction watermark close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	free(data);
	ksft_test_result_pass("compaction copies do not switch the store to lowmem\n");
}

/* A nonzero reply reserve is a floor: blocked replies cannot starve a healthy one. */
static void test_reply_reserve_floor(struct io_uring_opaque_config cfg)
{
	static unsigned char prefix[3000];
	struct io_uring_opaque_frame big = { .size = sizeof(big),
		.prefix = (uintptr_t)prefix, .prefix_length = sizeof(prefix) };
	struct io_uring_opaque_frame small = { .size = sizeof(small),
		.prefix = (uintptr_t)"HDR:", .prefix_length = 4 };
	const size_t length = 1U << 20, total = length + sizeof(prefix);
	unsigned char *data = malloc(length), *seen1 = malloc(total), *seen2 = malloc(total);
	struct slow_transfer rx1 = { .data = seen1, .length = total };
	struct slow_transfer rx2 = { .data = seen2, .length = total };
	struct io_uring_opaque_stat stat;
	unsigned char wire[10];
	pthread_t reader1, reader2;
	unsigned int i;
	struct ring r;
	uint32_t context;
	uint64_t stream, object, reply, first, second, healthy;
	int source[2], slow1[2], slow2[2], target[2];

	require(data && seen1 && seen2, "reply floor buffers");
	memset(prefix, 'P', sizeof(prefix));
	memset(data, 'D', length);
	cfg.hard_limit = 8U << 20;
	cfg.compact_headroom = 1U << 20;
	cfg.reply_reserve = 8U << 10;
	require(!ring_init(&r, IORING_SETUP_CQE32) && !register_store(&r, &cfg, &context),
		"reply floor store");
	tcp_pair(source);
	tcp_pair_small(slow1);
	tcp_pair_small(slow2);
	tcp_pair(target);
	stream = attach(&r, context, source[1], next_tag++);
	object = receive_complete(&r, context, stream, source[0], 0, data, length).extra[0];
	reply = receive_complete(&r, context, stream, source[0], length, "reply!", 6).extra[0];
	/* Two replies block on 4 KiB peers, holding 8 KiB of framing. */
	first = frame_stage(&r, context, slow1[0], object, length, &big, 0, 0);
	second = frame_stage(&r, context, slow2[0], object, length, &big, 0, 0);
	submit_only(&r);
	usleep(100000);
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		stat.framing_bytes == 2 * 4096, "two blocked replies hold framing");
	healthy = frame_stage(&r, context, target[0], reply, 6, &small, 0, 0);
	require(wait_tag_ms(&r, healthy, 5000).res == (int)sizeof(wire),
		"healthy reply beside blocked replies");
	read_all(target[1], wire, sizeof(wire));
	require(!memcmp(wire, "HDR:reply!", sizeof(wire)), "healthy reply wire bytes");
	rx1.fd = slow1[1];
	rx2.fd = slow2[1];
	require(!pthread_create(&reader1, NULL, slow_reader, &rx1) &&
		!pthread_create(&reader2, NULL, slow_reader, &rx2), "slow reply readers");
	require(wait_tag_ms(&r, first, 30000).res == (int)total &&
		wait_tag_ms(&r, second, 30000).res == (int)total, "blocked replies complete");
	pthread_join(reader1, NULL);
	pthread_join(reader2, NULL);
	require(rx1.done == total && rx2.done == total &&
		!memcmp(seen1, prefix, sizeof(prefix)) &&
		!memcmp(seen1 + sizeof(prefix), data, length) &&
		!memcmp(seen2, seen1, total), "blocked reply wire bytes");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"reply floor close");
	/* A send's framing is released when its request is freed, just after its CQE. */
	for (i = 0; i < 1000; i++) {
		require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res,
			"reply floor stat");
		if (!stat.framing_bytes)
			break;
		usleep(1000);
	}
	require(!stat.framing_bytes, "reply floor framing drains");
	ring_exit(&r);
	close(source[0]);
	close(source[1]);
	close(slow1[0]);
	close(slow1[1]);
	close(slow2[0]);
	close(slow2[1]);
	close(target[0]);
	close(target[1]);
	free(data);
	free(seen1);
	free(seen2);
	ksft_test_result_pass("a reply reserve is a floor for framing, not a cap\n");
}

static size_t drain_idle(int fd, unsigned char *buf, size_t cap, int idle_ms)
{
	size_t done = 0;

	while (done < cap) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		ssize_t n;

		if (poll(&p, 1, idle_ms) <= 0)
			break;
		n = recv(fd, buf + done, cap - done, MSG_DONTWAIT);
		require(n > 0, "drain queued wire bytes");
		done += n;
	}
	return done;
}

static void test_waiting_follower_cancel(struct io_uring_opaque_config *cfg)
{
	const size_t length = 300U << 10;
	unsigned char *data = malloc(length), *wire = malloc(length + 16);
	unsigned int mode, round;

	require(data && wire, "two-follower buffers");
	memset(data, 'A', length);
	for (mode = 0; mode < 4; mode++) {
		bool late = mode & 1, shared = mode & 2;

		for (round = 0; round < 5; round++) {
			struct ring head, other, *followers = &head;
			struct event event, first, second;
			uint32_t context, follower_context;
			uint64_t stream, handles[3], tags[3];
			struct io_uring_sqe cancel = { .opcode = IORING_OP_ASYNC_CANCEL, .fd = -1 };
			int source[2], target[2], big = 8U << 20;
			size_t got;

			require(!ring_init(&head, IORING_SETUP_CQE32), "two-follower head ring");
			require(!register_store(&head, cfg, &context), "two-follower store");
			follower_context = context;
			if (shared) {
				int fd = export_store(&head, context);

				require(fd >= 0 && !ring_init(&other, IORING_SETUP_CQE32) &&
					!import_store(&other, fd, &follower_context),
					"two-follower import");
				close(fd);
				followers = &other;
			}
			tcp_pair(source);
			tcp_pair_buffers(target, 16384, 16384);
			stream = attach(&head, context, source[1], next_tag++);
			handles[0] = receive_complete(&head, context, stream, source[0], 0,
						      data, length).extra[0];
			handles[1] = receive_complete(&head, context, stream, source[0], length,
						      "BBBBBBBB", 8).extra[0];
			handles[2] = receive_complete(&head, context, stream, source[0], length + 8,
						      "CCCCCCCC", 8).extra[0];
			tags[0] = send_stage(&head, context, target[0], handles[0], 0, length);
			if (shared)
				submit_only(&head);
			tags[1] = send_stage_flags(followers, follower_context, target[0],
						   handles[1], 0, 8, IORING_OPAQUE_SEND_LAST, 0, 0);
			tags[2] = send_stage_flags(followers, follower_context, target[0],
						   handles[2], 0, 8, IORING_OPAQUE_SEND_LAST, 0, 0);
			submit_only(followers);
			usleep(50000);
			cancel.addr = tags[1];
			cancel.user_data = next_tag++;
			if (!late) {
				stage(followers, cancel);
				require(!wait_tag_ms(followers, cancel.user_data, 5000).res,
					"early waiting follower cancel");
				first = wait_tag_ms(followers, tags[1], 5000);
			}
			if (setsockopt(target[0], SOL_SOCKET, SO_SNDBUFFORCE, &big, sizeof(big)))
				require(!setsockopt(target[0], SOL_SOCKET, SO_SNDBUF,
						    &big, sizeof(big)),
					"two-follower send capacity");
			got = drain_idle(target[1], wire, length + 16, 100);
			usleep(50000);
			if (late) {
				stage(followers, cancel);
				submit_only(followers);
			}
			event = wait_tag_ms(&head, tags[0], 10000);
			require(event.res == (int)length, "two-follower complete head");
			if (late) {
				require(!wait_tag_ms(followers, cancel.user_data, 5000).res,
					"late waiting follower cancel");
				first = wait_tag_ms(followers, tags[1], 5000);
			}
			second = wait_tag_ms(followers, tags[2], 10000);
			require(first.res == -ECANCELED && second.res == 8 &&
				(first.flags & IORING_CQE_F_OPAQUE_CONSUMED) &&
				(second.flags & IORING_CQE_F_OPAQUE_CONSUMED),
				"canceling a waiting follower preserves the next send");
			got += drain_idle(target[1], wire + got, length + 16 - got, 1000);
			require(got == length + 8 && !memcmp(data, wire, length) &&
				!memcmp(wire + length, "CCCCCCCC", 8), "two-follower wire order");
			require(!command(&head, context, IORING_OPAQUE_FREE,
					 handles[0], 0, 0, NULL).res &&
				!command(&head, context, IORING_OPAQUE_STREAM_CLOSE,
					 stream, 0, 0, NULL).res,
				"two-follower cleanup");
			if (shared)
				ring_exit(&other);
			ring_exit(&head);
			close(source[0]);
			close(source[1]);
			close(target[0]);
			close(target[1]);
		}
		ksft_test_result_pass("waiting SEND cancel: %s, %s (5 rounds)\n",
				      shared ? "shared rings" : "one ring",
				      late ? "head cleanup first" : "cancel first");
	}
	free(data);
	free(wire);
}

static void test_lowmem(struct io_uring_opaque_config cfg)
{
	const size_t retained = 128U << 10, discarded = 2U << 20, length = 8192;
	static const char header[] = "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n";
	struct io_uring_opaque_frame frame = { .size = sizeof(frame),
		.prefix = (uintptr_t)"HDR", .prefix_length = 3,
		.suffix = (uintptr_t)"!", .suffix_length = 1 };
	struct io_uring_opaque_object_stat object;
	struct io_uring_opaque_stat stat;
	struct ring r, imported;
	struct event event;
	unsigned char *data = malloc(discarded), *wire = malloc(length + 4);
	uint32_t context, other_context;
	uint64_t stream, cache, forward, offset = sizeof(header) - 1, tag;
	struct transfer tx = { .data = data, .length = discarded };
	pthread_t producer;
	int source[2], target[2], fd;

	require(data && wire, "lowmem buffers");
	memset(data, 'D', discarded);
	cfg.hard_limit = 512U << 10;
	cfg.compact_headroom = 64U << 10;
	cfg.reply_reserve = 64U << 10;
	cfg.high_watermark = 64U << 10;
	cfg.low_watermark = 32U << 10;
	require(!ring_init(&r, IORING_SETUP_CQE32) && !register_store(&r, &cfg, &context),
		"watermark store");
	fd = export_store(&r, context);
	require(fd >= 0 && !ring_init(&imported, IORING_SETUP_CQE32) &&
		!import_store(&imported, fd, &other_context), "watermark shared store");
	close(fd);
	tcp_pair(source);
	tcp_pair(target);
	stream = attach(&r, context, source[1], next_tag++);
	event = receive_complete(&r, context, stream, source[0], 0, data, retained);
	cache = event.extra[0];
	require(!event.extra[1] &&
		!command(&imported, other_context, IORING_OPAQUE_STAT,
			 0, 0, sizeof(stat), &stat).res &&
		stat.memory_mode == IORING_OPAQUE_MEMORY_LOWMEM, "shared lowmem transition");
	tag = cmd_stage(&r, context, IORING_OPAQUE_INSPECT, stream, retained, 512, wire);
	write_all(source[0], header, sizeof(header) - 1);
	event = wait_tag_ms(&r, tag, 5000);
	require(event.res == (int)sizeof(header) - 1 && !memcmp(wire, header, event.res),
		"lowmem available header inspection");
	require(command(&r, context, IORING_OPAQUE_DISCARD, stream, retained, offset, NULL).res ==
		(int)offset, "lowmem header disposition");
	event = receive_complete(&r, context, stream, source[0], retained + offset, data, length);
	forward = event.extra[0];
	offset += length;
	require(event.extra[1] == IORING_OPAQUE_OBJECT_F_FORWARD_ONLY,
		"lowmem object-ready reports forwarding-only classification");
	require(!command(&r, context, IORING_OPAQUE_OBJECT_STAT, forward, 0,
			 sizeof(object), &object).res &&
		(object.flags & IORING_OPAQUE_OBJECT_F_FORWARD_ONLY), "lowmem object flags");
	tag = cmd_stage(&r, context, IORING_OPAQUE_DISCARD, stream,
			retained + offset, discarded, NULL);
	tx.fd = source[0];
	require(!pthread_create(&producer, NULL, writer, &tx), "lowmem DISCARD producer");
	require(wait_tag_ms(&r, tag, 30000).res == (int)discarded, "lowmem direct body DISCARD");
	pthread_join(producer, NULL);
	offset += discarded;
	ksft_test_result_pass("lowmem parses short headers and discards a 2 MiB body\n");
	tag = send_stage(&imported, other_context, target[0], forward, 0, length);
	require(wait_tag_ms(&imported, tag, 5000).res == (int)length,
		"forwarding-only classification is advisory for SEND");
	read_all(target[1], wire, length);
	require(!memcmp(wire, data, length), "advisory SEND wire bytes");
	require(command(&r, context, IORING_OPAQUE_READ_OBJECT, cache, 0, 8, wire).res == 8,
		"lowmem preserves existing cache entries");
	require(!command(&r, context, IORING_OPAQUE_FREE, cache, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res,
		"evict cache backing");
	ksft_print_msg("forward-only backing after eviction: %llu bytes, mode %u\n",
		       (unsigned long long)stat.backing_bytes, stat.memory_mode);
	/* A short object can pin an RX allocation larger than the low watermark. */
	require(!command(&r, context, IORING_OPAQUE_COMPACT, forward, 0, 0, NULL).res,
		"forwarding object compaction");
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		stat.backing_bytes <= cfg.low_watermark &&
		stat.memory_mode == IORING_OPAQUE_MEMORY_NORMAL, "low watermark resumes normal");
	require(!command(&r, context, IORING_OPAQUE_OBJECT_STAT, forward, 0,
			 sizeof(object), &object).res &&
		(object.flags & IORING_OPAQUE_OBJECT_F_FORWARD_ONLY),
		"classification survives normal mode and compaction");
	tag = send_stage(&r, context, target[0], forward, 0, length);
	require(wait_tag_ms(&r, tag, 5000).res == (int)length, "advisory fan-out SEND");
	read_all(target[1], wire, length);
	tag = frame_stage(&r, context, target[0], forward, length, &frame,
			  IORING_OPAQUE_SEND_LAST, 0);
	event = wait_tag_ms(&r, tag, 5000);
	require(event.res == (int)length + 4 && (event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
		"lowmem framed final send");
	read_all(target[1], wire, length + 4);
	require(!memcmp(wire, "HDR", 3) && !memcmp(wire + 3, data, length) &&
		wire[length + 3] == '!',
		"lowmem complete reply wire bytes");
	require(command(&r, context, IORING_OPAQUE_FREE, forward, 0, 0, NULL).res == -ESTALE,
		"forwarding handle consumed");
	ksft_test_result_pass("forwarding-only flag survives mode changes and compaction\n");
	event = receive_complete(&r, context, stream, source[0], retained + offset, "normal!!", 8);
	require(!event.extra[1], "normal mode admits cacheable object");
	tag = send_stage(&r, context, target[0], event.extra[0], 0, 8);
	require(wait_tag_ms(&r, tag, 5000).res == 8, "normal reusable SEND");
	read_all(target[1], wire, 8);
	require(!memcmp(wire, "normal!!", 8) &&
		!command(&r, context, IORING_OPAQUE_FREE, event.extra[0], 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		!stat.backing_bytes && !stat.framing_bytes && !stat.objects && !stat.extents,
		"normal mode and forwarding references drain");
	ring_exit(&imported);
	ring_exit(&r);
	close(source[0]);
	close(source[1]);
	close(target[0]);
	close(target[1]);
	free(data);
	free(wire);
	ksft_test_result_pass("normal mode resumes cache retention below the low watermark\n");
}

static void test_reply_reserve(struct io_uring_opaque_config cfg)
{
	struct io_uring_opaque_frame frame = { .size = sizeof(frame),
		.prefix = (uintptr_t)"HDR:", .prefix_length = 4 };
	struct io_uring_opaque_stat stat;
	struct io_uring_opaque_stream_stat before, after;
	struct ring r;
	struct event event;
	uint32_t context;
	uint64_t greedy, responder, request, cached, collector = next_tag++, tag;
	unsigned char *flood = calloc(1, 256U << 10), wire[16];
	unsigned int attempt;
	int a[2], b[2], target[2], unread = 0, big = 1 << 20;
	uint64_t limit;

	cfg.hard_limit = 256U << 10;
	cfg.compact_headroom = 64U << 10;
	cfg.reply_reserve = sysconf(_SC_PAGESIZE);
	limit = cfg.hard_limit - cfg.compact_headroom - cfg.reply_reserve;
	require(flood && !ring_init(&r, IORING_SETUP_CQE32) && !register_store(&r, &cfg, &context),
		"reply reserve store");
	tcp_pair(a);
	tcp_pair(b);
	tcp_pair(target);
	require(!setsockopt(a[1], SOL_SOCKET, SO_RCVBUF, &big, sizeof(big)),
		"greedy receive buffer");
	responder = attach(&r, context, b[1], next_tag++);
	request = receive_complete(&r, context, responder, b[0], 0, "request!", 8).extra[0];
	cached = receive_complete(&r, context, responder, b[0], 8, "cached!!", 8).extra[0];
	greedy = attach(&r, context, a[1], collector);
	write_all(a[0], flood, 256U << 10);
	/* The collector runs only when the ring is entered: let TCP queue the flood first. */
	for (attempt = 0; attempt < 5000; attempt++) {
		require(!ioctl(a[1], FIONREAD, &unread), "TCP unread count");
		if (unread >= (int)limit + 4096)
			break;
		usleep(1000);
	}
	require(unread >= (int)limit + 4096, "flood queued on TCP before collection");
	event = wait_tag_ms(&r, collector, 5000);
	require(event.res == -ENOBUFS && (event.flags & IORING_CQE_F_MORE) &&
		event.extra[0] == greedy, "first collection pass fills the allowance");
	require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, greedy, 0,
			 sizeof(before), &before).res &&
		!ioctl(a[1], FIONREAD, &unread) && unread > 0, "excess data stays on TCP");
	require(!command(&r, context, IORING_OPAQUE_FREE, cached, 0, 0, NULL).res,
		"evict cached object");
	usleep(100000);
	enter(&r, 0);
	require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, greedy, 0,
			 sizeof(after), &after).res &&
		!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		after.undecided_bytes == before.undecided_bytes && stat.backing_bytes < limit,
		"held flooding stream does not refill freed capacity");
	{
		/* Framing larger than free capture capacity and the reserve together. */
		struct io_uring_opaque_frame large = { .size = sizeof(large),
			.prefix = (uintptr_t)flood, .prefix_length = IORING_OPAQUE_FRAME_MAX };

		tag = frame_stage(&r, context, target[0], request, 8, &large,
				  IORING_OPAQUE_SEND_LAST, 0);
		event = wait_tag_ms(&r, tag, 5000);
		require(event.res == -ENOBUFS && !(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
			"framing quota exhaustion preserves LAST ownership");
	}
	tag = frame_stage(&r, context, target[0], request, 8, &frame, IORING_OPAQUE_SEND_LAST, 0);
	event = wait_tag_ms(&r, tag, 5000);
	require(event.res == 12 && (event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
		"other connection admits framed reply under capture pressure");
	tag = frame_stage(&r, context, target[0], 0, 0, &frame, 0, 0);
	require(wait_tag_ms(&r, tag, 5000).res == 4, "generated reply under capture pressure");
	read_all(target[1], wire, sizeof(wire));
	require(!memcmp(wire, "HDR:request!HDR:", sizeof(wire)) &&
		!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, greedy, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, responder, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		!stat.backing_bytes && !stat.framing_bytes,
		"reserved reply wire bytes and release");
	ring_exit(&r);
	close(a[0]);
	close(a[1]);
	close(b[0]);
	close(b[1]);
	close(target[0]);
	close(target[1]);
	free(flood);
	ksft_test_result_pass("two connections retain reply progress with reserved framing\n");
}

static void test_watermark_config(struct io_uring_opaque_config cfg)
{
	struct io_uring_opaque_config bad;
	struct ring r;
	uint32_t context;
	unsigned int i;

	require(!ring_init(&r, IORING_SETUP_CQE32), "watermark ABI ring");
	cfg.reply_reserve = 64U << 10;
	cfg.high_watermark = 32U << 20;
	cfg.low_watermark = 16U << 20;
	for (i = 0; i < 7; i++) {
		bad = cfg;
		switch (i) {
		case 0:
			bad.low_watermark = bad.high_watermark;
			break;
		case 1:
			bad.low_watermark = 1;
			break;
		case 2:
			bad.high_watermark = 0;
			break;
		case 3:
			bad.reply_reserve = bad.hard_limit;
			break;
		case 4:
			bad.reply_reserve = 0;
			break;
		case 5:
			bad.zcrx_id = 1;
			break;
		case 6:
			bad.__resv1 = 1;
			break;
		}
		require(register_store(&r, &bad, &context) == -EINVAL,
			"reject invalid watermark configuration");
	}
	require(!register_store(&r, &cfg, &context),
		"valid watermark configuration registers");
	ring_exit(&r);
	ksft_test_result_pass("watermark and reply reserve ABI validation\n");
}

static void create_error(struct ring *r, uint32_t context, uint64_t handle,
			 uint64_t off, unsigned int length, void *ptr, int error)
{
	struct event event = command(r, context, IORING_OPAQUE_CREATE,
				     handle, off, length, ptr);

	require(event.res == error && !event.extra[0] && !event.extra[1] &&
		!event.flags, "CREATE error has no handle or ownership flags");
}

static void create_drained(struct ring *r, uint32_t context)
{
	struct io_uring_opaque_stat stat;
	unsigned int i;

	for (i = 0; i < 1000; i++) {
		require(!command(r, context, IORING_OPAQUE_STAT, 0, 0,
				 sizeof(stat), &stat).res, "CREATE drain STAT");
		if (!stat.backing_bytes && !stat.extents && !stat.objects && !stat.requests)
			return;
		usleep(1000);
	}
	require(false, "CREATE backing, extents, objects and requests drain");
}

static void test_create(struct io_uring_opaque_config cfg)
{
	struct ring r;
	struct event event;
	uint32_t context;
	unsigned char source[3] = { 1, 2, 3 }, dest[3];
	uint64_t handle;

	require(!ring_init(&r, IORING_SETUP_CQE32), "CREATE ring");
	require(!register_store(&r, &cfg, &context), "CREATE store");
	event = command(&r, context, IORING_OPAQUE_CREATE, 0, 0, sizeof(source), source);
	ksft_print_msg("CREATE result=%d handle=%llu\n", event.res,
		       (unsigned long long)event.extra[0]);
	require(event.res == sizeof(source) && event.extra[0] && !event.flags &&
		!event.extra[1], "CREATE complete object without TCP");
	handle = event.extra[0];
	memset(source, 9, sizeof(source));
	require(command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0,
			sizeof(dest), dest).res == sizeof(dest) &&
		dest[0] == 1 && dest[1] == 2 && dest[2] == 3,
		"CREATE source mutation does not change object");
	require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res,
		"CREATE FREE");
	create_drained(&r, context);
	{
		struct io_uring_sqe sqe = {
			.opcode = IORING_OP_OPAQUE_OBJ, .fd = -1,
			.ioprio = IORING_OPAQUE_CREATE, .opaque_store_id = context,
			.len = sizeof(source), .addr3 = (uintptr_t)source,
			.flags = IOSQE_ASYNC, .user_data = next_tag++,
		};

		stage(&r, sqe);
		event = wait_tag_ms(&r, sqe.user_data, 5000);
		require(event.res == sizeof(source) && event.extra[0] && !event.flags,
			"CREATE explicit ASYNC succeeds");
		require(command(&r, context, IORING_OPAQUE_READ_OBJECT, event.extra[0],
				0, sizeof(dest), dest).res == sizeof(dest) &&
			!memcmp(source, dest, sizeof(dest)), "CREATE ASYNC owns copied bytes");
		require(!command(&r, context, IORING_OPAQUE_FREE, event.extra[0],
				 0, 0, NULL).res, "CREATE ASYNC FREE");
		create_drained(&r, context);
	}
	ksft_test_result_pass("CREATE owns immutable copied bytes without TCP\n");
	{
		size_t page = sysconf(_SC_PAGESIZE), i, j;
		unsigned int lengths[] = { 1, page - 1, page, page + 1 };
		unsigned char *input = malloc(3 * page), *output = malloc(2 * page);
		struct io_uring_opaque_object_stat stat;

		require(input && output, "CREATE boundary buffers");
		for (j = 0; j < 3 * page; j++)
			input[j] = j % 251;
		for (i = 0; i < ARRAY_SIZE(lengths); i++) {
			event = command(&r, context, IORING_OPAQUE_CREATE, 0, 0,
					lengths[i], input + page - 1);
			require(event.res == (int)lengths[i] && event.extra[0],
				"CREATE unaligned cross-page source");
			handle = event.extra[0];
			require(command(&r, context, IORING_OPAQUE_READ_OBJECT, handle,
					0, lengths[i], output).res == (int)lengths[i] &&
				!memcmp(output, input + page - 1, lengths[i]),
				"CREATE boundary bytes");
			require(!command(&r, context, IORING_OPAQUE_OBJECT_STAT, handle,
					 0, sizeof(stat), &stat).res &&
				stat.backing_bytes == (lengths[i] + page - 1) / page * page &&
				(stat.flags & IORING_OPAQUE_OBJECT_F_COMPACTED),
				"CREATE dense page-rounded backing");
			require(!command(&r, context, IORING_OPAQUE_COMPACT, handle,
					 0, 0, NULL).res, "CREATE dense COMPACT no-op");
			require(!command(&r, context, IORING_OPAQUE_FREE, handle,
					 0, 0, NULL).res, "CREATE boundary FREE");
		}
		free(input);
		free(output);
		create_drained(&r, context);
	}
	ksft_test_result_pass("CREATE boundary lengths, unaligned pages and dense backing\n");
	{
		size_t page = sysconf(_SC_PAGESIZE);
		void *input = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		struct io_uring_opaque_stat before, after;

		require(input != MAP_FAILED, "CREATE fault mapping");
		memset(input, 0x67, page);
		require(!mprotect(input + page, page, PROT_NONE), "CREATE protected page");
		create_error(&r, context, 0, 0, 0, input, -EINVAL);
		create_error(&r, context, 0, 0, 1, NULL, -EINVAL);
		create_error(&r, context, 1, 0, 1, input, -EINVAL);
		create_error(&r, context, 0, 1, 1, input, -EINVAL);
		create_error(&r, context, 0, 0, UINT_MAX, input, -EINVAL);
		create_error(&r, context, 0, 0, INT_MAX, input, -EMSGSIZE);
		create_error(&r, context, 0, 0, cfg.max_object_size + 1, input, -EMSGSIZE);
		create_error(&r, context, 0, 0, 2, (void *)(uintptr_t)UINT64_MAX, -EFAULT);
		for (unsigned int i = 0; i < 6; i++) {
			struct io_uring_sqe sqe = {
				.opcode = IORING_OP_OPAQUE_OBJ, .fd = -1,
				.ioprio = IORING_OPAQUE_CREATE, .opaque_store_id = context,
				.len = 1, .addr3 = (uintptr_t)input, .user_data = next_tag++,
			};

			switch (i) {
			case 0:
				sqe.fd = 0;
				break;
			case 1:
				sqe.rw_flags = 1;
				break;
			case 2:
				sqe.buf_index = 1;
				break;
			case 3:
				sqe.__pad2[0] = 1;
				break;
			case 4:
				sqe.flags = IOSQE_FIXED_FILE;
				break;
			case 5:
				sqe.flags = IOSQE_CQE_SKIP_SUCCESS;
				break;
			}
			stage(&r, sqe);
			event = wait_tag(&r, sqe.user_data);
			require(event.res == -EINVAL && !event.extra[0] && !event.extra[1],
				"CREATE rejects unused fields and CQE suppression");
		}
		create_error(&r, context, 0, 0, 1, input + page, -EFAULT);
		require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0,
				 sizeof(before), &before).res, "CREATE copied before fault");
		create_error(&r, context, 0, 0, page + 1, input, -EFAULT);
		create_drained(&r, context);
		require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0,
				 sizeof(after), &after).res &&
			after.copied_bytes - before.copied_bytes == page,
			"CREATE counts copied prefix on failed input");
		munmap(input, 2 * page);
	}
	ksft_test_result_pass("CREATE validates inputs and unwinds partial user faults\n");
	{
		struct ring imported;
		uint32_t imported_context;
		int fd = export_store(&r, context), target[2], i;
		uint64_t tag;
		unsigned char expected[3] = { 9, 9, 9 };

		require(fd >= 0 && !ring_init(&imported, IORING_SETUP_CQE32),
			"CREATE shared ring");
		require(!import_store(&imported, fd, &imported_context), "CREATE import");
		close(fd);
		event = command(&r, context, IORING_OPAQUE_CREATE, 0, 0, 3, source);
		require(event.res == 3 && event.extra[0], "CREATE send object");
		handle = event.extra[0];
		for (i = 0; i < 3; i++) {
			/* Separate destinations exercise the shared store's send path. */
			tcp_pair(target);
			tag = send_stage_flags(&imported, imported_context, target[0], handle,
					       0, 3, i == 2 ? IORING_OPAQUE_SEND_LAST : 0, 0, 0);
			event = wait_tag(&imported, tag);
			require(event.res == 3 &&
				!!(event.flags & IORING_CQE_F_OPAQUE_CONSUMED) == (i == 2),
				"CREATE reusable sends then consuming LAST");
			require(recv(target[1], dest, 3, MSG_WAITALL) == 3 &&
				!memcmp(dest, expected, 3), "CREATE send wire bytes");
			close(target[0]);
			close(target[1]);
		}
		require(command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res ==
			-ESTALE, "CREATE LAST invalidates shared handle");
		ring_exit(&imported);
		create_drained(&r, context);
	}
	ksft_test_result_pass("CREATE shared imports, repeated sends and LAST ownership\n");
	ring_exit(&r);
}

static void test_create_quota(struct io_uring_opaque_config cfg)
{
	struct ring r;
	struct event event;
	struct io_uring_opaque_stat stat;
	uint32_t context;
	uint64_t handles[4];
	unsigned char source = 42;
	unsigned int i;
	size_t page = sysconf(_SC_PAGESIZE);

	cfg.hard_limit = 6 * page;
	cfg.compact_headroom = page;
	cfg.reply_reserve = page;
	cfg.high_watermark = 2 * page;
	cfg.low_watermark = page;
	cfg.max_object_size = 8 * page;
	cfg.max_objects = 8;
	cfg.max_extents = 4;
	require(!ring_init(&r, IORING_SETUP_CQE32), "CREATE quota ring");
	require(!register_store(&r, &cfg, &context), "CREATE quota store");
	create_error(&r, context, 0, 0, 4 * page + 1, &source, -EMSGSIZE);
	for (i = 0; i < 4; i++) {
		event = command(&r, context, IORING_OPAQUE_CREATE, 0, 0, 1, &source);
		require(event.res == 1 && event.extra[0] &&
			event.extra[1] == (i >= 2 ? IORING_OPAQUE_OBJECT_F_FORWARD_ONLY : 0),
			"CREATE samples lowmem before charging payload");
		handles[i] = event.extra[0];
	}
	create_error(&r, context, 0, 0, 1, &source, -ENOBUFS);
	{
		struct io_uring_opaque_frame frame = {
			.size = sizeof(frame), .prefix = (uintptr_t)&source, .prefix_length = 1,
		};
		int target[2];
		unsigned char received;

		tcp_pair(target);
		event = wait_tag(&r, frame_stage(&r, context, target[0], 0, 0, &frame, 0, 0));
		require(event.res == 1 && recv(target[1], &received, 1, MSG_WAITALL) == 1 &&
			received == source, "CREATE cannot consume protected reply reserve");
		close(target[0]);
		close(target[1]);
		for (i = 0; i < 1000; i++) {
			require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0,
					 sizeof(stat), &stat).res, "CREATE framing drain");
			if (!stat.framing_bytes)
				break;
			usleep(1000);
		}
		require(!stat.framing_bytes, "CREATE protected reply charge drains");
	}
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0,
			 sizeof(stat), &stat).res && stat.backing_bytes == 4 * page &&
		stat.copied_bytes == 4 && stat.memory_mode == IORING_OPAQUE_MEMORY_LOWMEM,
		"CREATE payload quota and copied counter");
	for (i = 0; i < 4; i++)
		require(!command(&r, context, IORING_OPAQUE_FREE, handles[i],
				 0, 0, NULL).res, "CREATE quota FREE");
	create_drained(&r, context);
	ring_exit(&r);
	ksft_test_result_pass("CREATE capture allowance, lowmem classification and quota unwind\n");

	cfg.hard_limit = 32 * page;
	require(!ring_init(&r, IORING_SETUP_CQE32), "CREATE extent ring");
	require(!register_store(&r, &cfg, &context), "CREATE extent store");
	for (i = 0; i < 4; i++) {
		event = command(&r, context, IORING_OPAQUE_CREATE, 0, 0, 1, &source);
		require(event.res == 1 && event.extra[0], "CREATE extent fixture");
		handles[i] = event.extra[0];
	}
	create_error(&r, context, 0, 0, 1, &source, -ENOBUFS);
	for (i = 0; i < 4; i++)
		require(!command(&r, context, IORING_OPAQUE_FREE, handles[i],
				 0, 0, NULL).res, "CREATE extent FREE");
	create_drained(&r, context);
	ring_exit(&r);

	cfg.max_objects = 1;
	require(!ring_init(&r, IORING_SETUP_CQE32), "CREATE slot ring");
	require(!register_store(&r, &cfg, &context), "CREATE slot store");
	event = command(&r, context, IORING_OPAQUE_CREATE, 0, 0, 1, &source);
	require(event.res == 1, "CREATE slot object");
	handles[0] = event.extra[0];
	create_error(&r, context, 0, 0, 1, &source, -ENOSPC);
	require(!command(&r, context, IORING_OPAQUE_FREE, handles[0], 0, 0, NULL).res,
		"CREATE slot release");
	create_drained(&r, context);
	ring_exit(&r);

	cfg.max_objects = 8;
	cfg.max_requests = 1;
	require(!ring_init(&r, IORING_SETUP_CQE32), "CREATE request cap ring");
	require(!register_store(&r, &cfg, &context), "CREATE request cap store");
	handles[0] = cmd_stage(&r, context, IORING_OPAQUE_CREATE, 0, 0, 1, &source);
	handles[1] = cmd_stage(&r, context, IORING_OPAQUE_CREATE, 0, 0, 1, &source);
	event = wait_tag(&r, handles[1]);
	require(event.res == -EAGAIN && !event.extra[0] && !event.extra[1],
		"CREATE counts against request cap");
	event = wait_tag(&r, handles[0]);
	require(event.res == 1 && event.extra[0], "CREATE admitted request completes");
	require(!command(&r, context, IORING_OPAQUE_FREE, event.extra[0], 0, 0, NULL).res,
		"CREATE request cap FREE");
	create_drained(&r, context);
	ring_exit(&r);
	ksft_test_result_pass("CREATE slot exhaustion and counted request admission\n");
}

static void test_create_cancel(struct io_uring_opaque_config cfg)
{
	struct ring r;
	uint32_t context;
	unsigned int i, canceled = 0;
	unsigned char *source = malloc(cfg.max_object_size);

	require(source, "CREATE cancel source");
	memset(source, 0x53, cfg.max_object_size);
	require(!ring_init(&r, IORING_SETUP_CQE32), "CREATE cancel ring");
	require(!register_store(&r, &cfg, &context), "CREATE cancel store");
	for (i = 0; i < 64; i++) {
		struct io_uring_sqe create = {
			.opcode = IORING_OP_OPAQUE_OBJ, .fd = -1,
			.ioprio = IORING_OPAQUE_CREATE, .opaque_store_id = context,
			.len = cfg.max_object_size, .addr3 = (uintptr_t)source,
			.flags = (i & 1 ? IOSQE_ASYNC : 0) | IOSQE_IO_LINK,
			.user_data = next_tag++,
		};
		struct io_uring_sqe successor = {
			.opcode = IORING_OP_NOP, .fd = -1, .user_data = next_tag++,
		};
		struct io_uring_sqe cancel = {
			.opcode = IORING_OP_ASYNC_CANCEL, .fd = -1,
			.addr = create.user_data, .user_data = next_tag++,
		};
		struct event done, canceled_event, next;

		stage(&r, create);
		stage(&r, successor);
		stage(&r, cancel);
		canceled_event = wait_tag(&r, cancel.user_data);
		require(canceled_event.res == 0 || canceled_event.res == -ENOENT ||
			canceled_event.res == -EALREADY, "CREATE cancellation arbitration");
		done = wait_tag(&r, create.user_data);
		next = wait_tag(&r, successor.user_data);
		if (done.res == -ECANCELED) {
			canceled++;
			require(!done.extra[0] && !done.extra[1] && !done.flags &&
				next.res == -ECANCELED,
				"CREATE canceled without publication or successor");
		} else {
			require(done.res == (int)cfg.max_object_size && done.extra[0] &&
				!done.flags && canceled_event.res != 0 && !next.res,
				"CREATE completion wins cancellation race");
			require(!command(&r, context, IORING_OPAQUE_FREE, done.extra[0],
					 0, 0, NULL).res, "CREATE race FREE");
		}
		create_drained(&r, context);
		enter(&r, 0);
		require(!r.nr_saved && !peek(&r, &done), "CREATE exactly one CQE per request");
	}
	ksft_print_msg("CREATE races: canceled=%u completed=%u\n", canceled, 64 - canceled);
	ring_exit(&r);
	for (i = 0; i < 8; i++) {
		pid_t pid = fork();
		int status;

		require(pid >= 0, "CREATE teardown fork");
		if (!pid) {
			require(!ring_init(&r, IORING_SETUP_CQE32), "CREATE teardown ring");
			require(!register_store(&r, &cfg, &context), "CREATE teardown store");
			for (unsigned int j = 0; j < 32; j++)
				cmd_stage(&r, context, IORING_OPAQUE_CREATE, 0, 0,
					  cfg.max_object_size, source);
			enter(&r, 0);
			ring_exit(&r);
			/* Keep the source mapped through task/io-wq teardown. */
			_exit(0);
		}
		require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
			!WEXITSTATUS(status), "CREATE teardown child drains");
	}
	free(source);
	ksft_test_result_pass("CREATE inline/ASYNC completion, pre-issue cancel and drain\n");
}

static void create_slot_reuse(struct ring *r, uint32_t context)
{
	unsigned char source = 0x63;
	struct event event;

	create_drained(r, context);
	event = command(r, context, IORING_OPAQUE_CREATE, 0, 0, 1, &source);
	require(event.res == 1 && event.extra[0], "CREATE failed request's only slot is reusable");
	require(!command(r, context, IORING_OPAQUE_FREE, event.extra[0], 0, 0, NULL).res,
		"CREATE reused slot FREE");
	create_drained(r, context);
}

static const char * const create_fault_names[] = {
	"probability", "interval", "times", "space", "task-filter", "require-start",
	"require-end", "reject-start", "reject-end", "stacktrace-depth",
	"ignore-gfp-wait", "verbose",
};

struct create_fault_settings {
	const char *kind;
	char saved[ARRAY_SIZE(create_fault_names)][64];
	char extra[2][64];
};

static const char *create_fault_extra(const char *kind, unsigned int index)
{
	if (!strcmp(kind, "failslab"))
		return index ? NULL : "cache-filter";
	return index ? "min-order" : "ignore-gfp-highmem";
}

static bool create_fault_save(struct create_fault_settings *s, const char *kind)
{
	char path[128];
	FILE *file;
	unsigned int i;

	s->kind = kind;
	if (geteuid())
		return false;
	for (i = 0; i < ARRAY_SIZE(create_fault_names) + 2; i++) {
		const char *name = i < ARRAY_SIZE(create_fault_names) ? create_fault_names[i] :
			create_fault_extra(kind, i - ARRAY_SIZE(create_fault_names));
		char *saved = i < ARRAY_SIZE(create_fault_names) ? s->saved[i] :
			s->extra[i - ARRAY_SIZE(create_fault_names)];

		if (!name)
			continue;
		snprintf(path, sizeof(path), "/sys/kernel/debug/%s/%s", kind, name);
		if (access(path, W_OK))
			return false;
		file = fopen(path, "r");
		require(file && fgets(saved, 64, file), "save CREATE fault settings");
		fclose(file);
	}
	return strtoul(s->saved[0], NULL, 10) == 0;
}

static void create_fault_setting(struct create_fault_settings *s,
				 const char *name, const char *value)
{
	char path[128];

	snprintf(path, sizeof(path), "/sys/kernel/debug/%s/%s", s->kind, name);
	write_setting(path, value);
}

/* Stack filters limit faults to the requested allocation stage. */
static void create_fault_arm(struct create_fault_settings *s, uint64_t start,
			     uint64_t end, unsigned int times,
			     unsigned int interval, unsigned int space)
{
	char value[64];

	create_fault_setting(s, "probability", "0");
	snprintf(value, sizeof(value), "%u", interval);
	create_fault_setting(s, "interval", value);
	snprintf(value, sizeof(value), "%u", space);
	create_fault_setting(s, "space", value);
	create_fault_setting(s, "task-filter", "0");
	create_fault_setting(s, "reject-start", "0");
	create_fault_setting(s, "reject-end", "0");
	create_fault_setting(s, "stacktrace-depth", "32");
	create_fault_setting(s, "ignore-gfp-wait", "0");
	create_fault_setting(s, "verbose", "2");
	create_fault_setting(s, create_fault_extra(s->kind, 0), "0");
	if (create_fault_extra(s->kind, 1))
		create_fault_setting(s, create_fault_extra(s->kind, 1), "0");
	snprintf(value, sizeof(value), "0x%llx", (unsigned long long)start);
	create_fault_setting(s, "require-start", value);
	snprintf(value, sizeof(value), "0x%llx", (unsigned long long)end);
	create_fault_setting(s, "require-end", value);
	snprintf(value, sizeof(value), "%u", times);
	create_fault_setting(s, "times", value);
	create_fault_setting(s, "probability", "100");
}

static unsigned int create_fault_restore(struct create_fault_settings *s, unsigned int times)
{
	char path[128];
	unsigned int i, remaining;

	create_fault_setting(s, "probability", "0");
	snprintf(path, sizeof(path), "/sys/kernel/debug/%s/times", s->kind);
	remaining = read_number(path);
	for (i = 1; i < ARRAY_SIZE(create_fault_names) + 2; i++) {
		const char *name = i < ARRAY_SIZE(create_fault_names) ? create_fault_names[i] :
			create_fault_extra(s->kind, i - ARRAY_SIZE(create_fault_names));
		const char *saved = i < ARRAY_SIZE(create_fault_names) ? s->saved[i] :
			s->extra[i - ARRAY_SIZE(create_fault_names)];

		if (name)
			create_fault_setting(s, name, saved);
	}
	create_fault_setting(s, "probability", s->saved[0]);
	return remaining < times ? times - remaining : 0;
}

static void test_create_allocation_faults(struct io_uring_opaque_config cfg)
{
	static const char * const names[] = {
		"io_opaque_data_alloc", "io_opaque_owned_page",
		"io_opaque_extent_new", "io_opaque_vector",
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct create_fault_settings settings;
		size_t page = sysconf(_SC_PAGESIZE), length = 65536 + page;
		unsigned char *source;
		struct io_uring_opaque_stat stat;
		struct ring r;
		struct event event;
		uint32_t context;
		uint64_t start, end, tag;
		unsigned int times = i == 1 ? 5 : 1, injected;

		if (!symbol_range(names[i], &start, &end) ||
		    !create_fault_save(&settings, i == 1 ? "fail_page_alloc" : "failslab")) {
			ksft_test_result_skip("CREATE %s needs allocation fault filters\n",
					      names[i]);
			continue;
		}
		cfg.max_objects = 1;
		require(!ring_init(&r, IORING_SETUP_CQE32), "CREATE allocation fault ring");
		require(!register_store(&r, &cfg, &context), "CREATE allocation fault store");
		source = malloc(length);
		require(source, "CREATE allocation fault source");
		memset(source, 0x51, length);
		/* Leave one initial chunk/extent intact, then reject its successor. */
		create_fault_arm(&settings, start, end, times, i == 2 ? 2 : 1,
				 i == 1 ? 65536 / page + 1 : 0);
		tag = cmd_stage(&r, context, IORING_OPAQUE_CREATE, 0, 0, length, source);
		event = wait_tag_ms(&r, tag, 5000);
		injected = create_fault_restore(&settings, times);
		if (event.res >= 0 && event.extra[0])
			command(&r, context, IORING_OPAQUE_FREE, event.extra[0], 0, 0, NULL);
		create_drained(&r, context);
		require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0,
				 sizeof(stat), &stat).res, "CREATE allocation fault copy counter");
		create_slot_reuse(&r, context);
		ring_exit(&r);
		free(source);
		require(injected, "CREATE fault was actually injected");
		require(event.res == -ENOMEM && !event.flags && !event.extra[0] && !event.extra[1],
			"CREATE allocation ENOMEM publishes no handle");
		require(!i ? !stat.copied_bytes :
			(i == 3 ? stat.copied_bytes == length :
			 stat.copied_bytes >= page && stat.copied_bytes < length),
			"CREATE allocation failed-prefix counter is cumulative");
		ksft_print_msg("CREATE fault: %s injected=%u copied=%llu\n", names[i],
			       injected, (unsigned long long)stat.copied_bytes);
		ksft_test_result_pass("CREATE %s ENOMEM, reservation drain and slot reuse\n",
				      names[i]);
	}
}

/*
 * Stores that bind the area of a MAIO zcrx instance. The instance uses
 * ZCRX_REG_NODEV, so every byte of a loopback stream is copied into area pages,
 * and no device is needed. The registration code follows zcrx_maio.c.
 */
#define MAIO_RQ_ENTRIES		64
#define MAIO_RQ_REGION_SIZE	8192

struct maio {
	struct io_uring_zcrx_ifq_reg reg;
	struct io_uring_zcrx_area_reg area;
	struct io_uring_region_desc rd;
	uint32_t id;
	unsigned char *map;
};

/* False when the kernel has no MAIO mode, or the task may not use it. */
static bool maio_ok;
/* Why the probe failed. -EOPNOTSUPP: no zero-copy receive. -EPERM: no CAP_NET_ADMIN. */
static int maio_err;

/* Register an instance on the device @ifindex, or without a device if it is 0. */
static int maio_register_on(struct ring *r, struct maio *z, unsigned int ifindex,
			    unsigned int pages)
{
	int ret;

	memset(z, 0, sizeof(*z));
	z->rd.size = MAIO_RQ_REGION_SIZE;
	z->area.len = (uint64_t)pages * sysconf(_SC_PAGESIZE);
	z->area.flags = IORING_ZCRX_AREA_KERNEL;
	z->reg.if_idx = ifindex;
	z->reg.rq_entries = MAIO_RQ_ENTRIES;
	z->reg.flags = ZCRX_REG_MAIO | (ifindex ? 0 : ZCRX_REG_NODEV);
	z->reg.area_ptr = (uintptr_t)&z->area;
	z->reg.region_ptr = (uintptr_t)&z->rd;
	ret = syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_ZCRX_IFQ, &z->reg, 1);
	if (ret < 0)
		return -errno;
	z->id = z->reg.zcrx_id;
	return 0;
}

static int maio_register(struct ring *r, struct maio *z, unsigned int pages)
{
	return maio_register_on(r, z, 0, pages);
}

static void maio_map(struct ring *r, struct maio *z)
{
	z->map = mmap(NULL, z->area.len, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd,
		      z->area.mmap_offset);
	require(z->map != MAP_FAILED, "map the area read-write");
}

static void maio_unmap(struct maio *z)
{
	if (z->map)
		munmap(z->map, z->area.len);
	z->map = NULL;
}

static int maio_export(struct ring *r, struct maio *z)
{
	struct zcrx_ctrl ctrl = { .zcrx_id = z->id, .op = ZCRX_CTRL_EXPORT };
	int ret = syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_ZCRX_CTRL, &ctrl, 0);

	return ret < 0 ? -errno : (int)ctrl.zc_export.zcrx_fd;
}

static int maio_import(struct ring *r, int fd, uint32_t *id)
{
	struct io_uring_zcrx_ifq_reg reg = { .if_idx = fd, .flags = ZCRX_REG_IMPORT };
	int ret = syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_ZCRX_IFQ, &reg, 1);

	*id = reg.zcrx_id;
	return ret < 0 ? -errno : 0;
}

/* Register a store that binds the instance. */
static int maio_store(struct ring *r, struct io_uring_opaque_config cfg, uint32_t zcrx_id,
		      uint32_t *context)
{
	cfg.flags = IORING_OPAQUE_CFG_MAIO;
	cfg.zcrx_id = zcrx_id;
	return register_store(r, &cfg, context);
}

/* Run a control and return its result. */
static int cmd_res(struct ring *r, uint32_t context, unsigned int op, uint64_t handle,
		   uint64_t offset, unsigned int length, void *ptr)
{
	return command(r, context, op, handle, offset, length, ptr).res;
}

/* FREE an object, or close a stream: the controls that take no buffer. */
static void maio_end(struct ring *r, uint32_t context, unsigned int op, uint64_t id)
{
	require(!cmd_res(r, context, op, id, 0, 0, NULL), "release an object or a stream");
}

static void maio_skip(unsigned int n)
{
	while (n--)
		ksft_test_result_skip("MAIO mode is unavailable\n");
}

static void maio_pattern(unsigned char *p, size_t len, unsigned int seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		p[i] = (unsigned char)(i * 131 + (i >> 8) * 7 + seed);
}

/* Like wait_tag_ms(), but a missing completion is an answer and not a failure. */
static bool maio_poll(struct ring *r, uint64_t tag, unsigned int ms, struct event *event)
{
	struct timespec start, now;
	struct event e;
	unsigned int i;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		struct __kernel_timespec ts = { .tv_nsec = 10000000 };
		struct io_uring_getevents_arg arg = { .ts = (uintptr_t)&ts };
		unsigned int pending = *r->sq_tail - *r->sq_head;
		int ret;

		for (i = 0; i < r->nr_saved; i++) {
			if (r->saved[i].tag == tag) {
				*event = r->saved[i];
				r->saved[i] = r->saved[--r->nr_saved];
				return true;
			}
		}
		ret = syscall(__NR_io_uring_enter, r->fd, pending, 1,
			      IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg, sizeof(arg));
		require(ret >= 0 || errno == ETIME || errno == EINTR, "bounded CQ wait");
		while (peek(r, &e)) {
			if (e.tag == tag) {
				*event = e;
				return true;
			}
			require(r->nr_saved < 64, "bounded wait completion stash");
			r->saved[r->nr_saved++] = e;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		if ((now.tv_sec - start.tv_sec) * 1000 +
		    (now.tv_nsec - start.tv_nsec) / 1000000 >= ms)
			return false;
	}
}

/*
 * Check the extents of an object against the area and gather its bytes from
 * the mapping. The extents cover the object, each one lies inside one page of
 * the area, and area id 0 is the only area. Return the number of extents.
 */
static unsigned int maio_gather(struct ring *r, uint32_t context, struct maio *z,
				uint64_t handle, uint64_t length, unsigned char *out,
				struct io_uring_opaque_extent *vec, unsigned int max)
{
	const uint64_t page = sysconf(_SC_PAGESIZE);
	struct io_uring_opaque_object_stat os;
	struct event event;
	uint64_t done = 0;
	unsigned int i, nr;

	require(!command(r, context, IORING_OPAQUE_OBJECT_STAT, handle, 0,
			 sizeof(os), &os).res, "object stat");
	nr = os.extents;
	require(nr && nr <= max && os.length == length, "object extents");
	event = command(r, context, IORING_OPAQUE_OBJECT_EXTENTS, handle, 0,
			nr * sizeof(*vec), vec);
	require(event.res == (int)(nr * sizeof(*vec)), "extents write one pair per extent");
	for (i = 0; i < nr; i++) {
		require(!(vec[i].area_off >> IORING_ZCRX_AREA_SHIFT), "extent in area 0");
		require(vec[i].len && vec[i].area_off + vec[i].len <= z->area.len,
			"extent inside the area");
		require((vec[i].area_off % page) + vec[i].len <= page, "extent inside a page");
		memcpy(out + done, z->map + vec[i].area_off, vec[i].len);
		done += vec[i].len;
	}
	require(done == length, "extent lengths add up to the object");
	return nr;
}

/* An object of a store that binds an area is visible and writable in place. */
static void test_maio_objects(struct io_uring_opaque_config cfg)
{
	const size_t page = sysconf(_SC_PAGESIZE);
	const size_t length = 3 * page + 123, length2 = 2 * page + 17;
	struct io_uring_opaque_policy policy = {
		.flags = IORING_OPAQUE_POLICY_F_AUTO_COMPACT,
		.bytes_per_second = 64U << 20,
		.burst_bytes = 8U << 20,
		.max_temporary_bytes = 8U << 20,
	};
	const unsigned int compaction_flags = IORING_OPAQUE_OBJECT_F_COMPACTED |
					      IORING_OPAQUE_OBJECT_F_COMPACTING;
	struct io_uring_opaque_extent vec[32], before[32], copy[2];
	struct io_uring_opaque_object_stat os;
	struct io_uring_opaque_stat stat;
	unsigned char *data, *expect, *actual, *got;
	uint64_t stream, plain_stream, handle, handle2, plain_handle, needed;
	uint64_t cursor = 0;
	uint32_t bound, plain;
	unsigned int nr, nr2, i;
	int source[2], source2[2], target[2];
	struct event event;
	struct maio z;
	struct ring r;

	if (!maio_ok) {
		maio_skip(5);
		return;
	}
	data = malloc(4 * length);
	require(data, "MAIO test buffers");
	expect = data + length;
	actual = expect + length;
	got = actual + length;
	maio_pattern(data, length, 1);
	memcpy(expect, data, length);

	require(!ring_init(&r, IORING_SETUP_CQE32), "MAIO ring");
	require(!maio_register(&r, &z, 64), "MAIO instance");
	maio_map(&r, &z);
	require(!maio_store(&r, cfg, z.id, &bound), "store that binds the instance");
	require(!register_store(&r, &cfg, &plain), "store without an instance");
	tcp_pair(source);
	tcp_pair(source2);
	tcp_pair(target);
	stream = attach(&r, bound, source[1], next_tag++);
	plain_stream = attach(&r, plain, source2[1], next_tag++);

	/* 1. The completion and the statistics say MAPPED. The extents cover the object. */
	event = receive_complete(&r, bound, stream, source[0], cursor, data, length);
	cursor += length;
	handle = event.extra[0];
	require(handle && (event.extra[1] & IORING_OPAQUE_OBJECT_F_MAPPED), "RECV_OBJECT: MAPPED");
	require(!(event.extra[1] & IORING_OPAQUE_OBJECT_F_FORWARD_ONLY), "no forwarding-only flag");
	require(!cmd_res(&r, bound, IORING_OPAQUE_OBJECT_STAT, handle, 0, sizeof(os), &os) &&
		(os.flags & IORING_OPAQUE_OBJECT_F_MAPPED) && !(os.flags & compaction_flags),
		"OBJECT_STAT says MAPPED");
	nr = maio_gather(&r, bound, &z, handle, length, actual, vec, ARRAY_SIZE(vec));
	require(nr >= 2, "an object of three pages has several extents");
	require(!memcmp(actual, data, length), "the mapping holds the received bytes");
	require(cmd_res(&r, bound, IORING_OPAQUE_READ_OBJECT, handle, 0, length, got) ==
		(int)length && !memcmp(got, data, length), "READ_OBJECT equals the mapping");
	require(!cmd_res(&r, bound, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat) &&
		stat.copied_bytes >= length, "copied_bytes counts the copies into the area");
	memcpy(before, vec, nr * sizeof(*vec));
	ksft_test_result_pass("bound store: extents, MAPPED, mapping equals READ_OBJECT\n");

	/* 2. A write through the mapping shows in READ_OBJECT and in a send. */
	z.map[vec[0].area_off + 5] ^= 0xff;
	expect[5] ^= 0xff;
	z.map[vec[nr - 1].area_off + vec[nr - 1].len - 1] ^= 0x5a;
	expect[length - 1] ^= 0x5a;
	require(cmd_res(&r, bound, IORING_OPAQUE_READ_OBJECT, handle, 0, length, got) ==
		(int)length && !memcmp(got, expect, length), "READ_OBJECT sees the write");
	require(wait_tag(&r, send_stage(&r, bound, target[0], handle, 0, length)).res ==
		(int)length, "send the object");
	read_all(target[1], got, length);
	require(!memcmp(got, expect, length), "the peer sees the written bytes");
	/* A range of the object, from the middle of a page. */
	require(wait_tag(&r, send_stage(&r, bound, target[0], handle, page - 3, 10)).res == 10,
		"send a range across a page boundary");
	read_all(target[1], got, 10);
	require(!memcmp(got, expect + page - 3, 10), "the peer sees the range");
	ksft_test_result_pass("bound store: a write in the mapping shows in READ_OBJECT, SEND\n");

	/* 3. The size and the errors of EXTENTS. */
	memset(copy, 0xaa, sizeof(copy));
	event = command(&r, bound, IORING_OPAQUE_OBJECT_EXTENTS, handle, 0, sizeof(copy[0]), copy);
	needed = nr * sizeof(vec[0]);
	require(event.res == -EOVERFLOW && event.extra[0] == needed, "small buffer: needed size");
	require(copy[0].area_off == 0xaaaaaaaaaaaaaaaaULL, "a small buffer is not written");
	event = command(&r, bound, IORING_OPAQUE_OBJECT_EXTENTS, handle, 0, 0, NULL);
	require(event.res == -EOVERFLOW && event.extra[0] == needed, "size query");
	event = command(&r, bound, IORING_OPAQUE_OBJECT_EXTENTS, handle, 0, needed - 1, vec);
	require(event.res == -EOVERFLOW && event.extra[0] == needed, "one byte short");
	memset(vec, 0xaa, sizeof(vec));
	event = command(&r, bound, IORING_OPAQUE_OBJECT_EXTENTS, handle, 0, sizeof(vec), vec);
	require(event.res == (int)needed && !memcmp(vec, before, needed), "same vector again");
	require(((unsigned char *)vec)[needed] == 0xaa, "nothing is written past the vector");
	require(cmd_res(&r, bound, IORING_OPAQUE_OBJECT_EXTENTS, handle, 0, 16, NULL) == -EINVAL,
		"a buffer size without a buffer");
	require(cmd_res(&r, bound, IORING_OPAQUE_OBJECT_EXTENTS, handle, 1, needed, vec) == -EINVAL,
		"EXTENTS takes no offset");
	require(cmd_res(&r, bound, IORING_OPAQUE_OBJECT_EXTENTS, handle + (1ULL << 20), 0, needed,
			vec) == -ESTALE, "EXTENTS of a stale handle");
	require(cmd_res(&r, bound, IORING_OPAQUE_OBJECT_EXTENTS, handle, 0, needed,
			(void *)(uintptr_t)1) == -EFAULT, "EXTENTS to a bad address");
	ksft_test_result_pass("bound store: EXTENTS size, vector bounds and bad arguments\n");

	/* 4. A store without an instance keeps the old behaviour. */
	event = receive_complete(&r, plain, plain_stream, source2[0], 0, data, length);
	plain_handle = event.extra[0];
	require(plain_handle && !(event.extra[1] & IORING_OPAQUE_OBJECT_F_MAPPED), "plain object");
	require(!cmd_res(&r, plain, IORING_OPAQUE_OBJECT_STAT, plain_handle, 0, sizeof(os), &os) &&
		!(os.flags & IORING_OPAQUE_OBJECT_F_MAPPED), "plain OBJECT_STAT");
	require(cmd_res(&r, plain, IORING_OPAQUE_OBJECT_EXTENTS, plain_handle, 0, sizeof(vec),
			vec) == -EOPNOTSUPP, "EXTENTS of an object of a store without an instance");
	require(cmd_res(&r, plain, IORING_OPAQUE_OBJECT_EXTENTS, plain_handle, 0, 0, NULL) ==
		-EOPNOTSUPP, "size query of a store without an instance");
	ksft_test_result_pass("store without an instance: no MAPPED, EXTENTS: -EOPNOTSUPP\n");

	/*
	 * 5. Compaction would move the bytes out of the area. A manual COMPACT is
	 * refused. The automatic compaction skips the objects, one that exists
	 * when the policy changes, and one that arrives later. The same policy
	 * compacts the object of the store without an instance, so the dispatcher runs.
	 */
	require(cmd_res(&r, bound, IORING_OPAQUE_COMPACT, handle, 0, 0, NULL) == -EOPNOTSUPP,
		"manual COMPACT of an object in the area");
	require(!cmd_res(&r, bound, IORING_OPAQUE_SET_POLICY, 0, 0, sizeof(policy), &policy),
		"policy of the bound store");
	maio_pattern(data, length2, 9);
	event = receive_complete(&r, bound, stream, source[0], cursor, data, length2);
	cursor += length2;
	handle2 = event.extra[0];
	require(handle2 && (event.extra[1] & IORING_OPAQUE_OBJECT_F_MAPPED), "second MAPPED");
	require(!cmd_res(&r, plain, IORING_OPAQUE_SET_POLICY, 0, 0, sizeof(policy), &policy),
		"policy of the plain store");
	for (i = 0; i < 500; i++) {
		require(!cmd_res(&r, plain, IORING_OPAQUE_OBJECT_STAT, plain_handle, 0,
				 sizeof(os), &os), "control object stat");
		if (os.flags & IORING_OPAQUE_OBJECT_F_COMPACTED)
			break;
		usleep(10000);
	}
	require(i < 500, "the policy compacts the object of the plain store");
	usleep(300000);
	for (i = 0; i < 2; i++) {
		uint64_t h = i ? handle2 : handle;

		require(!cmd_res(&r, bound, IORING_OPAQUE_OBJECT_STAT, h, 0, sizeof(os), &os) &&
			(os.flags & IORING_OPAQUE_OBJECT_F_MAPPED) &&
			!(os.flags & compaction_flags), "objects in the area are not compacted");
	}
	require(cmd_res(&r, bound, IORING_OPAQUE_COMPACT, handle2, 0, 0, NULL) == -EOPNOTSUPP,
		"manual COMPACT with a policy");
	nr2 = maio_gather(&r, bound, &z, handle, length, actual, vec, ARRAY_SIZE(vec));
	require(nr2 == nr && !memcmp(vec, before, nr * sizeof(*vec)) &&
		!memcmp(actual, expect, length), "the extents did not move");
	maio_gather(&r, bound, &z, handle2, length2, actual, vec, ARRAY_SIZE(vec));
	require(!memcmp(actual, data, length2), "the second object in the mapping");

	/* Release everything and check that nothing stays in the stores. */
	maio_end(&r, bound, IORING_OPAQUE_FREE, handle);
	maio_end(&r, bound, IORING_OPAQUE_FREE, handle2);
	maio_end(&r, plain, IORING_OPAQUE_FREE, plain_handle);
	maio_end(&r, bound, IORING_OPAQUE_STREAM_CLOSE, stream);
	maio_end(&r, plain, IORING_OPAQUE_STREAM_CLOSE, plain_stream);
	for (i = 0; i < 2; i++) {
		uint32_t id = i ? plain : bound;

		require(!cmd_res(&r, id, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat) &&
			!stat.backing_bytes && !stat.extents && !stat.objects && !stat.streams,
			"stores drain");
	}
	maio_unmap(&z);
	ring_exit(&r);
	for (i = 0; i < 2; i++) {
		close(source[i]);
		close(source2[i]);
		close(target[i]);
	}
	free(data);
	ksft_test_result_pass("objects in the area: COMPACT is refused, no automatic compaction\n");
}

/* The flag needs an instance in the ring, the id must exist, and the id needs the flag. */
static void test_maio_registration(struct io_uring_opaque_config cfg)
{
	struct io_uring_opaque_register reg;
	struct io_uring_opaque_stat stat;
	struct maio z, second;
	struct ring r;
	uint32_t context;

	require(!ring_init(&r, IORING_SETUP_CQE32), "MAIO registration ring");
	cfg.flags = IORING_OPAQUE_CFG_MAIO;
	if (!maio_ok) {
		/*
		 * A kernel without zero-copy receive cannot bind. A task without
		 * CAP_NET_ADMIN has no instance to bind.
		 */
		if (maio_err != -EOPNOTSUPP && maio_err != -EPERM) {
			ring_exit(&r);
			maio_skip(1);
			return;
		}
		require(register_store(&r, &cfg, &context) ==
			(maio_err == -EPERM ? -ENXIO : -EOPNOTSUPP),
			"the flag without an instance");
		ring_exit(&r);
		ksft_test_result_pass("the flag fails without MAIO mode\n");
		return;
	}
	require(register_store(&r, &cfg, &context) == -ENXIO, "the flag, no instance in the ring");
	/* Ids 0 and 1. An id of 0 is the same as no id, so test with id 1. */
	require(!maio_register(&r, &z, 4) && !maio_register(&r, &second, 4), "MAIO instances");
	require(z.id == 0 && second.id == 1, "instance ids");
	cfg.zcrx_id = 99;
	require(register_store(&r, &cfg, &context) == -ENXIO, "an id without an instance");
	cfg.zcrx_id = second.id;
	cfg.flags = 0;
	require(register_store(&r, &cfg, &context) == -EINVAL, "an id without the flag");
	cfg.flags = IORING_OPAQUE_CFG_MAIO | (1U << 1);
	require(register_store(&r, &cfg, &context) == -EINVAL, "an unknown flag");
	cfg.flags = IORING_OPAQUE_CFG_MAIO;
	cfg.__resv1 = 1;
	require(register_store(&r, &cfg, &context) == -EINVAL, "the remaining reserved word");
	cfg.__resv1 = 0;

	/* The shortest configuration that has the flag word binds instance 0. */
	reg = (struct io_uring_opaque_register) {
		.size = sizeof(reg), .fd = -1, .config = (uintptr_t)&cfg,
		.config_size = offsetof(struct io_uring_opaque_config, __resv),
	};
	cfg.zcrx_id = 0;
	require(!register_request(&r, &reg), "the shortest configuration binds id 0");
	context = reg.store_id;
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		stat.hard_limit == cfg.hard_limit, "the bound store works");
	ring_exit(&r);
	ksft_test_result_pass("the flag needs an instance in the ring, an id needs the flag\n");
}

/* Keep a stream of objects of the given size going until the area runs dry. */
static void test_maio_exhaustion(struct io_uring_opaque_config cfg)
{
	const size_t page = sysconf(_SC_PAGESIZE);
	struct io_uring_opaque_stat stat;
	struct io_uring_opaque_extent vec[8];
	uint64_t stream, a, b, c, handle_a, handle_b, handle_c;
	unsigned char *data, *got;
	struct transfer tx;
	uint32_t context;
	struct event event;
	pthread_t producer;
	int pair[2];
	struct maio z;
	struct ring r;
	size_t total;

	if (!maio_ok) {
		maio_skip(1);
		return;
	}
	/* Nine pages. Two objects of four pages leave one page for a third object of two. */
	total = 10 * page;
	data = malloc(2 * total);
	require(data, "exhaustion buffers");
	got = data + total;
	maio_pattern(data, total, 3);
	require(!ring_init(&r, IORING_SETUP_CQE32), "exhaustion ring");
	require(!maio_register(&r, &z, 9), "small instance");
	maio_map(&r, &z);
	require(!maio_store(&r, cfg, z.id, &context), "store of the small instance");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);

	a = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 0, 4 * page, NULL);
	b = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 4 * page, 4 * page, NULL);
	c = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 8 * page, 2 * page, NULL);
	submit_only(&r);
	tx = (struct transfer) { .fd = pair[0], .data = data, .length = total };
	require(!pthread_create(&producer, NULL, writer, &tx), "exhaustion producer");
	event = wait_tag_ms(&r, a, 30000);
	require(event.res == (int)(4 * page) && event.extra[0], "first object");
	handle_a = event.extra[0];
	event = wait_tag_ms(&r, b, 30000);
	require(event.res == (int)(4 * page) && event.extra[0], "second object");
	handle_b = event.extra[0];

	/*
	 * The third object copied one page, and then the area ran dry. The
	 * request waits and retries every 50 ms. It does not fail, and it keeps
	 * the bytes that it has.
	 */
	require(!maio_poll(&r, c, 500, &event), "the third object waits for a free page");
	require(!cmd_res(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat) &&
		stat.copied_bytes >= 9 * page && stat.backing_bytes == 9 * page,
		"the third object holds its first page");

	/* A page comes back, and the request finishes. */
	maio_end(&r, context, IORING_OPAQUE_FREE, handle_a);
	require(maio_poll(&r, c, 10000, &event), "the third object finishes after the release");
	require(event.res == (int)(2 * page) && event.extra[0], "third object");
	handle_c = event.extra[0];
	pthread_join(producer, NULL);
	require(cmd_res(&r, context, IORING_OPAQUE_READ_OBJECT, handle_c, 0, 2 * page, got) ==
		(int)(2 * page) && !memcmp(got, data + 8 * page, 2 * page),
		"the third object is intact");
	maio_gather(&r, context, &z, handle_c, 2 * page, got, vec, ARRAY_SIZE(vec));
	require(!memcmp(got, data + 8 * page, 2 * page), "the third object in the mapping");
	require(cmd_res(&r, context, IORING_OPAQUE_READ_OBJECT, handle_b, 0, 4 * page, got) ==
		(int)(4 * page) && !memcmp(got, data + 4 * page, 4 * page),
		"the second object is intact");

	maio_end(&r, context, IORING_OPAQUE_FREE, handle_b);
	maio_end(&r, context, IORING_OPAQUE_FREE, handle_c);
	maio_end(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream);
	require(!cmd_res(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat) &&
		!stat.backing_bytes && !stat.extents && !stat.objects, "store drains");
	maio_unmap(&z);
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	free(data);
	ksft_test_result_pass("an empty area: capture waits, a release lets it finish\n");
}

/*
 * Cycle objects through a small area. A page that a release does not return,
 * and a page that stays in limbo after a send, would end the loop with a wait
 * that never finishes. A last object takes the whole area.
 */
static void test_maio_recycle(struct io_uring_opaque_config cfg)
{
	const size_t page = sysconf(_SC_PAGESIZE);
	const size_t length = 2 * page + 904, area_pages = 16;
	struct io_uring_opaque_stat stat;
	unsigned char *data, *got, *whole;
	uint64_t stream, cursor = 0;
	uint32_t context;
	struct event event;
	int source[2], target[2];
	struct maio z;
	struct ring r;
	unsigned int i;

	if (!maio_ok) {
		maio_skip(1);
		return;
	}
	data = malloc(2 * length + area_pages * page);
	require(data, "recycle buffers");
	got = data + length;
	whole = got + length;
	require(!ring_init(&r, IORING_SETUP_CQE32), "recycle ring");
	require(!maio_register(&r, &z, area_pages), "recycle instance");
	maio_map(&r, &z);
	require(!maio_store(&r, cfg, z.id, &context), "recycle store");
	tcp_pair(source);
	tcp_pair(target);
	stream = attach(&r, context, source[1], next_tag++);

	for (i = 0; i < 300; i++) {
		uint64_t handle, tag;

		maio_pattern(data, length, i);
		tag = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, cursor,
				length, NULL);
		submit_only(&r);
		write_all(source[0], data, length);
		require(maio_poll(&r, tag, 20000, &event) && event.res == (int)length,
			"an object of the cycle");
		cursor += length;
		handle = event.extra[0];
		require(cmd_res(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0, length, got) ==
			(int)length && !memcmp(got, data, length), "object bytes in the cycle");
		if (i & 1) {
			/* The send holds the pages in TCP after the object is gone. */
			tag = send_stage_flags(&r, context, target[0], handle, 0, length,
					       IORING_OPAQUE_SEND_LAST, 0, 0);
			event = wait_tag(&r, tag);
			require(event.res == (int)length &&
				(event.flags & IORING_CQE_F_OPAQUE_CONSUMED), "consuming send");
			read_all(target[1], got, length);
			require(!memcmp(got, data, length), "peer bytes in the cycle");
		} else {
			maio_end(&r, context, IORING_OPAQUE_FREE, handle);
		}
	}

	/* The whole area fits in one object, so no page was lost. */
	maio_pattern(whole, area_pages * page, 77);
	event = receive_complete(&r, context, stream, source[0], cursor, whole, area_pages * page);
	require(event.extra[0], "object of the whole area");
	maio_end(&r, context, IORING_OPAQUE_FREE, event.extra[0]);
	maio_end(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream);
	require(!cmd_res(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat) &&
		!stat.backing_bytes && !stat.extents && !stat.objects, "store drains");
	maio_unmap(&z);
	ring_exit(&r);
	for (i = 0; i < 2; i++) {
		close(source[i]);
		close(target[i]);
	}
	free(data);
	ksft_test_result_pass("300 objects cycle through a 16 page area, FREE and SEND_LAST\n");
}

/*
 * The instance goes first. The ring that registered it exits while another ring
 * holds the store through an import. The objects keep their pages. The store
 * goes on taking pages of the area and releases them. The ring that holds the
 * store exits last.
 */
static void test_maio_instance_first(struct io_uring_opaque_config cfg)
{
	const size_t page = sysconf(_SC_PAGESIZE);
	const size_t length = 3 * page + 11;
	struct io_uring_opaque_extent vec[16];
	struct io_uring_opaque_stat stat;
	uint32_t context, imported;
	uint64_t stream, handle, handle2, stream2;
	unsigned char *data, *got;
	struct ring r, other;
	struct event event;
	int pair[2], pair2[2], fd;
	unsigned int i;
	struct maio z;

	if (!maio_ok) {
		maio_skip(1);
		return;
	}
	data = malloc(2 * length);
	require(data, "lifetime buffers");
	got = data + length;
	maio_pattern(data, length, 21);
	require(!ring_init(&r, IORING_SETUP_CQE32), "first ring");
	require(!maio_register(&r, &z, 32), "first instance");
	maio_map(&r, &z);
	require(!maio_store(&r, cfg, z.id, &context), "first store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	event = receive_complete(&r, context, stream, pair[0], 0, data, length);
	handle = event.extra[0];
	require(handle, "object before the exit");
	maio_end(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream);

	/* The second ring imports the store, and the first ring goes away. */
	fd = export_store(&r, context);
	require(fd >= 0, "export the store");
	require(!ring_init(&other, IORING_SETUP_CQE32), "second ring");
	require(!import_store(&other, fd, &imported), "import the store");
	close(fd);
	maio_unmap(&z);
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	/* Give the exit of the first ring time to unregister the instance. */
	usleep(500000);

	require(cmd_res(&other, imported, IORING_OPAQUE_READ_OBJECT, handle, 0, length, got) ==
		(int)length && !memcmp(got, data, length), "the object survives the instance");
	event = command(&other, imported, IORING_OPAQUE_OBJECT_EXTENTS, handle, 0, sizeof(vec),
			vec);
	require(event.res > 0 && !(event.res % sizeof(vec[0])), "extents after the exit");
	for (i = 0; i < event.res / sizeof(vec[0]); i++)
		require(vec[i].area_off != IORING_OPAQUE_EXTENT_UNMAPPED, "extent in the area");
	/* The store still takes pages of the area. */
	tcp_pair(pair2);
	stream2 = attach(&other, imported, pair2[1], next_tag++);
	maio_pattern(data, length, 22);
	event = receive_complete(&other, imported, stream2, pair2[0], 0, data, length);
	handle2 = event.extra[0];
	require(handle2 && (event.extra[1] & IORING_OPAQUE_OBJECT_F_MAPPED), "object after exit");
	require(cmd_res(&other, imported, IORING_OPAQUE_READ_OBJECT, handle2, 0, length, got) ==
		(int)length && !memcmp(got, data, length), "the new object in the area");
	maio_end(&other, imported, IORING_OPAQUE_FREE, handle);
	maio_end(&other, imported, IORING_OPAQUE_FREE, handle2);
	maio_end(&other, imported, IORING_OPAQUE_STREAM_CLOSE, stream2);
	require(!cmd_res(&other, imported, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat) &&
		!stat.backing_bytes && !stat.extents && !stat.objects, "store drains");
	ring_exit(&other);
	close(pair2[0]);
	close(pair2[1]);
	free(data);
	ksft_test_result_pass("the instance exits first: READ_OBJECT, new objects and FREE work\n");
}

/*
 * The store goes first. A ring imports the instance and binds a store to it,
 * keeps an object, and exits. The instance stays in the first ring, and the
 * area has all its pages back: a new store takes the whole area.
 */
static void test_maio_store_first(struct io_uring_opaque_config cfg)
{
	const size_t page = sysconf(_SC_PAGESIZE);
	const size_t pages = 16, length = 3 * page + 11;
	struct io_uring_opaque_extent vec[32];
	struct io_uring_opaque_stat stat;
	uint32_t context, other_context, id;
	uint64_t stream, handle;
	unsigned char *data, *got;
	struct ring r, other;
	struct event event;
	int pair[2], fd;
	struct maio z;

	if (!maio_ok) {
		maio_skip(1);
		return;
	}
	data = malloc(2 * pages * page);
	require(data, "lifetime buffers");
	got = data + pages * page;
	maio_pattern(data, length, 31);
	require(!ring_init(&r, IORING_SETUP_CQE32), "owner ring");
	require(!maio_register(&r, &z, pages), "owner instance");
	maio_map(&r, &z);
	fd = maio_export(&r, &z);
	require(fd >= 0, "export the instance");
	require(!ring_init(&other, IORING_SETUP_CQE32), "importing ring");
	require(!maio_import(&other, fd, &id), "import the instance");
	close(fd);
	require(!maio_store(&other, cfg, id, &other_context), "store of the importing ring");
	tcp_pair(pair);
	stream = attach(&other, other_context, pair[1], next_tag++);
	event = receive_complete(&other, other_context, stream, pair[0], 0, data, length);
	handle = event.extra[0];
	require(handle, "object of the importing ring");
	/* The owner sees the bytes in its mapping. */
	maio_gather(&other, other_context, &z, handle, length, got, vec, ARRAY_SIZE(vec));
	require(!memcmp(got, data, length), "the owner sees the bytes of the other store");
	maio_end(&other, other_context, IORING_OPAQUE_STREAM_CLOSE, stream);
	/* The importing ring exits with the object still in the store. */
	ring_exit(&other);
	close(pair[0]);
	close(pair[1]);
	usleep(500000);

	/* The instance is intact, and every page is free again. */
	require(!maio_store(&r, cfg, z.id, &context), "store of the owner");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	maio_pattern(data, pages * page, 32);
	event = receive_complete(&r, context, stream, pair[0], 0, data, pages * page);
	require(event.extra[0], "object of the whole area");
	maio_gather(&r, context, &z, event.extra[0], pages * page, got, vec, ARRAY_SIZE(vec));
	require(!memcmp(got, data, pages * page), "the whole area in the mapping");
	maio_end(&r, context, IORING_OPAQUE_FREE, event.extra[0]);
	maio_end(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream);
	require(!cmd_res(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat) &&
		!stat.backing_bytes && !stat.extents && !stat.objects, "store drains");
	maio_unmap(&z);
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	free(data);
	ksft_test_result_pass("the store exits first: the instance stays, every page is free\n");
}

/*
 * CREATE in a store that binds an instance copies into pages outside the area.
 * The process has the bytes already, so nothing needs to be made visible: the
 * completion and OBJECT_STAT do not say MAPPED, every extent is outside the
 * area, and the area stays whole. An object of every page of the area still
 * arrives while the created one lives.
 */
static void test_maio_create(struct io_uring_opaque_config cfg)
{
	const size_t page = sysconf(_SC_PAGESIZE);
	const size_t length = 3 * page + 123, pages = 8;
	struct io_uring_opaque_extent vec[32];
	struct io_uring_opaque_object_stat os;
	struct io_uring_opaque_stat stat;
	uint64_t stream, created, whole, total = 0;
	unsigned char *data, *got;
	unsigned int i, nr;
	struct event event;
	uint32_t bound;
	int source[2];
	struct maio z;
	struct ring r;

	if (!maio_ok) {
		maio_skip(1);
		return;
	}
	data = malloc(2 * pages * page);
	require(data, "MAIO test buffers");
	got = data + pages * page;
	maio_pattern(data, length, 3);
	require(!ring_init(&r, IORING_SETUP_CQE32), "MAIO ring");
	require(!maio_register(&r, &z, pages), "MAIO instance");
	maio_map(&r, &z);
	require(!maio_store(&r, cfg, z.id, &bound), "store that binds the instance");

	event = command(&r, bound, IORING_OPAQUE_CREATE, 0, 0, length, data);
	created = event.extra[0];
	require(event.res == (int)length && created && !event.flags && !event.extra[1],
		"CREATE in a bound store: complete, without MAPPED");
	require(!cmd_res(&r, bound, IORING_OPAQUE_OBJECT_STAT, created, 0, sizeof(os), &os) &&
		!(os.flags & IORING_OPAQUE_OBJECT_F_MAPPED) && os.length == length,
		"OBJECT_STAT of a created object: not MAPPED");
	nr = os.extents;
	require(nr && nr <= ARRAY_SIZE(vec), "extents of the created object");
	event = command(&r, bound, IORING_OPAQUE_OBJECT_EXTENTS, created, 0, sizeof(vec), vec);
	require(event.res == (int)(nr * sizeof(vec[0])), "EXTENTS writes one pair per extent");
	for (i = 0; i < nr; i++) {
		require(vec[i].area_off == IORING_OPAQUE_EXTENT_UNMAPPED && vec[i].len,
			"every extent of a created object is outside the area");
		total += vec[i].len;
	}
	require(total == length, "extent lengths add up to the object");
	require(cmd_res(&r, bound, IORING_OPAQUE_READ_OBJECT, created, 0, length, got) ==
		(int)length && !memcmp(got, data, length), "READ_OBJECT returns the created bytes");

	/* The area is whole: an object of every page of it arrives, and is in the area. */
	tcp_pair(source);
	stream = attach(&r, bound, source[1], next_tag++);
	maio_pattern(data, pages * page, 6);
	event = receive_complete(&r, bound, stream, source[0], 0, data, pages * page);
	whole = event.extra[0];
	require(whole && (event.extra[1] & IORING_OPAQUE_OBJECT_F_MAPPED),
		"an object of every page of the area, MAPPED");
	maio_gather(&r, bound, &z, whole, pages * page, got, vec, ARRAY_SIZE(vec));
	require(!memcmp(got, data, pages * page), "the whole area in the mapping");

	maio_end(&r, bound, IORING_OPAQUE_FREE, whole);
	maio_end(&r, bound, IORING_OPAQUE_FREE, created);
	maio_end(&r, bound, IORING_OPAQUE_STREAM_CLOSE, stream);
	require(!cmd_res(&r, bound, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat) &&
		!stat.backing_bytes && !stat.extents && !stat.objects, "store drains");
	maio_unmap(&z);
	ring_exit(&r);
	close(source[0]);
	close(source[1]);
	free(data);
	ksft_test_result_pass("bound store: CREATE stays outside the area, UNMAPPED, area whole\n");
}

static void test_timeout(int signal_number)
{
	static const char message[] = "OPAQUE timeout; adjust OPAQUE_TEST_TIMEOUT\n";
	ssize_t written;

	(void)signal_number;
	written = write(STDERR_FILENO, message, sizeof(message) - 1);
	(void)written;
	_exit(KSFT_FAIL);
}

static void set_test_timeout(void)
{
	const char *value = getenv("OPAQUE_TEST_TIMEOUT");
	unsigned long seconds = 600;
	char *end;

	if (value) {
		errno = 0;
		seconds = strtoul(value, &end, 10);
		require(!errno && *value >= '0' && *value <= '9' && !*end &&
			seconds && seconds <= UINT_MAX,
			"OPAQUE_TEST_TIMEOUT must be a positive number of seconds");
	}
	signal(SIGALRM, test_timeout);
	alarm(seconds);
}

/*
 * Stores that bind an instance on a netdevsim port. Port A has the instance.
 * Its peer B, and the TCP sockets that send through B, are in a namespace of
 * their own, so that the frames cross the devices. The pool of port A puts the
 * payload of each frame into a page of the area, as a driver with private
 * headers does. The fixture follows zcrx_maio.c. Without netdevsim, network
 * namespaces or root, the tests skip.
 */
#define LOCAL_ADDR		"10.77.0.1"
#define PEER_ADDR		"10.77.0.2"
#define PEER_SOCKETS		2

struct peer {
	unsigned int id;		/* netdevsim device id of the peer */
	unsigned int ifindex;		/* in the namespace of the peer */
	int netns;			/* a file descriptor of that namespace */
	int socks[PEER_SOCKETS];	/* TCP sockets in that namespace */
	int err;
};

struct pair {
	unsigned int id;		/* netdevsim device id of port A */
	char name[IFNAMSIZ];
	unsigned int ifindex;
	struct peer peer;
};

static int sysfs_write(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY), ret;

	if (fd < 0)
		return -errno;
	ret = write(fd, text, strlen(text));
	ret = ret < 0 ? -errno : 0;
	close(fd);
	return ret;
}

static bool nsim_create(unsigned int id, unsigned int queues, char name[IFNAMSIZ])
{
	char path[128], text[64];
	struct dirent *de;
	DIR *dir;
	int ret;

	snprintf(text, sizeof(text), "%u 1 %u", id, queues);
	ret = sysfs_write("/sys/bus/netdevsim/new_device", text);
	if (ret)
		return false;
	snprintf(path, sizeof(path), "/sys/bus/netdevsim/devices/netdevsim%u/net", id);
	dir = opendir(path);
	if (!dir)
		return false;
	name[0] = 0;
	while ((de = readdir(dir)))
		if (de->d_name[0] != '.')
			snprintf(name, IFNAMSIZ, "%.*s", IFNAMSIZ - 1, de->d_name);
	closedir(dir);
	return name[0];
}

static void nsim_destroy(unsigned int id)
{
	char text[32];

	snprintf(text, sizeof(text), "%u", id);
	sysfs_write("/sys/bus/netdevsim/del_device", text);
}

static int link_up(const char *name)
{
	struct ifreq ifr = {};
	int s = socket(AF_INET, SOCK_DGRAM, 0), ret;

	require(s >= 0, "control socket");
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
	ret = ioctl(s, SIOCGIFFLAGS, &ifr);
	if (!ret) {
		ifr.ifr_flags |= IFF_UP;
		ret = ioctl(s, SIOCSIFFLAGS, &ifr);
	}
	close(s);
	return ret ? -errno : 0;
}

static int set_addr(const char *name, const char *addr)
{
	struct sockaddr_in *sin;
	struct ifreq ifr = {};
	int s = socket(AF_INET, SOCK_DGRAM, 0), ret;

	require(s >= 0, "control socket");
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	sin->sin_family = AF_INET;
	inet_pton(AF_INET, addr, &sin->sin_addr);
	ret = ioctl(s, SIOCSIFADDR, &ifr);
	if (!ret) {
		inet_pton(AF_INET, "255.255.255.0", &sin->sin_addr);
		ret = ioctl(s, SIOCSIFNETMASK, &ifr);
	}
	close(s);
	return ret ? -errno : 0;
}

/* Without TSO the stack sends one frame per segment. */
static int tso_set(const char *name, bool on)
{
	struct ethtool_value eval = { .cmd = ETHTOOL_STSO, .data = on };
	struct ifreq ifr = {};
	int s = socket(AF_INET, SOCK_DGRAM, 0), ret;

	require(s >= 0, "control socket");
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
	ifr.ifr_data = (void *)&eval;
	ret = ioctl(s, SIOCETHTOOL, &ifr);
	close(s);
	return ret ? -errno : 0;
}

/* The name of the last device in this namespace that is not the loopback device. */
static bool first_nonlo(char name[IFNAMSIZ])
{
	struct if_nameindex *list, *e;

	name[0] = 0;
	list = if_nameindex();
	for (e = list; e && e->if_index; e++)
		if (strcmp(e->if_name, "lo"))
			snprintf(name, IFNAMSIZ, "%s", e->if_name);
	if_freenameindex(list);
	return name[0];
}

/*
 * The network namespace belongs to a thread. unshare() gives this thread a
 * namespace of its own, and netdevsim creates a device in the namespace of the
 * task that asks for it. The sockets keep the namespace after the thread ends.
 */
static void *peer_thread(void *arg)
{
	struct peer *p = arg;
	char name[IFNAMSIZ], text[32];
	int i;

	p->err = -1;
	if (unshare(CLONE_NEWNET))
		return NULL;
	snprintf(text, sizeof(text), "%u 1 1", p->id);
	if (sysfs_write("/sys/bus/netdevsim/new_device", text))
		return NULL;
	if (!first_nonlo(name) || set_addr(name, PEER_ADDR) || tso_set(name, false) ||
	    link_up(name))
		return NULL;
	p->ifindex = if_nametoindex(name);
	p->netns = open("/proc/thread-self/ns/net", O_RDONLY | O_CLOEXEC);
	for (i = 0; i < PEER_SOCKETS; i++)
		p->socks[i] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (!p->ifindex || p->netns < 0)
		return NULL;
	p->err = 0;
	return NULL;
}

/* Return false if the fixture is not available. */
static bool pair_create(struct pair *pr, unsigned int id_a, unsigned int id_b)
{
	char text[64];
	pthread_t th;
	int ns_a, ret;

	memset(pr, 0, sizeof(*pr));
	pr->id = id_a;
	pr->peer.id = id_b;
	pr->peer.netns = -1;
	if (!nsim_create(id_a, 1, pr->name))
		return false;
	pr->ifindex = if_nametoindex(pr->name);
	if (!pr->ifindex || set_addr(pr->name, LOCAL_ADDR) || link_up(pr->name))
		goto err_a;
	if (pthread_create(&th, NULL, peer_thread, &pr->peer))
		goto err_a;
	pthread_join(th, NULL);
	if (pr->peer.err)
		goto err_b;

	ns_a = open("/proc/thread-self/ns/net", O_RDONLY | O_CLOEXEC);
	snprintf(text, sizeof(text), "%d:%u %d:%u", ns_a, pr->ifindex,
		 pr->peer.netns, pr->peer.ifindex);
	ret = sysfs_write("/sys/bus/netdevsim/link_device", text);
	close(ns_a);
	if (!ret)
		return true;
err_b:
	nsim_destroy(id_b);
err_a:
	nsim_destroy(id_a);
	return false;
}

static void pair_destroy(struct pair *pr)
{
	int i;

	for (i = 0; i < PEER_SOCKETS; i++)
		close(pr->peer.socks[i]);
	close(pr->peer.netns);
	nsim_destroy(pr->id);
	nsim_destroy(pr->peer.id);
}

static int listen_local(uint16_t *port)
{
	struct sockaddr_in addr = { .sin_family = AF_INET };
	socklen_t len = sizeof(addr);
	int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

	require(s >= 0, "listen socket");
	inet_pton(AF_INET, LOCAL_ADDR, &addr.sin_addr);
	require(!bind(s, (void *)&addr, len), "bind the listen socket to port A");
	require(!getsockname(s, (void *)&addr, &len), "getsockname");
	require(!listen(s, 4), "listen");
	*port = ntohs(addr.sin_port);
	return s;
}

/* Connect a socket of the peer to port A and return the accepted socket. */
static int connect_peer(struct pair *pr, int idx, int listener, uint16_t port)
{
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port) };
	int fd;

	inet_pton(AF_INET, LOCAL_ADDR, &addr.sin_addr);
	alarm(120);
	require(!connect(pr->peer.socks[idx], (void *)&addr, sizeof(addr)),
		"connect the peer to port A");
	fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
	require(fd >= 0, "accept the connection of the peer");
	alarm(0);
	return fd;
}

/*
 * Register an instance on port A and a store that binds it. Return false, with
 * the fixture gone, if the port cannot take an instance.
 */
static bool maio_port(struct pair *pr, struct ring *r, struct maio *z, unsigned int pages,
		      struct io_uring_opaque_config cfg, uint32_t *bound)
{
	int ret;

	if (!maio_ok) {
		ksft_test_result_skip("MAIO mode is unavailable\n");
		return false;
	}
	if (!pair_create(pr, 300, 301)) {
		ksft_test_result_skip("netdevsim ports in two namespaces are unavailable\n");
		return false;
	}
	require(!ring_init(r, IORING_SETUP_CQE32), "ring for the port");
	ret = maio_register_on(r, z, pr->ifindex, pages);
	if (ret == -EOPNOTSUPP || ret == -ENODEV) {
		ksft_test_result_skip("the port takes no MAIO instance: %d\n", ret);
		ring_exit(r);
		pair_destroy(pr);
		return false;
	}
	require(!ret, "MAIO instance on the port");
	maio_map(r, z);
	require(!maio_store(r, cfg, z->id, bound), "store bound to the instance of the port");
	return true;
}

/* Wait until the store holds no payload. A consuming send frees its backing a little late. */
static void maio_drained(struct ring *r, uint32_t context)
{
	struct io_uring_opaque_stat stat;
	unsigned int i;

	for (i = 0; i < 500; i++) {
		require(!cmd_res(r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat),
			"store stat");
		if (!stat.backing_bytes && !stat.extents && !stat.objects)
			return;
		usleep(10000);
	}
	require(false, "the store drains");
}

/*
 * The pool pages of the port carry the payload, so the store holds them by
 * pool reference and copies no byte. Each segment of the stream is one extent.
 * 300 objects of 16 KiB cycle through an area of 128 pages. The objects with an
 * even number are freed, and the others go back to the peer with a consuming
 * send. The peer reads the bytes that it wrote.
 */
static void test_maio_device(struct io_uring_opaque_config cfg)
{
	const size_t length = 16384;
	struct io_uring_opaque_extent vec[64];
	struct io_uring_opaque_stat stat;
	unsigned char *data, *got;
	uint64_t stream, handle, cursor = 0;
	socklen_t optlen = sizeof(int);
	unsigned int nr, round;
	struct event event;
	uint16_t port;
	uint32_t bound;
	struct maio z;
	struct ring r;
	struct pair pr;
	int sock, listener, mss;

	if (!maio_port(&pr, &r, &z, 128, cfg, &bound))
		return;
	data = malloc(2 * length);
	require(data, "buffers");
	got = data + length;
	listener = listen_local(&port);
	sock = connect_peer(&pr, 0, listener, port);
	require(!getsockopt(pr.peer.socks[0], IPPROTO_TCP, TCP_MAXSEG, &mss, &optlen) && mss > 0,
		"segment size of the peer");
	stream = attach(&r, bound, sock, next_tag++);

	for (round = 0; round < 300; round++) {
		uint64_t tag;

		maio_pattern(data, length, round);
		tag = cmd_stage(&r, bound, IORING_OPAQUE_RECV_OBJECT, stream, cursor, length, NULL);
		submit_only(&r);
		write_all(pr.peer.socks[0], data, length);
		require(maio_poll(&r, tag, 20000, &event) && event.res == (int)length,
			"object from the port");
		cursor += length;
		handle = event.extra[0];
		require(event.extra[1] & IORING_OPAQUE_OBJECT_F_MAPPED, "the object is mapped");
		nr = maio_gather(&r, bound, &z, handle, length, got, vec, ARRAY_SIZE(vec));
		require(nr == (length + mss - 1) / mss, "one extent for each segment");
		require(!memcmp(got, data, length), "bytes through the mapping");
		if (round & 1) {
			tag = send_stage_flags(&r, bound, sock, handle, 0, length,
					       IORING_OPAQUE_SEND_LAST, 0, 0);
			require(wait_tag(&r, tag).res == (int)length, "consuming send to the peer");
			read_all(pr.peer.socks[0], got, length);
			require(!memcmp(got, data, length), "the peer reads the bytes back");
		} else {
			maio_end(&r, bound, IORING_OPAQUE_FREE, handle);
		}
	}
	require(!cmd_res(&r, bound, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat), "store stat");
	require(!stat.copied_bytes, "no byte is copied");
	maio_end(&r, bound, IORING_OPAQUE_STREAM_CLOSE, stream);
	maio_drained(&r, bound);
	maio_unmap(&z);
	ring_exit(&r);
	close(sock);
	close(listener);
	pair_destroy(&pr);
	free(data);
	ksft_test_result_pass("store on a port: 300 objects, pool references, no copy\n");
}

/*
 * The area of 64 pages is too small for the stream. A segment takes a whole
 * page of the pool, so the objects use up the area. An object that needs
 * another page waits and keeps what it has. It finishes after the process
 * frees an object.
 */
static void test_maio_device_small(struct io_uring_opaque_config cfg)
{
	const size_t length = 32768;
	const unsigned int objects = 4;
	struct io_uring_opaque_extent vec[64];
	unsigned char *data, *got;
	uint64_t stream, handle[4], tag[4], cursor = 0;
	unsigned int i, j, waiting = 0;
	bool done[4] = {};
	struct event event;
	uint16_t port;
	uint32_t bound;
	struct maio z;
	struct ring r;
	struct pair pr;
	int sock, listener;

	if (!maio_port(&pr, &r, &z, 64, cfg, &bound))
		return;
	data = malloc((objects + 1) * length);
	require(data, "buffers");
	got = data + objects * length;
	listener = listen_local(&port);
	sock = connect_peer(&pr, 0, listener, port);
	stream = attach(&r, bound, sock, next_tag++);

	for (i = 0; i < objects; i++) {
		maio_pattern(data + i * length, length, 5 + i);
		tag[i] = cmd_stage(&r, bound, IORING_OPAQUE_RECV_OBJECT, stream, cursor,
				   length, NULL);
		cursor += length;
	}
	submit_only(&r);
	write_all(pr.peer.socks[0], data, objects * length);

	/* The first objects find pages. Later ones may wait for the process. */
	for (i = 0; i < objects; i++) {
		done[i] = maio_poll(&r, tag[i], i < 2 ? 20000 : 1500, &event);
		if (done[i]) {
			require(event.res == (int)length, "object result");
			handle[i] = event.extra[0];
		} else {
			waiting++;
		}
	}
	require(waiting, "an object waits for a page of the area");

	/* Free the finished objects in turn. The waiting ones finish. */
	for (i = 0; i < objects; i++) {
		if (!done[i]) {
			require(maio_poll(&r, tag[i], 30000, &event), "a waiting object finishes");
			require(event.res == (int)length, "late object result");
			handle[i] = event.extra[0];
			done[i] = true;
		}
		maio_gather(&r, bound, &z, handle[i], length, got, vec, ARRAY_SIZE(vec));
		require(!memcmp(got, data + i * length, length), "object bytes");
		maio_end(&r, bound, IORING_OPAQUE_FREE, handle[i]);
		for (j = i + 1; j < objects; j++) {
			if (done[j] || !maio_poll(&r, tag[j], 1000, &event))
				continue;
			require(event.res == (int)length, "next object result");
			handle[j] = event.extra[0];
			done[j] = true;
		}
	}
	maio_end(&r, bound, IORING_OPAQUE_STREAM_CLOSE, stream);
	maio_drained(&r, bound);
	maio_unmap(&z);
	ring_exit(&r);
	close(sock);
	close(listener);
	pair_destroy(&pr);
	free(data);
	ksft_test_result_pass("store on a port: a full area makes an object wait for a free\n");
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

	set_test_timeout();
	ksft_print_header();
	ret = ring_init(&r, IORING_SETUP_CQE32);
	if (ret)
		ksft_exit_skip("io_uring unavailable: %s\n", strerror(-ret));
	ret = register_store(&r, &cfg, &context);
	if (ret == -EINVAL || ret == -EOPNOTSUPP)
		ksft_exit_skip("OPAQUE_OBJ unavailable\n");
	require(!ret, "opaque registration");
	{
		struct ring probe;
		struct maio z;

		require(!ring_init(&probe, IORING_SETUP_CQE32), "MAIO probe ring");
		maio_err = maio_register(&probe, &z, 1);
		maio_ok = !maio_err;
		ring_exit(&probe);
	}
	ksft_set_plan(121);
	ksft_test_result_pass("register opaque store without RX device or mappings\n");
	test_create(cfg);
	test_create_quota(cfg);
	test_create_cancel(cfg);
	test_create_allocation_faults(cfg);
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
		policy.burst_bytes = sysconf(_SC_PAGESIZE);
		require(!command(&r, context, IORING_OPAQUE_SET_POLICY,
				 0, 0, sizeof(policy), &policy).res, "ineligible auto policy");
		usleep(150000);
		require(!command(&r, context, IORING_OPAQUE_OBJECT_STAT, handle, 0,
				 sizeof(object_stat), &object_stat).res &&
			!(object_stat.flags & IORING_OPAQUE_OBJECT_F_COMPACTED),
			"automatic compaction respects the burst limit");
		policy.burst_bytes = 8U << 20;
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
	ksft_test_result_pass("auto policy re-enrolls objects and preserves bytes/identity\n");
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
	test_watermark_config(cfg);
	test_lowmem(cfg);
	test_compact_watermarks(cfg);
	test_reply_reserve(cfg);
	test_reply_reserve_floor(cfg);
	test_capture_gate(cfg);
	test_split_header(cfg);
	test_greedy_default_config(cfg);
	test_header_larger_than_old_window(cfg);
	test_gap_after_decision(cfg);
	test_reinspect_same_offset(cfg);
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
	test_waiting_follower_cancel(&cfg);
	test_send_cancel_race(&cfg);
	test_link_order(&cfg);
	test_keep_restore(&cfg);
	test_interleaved(cfg);
	test_compact_queue(&cfg, 0);
	test_compact_queue(&cfg, 1);
	test_compact_queue(&cfg, 2);
	test_compact_teardown(&cfg);
	test_budget_error(cfg);
	test_memcg(&cfg);
	test_disconnect(&cfg);
	test_socket_lifetime(&cfg);
	test_poll_first(&cfg);
	test_full_cq(&cfg);
	test_allocation_failure(&cfg);
	test_maio_registration(cfg);
	test_maio_objects(cfg);
	test_maio_create(cfg);
	test_maio_exhaustion(cfg);
	test_maio_recycle(cfg);
	test_maio_instance_first(cfg);
	test_maio_store_first(cfg);
	test_maio_device(cfg);
	test_maio_device_small(cfg);
	ksft_finished();
}
