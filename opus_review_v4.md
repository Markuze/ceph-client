# Review: io_uring opaque TCP payload objects, v4

**Series reviewed:** `origin/upstream/opaque-obj-v4` at `c5bcea485987`, 5 patches on top of
`af32da41b032` ("Merge tag 'net-7.3-rc7'"):

| # | Commit | Subject |
|---|--------|---------|
| 1/5 | `4be72a8eb29e` | net: add an optional exclusive TCP receive claim |
| 2/5 | `d8f97d0cc040` | io_uring: add independent opaque TCP object stores and framed I/O |
| 3/5 | `9ab4c9c7cebc` | io_uring: test opaque ownership, available inspection and compaction |
| 4/5 | `ca116a961486` | selftests/io_uring: exercise asynchronous objects and complete frames |
| 5/5 | `c5bcea485987` | docs: describe asynchronous opaque objects and framed-send ownership |

The series is about 7.5k lines, roughly twice v1. This review follows up `opus_review.md`,
which reviewed v1 (`ce7be6c326f8`). §3 maps each v1 finding to its v4 status. All
`file:line` references are to `c5bcea485987`.

As before, this is written from the perspective of an io_uring maintainer reviewing an
RFC. It is not an actual maintainer response.

---

## 1. Verdict

**Much better. Not mergeable yet, for one process reason, three new bugs, and a few
design asks.**

v4 takes the v1 review seriously, and it shows:

- **The store has its own registration and opcode.** No zcrx, no `RECV_ZC` overload, no
  `CAP_NET_ADMIN` reshuffle.
- **The net hook has a net-owned Kconfig symbol that defaults off.**
- **The collector is a real multishot.** It stays armed in poll while waiting for budget,
  and uses `IOU_REQUEUE` when it hits the batch cap.
- **Handles are resolved at issue**, so link order now orders effects.
- **A failed RECV_OBJECT restores its prefix to the stream.**
- **The send queue fails its followers when the head comes up short**, and blocking
  retries run in io-wq without the ring lock.
- **Accounting shares compound backings and packs the copy fallback.**
- **Everything is size-versioned, allocations are cached, and compaction moved to the
  shared workqueue.**

I re-ran my v1 reproducers against the v4 ABI. Every v1 bug is fixed (§2).

What still blocks it:

1. **No `Signed-off-by:` on any of the five patches.** checkpatch reports
   `ERROR:MISSING_SIGN_OFF` on each one, and v1 had them. Nothing can be applied without
   the DCO. `Documentation/process/coding-assistants.rst` puts the sign-off squarely on
   the human submitter.
2. **Three new bugs** (§4), two of them reproduced:
   - **N1 [high, liveness]:** framing is charged to the *capture* allowance. Once inbound
     data fills it, every framed reply and generated-only reply fails `-ENOBUFS`, even
     with free `compact_headroom`. A request/response proxy then can't send the objects
     that would free the store. The selftest asserts this behavior.
   - **N2 [medium, net semantics]:** a rejected `connect(AF_UNSPEC)` still increments
     `sk_disconnects` and sets `SS_DISCONNECTING`. A blocked `send()` on the same socket
     returns short, and later `connect()` calls return `EINVAL` instead of `EISCONN`.
   - **R [high, concurrency, by inspection]:** with a store shared across rings, a
     cancel of a queued SEND can race with the queue head promoting or failing it. Both
     sides then `io_req_task_work_add()` the same request.
3. **Design asks** (§5):
   - Split the framed-send feature out of the first series.
   - Justify the global CQE flag bit.
   - Deal with the store-wide `tables` mutex on every operation.
   - Bring benchmarks.

Appendix A has prototype fixes for N1, N2 and R. The N1 and N2 fixes are verified by the
reproducers. R can't be reproduced on UML, which is uniprocessor, so that fix is build-
and regression-tested only. With all three, KUnit is 23/23 and the selftest is 73/73
after flipping the one assertion that encoded N1.

---

## 2. What I ran

| Check | Result |
|-------|--------|
| Build every commit (`ARCH=um`, KASAN + PROVE_LOCKING + DEBUG_ATOMIC_SLEEP + failslab) | all 5 build, bisectable |
| `W=1` on `io_uring/`, `net/ipv4/tcp.o`, `net/core/sock.o` (UML) and `ARCH=i386` + `HIGHMEM4G` | no warnings; no 64-bit libgcc division helpers |
| `CONFIG_IO_URING_OPAQUE_OBJ=n` (so `SOCK_RX_OWNER=n`) | builds |
| `checkpatch.pl --strict` | **`ERROR:MISSING_SIGN_OFF` on all 5**; otherwise only MAINTAINERS file-path warnings |
| KUnit `io_uring-opaque` | 23/23 |
| `tools/testing/selftests/io_uring/opaque_obj`, UML with cgroup2 memory, debugfs and failslab mounted the way `run_opaque_vm.sh` does | **73/73, no skips, no kernel diagnostics** |
| My v1 reproducers ported to the v4 ABI (Appendix B) | all v1 cases pass |
| New cases N1, N2 | **both fail on `c5bcea4`**; both pass with Appendix A |
| Appendix A + KUnit + selftest | 23/23 and 73/73 (one assertion flipped, see Appendix A); no splats |

Reproducer results on `c5bcea4`:

| Case | Scenario | Result |
|------|----------|--------|
| A1 | collector, 300 bursts × 100 B, 3 ms apart; DISCARD of everything | 30000 bytes discarded, collector alive. **Fixed** (v1 died at about 128 cycles) |
| A2 | collector, 64 MiB continuous | 64 MiB discarded. **Fixed** |
| B | RECV_OBJECT(0, 4096); peer sends 1000 B then RST | RECV_OBJECT `-ECONNRESET`, no handle; collector `-ECONNRESET`. **Fixed** |
| D2 | SEND X (2 MiB) then SEND Y to a slow peer | 2097152 / 65536, in order. **Fixed** |
| D3 | as D2 with X `MSG_DONTWAIT` (short by design) | X = 6144, **Y = `-ECANCELED`**, no Y bytes on the wire. **Fixed** |
| E | FREE(h) → `IOSQE_IO_LINK` → READ_OBJECT(h) | READ = `-ESTALE`. **Fixed** |
| F | 32 KiB RECV_OBJECT, 256 KiB store, one task interleaving 1 KiB writes to two sockets | completes; 110592 bytes charged. **Fixed** (v1 stalled at 8 KiB) |
| N1 | 256 KiB store, 64 KiB headroom; inbound flood fills the 192 KiB capture allowance; then a 4-byte-header SEND_LAST, a generated-only reply, and a body-only SEND_LAST | **framed = `-ENOBUFS`, generated-only = `-ENOBUFS`**, body-only = 8 |
| N2 | blocked 16 MiB `send()` on the claimed socket; `connect(alias, AF_UNSPEC)` → `EBUSY`; peer drains | **`send()` returns 2616960**; next `connect()` returns **`EINVAL`** instead of `EISCONN` |

The `CQE_MIXED` attach (v1 case C) is covered by the series' own `test_mixed_cqe`, and
the fix is visible at `opaque.c:1168`.

---

## 3. Status of the v1 review

| v1 item | v4 status | Evidence |
|---------|-----------|----------|
| A: collector dies after 128 re-arms | **Fixed** | `REQ_F_APOLL_MULTISHOT` at `opaque.c:1108`; `IOU_REQUEUE` at `:1276-1278`; budget wait stays in poll at `:1263-1275`; A1/A2; series covers 10,000 cycles and 4 GiB |
| B: socket-error sign; truncated objects published | **Fixed** | `sock_error()` at `:1242`; publish requires `data->length == op->length` at `:1345`; `io_opaque_copy()` rejects gaps at `:1299-1327` |
| C: `CQE_MIXED` token CQE lacks `F_32` | **Fixed** | `ctx_cqe32_flags()` at `:1168`; `test_mixed_cqe` |
| D: blocking SEND short; FIFO splices followers | **Fixed** | `MSG_DONTWAIT` only for nonblocking issue (`:2070`); lock dropped around `sock_sendmsg()` (`:2094-2096`); `tx->failed` cancels followers (`:2020-2027`). **New race in the same code, R (§4)** |
| H1: `msg_flags` double fetch | **Fixed** | all SQE fields read once with `READ_ONCE()` in all three preps |
| H2: accounting has no progress guarantee | **Mostly fixed** | tail-backing sharing (`:876-888`); packed copy fallback (`:989-1003`); retain-or-copy against the dense remainder (`:890-895`); fail-fast `ENOBUFS` for a private prefix (`:1251-1260`). The fail-fast policy has its own cost (§5.3) |
| H3: handles resolved at prep | **Fixed** | `io_opaque_resolve()` at first issue (`:1469-1520`); SEND acquires at admission (`:1894-1917`); E passes |
| M1: stream holds the socket file | **Documented**; AF_UNSPEC now rejected | doc "Every stream token must be released with STREAM_CLOSE"; `tcp_disconnect()` check, **with side effects (N2)** |
| M2: parked collector deaf to the socket | **Fixed** | stays in multishot poll; `io_opaque_wake_budget()` kicks the socket |
| M3: transient `ENOMEM` sticky | **Fixed** | 50 ms backoff (`:1235-1236`, `:1264-1271`) |
| M4: cancel or timeout loses the KEEP prefix | **Fixed** | `io_opaque_restore()` (`:718-741`) from `io_opaque_ready()` and cleanup. Small window, §6 |
| M5: `SET_POLICY` blocks the ring | **Fixed** | `cancel_delayed_work()` (non-sync), `opaque_compact.c:247` |
| M5: per-store `WQ_MEM_RECLAIM` workqueue | **Fixed** | `system_dfl_wq`, `copy_lock` |
| M5: retained RX pages escape memcg | **Fixed** | `obj_cgroup_charge()` per retained backing (`:133-149`); memcg selftest |
| M5: thundering herd on budget release | **Partly fixed** | bounded batches of 32, but still an O(waiters) scan under `wait_lock` on every uncharge and extent free (§5.4) |
| M5: `POLL_FIRST`, `xchg(bool)`, `irqsave` | **Fixed** | |
| §4.1: don't live inside zcrx/`RECV_ZC` | **Fixed** | `IORING_REGISTER_OPAQUE_STORE`, `IORING_OP_OPAQUE_COLLECT`, `opaque_register.c` |
| §4.2: use io_uring's request model | **Mostly** | real multishot; per-store FIFO kept, but now with link-like failure semantics; private cancel list kept |
| §4.3: net hook layering | **Fixed** | `CONFIG_SOCK_RX_OWNER` in `net/Kconfig`, selected, default off; commit message now says the claim is not for memory safety |
| §4.4: UAPI surface | **Partly** | policy cut to 3 knobs; size-versioned register, config, stats, policy and frame; flags named per struct; 50-bit generations. **But** a framed-send ABI and a global CQE flag were added (§5.1–5.2) |
| §4.5: alternatives | **Discussed, not measured** | doc §"Claims and comparison boundaries" |
| §4.6: scalability | **Partly** | bitmaps, counters, alloc cache, union. Store-wide `tables` mutex remains on every op (§5.4) |
| §4.7: benchmarks | **Not addressed** | |
| Process: Signed-off-by | **Regressed** | missing on all patches |
| Tests in liburing; drop VM runner | Author kept kselftest + `run_opaque_vm.sh` | maintainer preference; not a blocker |

---

## 4. New bugs in v4

### N1. [high] Framing is charged to the capture allowance, so inbound data can block every framed reply

- **Where:**
  - `io_uring/opaque.c:1864-1866`: `charge = roundup_pow_of_two(max(bytes, PAGE_SIZE));
    io_opaque_charge(op->store, charge, true)`.
  - `rx = true` limits the charge to `hard_limit - compact_headroom`, the same pool the
    collector fills with undecided bytes.
- **Reproduced (N1):**
  1. 256 KiB store with 64 KiB `compact_headroom`.
  2. One 8-byte object is published, then the peer keeps sending. The collector fills the
     192 KiB capture allowance with undecided bytes, which is normal TCP backpressure.
  3. A 4-byte-header `SEND_LAST` of the object returns `-ENOBUFS`, and so does a
     generated-only reply. Only a body-only send works.
  4. The 64 KiB of headroom is free the whole time, but framing may not use it.
- **Why it's a liveness bug, not a quota policy:**
  - The doc requires that "a protocol connection must route all its replies, including
    generated-only replies, through one shared store's send queue".
  - In a request/response proxy, every reply has a header. The only non-lossy way to
    release store memory is to send objects with their headers (or FREE them, which
    drops data).
  - Once inbound capture saturates the allowance, the application can neither reply nor
    free anything without discarding protocol data.
  - Capture stops at the limit, so the store stays saturated. With several connections
    sharing a store, one fast client can stall replies on all of them.
- **The test encodes the bug:** `test_frame_quota` asserts this outcome ("framing quota
  failure leaves LAST handle reusable"). It uses a 2-page store, 1 page of headroom and
  1 page captured, and requires `-ENOBUFS`.
- **Fix:** Charge framing outside the capture allowance.
  - Minimal (Appendix A, verified): `io_opaque_charge(store, charge, false)`, so framing
    can use `compact_headroom`.
  - Better: a separate, documented reply reserve, or no store charge for framing at all.
    Framing is already per-request bounded (64 KiB), counted against `max_requests`, and
    allocated with `GFP_KERNEL_ACCOUNT` in the owner's memcg.
  - Keep a test where the *entire* hard limit is exhausted, to cover the pre-admission
    failure path that `test_frame_quota` was really after.

### N2. [medium] A rejected `connect(AF_UNSPEC)` still disturbs the claimed connection

- **Where:** the claim check is inside `tcp_disconnect()` (`net/ipv4/tcp.c:3386`). The
  AF_UNSPEC caller has already done its own bookkeeping (`net/ipv4/af_inet.c:651-655`):

  ```c
  sk->sk_disconnects++;
  err = sk->sk_prot->disconnect(sk, flags);   /* now -EBUSY */
  sock->state = err ? SS_DISCONNECTING : SS_UNCONNECTED;
  ```

- **Effects, both reproduced (N2):**
  - `sk_wait_event()` compares `sk_disconnects` before and after sleeping
    (`include/net/sock.h:1251-1264`). Every task asleep in it on that socket fails with
    `-EPIPE` when it wakes. The repro's blocking `send()` of 16 MiB on the claimed socket
    (TX stays with the application) returned **2616960**. A blocked opaque SEND in io-wq
    on the same socket would equally complete short, and the FIFO would then cancel its
    followers.
  - The connected socket is left in `SS_DISCONNECTING`, so a later `connect()` returns
    **`EINVAL`** instead of `EISCONN`.
- **The doc's promise:** "Disconnecting with `connect(AF_UNSPEC)` is rejected with
  EBUSY". The rejection has to happen before any state changes.
- **Fix (Appendix A, verified):** In `__inet_stream_connect()`, return `-EBUSY` for
  AF_UNSPEC on a claimed socket before `sk_disconnects++`. Keep or drop the
  `tcp_disconnect()` check as a backstop; with the claim held through a file reference,
  no other `tcp_disconnect()` caller can reach a claimed socket today. This is netdev's
  call, but the current placement is wrong.

### R. [high, by inspection] Cancel of a queued SEND can race with queue promotion and add the same task_work twice

- **Where:** `io_opaque_cancel_req()` (`opaque.c:1689-1720`) and `io_opaque_tx_put()`
  (`:2003-2039`).
- **The two sides:**
  - **Cancel** decides under `wait_lock` (`phase == SEND_WAIT`, not yet canceled), sets
    `canceled`, **drops `wait_lock`**, and only then calls `io_opaque_queue_ready()`.
  - **The head's `tx_put()`** promotes or fails the next follower based on
    `next->phase == IO_OPAQUE_SEND_WAIT`:
    - the success path checks under `wait_lock` (`:2030-2035`);
    - the failure path checks without any lock (`:2022-2026`).
- **Interleaving** (follower F in ring B, head H in ring A, one shared store, same
  destination socket; both "competing prepared final sends across rings" and
  export/import make this a supported configuration):
  1. B: `io_opaque_cancel_req(F)` takes `wait_lock`, sees `SEND_WAIT`, sets `canceled`,
     and releases `wait_lock`.
  2. A: H completes. `tx_put()` sees `F->phase == SEND_WAIT`, sets `QUEUED` and
     `func = io_opaque_resume`, and calls `io_req_task_work_add(F)`.
  3. B: `io_opaque_queue_ready(F, -ECANCELED)` sets `func = io_opaque_ready` and calls
     `io_req_task_work_add(F)` **again**.

  Adding the same `io_task_work` node to the llist twice corrupts it, so the request
  completes twice. That is a use-after-free. The failure path has the same window,
  because its `phase` read isn't under the lock.
- **Scope:** Within one ring, both paths run under the same `uring_lock`, so it takes two
  rings. UML is uniprocessor, so I couldn't reproduce it; this needs an SMP stress test
  with linked timeouts on queued followers. RANGE_WAIT operations are safe because cancel
  holds `stream->lock` while removing the decision.
- **Fix (Appendix A, build- and regression-tested):** Claim the completion inside the
  locked section. Cancel sets `phase = QUEUED` before dropping `wait_lock`, and the
  `tx_put()` failure loop checks and claims under `wait_lock`. Then exactly one context
  queues the request.

---

## 5. Design review for v4

### 5.1 Scope: split framed sends out of the first series

v4 adds:

- copied prefix/suffix framing with its own size-versioned struct and a 64 KiB bound;
- generated-only replies;
- `SEND_LAST` ownership transfer;
- a framing quota and two framing counters.

That's real application-protocol machinery inside io_uring's send path, and it is most of
the growth from 3.6k to 7.5k lines.

I understand the motivation. The body is kernel memory, so a user header and the body
can't share one iovec, and the header must not be spliced in between someone else's
bytes. But a reviewer will ask why the first series can't be:

- the store, collector, INSPECT / RECV_OBJECT / DISCARD / READ_OBJECT / FREE, and a
  body-only SEND;
- headers sent as ordinary linked SENDs on the same queue;
- framing as a follow-up once there are numbers showing the saved SQE/CQE matters.

N1 is a direct consequence of that extra machinery.

### 5.2 UAPI

- **`IORING_CQE_F_OPAQUE_CONSUMED` takes generic CQE flag bit 6.** Only bits 6–14 are
  left. There's precedent (`F_NOTIF`, `F_SOCK_NONEMPTY`), but expect pushback for a
  single opcode's state. Every ring using these ops is `CQE32` or `CQE_MIXED` anyway, so
  `SEND_LAST` could post a 32-byte CQE with the consumed indication in `extra1` and leave
  the flag space alone.
- **INSPECT has no minimum length.** It completes with whatever is available, so a
  fixed-size header split across segments takes one SQE/CQE round trip per segment.
  "There is no WAITALL option" is deliberate, but an optional `min` (complete once at
  least N bytes or EOF) would let fixed-size preambles cost one CQE. A msgr2 preamble
  (32 B), for example, would no longer need a loop in userspace.
- **`IORING_OP_OPAQUE_OBJ` still multiplexes 11 controls through `ioprio`.** STAT,
  OBJECT_STAT, STREAM_STAT and SET_POLICY are size-versioned now, but they are still
  SQEs. That's fine for an RFC; expect a request to move pure queries to
  `IORING_REGISTER_QUERY` or the store descriptor.

### 5.3 RECV_OBJECT fails fast under any quota pressure

`io_opaque_recv()` fails the RECV_OBJECT currently being captured with `-ENOBUFS`
whenever the collector hits the budget with a nonzero prefix (`:1251-1260`). That avoids
"a private prefix waiting for itself". But in a shared store, transient pressure from
*other* streams' objects also fails every in-progress object. Each one is restored and
must be retried by the application, which is a retry storm under load.

`stream_process()` already detects "this object can never fit" (`:809-812`). Consider
failing fast only in that case, and otherwise waiting with the prefix held, bounded by
the application's deadline as today.

### 5.4 Scalability

- **One `store->tables` mutex on every operation.** Every handle and token resolution
  takes it (`io_opaque_lookup()` asserts it, `:304`). Every SEND takes it twice
  (`tx_enter()`/`tx_put()`), and so do slot reserve and release. Sharing a store across
  rings and threads is the stated use case, so this is the contention point. An xarray
  with RCU lookup and per-object refcounts, as `io_rsrc_node` does, would remove it from
  the read paths.
- **Budget wakeups scan every waiter on every free.** `io_opaque_wake_budget()` runs on
  every uncharge and every extent free, scanning all budget waiters under `wait_lock`
  (`:40-76`). Freeing a 1000-extent object with W parked collectors costs 1000·W
  iterations. Wake once per data release, not per extent.
- **Framing allocations are oversized.** Each framed send allocates and charges at least
  one page with `kmalloc(roundup_pow_of_two(...))`, up to an order-4 64 KiB block per
  request. A 20-byte HTTP header pins 4 KiB per in-flight send. Use `kmalloc(bytes)` and
  charge `ksize()`, or a small per-store cache.

### 5.5 Net hook

The shape is right now:

- a net-owned symbol;
- default off;
- a clear statement that it enforces the offset contract rather than memory safety;
- AF_UNSPEC covered.

Fix N2, and expect netdev to ask whether the per-iteration check in
`tcp_recvmsg_locked()` (`tcp.c:2724`) can become a static key, or go away given the
`sk_wait_data()` recheck. That's their call.

### 5.6 Benchmarks

Still none. The doc's comparison section is careful about what isn't claimed, and the
measurement plan is right. Nothing about merging changes until those numbers exist.

---

## 6. Smaller notes

- **Restore happens in task work, after the decision is gone.** Paths that remove a
  failed RECV_OBJECT from the decision tree don't restore its prefix at that point:
  cancel (`:1708-1714`), the `ENOBUFS` fail-fast (`:1255-1259`) and `stream_process()`
  (`:815-819`). The restore happens later in `io_opaque_ready()` (`:1382-1387`). Until
  then, INSPECT or DISCARD at those offsets sees a consumed gap (`-ENODATA`). Restore in
  the same `stream->lock` section that removes the decision.
- **`io_opaque_attach()` still holds the store-wide `tables` mutex across `lock_sock()`
  and the CQE post.**
- **The ABI doc still opens with prototype provenance** ("originates from the kpass
  stream/object prototype, revision a436cfe75e2c") **and carries the claims and
  comparison section.** Both belong in the cover letter. There is no cover letter on the
  branch; please send one.
- **`opaque_waits` in `struct io_ring_ctx` isn't under `CONFIG_IO_URING_OPAQUE_OBJ`**,
  unlike the cache and the xarray.

---

## 7. What's good in v4

- **The registration, collector and send rework addresses the v1 blockers at the root**,
  not with patches on top. Collector longevity, reset handling, link ordering, prefix
  restore and FIFO failure semantics are all verified independently above.
- **The ownership-transfer model holds together:**
  - `SEND_LAST` takes the table reference at admission and reports
    `IORING_CQE_F_OPAQUE_CONSUMED` independently of the byte count.
  - Compaction can't resurrect a consumed handle.
  - The tests cover cross-ring competing `SEND_LAST`s.
- **The memcg charging of retained backing closes the v1 escape** and is tested with a
  real cgroup.
- **The fault-injection tests (failslab plus stack filter) for capture and publication
  `ENOMEM` are a good addition.** They ran here and passed.
- **The KASAN, lockdep, atomic-sleep, W=1, i386 and `CONFIG=n` builds and runs are clean.**

---

## 8. Path to v5

1. **Add `Signed-off-by:` to every patch.**
2. **Fix N1, N2 and R** (Appendix A), and add tests:
   - a framed reply under capture saturation;
   - a blocked sender across a rejected AF_UNSPEC;
   - an SMP stress test of cross-ring cancel against head completion and failure, with
     linked timeouts on followers.
3. **Split framed sends into a follow-up series** (§5.1), or justify keeping them with
   numbers.
4. **Replace the CQE flag bit with a 32-byte CQE indication** (§5.2), and consider an
   INSPECT minimum length.
5. **Revisit the fail-fast `ENOBUFS` policy for RECV_OBJECT** (§5.3) **and the
   store-wide mutex** (§5.4).
6. **Bring benchmarks.**

---

## Appendix A: prototype fixes for N1, N2 and R

This is a direction check, not a submission-quality patch. It applies on top of
`c5bcea485987`.

- **N1:** framing may use the non-capture headroom.
- **N2:** AF_UNSPEC is rejected before `__inet_stream_connect()` touches socket state.
- **R:** cancel and the TX queue claim a request's completion under `wait_lock`.

With it, N1 and N2 pass, KUnit is 23/23, the selftest is 73/73 and there are no kernel
diagnostics. One selftest change is required: `test_frame_quota` asserted the N1
behavior, so it now expects the framed `SEND_LAST` to succeed through the headroom. A
replacement test should still exhaust the *entire* hard limit, to cover pre-admission
failure.

R can't be exercised on uniprocessor UML; it needs an SMP stress test.

```diff
diff --git a/io_uring/opaque.c b/io_uring/opaque.c
index 6955a63e1bff..63a382f1ce3b 100644
--- a/io_uring/opaque.c
+++ b/io_uring/opaque.c
@@ -1689,6 +1689,7 @@ int io_opaque_issue(struct io_kiocb *req, unsigned int issue_flags)
 static bool io_opaque_cancel_req(struct io_opaque_req *op)
 {
 	enum io_opaque_phase phase;
+	bool queue;
 
 	if (op->stream)
 		mutex_lock(&op->stream->lock);
@@ -1704,6 +1705,10 @@ static bool io_opaque_cancel_req(struct io_opaque_req *op)
 	/* A queued copy is owned by the shared worker until it reports readiness. */
 	if (phase != IO_OPAQUE_COPY_WORK)
 		list_del_init(&op->wait);
+	/* Claim the completion before another context can queue this request. */
+	queue = phase != IO_OPAQUE_QUEUED && phase != IO_OPAQUE_COPY_WORK;
+	if (queue)
+		op->phase = IO_OPAQUE_QUEUED;
 	spin_unlock(&op->store->wait_lock);
 	if (phase == IO_OPAQUE_RANGE_WAIT) {
 		list_del_init(&op->read);
@@ -1714,7 +1719,7 @@ static bool io_opaque_cancel_req(struct io_opaque_req *op)
 	}
 	if (op->stream)
 		mutex_unlock(&op->stream->lock);
-	if (phase != IO_OPAQUE_QUEUED && phase != IO_OPAQUE_COPY_WORK)
+	if (queue)
 		io_opaque_queue_ready(op, -ECANCELED);
 	return true;
 }
@@ -1862,7 +1867,7 @@ static int io_opaque_frame_import(struct io_opaque_req *op)
 			struct mem_cgroup *old;
 
 			charge = roundup_pow_of_two(max_t(u32, bytes, PAGE_SIZE));
-			if (!io_opaque_charge(op->store, charge, true))
+			if (!io_opaque_charge(op->store, charge, false))
 				return -ENOBUFS;
 			op->frame_charge = charge;
 			atomic64_add(charge, &op->store->framing);
@@ -2020,8 +2025,15 @@ static void io_opaque_tx_put(struct io_opaque_req *op)
 		if (op->progress != op->total_length) {
 			tx->failed = true;
 			list_for_each_entry(next, &tx->requests, send) {
-				WRITE_ONCE(next->canceled, true);
-				if (next->phase == IO_OPAQUE_SEND_WAIT)
+				bool queue;
+
+				scoped_guard(spinlock, &store->wait_lock) {
+					WRITE_ONCE(next->canceled, true);
+					queue = next->phase == IO_OPAQUE_SEND_WAIT;
+					if (queue)
+						next->phase = IO_OPAQUE_QUEUED;
+				}
+				if (queue)
 					io_opaque_queue_ready(next, -ECANCELED);
 			}
 			goto out;
diff --git a/net/ipv4/af_inet.c b/net/ipv4/af_inet.c
index 14ce01092fda..fa261eb782d8 100644
--- a/net/ipv4/af_inet.c
+++ b/net/ipv4/af_inet.c
@@ -649,6 +649,9 @@ int __inet_stream_connect(struct socket *sock, struct sockaddr_unsized *uaddr,
 			return -EINVAL;
 
 		if (uaddr->sa_family == AF_UNSPEC) {
+			/* A claimed receive sequence must not be reset or perturbed. */
+			if (sock_rx_owned(sk))
+				return -EBUSY;
 			sk->sk_disconnects++;
 			err = sk->sk_prot->disconnect(sk, flags);
 			sock->state = err ? SS_DISCONNECTING : SS_UNCONNECTED;
diff --git a/tools/testing/selftests/io_uring/opaque_obj.c b/tools/testing/selftests/io_uring/opaque_obj.c
index 8544b745124c..47bd20d0c50b 100644
--- a/tools/testing/selftests/io_uring/opaque_obj.c
+++ b/tools/testing/selftests/io_uring/opaque_obj.c
@@ -2624,11 +2624,10 @@ static void test_frame_quota(struct io_uring_opaque_config *cfg)
 	handle = event.extra[0];
 	tag = frame_stage(&r, context, target[0], handle, 8, &frame, IORING_OPAQUE_SEND_LAST, 0);
 	event = wait_tag(&r, tag);
-	require(event.res == -ENOBUFS && !(event.flags & IORING_CQE_F_OPAQUE_CONSUMED) &&
-		command(&r, context, IORING_OPAQUE_READ_OBJECT, handle, 0, 8, buf).res == 8 &&
-		!memcmp(buf, "quota123", 8), "frame quota failure preserves ownership");
-	require(!command(&r, context, IORING_OPAQUE_FREE, handle, 0, 0, NULL).res &&
-		!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res &&
+	require(event.res == 9 && (event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
+		"framing uses non-capture headroom when capture is full");
+	read_all(target[1], buf, 8);
+	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res &&
 		!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
 		!stat.backing_bytes && !stat.framing_bytes, "frame quota drains");
 	ring_exit(&r);
```

## Appendix B: v4 reproducer

`helpers.h` is the series' own selftest prologue (lines 1–412 minus the kselftest
include), so the reproducer uses exactly the v4 raw ABI:

```sh
sed -n '1,21p;24,412p' tools/testing/selftests/io_uring/opaque_obj.c \
  | sed 's/ksft_exit_fail_msg(\(.*\));/{ fprintf(stderr, \1); exit(1); }/' > helpers.h
make ARCH=x86_64 O=$HDR headers_install INSTALL_HDR_PATH=$HDR/usr
gcc -O2 -Wall -static -pthread -I$HDR/usr/include -o repro4 repro4.c
./repro4            # all cases; or e.g. ./repro4 N
```

UML recipe used for everything above (no root/KVM). The `/init` mounts proc, sysfs,
devtmpfs, debugfs and cgroup2 with `+memory`, like `run_opaque_vm.sh`, so the series'
memcg and failslab tests run instead of skipping:

```sh
make ARCH=um O=$B x86_64_defconfig
scripts/config --file $B/.config -e IO_URING -e IPV6 -e KUNIT -e IO_URING_OPAQUE_OBJ \
  -e IO_URING_OPAQUE_OBJ_KUNIT_TEST -e KASAN -e PROVE_LOCKING -e DEBUG_ATOMIC_SLEEP \
  -e FAILSLAB -e FAULT_INJECTION_DEBUG_FS -e FAULT_INJECTION_STACKTRACE_FILTER \
  -e DEBUG_FS -e KALLSYMS_ALL -e MEMCG
make ARCH=um O=$B olddefconfig && make ARCH=um O=$B -j32
$B/linux mem=2048M rootfstype=hostfs rootflags=$ROOT rw init=/init con=null con0=null,fd:1
```

<details><summary>repro4.c</summary>

```c
// SPDX-License-Identifier: GPL-2.0
/* Review reproducers for the io_uring OPAQUE_OBJ RFC v4 (origin/upstream/opaque-obj-v4). */
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

static void sendall_nofail(int fd, const unsigned char *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = send(fd, buf + done, len - done, MSG_NOSIGNAL);

		if (n <= 0)
			return;
		done += n;
	}
}

struct burst_writer {
	int fd;
	size_t burst;
	unsigned int bursts;
	unsigned int sleep_us;
};

static void *burst_writer_fn(void *arg)
{
	struct burst_writer *w = arg;
	unsigned char *buf = calloc(1, w->burst);
	unsigned int i;

	for (i = 0; i < w->bursts; i++) {
		sendall_nofail(w->fd, buf, w->burst);
		if (w->sleep_us)
			usleep(w->sleep_us);
	}
	free(buf);
	return NULL;
}

/* A: collector longevity across many poll cycles (v1 died after 128). */
static void test_collector(const char *name, size_t burst, unsigned int bursts,
			   unsigned int sleep_us)
{
	struct io_uring_opaque_config cfg = default_cfg();
	struct burst_writer w = { .burst = burst, .bursts = bursts, .sleep_us = sleep_us };
	unsigned long long total = (unsigned long long)burst * bursts;
	struct event ev;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, discard;
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
	require(!pthread_create(&thr, NULL, burst_writer_fn, &w), "writer");
	got = wait_tag_to(&r, discard, 120000, &ev);
	shutdown(pair[0], SHUT_RDWR);
	pthread_join(thr, NULL);
	printf("[A:%s] DISCARD of %llu bytes: %s res=%d -> %s\n", name, total,
	       got ? "completed" : "TIMEOUT", got ? ev.res : 0,
	       got && ev.res == (int)total ? "ok" : "BUG");
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL);
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
}

/* B: RST in the middle of a RECV_OBJECT must fail it, not publish a short object. */
static void test_reset(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	struct linger lg = { .l_onoff = 1, .l_linger = 0 };
	unsigned char buf[4096] = { 0 };
	struct event ev, col;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, keep;
	int pair[2];

	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(pair);
	stream = attach(&r, ctx, pair[1], COLLECTOR_TAG);
	keep = cmd_stage(&r, ctx, IORING_OPAQUE_RECV_OBJECT, stream, 0, 4096, NULL);
	enter(&r, 0);
	write_all(pair[0], buf, 1000);
	usleep(50000);
	enter(&r, 0);
	require(!setsockopt(pair[0], SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)), "linger");
	close(pair[0]);
	require(wait_tag_to(&r, keep, 5000, &ev), "RECV_OBJECT completion after RST");
	printf("[B] RECV_OBJECT(0, 4096) after 1000 bytes + RST: res=%d handle=0x%llx\n",
	       ev.res, (unsigned long long)ev.extra[0]);
	if (wait_tag_to(&r, COLLECTOR_TAG, 2000, &col))
		printf("[B] collector terminal CQE res=%d\n", col.res);
	printf("[B] RESULT: %s\n", ev.res < 0 && col.res < 0 ? "ok" : "BUG");
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL);
	ring_exit(&r);
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

/* D2/D3: two queued sends to one socket; short head must not splice the stream. */
static void test_fifo(bool dontwait)
{
	struct io_uring_opaque_config cfg = default_cfg();
	size_t xlen = 2U << 20, ylen = 64U << 10, total = xlen + ylen, i, first_b = 0;
	unsigned char *src = malloc(total), *seen = calloc(1, total);
	struct transfer tx = { .data = src, .length = total };
	struct collect_reader cr = { .buf = seen, .cap = total, .sleep_us = 500 };
	const char *name = dontwait ? "D3" : "D2";
	struct event ev, sx, sy;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, kx, ky, hx, hy, tx_x, tx_y;
	int s[2], d[2];
	pthread_t prod, cons;
	bool got_b = false;

	memset(src, 'A', xlen);
	memset(src + xlen, 'B', ylen);
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(s);
	tcp_pair_small(d, 4096, 4096);
	stream = attach(&r, ctx, s[1], COLLECTOR_TAG);
	kx = cmd_stage(&r, ctx, IORING_OPAQUE_RECV_OBJECT, stream, 0, xlen, NULL);
	ky = cmd_stage(&r, ctx, IORING_OPAQUE_RECV_OBJECT, stream, xlen, ylen, NULL);
	tx.fd = s[0];
	require(!pthread_create(&prod, NULL, writer, &tx), "producer");
	require(wait_tag_to(&r, kx, 30000, &ev) && ev.res == (int)xlen, "RECV_OBJECT X");
	hx = ev.extra[0];
	require(wait_tag_to(&r, ky, 30000, &ev) && ev.res == (int)ylen, "RECV_OBJECT Y");
	hy = ev.extra[0];
	pthread_join(prod, NULL);
	tx_x = send_stage_flags(&r, ctx, d[0], hx, 0, xlen, 0, dontwait ? MSG_DONTWAIT : 0, 0);
	tx_y = send_stage(&r, ctx, d[0], hy, 0, ylen);
	cr.fd = d[1];
	require(!pthread_create(&cons, NULL, collect_reader, &cr), "consumer");
	require(wait_tag_to(&r, tx_x, 120000, &sx), "SEND X");
	require(wait_tag_to(&r, tx_y, 120000, &sy), "SEND Y");
	shutdown(d[0], SHUT_WR);
	pthread_join(cons, NULL);
	for (i = 0; i < cr.done; i++)
		if (seen[i] == 'B') {
			first_b = i;
			got_b = true;
			break;
		}
	printf("[%s] SEND X%s res=%d, SEND Y res=%d; peer got %zu bytes, 'B' %s%zu\n", name,
	       dontwait ? "(MSG_DONTWAIT)" : "", sx.res, sy.res, cr.done,
	       got_b ? "first at " : "never, ", got_b ? first_b : 0);
	printf("[%s] RESULT: %s\n", name,
	       (sx.res == (int)xlen && (!got_b || first_b == xlen)) ||
	       (sx.res < (int)xlen && !got_b && sy.res == -ECANCELED) ? "ok" :
	       "BUG: destination stream spliced");
	ring_exit(&r);
	close(s[0]); close(s[1]); close(d[0]); close(d[1]);
	free(src);
	free(seen);
}

/* E: link order now resolves handles at issue. */
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
	keep = cmd_stage(&r, ctx, IORING_OPAQUE_RECV_OBJECT, stream, 0, 8, NULL);
	write_all(pair[0], "abcdefgh", 8);
	ev = wait_tag(&r, keep);
	require(ev.res == 8, "recv object");
	handle = ev.extra[0];
	free_tag = cmd_stage(&r, ctx, IORING_OPAQUE_FREE, handle, 0, 0, NULL);
	index = (*r.sq_tail - 1) & *r.sq_mask;
	r.sqes[index].flags |= IOSQE_IO_LINK;
	read_tag = cmd_stage(&r, ctx, IORING_OPAQUE_READ_OBJECT, handle, 0, 8, buf);
	fr = wait_tag(&r, free_tag);
	rd = wait_tag(&r, read_tag);
	printf("[E] FREE(h) -> LINK -> READ_OBJECT(h): FREE=%d READ=%d -> %s\n", fr.res, rd.res,
	       fr.res == 0 && rd.res == -ESTALE ? "ok" : "BUG");
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL);
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

/* F: interleaved fragments from one task_frag page should share a backing charge. */
static void test_interleave(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	struct interleave iw = { .rounds = 32, .chunk = 1024 };
	struct io_uring_opaque_stat stat;
	struct event ev;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, keep;
	int a[2], b[2];
	pthread_t thr;

	cfg.hard_limit = 256U << 10;
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(a);
	tcp_pair(b);
	stream = attach(&r, ctx, a[1], COLLECTOR_TAG);
	keep = cmd_stage(&r, ctx, IORING_OPAQUE_RECV_OBJECT, stream, 0, iw.rounds * iw.chunk, NULL);
	iw.a = a[0];
	iw.b = b[0];
	require(!pthread_create(&thr, NULL, interleave_writer, &iw), "writer");
	pthread_join(thr, NULL);
	require(wait_tag_to(&r, keep, 10000, &ev), "keep");
	require(!command(&r, ctx, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res, "stat");
	printf("[F] RECV_OBJECT 32 KiB, 256 KiB store, interleaved writer: res=%d backing=%llu copied=%llu -> %s\n",
	       ev.res, (unsigned long long)stat.backing_bytes,
	       (unsigned long long)stat.copied_bytes, ev.res == 32768 ? "ok" : "BUG");
	if (ev.res > 0)
		command(&r, ctx, IORING_OPAQUE_FREE, ev.extra[0], 0, 0, NULL);
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL);
	ring_exit(&r);
	close(a[0]); close(a[1]); close(b[0]); close(b[1]);
}

/*
 * N1: framing is charged against the capture allowance. Once inbound capture
 * fills it, no framed or generated reply can be sent, even with free
 * compaction headroom: a request/response proxy cannot release its objects.
 */
static void test_frame_starvation(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	struct io_uring_opaque_frame frame = {
		.size = sizeof(frame), .prefix = (uintptr_t)"HDR:", .prefix_length = 4,
	};
	struct burst_writer w = { .burst = 64U << 10, .bursts = 16 };
	struct io_uring_opaque_stat stat;
	struct event ev, s1, s2, s3;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, keep, handle, t;
	unsigned int i;
	int src[2], dst[2];
	pthread_t thr;

	cfg.hard_limit = 256U << 10;
	cfg.compact_headroom = 64U << 10;
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(src);
	tcp_pair(dst);
	stream = attach(&r, ctx, src[1], COLLECTOR_TAG);
	keep = cmd_stage(&r, ctx, IORING_OPAQUE_RECV_OBJECT, stream, 0, 8, NULL);
	write_all(src[0], "object!!", 8);
	require(wait_tag_to(&r, keep, 5000, &ev) && ev.res == 8, "small object");
	handle = ev.extra[0];
	/* The peer keeps sending; nothing has been decided about these bytes yet. */
	w.fd = src[0];
	require(!pthread_create(&thr, NULL, burst_writer_fn, &w), "flood");
	for (i = 0; i < 100; i++) {
		struct event tmp;

		wait_tag_to(&r, ~0ULL, 20, &tmp);
		require(!command(&r, ctx, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res, "stat");
		if (stat.backing_bytes >= cfg.hard_limit - cfg.compact_headroom - 4096)
			break;
	}
	printf("[N1] capture allowance %llu, headroom %llu: backing=%llu after inbound flood\n",
	       (unsigned long long)(cfg.hard_limit - cfg.compact_headroom),
	       (unsigned long long)cfg.compact_headroom, (unsigned long long)stat.backing_bytes);
	t = frame_stage(&r, ctx, dst[0], handle, 8, &frame, IORING_OPAQUE_SEND_LAST, 0);
	require(wait_tag_to(&r, t, 5000, &s1), "framed SEND_LAST");
	t = frame_stage(&r, ctx, dst[0], 0, 0, &frame, 0, 0);
	require(wait_tag_to(&r, t, 5000, &s2), "generated-only reply");
	t = send_stage_flags(&r, ctx, dst[0], handle, 0, 8, IORING_OPAQUE_SEND_LAST, 0, 0);
	require(wait_tag_to(&r, t, 5000, &s3), "body-only SEND_LAST");
	printf("[N1] framed SEND_LAST=%d consumed=%d; generated-only reply=%d; body-only SEND_LAST=%d\n",
	       s1.res, !!(s1.flags & IORING_CQE_F_OPAQUE_CONSUMED), s2.res, s3.res);
	printf("[N1] RESULT: %s\n", s1.res == -ENOBUFS || s2.res == -ENOBUFS ?
	       "LIVENESS: inbound capture blocks framed replies despite free headroom" : "ok");
	shutdown(src[0], SHUT_RDWR);
	pthread_join(thr, NULL);
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL);
	ring_exit(&r);
	close(src[0]); close(src[1]); close(dst[0]); close(dst[1]);
}

struct blocked_send {
	int fd;
	size_t len;
	ssize_t result;
	int error;
};

static void *blocked_sender(void *arg)
{
	struct blocked_send *b = arg;
	unsigned char *buf = calloc(1, b->len);

	b->result = send(b->fd, buf, b->len, MSG_NOSIGNAL);
	b->error = errno;
	free(buf);
	return NULL;
}

static void *drain_reader(void *arg)
{
	struct collect_reader *c = arg;
	unsigned char buf[65536];

	while (c->done < c->cap) {
		ssize_t n = read(c->fd, buf, sizeof(buf));

		if (n <= 0)
			break;
		c->done += n;
	}
	return NULL;
}

/*
 * N2: a rejected connect(AF_UNSPEC) on a claimed socket still bumps
 * sk_disconnects and sets SS_DISCONNECTING in __inet_stream_connect().
 */
static void test_disconnect_side_effects(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	struct sockaddr unspec = { .sa_family = AF_UNSPEC };
	struct sockaddr_in peer;
	socklen_t plen = sizeof(peer);
	struct blocked_send bs = { .len = 16U << 20 };
	struct collect_reader dr = { .cap = 16U << 20 };
	struct ring r;
	uint32_t ctx;
	uint64_t stream;
	pthread_t snd, rdr;
	int pair[2], alias, rc, err;

	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(pair);
	alias = dup(pair[1]);
	stream = attach(&r, ctx, pair[1], COLLECTOR_TAG);
	require(!getpeername(pair[1], (void *)&peer, &plen), "getpeername");
	/* TX stays with the application: block a sender on the claimed socket. */
	bs.fd = pair[1];
	require(!pthread_create(&snd, NULL, blocked_sender, &bs), "sender");
	usleep(300000);
	rc = connect(alias, &unspec, sizeof(unspec));
	err = errno;
	printf("[N2] connect(AF_UNSPEC) on claimed socket: %d errno=%d (%s)\n", rc, err,
	       rc ? strerror(err) : "ok");
	dr.fd = pair[0];
	require(!pthread_create(&rdr, NULL, drain_reader, &dr), "reader");
	pthread_join(snd, NULL);
	printf("[N2] blocked send(16 MiB) on the same socket returned %zd (errno %d)\n",
	       bs.result, bs.result < 0 ? bs.error : 0);
	rc = connect(alias, (void *)&peer, sizeof(peer));
	err = errno;
	printf("[N2] connect() to the existing peer now fails with %s (expected EISCONN)\n",
	       rc ? strerror(err) : "success");
	printf("[N2] RESULT: %s\n", rc == -1 && err == EISCONN && bs.result == (ssize_t)bs.len ?
	       "ok" : "BUG: rejected disconnect still disturbed the connected socket");
	shutdown(pair[1], SHUT_WR);
	pthread_join(rdr, NULL);
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL);
	ring_exit(&r);
	close(alias);
	close(pair[0]);
	close(pair[1]);
}

static void run(const char *name, void (*fn)(void))
{
	int status;
	pid_t pid = fork();

	if (!pid) {
		alarm(240);
		fn();
		_exit(0);
	}
	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status))
		printf("[%s] test process killed by signal %d\n", name, WTERMSIG(status));
	else if (WEXITSTATUS(status))
		printf("[%s] test process exited with %d\n", name, WEXITSTATUS(status));
}

static void test_a1(void) { test_collector("idle-bursts", 100, 300, 3000); }
static void test_a2(void) { test_collector("continuous", 1U << 20, 64, 0); }
static void test_d2(void) { test_fifo(false); }
static void test_d3(void) { test_fifo(true); }

int main(int argc, char **argv)
{
	const char *only = argc > 1 ? argv[1] : NULL;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (!only || strchr(only, 'A')) {
		run("A1", test_a1);
		run("A2", test_a2);
	}
	if (!only || strchr(only, 'B'))
		run("B", test_reset);
	if (!only || strchr(only, 'D')) {
		run("D2", test_d2);
		run("D3", test_d3);
	}
	if (!only || strchr(only, 'E'))
		run("E", test_link_order);
	if (!only || strchr(only, 'F'))
		run("F", test_interleave);
	if (!only || strchr(only, 'N')) {
		run("N1", test_frame_starvation);
		run("N2", test_disconnect_side_effects);
	}
	return 0;
}
```

</details>
