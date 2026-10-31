/* SPDX-License-Identifier: MIT */
/* based on linux-kernel/tools/testing/selftests/net/msg_zerocopy.c */
#include <assert.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/errqueue.h>
#include <linux/if_packet.h>
#include <linux/io_uring.h>
#include <linux/ipv6.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <io_uring/mini_liburing.h>

#ifndef IORING_SEND_ZC_NO_RX_COPY
#define IORING_SEND_ZC_NO_RX_COPY	0x40	/* bit 6 of sqe->ioprio */
#endif

#define NOTIF_TAG 0xfffffffULL
#define NONZC_TAG 0
#define ZC_TAG 1

enum {
	MODE_NONZC	= 0,
	MODE_ZC		= 1,
	MODE_ZC_FIXED	= 2,
	MODE_MIXED	= 3,
};

static bool cfg_cork		= false;
static bool cfg_norx;		/* -x: IORING_SEND_ZC_NO_RX_COPY on the registered-buffer sends */
static bool cfg_reject;		/* -R: check where the flag is refused, and where it is not */
static int  cfg_tap_ms		= -1;	/* -T: a packet socket holds the frames this long */
static int  cfg_mode		= MODE_ZC_FIXED;
static int  cfg_nr_reqs		= 8;
static int  cfg_family		= PF_UNSPEC;
static int  cfg_payload_len;
static int  cfg_port		= 8000;
static int  cfg_runtime_ms	= 4200;

static socklen_t cfg_alen;
static struct sockaddr_storage cfg_dst_addr;

static char payload[IP_MAXPACKET] __attribute__((aligned(4096)));

static unsigned long gettimeofday_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

static void do_setsockopt(int fd, int level, int optname, int val)
{
	if (setsockopt(fd, level, optname, &val, sizeof(val)))
		error(1, errno, "setsockopt %d.%d: %d", level, optname, val);
}

static int do_setup_tx(int domain, int type, int protocol)
{
	int fd;

	fd = socket(domain, type, protocol);
	if (fd == -1)
		error(1, errno, "socket t");

	do_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, 1 << 21);

	if (connect(fd, (void *) &cfg_dst_addr, cfg_alen))
		error(1, errno, "connect");
	return fd;
}

static void do_tx(int domain, int type, int protocol)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	unsigned long packets = 0, bytes = 0;
	struct io_uring ring;
	struct iovec iov;
	uint64_t tstop;
	int i, fd, ret;
	int compl_cqes = 0;

	fd = do_setup_tx(domain, type, protocol);

	ret = io_uring_queue_init(512, &ring, 0);
	if (ret)
		error(1, -ret, "io_uring: queue init");

	iov.iov_base = payload;
	iov.iov_len = cfg_payload_len;

	ret = io_uring_register_buffers(&ring, &iov, 1);
	if (ret)
		error(1, -ret, "io_uring: buffer registration");

	tstop = gettimeofday_ms() + cfg_runtime_ms;
	do {
		if (cfg_cork)
			do_setsockopt(fd, IPPROTO_UDP, UDP_CORK, 1);

		for (i = 0; i < cfg_nr_reqs; i++) {
			unsigned zc_flags = 0;
			unsigned buf_idx = 0;
			unsigned mode = cfg_mode;
			unsigned msg_flags = MSG_WAITALL;

			if (cfg_mode == MODE_MIXED)
				mode = rand() % 3;

			sqe = io_uring_get_sqe(&ring);

			if (mode == MODE_NONZC) {
				io_uring_prep_send(sqe, fd, payload,
						   cfg_payload_len, msg_flags);
				sqe->user_data = NONZC_TAG;
			} else {
				io_uring_prep_sendzc(sqe, fd, payload,
						     cfg_payload_len,
						     msg_flags, zc_flags);
				if (mode == MODE_ZC_FIXED) {
					sqe->ioprio |= IORING_RECVSEND_FIXED_BUF;
					if (cfg_norx)
						sqe->ioprio |= IORING_SEND_ZC_NO_RX_COPY;
					sqe->buf_index = buf_idx;
				}
				sqe->user_data = ZC_TAG;
			}
		}

		ret = io_uring_submit(&ring);
		if (ret != cfg_nr_reqs)
			error(1, -ret, "submit");

		if (cfg_cork)
			do_setsockopt(fd, IPPROTO_UDP, UDP_CORK, 0);
		for (i = 0; i < cfg_nr_reqs; i++) {
			ret = io_uring_wait_cqe(&ring, &cqe);
			if (ret)
				error(1, -ret, "wait cqe");

			if (cqe->user_data != NONZC_TAG &&
			    cqe->user_data != ZC_TAG)
				error(1, EINVAL, "invalid cqe->user_data");

			if (cqe->flags & IORING_CQE_F_NOTIF) {
				if (cqe->flags & IORING_CQE_F_MORE)
					error(1, EINVAL, "invalid notif flags");
				if (compl_cqes <= 0)
					error(1, EINVAL, "notification mismatch");
				compl_cqes--;
				i--;
				io_uring_cqe_seen(&ring);
				continue;
			}
			if (cqe->flags & IORING_CQE_F_MORE) {
				if (cqe->user_data != ZC_TAG)
					error(1, -cqe->res, "unexpected F_MORE");
				compl_cqes++;
			}
			if (cqe->res >= 0) {
				packets++;
				bytes += cqe->res;
			} else if (cqe->res != -EAGAIN) {
				error(1, -cqe->res, "send failed");
			}
			io_uring_cqe_seen(&ring);
		}
	} while (gettimeofday_ms() < tstop);

	while (compl_cqes) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret)
			error(1, -ret, "wait cqe");
		if (cqe->flags & IORING_CQE_F_MORE)
			error(1, EINVAL, "invalid notif flags");
		if (!(cqe->flags & IORING_CQE_F_NOTIF))
			error(1, EINVAL, "missing notif flag");

		io_uring_cqe_seen(&ring);
		compl_cqes--;
	}

	fprintf(stderr, "tx=%lu (MB=%lu), tx/s=%lu (MB/s=%lu)\n",
			packets, bytes >> 20,
			packets / (cfg_runtime_ms / 1000),
			(bytes >> 20) / (cfg_runtime_ms / 1000));

	if (close(fd))
		error(1, errno, "close");
}

static bool peek_cqe(struct io_uring *ring, struct io_uring_cqe **cqe)
{
	struct io_uring_cq *cq = &ring->cq;

	read_barrier();
	if (*cq->khead == *cq->ktail)
		return false;
	*cqe = &cq->cqes[*cq->khead & *cq->kring_mask];
	return true;
}

/*
 * IORING_SEND_ZC_NO_RX_COPY is for SEND_ZC with a registered buffer: any other
 * send that carries it answers -EINVAL, and one that is right for it works,
 * also as a vector and as a SENDMSG_ZC.
 */
#define REJECT_CASES	5

static void do_reject(int domain, int type, int protocol)
{
	static const char * const what[REJECT_CASES] = {
		"SEND_ZC, buffer not registered", "SEND", "SEND_ZC registered",
		"SEND_ZC registered, vectorized", "SENDMSG_ZC registered",
	};
	static const int want_einval[REJECT_CASES] = { 1, 1, 0, 0, 0 };
	const unsigned int fixed = IORING_RECVSEND_FIXED_BUF | IORING_SEND_ZC_NO_RX_COPY;
	struct iovec iov, vec[2];
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	struct msghdr msg = { 0 };
	int res[REJECT_CASES], more[REJECT_CASES] = { 0 }, notifs[REJECT_CASES] = { 0 };
	int due = 0, i, fd, ret, mains = 0;
	size_t half = cfg_payload_len / 2;

	fd = do_setup_tx(domain, type, protocol);
	/* a send that fails in prep must not stop the submission of the rest */
	ret = io_uring_queue_init(16, &ring, IORING_SETUP_SUBMIT_ALL);
	if (ret)
		error(1, -ret, "io_uring: queue init");
	iov.iov_base = payload;
	iov.iov_len = cfg_payload_len;
	ret = io_uring_register_buffers(&ring, &iov, 1);
	if (ret)
		error(1, -ret, "io_uring: buffer registration");
	vec[0] = (struct iovec) { .iov_base = payload, .iov_len = half };
	vec[1] = (struct iovec) { .iov_base = payload + half, .iov_len = half };
	msg.msg_iov = vec;
	msg.msg_iovlen = 2;
	memset(res, 0, sizeof(res));

	for (i = 0; i < REJECT_CASES; i++) {
		sqe = io_uring_get_sqe(&ring);
		if (i == 1) {
			io_uring_prep_send(sqe, fd, payload, cfg_payload_len, 0);
			sqe->ioprio = IORING_SEND_ZC_NO_RX_COPY;
		} else {
			io_uring_prep_sendzc(sqe, fd, payload, cfg_payload_len, 0,
					     i == 0 ? IORING_SEND_ZC_NO_RX_COPY : fixed);
			sqe->buf_index = 0;
		}
		if (i == 3) {
			sqe->addr = (unsigned long)vec;
			sqe->len = 2;
			sqe->ioprio |= IORING_SEND_VECTORIZED;
		} else if (i == 4) {
			sqe->opcode = IORING_OP_SENDMSG_ZC;
			sqe->addr = (unsigned long)&msg;
			sqe->len = 1;
		}
		sqe->user_data = i + 1;
	}
	ret = io_uring_submit(&ring);
	if (ret != REJECT_CASES)
		error(1, ret < 0 ? -ret : EIO, "submit");

	while (mains < REJECT_CASES || due) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret)
			error(1, -ret, "wait cqe");
		i = cqe->user_data - 1;
		if (i < 0 || i >= REJECT_CASES)
			error(1, EINVAL, "invalid cqe->user_data");
		if (cqe->flags & IORING_CQE_F_NOTIF) {
			notifs[i]++;
			due--;
		} else {
			res[i] = cqe->res;
			mains++;
			/* F_MORE: a notification follows, also after a send that failed in prep */
			if (cqe->flags & IORING_CQE_F_MORE) {
				more[i] = 1;
				due++;
			}
		}
		io_uring_cqe_seen(&ring);
	}

	for (i = 0; i < REJECT_CASES; i++) {
		bool ok = want_einval[i] ? res[i] == -EINVAL : res[i] > 0;

		fprintf(stderr, "reject: %-32s res=%d notifs=%d %s\n", what[i], res[i], notifs[i],
			ok ? "ok" : "FAILED");
		if (!ok)
			error(1, EINVAL, "%s: unexpected result", what[i]);
		/* a notification for every F_MORE, and for a plain send neither */
		if (notifs[i] != more[i] || (i == 1 && more[i]) ||
		    (!want_einval[i] && !more[i]))
			error(1, EINVAL, "%s: %d notifications, F_MORE %d", what[i],
			      notifs[i], more[i]);
	}
	if (close(fd))
		error(1, errno, "close");
}

/*
 * A packet socket on the sending host is a tap, as tcpdump is: dev_queue_xmit_nit()
 * clones every frame that goes out for it, and the receive path copies the
 * clone, unless the request waived that. The tap reads nothing for the hold, so
 * what is waived is held: its notification may not be posted, and the data that
 * the tap reads when the hold is over is that of the buffer, unchanged. Without
 * the waiver the copy lets the notification go at once, and reports the copy.
 * UDP, so that a frame is a datagram.
 */
static void tap_hold(struct io_uring *ring, int ms, int *notifs)
{
	struct io_uring_cqe *cqe;
	unsigned long stop = gettimeofday_ms() + ms;

	do {
		while (peek_cqe(ring, &cqe)) {
			if (!(cqe->flags & IORING_CQE_F_NOTIF))
				error(1, EINVAL, "unexpected cqe while the tap holds");
			notifs[0]++;
			if (cqe->res & IORING_NOTIF_USAGE_ZC_COPIED)
				notifs[1]++;
			io_uring_cqe_seen(ring);
		}
		usleep(1000);
	} while (gettimeofday_ms() < stop);
}

static unsigned int tap_drain(int tap, int dport, unsigned int *bad)
{
	static unsigned char frame[IP_MAXPACKET + 128];
	unsigned int frames = 0;

	for (;;) {
		struct sockaddr_ll from;
		socklen_t alen = sizeof(from);
		const unsigned char *p = frame + ETH_HLEN, *udp;
		unsigned int proto, len, udplen;
		ssize_t n;

		n = recvfrom(tap, frame, sizeof(frame), MSG_DONTWAIT,
			     (struct sockaddr *)&from, &alen);
		if (n < 0 && errno == EAGAIN)
			return frames;
		if (n < 0)
			error(1, errno, "tap recv");
		if (from.sll_pkttype != PACKET_OUTGOING || n < ETH_HLEN)
			continue;
		proto = frame[12] << 8 | frame[13];
		if (proto == ETH_P_IP && n >= ETH_HLEN + 28 && p[9] == IPPROTO_UDP)
			len = (p[0] & 0xf) * 4;
		else if (proto == ETH_P_IPV6 && n >= ETH_HLEN + 48 && p[6] == IPPROTO_UDP)
			len = 40;
		else
			continue;
		udp = p + len;
		if ((udp[2] << 8 | udp[3]) != dport)
			continue;
		udplen = (udp[4] << 8 | udp[5]) - sizeof(struct udphdr);
		if (udplen != (unsigned int)cfg_payload_len ||
		    n < (ssize_t)(ETH_HLEN + len + sizeof(struct udphdr) + udplen))
			error(1, EINVAL, "tap: a frame of %u payload bytes, want %d", udplen,
			      cfg_payload_len);
		if (memcmp(udp + sizeof(struct udphdr), payload, udplen))
			(*bad)++;
		frames++;
	}
}

static void do_tap(int domain, int type, int protocol)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct io_uring ring;
	struct iovec iov;
	unsigned int frames, bad = 0;
	int notifs[2] = { 0 }, sends = 0, tap, fd, i, ret, rcvbuf = 64 << 20;
	unsigned int ioprio = IORING_RECVSEND_FIXED_BUF | IORING_SEND_ZC_REPORT_USAGE;

	if (type != SOCK_DGRAM)
		error(1, 0, "-T: udp only");
	/* open before the first send: every frame of the run is cloned to it */
	tap = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (tap < 0)
		error(1, errno, "socket packet");
	if (setsockopt(tap, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)))
		error(1, errno, "setsockopt SO_RCVBUFFORCE");
	fd = do_setup_tx(domain, type, protocol);
	ret = io_uring_queue_init(512, &ring, 0);
	if (ret)
		error(1, -ret, "io_uring: queue init");
	iov.iov_base = payload;
	iov.iov_len = cfg_payload_len;
	ret = io_uring_register_buffers(&ring, &iov, 1);
	if (ret)
		error(1, -ret, "io_uring: buffer registration");
	if (cfg_norx)
		ioprio |= IORING_SEND_ZC_NO_RX_COPY;

	for (i = 0; i < cfg_nr_reqs; i++) {
		sqe = io_uring_get_sqe(&ring);
		io_uring_prep_sendzc(sqe, fd, payload, cfg_payload_len, 0, ioprio);
		sqe->buf_index = 0;
		sqe->user_data = ZC_TAG;
	}
	ret = io_uring_submit(&ring);
	if (ret != cfg_nr_reqs)
		error(1, ret < 0 ? -ret : EIO, "submit");
	/* the sends complete, each with a notification to follow */
	while (sends < cfg_nr_reqs) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret)
			error(1, -ret, "wait cqe");
		if (cqe->flags & IORING_CQE_F_NOTIF) {
			notifs[0]++;
			if (cqe->res & IORING_NOTIF_USAGE_ZC_COPIED)
				notifs[1]++;
		} else {
			if (cqe->res != cfg_payload_len || !(cqe->flags & IORING_CQE_F_MORE))
				error(1, cqe->res < 0 ? -cqe->res : EIO, "send: res %d flags %x",
				      cqe->res, cqe->flags);
			sends++;
		}
		io_uring_cqe_seen(&ring);
	}

	tap_hold(&ring, cfg_tap_ms, notifs);
	fprintf(stderr, "tap: held %d ms: %d of %d notifications posted\n", cfg_tap_ms,
		notifs[0], cfg_nr_reqs);
	if (cfg_norx && notifs[0])
		error(1, EINVAL, "a tap holds the frames, but their notifications are posted");

	frames = tap_drain(tap, cfg_port, &bad);
	/* the tap let go of what it held: now they are all due */
	while (notifs[0] < cfg_nr_reqs) {
		ret = io_uring_wait_cqe(&ring, &cqe);
		if (ret)
			error(1, -ret, "wait cqe");
		if (!(cqe->flags & IORING_CQE_F_NOTIF))
			error(1, EINVAL, "unexpected cqe");
		notifs[0]++;
		if (cqe->res & IORING_NOTIF_USAGE_ZC_COPIED)
			notifs[1]++;
		io_uring_cqe_seen(&ring);
	}
	fprintf(stderr, "tap: %u frames of %d bytes, %u changed; %d notifs, %d report a copy\n",
		frames, cfg_payload_len, bad, notifs[0], notifs[1]);
	if (frames != (unsigned int)cfg_nr_reqs || bad)
		error(1, EINVAL, "the tap read %u frames, %u changed, want %d unchanged",
		      frames, bad, cfg_nr_reqs);
	if (cfg_norx && notifs[1])
		error(1, EINVAL, "%d notifications report a copy that was waived", notifs[1]);
	if (!cfg_norx && notifs[1] != cfg_nr_reqs)
		error(1, EINVAL, "%d of %d notifications report the tap's copy", notifs[1],
		      cfg_nr_reqs);
	if (close(fd) || close(tap))
		error(1, errno, "close");
}

static void do_test(int domain, int type, int protocol)
{
	int i;

	for (i = 0; i < IP_MAXPACKET; i++)
		payload[i] = 'a' + (i % 26);
	if (cfg_reject)
		do_reject(domain, type, protocol);
	else if (cfg_tap_ms >= 0)
		do_tap(domain, type, protocol);
	else
		do_tx(domain, type, protocol);
}

static void usage(const char *filepath)
{
	fprintf(stderr, "%s -x: IORING_SEND_ZC_NO_RX_COPY on registered-buffer sends\n",
		filepath);
	fprintf(stderr, "%s -R: check where the flag is refused; -T<ms>: a tap holds frames\n",
		filepath);
	error(1, 0, "Usage: %s (-4|-6) (udp|tcp) -D<dst_ip> [-s<payload size>] "
		    "[-t<time s>] [-n<batch>] [-p<port>] [-m<mode>]", filepath);
}

static void parse_opts(int argc, char **argv)
{
	const int max_payload_len = sizeof(payload) -
				    sizeof(struct ipv6hdr) -
				    sizeof(struct tcphdr) -
				    40 /* max tcp options */;
	struct sockaddr_in6 *addr6 = (void *) &cfg_dst_addr;
	struct sockaddr_in *addr4 = (void *) &cfg_dst_addr;
	char *daddr = NULL;
	int c;

	if (argc <= 1)
		usage(argv[0]);
	cfg_payload_len = max_payload_len;

	while ((c = getopt(argc, argv, "46D:p:s:t:n:c:m:xRT:")) != -1) {
		switch (c) {
		case '4':
			if (cfg_family != PF_UNSPEC)
				error(1, 0, "Pass one of -4 or -6");
			cfg_family = PF_INET;
			cfg_alen = sizeof(struct sockaddr_in);
			break;
		case '6':
			if (cfg_family != PF_UNSPEC)
				error(1, 0, "Pass one of -4 or -6");
			cfg_family = PF_INET6;
			cfg_alen = sizeof(struct sockaddr_in6);
			break;
		case 'D':
			daddr = optarg;
			break;
		case 'p':
			cfg_port = strtoul(optarg, NULL, 0);
			break;
		case 's':
			cfg_payload_len = strtoul(optarg, NULL, 0);
			break;
		case 't':
			cfg_runtime_ms = 200 + strtoul(optarg, NULL, 10) * 1000;
			break;
		case 'n':
			cfg_nr_reqs = strtoul(optarg, NULL, 0);
			break;
		case 'c':
			cfg_cork = strtol(optarg, NULL, 0);
			break;
		case 'm':
			cfg_mode = strtol(optarg, NULL, 0);
			break;
		case 'x':
			cfg_norx = true;
			break;
		case 'R':
			cfg_reject = true;
			break;
		case 'T':
			cfg_tap_ms = strtol(optarg, NULL, 0);
			break;
		}
	}

	switch (cfg_family) {
	case PF_INET:
		memset(addr4, 0, sizeof(*addr4));
		addr4->sin_family = AF_INET;
		addr4->sin_port = htons(cfg_port);
		if (daddr &&
		    inet_pton(AF_INET, daddr, &(addr4->sin_addr)) != 1)
			error(1, 0, "ipv4 parse error: %s", daddr);
		break;
	case PF_INET6:
		memset(addr6, 0, sizeof(*addr6));
		addr6->sin6_family = AF_INET6;
		addr6->sin6_port = htons(cfg_port);
		if (daddr &&
		    inet_pton(AF_INET6, daddr, &(addr6->sin6_addr)) != 1)
			error(1, 0, "ipv6 parse error: %s", daddr);
		break;
	default:
		error(1, 0, "illegal domain");
	}

	if (cfg_payload_len > max_payload_len)
		error(1, 0, "-s: payload exceeds max (%d)", max_payload_len);
	if (optind != argc - 1)
		usage(argv[0]);
}

int main(int argc, char **argv)
{
	const char *cfg_test = argv[argc - 1];

	parse_opts(argc, argv);

	if (!strcmp(cfg_test, "tcp"))
		do_test(cfg_family, SOCK_STREAM, 0);
	else if (!strcmp(cfg_test, "udp"))
		do_test(cfg_family, SOCK_DGRAM, 0);
	else
		error(1, 0, "unknown cfg_test %s", cfg_test);
	return 0;
}
