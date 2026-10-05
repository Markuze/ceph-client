/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef KPASS_OBJECTS_H
#define KPASS_OBJECTS_H

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <liburing.h>
#include "../include/uapi/ceph_kpass.h"

/*
 * Preparation only: the caller owns submission, CQEs and full 64-bit tags.
 * Open CEPH_KPASS_DEVICE and use SQE128 | CQE32. Do not issue legacy INIT.
 * Buffers supplied for reads must remain valid until their CQE arrives.
 */
static inline struct kpass_sqe_cmd *
kpass_prep_cmd(struct io_uring *ring, struct io_uring_sqe *sqe, int fd,
	       uint8_t op, uint16_t sock, uint64_t tag)
{
	struct kpass_sqe_cmd *cmd;
	unsigned int required = IORING_SETUP_SQE128 | IORING_SETUP_CQE32;

	if ((ring->flags & required) != required) {
		errno = EINVAL;
		return NULL;
	}
	io_uring_prep_uring_cmd(sqe, 0, fd);
	cmd = (struct kpass_sqe_cmd *)sqe->cmd;
	memset(cmd, 0, sizeof(*cmd));
	cmd->op = op;
	cmd->sock_id = sock;
	cmd->tag = tag;
	sqe->user_data = tag;
	return cmd;
}

/* READ_STREAM / KEEP / DISCARD / OBJECT_READ / OBJECT_SEND. */
static inline int
kpass_prep_range(struct io_uring *ring, struct io_uring_sqe *sqe, int fd,
		 uint8_t op, uint16_t sock, uint64_t object, uint64_t offset,
		 uint64_t length, void *buffer, uint64_t tag)
{
	struct kpass_sqe_cmd *cmd = kpass_prep_cmd(ring, sqe, fd, op, sock, tag);

	if (!cmd)
		return -EINVAL;
	cmd->stream.object = object;
	cmd->stream.offset = offset;
	cmd->stream.length = length;
	cmd->stream.addr = (uintptr_t)buffer;
	return 0;
}

static inline uint64_t kpass_cqe_object(const struct io_uring_cqe *cqe)
{
	return cqe->res < 0 ? KPASS_OBJECT_INVALID : cqe->big_cqe[0];
}

/* Compact the entire object asynchronously, preserving its existing handle. */
static inline int
kpass_prep_compact(struct io_uring *ring, struct io_uring_sqe *sqe, int fd,
		   uint64_t object, uint64_t tag)
{
	return kpass_prep_range(ring, sqe, fd, KPASS_OP_OBJECT_COMPACT, 0,
				object, 0, 0, NULL, tag);
}

#endif /* KPASS_OBJECTS_H */
