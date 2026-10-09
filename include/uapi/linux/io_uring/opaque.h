/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT */
#ifndef LINUX_IO_URING_OPAQUE_H
#define LINUX_IO_URING_OPAQUE_H

#include <linux/types.h>

/* ZCRX_REG_OPAQUE_OBJ: io_uring_zcrx_ifq_reg::opaque_config points here. */
struct io_uring_opaque_config {
	__u64 hard_limit;
	__u64 compact_headroom;
	__u32 max_object_size;
	__u32 max_objects;
	__u32 max_streams;
	__u32 max_extents;
	__u32 max_requests;
	__u32 flags;
	__u64 __resv[2];
};

/* IORING_OP_OPAQUE_OBJ: sqe->ioprio selects the operation. */
enum io_uring_opaque_op {
	IORING_OPAQUE_READ_STREAM,
	IORING_OPAQUE_KEEP,
	IORING_OPAQUE_DISCARD,
	IORING_OPAQUE_READ,
	IORING_OPAQUE_FREE,
	IORING_OPAQUE_STAT,
	IORING_OPAQUE_OBJECT_STAT,
	IORING_OPAQUE_STREAM_STAT,
	IORING_OPAQUE_STREAM_CLOSE,
	IORING_OPAQUE_COMPACT,
	IORING_OPAQUE_SET_POLICY,
};

/* IORING_OP_OPAQUE_OBJ_SEND: sqe->ioprio contains these flags. */
#define IORING_OPAQUE_SEND_LAST	(1U << 0)

#define IORING_OPAQUE_COMPACTED	(1U << 0)
#define IORING_OPAQUE_COMPACTING	(1U << 1)
#define IORING_OPAQUE_STREAM_EOF	(1U << 0)
#define IORING_OPAQUE_AUTO_COMPACT	(1U << 0)

struct io_uring_opaque_stat {
	__u64 backing_bytes;
	__u64 hard_limit;
	__u64 copied_bytes;
	__u64 compacted_bytes;
	__u64 compact_count;
	__u32 objects;
	__u32 streams;
	__u32 extents;
	__u32 requests;
};

struct io_uring_opaque_object_stat {
	__u64 length;
	__u64 backing_bytes;
	__u32 extents;
	__u32 flags;
};

struct io_uring_opaque_stream_stat {
	__u64 rx_next;
	__u64 undecided_bytes;
	__u32 flags;
	__s32 error;
};

struct io_uring_opaque_policy {
	__u32 flags;
	__u32 min_age_ms;
	__u64 min_saving;
	__u32 min_slack_percent;
	__u32 min_extents;
	__u64 bytes_per_second;
	__u64 burst_bytes;
	__u64 max_temporary_bytes;
	__u64 __resv[2];
};

#endif /* LINUX_IO_URING_OPAQUE_H */
