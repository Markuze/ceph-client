# Ceph Zero-Copy Kernel Passthrough (kpass)

## Overview

kpass is a Linux kernel module (`ceph_kpass.ko`) and companion userspace library
(`libceph_kpass`) that provides page-based network I/O for TCP forwarding
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

Eligible payload: no copy during capture or TCP page splicing
```

This diagram describes the eligible path. RX copies unsafe backing into pool
pages; TX can copy when the route or device cannot transmit the page layout.
It does not establish that the entire NIC/driver/network path is copy-free.
For data origination (poke + send), `copy_from_user` transfers data into the
pool before TX uses `MSG_SPLICE_PAGES`.

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

- Retains eligible linear heads and paged fragments via `get_page()` in the
  buffer's scatter-gather vector (`sgvec`).
- Walks chained skbs (`frag_list`), including nested chains.
- Copies slab-backed or cloned heads and externally mutable fragments into
  separately allocated pages. Linear data is not inherently a copy case: a
  page fragment can back the linear head, even when `nr_frags == 0`.

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

Every active sgvec entry owns one page reference, including entries pointing
to copied RX data. Entries stay within one base page so PEEK's local mapping
and TX's bvec describe the same bytes, including higher-order source pages.
References are released when a buffer is freed, replaced by another receive,
or destroyed with its session. TCP can free its skb while the capture lives.
RX fallback copies use separately allocated pages, so freeing or reusing a
buffer handle cannot overwrite bytes held by an outstanding TX reference.

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
callback receives individual skbs. It limits capture to the remaining
descriptor count, available skb bytes, and buffer capacity before walking the
head, `frags[]`, and `frag_list` in stream order.

| RX backing | Action | Reason |
| --- | --- | --- |
| Linear head with `!skb_head_is_locked(skb)` | Retain page references | Linux splice's rule: `head_frag` is set and the head is not cloned |
| Slab-backed or cloned linear head | Copy to newly allocated pages | A page reference alone cannot safely retain this head |
| Ordinary readable fragments | Retain page references | The pages outlive the skb through their references |
| Fragments marked `SKBFL_SHARED_FRAG` | Copy to newly allocated pages | External writers may change the source after capture |
| Children of an skb marked `SKBFL_SHARED_FRAG` | Copy their payload | Conservatively propagate the parent's sharing flag through the chain |
| Unreadable skb fragments | Return `-EIO` | Device-memory payload cannot be mapped by this implementation |

An skb's own sharing flag applies to its fragments; its eligible private
linear head can still be retained. Page-pool pages use ordinary `get_page`:
the extra reference prevents recycling when the skb is freed. This follows
the existing splice ownership scheme rather than retaining the entire skb.

Both copied and retained extents set `captured` and extend `total_len`.
`copied_len` counts fallback bytes for the current capture internally; it is
not yet exposed through the UAPI. Each fallback extent allocates an ordinary
page in RX worker context and transfers the allocation's reference to sgvec.
The vmalloc pool is used only for demo origination. Pool stride and later
slot writes cannot affect captured RX bytes. `skb_copy_bits` errors release
the new page before returning; allocation failure returns `-ENOMEM` when
no prefix has been captured, or completes a short receive after a prefix.

A full vector produces a short receive, not an extra payload copy. If an
error follows a successfully captured prefix, that prefix is returned to TCP
as consumed; a later read reaches the remaining data/error. Zero-length RECV
commands complete immediately. Nonzero RECV offsets still follow the
prototype's existing behavior: they constrain validation, while captured
bytes are indexed from zero in sgvec. Use offset zero for receive.

See [README.md](README.md#rx-kernel-tests) for the reproducible KUnit and
loopback TCP tests. These tests cover the capture layer; they do not validate
all io_uring scheduling, cancellation, or EOF behavior.

### TX Path: `MSG_SPLICE_PAGES`

`kpass_tx_work_fn` handles both buffer types:

- **Captured buffers**: iterates sgvec entries, creating a `bvec` for each and
  sending with `MSG_SPLICE_PAGES`
- **Pool buffers**: iterates pages array with offset/length, same
  `MSG_SPLICE_PAGES` path

`MSG_SPLICE_PAGES` requests page splicing. It is not a guarantee of copy-free
transmission through the complete network stack.

#### TX review on Linux 7.2.9

The current kpass TX worker builds one `bio_vec` at a time, initializes an
`ITER_SOURCE` iterator, and calls `sock_sendmsg` with
`MSG_SPLICE_PAGES | MSG_DONTWAIT`, adding `MSG_MORE` between extents.

| Stage/condition | Behavior verified in this tree |
| --- | --- |
| TCP route supports `NETIF_F_SG` | `tcp_sendmsg_locked` selects `MSG_SPLICE_PAGES`; `skb_splice_from_iter` adds page references |
| Route lacks `NETIF_F_SG` | TCP uses `skb_copy_to_page_nocache` and copies the payload |
| TX skb reaches its fragment limit | TCP starts another segment after `-EMSGSIZE`; the limit alone does not require a payload copy |
| An unsafe page reaches `skb_splice_from_iter` | `sendpage_ok` rejects it with a warning/error; our bvec path must supply valid non-slab pages |
| Device cannot transmit the skb layout | `validate_xmit_skb` may linearize it, copying payload |
| Software checksum is needed for shared fragments | `skb_checksum_help` linearizes the fragments before checksumming |

Source locations: [`tcp_sendmsg_locked`](../net/ipv4/tcp.c),
[`skb_splice_from_iter`](../net/core/skbuff.c),
[`sendpage_ok`](../include/linux/net.h),
[`skb_needs_linearize`](../include/linux/skbuff.h), and
[`validate_xmit_skb` / `skb_checksum_help`](../net/core/dev.c).

TCP marks these spliced fragments `SKBFL_SHARED_FRAG` unless the sender sets
`MSG_NO_SHARED_FRAGS`. Thus a local kpass-to-kpass transfer can take our RX
copy fallback.

For opaque objects, reusing a handle means sending the same retained data
again. The object's page references prevent RX allocator recycling; TCP
takes its own references for transmission. Repeated sends need no new
backing, sealing operation, or additional immutability state. SEND completes
when the requested bytes have been accepted into TCP; the page references
provide the remaining lifetime. Compaction is deferred and will address
changes to object backing when it is implemented.

POKE is the prototype demo's userspace-to-pool copy ioctl and already rejects
captured RX buffers. It is not needed for forwarding or repeated object
sends. RX copy fallbacks now use ordinary pages owned by each capture.
The capture and TCP release their references independently. Reposting,
freeing/reallocating the same handle, or destroying its session leaves
already-spliced data intact until the final page reference is released.
The tests reproduce the old overwrite both in a TX skb and over live TCP.

`MSG_NO_SHARED_FRAGS` fits the retained-object path under these semantics.
Its use can cover captured RX pages, including the separately allocated
copy fallback pages. Legacy POKE buffers still permit writes to fixed pool
storage and need their own lifetime handling before using that flag.

The next TX implementation work is:

1. Enforce captured-buffer offset/length bounds; the existing worker sends
   through the end of sgvec without applying the requested length.
2. Preserve a cumulative send count and reliable progress across partial
   sends and `EAGAIN`; the current completion reports only the last work run.
3. Address legacy POKE pool lifetime and pool-page addressing for
   non-page-aligned buffer strides. Captured RX pages now have independent
   reference lifetimes, including copied data.
4. Batch bvec extents into fewer send calls and use `MSG_NO_SHARED_FRAGS`
   where the retained-page ownership permits it.
5. Measure copies with the target NIC and offload settings, including SG and
   checksum fallbacks. The RX loopback test is not that measurement.

### io_uring Command Interface

All async operations use `io_uring_cmd` with the 64-byte `kpass_sqe_cmd`
structure embedded in an SQE128 entry. The library requests
`IORING_SETUP_SQE128`; a standard SQE has only 16 command bytes. The
module rejects undersized SQEs and reads the payload with
`io_uring_sqe128_cmd`. Completions use 3-argument
`io_uring_cmd_done(ioucmd, res, issue_flags)`:

- In `kpass_uring_cmd`: uses the `issue_flags` parameter passed by io_uring
- In workqueue context: uses `IO_URING_F_UNLOCKED`

---

## Build and Usage

### Prerequisites

- A completed Linux 7.2.9 build (matching the runtime test kernel)
- liburing 2.15 or newer (2.15 is pinned for this branch)
- GCC with C11 support

See [README.md](README.md) for the pinned revisions, local dependency
setup, reproducible build commands, and current verification limits.

### Build

```sh
cd kpass
make lib demos    # userspace library and demo apps
make module KDIR=/path/to/kernel/build
make KDIR=/path/to/kernel/build  # all of the above
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
4. **Some RX backing requires copying** -- slab/cloned heads and externally
   mutable fragments; unreadable fragments are unsupported
5. **Single accept at a time** -- only one pending accept per listening socket
6. **Buffer size capped at 1MB** -- `KPASS_MAX_BUF_SIZE`
7. **Scatter-gather limited to 128 entries** -- `KPASS_MAX_SG_ENTRIES`;
   fragmented receives can complete short before reaching the byte limit
8. **Prototype completion and lifetime gaps** -- TX range/progress and legacy
   POKE pool reuse issues above, plus connect/EOF completion, cancellation,
   and teardown, must be resolved before performance evaluation
