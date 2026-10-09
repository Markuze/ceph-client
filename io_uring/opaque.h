/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IO_URING_OPAQUE_H
#define IO_URING_OPAQUE_H

#include <linux/io_uring_types.h>

struct io_opaque_store;
struct io_cancel_data;

#ifdef CONFIG_IO_URING_OPAQUE_OBJ
struct io_opaque_store *io_opaque_alloc(struct io_ring_ctx *ctx, u64 config);
void io_opaque_stop(struct io_opaque_store *store);
void io_opaque_free(struct io_opaque_store *store);
int io_opaque_recv_prep(struct io_kiocb *req, struct io_opaque_store *store);
int io_opaque_recv(struct io_kiocb *req, unsigned int issue_flags);
int io_opaque_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_opaque_issue(struct io_kiocb *req, unsigned int issue_flags);
int io_opaque_send_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_opaque_send(struct io_kiocb *req, unsigned int issue_flags);
void io_opaque_cleanup(struct io_kiocb *req);
void io_opaque_fail(struct io_kiocb *req);
int io_opaque_cancel(struct io_ring_ctx *ctx, struct io_cancel_data *cd,
		     unsigned int issue_flags);
bool io_opaque_cancel_all(struct io_ring_ctx *ctx, struct io_uring_task *tctx,
			  bool cancel_all);
#else
static inline struct io_opaque_store *io_opaque_alloc(struct io_ring_ctx *ctx,
						      u64 config)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline void io_opaque_stop(struct io_opaque_store *store)
{
}

static inline void io_opaque_free(struct io_opaque_store *store)
{
}

static inline int io_opaque_recv_prep(struct io_kiocb *req,
				      struct io_opaque_store *store)
{
	return -EOPNOTSUPP;
}

static inline int io_opaque_recv(struct io_kiocb *req, unsigned int issue_flags)
{
	return -EOPNOTSUPP;
}

static inline void io_opaque_cleanup(struct io_kiocb *req)
{
}

static inline int io_opaque_cancel(struct io_ring_ctx *ctx,
				   struct io_cancel_data *cd,
				   unsigned int issue_flags)
{
	return -ENOENT;
}

static inline bool io_opaque_cancel_all(struct io_ring_ctx *ctx,
					struct io_uring_task *tctx, bool cancel_all)
{
	return false;
}
#endif
#endif
