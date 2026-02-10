/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Ceph Zero-Overhead Kernel Passthrough (kpass) - UAPI Header
 *
 * This header defines the interface between userspace and the kernel
 * ceph_kpass module. It is shared between kernel and userspace.
 *
 * Zero-Overhead Architecture:
 * - Data stays in kernel, never mapped to userspace (no mmap)
 * - Userspace operates on handles (buffer IDs, socket IDs) only
 * - Optional peek API for explicit data copy when needed
 */

#ifndef _UAPI_CEPH_KPASS_H
#define _UAPI_CEPH_KPASS_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <linux/types.h>
#include <sys/ioctl.h>
#endif

#define CEPH_KPASS_DEVICE        "/dev/ceph_kpass"
#define CEPH_KPASS_MAX_SOCKETS   64
#define CEPH_KPASS_MAX_BUFFERS   4096
#define CEPH_KPASS_BUF_SIZE      (128 * 1024)  /* 128KB default */
#define KPASS_AUTO_PEEK_MAX      256           /* Max auto-peek bytes */

/*
 * Command opcodes for io_uring_cmd
 */
enum kpass_op {
    /* Session lifecycle */
    KPASS_OP_INIT            = 0x01,
    KPASS_OP_SHUTDOWN        = 0x02,

    /* Socket management */
    KPASS_OP_SOCK_CREATE     = 0x10,
    KPASS_OP_SOCK_CONNECT    = 0x11,
    KPASS_OP_SOCK_CLOSE      = 0x12,
    KPASS_OP_SOCK_LISTEN     = 0x13,
    KPASS_OP_SOCK_ACCEPT     = 0x14,

    /* Buffer management */
    KPASS_OP_BUF_ALLOC       = 0x20,
    KPASS_OP_BUF_FREE        = 0x21,

    /* Network I/O */
    KPASS_OP_SEND            = 0x30,
    KPASS_OP_RECV            = 0x31,

    /* Async notifications */
    KPASS_OP_NOTIFY_CONNECTED = 0x40,
    KPASS_OP_NOTIFY_SENT      = 0x41,
    KPASS_OP_NOTIFY_RECVD     = 0x42,
    KPASS_OP_NOTIFY_ERROR     = 0x43,
    KPASS_OP_NOTIFY_CLOSED    = 0x44,
    KPASS_OP_NOTIFY_ACCEPTED  = 0x45,
};

/*
 * SQE command payload for io_uring_cmd (shared with kernel).
 * Must fit into io_uring SQE cmd area (64 bytes).
 */
#ifdef __KERNEL__
#define KPASS_PACKED __packed
#else
#define KPASS_PACKED __attribute__((packed))
#endif

struct kpass_sqe_cmd {
    __u8    op;
    __u8    flags;
    __u16   sock_id;
    __u32   buf_id;
    __u64   tag;

    union {
        struct {
            __u32 num_buffers;
            __u32 buf_size;
        } init;

        struct {
            __u16 family;
            __u16 port;
            __u8  addr[16];
        } connect;

        struct {
            __u16 family;
            __u16 port;
            __u32 backlog;
        } listen;

        struct {
            __u32 offset;
            __u32 len;
            __u32 flags;
        } send;

        struct {
            __u32 offset;
            __u32 len;
        } recv;

        __u8 _pad[48];
    };
} KPASS_PACKED;

#undef KPASS_PACKED

/*
 * IOCTLs
 */
#define KPASS_IOC_MAGIC  'K'

/* Device info */
struct kpass_info {
    __u32   version;
    __u32   max_sockets;
    __u32   max_buffers;
    __u32   default_buf_size;
};

/*
 * Peek: Copy data FROM kernel buffer TO userspace
 * Poke: Copy data FROM userspace TO kernel buffer
 * These are the ONLY ways to access buffer data in zero-overhead mode
 */
struct kpass_peek_args {
    __u32   buf_id;         /* Buffer handle */
    __u32   offset;         /* Offset within buffer */
    __u32   length;         /* Max bytes to copy */
    __u32   bytes_copied;   /* Output: actual bytes copied */
    __u64   dest_addr;      /* Userspace address (source for poke, dest for peek) */
};

/* Poke uses same structure as peek */
#define kpass_poke_args kpass_peek_args

/* Auto-peek configuration */
struct kpass_auto_peek_args {
    __u32   size;           /* 0 to disable, max KPASS_AUTO_PEEK_MAX */
    __u32   _reserved;
};

/* Buffer allocation (for ioctl) */
struct kpass_buf_alloc_args {
    __s32   buf_id;         /* Output: allocated buffer ID, or -1 on error */
    __u32   _reserved;
};

/* IOCTL commands */
#define KPASS_IOC_GET_INFO       _IOR(KPASS_IOC_MAGIC, 1, struct kpass_info)
/* REMOVED: KPASS_IOC_GET_MMAP - no mmap in zero-overhead mode */
#define KPASS_IOC_BUF_ALLOC      _IOR(KPASS_IOC_MAGIC, 3, struct kpass_buf_alloc_args)
#define KPASS_IOC_BUF_FREE       _IOW(KPASS_IOC_MAGIC, 4, __u32)
#define KPASS_IOC_PEEK           _IOWR(KPASS_IOC_MAGIC, 5, struct kpass_peek_args)
#define KPASS_IOC_SET_AUTO_PEEK  _IOW(KPASS_IOC_MAGIC, 6, struct kpass_auto_peek_args)
#define KPASS_IOC_GET_AUTO_PEEK  _IOR(KPASS_IOC_MAGIC, 7, struct kpass_auto_peek_args)
#define KPASS_IOC_POKE           _IOWR(KPASS_IOC_MAGIC, 8, struct kpass_poke_args)

#define KPASS_FLAG_MORE      (1 << 0)

#endif /* _UAPI_CEPH_KPASS_H */
