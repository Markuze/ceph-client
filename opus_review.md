# Review: io_uring opaque TCP payload objects (`OPAQUE_OBJ`)

**Series reviewed:** `origin/upstream/opaque-obj` at `ce7be6c326f8`, 5 patches on top of
`af32da41b032` ("Merge tag 'net-7.3-rc7'"):

| # | Commit | Subject |
|---|--------|---------|
| 1/5 | `3f40353f7e7e` | net: protect TCP receive sequence ownership for opaque collectors |
| 2/5 | `feb644a2b4a9` | io_uring: add kernel-resident opaque TCP objects and native send |
| 3/5 | `5165449a674a` | io_uring: compact opaque objects manually or with bounded automatic policy |
| 4/5 | `573b684fa2c0` | io_uring: test opaque object ownership, compaction and native ABI |
| 5/5 | `ce7be6c326f8` | docs: describe opaque object ABI, lifetime and comparison claims |

The checked-out branch `opaqu_objects` (`a436cfe75e2c`) holds the out-of-tree `kpass`
prototype the series was ported from. I reviewed the io_uring series. I looked at the
prototype only to see where the port's behavior differs.

All `file:line` references are to `origin/upstream/opaque-obj` at `ce7be6c326f8`.

This is written from the perspective of an io_uring maintainer reviewing an RFC: blunt
about what blocks merging, specific about how to fix it. It is not an actual maintainer
response.

---

## 1. Verdict

**NAK for this revision; useful RFC.**

The core idea is interesting and deserves a real look. Kernel-resident, immutable TCP
payload objects are inspected through bounded copies, published only when complete, and
retransmitted many times without user mappings and without SEND_ZC's notification
round-trip. The "no reuse notification because userspace can never overwrite the payload"
argument is sound. The documentation is unusually honest about what is and isn't claimed.

The series can't go in as is, for four reasons:

1. **Bugs that real workloads hit immediately.** I reproduced all of them; see §3.
   - The collector dies after 128 poll re-arms, which is about 32 MiB of continuous data
     or 128 idle→busy transitions. When it dies it silently drops undecided bytes.
   - A TCP reset publishes **truncated objects as complete**.
   - A blocking object SEND completes short, and the FIFO then **splices the next object
     into the middle of the previous one on the wire**.
2. **It works around io_uring's request model instead of using it.** It adds a private
   budget-wait list, a private cancel list, a private per-socket FIFO and task_work
   resubmission next to poll/apoll/links. Bugs A and D both come from that.
3. **It moves into zcrx and `RECV_ZC` without belonging there.** No device, no area, no
   refill ring, no page pool and no data CQEs, yet it reuses their registration, opcode
   and ID space, and it reshuffles zcrx's `CAP_NET_ADMIN` gate.
4. **It needs a hook in core networking first.** That's a new `struct sock` field under an
   io_uring Kconfig symbol (`default y`), plus checks in the TCP receive hot path. netdev
   has to agree to that first.

The individual bugs are easy to fix; prototype fixes for A–D are in Appendix A and pass
everything. The design concerns in §4 are what decide whether a v2 can converge.

---

## 2. What I ran

The tree has no QEMU, so I used UML (`ARCH=um`). It needs no root and no KVM, and it runs
KUnit natively. On UML, `CONFIG_VETH` is only there to select `PAGE_POOL` for
`IO_URING_ZCRX`.

| Check | Result |
|-------|--------|
| Build every commit (`ARCH=um`, KASAN + lockdep config) | all 5 build, bisectable |
| `W=1` on `io_uring/` and `net/ipv4/tcp.o` (UML 64-bit and `ARCH=i386` + `HIGHMEM`) | no new warnings; no 64-bit libgcc division helpers in `opaque.o` |
| `CONFIG_IO_URING_OPAQUE_OBJ=n` | builds |
| `checkpatch.pl --strict` | clean apart from the MAINTAINERS file-path warnings |
| KUnit `io_uring-opaque` (KASAN, PROVE_LOCKING, DEBUG_ATOMIC_SLEEP, DEBUG_LIST, DEBUG_VM) | 11/11 pass |
| `tools/testing/selftests/io_uring/opaque_obj` (static, raw ABI) | 21/21 pass, no splats |
| My reproducers (Appendix B), baseline | **7 of 9 cases fail** (A1, A2, B, C, D, D2, F); E and G show the questionable semantics described in §3 |
| Reproducers + selftest + KUnit with Appendix A applied | A–D pass; selftest 21/21; KUnit 11/11; no splats |

The existing tests pass because none of them cross 128 poll cycles on a collector, reset
a connection mid-object, use a `CQE_MIXED` ring, throttle a receiver enough to force more
than 127 `POLLOUT` waits, or interleave fragments from one compound page.

Reproducer results (`./repro <letters>`, one forked process per case):

| Case | Scenario | Baseline (`ce7be6c`) | With Appendix A |
|------|----------|----------------------|-----------------|
| A1 | collector, 300 bursts × 100 B, 3 ms apart, DISCARD of everything | collector CQE `-EAGAIN`; DISCARD `-ECANCELED` | pass |
| A2 | collector, 64 MiB continuous | collector CQE `-EAGAIN`; DISCARD `-ECANCELED` | pass |
| B | KEEP(0, 4096); peer sends 1000 B then RST | **KEEP = 4096 with a handle; OBJECT_STAT.length = 1000**; READ_STREAM = 64 with buffer untouched; collector res = **+104** | all three `-ECONNRESET` |
| C | attach on a `CQE_MIXED` ring | 32-byte CQE without `IORING_CQE_F_32`; phantom CQE with `user_data` = token | pass |
| D | 2 MiB OBJ_SEND, no `MSG_DONTWAIT`, slow peer | res = 526336 (short) | 2097152 |
| D2 | SEND X (2 MiB 'A') then SEND Y (64 KiB 'B'), same socket | **peer stream: 526336 'A', then 'B'; the rest of X is never sent** | in order |
| E | FREE(h) → `IOSQE_IO_LINK` → READ(h) | FREE = 0, **READ = 8** | unchanged (design, §3.H4) |
| F | 256 KiB store, 32 KiB KEEP, one sender thread interleaving 1 KiB writes to two sockets | **8 KiB captured, 262144/262144 charged, KEEP never completes** | unchanged (design, §3.H2) |
| G | `close()` the collected socket fd | peer sees no FIN | unchanged (design, §3.M1) |

I also tried the two obvious partial fixes for A, to show why A is not a one-liner:

- **Only setting `REQ_F_APOLL_MULTISHOT`.** A1 passes. A2 and selftest #13 hang: the
  256 KiB batch cap with edge-triggered multishot poll needs `IOU_REQUEUE`.
- **Adding `IOU_REQUEUE` as well.** A1 and A2 pass. The selftest budget test then hits
  `WARNING: io_uring/io_uring.c:1447 at io_poll_issue`, because the budget-park path
  returns `IOU_ISSUE_SKIP_COMPLETE` from multishot context.

Appendix A resolves both.

---

## 3. Bugs (must fix)

### A. [critical] The collector dies after `APOLL_MAX_RETRY` re-arms, and its death destroys stream state

- **Where:** `io_uring/net.c:1317-1321`, `io_uring/opaque.c:1078`, `io_uring/opaque.c:1530-1536`.
- **Mechanism:**
  1. The opaque branch of `io_recvzc_prep()` returns before
     `req->flags |= REQ_F_APOLL_MULTISHOT`. `io_opaque_recv()` then returns `-EAGAIN`
     after every batch.
  2. Every time the collector drains the socket or uses up `IO_OPAQUE_RX_BATCH` (256 KiB),
     `io_queue_async()` re-arms a **oneshot** apoll. `io_req_alloc_apoll()` decrements
     `apoll->poll.retries` on every re-arm (`io_uring/poll.c:653-678`), and
     `REQ_F_POLLED` is only cleared at free.
  3. After 128 arms, `io_arm_poll_handler()` returns `IO_APOLL_ABORTED` and the request
     goes to io-wq.
  4. In io-wq, `needs_poll` is false (no `FORCE_ASYNC`, which prep rejects anyway) and
     `io_opaque_recv()` is nonblocking by construction. So the `-EAGAIN` is final
     (`io_uring/io_uring.c:1544`, then `1562`).
  5. `io_opaque_cleanup()` sees `collector == op` and **closes the stream**: undecided
     bytes are freed, the receive claim is released, and every pending READ_STREAM / KEEP /
     DISCARD gets `-ECANCELED`.
- **Impact:** Any connection that moves more than about 32 MiB, or goes idle and busy 128
  times, loses its collector and the bytes already consumed from TCP but not yet decided.
  If the application re-arms, it gets a *new* stream whose offset 0 is after the lost
  bytes. That is silent data loss for a data mover.
- **Fix:** Not a one-liner; see the two failed attempts in §2. The collector has to be a
  real multishot:
  1. Set `REQ_F_APOLL_MULTISHOT` and return `IOU_RETRY`.
  2. Return `IOU_REQUEUE` when the batch cap was hit under `IO_URING_F_MULTISHOT`, the
     way `io_recv()` does with `MULTISHOT_MAX_RETRY`.
  3. Never return `IOU_ISSUE_SKIP_COMPLETE` from `io_poll_issue()` context.

  Appendix A does (3) by keeping the poll armed on budget exhaustion and having
  `io_opaque_wake_budget()` re-trigger it by waking the socket wait queue. That passes all
  tests, but it's a minimal patch, not what I'd want long term. The idiomatic design is to
  terminate the multishot with `-ENOBUFS`, like provided-buffer exhaustion, and **decouple
  stream lifetime from the collector request**, so the application re-arms by stream token
  without losing undecided bytes (see §4.2).

### B. [critical] A socket error is stored with the wrong sign; truncated objects are published as complete

- **Where:** `io_uring/opaque.c:1058`, consumed at `io_uring/opaque.c:724-725` and `:736-737`.
- **Mechanism:**
  1. `sock_error()` already returns a negative errno (and clears `sk_err`), so
     `stream->error = -sock_error(sk)` stores `+ECONNRESET`, `+ETIMEDOUT`, and so on.
  2. `io_opaque_stream_process()` then does
     `ret = stream->error ?: -ENODATA; io_opaque_queue_ready(op, ret < 0 ? ret : op->length)`.
     **Every pending range completes as success:**
     - **KEEP** publishes the partial assembly under a valid handle and reports the full
       length.
     - **READ_STREAM** reports `op->length` even though nothing was snapshotted.
       `io_opaque_copy()` skips missing extents and still returns `length`, so the user
       buffer is left stale.
     - **DISCARD** claims everything was discarded.
     - The collector's terminal CQE carries a **positive** errno, and `STREAM_STAT.error`
       is positive.
- **Impact:** A peer RST or keepalive timeout mid-object turns into a "complete" object
  that a proxy then forwards. This is silent data corruption.
- **History:** The prototype had `ksock->error = -sk->sk_err`
  (`kpass/kernel/ceph_kpass_stream.c:461`), which is correct. This is a porting
  regression.
- **Fix (verified):** `stream->error = sock_error(sk);`.
- **Hardening:** Also make success structurally require proof. `stream_process()` should
  only complete a range successfully when `io_opaque_take()` / `io_opaque_snapshot()`
  returned 1. `io_opaque_copy()` should fail if the extents don't cover the requested
  range. `WARN_ON_ONCE(stream->error > 0)` costs nothing.

### D. [critical] Blocking OBJ_SEND completes short, then the FIFO splices the next object into the stream

- **Where:**
  - `io_uring/opaque.c:1671`: `MSG_DONTWAIT` is set unconditionally.
  - `:1692-1694`: partial progress is turned into `-EAGAIN`.
  - `:1558-1559`: `->fail` turns the failure into a positive short count.
  - `:1649-1661`: `io_opaque_tx_put()` promotes the next queued send.
- **Mechanism:**
  1. The head of the per-(store, socket) queue arms `POLLOUT` apoll. It runs into the same
     128 cap as A and is punted to io-wq.
  2. In io-wq the send is still `MSG_DONTWAIT`, so `-EAGAIN` is final. The request
     completes with `REQ_F_FAIL` and `res = progress`.
  3. Its cleanup promotes the next queued send, which transmits its object
     **before the rest of the previous object**.
- **Doc contradiction:** The documentation promises "Without MSG_DONTWAIT, the request
  continues until the range is queued or an error/cancellation ends it" and
  "Object sends are FIFO".
- **Reproduced:** In D2 the peer's byte stream is 526336 bytes of object X, then all of
  object Y, and the rest of X never arrives.
- **Fix (verified):** Do what `io_send()` does. Add `MSG_DONTWAIT` only when
  `IO_URING_F_NONBLOCK` is set, so the io-wq fallback blocks. Drop `uring_lock` around the
  blocking `sock_sendmsg()` in io-wq: `io_ring_submit_unlock()` is a no-op inline.
- **Still needed (not prototyped):** If the head completes short or with an error for any
  reason, fail the queued followers on that (store, socket) queue (`-ECANCELED`, like a
  broken link chain). The causes include `MSG_DONTWAIT`, `SO_SNDTIMEO`, signals, and
  `-EPIPE` after progress. Otherwise the FIFO guarantee is the thing corrupting the
  stream.

### C. [high] `CQE_MIXED`: the stream-token CQE is posted without `IORING_CQE_F_32`

- **Where:** `io_uring/opaque.c:999` sets `cqe[0].flags = IORING_CQE_F_MORE`.
- **The rule:** In a `CQE_MIXED` ring, a 32-byte entry must carry `IORING_CQE_F_32`
  (`zcrx.c:1711` does this; `io_req_set_res32()` adds it via `ctx_cqe32_flags()`).
- **Impact:** Userspace parses the first half as a 16-byte CQE. It then reads the second
  half, which is the token, as a phantom CQE with `user_data = token`. The documentation
  says `CQE_MIXED` is supported, but every selftest uses `CQE32`.
- **Fix (verified):** `cqe[0].flags = IORING_CQE_F_MORE | ctx_cqe32_flags(ctx);`.

### H1. [high] TOCTOU: double fetch of `sqe->msg_flags` in send prep

- **Where:** The flags are validated with a plain read at `io_uring/opaque.c:1571-1572`
  and fetched again with `READ_ONCE()` at `:1581`.
- **Why it matters:** The SQE is shared memory, so userspace can change it between the
  check and the use. Arbitrary flags then reach a kernel-originated
  `sendmsg(MSG_SPLICE_PAGES)`. That includes the `MSG_INTERNAL_SENDMSG_FLAGS` set that
  `__sys_sendmsg()` strips because userspace must never set them:
  - `MSG_SENDPAGE_DECRYPTED` sets `skb->decrypted` at `tcp.c:1264`.
  - `MSG_NO_SHARED_FRAGS` keeps TX frags from being marked `SKBFL_SHARED_FRAG` at
    `tcp.c:1366`.
  - `MSG_SENDPAGE_NOPOLICY`.

  It also lets through `MSG_ZEROCOPY`, `MSG_OOB`, `MSG_FASTOPEN` and others.
- **Exploitability:** I didn't construct an exploit; the effects I traced are
  self-inflicted. But this is exactly what the read-once rule is for.
- **Fix:** Read every SQE field exactly once into a local and validate the local. Do this
  in both prep functions; `zcrx_ifq_idx`, `fd`, `rw_flags`, `buf_index` and `__pad2` are
  all plain reads today.

### H2. [high] Accounting has no progress guarantee: one sub-page fragment can charge a whole compound page

- **Where:**
  - `io_uring/opaque.c:228`: every new extent charges `page_size(compound_head(page))`.
  - `:773-776`: only exactly adjacent captures coalesce.
  - `:1234-1237`: the `EMSGSIZE` check compares *length*, not charge, against the limit.
- **Reproduced (F):** One task alternates 1 KiB writes to two sockets. That's normal for
  any multi-connection sender, because of the shared task_frag. 8 KiB of payload in 8
  extents charged the entire 256 KiB store, so a 32 KiB KEEP can never complete; it only
  ended through a linked timeout.
- **In production:** The same thing happens with `page_frag_cache` heads (32 KiB) and with
  any driver that uses high-order page_pool pages.
- **Why it's a policy problem:** The charge isn't dishonest, because retention really
  pins the compound page. The problem is that a hard limit with no guaranteed progress is
  unusable as a configuration knob.
- **Suggestions:**
  1. Retain only if the compound charge fits in the *remaining* budget; otherwise copy.
     Then any KEEP up to `hard_limit / 2` or so is guaranteed to progress.
  2. Share one backing (one charge, one page ref) between all extents of the same
     compound page in a stream. A one-entry "last backing" cache already catches the
     interleaved case.
  3. Pack the copy path. It currently allocates one page per ≤4 KiB fragment, so 100-byte
     segments cost 4 KiB each (`:851-873`).
  4. Bound or compact private assembly; the doc admits it isn't compacted.

### H3. [high] Handles are resolved and pinned in `->prep()`, which breaks link ordering

- **Where:**
  - `io_uring/opaque.c:1251-1284`: stream and object lookup, data pin, slot reservation
    and generation bump.
  - `:1587-1598`: SEND prep pins `data` and builds the iterator.
- **The problem:** Prep runs at submission time, so link order doesn't order effects.
  FREE(h) → `IOSQE_IO_LINK` → READ(h) gives FREE = 0, READ = 8 (reproduced). The same
  applies to SEND, OBJECT_STAT and COMPACT queued behind a FREE or a COMPACT. Prep-time
  pinning also holds backing for requests sitting in deferred or linked chains.
- **Precedent:** io_uring moved fixed file and fixed buffer resolution to issue time for
  exactly this reason.
- **Fix:** Resolve at issue. The doc sentence "An SQE awaiting preparation has not
  acquired its backing reference" is describing this bug, not a feature.

### M1. [medium] The stream holds the socket file; `close(fd)` doesn't close the connection

- **Where:** `stream->file = get_file()` at `io_uring/opaque.c:970` holds the socket until
  STREAM_CLOSE or store teardown. Reproduced (G): no FIN after `close()`.
- **Also:** After the collector terminates on EOF, the receive claim stays set until
  STREAM_CLOSE (`close = !eof && ...` at `:1532`).
- **Ask:** Either document this prominently ("you must STREAM_CLOSE every stream; closing
  the fd is not enough"), or don't hold a file reference. For example, clear ownership
  from `sk_destruct`/`release`, or tie the stream to the collector's fd.
- **Related:** Ownership isn't tied to the connection. `connect(AF_UNSPEC)` →
  `tcp_disconnect()` isn't blocked while claimed.

### M2. [medium] A parked collector is deaf to the socket

- **Where:** In `BUDGET_WAIT` (`io_uring/opaque.c:1069-1076`) the collector isn't on the
  socket wait queue.
- **Impact:** A RST, a FIN or urgent data isn't noticed until some *other* request frees
  memory. Pending KEEPs on that stream just hang.
- **Fix:** Appendix A's approach (stay armed in poll) fixes this as a side effect.

### M3. [medium] Transient capture failures are sticky

- **Where:** `if (ret < 0 && ret != -ENOBUFS) stream->error = ret;` at
  `io_uring/opaque.c:1064`.
- **Impact:** A single `-ENOMEM` from `kzalloc_obj()` or `alloc_page()` kills the stream.
- **Fix:** Treat `-ENOMEM` like `-ENOBUFS` (back off and retry), and keep sticky errors
  for protocol conditions only.

### M4. [medium] Cancel, timeout or vector `-ENOMEM` on KEEP destroys consumed bytes

- **The problem:** The private prefix is freed, so the stream becomes unrecoverable after
  a linked timeout on KEEP. KEEP is the operation the doc recommends bounding with
  timeouts.
- **Fix:** Return the private extents to `stream->extents` at their stream offsets. They
  are still in stream order and don't overlap anything. The application can then retry
  KEEP or DISCARD.
- **Related:** `io_opaque_vector()` failing in `io_opaque_ready()` has the same loss.

### M5. [medium] Smaller correctness and robustness items

- **`SET_POLICY` blocks the ring.** It calls `cancel_delayed_work_sync()` from issue with
  `uring_lock` held (`opaque_compact.c:217`). That waits out an in-progress auto-compaction
  copy of up to `max_temporary_bytes` (which can be as large as `hard_limit`).
- **One workqueue and rescuer thread per store.**
  `alloc_ordered_workqueue("iou-opaque", WQ_MEM_RECLAIM)` (`opaque.c:420`) creates one
  workqueue and one rescuer kthread **per store**, and stores are creatable without
  privilege. Compaction isn't in the reclaim path, so `WQ_MEM_RECLAIM` isn't justified.
  Use a shared unbound workqueue.
- **Retained RX pages escape memcg.** They were socket memory, and TCP uncharged them on
  consumption; only memlock bounds them now. Copy-path pages *are* charged to memcg
  (`set_active_memcg()` + `GFP_KERNEL_ACCOUNT`), which is inconsistent. Consider charging
  the store's retained bytes as socket memory (`mem_cgroup_charge_skmem()`) to the owner's
  memcg.
- **Thundering herd.** `io_opaque_wake_budget()` requeues *every* waiter on every extent
  free (`opaque.c:177-188`, called from `io_opaque_extent_free()`).
- **Attach with a full CQ fails `-ENOSPC`.** `io_req_post_cqe32()` doesn't overflow.
  Document it or overflow like everything else.
- **`IORING_RECVSEND_POLL_FIRST` is accepted and ignored** for opaque collectors.
- **`xchg()` on a `bool`** (`opaque.c:442`). Use an int or a bit op.
- **`wait_lock` is `spinlock_irqsave` but has no IRQ-context users.**

---

## 4. Design review (what decides v2)

### 4.1 Don't build this inside zcrx and `RECV_ZC`

An opaque store has none of zcrx's substance: no netdev queue, no memory provider, no
area, no refill ring and no `net_iov`. The series reuses zcrx for its ID space and
export/import, and pays for that with special cases throughout `zcrx.c`:

- `io_zcrx_scrub()`
- `io_zcrx_get_region()`
- `import_zcrx()`
- `io_zcrx_ctrl()`
- an `opaque` pointer in `struct io_zcrx_ifq`
- `io_cold_defs[IORING_OP_RECV_ZC].cleanup` set for **all** RECV_ZC requests

It also moves zcrx's `CAP_NET_ADMIN` check below a new unprivileged branch
(`zcrx.c:1042-1053`). That is safe today, but now every future zcrx flag has to be
reasoned about against that ordering. `IORING_OP_RECV_ZC` with an opaque ifq is a
different operation altogether: zero length, no data CQEs, a token CQE, and
"multishot" without multishot semantics.

I'd expect the zcrx maintainer to NAK this, and I would. Give the store its own
registration opcode (or a generic "kernel object table" registration) with its own
export/import fds, and give attach/collect its own opcode. You'll delete code doing it.

### 4.2 Use io_uring's request model instead of building a second one

The series adds:

- `ctx->opaque_waits` plus `io_opaque_cancel()` / `io_opaque_cancel_all()`, a second
  cancellation registry.
- `store->budget_waits` plus `io_opaque_resume()`, which parks requests outside poll.
- A per-(store, socket) FIFO with head-only polling and task_work resubmission.
- Phase/state machines (`IO_OPAQUE_*`) arbitrated by a spinlock.
- Per-request `kzalloc_obj()` with manual `REQ_F_ASYNC_DATA` instead of `.async_size`
  plus an alloc cache.

Bugs A and D are what happens when that machinery meets the core's assumptions:
`APOLL_MAX_RETRY`, io-wq's blocking-fallback contract, and the `io_poll_issue()` WARN. The
idiomatic shapes are:

- **Collector:** A real multishot (`REQ_F_APOLL_MULTISHOT`, `IOU_RETRY`/`IOU_REQUEUE`).
  It terminates with `-ENOBUFS` when the store is full, just as multishot recv terminates
  when provided buffers run out. The stream (token, offsets, undecided extents, receive
  claim) outlives the request, and the application re-arms with the token. Then
  `budget_waits`, the `BUDGET_WAIT` phase and the deaf-collector problem (M2) disappear.
- **Ordering:** Use `IOSQE_IO_LINK` for "send X then Y on this socket". Links already have
  the right failure semantics; a short head breaks the chain. If you really want an
  implicit per-socket queue, it has to fail its followers (D), and it should be a
  socket-level feature rather than per store.
- **Waiting for ranges:** KEEP/READ_STREAM waiting for future bytes is the one genuinely
  new kind of wait. Keep it, but key it off the stream, and have the collector, which
  already runs in the right context, complete it. That part is mostly fine today.

### 4.3 The net-core hook (patch 1/5) needs netdev to agree, and may not be needed

- **Layering.** `struct sock` gains `sk_rx_owner` under `CONFIG_IO_URING_OPAQUE_OBJ`, a
  symbol the *net* patch adds to `io_uring/Kconfig` with `default y`. netdev will want a
  net-level symbol, or a flag bit instead of a pointer, and no io_uring symbol in
  `include/net/sock.h`.
- **Hot-path checks.** New checks in `tcp_recvmsg_locked()`, including one per loop
  iteration (`tcp.c:2682`, `:2724`), and in `tcp_read_sock()`, `tcp_read_sock_noack()`,
  `tcp_read_skb()`, `tcp_zerocopy_receive()`, `TCP_REPAIR`, `tcp_set_ulp()`,
  `sk_psock_init()` and `sk_wait_data()`. Expect a request for a static key, or for the
  hot-loop check to go.
- **New user-visible behavior.** `recv()` returns `-EBUSY`, including for readers already
  blocked when the claim is taken.
- **The real question:** Hardware RECV_ZC has the same exposure, since a concurrent
  `recv()` desynchronizes the application's view, and it relies on "don't do that". Does
  the opaque store need *kernel* enforcement for memory safety? From what I can see, no.
  Captured bytes are owned by the store, and a competing reader only corrupts the
  application's own offsets. If enforcement is only a convenience, drop patch 1 from the
  first series. If you think it's required for safety, the commit message has to show the
  failure it prevents.
- **Gaps if you keep it:**
  - `tcp_disconnect()` / reconnect isn't covered.
  - `sock_rx_owner_conflict()` reads `sk->sk_rx_owner` without `READ_ONCE()`, while
    `sock_rx_owned()` uses it.

### 4.4 UAPI surface: trim before anything else

Today's surface:

- **Two opcodes.** One of them multiplexes 11 sub-operations through `sqe->ioprio`.
- **A zcrx flag, a feature bit and a union in `io_uring_zcrx_ifq_reg`.**
- **Five structs, three of which reuse bit 0 for unrelated flags.**
  `IORING_OPAQUE_COMPACTED`, `IORING_OPAQUE_STREAM_EOF` and `IORING_OPAQUE_AUTO_COMPACT`
  all live there; name the flags per struct.
- **An automatic-compaction policy with eight tunables** (age, saving, slack %, extent
  count, rate, burst, temporary bytes). That is policy, and it would be ABI forever.

Suggestions:

- **Smallest first series:** register/export/import, attach (collector), READ_STREAM,
  KEEP, DISCARD, READ, FREE, SEND, and STREAM_CLOSE.
- **Manual COMPACT can follow** once there are numbers. **Drop automatic compaction**;
  userspace can decide when to COMPACT.
- **Move STAT, OBJECT_STAT, STREAM_STAT and SET_POLICY to register-time ops** or fdinfo;
  they don't need to be SQEs.
- **`len == sizeof(struct)` exact matching means none of these structs can ever grow.**
  Use size-versioned copies.
- **Handle encoding:** 32-bit generation plus 32-bit index, with `max_objects ≤ 16384`.
  Lowest-free-first reuse retires slot 0 after 2^32 KEEPs. That's about 7 minutes at
  10M KEEP/s, and the whole store after 16384 × 2^32. Spend more bits on the generation.

### 4.5 Relationship to what already exists: say why this isn't splice, kernel bvec buffers or zcrx NODEV

A maintainer's first question will be "why can't you do this with what's there?" The
cover letter (there isn't one; please add it) needs to answer it with numbers:

- **TCP → pipe → TCP splice** (`IORING_OP_SPLICE`) is already zero-copy socket-to-socket
  forwarding: it retains RX pages and sends with `MSG_SPLICE_PAGES`. `IORING_OP_TEE` gives
  fan-out. What it lacks is handles, framing inspection without consuming, and long-lived
  caching. Is that gap worth 2k lines plus UAPI?
- **Kernel-owned bvec buffers in the registered buffer table** (`io_buffer_register_bvec()`,
  used by ublk) plus SEND_ZC with fixed buffers already give "kernel pages, io_uring
  lifetime via `io_rsrc_node`, issue-time resolution, link ordering".
  - Publishing a completed KEEP as a kernel bvec buffer node would reuse that
    infrastructure instead of `io_opaque_slot` / `io_opaque_data` / generation handles.
  - The remaining question would be a SEND_ZC mode that skips the notification for
    immutable kernel buffers, which is a much smaller and more general patch.
  - I'd like to see this alternative evaluated explicitly.
- **zcrx `NODEV`** already gives hardware-independent RECV_ZC, one copy into a user area,
  and SEND_ZC from it. The opaque store saves the receive copy *when retention is
  possible*. Retention needs unshared, uncloned skbs, so any packet tap such as tcpdump
  or AF_PACKET turns every byte into a copy, and driver copybreak copies small packets
  anyway. That cost is the memory amplification shown in H2. Measure the copy fraction
  and memory density on real NICs (page_pool fragment drivers such as mlx5, ice, bnxt and
  virtio-net) with and without a tap.

### 4.6 Scalability and cost on the hot path

- **One `store->tables` mutex everywhere.** Every handle lookup takes it (prep of every
  op), and every SEND issue takes it twice (`tx_enter`/`tx_put`). Sharing a store across
  rings and threads is the stated use case. Use an xarray with RCU lookup and
  per-object refcounts, which is exactly what `io_rsrc_node` gives you.
- **Linear scans:**
  - Slot reservation is O(`max_objects`) per KEEP (`opaque.c:340-359`).
  - The stream-slot search is O(`max_streams`).
  - STAT and SET_POLICY are O(`max_objects`).
  - `io_opaque_stream_process()` re-walks every pending read and decision after every
    receive batch. `io_opaque_available()` and `io_opaque_take()` re-walk the extent
    list from the head, which is O(R×E) and O(E²) in bad cases. The doc's "capture
    consults the earliest decision rather than scanning all pending requests" is true
    for the actor but not for `stream_process()`.
- **Allocations per op:** a several-hundred-byte `io_opaque_req` per op (union the per-op fields;
  they're mutually exclusive), an allocation per extent, and for a split another
  allocation per piece. A data mover doing millions of objects per second will see all of
  these.

### 4.7 Benchmarks

The doc's "no throughput, latency, CPU-efficiency or physical-NIC superiority is claimed"
is admirably honest. It's also why this can't be merged yet: 2k lines plus a new hook in
`struct sock` plus UAPI need evidence. The comparison matrix in the doc is the right one.
Add memory density (retained compound bytes per payload byte) and copy fraction under
real drivers. Show them with compaction off, manual, and automatic.

---

## 5. Patch-by-patch notes

### 1/5 net: protect TCP receive sequence ownership

- **Kconfig placement.** The patch adds `CONFIG_IO_URING_OPAQUE_OBJ` to `io_uring/Kconfig`
  (with `default y`) and uses it in `include/net/sock.h`. Split it: a net-level symbol,
  selected by io_uring, and `default n` for anything new and experimental.
- **Read consistency.** `sock_rx_owner_conflict()` should `READ_ONCE()` the owner like
  `sock_rx_owned()` does. Better, lockdep-assert ownership of the socket lock in both.
- **`sk_wait_data()` change** (`sock.c:3324`). This wakes every protocol's sleepers on
  ownership. Harmless for non-TCP today, but it belongs in the commit message.
- **New errno.** `recv()` returning `-EBUSY` is a new user-visible errno for TCP; document
  it in the commit message and the man pages.
- **Missing state transitions.** Nothing covers `tcp_disconnect()`, and there's no
  `sk_destruct` path. If the hook stays, claims should die with the connection.

### 2/5 io_uring: add kernel-resident opaque TCP objects and native send

- **Confirmed bugs here:** A (`net.c:1317-1321`, `opaque.c:1078`), B (`opaque.c:1058`),
  D (`opaque.c:1671`, `:1649-1661`), C (`opaque.c:999`), H1 (`opaque.c:1571`/`:1581`),
  H3 (`opaque.c:1251-1284`, `:1587-1598`).
- **`io_opaque_alloc()`:**
  - It charges the whole `hard_limit` to `RLIMIT_MEMLOCK`/`pinned_vm` up front. That's
    consistent with io_uring practice, so fine, but say so in the doc's permission
    section.
  - The `hard_limit ≤ 1 TiB` cap is arbitrary. Tie it to something, or drop it and let
    memlock decide.
  - It runs `alloc_ordered_workqueue()` per store (M5).
- **`io_opaque_req_alloc()`:**
  - `kzalloc_obj()` plus hand-set `REQ_F_ASYNC_DATA`. Use `.async_size` and the
    io_uring alloc caches.
  - Control ops aren't counted against `max_requests`. That's fine, but an unbounded
    number of linked FREE/STAT ops can then sit behind a pending KEEP, each holding an
    allocation.
- **`io_opaque_attach()`:**
  - It posts the CQE under `lock_sock()` plus `write_lock_bh(sk_callback_lock)` while
    holding `store->tables` (a mutex every other ring's prep needs). `lock_sock()` under
    `tables` can sleep behind a long socket-lock holder.
  - It checks `sk_user_data` only when a free stream slot exists.
- **`io_opaque_capture()`:**
  - The retention rules mirror splice's: `skb_head_is_locked()`, cloned or shared
    frags, `SKBFL_SHARED_FRAG`, and `skb_frags_readable()`. KASAN and lockdep were clean
    across all my runs. Good.
  - The copy path allocates and charges a whole page per ≤ `PAGE_SIZE` chunk, with no
    packing (H2).
  - The `retain_limit` check compares against the entire capture allowance, not the
    remaining budget (H2).
- **`io_opaque_actor()`:** `u32 chunk = length;` from `size_t` is fine given the 256 KiB
  `desc.count`, but say so. The DISCARD fast path is nice.
- **`io_opaque_recv()`:**
  - It takes `uring_lock` in io-wq (`io_ring_submit_lock()`), then the stream mutex, then
    `lock_sock()`, and holds them across `tcp_read_sock()`, page allocation and copies.
  - The order `uring_lock` → (`tables` or `stream->lock`, never nested) → `sk_lock` →
    `sk_callback_lock` is consistent everywhere I looked, and lockdep agreed. Document it at the top of the
    file.
- **`io_opaque_ready()`:** KEEP publication builds the bvec array at completion time.
  `-ENOMEM` there loses the consumed bytes (M4).
- **`io_opaque_prep()`:**
  - It sets `req->file = get_file(stream->file)` on a `needs_file = 0` opcode so that
    cancel-by-fd works. That's clever, but surprising; document it, or make cancel match
    on the stream token instead.
  - The `fd == -1` requirement is unusual for io_uring; unused fields are normally 0.
- **`io_opaque_cancel()`** only cancels the first match. That's fine with the generic
  `CANCEL_ALL` loop, but `io_opaque_cancel_all()` running from `io_uring_try_cancel_requests()`
  takes every stream mutex under `uring_lock`. That's OK, but note it for v2's lock
  documentation.
- **`IORING_OP_OPAQUE_OBJ` with `IOSQE_ASYNC`** runs the whole issue path in io-wq,
  including `copy_to_user()` for READ and compaction start. That works, but it isn't
  covered by any test.

### 3/5 io_uring: compact opaque objects

- **Fold the fix back.** The copy-path charging change in this patch (charge before
  `alloc_page()`, uncharge on failure) fixes code introduced in 2/5. Squash it into 2/5.
- **Compaction itself looks correct:**
  - reserve-then-allocate
  - `op->reserved` bookkeeping
  - persistent cursor
  - publish-if-unchanged under `tables`
  - the old version staying alive for in-flight readers and senders

  KUnit covers the arithmetic. Nice.
- **`mod_delayed_work(..., 1)` on every KEEP** with auto-compaction on (`opaque.c:1154`).
  At high KEEP rates that keeps pushing the timer out. It's also a strong hint that
  automatic compaction should be userspace policy (§4.4).
- **`io_opaque_auto_work()`** uses an on-stack `struct io_opaque_req` with only `store`,
  `data` and `handle` set. That works today, but it's fragile: any helper that starts
  touching `op->req` will crash. Use a dedicated small struct.
- **`saving`** uses `data->charge`, which double-counts shared backings (the doc admits
  this), so the policy can pick objects whose compaction frees nothing.

### 4/5 tests

- **Test placement.** io_uring feature tests normally live in liburing's test suite;
  that's where CI runs and where we'll look. In-tree kselftests exist only for the
  net-facing zcrx/zerocopy bits. Expect a request for liburing helpers and tests. If you
  keep a kselftest, drop `run_opaque_vm.sh`: it's a QEMU plus BusyBox initramfs launcher,
  and virtme-ng and the kselftest runners already cover this. Note that the test passes
  under UML without it.
- **Missing coverage.** Add a regression test for each case in §2: long streams (more
  than 128 cycles, more than 32 MiB), RST mid-object, `CQE_MIXED`, throttled receivers
  with two queued sends, interleaved fragments, `close()` of the collected fd, and
  `IOSQE_ASYNC` on OPAQUE_OBJ.
- **Further coverage worth adding:**
  - A shared store with collectors in two rings, one ring exiting mid-KEEP
  - IPv6
  - `max_requests` exhaustion on the collector path
- **KUnit:** `opaque_test.c` and `opaque_compact.c` are `#include`d into `opaque.c`.
  Separate compilation units with `EXPORT_SYMBOL_IF_KUNIT` / `VISIBLE_IF_KUNIT` are the
  current convention.

### 5/5 docs

- **Move motivation out of the ABI doc.** "Claims and comparison boundaries" is cover
  letter material; the ABI document should describe semantics only.
- **Corrections once the bugs above are fixed:**
  - `CQE_MIXED` support (C)
  - "continues until the range is queued" (D)
  - "publish an object only when the entire range ... is complete" (B)
  - the M1 lifetime rule
  - "SQE awaiting preparation" (H3)
- **Permissions section:** State that registration needs no `CAP_NET_ADMIN`, and say
  what the unprivileged attack surface is. About 2k lines become reachable by any user
  `io_uring_disabled` allows.

---

## 6. Process notes

- **Send as an RFC with a cover letter** that gives motivation, numbers, and the
  alternatives in §4.5.
- **Send patch 1 separately to netdev**, CC'ing the TCP maintainers. CC the zcrx
  maintainer on anything touching `zcrx.[ch]` and `RECV_ZC`.
- **`Assisted-by:` tag format.** The tags are there, which is good.
  `Documentation/process/coding-assistants.rst` in this tree documents the format as
  `Assisted-by: LLM [TOOL1] [TOOL2]`; check `Codex:GPT-6` against it. The same document
  expects the human submitter to have reviewed all generated code. A reviewer who hits
  the sign error in B will ask how closely that review was done. Expect closer scrutiny
  on v2.
- **New config default.** `default y` for `IO_URING_OPAQUE_OBJ` should be `default n`.

---

## 7. What's good

- **Immutable kernel-owned payload** with one completion per send and no SEND_ZC
  notification. That's a real simplification for applications, and the reasoning holds.
- **Generation handles** that retire slots instead of wrapping. Stable handles across
  compaction, with old versions kept alive for in-flight users.
- **Page-retention safety rules** that follow splice's, with copy fallbacks for cloned,
  shared, shared-frag and unreadable skbs. KASAN, lockdep and atomic-sleep were clean
  across every run I did, including the failure cases.
- **Ordered, non-overlapping decision tree.** O(log n) insertion with the right overlap
  argument.
- **Clean build hygiene.** Every commit builds; it's clean on 64-bit and 32-bit, at
  `W=1` and with `CONFIG=n`; checkpatch is clean.
- **Tests at two levels.** A raw-ABI selftest with no liburing dependency, plus KUnit for
  the internals.
- **Documentation that separates what the code supports from what still needs measuring.**
  Keep that discipline in the cover letter.

---

## 8. Suggested path to v2

1. **Fix A–D, H1 and H3 now.** Appendix A has verified minimal fixes for A, B, C and D,
   and each one needs a regression test.
2. **Decide on the net hook** (§4.3). Get netdev's view first, or drop it.
3. **Lift the store out of zcrx** (§4.1). Rework the collector as a standard multishot
   whose stream outlives it, and use links for ordering (§4.2).
4. **Rework accounting** for guaranteed progress and charge sharing (H2). Return the
   KEEP prefix to the stream on cancel (M4).
5. **Shrink the UAPI** (§4.4). Ship manual COMPACT later; drop automatic compaction.
6. **Evaluate the alternatives** in §4.5, especially kernel bvec registered buffers.
   Bring numbers.
7. **Write liburing helpers and tests.**

---

## Appendix A: prototype fixes for A, B, C and D (verified under UML)

This is a direction check, not a submission-quality patch. It applies on top of
`ce7be6c326f8`. With it, all reproducers pass except E, F and G (design issues), the
selftest is 21/21 and KUnit is 11/11, with KASAN, lockdep and atomic-sleep clean.
It does **not** implement the "fail queued followers" part of D, or H1–H3.

```diff
diff --git a/io_uring/net.c b/io_uring/net.c
index db1107b2f804..5573d2778697 100644
--- a/io_uring/net.c
+++ b/io_uring/net.c
@@ -1317,6 +1317,7 @@ int io_recvzc_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
 	if (zc->ifq->opaque) {
 		if (zc->len || (req->flags & (REQ_F_CQE_SKIP | REQ_F_FORCE_ASYNC)))
 			return -EINVAL;
+		req->flags |= REQ_F_APOLL_MULTISHOT;
 		return io_opaque_recv_prep(req, zc->ifq->opaque);
 	}
 	/* All data completions are posted as aux CQEs. */
diff --git a/io_uring/opaque.c b/io_uring/opaque.c
index 930d4747329f..ce80a2805f80 100644
--- a/io_uring/opaque.c
+++ b/io_uring/opaque.c
@@ -81,6 +81,7 @@ enum io_opaque_phase {
 	IO_OPAQUE_IDLE,
 	IO_OPAQUE_RANGE_WAIT,
 	IO_OPAQUE_BUDGET_WAIT,
+	IO_OPAQUE_BUDGET_POLL,
 	IO_OPAQUE_SEND_WAIT,
 	IO_OPAQUE_COPY_WORK,
 	IO_OPAQUE_QUEUED,
@@ -181,6 +182,13 @@ static void io_opaque_wake_budget(struct io_opaque_store *store)
 	guard(spinlock_irqsave)(&store->wait_lock);
 	list_for_each_entry_safe(op, next, &store->budget_waits, wait) {
 		list_del_init(&op->wait);
+		if (op->phase == IO_OPAQUE_BUDGET_POLL) {
+			/* Still armed in multishot poll: re-trigger it. */
+			op->phase = IO_OPAQUE_IDLE;
+			wake_up_interruptible_poll(sk_sleep(sock_from_file(op->req->file)->sk),
+						   EPOLLIN);
+			continue;
+		}
 		op->phase = IO_OPAQUE_QUEUED;
 		op->req->io_task_work.func = io_opaque_resume;
 		io_req_task_work_add(op->req);
@@ -996,7 +1004,7 @@ static int io_opaque_attach(struct io_opaque_req *op)
 		slot->generation++;
 		stream->token = (u64)slot->generation << 32 | (i + 1);
 		cqe[0].user_data = op->req->cqe.user_data;
-		cqe[0].flags = IORING_CQE_F_MORE;
+		cqe[0].flags = IORING_CQE_F_MORE | ctx_cqe32_flags(op->req->ctx);
 		memcpy(&cqe[1], &stream->token, sizeof(stream->token));
 		if (!io_req_post_cqe32(op->req, cqe)) {
 			write_unlock_bh(&sock->sk->sk_callback_lock);
@@ -1055,7 +1063,7 @@ int io_opaque_recv(struct io_kiocb *req, unsigned int issue_flags)
 	else
 		ret = tcp_read_sock(sock->sk, &desc, io_opaque_actor);
 	if (sock->sk->sk_err)
-		stream->error = -sock_error(sock->sk);
+		stream->error = sock_error(sock->sk);
 	if ((sock->sk->sk_shutdown & RCV_SHUTDOWN) &&
 	    skb_queue_empty(&sock->sk->sk_receive_queue))
 		stream->eof = true;
@@ -1066,6 +1074,19 @@ int io_opaque_recv(struct io_kiocb *req, unsigned int issue_flags)
 	io_opaque_stream_process(stream);
 	if (stream->error || stream->eof) {
 		ret = stream->error;
+	} else if (ret == -ENOBUFS && (issue_flags & IO_URING_F_MULTISHOT)) {
+		/* Stay armed; io_poll_issue() must not see IOU_ISSUE_SKIP_COMPLETE. */
+		scoped_guard(spinlock_irqsave, &op->store->wait_lock) {
+			if (list_empty(&op->wait)) {
+				op->phase = IO_OPAQUE_BUDGET_POLL;
+				list_add_tail(&op->wait, &op->store->budget_waits);
+			}
+		}
+		if (atomic64_read(&op->store->bytes) + stream->need_bytes <=
+		    op->store->config.hard_limit - op->store->config.compact_headroom &&
+		    atomic_read(&op->store->extents) < op->store->config.max_extents - 2)
+			io_opaque_wake_budget(op->store);
+		ret = IOU_RETRY;
 	} else if (ret == -ENOBUFS) {
 		io_opaque_wait(op, IO_OPAQUE_BUDGET_WAIT);
 		/* Recheck after enrollment to close the release/enrollment race. */
@@ -1074,8 +1095,11 @@ int io_opaque_recv(struct io_kiocb *req, unsigned int issue_flags)
 		    atomic_read(&op->store->extents) < op->store->config.max_extents - 2)
 			io_opaque_wake_budget(op->store);
 		ret = IOU_ISSUE_SKIP_COMPLETE;
+	} else if (!desc.count && (issue_flags & IO_URING_F_MULTISHOT)) {
+		/* Batch cap hit with data left: multishot poll is edge triggered. */
+		ret = IOU_REQUEUE;
 	} else {
-		ret = -EAGAIN;
+		ret = IOU_RETRY;
 	}
 unlock_stream:
 	mutex_unlock(&stream->lock);
@@ -1456,7 +1480,7 @@ static bool io_opaque_cancel_req(struct io_opaque_req *op)
 		mutex_lock(&op->stream->lock);
 	spin_lock_irqsave(&op->store->wait_lock, flags);
 	phase = op->phase;
-	if (op->canceled || phase == IO_OPAQUE_IDLE) {
+	if (op->canceled || phase == IO_OPAQUE_IDLE || phase == IO_OPAQUE_BUDGET_POLL) {
 		spin_unlock_irqrestore(&op->store->wait_lock, flags);
 		if (op->stream)
 			mutex_unlock(&op->stream->lock);
@@ -1668,11 +1692,14 @@ int io_opaque_send(struct io_kiocb *req, unsigned int issue_flags)
 	struct io_opaque_req *op = req->async_data;
 	struct socket *sock = sock_from_file(req->file);
 	struct msghdr msg = {
-		.msg_flags = MSG_SPLICE_PAGES | MSG_DONTWAIT | MSG_NOSIGNAL | op->msg_flags,
+		.msg_flags = MSG_SPLICE_PAGES | MSG_NOSIGNAL | op->msg_flags,
 		.msg_iter = op->iter,
 	};
 	int ret;
 
+	if (issue_flags & IO_URING_F_NONBLOCK)
+		msg.msg_flags |= MSG_DONTWAIT;
+
 	io_ring_submit_lock(req->ctx, issue_flags);
 	if (!sock || sock->type != SOCK_STREAM || sock->sk->sk_protocol != IPPROTO_TCP) {
 		ret = -EOPNOTSUPP;
@@ -1685,7 +1712,10 @@ int io_opaque_send(struct io_kiocb *req, unsigned int issue_flags)
 	ret = io_opaque_tx_enter(op, sock->sk);
 	if (ret)
 		goto out;
+	/* Only drops the lock for io-wq, where the send may block. */
+	io_ring_submit_unlock(req->ctx, issue_flags);
 	ret = sock_sendmsg(sock, &msg);
+	io_ring_submit_lock(req->ctx, issue_flags);
 	if (ret > 0) {
 		op->iter = msg.msg_iter;
 		op->progress += ret;
```

## Appendix B: reproducer

Build helpers from the series' own selftest, then the reproducer, against the patched
UAPI headers:

```sh
sed -n '1,21p;23,305p' tools/testing/selftests/io_uring/opaque_obj.c \
  | sed 's/ksft_exit_fail_msg(\(.*\));/{ fprintf(stderr, \1); exit(1); }/' > helpers.h
make ARCH=x86_64 O=$HDR headers_install INSTALL_HDR_PATH=$HDR/usr
gcc -O2 -Wall -static -pthread -I$HDR/usr/include -o repro repro.c
./repro            # all cases; or e.g. ./repro BCD
```

UML recipe (no root/KVM needed):

```sh
make ARCH=um O=$B x86_64_defconfig
scripts/config --file $B/.config -e VETH -e IO_URING -e KUNIT -e IO_URING_OPAQUE_OBJ \
  -e IO_URING_OPAQUE_OBJ_KUNIT_TEST -e KASAN -e PROVE_LOCKING -e DEBUG_ATOMIC_SLEEP
make ARCH=um O=$B olddefconfig && make ARCH=um O=$B -j32
$B/linux mem=1536M rootfstype=hostfs rootflags=$ROOT rw init=/init con=null con0=null,fd:1
```

`$ROOT` holds static busybox, an `/init` that does `ip link set lo up` and runs the test
binaries.

<details><summary>repro.c</summary>

```c
// SPDX-License-Identifier: GPL-2.0
/* Review reproducers for the io_uring OPAQUE_OBJ RFC (origin/upstream/opaque-obj). */
#define _GNU_SOURCE
#include <stdio.h>
#include <poll.h>
#include <time.h>
#include "helpers.h"

#define COLLECTOR_TAG 9000

static struct io_uring_opaque_config default_cfg(void)
{
	struct io_uring_opaque_config cfg = {
		.hard_limit = 64U << 20,
		.compact_headroom = 0,
		.max_object_size = 64U << 20,
		.max_objects = 128,
		.max_streams = 16,
		.max_extents = 65536,
		.max_requests = 256,
	};
	return cfg;
}

/* Wait for a tag with a timeout; returns false on timeout. */
static bool wait_tag_to(struct ring *r, uint64_t tag, int ms, struct event *out)
{
	struct timespec start, now;
	struct event event;
	unsigned int i;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		struct __kernel_timespec ts = { .tv_nsec = 10 * 1000 * 1000 };
		struct io_uring_getevents_arg arg = { .ts = (uintptr_t)&ts };
		unsigned int pending = *r->sq_tail - *r->sq_head;

		for (i = 0; i < r->nr_saved; i++) {
			if (r->saved[i].tag != tag)
				continue;
			*out = r->saved[i];
			r->saved[i] = r->saved[--r->nr_saved];
			return true;
		}
		syscall(__NR_io_uring_enter, r->fd, pending, 1,
			IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg, sizeof(arg));
		while (peek(r, &event)) {
			if (event.tag == tag) {
				*out = event;
				return true;
			}
			if (r->nr_saved < 64)
				r->saved[r->nr_saved++] = event;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		if ((now.tv_sec - start.tv_sec) * 1000 +
		    (now.tv_nsec - start.tv_nsec) / 1000000 > ms)
			return false;
	}
}

static bool take_saved(struct ring *r, uint64_t tag, struct event *out)
{
	unsigned int i;

	for (i = 0; i < r->nr_saved; i++) {
		if (r->saved[i].tag != tag)
			continue;
		*out = r->saved[i];
		r->saved[i] = r->saved[--r->nr_saved];
		return true;
	}
	return false;
}

struct burst_writer {
	int fd;
	size_t burst;
	unsigned int bursts;
	unsigned int sleep_us;
};

static void *burst_writer(void *arg)
{
	struct burst_writer *w = arg;
	unsigned char *buf = calloc(1, w->burst);
	unsigned int i;

	for (i = 0; i < w->bursts; i++) {
		size_t done = 0;

		while (done < w->burst) {
			ssize_t n = send(w->fd, buf + done, w->burst - done, MSG_NOSIGNAL);

			if (n <= 0)
				goto out;
			done += n;
		}
		if (w->sleep_us)
			usleep(w->sleep_us);
	}
out:
	free(buf);
	return NULL;
}

/*
 * A: the collector is re-armed with a oneshot apoll on every -EAGAIN because
 * REQ_F_APOLL_MULTISHOT is never set for the opaque RECV_ZC path. After
 * APOLL_MAX_RETRY (128) arms the request is punted to io-wq, where a
 * nonblocking -EAGAIN is final, so the collector dies and cleanup closes the
 * stream (dropping undecided bytes and canceling pending ranges).
 */
static void test_collector_rearm(const char *name, size_t burst, unsigned int bursts,
				 unsigned int sleep_us)
{
	struct io_uring_opaque_config cfg = default_cfg();
	struct burst_writer w = { .burst = burst, .bursts = bursts, .sleep_us = sleep_us };
	struct event ev, col;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, discard;
	unsigned long long total = (unsigned long long)burst * bursts;
	pthread_t thr;
	int pair[2];
	bool got;

	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(pair);
	stream = attach(&r, ctx, pair[1], COLLECTOR_TAG);
	discard = cmd_stage(&r, ctx, IORING_OPAQUE_DISCARD, stream, 0, total, NULL);
	enter(&r, 0);
	w.fd = pair[0];
	require(!pthread_create(&thr, NULL, burst_writer, &w), "writer");
	got = wait_tag_to(&r, discard, 60000, &ev);
	shutdown(pair[0], SHUT_RDWR);
	pthread_join(thr, NULL);
	printf("[A:%s] DISCARD of %llu bytes (%u bursts of %zu): %s res=%d\n", name, total,
	       bursts, burst, got ? "completed" : "TIMEOUT", got ? ev.res : 0);
	if (take_saved(&r, COLLECTOR_TAG, &col) || wait_tag_to(&r, COLLECTOR_TAG, 200, &col))
		printf("[A:%s] collector terminated: res=%d flags=0x%x\n", name, col.res, col.flags);
	else
		printf("[A:%s] collector still armed\n", name);
	printf("[A:%s] RESULT: %s\n", name,
	       got && ev.res == (int)total ? "ok" : "BUG: collector died before stream end");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
}

/*
 * B: io_opaque_recv() stores -sock_error(sk), i.e. a positive errno.
 * Pending ranges then complete as success with op->length.
 */
static void test_reset_publishes_truncated(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	struct io_uring_opaque_object_stat ostat;
	struct linger lg = { .l_onoff = 1, .l_linger = 0 };
	unsigned char buf[4096] = { 0 };
	struct event ev, col;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, keep, rd;
	int pair[2];

	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(pair);
	stream = attach(&r, ctx, pair[1], COLLECTOR_TAG);
	keep = cmd_stage(&r, ctx, IORING_OPAQUE_KEEP, stream, 0, 4096, NULL);
	memset(buf, 0xAB, 4096);
	rd = cmd_stage(&r, ctx, IORING_OPAQUE_READ_STREAM, stream, 4096, 64, buf);
	enter(&r, 0);
	write_all(pair[0], buf, 1000);
	usleep(50000);
	enter(&r, 0);
	require(!setsockopt(pair[0], SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)), "linger");
	close(pair[0]);
	require(wait_tag_to(&r, keep, 5000, &ev), "KEEP completion after RST");
	printf("[B] KEEP(0, 4096) after 1000 bytes + RST: res=%d handle=0x%llx\n", ev.res,
	       (unsigned long long)ev.extra[0]);
	if (ev.res > 0) {
		struct event st = command(&r, ctx, IORING_OPAQUE_OBJECT_STAT, ev.extra[0], 0,
					  sizeof(ostat), &ostat);

		printf("[B] OBJECT_STAT res=%d length=%llu (KEEP reported %d)\n", st.res,
		       (unsigned long long)ostat.length, ev.res);
	}
	if (wait_tag_to(&r, rd, 2000, &ev))
		printf("[B] READ_STREAM(4096, 64) after RST: res=%d (buffer untouched: %s)\n",
		       ev.res, buf[0] == 0xAB ? "yes" : "no");
	if (wait_tag_to(&r, COLLECTOR_TAG, 2000, &col))
		printf("[B] collector terminal CQE res=%d (positive errno => sign bug)\n", col.res);
	printf("[B] RESULT: %s\n", ev.res > 0 || col.res > 0 ?
	       "BUG: reset reported as success / positive errno" : "ok");
	ring_exit(&r);
	close(pair[1]);
}

/* C: the stream-token CQE in a CQE_MIXED ring lacks IORING_CQE_F_32. */
static void test_cqe_mixed_token(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	struct io_uring_params p = { .flags = IORING_SETUP_SINGLE_ISSUER |
				     IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CQE_MIXED };
	struct io_uring_sqe *sqes, sqe = { .opcode = IORING_OP_RECV_ZC,
					   .ioprio = IORING_RECV_MULTISHOT,
					   .user_data = COLLECTOR_TAG };
	struct io_uring_cqe *cqes;
	unsigned int *sq_tail, *sq_array, *cq_head, *cq_tail, head, tail, i;
	size_t sq_size, cq_size;
	uint32_t ctx;
	void *sq, *cq;
	int fd, pair[2], ret;
	struct io_uring_zcrx_ifq_reg reg = { .flags = ZCRX_REG_OPAQUE_OBJ,
					     .opaque_config = (uintptr_t)&cfg };

	fd = syscall(__NR_io_uring_setup, 16, &p);
	if (fd < 0) {
		printf("[C] CQE_MIXED unsupported: %s\n", strerror(errno));
		return;
	}
	sq_size = p.sq_off.array + p.sq_entries * sizeof(unsigned int);
	cq_size = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
	if (p.features & IORING_FEAT_SINGLE_MMAP)
		sq_size = cq_size = sq_size > cq_size ? sq_size : cq_size;
	sq = mmap(NULL, sq_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IORING_OFF_SQ_RING);
	cq = (p.features & IORING_FEAT_SINGLE_MMAP) ? sq :
	     mmap(NULL, cq_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IORING_OFF_CQ_RING);
	sqes = mmap(NULL, p.sq_entries * sizeof(*sqes), PROT_READ | PROT_WRITE, MAP_SHARED,
		    fd, IORING_OFF_SQES);
	require(sq != MAP_FAILED && cq != MAP_FAILED && sqes != MAP_FAILED, "mixed mmap");
	sq_tail = sq + p.sq_off.tail;
	sq_array = sq + p.sq_off.array;
	cq_head = cq + p.cq_off.head;
	cq_tail = cq + p.cq_off.tail;
	cqes = cq + p.cq_off.cqes;
	ret = syscall(__NR_io_uring_register, fd, IORING_REGISTER_ZCRX_IFQ, &reg, 1);
	require(ret == 0, "mixed store");
	ctx = reg.zcrx_id;
	tcp_pair(pair);
	sqe.fd = pair[1];
	sqe.zcrx_ifq_idx = ctx;
	sqes[0] = sqe;
	sq_array[0] = 0;
	__atomic_store_n(sq_tail, 1, __ATOMIC_RELEASE);
	ret = syscall(__NR_io_uring_enter, fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
	require(ret >= 0, "mixed enter");
	head = *cq_head;
	tail = __atomic_load_n(cq_tail, __ATOMIC_ACQUIRE);
	printf("[C] CQE_MIXED ring: %u 16-byte CQ slots posted for the attach\n", tail - head);
	for (i = head; i != tail; ) {
		struct io_uring_cqe *c = &cqes[i & (p.cq_entries - 1)];

		printf("[C]   slot %u: user_data=0x%llx res=%d flags=0x%x%s\n", i - head,
		       (unsigned long long)c->user_data, c->res, c->flags,
		       (c->flags & IORING_CQE_F_32) ? " (F_32)" : "");
		i += (c->flags & IORING_CQE_F_32) ? 2 : 1;
	}
	printf("[C] RESULT: %s\n", (tail - head) == 2 &&
	       !(cqes[head & (p.cq_entries - 1)].flags & IORING_CQE_F_32) ?
	       "BUG: 32-byte token CQE posted without IORING_CQE_F_32 (phantom 2nd CQE)" : "ok");
	close(fd);
	close(pair[0]);
	close(pair[1]);
}

static void tcp_pair_small(int pair[2], int sndbuf, int rcvbuf)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	socklen_t len = sizeof(addr);
	int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;

	require(listener >= 0, "listen socket");
	require(!setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)), "rcvbuf");
	require(!bind(listener, (void *)&addr, sizeof(addr)), "bind");
	require(!getsockname(listener, (void *)&addr, &len), "getsockname");
	require(!listen(listener, 1), "listen");
	pair[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	require(!setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)), "sndbuf");
	require(!connect(pair[0], (void *)&addr, sizeof(addr)), "connect");
	pair[1] = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
	require(pair[1] >= 0, "accept");
	require(!setsockopt(pair[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)), "nodelay");
	close(listener);
}

struct slow_reader {
	int fd;
	size_t total, done;
	size_t chunk;
	unsigned int sleep_us;
};

static void *slow_reader(void *arg)
{
	struct slow_reader *s = arg;
	unsigned char buf[65536];

	while (s->done < s->total) {
		ssize_t n = read(s->fd, buf, s->chunk);

		if (n <= 0)
			break;
		s->done += n;
		usleep(s->sleep_us);
	}
	return NULL;
}

/* D: SEND without MSG_DONTWAIT completes short after ~128 POLLOUT re-arms. */
static void test_send_short(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	size_t length = 2U << 20;
	unsigned char *data = calloc(1, length);
	struct transfer tx = { .data = data, .length = length };
	struct slow_reader sr = { .total = length, .chunk = 4096, .sleep_us = 500 };
	struct event ev;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, keep, send, handle;
	int src[2], dst[2];
	pthread_t prod, cons;

	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(src);
	tcp_pair_small(dst, 4096, 4096);
	stream = attach(&r, ctx, src[1], COLLECTOR_TAG);
	keep = cmd_stage(&r, ctx, IORING_OPAQUE_KEEP, stream, 0, length, NULL);
	tx.fd = src[0];
	require(!pthread_create(&prod, NULL, writer, &tx), "producer");
	require(wait_tag_to(&r, keep, 30000, &ev) && ev.res == (int)length, "KEEP 2MB");
	handle = ev.extra[0];
	pthread_join(prod, NULL);
	sr.fd = dst[1];
	send = send_stage(&r, ctx, dst[0], handle, 0, length);
	require(!pthread_create(&cons, NULL, slow_reader, &sr), "consumer");
	require(wait_tag_to(&r, send, 120000, &ev), "SEND completion");
	printf("[D] OPAQUE_OBJ_SEND of %zu bytes without MSG_DONTWAIT: res=%d\n", length, ev.res);
	printf("[D] RESULT: %s\n", ev.res == (int)length ? "ok" :
	       "BUG: blocking send completed short / failed (apoll retry cap + io-wq -EAGAIN)");
	shutdown(dst[0], SHUT_WR);
	pthread_join(cons, NULL);
	ring_exit(&r);
	close(src[0]); close(src[1]); close(dst[0]); close(dst[1]);
	free(data);
}


struct collect_reader {
	int fd;
	unsigned char *buf;
	size_t cap, done;
	unsigned int sleep_us;
};

static void *collect_reader(void *arg)
{
	struct collect_reader *c = arg;

	while (c->done < c->cap) {
		ssize_t n = read(c->fd, c->buf + c->done,
				 c->cap - c->done > 4096 ? 4096 : c->cap - c->done);

		if (n <= 0)
			break;
		c->done += n;
		usleep(c->sleep_us);
	}
	return NULL;
}

/*
 * D2: two queued object sends to one destination. If the first (blocking)
 * send completes short, the FIFO releases the second, which is transmitted
 * before the first object's remainder: the destination byte stream is spliced.
 */
static void test_send_fifo_corruption(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	size_t xlen = 2U << 20, ylen = 64U << 10, total = xlen + ylen, i, first_b = 0;
	unsigned char *src = malloc(total), *seen = calloc(1, total);
	struct transfer tx = { .data = src, .length = total };
	struct collect_reader cr = { .buf = seen, .cap = total, .sleep_us = 500 };
	struct event ev, sx, sy;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, kx, ky, hx, hy, tx_x, tx_y;
	int s[2], d[2];
	pthread_t prod, cons;

	memset(src, 'A', xlen);
	memset(src + xlen, 'B', ylen);
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(s);
	tcp_pair_small(d, 4096, 4096);
	stream = attach(&r, ctx, s[1], COLLECTOR_TAG);
	kx = cmd_stage(&r, ctx, IORING_OPAQUE_KEEP, stream, 0, xlen, NULL);
	ky = cmd_stage(&r, ctx, IORING_OPAQUE_KEEP, stream, xlen, ylen, NULL);
	tx.fd = s[0];
	require(!pthread_create(&prod, NULL, writer, &tx), "producer");
	require(wait_tag_to(&r, kx, 30000, &ev) && ev.res == (int)xlen, "KEEP X");
	hx = ev.extra[0];
	require(wait_tag_to(&r, ky, 30000, &ev) && ev.res == (int)ylen, "KEEP Y");
	hy = ev.extra[0];
	pthread_join(prod, NULL);
	cr.fd = d[1];
	tx_x = send_stage(&r, ctx, d[0], hx, 0, xlen);
	tx_y = send_stage(&r, ctx, d[0], hy, 0, ylen);
	require(!pthread_create(&cons, NULL, collect_reader, &cr), "consumer");
	require(wait_tag_to(&r, tx_x, 120000, &sx), "SEND X");
	require(wait_tag_to(&r, tx_y, 120000, &sy), "SEND Y");
	shutdown(d[0], SHUT_WR);
	pthread_join(cons, NULL);
	for (i = 0; i < cr.done; i++)
		if (seen[i] == 'B') {
			first_b = i;
			break;
		}
	printf("[D2] SEND X(%zu 'A') res=%d, SEND Y(%zu 'B') res=%d; peer got %zu bytes, first 'B' at %zu\n",
	       xlen, sx.res, ylen, sy.res, cr.done, first_b);
	printf("[D2] RESULT: %s\n", first_b == xlen && cr.done == total ? "ok" :
	       "BUG: destination stream spliced (Y sent before the rest of X)");
	ring_exit(&r);
	close(s[0]); close(s[1]); close(d[0]); close(d[1]);
	free(src);
	free(seen);
}

/* E: handles are resolved at prep, so link order does not order FREE vs READ/SEND. */
static void test_link_order(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	unsigned char buf[8];
	struct event ev, fr, rd;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, keep, free_tag, read_tag, handle;
	unsigned int index;
	int pair[2];

	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(pair);
	stream = attach(&r, ctx, pair[1], COLLECTOR_TAG);
	keep = cmd_stage(&r, ctx, IORING_OPAQUE_KEEP, stream, 0, 8, NULL);
	write_all(pair[0], "abcdefgh", 8);
	ev = wait_tag(&r, keep);
	require(ev.res == 8, "keep");
	handle = ev.extra[0];
	free_tag = cmd_stage(&r, ctx, IORING_OPAQUE_FREE, handle, 0, 0, NULL);
	index = (*r.sq_tail - 1) & *r.sq_mask;
	r.sqes[index].flags |= IOSQE_IO_LINK;
	read_tag = cmd_stage(&r, ctx, IORING_OPAQUE_READ, handle, 0, 8, buf);
	fr = wait_tag(&r, free_tag);
	rd = wait_tag(&r, read_tag);
	printf("[E] FREE(h) -> IOSQE_IO_LINK -> READ(h): FREE res=%d, READ res=%d\n",
	       fr.res, rd.res);
	printf("[E] RESULT: %s\n", fr.res == 0 && rd.res == 8 ?
	       "SEMANTIC: READ linked after a successful FREE still succeeds (prep-time pinning)" :
	       "link order respected");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
}

struct interleave {
	int a, b;
	unsigned int rounds;
	size_t chunk;
};

static void *interleave_writer(void *arg)
{
	struct interleave *iw = arg;
	unsigned char buf[4096] = { 0x5a };
	unsigned int i;

	for (i = 0; i < iw->rounds; i++) {
		write_all(iw->a, buf, iw->chunk);
		write_all(iw->b, buf, iw->chunk);
		usleep(1000);
	}
	return NULL;
}

/*
 * F: every captured extent charges the full compound page. Interleaved writes
 * from one task place non-adjacent fragments of one stream in the same
 * task_frag page, so a small KEEP can exhaust a much larger limit and stall.
 */
static void test_overcharge_stall(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	struct interleave iw = { .rounds = 32, .chunk = 1024 };
	struct io_uring_opaque_stat stat;
	struct __kernel_timespec timeout = { .tv_sec = 3 };
	struct io_uring_sqe lt = { .opcode = IORING_OP_LINK_TIMEOUT, .fd = -1,
				   .addr = (uintptr_t)&timeout, .len = 1 };
	struct event ev;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, keep;
	unsigned int index;
	int a[2], b[2];
	pthread_t thr;

	cfg.hard_limit = 256U << 10;
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(a);
	tcp_pair(b);
	stream = attach(&r, ctx, a[1], COLLECTOR_TAG);
	keep = cmd_stage(&r, ctx, IORING_OPAQUE_KEEP, stream, 0, iw.rounds * iw.chunk, NULL);
	index = (*r.sq_tail - 1) & *r.sq_mask;
	r.sqes[index].flags |= IOSQE_IO_LINK;
	lt.user_data = next_tag++;
	stage(&r, lt);
	iw.a = a[0];
	iw.b = b[0];
	require(!pthread_create(&thr, NULL, interleave_writer, &iw), "writer");
	pthread_join(thr, NULL);
	{
		struct io_uring_opaque_stream_stat sst;
		struct event tmp;

		/* Let the collector run, then sample while the KEEP is still pending. */
		wait_tag_to(&r, ~0ULL, 1000, &tmp);
		require(!command(&r, ctx, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res, "stat");
		require(!command(&r, ctx, IORING_OPAQUE_STREAM_STAT, stream, 0, sizeof(sst), &sst).res,
			"stream stat");
		printf("[F] mid-stall: stream rx_next=%llu bytes captured; store backing=%llu/%llu copied=%llu extents=%u\n",
		       (unsigned long long)sst.rx_next, (unsigned long long)stat.backing_bytes,
		       (unsigned long long)stat.hard_limit, (unsigned long long)stat.copied_bytes,
		       stat.extents);
	}
	require(wait_tag_to(&r, keep, 10000, &ev), "keep completion");
	printf("[F] KEEP of %u bytes with hard_limit=%llu: res=%d\n",
	       iw.rounds * (unsigned int)iw.chunk, (unsigned long long)cfg.hard_limit, ev.res);
	printf("[F] RESULT: %s\n", ev.res == (int)(iw.rounds * iw.chunk) ? "ok (no stall)" :
	       "BUG: small KEEP stalled; per-extent compound-page charging exhausted the limit");
	ring_exit(&r);
	close(a[0]); close(a[1]); close(b[0]); close(b[1]);
}

/* G: closing the application's socket fd does not release the connection. */
static void test_close_holds_socket(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	struct pollfd pfd;
	struct ring r;
	uint32_t ctx;
	unsigned char byte;
	int pair[2], n;

	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(pair);
	attach(&r, ctx, pair[1], COLLECTOR_TAG);
	close(pair[1]);
	pfd.fd = pair[0];
	pfd.events = POLLIN;
	n = poll(&pfd, 1, 300);
	printf("[G] after close(collected fd), peer sees %s\n",
	       n > 0 && recv(pair[0], &byte, 1, MSG_DONTWAIT) == 0 ? "EOF" : "nothing (no FIN)");
	ring_exit(&r);
	close(pair[0]);
}

static void run(const char *name, void (*fn)(void))
{
	int status;
	pid_t pid = fork();

	if (!pid) {
		alarm(180);
		fn();
		_exit(0);
	}
	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status))
		printf("[%s] test process killed by signal %d (hang/timeout)\n", name, WTERMSIG(status));
	else if (WEXITSTATUS(status))
		printf("[%s] test process exited with %d\n", name, WEXITSTATUS(status));
}

static void test_a1(void) { test_collector_rearm("idle-bursts", 100, 300, 3000); }
static void test_a2(void) { test_collector_rearm("continuous", 1U << 20, 64, 0); }

int main(int argc, char **argv)
{
	const char *only = argc > 1 ? argv[1] : NULL;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (!only || strchr(only, 'A')) {
		run("A1", test_a1);
		run("A2", test_a2);
	}
	if (!only || strchr(only, 'B'))
		run("B", test_reset_publishes_truncated);
	if (!only || strchr(only, 'C'))
		run("C", test_cqe_mixed_token);
	if (!only || strchr(only, 'D')) {
		run("D", test_send_short);
		run("D2", test_send_fifo_corruption);
	}
	if (!only || strchr(only, 'E'))
		run("E", test_link_order);
	if (!only || strchr(only, 'F'))
		run("F", test_overcharge_stall);
	if (!only || strchr(only, 'G'))
		run("G", test_close_holds_socket);
	return 0;
}
```

</details>
