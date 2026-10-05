// SPDX-License-Identifier: GPL-2.0
/* End-to-end stream/object UAPI tests; run inside the disposable guest. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "../lib/kpass_objects.h"

struct result {
	uint64_t tag;
	uint64_t object;
	int res;
};

static struct io_uring ring;
#define RESULT_SLOTS 4096
static struct result results[RESULT_SLOTS];
static uint64_t sequence = 1ULL << 48;
static int device;

static struct kpass_sqe_cmd *command(uint8_t op, uint16_t sock, uint64_t *tag)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
	struct kpass_sqe_cmd *cmd;

	assert(sqe);
	*tag = ++sequence;
	cmd = kpass_prep_cmd(&ring, sqe, device, op, sock, *tag);
	assert(cmd);
	return cmd;
}

static void save_cqe(struct io_uring_cqe *cqe)
{
	struct result *r = &results[cqe->user_data % RESULT_SLOTS];

	assert(!r->tag);
	r->tag = cqe->user_data;
	r->res = cqe->res;
	r->object = cqe->big_cqe[0];
	io_uring_cqe_seen(&ring, cqe);
}

static struct result wait_result(uint64_t tag)
{
	struct result *r = &results[tag % RESULT_SLOTS];
	struct __kernel_timespec timeout = { .tv_sec = 5 };
	struct io_uring_cqe *cqe;
	struct result out;

	assert(io_uring_submit(&ring) >= 0);
	while (r->tag != tag) {
		int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &timeout);

		if (ret) {
			fprintf(stderr, "wait tag %llu: %s\n",
				(unsigned long long)tag, strerror(-ret));
			abort();
		}
		save_cqe(cqe);
	}
	out = *r;
	r->tag = 0;
	return out;
}

static uint64_t range(uint8_t op, uint16_t sock, uint64_t object,
		       uint64_t off, uint64_t len, void *dest)
{
	uint64_t tag;
	struct kpass_sqe_cmd *cmd = command(op, sock, &tag);

	cmd->stream.object = object;
	cmd->stream.offset = off;
	cmd->stream.length = len;
	cmd->stream.addr = (uintptr_t)dest;
	assert(io_uring_submit(&ring) >= 0);
	return tag;
}

static struct result expect(uint64_t tag, int res)
{
	struct result r = wait_result(tag);

	if (r.res != res) {
		fprintf(stderr, "tag %llu: got %d, expected %d\n",
			(unsigned long long)tag, r.res, res);
		abort();
	}
	if (res < 0)
		assert(!r.object);
	return r;
}

static struct kpass_stream_stat stat_socket(uint16_t sock)
{
	struct kpass_stream_stat stat;

	expect(range(KPASS_OP_STREAM_STAT, sock, 0, 0, 0, &stat), 0);
	return stat;
}

static struct kpass_object_stat stat_object(uint64_t object)
{
	struct kpass_object_stat stat;

	expect(range(KPASS_OP_OBJECT_STAT, 0, object, 0, 0, &stat), 0);
	return stat;
}

static uint64_t compact_object(uint64_t object)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
	uint64_t tag = ++sequence;

	assert(sqe);
	assert(!kpass_prep_compact(&ring, sqe, device, object, tag));
	assert(io_uring_submit(&ring) >= 0);
	return tag;
}

static struct kpass_stream_stat await_received(uint16_t sock, uint64_t end)
{
	struct kpass_stream_stat stat;
	unsigned int tries;

	for (tries = 0; tries < 3000; tries++) {
		stat = stat_socket(sock);
		if (stat.received >= end)
			return stat;
		usleep(1000);
	}
	fprintf(stderr, "received %llu, waiting for %llu (charge %llu)\n",
		(unsigned long long)stat.received, (unsigned long long)end,
		(unsigned long long)stat.backing_bytes);
	abort();
}

static void still_pending(uint64_t tag)
{
	struct io_uring_cqe *cqe;

	while (!io_uring_peek_cqe(&ring, &cqe))
		save_cqe(cqe);
	assert(results[tag % RESULT_SLOTS].tag != tag);
}

static int connect_pair(uint16_t *sock)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct timeval timeout = { .tv_sec = 5 };
	socklen_t len = sizeof(addr);
	uint64_t tag;
	struct kpass_sqe_cmd *cmd;
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	int peer;

	assert(listener >= 0);
	assert(!bind(listener, (struct sockaddr *)&addr, len));
	assert(!getsockname(listener, (struct sockaddr *)&addr, &len));
	assert(!listen(listener, 8));
	command(KPASS_OP_SOCK_CREATE, 0, &tag);
	*sock = expect(tag, 0).res;
	cmd = command(KPASS_OP_SOCK_CONNECT, *sock, &tag);
	cmd->connect.family = AF_INET;
	cmd->connect.port = ntohs(addr.sin_port);
	memcpy(cmd->connect.addr, &addr.sin_addr, 4);
	expect(tag, 0);
	peer = accept(listener, NULL, NULL);
	assert(peer >= 0);
	close(listener);
	assert(!setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
	assert(!setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
	return peer;
}

static void send_bytes(int peer, const void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = send(peer, buf + done, len - done, MSG_NOSIGNAL);

		assert(n > 0);
		done += n;
	}
}

static void read_bytes(int peer, void *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = recv(peer, buf + done, len - done, 0);

		assert(n > 0);
		done += n;
	}
}

static void release_object(uint64_t object)
{
	expect(range(KPASS_OP_OBJECT_FREE, 0, object, 0, 0, NULL), 0);
}

static void close_pair(uint16_t sock, int peer)
{
	uint64_t tag;

	command(KPASS_OP_SOCK_CLOSE, sock, &tag);
	expect(tag, 0);
	close(peer);
}

static void assembly_test(void)
{
	uint16_t sock;
	int peer = connect_pair(&sock);
	struct kpass_stream_stat stat;
	uint64_t keep, next, id, second, a, b;
	char header[4], payload[20], forwarded[14];

	send_bytes(peer, "HEADabcdefgh", 12);
	stat = await_received(sock, 12);
	assert(stat.buffered == 12 && !stat.objects);
	assert(!expect(range(KPASS_OP_READ_STREAM, sock, 0, 0, 4, header), 4).object);
	assert(!memcmp(header, "HEAD", 4));
	keep = range(KPASS_OP_KEEP, sock, 0, 4, 20, NULL);
	next = range(KPASS_OP_READ_STREAM, sock, 0, 24, 4, header);
	stat = stat_socket(sock);
	assert(!stat.objects && stat.buffered == 4);
	still_pending(keep);
	still_pending(next);
	expect(range(KPASS_OP_KEEP, sock, 0, 6, 2, NULL), -EBUSY);
	expect(range(KPASS_OP_DISCARD, sock, 0, 0, 4, NULL), 4);
	send_bytes(peer, "ijklmnopqrstNEXT", 16);
	id = expect(keep, 20).object;
	assert(id);
	assert(!expect(next, 4).object);
	assert(!memcmp(header, "NEXT", 4));
	expect(range(KPASS_OP_OBJECT_READ, 0, id, 0, 20, payload), 20);
	assert(!memcmp(payload, "abcdefghijklmnopqrst", 20));
	second = expect(range(KPASS_OP_KEEP, sock, 0, 24, 4, NULL), 4).object;
	assert(second && second != id);
	expect(range(KPASS_OP_READ_STREAM, sock, 0, 0, 4, header), -ENODATA);
	a = range(KPASS_OP_OBJECT_SEND, sock, id, 3, 7, NULL);
	b = range(KPASS_OP_OBJECT_SEND, sock, id, 3, 7, NULL);
	release_object(id);
	expect(a, 7);
	expect(b, 7);
	read_bytes(peer, forwarded, sizeof(forwarded));
	assert(!memcmp(forwarded, "defghijdefghij", sizeof(forwarded)));
	expect(range(KPASS_OP_OBJECT_READ, 0, id, 0, 4, header), -ESTALE);
	keep = range(KPASS_OP_DISCARD, sock, 0, 28, 5, NULL);
	send_bytes(peer, "trashTAIL", 9);
	expect(keep, 5);
	a = expect(range(KPASS_OP_KEEP, sock, 0, 33, 4, NULL), 4).object;
	assert((uint32_t)a == (uint32_t)id && a != id);
	expect(range(KPASS_OP_OBJECT_FREE, 0, id, 0, 0, NULL), -ESTALE);
	expect(range(KPASS_OP_OBJECT_READ, 0, a, 0, 4, header), 4);
	assert(!memcmp(header, "TAIL", 4));
	release_object(a);
	release_object(second);
	keep = range(KPASS_OP_KEEP, sock, 0, 37, 10, NULL);
	send_bytes(peer, "abc", 3);
	assert(!shutdown(peer, SHUT_WR));
	expect(keep, -ENODATA);
	stat = stat_socket(sock);
	assert(stat.eof && !stat.objects && !stat.backing_bytes);
	close_pair(sock, peer);
	puts("PASS: automatic RX, headers, future KEEP, next header, IDs, reuse, discard, EOF");
}

static void cancel_test(void)
{
	uint16_t sock;
	int peer = connect_pair(&sock);
	uint64_t keep, read, tag;
	char header[4];
	struct io_uring other;
	struct io_uring_sqe *sqe;

	keep = range(KPASS_OP_KEEP, sock, 0, 0, 16, NULL);
	send_bytes(peer, "partial!", 8);
	await_received(sock, 8);
	expect(range(KPASS_OP_CANCEL, 0, keep, 0, 0, NULL), 0);
	expect(keep, -ECANCELED);
	assert(!stat_socket(sock).backing_bytes);
	read = range(KPASS_OP_READ_STREAM, sock, 0, 16, 4, header);
	expect(range(KPASS_OP_CANCEL, 0, read, 0, 0, NULL), 0);
	expect(read, -ECANCELED);
	/* Invalid user memory is reported in the submitter's context. */
	send_bytes(peer, "NEXT", 4);
	await_received(sock, 12);
	expect(range(KPASS_OP_READ_STREAM, sock, 0, 8, 4, (void *)1), -EFAULT);
	expect(range(KPASS_OP_KEEP, sock, 0, UINT64_MAX - 2, 4, NULL), -EINVAL);
	expect(range(KPASS_OP_KEEP, sock, 0, 12, 0, NULL), -EINVAL);
	expect(range(KPASS_OP_KEEP, sock, 0, 12, (uint64_t)INT_MAX + 1, NULL), -EINVAL);
	/* Ring teardown cancels a pending command even with the device still open. */
	assert(!io_uring_queue_init(8, &other, IORING_SETUP_SQE128 | IORING_SETUP_CQE32));
	sqe = io_uring_get_sqe(&other);
	assert(!kpass_prep_range(&other, sqe, device, KPASS_OP_KEEP, sock,
				 0, 100, 4, NULL, 7));
	assert(io_uring_submit(&other) == 1);
	assert(io_uring_submit_and_wait(&other, 0) >= 0);
	expect(range(KPASS_OP_CANCEL, 0, 7, 0, 0, NULL), -ENOENT);
	io_uring_queue_exit(&other);
	read = range(KPASS_OP_READ_STREAM, sock, 0, 16, 4, header);
	command(KPASS_OP_SOCK_CLOSE, sock, &tag);
	expect(tag, 0);
	expect(read, -ECANCELED);
	close(peer);
	puts("PASS: cancellation, faulting read, range validation, ring teardown, socket close");
}

static void budget_test(void)
{
	uint16_t sock;
	int peer = connect_pair(&sock);
	struct kpass_stream_stat stat;
	uint64_t id, discard;
	char data[32];

	memset(data, 'A', sizeof(data));
	send_bytes(peer, data, sizeof(data));
	stat = await_received(sock, sizeof(data));
	assert(stat.backing_bytes);
	expect(range(KPASS_OP_LIMIT, 0, 0, 0, stat.backing_bytes, NULL), 0);
	id = expect(range(KPASS_OP_KEEP, sock, 0, 0, 32, NULL), 32).object;
	send_bytes(peer, data, sizeof(data));
	usleep(10000);
	assert(stat_socket(sock).received == 32);
	discard = range(KPASS_OP_DISCARD, sock, 0, 32, 32, NULL);
	expect(discard, 32);
	send_bytes(peer, "TAIL", 4);
	usleep(10000);
	assert(stat_socket(sock).received == 64);
	release_object(id);
	/* Releasing backing must restart RX without a new packet. */
	await_received(sock, 68);
	id = expect(range(KPASS_OP_KEEP, sock, 0, 64, 4, NULL), 4).object;
	expect(range(KPASS_OP_OBJECT_READ, 0, id, 0, 4, data), 4);
	assert(!memcmp(data, "TAIL", 4));
	release_object(id);
	expect(range(KPASS_OP_LIMIT, 0, 0, 0, KPASS_DEFAULT_LIMIT, NULL), 0);
	close_pair(sock, peer);
	puts("PASS: budget stall, discard while full, release wakes RX");
}

static void large_send_test(void)
{
	const size_t len = 4 * 1024 * 1024;
	uint16_t sock;
	int peer = connect_pair(&sock);
	unsigned char *data = malloc(len), *got = malloc(len);
	uint64_t keep, id, a, b;
	size_t i;

	assert(data && got);
	for (i = 0; i < len; i++)
		data[i] = (i * 17 + i / 4096) & 255;
	expect(range(KPASS_OP_LIMIT, 0, 0, 0, 512ULL * 1024 * 1024, NULL), 0);
	keep = range(KPASS_OP_KEEP, sock, 0, 0, len, NULL);
	send_bytes(peer, data, len);
	id = expect(keep, len).object;
	assert(id);
	a = range(KPASS_OP_OBJECT_SEND, sock, id, 0, len, NULL);
	still_pending(a);
	expect(compact_object(id), 0);
	assert(stat_object(id).flags & KPASS_OBJECT_COMPACTED);
	b = range(KPASS_OP_OBJECT_SEND, sock, id, 0, len, NULL);
	release_object(id);
	still_pending(a);
	still_pending(b);
	read_bytes(peer, got, len);
	assert(!memcmp(got, data, len));
	read_bytes(peer, got, len);
	assert(!memcmp(got, data, len));
	expect(a, len);
	expect(b, len);
	assert(!stat_socket(sock).backing_bytes);
	close_pair(sock, peer);
	free(data);
	free(got);
	puts("PASS: 4 MiB compaction during TX, old/new backing, partial sends, free during TX");
}

static void compact_test(void)
{
	const size_t page = sysconf(_SC_PAGESIZE);
	const size_t len = 2 * 65536 + page + 29;
	const size_t packed = (len + page - 1) / page * page;
	uint16_t sock;
	int peer = connect_pair(&sock);
	unsigned char *data = malloc(len), *got = malloc(len);
	struct kpass_object_stat before, after;
	struct kpass_stream_stat memory;
	uint64_t keep, id, send;
	size_t i;

	assert(data && got);
	for (i = 0; i < len; i++)
		data[i] = (i * 31 + i / 997) & 255;
	keep = range(KPASS_OP_KEEP, sock, 0, 0, len, NULL);
	send_bytes(peer, data, len);
	id = expect(keep, len).object;
	before = stat_object(id);
	assert(before.length == len && !before.flags);
	memory = stat_socket(sock);
	expect(range(KPASS_OP_LIMIT, 0, 0, 0, memory.backing_bytes, NULL), 0);
	expect(compact_object(id), -ENOBUFS);
	after = stat_object(id);
	assert(!after.flags && after.extents == before.extents);
	expect(range(KPASS_OP_OBJECT_READ, 0, id, 0, len, got), len);
	assert(!memcmp(got, data, len));
	expect(range(KPASS_OP_LIMIT, 0, 0, 0, memory.backing_bytes + packed, NULL), 0);
	assert(!expect(compact_object(id), 0).object);
	after = stat_object(id);
	assert(after.length == len && after.flags == KPASS_OBJECT_COMPACTED);
	assert(after.backing_bytes == packed && after.extents < before.extents);
	assert(stat_socket(sock).backing_bytes == packed);
	expect(range(KPASS_OP_OBJECT_READ, 0, id, 0, len, got), len);
	assert(!memcmp(got, data, len));
	expect(range(KPASS_OP_OBJECT_READ, 0, id, page - 5, page + 17, got), page + 17);
	assert(!memcmp(got, data + page - 5, page + 17));
	send = range(KPASS_OP_OBJECT_SEND, sock, id, 65531, 8000, NULL);
	read_bytes(peer, got, 8000);
	expect(send, 8000);
	assert(!memcmp(got, data + 65531, 8000));
	/* Already packed: no new allocation needed, even at the hard limit. */
	expect(range(KPASS_OP_LIMIT, 0, 0, 0, packed, NULL), 0);
	expect(compact_object(id), 0);
	assert(stat_socket(sock).backing_bytes == packed);
	release_object(id);
	expect(compact_object(id), -ESTALE);
	expect(range(KPASS_OP_OBJECT_STAT, 0, id, 0, 0, &after), -ESTALE);
	assert(!stat_socket(sock).backing_bytes);
	expect(range(KPASS_OP_LIMIT, 0, 0, 0, KPASS_DEFAULT_LIMIT, NULL), 0);
	close_pair(sock, peer);
	free(data);
	free(got);
	puts("PASS: compaction packing, unchanged ID/data, budget rollback, no-op, ranged I/O");
}

static void compact_lifetime_test(void)
{
	const size_t len = 4 * 1024 * 1024;
	uint16_t sock;
	int peer = connect_pair(&sock);
	unsigned char *data = malloc(len), *got = malloc(len);
	unsigned int round, tries;
	uint64_t id, keep, compact, cancel;
	struct result r;
	struct kpass_object_stat stat;
	struct io_uring other;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec timeout = { .tv_sec = 5 };

	assert(data && got);
	memset(data, 0x6b, len);
	for (round = 0; round < 2; round++) {
		keep = range(KPASS_OP_KEEP, sock, 0, round * len, len, NULL);
		send_bytes(peer, data, len);
		id = expect(keep, len).object;
		if (!round) {
			command(KPASS_OP_OBJECT_COMPACT, 0, &compact)->stream.object = id;
			command(KPASS_OP_CANCEL, 0, &cancel)->stream.object = compact;
			r = wait_result(cancel);
			/* Both legal outcomes of cancellation racing with completion. */
			assert(!r.res || r.res == -ENOENT);
			expect(compact, r.res ? 0 : -ECANCELED);
			printf("compaction cancellation: %s\n",
			       r.res ? "completed before cancel" : "canceled by worker");
		} else {
			assert(!io_uring_queue_init(8, &other,
				IORING_SETUP_SQE128 | IORING_SETUP_CQE32));
			sqe = io_uring_get_sqe(&other);
			assert(!kpass_prep_compact(&other, sqe, device, id, 1));
			sqe = io_uring_get_sqe(&other);
			assert(!kpass_prep_range(&other, sqe, device, KPASS_OP_OBJECT_STAT,
						 0, id, 0, 0, &stat, 2));
			assert(io_uring_submit(&other) == 2);
			/* The STAT completion proves the worker request was issued. */
			for (;;) {
				uint64_t tag;

				assert(!io_uring_wait_cqe_timeout(&other, &cqe, &timeout));
				tag = cqe->user_data;
				assert(!cqe->res && (tag == 1 || tag == 2));
				io_uring_cqe_seen(&other, cqe);
				if (tag == 2)
					break;
			}
			assert(stat.flags & (KPASS_OBJECT_COMPACTING | KPASS_OBJECT_COMPACTED));
			printf("compaction ring teardown: %s\n",
			       stat.flags & KPASS_OBJECT_COMPACTING ? "in flight" : "completed");
			io_uring_queue_exit(&other);
		}
		/* Teardown can finish asynchronously; all source/copy pins must drain. */
		for (tries = 0; tries < 3000; tries++) {
			stat = stat_object(id);
			if (!(stat.flags & KPASS_OBJECT_COMPACTING) &&
			    stat_socket(sock).pending == 1)
				break;
			usleep(1000);
		}
		assert(tries < 3000);
		assert(stat_socket(sock).backing_bytes == stat.backing_bytes);
		expect(range(KPASS_OP_OBJECT_READ, 0, id, 0, len, got), len);
		assert(!memcmp(got, data, len));
		release_object(id);
		assert(!stat_socket(sock).backing_bytes);
	}
	close_pair(sock, peer);
	free(data);
	free(got);
	puts("PASS: compaction cancel/teardown, preserved bytes, drained references");
}

static void accept_test(void)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
		.sin_port = htons(19876),
	};
	struct kpass_sqe_cmd *cmd;
	uint16_t listener, accepted;
	uint64_t tag, accept_tag, id;
	struct result r;
	char data[4];
	int peer = socket(AF_INET, SOCK_STREAM, 0);

	assert(peer >= 0);
	command(KPASS_OP_SOCK_CREATE, 0, &tag);
	listener = expect(tag, 0).res;
	cmd = command(KPASS_OP_SOCK_LISTEN, listener, &tag);
	cmd->listen.family = AF_INET;
	cmd->listen.port = 19876;
	cmd->listen.backlog = 4;
	expect(tag, 0);
	command(KPASS_OP_SOCK_ACCEPT, listener, &accept_tag);
	assert(io_uring_submit(&ring) >= 0);
	assert(!connect(peer, (struct sockaddr *)&addr, sizeof(addr)));
	send_bytes(peer, "LIVE", 4);
	r = wait_result(accept_tag);
	assert(r.res > 0 && !r.object);
	accepted = r.res;
	await_received(accepted, 4);
	id = expect(range(KPASS_OP_KEEP, accepted, 0, 0, 4, NULL), 4).object;
	close_pair(accepted, peer);
	command(KPASS_OP_SOCK_CLOSE, listener, &tag);
	expect(tag, 0);
	/* Completed objects outlive both the accepted socket and its listener. */
	expect(range(KPASS_OP_OBJECT_READ, 0, id, 0, 4, data), 4);
	assert(!memcmp(data, "LIVE", 4));
	release_object(id);
	puts("PASS: accept, automatic RX before a read, object lifetime after socket close");
}

static void framing_test(void)
{
	uint16_t sock;
	int peer = connect_pair(&sock);
	char header[4], body[3];
	uint64_t read, first, second, a, b, tag;
	struct linger reset = { .l_onoff = 1 };

	read = range(KPASS_OP_READ_STREAM, sock, 0, 0, 4, header);
	send_bytes(peer, "HE", 2);
	await_received(sock, 2);
	still_pending(read);
	send_bytes(peer, "ADonetwo", 8);
	expect(read, 4);
	assert(!memcmp(header, "HEAD", 4));
	first = range(KPASS_OP_KEEP, sock, 0, 4, 3, NULL);
	second = range(KPASS_OP_KEEP, sock, 0, 7, 3, NULL);
	a = expect(first, 3).object;
	b = expect(second, 3).object;
	assert(a && b && a != b);
	expect(range(KPASS_OP_OBJECT_READ, 0, a, 0, 3, body), 3);
	assert(!memcmp(body, "one", 3));
	expect(range(KPASS_OP_OBJECT_READ, 0, b, 0, 3, body), 3);
	assert(!memcmp(body, "two", 3));
	expect(range(KPASS_OP_OBJECT_SEND, sock, a, 2, 2, NULL), -EINVAL);
	release_object(a);
	release_object(b);
	first = range(KPASS_OP_KEEP, sock, 0, 10, 8, NULL);
	send_bytes(peer, "part", 4);
	await_received(sock, 14);
	assert(!setsockopt(peer, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)));
	close(peer);
	expect(first, -ECONNRESET);
	command(KPASS_OP_SOCK_CLOSE, sock, &tag);
	expect(tag, 0);
	puts("PASS: split header, multiple objects in one receive, bounds, reset without a handle");
}

static void pending_limit_test(void)
{
	uint16_t sock;
	int peer = connect_pair(&sock);
	uint64_t tags[KPASS_MAX_PENDING], close_tag;
	char byte;
	size_t i;

	for (i = 0; i < KPASS_MAX_PENDING; i++)
		tags[i] = range(KPASS_OP_READ_STREAM, sock, 0, 0, 1, &byte);
	expect(range(KPASS_OP_READ_STREAM, sock, 0, 0, 1, &byte), -ENOSPC);
	/* Control operations remain usable when the pending-operation cap is full. */
	assert(stat_socket(sock).pending >= KPASS_MAX_PENDING);
	expect(range(KPASS_OP_LIMIT, 0, 0, 0, KPASS_DEFAULT_LIMIT, NULL), 0);
	command(KPASS_OP_SOCK_CLOSE, sock, &close_tag);
	expect(close_tag, 0);
	for (i = 0; i < KPASS_MAX_PENDING; i++)
		expect(tags[i], -ECANCELED);
	close(peer);
	puts("PASS: pending cap, control progress while full, 1024 terminal CQEs with CQ overflow");
}

static void legacy_test(void)
{
	struct io_uring legacy;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct kpass_sqe_cmd *cmd;
	int fd = open(CEPH_KPASS_DEVICE, O_RDWR | O_CLOEXEC);
	const uint8_t ops[] = { KPASS_OP_INIT, KPASS_OP_BUF_ALLOC, KPASS_OP_BUF_FREE };
	size_t i;

	assert(fd >= 0);
	assert(!io_uring_queue_init(8, &legacy, IORING_SETUP_SQE128));
	for (i = 0; i < sizeof(ops); i++) {
		sqe = io_uring_get_sqe(&legacy);
		assert(sqe);
		io_uring_prep_uring_cmd(sqe, 0, fd);
		cmd = (struct kpass_sqe_cmd *)sqe->cmd;
		memset(cmd, 0, sizeof(*cmd));
		cmd->op = ops[i];
		if (ops[i] == KPASS_OP_INIT) {
			cmd->init.num_buffers = 2;
			cmd->init.buf_size = 4096;
		}
		assert(io_uring_submit(&legacy) == 1);
		assert(!io_uring_wait_cqe(&legacy, &cqe));
		assert(!cqe->res);
		io_uring_cqe_seen(&legacy, cqe);
	}
	io_uring_queue_exit(&legacy);
	close(fd);
	puts("PASS: legacy INIT and buffer allocation with CQE16");
}

int main(void)
{
	setbuf(stdout, NULL);
	device = open(CEPH_KPASS_DEVICE, O_RDWR | O_CLOEXEC);
	assert(device >= 0);
	assert(!io_uring_queue_init(64, &ring,
				   IORING_SETUP_SQE128 | IORING_SETUP_CQE32));
	assembly_test();
	cancel_test();
	budget_test();
	compact_test();
	large_send_test();
	compact_lifetime_test();
	accept_test();
	framing_test();
	pending_limit_test();
	legacy_test();
	io_uring_queue_exit(&ring);
	close(device);
	puts("KPASS_STREAM_TEST_PASSED");
	return 0;
}
