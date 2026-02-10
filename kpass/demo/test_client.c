/*
 * Test Client for Zero-Overhead Echo Demo
 *
 * This client connects to the echo server and sends test data,
 * demonstrating the kpass library usage with zero-overhead API.
 *
 * Usage:
 *   ./test_client <host> <port> [message]
 *
 * Example:
 *   ./test_client 127.0.0.1 8888 "Hello, Zero-Overhead World!"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>

#include "ceph_kpass.h"

#define NUM_BUFFERS     8
#define RING_DEPTH      16
#define MAX_EVENTS      8
#define AUTO_PEEK_SIZE  256

static volatile bool running = true;

static void sighandler(int sig)
{
    (void)sig;
    running = false;
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <host> <port> [message]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Zero-Overhead Test Client:\n");
    fprintf(stderr, "  - Uses poke() to write data to kernel buffer\n");
    fprintf(stderr, "  - Uses peek/auto-peek to read received data\n");
    fprintf(stderr, "  - Data stays in kernel, userspace handles only\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  %s 127.0.0.1 8888 \"Hello, Zero-Overhead!\"\n", prog);
}

int main(int argc, char *argv[])
{
    struct ceph_kpass_ctx *ctx;
    struct kpass_event events[MAX_EVENTS];
    kpass_sock_id sock = KPASS_SOCK_INVALID;
    kpass_buf_id send_buf = KPASS_BUF_INVALID;
    kpass_buf_id recv_buf = KPASS_BUF_INVALID;
    const char *host;
    int port;
    const char *message;
    char default_msg[256];
    int ret = 1;
    bool connected = false;
    bool send_done = false;

    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    host = argv[1];
    port = atoi(argv[2]);

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number\n");
        return 1;
    }

    if (argc >= 4) {
        message = argv[3];
    } else {
        time_t now = time(NULL);
        snprintf(default_msg, sizeof(default_msg),
                 "Test message from test_client at %s", ctime(&now));
        /* Remove newline from ctime */
        char *nl = strchr(default_msg, '\n');
        if (nl) *nl = '\0';
        message = default_msg;
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGPIPE, SIG_IGN);

    printf("=== Ceph Zero-Overhead Kernel Passthrough Test Client ===\n\n");
    printf("Creating kpass context...\n");

    ctx = ceph_kpass_create(NUM_BUFFERS, RING_DEPTH);
    if (!ctx) {
        fprintf(stderr, "Failed to create kpass context: %s\n", strerror(errno));
        return 1;
    }

    printf("Buffer pool: %u buffers x %zu bytes (kernel-only)\n",
           ceph_kpass_buf_count(ctx), ceph_kpass_buf_size(ctx));

    /* Enable auto-peek to see received data */
    printf("Enabling auto-peek: first %d bytes in recv events\n", AUTO_PEEK_SIZE);
    ceph_kpass_set_auto_peek(ctx, AUTO_PEEK_SIZE);

    /* Allocate buffer handles (not data!) */
    send_buf = ceph_kpass_buf_alloc(ctx);
    recv_buf = ceph_kpass_buf_alloc(ctx);

    if (send_buf == KPASS_BUF_INVALID || recv_buf == KPASS_BUF_INVALID) {
        fprintf(stderr, "Failed to allocate buffers\n");
        goto cleanup;
    }
    printf("Allocated buffer handles: send=%u, recv=%u\n", send_buf, recv_buf);

    /* Create socket */
    sock = ceph_kpass_socket_create(ctx);
    if (sock == KPASS_SOCK_INVALID) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        goto cleanup;
    }

    printf("Connecting to %s:%d...\n", host, port);

    /* Initiate connection */
    if (ceph_kpass_socket_connect(ctx, sock, host, port, 0) < 0) {
        fprintf(stderr, "Failed to connect: %s\n", strerror(errno));
        goto cleanup;
    }

    ceph_kpass_flush(ctx);

    /* Wait for connection */
    while (running && !connected) {
        int n = ceph_kpass_poll(ctx, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "Poll error: %s\n", strerror(errno));
            goto cleanup;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].type == KPASS_EV_CONNECTED) {
                printf("Connected!\n");
                connected = true;
            } else if (events[i].type == KPASS_EV_ERROR) {
                fprintf(stderr, "Connection failed: %s\n",
                        strerror(events[i].err.error));
                goto cleanup;
            }
        }
    }

    if (!connected)
        goto cleanup;

    /*
     * ZERO-OVERHEAD: Write message to kernel buffer via poke()
     * We don't have direct access to the buffer - poke copies our data in.
     */
    size_t msg_len = strlen(message);
    if (msg_len > ceph_kpass_buf_size(ctx)) {
        msg_len = ceph_kpass_buf_size(ctx);
    }

    printf("\nWriting message to buffer %u via poke()...\n", send_buf);
    int poked = ceph_kpass_poke(ctx, send_buf, 0, msg_len, message);
    if (poked < 0) {
        fprintf(stderr, "Failed to poke data into buffer: %s\n", strerror(errno));
        fprintf(stderr, "(poke requires kernel module - may be running in fallback mode)\n");
        goto cleanup;
    }

    printf("Sending message (%d bytes written to kernel buffer):\n  \"%s\"\n\n",
           poked, message);

    /* Send the message using buffer handle */
    if (ceph_kpass_send(ctx, sock, send_buf, 0, poked, 1) < 0) {
        fprintf(stderr, "Failed to send: %s\n", strerror(errno));
        goto cleanup;
    }

    /* Post receive for any response (echo back) */
    if (ceph_kpass_recv(ctx, sock, recv_buf, 0, ceph_kpass_buf_size(ctx), 2) < 0) {
        fprintf(stderr, "Failed to post recv: %s\n", strerror(errno));
        goto cleanup;
    }

    ceph_kpass_flush(ctx);

    /* Wait for send completion and optional response */
    int timeout_count = 0;
    while (running && timeout_count < 5) {
        int n = ceph_kpass_poll(ctx, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "Poll error: %s\n", strerror(errno));
            goto cleanup;
        }

        if (n == 0) {
            timeout_count++;
            if (!send_done) {
                printf("Waiting for send completion...\n");
            } else {
                printf("Waiting for response... (%d)\n", timeout_count);
            }
            continue;
        }

        timeout_count = 0;

        for (int i = 0; i < n; i++) {
            struct kpass_event *ev = &events[i];

            switch (ev->type) {
            case KPASS_EV_SEND_DONE:
                printf("Send completed: %u bytes sent from buffer %u\n",
                       ev->sent.bytes, ev->buf);
                send_done = true;
                break;

            case KPASS_EV_RECV_DONE:
                printf("\nReceived response (%u bytes into buffer %u):\n",
                       ev->recvd.bytes, ev->buf);

                /*
                 * ZERO-OVERHEAD: Use auto-peek data from the event
                 * We don't access the actual buffer - just the peek copy
                 */
                if (ev->recvd.peek_len > 0) {
                    printf("  Peek (%u bytes): \"", ev->recvd.peek_len);
                    fwrite(ev->recvd.peek, 1, ev->recvd.peek_len, stdout);
                    printf("\"\n\n");
                } else {
                    printf("  (auto-peek disabled - data in kernel only)\n\n");
                }

                ret = 0;
                goto cleanup;

            case KPASS_EV_CLOSED:
                printf("Connection closed by peer\n");
                if (send_done)
                    ret = 0;  /* Success if we managed to send */
                goto cleanup;

            case KPASS_EV_ERROR:
                fprintf(stderr, "Error: %s\n", strerror(ev->err.error));
                goto cleanup;

            default:
                break;
            }
        }
    }

    if (send_done) {
        printf("Message sent successfully (no response received)\n");
        ret = 0;
    }

cleanup:
    printf("\nCleaning up...\n");

    if (send_buf != KPASS_BUF_INVALID)
        ceph_kpass_buf_free(ctx, send_buf);
    if (recv_buf != KPASS_BUF_INVALID)
        ceph_kpass_buf_free(ctx, recv_buf);
    if (sock != KPASS_SOCK_INVALID)
        ceph_kpass_socket_close(ctx, sock);

    ceph_kpass_destroy(ctx);

    printf("Done.\n");
    return ret;
}
