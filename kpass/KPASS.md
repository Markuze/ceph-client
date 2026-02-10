# Ceph Zero-Copy Kernel Passthrough (kpass)

## Overview

kpass is a Linux kernel module (`ceph_kpass.ko`) and companion userspace library
(`libceph_kpass`) that provides zero-copy network I/O for TCP forwarding
workloads. Userspace operates entirely on **handles** (buffer IDs, socket IDs)
and **metadata** (offset, length, byte count). Data never crosses the
kernel-user boundary except via the optional peek API.

### Architecture

```
NIC DMA --> skb frag pages
                | tcp_read_sock actor
          get_page() --> kpass_buf.sgvec[]   (0 copies)
                | userspace: forward handle
          sock_sendmsg(MSG_SPLICE_PAGES)    (0 copies)
                |
          TX skb --> NIC DMA

Total kernel copies: 0
```

For data origination (poke + send), one unavoidable `copy_from_user` transfers
data into the pool. TX still uses `MSG_SPLICE_PAGES` for zero-copy to the NIC.

### Components

| Component | Path | Description |
|-----------|------|-------------|
| Kernel module | `kernel/ceph_kpass.c` | `/dev/ceph_kpass` misc device |
| Internal header | `kernel/ceph_kpass_internal.h` | Buffer, socket, session structs |
| UAPI header | `include/uapi/ceph_kpass.h` | Shared kernel/user constants and command structs |
| Userspace library | `lib/ceph_kpass.c` | io_uring-based async API |
| Library header | `lib/ceph_kpass.h` | Public API |
| Echo server demo | `demo/echo_server.c` | Two-socket forwarding demo |
| Test client demo | `demo/test_client.c` | Poke + send + recv round-trip test |

---

## Zero-Copy Data Flow

### Forwarding (echo server)

```
NIC DMA --> skb frag pages
                | tcp_read_sock actor: kpass_tcp_recv_actor()
          get_page() --> kpass_buf.sgvec[]   (zero-copy capture)
                |
          Userspace receives: {buf_id=5, bytes=1024}
          Userspace commands: send(sock=2, buf=5, len=1024)
                |
          sock_sendmsg(MSG_SPLICE_PAGES)    (zero-copy TX)
                |
          TX skb --> NIC DMA
```

The RX actor uses `tcp_read_sock` with a custom callback that:
- Captures paged skb fragments via `get_page()` into the buffer's scatter-gather
  vector (`sgvec`) -- zero-copy
- Copies linear skb data (rare, from `skb->data`) into pool pages as a fallback

The TX path uses `MSG_SPLICE_PAGES` with `bvec` iterators for both captured
(sgvec) and pool (pages array) buffers, allowing TCP to take page references
instead of copying.

### Origination (test client)

```
Userspace data
    | poke ioctl (copy_from_user)
Pool page in kpass_buf.pages[]             (1 copy: user -> kernel)
    | sock_sendmsg(MSG_SPLICE_PAGES)
TX skb --> NIC DMA                         (0 copies)
```

---

## API Reference

### Lifecycle

| Function | Description |
|----------|-------------|
| `ceph_kpass_create(num_buffers, ring_depth)` | Open device, init buffer pool, set up io_uring |
| `ceph_kpass_destroy(ctx)` | Close all sockets, free resources |

### Sockets

| Function | Description |
|----------|-------------|
| `ceph_kpass_socket_create(ctx)` | Create kernel TCP socket, returns `sock_id` |
| `ceph_kpass_socket_listen(ctx, sock, port, backlog)` | Bind and listen |
| `ceph_kpass_socket_accept(ctx, sock, tag)` | Async accept (completes via event) |
| `ceph_kpass_socket_connect(ctx, sock, addr, port, tag)` | Async connect |
| `ceph_kpass_socket_close(ctx, sock)` | Close socket |

### Buffers

| Function | Description |
|----------|-------------|
| `ceph_kpass_buf_alloc(ctx)` | Allocate buffer handle from pool |
| `ceph_kpass_buf_free(ctx, buf)` | Return buffer to free list |
| `ceph_kpass_buf_size(ctx)` | Get per-buffer size |
| `ceph_kpass_buf_count(ctx)` | Get total buffer count |

### I/O

| Function | Description |
|----------|-------------|
| `ceph_kpass_send(ctx, sock, buf, offset, len, tag)` | Queue async send |
| `ceph_kpass_recv(ctx, sock, buf, offset, max_len, tag)` | Queue async receive |
| `ceph_kpass_flush(ctx)` | Submit pending SQEs to io_uring |
| `ceph_kpass_poll(ctx, events, max, timeout_ms)` | Wait for completions |

### Peek/Poke (explicit data copy)

| Function | Description |
|----------|-------------|
| `ceph_kpass_peek(ctx, buf, offset, len, dest)` | Copy bytes from kernel buffer to userspace |
| `ceph_kpass_poke(ctx, buf, offset, len, src)` | Copy bytes from userspace into kernel buffer |
| `ceph_kpass_set_auto_peek(ctx, size)` | Auto-copy first N bytes in recv events (0 = off) |

### Events

Events returned by `ceph_kpass_poll`:

| Event Type | Fields | Meaning |
|------------|--------|---------|
| `KPASS_EV_SEND_DONE` | `.sent.bytes` | Send completed |
| `KPASS_EV_RECV_DONE` | `.recvd.bytes`, `.recvd.peek_len`, `.recvd.peek[]` | Receive completed |
| `KPASS_EV_ACCEPTED` | `.accepted.new_sock` | New connection accepted |
| `KPASS_EV_CONNECTED` | `.sock` | Connect completed |
| `KPASS_EV_CLOSED` | `.sock` | Socket closed/disconnected |
| `KPASS_EV_ERROR` | `.err.error` | Error on socket |

---

## Kernel Module Internals

### Buffer Pool

The buffer pool is a contiguous `vmalloc` region (not `vmalloc_user` -- never
mapped to userspace). Each buffer has:

- **Pool pages** (`pages[]`): vmalloc-backed pages for origination (poke + send)
- **Scatter-gather vector** (`sgvec[]`): array of `{page, offset, length}` entries
  for zero-copy RX capture and forwarding
- **State machine**: `BUF_FREE` -> `BUF_ALLOCATED` -> `BUF_TX_QUEUED` /
  `BUF_RX_POSTED` -> `BUF_TX_INFLIGHT` -> `BUF_ALLOCATED`

When a buffer is freed, any captured page references (from `get_page` in the RX
actor) are released via `put_page`.

### Socket Callbacks

All socket callbacks (`sk_data_ready`, `sk_write_space`, `sk_state_change`) run
in softirq context and only schedule workqueue items:

- `sk_data_ready`: schedules `rx_work` or `accept_work`
- `sk_write_space`: schedules `tx_work`
- `sk_state_change`: updates connection state

The accept path is fully deferred to a workqueue (`accept_work`) to avoid
sleeping in softirq context.

### RX Path: `tcp_read_sock` + Actor

`kpass_rx_work_fn` calls `tcp_read_sock` with `kpass_tcp_recv_actor`. The actor
callback receives individual skbs and:

1. For linear data (`offset < skb_headlen`): copies into pool pages via
   `skb_copy_bits`, then adds pool page reference to sgvec
2. For paged fragments: calls `get_page` on the skb fragment page and records
   it in sgvec -- true zero-copy

The buffer's `captured` flag is set to `true` after successful capture,
indicating that `sgvec` entries hold page references that must be released.

### TX Path: `MSG_SPLICE_PAGES`

`kpass_tx_work_fn` handles both buffer types:

- **Captured buffers**: iterates sgvec entries, creating a `bvec` for each and
  sending with `MSG_SPLICE_PAGES`
- **Pool buffers**: iterates pages array with offset/length, same
  `MSG_SPLICE_PAGES` path

`MSG_SPLICE_PAGES` tells TCP to take a reference on the page rather than copy
its contents, achieving zero-copy from buffer to NIC.

### io_uring Command Interface

All async operations use `io_uring_cmd` with the `kpass_sqe_cmd` structure
embedded in the SQE. Completions use 3-argument `io_uring_cmd_done(ioucmd, res,
issue_flags)`:

- In `kpass_uring_cmd`: uses the `issue_flags` parameter passed by io_uring
- In workqueue context: uses `IO_URING_F_UNLOCKED`

---

## Build and Usage

### Prerequisites

- Linux kernel headers (matching running kernel)
- `liburing-dev` (for userspace library)
- GCC with C11 support

### Build

```sh
cd kpass
make lib demos    # userspace library and demo apps
make module       # kernel module (requires kernel headers)
make              # all of the above
```

### Load Module

```sh
sudo make load    # insmod kernel/ceph_kpass.ko
sudo make unload  # rmmod ceph_kpass
```

### Run Test

```sh
# Module must be loaded first
make test
```

### Manual Test

Terminal 1 (destination):
```sh
nc -l 9999
```

Terminal 2 (echo server):
```sh
./demo/echo_server 8888 127.0.0.1 9999
```

Terminal 3 (client):
```sh
./demo/test_client 127.0.0.1 8888 "hello"
```

---

## Known Limitations

1. **IPv4 only** -- IPv6 socket support not implemented
2. **TCP only** -- no UDP or other protocol support
3. **No mmap** -- userspace cannot directly access buffer data (by design);
   use peek/poke for data access
4. **Linear skb data is copied** -- rare with GRO/TSO but not zero-copy for
   small packets that stay in `skb->data`
5. **Single accept at a time** -- only one pending accept per listening socket
6. **Buffer size capped at 1MB** -- `KPASS_MAX_BUF_SIZE`
7. **Scatter-gather limited to 128 entries** -- `KPASS_MAX_SG_ENTRIES`
