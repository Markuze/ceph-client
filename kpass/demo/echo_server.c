/*
 * Zero-Overhead Echo Demo Server
 *
 * Receives data on one connection, forwards to another using the same buffer.
 * Demonstrates zero-overhead forwarding: userspace never accesses actual data,
 * only handles (buffer IDs) and metadata (offset, length).
 *
 * Usage: ./echo_server <listen_port> <forward_host> <forward_port>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>

#include "ceph_kpass.h"

#define NUM_BUFFERS     16
#define RING_DEPTH      32
#define MAX_EVENTS      16
#define AUTO_PEEK_SIZE  64   /* First 64 bytes shown in recv events */

static volatile bool running = true;

static void sighandler(int sig) { (void)sig; running = false; }

struct connection {
    kpass_sock_id   sock;
    bool            connected;
    const char      *name;
};

struct pending_io {
    kpass_buf_id    buf;
    uint32_t        len;
    bool            recv_pending;
    bool            send_pending;
};

#define MAX_PENDING 16
static struct pending_io pending[MAX_PENDING];
static int num_pending = 0;

static struct pending_io *find_pending_by_buf(kpass_buf_id buf) {
    for (int i = 0; i < num_pending; i++)
        if (pending[i].buf == buf) return &pending[i];
    return NULL;
}

static struct pending_io *alloc_pending(kpass_buf_id buf) {
    if (num_pending >= MAX_PENDING) return NULL;
    struct pending_io *p = &pending[num_pending++];
    p->buf = buf; p->len = 0; p->recv_pending = false; p->send_pending = false;
    return p;
}

static void free_pending(struct pending_io *p) {
    int idx = p - pending;
    if (idx >= 0 && idx < num_pending) pending[idx] = pending[--num_pending];
}

static int post_recv(struct ceph_kpass_ctx *ctx, struct connection *conn, kpass_buf_id buf) {
    struct pending_io *p = alloc_pending(buf);
    if (!p) { fprintf(stderr, "No pending slots\n"); return -1; }
    if (ceph_kpass_recv(ctx, conn->sock, buf, 0, ceph_kpass_buf_size(ctx), buf) < 0) {
        fprintf(stderr, "recv failed: %s\n", strerror(errno));
        free_pending(p); return -1;
    }
    p->recv_pending = true;
    printf("[%s] Posted recv on buffer %u\n", conn->name, buf);
    return 0;
}

static int forward_data(struct ceph_kpass_ctx *ctx, struct connection *out, struct pending_io *p) {
    /*
     * ZERO-OVERHEAD FORWARDING:
     * We pass the same buffer HANDLE to the send operation.
     * We never access the actual data - it stays in the kernel.
     */
    if (ceph_kpass_send(ctx, out->sock, p->buf, 0, p->len, p->buf) < 0) {
        fprintf(stderr, "send failed: %s\n", strerror(errno));
        return -1;
    }
    p->send_pending = true;
    printf("[%s] Forwarding %u bytes from buffer %u (zero-overhead)\n", out->name, p->len, p->buf);
    return 0;
}

static int run_echo_loop(struct ceph_kpass_ctx *ctx, struct connection *in, struct connection *out) {
    struct kpass_event events[MAX_EVENTS];

    /* Allocate buffer handles and post receives */
    for (int i = 0; i < 4; i++) {
        kpass_buf_id buf = ceph_kpass_buf_alloc(ctx);
        if (buf == KPASS_BUF_INVALID || post_recv(ctx, in, buf) < 0) return -1;
    }
    ceph_kpass_flush(ctx);

    printf("\n=== Zero-Overhead Echo Server Running ===\n");
    printf("Receiving on %s, forwarding to %s\n", in->name, out->name);
    printf("Data stays in kernel - userspace sees only handles\n\n");

    while (running) {
        int ret = ceph_kpass_poll(ctx, events, MAX_EVENTS, 1000);
        if (ret < 0 && errno != EINTR) { fprintf(stderr, "Poll error: %s\n", strerror(errno)); return -1; }

        for (int i = 0; i < ret; i++) {
            struct kpass_event *ev = &events[i];
            struct pending_io *p;

            switch (ev->type) {
            case KPASS_EV_RECV_DONE:
                p = find_pending_by_buf(ev->buf);
                if (!p) break;
                p->recv_pending = false;
                p->len = ev->recvd.bytes;

                printf("[%s] Received %u bytes into buffer %u\n",
                       in->name, ev->recvd.bytes, ev->buf);

                /*
                 * ZERO-OVERHEAD: We don't access the actual buffer data.
                 * For debugging, we use the auto-peek feature which provides
                 * a COPY of the first N bytes in the event structure.
                 */
                if (ev->recvd.peek_len > 0) {
                    /* Show first bytes from auto-peek (this is a COPY, not actual data) */
                    uint32_t show_len = ev->recvd.peek_len;
                    if (show_len > 64) show_len = 64;
                    printf("  Peek (%u bytes): \"", ev->recvd.peek_len);
                    fwrite(ev->recvd.peek, 1, show_len, stdout);
                    if (ev->recvd.peek_len > 64) printf("...");
                    printf("\"\n");
                } else {
                    printf("  (auto-peek disabled - data in kernel only)\n");
                }

                if (out->connected) {
                    forward_data(ctx, out, p);
                    ceph_kpass_flush(ctx);
                } else {
                    free_pending(p);
                    post_recv(ctx, in, ev->buf);
                    ceph_kpass_flush(ctx);
                }
                break;

            case KPASS_EV_SEND_DONE:
                p = find_pending_by_buf(ev->buf);
                if (!p) break;
                p->send_pending = false;
                printf("[%s] Sent %u bytes from buffer %u\n", out->name, ev->sent.bytes, ev->buf);
                free_pending(p);
                /* Reuse buffer handle for next receive */
                post_recv(ctx, in, ev->buf);
                ceph_kpass_flush(ctx);
                break;

            case KPASS_EV_CLOSED:
                printf("[sock %u] Connection closed\n", ev->sock);
                if (ev->sock == in->sock) return 0;
                if (ev->sock == out->sock) out->connected = false;
                break;

            case KPASS_EV_ERROR:
                fprintf(stderr, "[sock %u] Error: %s\n", ev->sock, strerror(ev->err.error));
                break;
            default: break;
            }
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <listen_port> <forward_host> <forward_port>\n", argv[0]);
        fprintf(stderr, "\nZero-Overhead Echo Server:\n");
        fprintf(stderr, "  Forwards data between two TCP connections\n");
        fprintf(stderr, "  Data stays in kernel - userspace handles only\n");
        return 1;
    }

    int listen_port = atoi(argv[1]), forward_port = atoi(argv[3]);
    const char *forward_host = argv[2];

    signal(SIGINT, sighandler); signal(SIGTERM, sighandler); signal(SIGPIPE, SIG_IGN);

    printf("=== Ceph Zero-Overhead Kernel Passthrough Demo ===\n\n");
    printf("Creating kpass context with %d buffers of %d KB each\n", NUM_BUFFERS, CEPH_KPASS_BUF_SIZE / 1024);

    struct ceph_kpass_ctx *ctx = ceph_kpass_create(NUM_BUFFERS, RING_DEPTH);
    if (!ctx) { fprintf(stderr, "Failed to create context: %s\n", strerror(errno)); return 1; }

    printf("Buffer pool: %u buffers x %zu bytes = %zu KB (kernel-only)\n",
           ceph_kpass_buf_count(ctx), ceph_kpass_buf_size(ctx),
           (ceph_kpass_buf_count(ctx) * ceph_kpass_buf_size(ctx)) / 1024);

    /* Enable auto-peek to see first bytes in recv events */
    printf("Enabling auto-peek: first %d bytes copied to recv events\n", AUTO_PEEK_SIZE);
    ceph_kpass_set_auto_peek(ctx, AUTO_PEEK_SIZE);

    struct connection in = { KPASS_SOCK_INVALID, false, "IN" };
    struct connection out = { KPASS_SOCK_INVALID, false, "OUT" };
    kpass_sock_id listen_sock = ceph_kpass_socket_create(ctx);
    if (listen_sock == KPASS_SOCK_INVALID || ceph_kpass_socket_listen(ctx, listen_sock, listen_port, 5) < 0) {
        fprintf(stderr, "Listen failed: %s\n", strerror(errno)); goto cleanup;
    }
    printf("Listening on port %d\n", listen_port);

    out.sock = ceph_kpass_socket_create(ctx);
    printf("Connecting to %s:%d...\n", forward_host, forward_port);
    if (ceph_kpass_socket_connect(ctx, out.sock, forward_host, forward_port, 0) < 0) {
        fprintf(stderr, "Connect failed: %s\n", strerror(errno)); goto cleanup;
    }
    ceph_kpass_socket_accept(ctx, listen_sock, 1);
    ceph_kpass_flush(ctx);

    printf("Waiting for connections...\n");
    struct kpass_event events[MAX_EVENTS];
    while (running && (!in.connected || !out.connected)) {
        int n = ceph_kpass_poll(ctx, events, MAX_EVENTS, 1000);
        for (int i = 0; i < n; i++) {
            if (events[i].type == KPASS_EV_ACCEPTED) {
                printf("Accepted inbound (sock %u)\n", events[i].accepted.new_sock);
                in.sock = events[i].accepted.new_sock; in.connected = true;
            } else if (events[i].type == KPASS_EV_CONNECTED) {
                printf("Outbound connected\n"); out.connected = true;
            } else if (events[i].type == KPASS_EV_ERROR) {
                fprintf(stderr, "Error: %s\n", strerror(events[i].err.error)); goto cleanup;
            }
        }
    }

    if (running) run_echo_loop(ctx, &in, &out);

cleanup:
    printf("\nCleaning up...\n");
    if (in.sock != KPASS_SOCK_INVALID) ceph_kpass_socket_close(ctx, in.sock);
    if (out.sock != KPASS_SOCK_INVALID) ceph_kpass_socket_close(ctx, out.sock);
    if (listen_sock != KPASS_SOCK_INVALID) ceph_kpass_socket_close(ctx, listen_sock);
    ceph_kpass_destroy(ctx);
    printf("Done.\n");
    return 0;
}
