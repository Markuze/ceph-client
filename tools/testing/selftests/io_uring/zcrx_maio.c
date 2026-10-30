// SPDX-License-Identifier: GPL-2.0
/*
 * Test the MAIO mode of io_uring zero-copy receive: a kernel-allocated area
 * that the process maps read-write, and a provider of readable pages on every
 * receive queue of a device.
 *
 * The tests use raw SQEs and the 32-byte CQE ABI, no liburing. The device
 * tests use netdevsim and need a kernel that has it. Without it, they skip.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/ethtool.h>
#include <linux/io_uring.h>
#include <linux/io_uring/query.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../kselftest.h"

#define PLAN			14
#define AREA_PAGES		64
#define RQ_ENTRIES		64
#define RQ_REGION_SIZE		8192
/* The stats of an instance are in the refill ring region, behind the entries. */
#define STATS_OFFSET		2048
#define NOTIF_TAG		0xe0e0e0e0ULL
#define MAX_DATA		512
/* The kernel keeps these two values private. The mmap offsets are documented
 * in the commit that adds them. The tests check them against the offset that
 * the kernel returns.
 */
#define MAP_OFF_ZCRX_AREA	0x38000000ULL
#define OFF_ZCRX_SHIFT		16

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
};

/* One zcrx instance as the application sees it. */
struct zcrx {
	struct ring *ring;
	uint32_t id;
	struct io_uring_zcrx_offsets off;
	struct io_uring_region_desc rd;
	struct io_uring_zcrx_area_reg area;
	struct io_uring_zcrx_ifq_reg reg;
	struct zcrx_event_desc ev;
	void *rq;			/* the mapped refill ring region */
	unsigned char *map;		/* the mapped area, if any */
};

static uint64_t next_tag = 1000;
static size_t page_size;

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

	if (head == __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE))
		return false;
	*event = r->cqes[head & *r->cq_mask];
	__atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
	return true;
}

static int zcrx_register_raw(struct ring *r, struct io_uring_zcrx_ifq_reg *reg)
{
	int ret = syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_ZCRX_IFQ, reg, 1);

	return ret < 0 ? -errno : ret;
}

static int zcrx_ctrl(struct ring *r, struct zcrx_ctrl *ctrl)
{
	int ret = syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_ZCRX_CTRL, ctrl, 0);

	return ret < 0 ? -errno : ret;
}

/*
 * Register an instance. The area is a kernel area of @pages pages, except
 * that a test can change the registration before the call.
 */
static void zcrx_prepare(struct zcrx *z, struct ring *r, unsigned int flags,
			 unsigned int if_idx, unsigned int pages)
{
	memset(z, 0, sizeof(*z));
	z->ring = r;
	z->rd.size = RQ_REGION_SIZE;
	z->area.len = (uint64_t)pages * page_size;
	z->area.flags = IORING_ZCRX_AREA_KERNEL;
	z->reg.if_idx = if_idx;
	z->reg.rq_entries = RQ_ENTRIES;
	z->reg.flags = flags;
	z->reg.area_ptr = (uintptr_t)&z->area;
	z->reg.region_ptr = (uintptr_t)&z->rd;
}

/* Ask for the events in @mask and for the stats area, before the registration. */
static void zcrx_want_events(struct zcrx *z, uint32_t mask)
{
	z->ev.user_data = NOTIF_TAG;
	z->ev.type_mask = mask;
	z->ev.flags = ZCRX_EVENT_DESC_FLAG_STATS;
	z->ev.stats_offset = STATS_OFFSET;
	z->reg.event_desc = (uintptr_t)&z->ev;
}

static struct zcrx_stats *zcrx_stats(struct zcrx *z)
{
	return z->rq + STATS_OFFSET;
}

static uint64_t stat_read(const __u64 *counter)
{
	return __atomic_load_n(counter, __ATOMIC_RELAXED);
}

static int zcrx_do_register(struct zcrx *z)
{
	int ret = zcrx_register_raw(z->ring, &z->reg);

	if (ret < 0)
		return ret;
	z->id = z->reg.zcrx_id;
	z->off = z->reg.offsets;
	z->rq = mmap(NULL, z->rd.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		     z->ring->fd, z->rd.mmap_offset);
	require(z->rq != MAP_FAILED, "map the refill ring region");
	return 0;
}

static void zcrx_close(struct zcrx *z)
{
	if (z->map)
		munmap(z->map, z->area.len);
	if (z->rq)
		munmap(z->rq, z->rd.size);
	z->map = NULL;
	z->rq = NULL;
}

static void *map_area(struct ring *r, uint64_t offset, size_t len, int prot, int flags)
{
	return mmap(NULL, len, prot, flags, r->fd, offset);
}

static unsigned int *rq_head(struct zcrx *z)
{
	return z->rq + z->off.head;
}

static unsigned int *rq_tail(struct zcrx *z)
{
	return z->rq + z->off.tail;
}

static struct io_uring_zcrx_rqe *rq_rqes(struct zcrx *z)
{
	return z->rq + z->off.rqes;
}

static void fill_pattern(unsigned char *p, size_t len, unsigned int seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		p[i] = (unsigned char)((i * 131 + seed) ^ (i >> 8));
}

static bool check_pattern(const unsigned char *p, size_t len, unsigned int seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (p[i] != (unsigned char)((i * 131 + seed) ^ (i >> 8)))
			return false;
	return true;
}

static bool is_zero(const unsigned char *p, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (p[i])
			return false;
	return true;
}

/* Return the VmFlags line of the mapping at @addr, or NULL. */
static bool vm_flags_of(void *addr, char *out, size_t len)
{
	FILE *f = fopen("/proc/self/smaps", "r");
	char line[512];
	bool in_map = false;
	unsigned long start, end;

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx ", &start, &end) == 2) {
			in_map = (unsigned long)addr >= start && (unsigned long)addr < end;
			continue;
		}
		if (in_map && !strncmp(line, "VmFlags:", 8)) {
			snprintf(out, len, "%.*s", (int)len - 1, line + 8);
			fclose(f);
			return true;
		}
	}
	fclose(f);
	return false;
}

static bool has_flag(const char *flags, const char *flag)
{
	char pat[8];
	const char *p;

	snprintf(pat, sizeof(pat), " %s ", flag);
	p = strstr(flags, pat);
	if (p)
		return true;
	/* The first and the last flag lack one space. */
	snprintf(pat, sizeof(pat), " %s\n", flag);
	return strstr(flags, pat);
}

/* 1. Register, map the area read-write, write a pattern and read it back. */
static void test_register_map(struct ring *r)
{
	struct zcrx z, z2;
	size_t len;
	unsigned int i;
	char flags[256];
	uint64_t want;
	void *priv, *grow;
	pid_t pid;
	int status;

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	len = z.area.len;
	require(!zcrx_do_register(&z), "register MAIO|NODEV with a kernel area");
	require(z.reg.rx_buf_len == page_size, "the slot size is the page size");
	want = MAP_OFF_ZCRX_AREA + ((uint64_t)z.id << OFF_ZCRX_SHIFT);
	require(z.area.mmap_offset == want, "the kernel returns the area mmap offset");
	require(!z.area.rq_area_token, "the area has id 0");

	/* A private mapping would copy on write. The kernel refuses it. */
	priv = map_area(r, z.area.mmap_offset, len, PROT_READ | PROT_WRITE, MAP_PRIVATE);
	require(priv == MAP_FAILED && errno == EINVAL, "a private mapping is refused");
	/* No area exists for the next zcrx id. */
	priv = map_area(r, z.area.mmap_offset + (1ULL << OFF_ZCRX_SHIFT), len,
			PROT_READ | PROT_WRITE, MAP_SHARED);
	require(priv == MAP_FAILED, "an unknown id has no area");

	z.map = map_area(r, z.area.mmap_offset, len, PROT_READ | PROT_WRITE, MAP_SHARED);
	require(z.map != MAP_FAILED, "map the area read-write");
	require(is_zero(z.map, len), "the kernel zeroes the area");

	for (i = 0; i < AREA_PAGES; i++)
		fill_pattern(z.map + i * page_size, page_size, i + 1);
	for (i = 0; i < AREA_PAGES; i++)
		require(check_pattern(z.map + i * page_size, page_size, i + 1),
			"read back the pattern through the mapping");

	/* A second mapping shows the same pages. */
	z2 = z;
	z2.map = map_area(r, z.area.mmap_offset, len, PROT_READ | PROT_WRITE, MAP_SHARED);
	require(z2.map != MAP_FAILED, "map the area a second time");
	for (i = 0; i < AREA_PAGES; i++)
		require(check_pattern(z2.map + i * page_size, page_size, i + 1),
			"the second mapping shows the same pages");
	z2.map[0] ^= 0xff;
	require(z.map[0] == z2.map[0], "a write shows in both mappings");
	munmap(z2.map, len);

	require(vm_flags_of(z.map, flags, sizeof(flags)), "find the VMA flags");
	ksft_print_msg("area VMA flags:%s", flags);
	require(has_flag(flags, "sh"), "the mapping is shared");
	require(has_flag(flags, "dc"), "the mapping is not inherited (VM_DONTCOPY)");
	require(has_flag(flags, "dd"), "the mapping is not dumped (VM_DONTDUMP)");
	require(has_flag(flags, "de"), "the mapping cannot grow (VM_DONTEXPAND)");

	grow = mremap(z.map, len, len * 2, 0);
	require(grow == MAP_FAILED, "the mapping does not grow");

	/* A child does not inherit the mapping. */
	pid = fork();
	require(pid >= 0, "fork");
	if (!pid) {
		signal(SIGSEGV, SIG_DFL);
		(void)__atomic_load_n(z.map, __ATOMIC_RELAXED);
		_exit(0);
	}
	require(waitpid(pid, &status, 0) == pid, "wait for the child");
	require(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
		"the child cannot read the area");

	/* A mapping survives a new one after the old one is gone. */
	munmap(z.map, len);
	z.map = map_area(r, z.area.mmap_offset, len, PROT_READ | PROT_WRITE, MAP_SHARED);
	require(z.map != MAP_FAILED, "map the area again");
	require(z.map[1] == (unsigned char)(((1 * 131 + 1) ^ 0)), "data survives a remap");
	zcrx_close(&z);
	ksft_test_result_pass("register MAIO|NODEV, map the area read-write, data and VMA flags\n");
}

/* Two instances of one ring have separate areas and separate mmap offsets. */
static void test_two_instances(struct ring *r)
{
	struct zcrx a, b;

	zcrx_prepare(&a, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, 16);
	zcrx_prepare(&b, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, 32);
	require(!zcrx_do_register(&a), "register the first instance");
	require(!zcrx_do_register(&b), "register the second instance");
	require(a.id != b.id, "the instances have different ids");
	require(b.area.mmap_offset - a.area.mmap_offset ==
		((uint64_t)b.id - a.id) << OFF_ZCRX_SHIFT, "the offsets follow the ids");

	a.map = map_area(r, a.area.mmap_offset, a.area.len, PROT_READ | PROT_WRITE, MAP_SHARED);
	b.map = map_area(r, b.area.mmap_offset, b.area.len, PROT_READ | PROT_WRITE, MAP_SHARED);
	require(a.map != MAP_FAILED && b.map != MAP_FAILED, "map both areas");
	fill_pattern(a.map, a.area.len, 21);
	require(is_zero(b.map, b.area.len), "the second area is not the first");
	fill_pattern(b.map, b.area.len, 22);
	require(check_pattern(a.map, a.area.len, 21), "the first area keeps its data");
	/* A mapping that is shorter than the area fails. */
	require(map_area(r, b.area.mmap_offset, b.area.len - page_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED) == MAP_FAILED, "a mapping shorter than the area fails");
	zcrx_close(&a);
	zcrx_close(&b);
	ksft_test_result_pass("two instances of a ring map separate areas\n");
}

/* 2. The kernel rejects inconsistent flag combinations. */
static void test_flag_validation(struct ring *r)
{
	struct zcrx z;
	int ret;

	zcrx_prepare(&z, r, ZCRX_REG_NODEV, 0, AREA_PAGES);
	ret = zcrx_do_register(&z);
	require(ret == -EINVAL, "a kernel area needs ZCRX_REG_MAIO");

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	z.area.flags = 0;
	require(zcrx_do_register(&z) < 0, "ZCRX_REG_MAIO needs a kernel area");

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	z.area.flags |= IORING_ZCRX_AREA_DMABUF;
	require(zcrx_do_register(&z) == -EINVAL, "a kernel area is not a dma-buf");

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	z.area.addr = 0x10000;
	require(zcrx_do_register(&z) == -EINVAL, "a kernel area takes no address");

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	z.area.mmap_offset = 1;
	require(zcrx_do_register(&z) == -EINVAL, "the mmap offset is an output");

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	z.area.len += 1;
	require(zcrx_do_register(&z) == -EINVAL, "the length is a page multiple");

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	z.area.len = 0;
	require(zcrx_do_register(&z) < 0, "the length is not zero");

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	z.reg.if_rxq = 1;
	require(zcrx_do_register(&z) == -EINVAL, "a MAIO instance names no queue");

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	z.reg.rx_buf_len = 2 * page_size;
	require(zcrx_do_register(&z) == -EINVAL, "a slot is one page");

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 5, AREA_PAGES);
	require(zcrx_do_register(&z) == -EINVAL, "NODEV takes no device");

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_IMPORT, 0, AREA_PAGES);
	require(zcrx_do_register(&z) < 0, "MAIO cannot combine with import");

	/* After all the failures, a valid registration still works. */
	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	require(!zcrx_do_register(&z), "register after failures");
	zcrx_close(&z);
	ksft_test_result_pass("reject inconsistent MAIO flags and fields\n");
}

/* 3. Entries in the refill ring are parsed, and invalid ones are dropped. */
static void test_refill_ring(struct ring *r)
{
	struct zcrx_ctrl ctrl = {};
	struct zcrx z;
	unsigned int i, tail;

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	require(!zcrx_do_register(&z), "register for the refill ring test");

	/*
	 * No slot has a user reference, so a flush returns nothing to the
	 * area. It consumes the entries.
	 */
	tail = *rq_tail(&z);
	for (i = 0; i < 4; i++) {
		struct io_uring_zcrx_rqe *rqe = &rq_rqes(&z)[(tail + i) & (RQ_ENTRIES - 1)];

		rqe->off = (uint64_t)i * page_size;
		rqe->len = page_size;
		rqe->__pad = 0;
	}
	__atomic_store_n(rq_tail(&z), tail + 4, __ATOMIC_RELEASE);
	ctrl.zcrx_id = z.id;
	ctrl.op = ZCRX_CTRL_FLUSH_RQ;
	require(!zcrx_ctrl(r, &ctrl), "flush the refill ring");
	require(__atomic_load_n(rq_head(&z), __ATOMIC_ACQUIRE) == tail + 4,
		"the flush consumes the entries");

	/* An offset behind the area stops the parser at that entry. */
	tail = *rq_tail(&z);
	rq_rqes(&z)[tail & (RQ_ENTRIES - 1)].off = (uint64_t)AREA_PAGES * page_size;
	rq_rqes(&z)[tail & (RQ_ENTRIES - 1)].len = page_size;
	rq_rqes(&z)[tail & (RQ_ENTRIES - 1)].__pad = 0;
	__atomic_store_n(rq_tail(&z), tail + 1, __ATOMIC_RELEASE);
	require(!zcrx_ctrl(r, &ctrl), "flush an invalid entry");
	require(__atomic_load_n(rq_head(&z), __ATOMIC_ACQUIRE) == tail + 1,
		"the flush drops an invalid entry");

	zcrx_close(&z);
	ksft_test_result_pass("flush the refill ring of a MAIO instance\n");
}

/* 4. A MAIO instance has one area. */
static void test_add_area(struct ring *r)
{
	struct io_uring_zcrx_area_reg area = {};
	struct zcrx_ctrl ctrl = {};
	struct zcrx z;

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	require(!zcrx_do_register(&z), "register for the add area test");

	area.len = AREA_PAGES * page_size;
	area.flags = IORING_ZCRX_AREA_KERNEL;
	ctrl.zcrx_id = z.id;
	ctrl.op = ZCRX_CTRL_ADD_AREA;
	ctrl.zc_area.area_ptr = (uintptr_t)&area;
	require(zcrx_ctrl(r, &ctrl) == -EOPNOTSUPP, "ADD_AREA is not supported");
	zcrx_close(&z);
	ksft_test_result_pass("ZCRX_CTRL_ADD_AREA returns -EOPNOTSUPP on a MAIO instance\n");
}

/* 5. A second ring imports the instance and maps the same pages. */
static void test_export_import(struct ring *r)
{
	struct io_uring_zcrx_area_reg info = {};
	struct io_uring_zcrx_ifq_reg reg = {};
	struct zcrx_ctrl ctrl = {};
	struct ring r2;
	struct zcrx z;
	unsigned char *map2;
	int fd;

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	require(!zcrx_do_register(&z), "register for the export test");
	z.map = map_area(r, z.area.mmap_offset, z.area.len, PROT_READ | PROT_WRITE, MAP_SHARED);
	require(z.map != MAP_FAILED, "map the exported area");
	fill_pattern(z.map, page_size, 7);

	ctrl.zcrx_id = z.id;
	ctrl.op = ZCRX_CTRL_EXPORT;
	require(!zcrx_ctrl(r, &ctrl), "export the instance");
	fd = ctrl.zc_export.zcrx_fd;
	require(fd >= 0, "the export returns a file descriptor");

	require(!ring_init(&r2, IORING_SETUP_CQE32), "second ring");
	/* The area pointer is an output buffer for a MAIO instance. */
	reg.if_idx = fd;
	reg.flags = ZCRX_REG_IMPORT;
	reg.area_ptr = (uintptr_t)&info;
	require(zcrx_register_raw(&r2, &reg) == 0, "import the instance");
	require(info.flags == IORING_ZCRX_AREA_KERNEL && info.len == z.area.len,
		"import reports the area");
	require(info.mmap_offset == MAP_OFF_ZCRX_AREA + ((uint64_t)reg.zcrx_id << OFF_ZCRX_SHIFT),
		"import reports the offset for this ring's id");

	map2 = map_area(&r2, info.mmap_offset, info.len, PROT_READ | PROT_WRITE, MAP_SHARED);
	require(map2 != MAP_FAILED, "the importing ring maps the area");
	require(check_pattern(map2, page_size, 7), "the importer sees the exporter's data");
	map2[page_size] = 0x5a;
	require(z.map[page_size] == 0x5a, "the exporter sees the importer's write");

	/* The instance outlives the exporting ring. */
	munmap(z.map, z.area.len);
	z.map = NULL;
	zcrx_close(&z);
	ring_exit(r);
	require(check_pattern(map2, page_size, 7), "the importer keeps the data");
	munmap(map2, info.len);
	close(fd);
	ring_exit(&r2);
	ksft_test_result_pass("export and import a MAIO instance, both rings map the same pages\n");
}

static void drop_cap(unsigned int cap)
{
	struct __user_cap_header_struct hdr = { .version = _LINUX_CAPABILITY_VERSION_3 };
	struct __user_cap_data_struct data[2];

	require(!syscall(SYS_capget, &hdr, data), "capget");
	data[0].effective &= ~(1U << cap);
	data[0].permitted &= ~(1U << cap);
	data[0].inheritable &= ~(1U << cap);
	require(!syscall(SYS_capset, &hdr, data), "capset");
}

/* 6. The registration needs CAP_NET_ADMIN. */
static void test_capability(void)
{
	pid_t pid = fork();
	int status;

	require(pid >= 0, "fork");
	if (!pid) {
		struct ring r;
		struct zcrx z;
		int ret;

		drop_cap(CAP_NET_ADMIN);
		if (ring_init(&r, IORING_SETUP_CQE32))
			_exit(2);
		zcrx_prepare(&z, &r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
		ret = zcrx_register_raw(&r, &z.reg);
		_exit(ret == -EPERM ? 0 : 1);
	}
	require(waitpid(pid, &status, 0) == pid, "wait for the child");
	require(WIFEXITED(status) && !WEXITSTATUS(status),
		"registration without CAP_NET_ADMIN returns -EPERM");
	ksft_test_result_pass("registration without CAP_NET_ADMIN fails with -EPERM\n");
}

static void tcp_pair(int pair[2])
{
	struct sockaddr_in addr = { .sin_family = AF_INET };
	socklen_t len = sizeof(addr);
	int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

	require(listener >= 0, "listen socket");
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	require(!bind(listener, (void *)&addr, len), "bind");
	require(!getsockname(listener, (void *)&addr, &len), "getsockname");
	require(!listen(listener, 1), "listen");
	pair[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	require(pair[0] >= 0, "connect socket");
	require(!connect(pair[0], (void *)&addr, sizeof(addr)), "connect");
	pair[1] = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
	require(pair[1] >= 0, "accept");
	close(listener);
}

/* The byte of a stream at position @pos. */
static unsigned char stream_byte(uint64_t pos, unsigned int seed)
{
	return (unsigned char)((pos * 131 + seed) ^ (pos >> 8));
}

static void fill_stream(unsigned char *p, uint64_t pos, size_t len, unsigned int seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		p[i] = stream_byte(pos + i, seed);
}

static bool check_stream(const unsigned char *p, uint64_t pos, size_t len, unsigned int seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (p[i] != stream_byte(pos + i, seed))
			return false;
	return true;
}

static void send_stream(int fd, uint64_t pos, size_t len, unsigned int seed)
{
	unsigned char *buf = malloc(len);
	size_t done = 0;

	require(buf, "buffer for the stream");
	fill_stream(buf, pos, len, seed);
	while (done < len) {
		ssize_t n = send(fd, buf + done, len - done, MSG_NOSIGNAL);

		require(n > 0, "send the stream");
		done += n;
	}
	free(buf);
}

/* The completions of one RECV_ZC request and of the events of its instance. */
struct rx {
	uint64_t recv_tag;
	struct event data[MAX_DATA];
	unsigned int nr_data;
	unsigned int nr_event[2];	/* by ZCRX_EVENT_* */
	uint64_t bytes;			/* the sum of the data CQEs */
	bool done;			/* the CQE that ends the request came */
	int final_res;
};

/* Start a request. The event counts of @rx survive, the rest starts again. */
static void rx_start(struct ring *r, struct rx *rx, struct zcrx *z, int fd)
{
	struct io_uring_sqe sqe = {};

	rx->nr_data = 0;
	rx->bytes = 0;
	rx->done = false;
	sqe.opcode = IORING_OP_RECV_ZC;
	sqe.fd = fd;
	sqe.ioprio = IORING_RECV_MULTISHOT;
	sqe.zcrx_ifq_idx = z->id;
	sqe.user_data = next_tag++;
	rx->recv_tag = sqe.user_data;
	stage(r, sqe);
}

static void rx_drain(struct ring *r, struct rx *rx)
{
	struct event e;

	while (peek(r, &e)) {
		if (e.tag == rx->recv_tag && (e.flags & IORING_CQE_F_MORE)) {
			require(rx->nr_data < MAX_DATA, "room for the data CQEs");
			require(e.res > 0, "a data CQE carries bytes");
			rx->data[rx->nr_data++] = e;
			rx->bytes += e.res;
		} else if (e.tag == rx->recv_tag) {
			rx->done = true;
			rx->final_res = e.res;
		} else if (e.tag == NOTIF_TAG) {
			require(e.res >= 0 && e.res < 2, "a known event type");
			rx->nr_event[e.res]++;
		}
	}
}

/* Run the ring until @bytes bytes arrived or the request ended. */
static void rx_wait(struct ring *r, struct rx *rx, uint64_t bytes)
{
	alarm(120);
	for (;;) {
		rx_drain(r, rx);
		if (rx->bytes >= bytes || rx->done)
			break;
		enter(r, 1);
	}
	/* Run the deferred work that is left, so that the event CQEs are in. */
	enter(r, 0);
	rx_drain(r, rx);
	alarm(0);
}

/* Close the sending side and wait for the CQE that ends the request. */
static void rx_finish(struct ring *r, struct rx *rx, int sender)
{
	close(sender);
	rx_wait(r, rx, UINT64_MAX);
	require(rx->done, "the request ends");
}

/*
 * Check the data CQEs from @first on. Each names a copy in the area. The area
 * holds the bytes of the stream that the peer sent. Return the bit mask of the
 * slots that the completions name.
 */
static uint64_t rx_check_copies(struct zcrx *z, struct rx *rx, unsigned int first,
				uint64_t pos, unsigned int seed)
{
	uint64_t slots = 0;
	unsigned int i;

	for (i = first; i < rx->nr_data; i++) {
		uint64_t off = rx->data[i].extra[0];
		uint32_t len = rx->data[i].res;

		require(!(off >> IORING_ZCRX_AREA_SHIFT), "the completion names area 0");
		require(!(off & (page_size - 1)), "a copy starts at the start of a page");
		require(len <= page_size, "a copy fits in a page");
		require(off + len <= z->area.len, "the completion is inside the area");
		require(check_stream(z->map + off, pos, len, seed),
			"the area holds the bytes that the peer sent");
		slots |= 1ULL << (off / page_size);
		pos += len;
	}
	return slots;
}

static void rq_push(struct zcrx *z, uint64_t off)
{
	unsigned int tail = *rq_tail(z);
	struct io_uring_zcrx_rqe *rqe = &rq_rqes(z)[tail & (RQ_ENTRIES - 1)];

	require(tail - __atomic_load_n(rq_head(z), __ATOMIC_ACQUIRE) < RQ_ENTRIES,
		"room in the refill ring");
	rqe->off = off;
	rqe->len = page_size;
	rqe->__pad = 0;
	__atomic_store_n(rq_tail(z), tail + 1, __ATOMIC_RELEASE);
}

/* Give the slots of the data CQEs from @first on back to the kernel. */
static void rq_return(struct zcrx *z, struct rx *rx, unsigned int first)
{
	unsigned int i;

	for (i = first; i < rx->nr_data; i++)
		rq_push(z, rx->data[i].extra[0]);
}

/* An instance without a device does not consume the refill ring by itself. */
static void rq_flush(struct ring *r, struct zcrx *z)
{
	struct zcrx_ctrl ctrl = { .zcrx_id = z->id, .op = ZCRX_CTRL_FLUSH_RQ };

	require(!zcrx_ctrl(r, &ctrl), "flush the refill ring");
	require(__atomic_load_n(rq_head(z), __ATOMIC_ACQUIRE) == *rq_tail(z),
		"the flush consumes the entries");
}

/*
 * 7. RECV_ZC on an instance without a device copies every byte into a page of
 * the area. The completions name the pages, the stats and the event count the
 * copies, and a slot that the process returns serves the next copy.
 */
static void test_recv_copy(struct ring *r)
{
	const size_t len1 = 5 * page_size + 100, len2 = 3 * page_size;
	uint64_t slots1, slots2, copies1, pos = 0;
	struct zcrx_stats *st;
	unsigned int n1;
	struct rx rx = {};
	struct zcrx z;
	int tcp[2];

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, AREA_PAGES);
	zcrx_want_events(&z, (1U << ZCRX_EVENT_COPY) | (1U << ZCRX_EVENT_ALLOC_FAIL));
	require(!zcrx_do_register(&z), "register for the receive test");
	z.map = map_area(r, z.area.mmap_offset, z.area.len, PROT_READ | PROT_WRITE, MAP_SHARED);
	require(z.map != MAP_FAILED, "map the area for the receive test");
	st = zcrx_stats(&z);
	tcp_pair(tcp);
	rx_start(r, &rx, &z, tcp[1]);

	/* Round one: the completions name copies that hold the stream. */
	send_stream(tcp[0], pos, len1, 1);
	rx_wait(r, &rx, len1);
	require(rx.bytes == len1, "all the bytes arrive");
	slots1 = rx_check_copies(&z, &rx, 0, pos, 1);
	n1 = rx.nr_data;
	ksft_print_msg("round one: %u completions for %zu bytes, copy_count %lu\n",
		       n1, len1, (unsigned long)stat_read(&st->copy_count));
	/* A copy of a fragment can need several pages, so copy_count <= n1. */
	copies1 = stat_read(&st->copy_count);
	require(copies1 >= 1 && copies1 <= n1, "copy_count counts the copied fragments");
	require(stat_read(&st->copy_bytes) == len1, "copy_bytes counts the bytes");
	require(rx.nr_event[ZCRX_EVENT_COPY] == 1, "the copy event fires");
	require(!rx.nr_event[ZCRX_EVENT_ALLOC_FAIL], "no allocation failure");
	require(!stat_read(&st->exhaust_count), "the area did not run out");

	/* Return the slots. The next copies use them again. */
	rq_return(&z, &rx, 0);
	rq_flush(r, &z);
	pos += len1;
	send_stream(tcp[0], pos, len2, 2);
	rx_wait(r, &rx, len1 + len2);
	require(rx.bytes == len1 + len2, "all the bytes of round two arrive");
	slots2 = rx_check_copies(&z, &rx, n1, pos, 2);
	require(slots2 && !(slots2 & ~slots1), "the copies reuse the returned slots");
	require(stat_read(&st->copy_count) > copies1 &&
		stat_read(&st->copy_count) <= rx.nr_data, "copy_count grows");
	require(stat_read(&st->copy_bytes) == rx.bytes, "copy_bytes counts all bytes");
	/* An event fires once until the process arms it again. */
	require(rx.nr_event[ZCRX_EVENT_COPY] == 1, "the copy event fires once");

	rx_finish(r, &rx, tcp[0]);
	require(rx.final_res == 0, "the request ends at the end of the stream");
	close(tcp[1]);
	zcrx_close(&z);
	ksft_test_result_pass("RECV_ZC on MAIO|NODEV copies into the area and reuses slots\n");
}

static void arm_event(struct ring *r, struct zcrx *z, unsigned int type, int want)
{
	struct zcrx_ctrl ctrl = { .zcrx_id = z->id, .op = ZCRX_CTRL_ARM_EVENT };

	ctrl.zc_arm_event.event_type = type;
	require(zcrx_ctrl(r, &ctrl) == want, "arm an event");
}

/*
 * 8. The area runs out during a copy. The request gets the bytes that fit and
 * then -ENOMEM. The first copy that finds the area empty starts an episode.
 * It counts once, also if the request tries again, and it sends
 * ZCRX_EVENT_ALLOC_FAIL once until the process arms the event. A slot that
 * the process returns ends the episode, and the next receive works.
 */
static void test_recv_exhaust(struct ring *r)
{
	const unsigned int pages = 8;
	/* One page more than the area holds. */
	const size_t send_len = (pages + 1) * page_size;
	uint64_t sent = 0, recvd = 0;
	struct zcrx_stats *st;
	struct rx rx = {};
	struct zcrx z;
	int tcp[2], round;

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, pages);
	zcrx_want_events(&z, (1U << ZCRX_EVENT_ALLOC_FAIL) | (1U << ZCRX_EVENT_COPY));
	require(!zcrx_do_register(&z), "register a small area");
	z.map = map_area(r, z.area.mmap_offset, z.area.len, PROT_READ | PROT_WRITE, MAP_SHARED);
	require(z.map != MAP_FAILED, "map the small area");
	st = zcrx_stats(&z);
	tcp_pair(tcp);

	/* The event has not fired, so there is nothing to arm. */
	arm_event(r, &z, ZCRX_EVENT_ALLOC_FAIL, -EINVAL);

	for (round = 1; round <= 3; round++) {
		rx_start(r, &rx, &z, tcp[1]);
		send_stream(tcp[0], sent, send_len, 4);
		sent += send_len;
		rx_wait(r, &rx, UINT64_MAX);
		require(rx.done && rx.final_res == -ENOMEM,
			"the request ends with -ENOMEM when the area is empty");
		require(rx.nr_data >= 1 && rx.nr_data <= pages, "one completion per slot at most");
		require(rx.bytes < sent - recvd, "the socket keeps bytes that did not fit");
		rx_check_copies(&z, &rx, 0, recvd, 4);
		ksft_print_msg("round %d: %u completions, %lu bytes, then -ENOMEM\n",
			       round, rx.nr_data, (unsigned long)rx.bytes);
		recvd += rx.bytes;

		/* The request tried again with an empty area: still one episode. */
		require(stat_read(&st->exhaust_count) == (unsigned int)round,
			"each exhaustion counts once");
		/* The event fires once. It fires again after the process armed it. */
		require(rx.nr_event[ZCRX_EVENT_ALLOC_FAIL] == (round < 3 ? 1U : 2U),
			"the allocation failure event follows the latch");
		if (round == 2)
			arm_event(r, &z, ZCRX_EVENT_ALLOC_FAIL, 0);

		/* Return the slots. The next round starts with free slots. */
		rq_return(&z, &rx, 0);
		rq_flush(r, &z);
	}

	/* The bytes that did not fit arrive when the slots are back. */
	rx_start(r, &rx, &z, tcp[1]);
	rx_wait(r, &rx, sent - recvd);
	require(rx.bytes == sent - recvd, "the next receive gets the bytes that were left");
	rx_check_copies(&z, &rx, 0, recvd, 4);
	require(stat_read(&st->exhaust_count) == 3, "no new episode");
	require(!st->limbo_enter && !st->fallback_pages, "no limbo and no fallback pages");

	rx_finish(r, &rx, tcp[0]);
	require(rx.final_res == 0, "the request ends at the end of the stream");
	close(tcp[1]);
	zcrx_close(&z);
	ksft_test_result_pass("empty area: bytes that fit, -ENOMEM, one episode, one event\n");
}

/* The netdevsim fixture. */
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

static int set_channels(const char *name, unsigned int combined)
{
	struct ethtool_channels ch = { .cmd = ETHTOOL_GCHANNELS };
	struct ifreq ifr = {};
	int s = socket(AF_INET, SOCK_DGRAM, 0), ret;

	require(s >= 0, "control socket");
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
	ifr.ifr_data = (void *)&ch;
	ret = ioctl(s, SIOCETHTOOL, &ifr);
	if (!ret) {
		ch.cmd = ETHTOOL_SCHANNELS;
		ch.combined_count = combined;
		ret = ioctl(s, SIOCETHTOOL, &ifr);
	}
	close(s);
	return ret ? -errno : 0;
}

/* Poll until the provider is gone after a ring exits. */
static int register_retry(struct zcrx *z)
{
	int i, ret = 0;

	for (i = 0; i < 100; i++) {
		ret = zcrx_do_register(z);
		if (ret != -EEXIST)
			return ret;
		usleep(100 * 1000);
	}
	return ret;
}

/* 8. Install and remove the provider on every queue of a device. */
static void test_device(void)
{
	struct ring r, r2;
	struct zcrx z, z2;
	char name[IFNAMSIZ];
	unsigned int ifindex;
	int ret;

	if (!nsim_create(100, 4, name)) {
		ksft_test_result_skip("netdevsim is not available\n");
		return;
	}
	require(!link_up(name), "bring the netdevsim device up");
	ifindex = if_nametoindex(name);
	require(ifindex, "netdevsim ifindex");
	require(!set_channels(name, 3), "set 3 channels");

	require(!ring_init(&r, IORING_SETUP_CQE32), "ring for the device test");
	zcrx_prepare(&z, &r, ZCRX_REG_MAIO, ifindex, AREA_PAGES);
	ret = zcrx_do_register(&z);
	if (ret == -EOPNOTSUPP || ret == -ENODEV) {
		ksft_test_result_skip("the device has no queue API: %d\n", ret);
		ring_exit(&r);
		nsim_destroy(100);
		return;
	}
	require(!ret, "install the provider on a device");

	/* All three queues have the provider. */
	require(set_channels(name, 2) == -EINVAL, "fewer channels are refused");
	require(set_channels(name, 1) == -EINVAL, "one channel is refused");
	require(set_channels(name, 4) == -EINVAL, "more channels are refused");

	/* A second instance cannot take the queues. */
	require(!ring_init(&r2, IORING_SETUP_CQE32), "second ring");
	zcrx_prepare(&z2, &r2, ZCRX_REG_MAIO, ifindex, AREA_PAGES);
	require(zcrx_do_register(&z2) == -EEXIST, "a second instance is refused");
	ring_exit(&r2);

	/* The area is usable while the provider runs. */
	z.map = map_area(&r, z.area.mmap_offset, z.area.len, PROT_READ | PROT_WRITE, MAP_SHARED);
	require(z.map != MAP_FAILED, "map the area of a device instance");
	fill_pattern(z.map, z.area.len, 3);
	require(check_pattern(z.map, z.area.len, 3), "write the whole area");
	zcrx_close(&z);

	/* Closing the ring removes the provider from all queues. */
	ring_exit(&r);
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring after the removal");
	zcrx_prepare(&z, &r, ZCRX_REG_MAIO, ifindex, AREA_PAGES);
	require(!register_retry(&z), "install again after the removal");
	zcrx_close(&z);
	ring_exit(&r);

	/* After the removal, the channel count can change again. */
	for (ret = 0; ret < 100; ret++) {
		if (!set_channels(name, 2))
			break;
		usleep(100 * 1000);
	}
	require(ret < 100, "channel changes are allowed after the removal");
	nsim_destroy(100);
	ksft_test_result_pass("netdevsim: install on all queues, refuse changes, remove on exit\n");
}

/* The device goes away while the instance has the provider. */
static void test_device_removal(void)
{
	char name[IFNAMSIZ];
	unsigned int ifindex;
	struct ring r;
	struct zcrx z;

	if (!nsim_create(101, 2, name)) {
		ksft_test_result_skip("netdevsim is not available\n");
		return;
	}
	require(!link_up(name), "bring the second netdevsim device up");
	ifindex = if_nametoindex(name);
	require(ifindex, "second netdevsim ifindex");
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring for the removal test");
	zcrx_prepare(&z, &r, ZCRX_REG_MAIO, ifindex, AREA_PAGES);
	if (zcrx_do_register(&z)) {
		ksft_test_result_skip("the device has no queue API\n");
		ring_exit(&r);
		nsim_destroy(101);
		return;
	}
	z.map = map_area(&r, z.area.mmap_offset, z.area.len, PROT_READ | PROT_WRITE, MAP_SHARED);
	require(z.map != MAP_FAILED, "map the area before the removal");
	fill_pattern(z.map, z.area.len, 9);

	/* The core removes the provider from the queues as it unregisters the device. */
	nsim_destroy(101);
	require(!if_nametoindex(name), "the device is gone");

	/* The instance and its area outlive the device. */
	require(check_pattern(z.map, z.area.len, 9), "the area survives the device");
	zcrx_close(&z);
	ring_exit(&r);
	ksft_test_result_pass("netdevsim: the instance outlives the removal of the device\n");
}

/* 9. A device without the queue API cannot take the provider. */
static void test_loopback(struct ring *r)
{
	struct zcrx z;
	unsigned int lo = if_nametoindex("lo");
	int ret;

	require(lo, "loopback ifindex");
	zcrx_prepare(&z, r, ZCRX_REG_MAIO, lo, AREA_PAGES);
	ret = zcrx_do_register(&z);
	require(ret == -EOPNOTSUPP, "the loopback device has no queue API");
	zcrx_prepare(&z, r, ZCRX_REG_MAIO, 4000000, AREA_PAGES);
	require(zcrx_do_register(&z) == -ENODEV, "an unknown device is refused");
	ksft_test_result_pass("a device without the queue API is refused, the area is released\n");
}

/* 10. A large area exercises the chunk table and the mapping. */
static void test_large_area(struct ring *r)
{
	const unsigned int pages = 8192;
	struct zcrx z;
	size_t len;
	unsigned int i;

	zcrx_prepare(&z, r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, pages);
	len = z.area.len;
	require(!zcrx_do_register(&z), "register a large area");
	z.map = map_area(r, z.area.mmap_offset, len, PROT_READ | PROT_WRITE, MAP_SHARED);
	require(z.map != MAP_FAILED, "map a large area");
	for (i = 0; i < pages; i += 97)
		fill_pattern(z.map + (size_t)i * page_size, page_size, i);
	for (i = 0; i < pages; i += 97)
		require(check_pattern(z.map + (size_t)i * page_size, page_size, i),
			"read back a large area");
	zcrx_close(&z);
	ksft_test_result_pass("register and map a %zu MiB area\n", len >> 20);
}

/* 11. The locked memory limit applies to the area. */
static void test_memlock(void)
{
	pid_t pid = fork();
	int status;

	require(pid >= 0, "fork");
	if (!pid) {
		struct rlimit lim = { .rlim_cur = 1 << 20, .rlim_max = 1 << 20 };
		struct ring r;
		struct zcrx z;
		int ret;

		/*
		 * io_uring accounts locked memory only for a task without
		 * CAP_IPC_LOCK. The ring needs memory too, so set the limit
		 * after the setup.
		 */
		drop_cap(CAP_IPC_LOCK);
		if (ring_init(&r, IORING_SETUP_CQE32))
			_exit(2);
		if (setrlimit(RLIMIT_MEMLOCK, &lim))
			_exit(3);
		zcrx_prepare(&z, &r, ZCRX_REG_MAIO | ZCRX_REG_NODEV, 0, 2048);
		ret = zcrx_register_raw(&r, &z.reg);
		if (ret != -ENOMEM)
			_exit(100 + (-ret & 0x7f));

		/*
		 * A registration that fails after it allocated the area gives
		 * the accounting back. Three tries of 8 MiB must fit in 16 MiB.
		 */
		lim.rlim_cur = 16 << 20;
		lim.rlim_max = 16 << 20;
		if (setrlimit(RLIMIT_MEMLOCK, &lim))
			_exit(3);
		for (int i = 0; i < 3; i++) {
			zcrx_prepare(&z, &r, ZCRX_REG_MAIO, if_nametoindex("lo"), 2048);
			ret = zcrx_register_raw(&r, &z.reg);
			if (ret != -EOPNOTSUPP)
				_exit(110 + (-ret & 0x7f));
		}
		_exit(0);
	}
	require(waitpid(pid, &status, 0) == pid, "wait for the child");
	require(WIFEXITED(status) && !WEXITSTATUS(status),
		"an area above the locked memory limit fails with -ENOMEM");
	ksft_test_result_pass("the area counts against the memory limit, a failure releases it\n");
}

int main(void)
{
	struct rlimit lim = { RLIM_INFINITY, RLIM_INFINITY };
	struct io_uring_query_zcrx q = {};
	struct io_uring_query_hdr hdr = {
		.query_op = IO_URING_QUERY_ZCRX,
		.query_data = (uintptr_t)&q,
		.size = sizeof(q),
	};
	struct ring r;
	int ret;

	page_size = sysconf(_SC_PAGESIZE);
	ksft_print_header();
	setrlimit(RLIMIT_MEMLOCK, &lim);
	ret = ring_init(&r, IORING_SETUP_CQE32);
	if (ret)
		ksft_exit_skip("io_uring unavailable: %s\n", strerror(-ret));
	ret = syscall(__NR_io_uring_register, r.fd, IORING_REGISTER_QUERY, &hdr, 0);
	if (ret < 0 || hdr.result || !(q.register_flags & ZCRX_REG_MAIO) ||
	    !(q.area_flags & IORING_ZCRX_AREA_KERNEL))
		ksft_exit_skip("MAIO mode is unavailable\n");
	ksft_set_plan(PLAN);

	test_register_map(&r);
	test_two_instances(&r);
	test_flag_validation(&r);
	test_refill_ring(&r);
	test_add_area(&r);
	test_capability();
	test_recv_copy(&r);
	test_recv_exhaust(&r);
	test_loopback(&r);
	test_large_area(&r);
	test_memlock();
	test_device();
	test_device_removal();
	/* The last test closes the first ring. */
	test_export_import(&r);

	ksft_finished();
}
