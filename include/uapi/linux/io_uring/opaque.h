/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT */
#ifndef LINUX_IO_URING_OPAQUE_H
#define LINUX_IO_URING_OPAQUE_H

#include <linux/types.h>

/* IORING_REGISTER_OPAQUE_STORE, nr_args == 1. size includes reserved bytes. */
struct io_uring_opaque_register {
	__u32 size;
	__u32 flags;
	__s32 fd;
	__u32 store_id;
	__u64 config;
	__u32 config_size;
	__u32 __resv0;
	__u64 __resv[2];
};

#define IORING_OPAQUE_REG_IMPORT	(1U << 0)
#define IORING_OPAQUE_REG_EXPORT	(1U << 1)

/* config/config_size select a size-versioned configuration on creation. */
struct io_uring_opaque_config {
	__u64 hard_limit;
	__u64 compact_headroom;
	__u32 max_object_size;
	__u32 max_objects;
	__u32 max_streams;
	__u32 max_extents;
	__u32 max_requests;
	__u32 flags;			/* IORING_OPAQUE_CFG_* */
	__u64 __resv[2];
	__u64 high_watermark;
	__u64 low_watermark;
	__u64 reply_reserve;
	/*
	 * The zcrx id of the MAIO instance to bind, in the registering ring.
	 * Valid only with IORING_OPAQUE_CFG_MAIO. Must be 0 without it.
	 */
	__u32 zcrx_id;
	__u32 __resv1;
};

/*
 * Bind the store to a MAIO zcrx instance (ZCRX_REG_MAIO) of the registering
 * ring. @zcrx_id names the instance. The store keeps every byte that it
 * captures from a stream in the area of the instance. The process that maps
 * the area can read and write the bytes of such an object in place. A send
 * transmits the bytes that are present when TCP builds the segment. A byte
 * that is not in the area is copied into it. When the area has no free page,
 * the copy waits and retries, as it does when a page allocation fails.
 * IORING_OPAQUE_CREATE is the exception. It copies the bytes of the process
 * into pages outside the area, and the object is not
 * IORING_OPAQUE_OBJECT_F_MAPPED.
 *
 * The store holds a reference to the instance until the store is gone. The
 * instance can go first, for example when its ring exits. The objects keep
 * their pages, and the store keeps taking free pages of the area. Another ring
 * can import the store. The store stays bound to the instance of the ring that
 * created it.
 *
 * Registration fails with -ENXIO if the ring has no MAIO instance with this
 * id, and with -EOPNOTSUPP if the kernel has no zero-copy receive.
 */
#define IORING_OPAQUE_CFG_MAIO		(1U << 0)

/* IORING_OP_OPAQUE_OBJ: sqe->ioprio selects the operation. */
enum io_uring_opaque_op {
	/* Nonconsuming available window; len is capacity, never WAITALL. */
	IORING_OPAQUE_INSPECT,
	/* Complete-only object-ready CQE for the declared future stream range. */
	IORING_OPAQUE_RECV_OBJECT,
	IORING_OPAQUE_DISCARD,
	/* Copy from an already complete immutable object. */
	IORING_OPAQUE_READ_OBJECT,
	IORING_OPAQUE_FREE,
	IORING_OPAQUE_STAT,
	IORING_OPAQUE_OBJECT_STAT,
	IORING_OPAQUE_STREAM_STAT,
	IORING_OPAQUE_STREAM_CLOSE,
	IORING_OPAQUE_COMPACT,
	IORING_OPAQUE_SET_POLICY,
	/* Copy userspace bytes into a new complete immutable object. */
	IORING_OPAQUE_CREATE,
	/* Nonconsuming contiguous byte count, without copying payload. */
	IORING_OPAQUE_AVAILABLE,
	/*
	 * List where the bytes of an object are in the area of the MAIO
	 * instance that the store binds. addr is the handle and off is 0. The
	 * buffer at addr3 has len bytes. The kernel writes one
	 * struct io_uring_opaque_extent for each extent of the object, in object
	 * order, and the result is the number of bytes written. If the buffer is
	 * too small, the result is -EOVERFLOW, nothing is written, and the first
	 * extra word of the completion holds the size that is needed. A buffer
	 * with len 0 and addr3 0 asks for that size. A store that binds no
	 * instance returns -EOPNOTSUPP. An extent that is not in the area, such
	 * as one of an object that IORING_OPAQUE_CREATE made, has area_off
	 * IORING_OPAQUE_EXTENT_UNMAPPED.
	 */
	IORING_OPAQUE_OBJECT_EXTENTS,
};

/*
 * One extent of an object in the area of a MAIO instance. The sum of the @len
 * of all extents is the length of the object.
 */
struct io_uring_opaque_extent {
	/*
	 * The zcrx offset of the first byte of the extent: the area id in the
	 * bits from IORING_ZCRX_AREA_SHIFT up, and the byte offset in the area
	 * below. It is the format of the offset in the completion of RECV_ZC.
	 * IORING_OPAQUE_EXTENT_UNMAPPED if the extent is not in the area.
	 */
	__u64 area_off;
	/* The number of bytes of the extent. They do not cross a page. */
	__u64 len;
};

/* @area_off of an extent that is not in the area, as all those of a created object. */
#define IORING_OPAQUE_EXTENT_UNMAPPED	(~0ULL)

/* IORING_OP_OPAQUE_OBJ_SEND: sqe->ioprio contains these flags. */
#define IORING_OPAQUE_SEND_LAST	(1U << 0)
/* addr points at len struct io_uring_opaque_send_entry, off is zero: one send, many ranges. */
#define IORING_OPAQUE_SEND_VECTOR	(1U << 1)
/* Entries of one vectored send, as many as SEND_ZC takes iovecs (UIO_MAXIOV). */
#define IORING_OPAQUE_SEND_VEC_MAX	1024

/* One range of a vectored send, named as addr, off and len name it in a single one. */
struct io_uring_opaque_send_entry {
	__u64 handle;
	__u64 off;
	__u32 len;
	__u32 __resv;
};

/* Optional SEND sqe->addr3 framing; size includes zero reserved extensions. */
struct io_uring_opaque_frame {
	__u32 size;
	__u32 flags;
	__u64 prefix;
	__u64 suffix;
	__u32 prefix_length;
	__u32 suffix_length;
	__u64 __resv[2];
};

/* Combined copied prefix and suffix, independently of object body length. */
#define IORING_OPAQUE_FRAME_MAX	(64U * 1024U)

#define IORING_OPAQUE_OBJECT_F_COMPACTED	(1U << 0)
#define IORING_OPAQUE_OBJECT_F_COMPACTING	(1U << 1)
#define IORING_OPAQUE_OBJECT_F_FORWARD_ONLY (1U << 2)
/*
 * Every extent of the object is in the area of the MAIO instance that the
 * store binds, so the process can see all its bytes through the mapping of the
 * area. Set for every object that a bound store captured, and never for an
 * object that IORING_OPAQUE_CREATE made. The flag is in the flags of
 * IORING_OPAQUE_OBJECT_STAT and in the second extra word of the completion of
 * IORING_OPAQUE_RECV_OBJECT, next to IORING_OPAQUE_OBJECT_F_FORWARD_ONLY.
 */
#define IORING_OPAQUE_OBJECT_F_MAPPED		(1U << 3)
#define IORING_OPAQUE_STREAM_F_EOF		(1U << 0)
#define IORING_OPAQUE_POLICY_F_AUTO_COMPACT	(1U << 0)

enum io_uring_opaque_memory_mode {
	IORING_OPAQUE_MEMORY_NORMAL,
	IORING_OPAQUE_MEMORY_LOWMEM,
};

struct io_uring_opaque_stat {
	__u32 size;
	__u32 __resv;
	__u64 backing_bytes;
	__u64 hard_limit;
	__u64 copied_bytes;
	__u64 compacted_bytes;
	__u64 compact_count;
	__u32 objects;
	__u32 streams;
	__u32 extents;
	__u32 requests;
	__u64 framing_bytes;
	__u64 framing_copied_bytes;
	__u32 memory_mode;
	__u32 __resv1;
};

struct io_uring_opaque_object_stat {
	__u32 size;
	__u32 flags;
	__u64 length;
	__u64 backing_bytes;
	__u32 extents;
	__u32 __resv;
};

struct io_uring_opaque_stream_stat {
	__u32 size;
	__u32 flags;
	__u64 rx_next;
	__u64 undecided_bytes;
	__s32 error;
	__u32 __resv;
};

struct io_uring_opaque_policy {
	__u32 flags;
	__u32 __resv0;
	__u64 bytes_per_second;
	__u64 burst_bytes;
	__u64 max_temporary_bytes;
	__u64 __resv[2];
};

#endif /* LINUX_IO_URING_OPAQUE_H */
