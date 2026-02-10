/*
 * Ceph Zero-Overhead Kernel Passthrough (kpass) - Userspace Library API
 *
 * Zero-Overhead Architecture:
 * - Data stays in kernel, never mapped to userspace (no mmap)
 * - Userspace operates on handles (buffer IDs, socket IDs) only
 * - Optional peek API for explicit data copy when needed
 */

#ifndef CEPH_KPASS_LIB_H
#define CEPH_KPASS_LIB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Constants
 */
#define CEPH_KPASS_MAX_SOCKETS   64
#define CEPH_KPASS_MAX_BUFFERS   4096
#define CEPH_KPASS_BUF_SIZE      (128 * 1024)  /* 128KB default */
#define KPASS_AUTO_PEEK_MAX      256           /* Max auto-peek bytes */

/*
 * Opaque context handle
 */
struct ceph_kpass_ctx;

/*
 * Handle types - userspace operates on handles only, not data pointers
 */
typedef uint32_t kpass_buf_id;
typedef uint16_t kpass_sock_id;

#define KPASS_BUF_INVALID  ((kpass_buf_id)-1)
#define KPASS_SOCK_INVALID ((kpass_sock_id)-1)

/*
 * Event types
 */
enum kpass_event_type {
    KPASS_EV_NONE = 0,
    KPASS_EV_CONNECTED,
    KPASS_EV_ACCEPTED,
    KPASS_EV_SEND_DONE,
    KPASS_EV_RECV_DONE,
    KPASS_EV_ERROR,
    KPASS_EV_CLOSED,
};

/*
 * Event structure with auto-peek support
 *
 * In zero-overhead mode, userspace cannot access buffer data directly.
 * The recvd.peek[] field provides a COPY of the first N bytes when
 * auto-peek is enabled.
 */
struct kpass_event {
    enum kpass_event_type   type;
    kpass_sock_id           sock;       /* Socket handle */
    kpass_buf_id            buf;        /* Buffer handle */
    uint64_t                tag;        /* User correlation tag */

    union {
        struct {
            uint32_t bytes;             /* Total bytes sent */
        } sent;

        struct {
            uint32_t bytes;             /* Total bytes received */
            uint16_t peek_len;          /* Bytes in peek[] (0 if disabled) */
            uint8_t  peek[KPASS_AUTO_PEEK_MAX]; /* First N bytes (COPY) */
        } recvd;

        struct {
            kpass_sock_id new_sock;     /* Newly accepted socket handle */
        } accepted;

        struct {
            int error;                  /* errno value */
        } err;
    };
};

/*
 * Lifecycle
 */
struct ceph_kpass_ctx *ceph_kpass_create(uint32_t num_buffers, uint32_t ring_depth);
void ceph_kpass_destroy(struct ceph_kpass_ctx *ctx);
int ceph_kpass_fd(struct ceph_kpass_ctx *ctx);

/*
 * Socket Management (handle-based)
 */
kpass_sock_id ceph_kpass_socket_create(struct ceph_kpass_ctx *ctx);
int ceph_kpass_socket_listen(struct ceph_kpass_ctx *ctx, kpass_sock_id sock,
                            uint16_t port, int backlog);
int ceph_kpass_socket_accept(struct ceph_kpass_ctx *ctx, kpass_sock_id sock,
                            uint64_t tag);
int ceph_kpass_socket_connect(struct ceph_kpass_ctx *ctx, kpass_sock_id sock,
                             const char *host, uint16_t port, uint64_t tag);
void ceph_kpass_socket_close(struct ceph_kpass_ctx *ctx, kpass_sock_id sock);

/*
 * Buffer Management (handle-based, NO data access)
 *
 * REMOVED: ceph_kpass_buf_ptr() - No userspace data access in zero-overhead mode
 * Use ceph_kpass_peek() for explicit data copy when needed.
 */
kpass_buf_id ceph_kpass_buf_alloc(struct ceph_kpass_ctx *ctx);
void ceph_kpass_buf_free(struct ceph_kpass_ctx *ctx, kpass_buf_id buf);
size_t ceph_kpass_buf_size(struct ceph_kpass_ctx *ctx);
uint32_t ceph_kpass_buf_count(struct ceph_kpass_ctx *ctx);

/*
 * Peek/Poke API - Explicit data copy between kernel buffer and userspace
 *
 * These are the ONLY ways to access buffer data in zero-overhead mode.
 * The data is COPIED - these are replicas, not the actual buffer.
 *
 * For forwarding use cases, peek/poke are not needed - just use handles.
 * For data origination/inspection, use peek (read) and poke (write).
 */

/* Copy data FROM kernel buffer TO userspace destination */
int ceph_kpass_peek(struct ceph_kpass_ctx *ctx, kpass_buf_id buf,
                   uint32_t offset, uint32_t length, void *dest);

/* Copy data FROM userspace source TO kernel buffer */
int ceph_kpass_poke(struct ceph_kpass_ctx *ctx, kpass_buf_id buf,
                   uint32_t offset, uint32_t length, const void *src);

/* Configure auto-peek: first N bytes copied into recv events (0 to disable) */
int ceph_kpass_set_auto_peek(struct ceph_kpass_ctx *ctx, uint32_t size);

/* Get current auto-peek configuration */
uint32_t ceph_kpass_get_auto_peek(struct ceph_kpass_ctx *ctx);

/*
 * Network I/O (Async, handle-based)
 */
int ceph_kpass_send(struct ceph_kpass_ctx *ctx, kpass_sock_id sock,
                   kpass_buf_id buf, uint32_t offset, uint32_t len, uint64_t tag);
int ceph_kpass_recv(struct ceph_kpass_ctx *ctx, kpass_sock_id sock,
                   kpass_buf_id buf, uint32_t offset, uint32_t max_len, uint64_t tag);
int ceph_kpass_flush(struct ceph_kpass_ctx *ctx);

/*
 * Event Handling
 */
int ceph_kpass_poll(struct ceph_kpass_ctx *ctx, struct kpass_event *events,
                   int max_events, int timeout_ms);

/*
 * FUSE Integration
 */
struct io_uring *ceph_kpass_get_ring(struct ceph_kpass_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CEPH_KPASS_LIB_H */
