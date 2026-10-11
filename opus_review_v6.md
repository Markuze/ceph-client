# Review: io_uring opaque TCP payload objects, v6

**Series reviewed:** `origin/upstream/opaque-obj-v6` at `8fae2736b062`, 5 patches on top of
`af32da41b032` ("Merge tag 'net-7.3-rc7'"):

| # | Commit | Subject |
|---|--------|---------|
| 1/5 | `89371c5f7948` | net: add an optional exclusive TCP receive claim |
| 2/5 | `03750aaec433` | io_uring: add independent opaque TCP object stores and framed I/O |
| 3/5 | `da7d09c87974` | io_uring: test opaque ownership, available inspection and compaction |
| 4/5 | `3b1793525d20` | selftests/io_uring: exercise opaque quota, ownership and SMP cancellation |
| 5/5 | `8fae2736b062` | docs: describe opaque object, framed-send and quota ownership contracts |

Context:

- v5 (`cc7e5daea94b`) is v4 plus the fixes for my v4 review, a quota notification on
  the collector, and smaller framing allocations.
- v6 folds one more change into v5: manual and automatic compaction now share one
  delayed work item, and `copy_lock` is gone.
- Supporting material:
  - The author's errata and optimization notes on `origin/work/opaque-obj-v6`
    (`b88a180a8633`).
  - The cover letter and validation record in `zc_proxy`,
    `experiments/opaque_obj_rfc_v6` (`edf42dd`).

This review follows `opus_review_v4.md`. §3 maps each v4 finding to its v6 status. All
`file:line` references are to `8fae2736b062` unless stated otherwise. As before, this is
written from the perspective of an io_uring maintainer reviewing an RFC; it is not an
actual maintainer response.

---

## 1. Verdict

**Converging. Still not mergeable, for one process reason, one design disagreement,
one new bug and one bisect break.**

v6 handled the v4 review well:

- **N2 and R are fixed exactly as proposed**, each with a regression test.
  - The R test is a 10,000-round cross-ring SMP race, and it passed here on SMP under
    KASAN and lockdep.
  - The author reports it also passed on the v4 image. It guards the fix but doesn't
    demonstrate the race; R remains an inspection finding.
- **Every smaller note is addressed:**
  - failed RECV_OBJECT prefixes are restored in the same `stream->lock` section;
  - provenance and claims moved from the ABI doc to a careful cover letter;
  - `opaque_waits` is under the config symbol;
  - framing uses its allocator bucket instead of at least a page.
- **The compaction rework is a real simplification:** one work item, one fewer mutex,
  and ineligible objects no longer keep a timer alive. It showed no regressions in my
  runs.
- **The errata and optimization notes are an honest record** of what is deferred.

What still blocks it:

1. **No `Signed-off-by:` on any patch.** The cover letter says human DCO certification
   is pending. The `Assisted-by: LLM` trailer also doesn't follow the documented
   `AGENT_NAME:MODEL_VERSION` format, and checkpatch warns on each patch.
2. **N1 is reclassified as policy (E01), not fixed.** The new pressure CQE makes the
   stall visible, but the application still has no way out (§4.3). In a new two-connection
   reproducer, the application evicts an object as the errata recommend. A greedy
   stream on another connection immediately recaptures the freed page, and every
   framed or generated reply on the other connection still fails `-ENOBUFS`. Capture is
   greedy and store-wide, so the "application retention margin" mitigation can't be
   enforced by the application.
3. **New bug Q [medium]:** whether canceling one queued SEND also cancels the next
   one depends on task-work batching (§4.1). It reproduces deterministically in a single
   ring: same submissions, two outcomes, depending only on when the cancel lands. The
   fix is small and verified (Appendix A).
4. **New bisect break B [low]:** patch 2/5 adds the KUnit Kconfig symbol and Makefile
   rule, but `opaque_test.c` only arrives in 3/5. With the KUnit option enabled (the
   default under `KUNIT_ALL_TESTS`), 2/5 fails to build (§4.2).
5. **Design asks carried over from v4 (§5), all deferred by the author:**
   - split framed sends;
   - the generic CQE flag bit;
   - the store-wide `tables` mutex;
   - benchmarks.

---

## 2. What I ran

This time the machine could run SMP. This base has UML SMP support (`CONFIG_SMP=y`,
booted with `seccomp=on ncpus=2`), so the cross-ring tests ran on two vCPUs instead of
the uniprocessor UML used for v4.

| Check | Result |
|-------|--------|
| UML build, KASAN + PROVE_LOCKING + DEBUG_ATOMIC_SLEEP + failslab + **SMP (2 vCPUs) + PREEMPT** | builds, 0 warnings |
| Per-commit builds with the feature **forced on** at each commit | 1/5 OK; **2/5 fails with `IO_URING_OPAQUE_OBJ_KUNIT_TEST=y`** (OK without it); 3/5 and 5/5 OK; 4/5 touches only `tools/` and MAINTAINERS (§4.2) |
| `W=1` on `io_uring/`, `net/ipv4/{tcp,af_inet,tcp_ulp}.o`, `net/core/sock.o`, and `net/core/skmsg.o` (with `TLS` → `NET_SOCK_MSG`) | 0 warnings |
| `CONFIG_IO_URING_OPAQUE_OBJ=n` | builds, 0 warnings |
| `checkpatch.pl --strict` | `ERROR:MISSING_SIGN_OFF` and an `Assisted-by` format warning on all 5; otherwise only MAINTAINERS file-path warnings. The SPDX check didn't run here (no `python3-ply`) |
| KUnit `io_uring-opaque` | 23/23 |
| `tools/testing/selftests/io_uring/opaque_obj`, 2-vCPU UML, cgroup2 memory + debugfs + failslab | **80/80, no skips, no kernel diagnostics.** This includes 10,000 cross-ring SEND races on SMP (5000 full and 5000 partial heads, 9581 followers canceled, 419 sent, 5000 linked-timeout expiries), 10,000 arrival/drain cycles and 4 GiB of DISCARD |
| My v1/v4 reproducers (`repro4`; the v4 binary still runs against v6) | A1, A2, B, D2, D3, E, F, N2 pass; **N1 still fails, by policy** |
| New reproducers (`repro6`, Appendix B) | **Q fails 5/5**; **two-connection N1 fails** |
| Appendix A (Q fix) on the same image | KUnit 23/23; **selftest 80/80**, including the 10,000 SMP races (5000/5000 heads, 9549 followers canceled, 451 sent); Q-early and Q-late both correct 5/5; no kernel diagnostics |

Notes:

- **The selftest's `alarm(600)` is too short for UML.** With SMP and KASAN under UML, the
  10,000-round race test reached about 4,300 rounds before SIGALRM ended the run
  (rc 142). No test had failed by then. I reran with the alarm raised to 7200 s and no
  other change. `SEND_CANCEL_ROUNDS` can be overridden at build time, but the alarm
  can't (§6).
- **Correction to my v4 build check.** I didn't force the option on at each commit for
  v4, so a config could drop it at intermediate commits. I re-checked v4's 2/5 with the
  feature on, and it builds, so the v4 "bisectable" result stands. The v6 break is new:
  v4 added the KUnit symbol, rule and file together in 3/5.

Reproducer results on `8fae273`:

| Case | Scenario | Result |
|------|----------|--------|
| N2 (v4) | blocked 16 MiB `send()` on the claimed socket; `connect(alias, AF_UNSPEC)` | `EBUSY`; `send()` returns **16777216**; next `connect()` returns `EISCONN`. **Fixed** |
| D3 (v4) | short `MSG_DONTWAIT` head, follower Y | X = 6144, Y = `-ECANCELED`, no Y bytes. Still correct |
| N1 (v4) | single store, inbound flood fills the 192 KiB capture allowance; framed reply | framed = `-ENOBUFS`, generated-only = `-ENOBUFS`, body-only = 8. **Unchanged, by policy** |
| N1 (v6) | greedy connection A floods; the app gets A's pressure CQE and FREEs a cached object; connection B replies | pressure CQE `res=-ENOBUFS`, `F_MORE`, correct token and offset; A's undecided bytes grow by exactly the freed 4096; **B's framed SEND_LAST and generated-only reply both `-ENOBUFS`** |
| Q-early | H (300 KiB) blocked at the head; F1, F2 queued; cancel F1 while H is blocked | H full, F1 `-ECANCELED`, **F2 = 8**, F2's bytes on the wire. Correct |
| Q-late | same submissions; cancel F1 after H's final `POLLOUT` task work is queued but not yet run | H full, F1 `-ECANCELED`, **F2 = `-ECANCELED`**, no F2 bytes. **Bug**, 5/5 |

---

## 3. Status of the v4 review

| v4 item | v6 status | Evidence |
|---------|-----------|----------|
| N1: framing charged to the capture allowance | **Not fixed; declared policy (E01)**. Pressure CQE added | `opaque.c:1904-1905` still charges with `rx = true`; doc: "framing cannot consume the compaction reserve"; `test_frame_quota` still asserts `-ENOBUFS`. New evidence in §4.3 |
| N2: rejected `connect(AF_UNSPEC)` changes socket state | **Fixed** | `af_inet.c:652-653` returns `-EBUSY` before `sk_disconnects++`; IPv4/IPv6 blocked-send tests; my N2 repro passes |
| R: cross-ring cancel vs. head promotion double-queues task work | **Fixed** | cancel claims `QUEUED` under `wait_lock` (`opaque.c:1740-1743`); the `tx_put()` failure loop decides under `wait_lock` (`:2062-2072`); 10,000-round SMP race test passes here on 2 vCPUs |
| Process: `Signed-off-by` | **Still missing** | checkpatch; the cover letter says DCO certification is pending |
| §5.1: split framed sends into a follow-up | Not done | still in 2/5 |
| §5.2: generic CQE flag bit | Unchanged | `IORING_CQE_F_OPAQUE_CONSUMED` |
| §5.2: INSPECT minimum length | Not added | doc keeps "never WAITALL" |
| §5.3: RECV_OBJECT fail-fast under shared pressure | Deferred | `opaque.c:1272-1281`; cover letter "remaining scope" |
| §5.4: store-wide `tables` mutex; `wake_budget()` O(waiters) per free | Deferred | unchanged |
| §5.4: framing allocations oversized | **Fixed** | `kmalloc_size_roundup(bytes)` (`opaque.c:1904`) |
| §5.5: net hook | N2 fixed; the static-key question remains netdev's call | |
| §5.6: benchmarks | **None** | cover letter: correctness only, KVS/relay pilot planned |
| §6: restore in the same locked section | **Fixed** | `stream_process()` `:819-820`, fail-fast `:1279`, cancel `:1752-1754`; same-batch cancel/INSPECT test |
| §6: `attach()` holds `tables` across `lock_sock()` and the CQE post | Unchanged | `opaque.c:1138-1189` |
| §6: provenance and claims in the ABI doc | **Fixed** | moved to the cover letter |
| §6: `opaque_waits` not under the config symbol | **Fixed** | `io_uring_types.h`, `io_uring.c:295` |

---

## 4. New findings in v6

### 4.1 Q. [medium] Canceling one queued SEND can cancel the next one, depending on cleanup order

- **Where:** `io_opaque_tx_put()` (`opaque.c:2043-2086`), called from
  `io_opaque_cleanup()` (`:1832`), i.e. when each request is freed.
  - The "broken head" test (`:2059-2074`) is applied to whichever request is first in
    `tx->requests` *at cleanup time*.
  - The success path promotes only the first remaining entry, and only if it is in
    `SEND_WAIT` (`:2076-2082`).
- **What happens.** H is the head; F1 and F2 wait behind it on the same socket. The
  application cancels F1 only, with `ASYNC_CANCEL` or a linked timeout.
  - **If F1's cleanup runs before H's:** F1 isn't the head, so it is simply unlinked. H
    later completes and promotes F2. F2 is sent.
  - **If H's cleanup runs first:**
    1. H's `tx_put()` sees F1 first. F1 is already completing (`IO_OPAQUE_IDLE`), so
       nothing is promoted.
    2. F1's cleanup then finds F1 at the head with `progress (0) != total_length`.
    3. It sets `tx->failed` and cancels F2.
    4. `tx->failed` also rejects new SENDs to that socket until the queue drains.
- **The order is not under the application's control.** With `DEFER_TASKRUN`, which
  registration requires, H's final `POLLOUT` retry and F1's cancellation are both local
  task work. When they land in the same batch, the completions are flushed and freed
  in queue order: H, then F1. Across rings it is plain timing.
- **Reproduced, single ring (repro6 Q, 5/5):**
  - Q-early (cancel while H is blocked): F2 = 8, F2's bytes on the wire.
  - Q-late (cancel after H's final `POLLOUT` wakeup is queued): F2 = `-ECANCELED`, no
    F2 bytes.
  - The submissions are identical; only the moment the cancel is submitted differs.
- **Why it's a bug, not a policy:**
  - The doc says "Canceling a follower alone does not interrupt an earlier send" and
    says nothing about later followers.
  - Either rule could be defended. A rule that depends on task-work batching can't
    be: the application cannot tell whether F2 will go out.
  - The wire is consistent in both outcomes, so there's no corruption, but the
    application's reply bookkeeping diverges.
- **Not covered by the series' tests.** `test_send_cancel_race` uses exactly one
  follower per round, so a second follower's fate is never checked.
- **Fix (Appendix A, verified):**
  - Record whether a request ever reached the head and attempted to transmit
    (`tx_started`, set after `io_opaque_tx_enter()` returns 0).
  - Only such a request can fail its followers. A request canceled while waiting
    never transmitted, so removing it just promotes the next waiter.
  - E03's current behavior is kept: a head that started and failed with zero progress
    still fails its followers.
  - With the fix, Q-early and Q-late both give F2 = 8, 5/5 each.
  - If the author prefers "skipping a reply poisons the queue", the deterministic
    alternative is to fail all *later* followers whenever any admitted request is
    removed incomplete, head or not. Pick one and document it.
- **Add a test:** two followers, cancel the first, both before and after the head's
  last wakeup, in one ring and across rings.

### 4.2 B. [low, process] 2/5 doesn't build with the KUnit option enabled

2/5 adds `IO_URING_OPAQUE_OBJ_KUNIT_TEST` to `io_uring/Kconfig` and
`obj-$(CONFIG_IO_URING_OPAQUE_OBJ_KUNIT_TEST) += opaque_test.o` to `io_uring/Makefile`.
`opaque_test.c` itself arrives in 3/5. With the option enabled, which is the default
under `KUNIT_ALL_TESTS`, 2/5 fails:

```
make[4]: *** No rule to make target 'io_uring/opaque_test.o', needed by 'io_uring/built-in.a'.  Stop.
```

v4 added the symbol, the rule and the file together in 3/5. Move them back.

### 4.3 N1, revisited. [high, design] The pressure CQE makes the stall visible but doesn't give the application a way out

The author's position (E01, ABI doc): quota exhaustion is propagated, not prevented.

- The collector posts a nonterminal `-ENOBUFS` CQE with the token and unread offset,
  coalesced per offset.
- The application is expected to release capacity, and keep "an application retention
  margin for receive progress and concurrent response framing".
- Framing may not use compaction headroom, so that compaction keeps a guaranteed
  reserve.

The notification itself is good. My reproducer saw the right token and the right
unread offset. The series' tests 46–47, which pass here, cover per-offset coalescing and
`F_32` on mixed rings. The problem is that the margin it relies on isn't something the
application controls:

- **Capture is greedy and has no per-stream bound.**
  - The collector pulls every byte the peer sends into the store as undecided data,
    until the store-wide allowance is full.
  - `stream->undecided` is only an accounting field; nothing limits it.
  - The undecided bytes belong to whichever connection is fastest, not to the objects
    the application chose to keep.
- **Released capacity goes back to the stalled collector.**
  - A parked collector is woken by every uncharge (`io_opaque_wake_budget()`).
  - When the application frees an object in response to the CQE, the greedy stream
    takes the page.
- **Reproduced (repro6 N1):**
  1. 256 KiB store, 64 KiB headroom. Connection B has one published request object and
     one cached object.
  2. Connection A floods. The store reaches the 192 KiB capture allowance (196,608
     bytes of backing, 159,690 of them undecided bytes on A), and A's collector posts
     the pressure CQE.
  3. The application FREEs B's cached object, as the errata recommend.
  4. A's undecided count rises by exactly 4096.
  5. B's framed `SEND_LAST` and a generated-only reply both fail `-ENOBUFS`. The
     compaction headroom stays free throughout.
- **Liveness, as in v4:**
  - A request/response proxy can't send the framed replies that would release its
    objects without dropping data. Only body-only sends still work.
  - One fast client can stall replies on every connection that shares the store.
  - The pressure CQE turns a silent stall into a visible one, but every response
    available to the application is lossy: FREE live objects, DISCARD unparsed bytes,
    or close a connection.

What I'd ask for, keeping the author's compaction guarantee intact:

1. **A per-stream capture window:** a bound on undecided plus private in-progress bytes
   per stream, configurable per store or per COLLECT.
   - Bytes beyond the window stay in the TCP receive queue, so TCP flow control applies
     per connection, as `SO_RCVBUF` does for ordinary sockets.
   - The application can then size the store as
     `max_streams × window + retained objects + reply reserve`, which is a margin it can
     actually keep.
2. **A reply reserve for framing that capture can't use,** separate from
   `compact_headroom`.
   - Framing is already bounded per request (64 KiB) and by `max_requests`.
   - Framing is released when its send completes, independently of capture and of the
     application freeing objects. A reply then never waits for inbound data to be
     consumed first.

Either change alone helps. Together they make N1 impossible by construction. Keep
`test_frame_quota`'s "entire hard limit exhausted" case, and add the two-connection
case.

### 4.4 Pressure-notification ABI notes [low to medium]

- **A full CQ ends collection on that stream for good.**
  - If the pressure CQE can't be posted, the collector ends with `ENOSPC` and
    `stream->error` is set (`opaque.c:1283-1291`).
  - COLLECT has no way to re-arm on an existing token. Recovery means draining the
    captured bytes, STREAM_CLOSE, and a fresh COLLECT whose offsets restart at zero.
    `test_budget_full_cq` documents exactly this.
  - Elsewhere in io_uring, a multishot request that can't post just terminates, and the
    application re-arms it with nothing lost.
  - Here a transient CQ-full makes the CQ size a correctness parameter for every
    connection.
  - Consider letting COLLECT re-arm a stream token whose collector has ended. That would
    also make collector cancellation non-fatal; today `io_opaque_cleanup()` closes the
    stream (`:1816-1819`).
- **A negative `res` with `F_MORE`.**
  - There is precedent: a failed `SEND_ZC` can post `res < 0` with `F_MORE`, because its
    notification still follows.
  - But many applications treat `res < 0` as terminal.
  - Call it out prominently in the doc and any liburing helper, or encode the event in
    `extra` with `res = 0`.
- **No "resumed" edge.** Coalescing per offset is fine. Note in the doc that the absence of
  a new CQE doesn't mean the stream is progressing: freed capacity may go to another
  collector. STREAM_STAT is the way to check.

### 4.5 Compaction dispatcher: correct in my runs; one performance note, one comment nit

**Good:**

- The single delayed work item relies on workqueue non-reentrancy instead of a mutex.
- Enrollment is closed under `tables` before the flush.
- Disabling the policy keeps accepted manual requests.
- Permanently ineligible objects leave the scan list, and `SET_POLICY` re-enrolls them.
- No lockdep, KASAN or `DEBUG_ATOMIC_SLEEP` reports in any run, including the
  compaction teardown and shared-ring tests on SMP.

**Performance:** every publication defeats the scan pacing.

- With `AUTO_COMPACT` on, each RECV_OBJECT publication sets `store->auto_at = jiffies`
  and kicks the dispatcher immediately (`opaque.c:1394-1395`).
- But a new object can't be selected for 100 ms (`opaque_compact.c:308-310`).
- So under a high publication rate the dispatcher scans at up to that rate. Each scan
  holds the store-wide `tables` mutex for up to 32 candidates, none of which can be
  eligible because of age, adding contention on the mutex every operation already takes.
- Suggested change on publication:
  - leave `auto_at` alone;
  - `queue_delayed_work()` the dispatcher for the new object's eligibility time
    (`born + 100 ms`);
  - `queue_delayed_work()` won't shorten an already-pending timer, so this coalesces
    naturally.

**Nit:** `opaque_compact.c:167` says "Shutdown drains every accepted request, without
another invocation".

- That isn't literally true. If the 8-request batch ends just before `io_opaque_stop()`
  sets `dead`, the instance requeues itself once (`:190-194`).
- I forced that ordering with temporary delays and tracing. `io_opaque_stop()`'s
  `flush_delayed_work()` still waited for the second invocation, so I found no lifetime
  problem.
- Looping instead of requeueing once `dead` is set would make the comment true and
  the shutdown argument simpler.

---

## 5. Design status

These are carried over from v4. The author has explicitly deferred all of them; I've
noted only what changed.

- **Framed sends in the first series (§5.1 of v4).** Still in 2/5. N1 is still a direct
  consequence of charging framing to the capture allowance. Q isn't framing-specific; it
  reproduces with body-only sends.
- **UAPI (§5.2 of v4).**
  - The generic CQE flag bit is unchanged.
  - INSPECT has no minimum.
  - `ioprio` still multiplexes 11 controls.
  - A 32-byte CQE indication is still my preference, given that every ring using this
    is CQE32 or CQE_MIXED.
- **Fail-fast RECV_OBJECT under shared pressure (§5.3 of v4).** Unchanged. With a
  per-stream window (§4.3), the "private prefix waiting for itself" case becomes much
  rarer, which may make the fail-fast unnecessary.
- **Scalability (§5.4 of v4).**
  - The `tables` mutex and the O(waiters) `wake_budget()` remain.
  - The publication kick in §4.5 adds to the mutex traffic.
- **Benchmarks (§5.6 of v4).** Still none. The cover letter is careful not to claim any.
  A shared KVS/relay pilot is planned.

---

## 6. Smaller notes

- **`Assisted-by: LLM`** should name the agent and model
  (`Documentation/process/coding-assistants.rst`). The `Signed-off-by` must come from
  the human submitter.
- **The selftest's `alarm(600)` is global and fixed.**
  - On slower targets (UML SMP with KASAN here), the race test alone overruns it.
  - Scale the alarm with `SEND_CANCEL_ROUNDS`, or make both runtime parameters, so a
    slow CI machine reports a timeout rather than an unexplained SIGALRM partway
    through the suite.
- **`io_opaque_attach()` still holds the store-wide `tables` mutex across `lock_sock()`
  and the CQE post.**
- **The errata file is useful.**
  - Q is a nondeterministic cousin of E03 (zero-progress heads).
  - E07 (burst size caps eligible object size) deserves a line in the ABI doc, since
    the doc already lists the heuristics as non-ABI.

---

## 7. What's good in v6

- **The turnaround is precise.** N2 and R were fixed as proposed.
  - The N2 and restore tests fail on the old image, per the author's runs.
  - The SMP race test exercises real concurrency, even though it didn't catch R on v4.
- **Restoration semantics are now immediate,** and they're tested with same-batch cancel
  and INSPECT.
- **The dispatcher merge removes a work item and a mutex** without regressions, and its
  shutdown ordering (close enrollment under `tables`, then flush) is right.
- **The documentation is cleaner:**
  - provenance and claims moved to a cover letter that states plainly what is not
    claimed;
  - errata and optimizations are kept separately and honestly.
- **Build hygiene is good:** `W=1`, `CONFIG=n`, `NET_SOCK_MSG` and PREEMPT/SMP builds
  are clean, and checkpatch is clean apart from the DCO.

---

## 8. Path to v7

1. **A human `Signed-off-by:` on every patch;** fix the `Assisted-by` format.
2. **Fix Q** (Appendix A, or the alternative rule) and add the two-follower tests.
3. **Move the KUnit Kconfig symbol and Makefile rule into 3/5.**
4. **N1:** add a per-stream capture window and a framing reserve, or explain why an
   application can keep a margin it doesn't control. Add the two-connection test.
5. **Decide the CQ-full semantics for the collector.** A re-armable COLLECT on an
   existing token is my suggestion.
6. **Stop kicking the dispatcher on every publication.**
7. **Carried over from v4:**
   - split framed sends, or justify keeping them with numbers;
   - a CQE32 indication instead of the flag bit;
   - the `tables` mutex;
   - benchmarks.

---

## Appendix A: prototype fix for Q

This is a direction check, not a submission-quality patch. It applies on top of
`8fae2736b062`.

```diff
diff --git a/io_uring/opaque.c b/io_uring/opaque.c
index a06fac1aa967..2d9651d43fe8 100644
--- a/io_uring/opaque.c
+++ b/io_uring/opaque.c
@@ -2056,8 +2056,12 @@ static void io_opaque_tx_put(struct io_opaque_req *op)
 		rb_erase(&tx->node, &store->txs);
 		kfree(tx);
 	} else if (head) {
-		/* A broken object must not splice the next queued object onto TCP. */
-		if (op->progress != op->total_length) {
+		/*
+		 * A broken object must not splice the next queued object onto TCP.
+		 * A follower canceled before it ever transmitted has queued nothing,
+		 * whether or not an earlier head was cleaned up first.
+		 */
+		if (op->tx_started && op->progress != op->total_length) {
 			tx->failed = true;
 			list_for_each_entry(next, &tx->requests, send) {
 				bool queue;
@@ -2110,6 +2114,7 @@ int io_opaque_send(struct io_kiocb *req, unsigned int issue_flags)
 	ret = io_opaque_tx_enter(op, sock->sk);
 	if (ret)
 		goto out;
+	op->tx_started = true;
 
 again:
 	memset(&msg, 0, sizeof(msg));
diff --git a/io_uring/opaque.h b/io_uring/opaque.h
index cd2c478aa665..cb4729cb6dac 100644
--- a/io_uring/opaque.h
+++ b/io_uring/opaque.h
@@ -65,6 +65,8 @@ struct io_opaque_req {
 			u32 total_length;
 			u32 msg_flags;
 			u16 send_flags;
+			/* Reached the queue head and attempted to transmit. */
+			bool tx_started;
 		};
 		struct {
 			struct io_opaque_data *replacement;
```

Results with this patch (same UML SMP image, KASAN + lockdep):

- `repro6 Q`: Q-early and Q-late both give H full, F1 `-ECANCELED`, F2 = 8, with F2's
  bytes on the wire. 5/5 runs each. No kernel diagnostics.
- KUnit 23/23 and selftest 80/80 with no skips, including the 10,000-round SMP SEND race
  (5000 full and 5000 partial heads; 9549 followers canceled, 451 sent). No kernel
  diagnostics.

---

## Appendix B: v6 reproducer

`repro6.c` uses the same `helpers.h` as the v4 reproducer (Appendix B of
`opus_review_v4.md`). Build it statically against the v6 UAPI headers and run it inside
the UML image:

```sh
make ARCH=x86_64 O=$hdr headers_install INSTALL_HDR_PATH=$hdr/usr
gcc -static -O2 -Wall -I$hdr/usr/include -o repro6 repro6.c -pthread
# UML: CONFIG_SMP=y, CONFIG_PREEMPT=y, KASAN, PROVE_LOCKING; hostfs root with an /init
# that mounts proc, sysfs, devtmpfs, debugfs and cgroup2 (+memory), as in v4.
./linux mem=2G seccomp=on ncpus=2 rootfstype=hostfs rootflags=$root rw init=/init \
    TESTS=repro6 ARGS=QN con=null con0=null,fd:1
```

```c
// SPDX-License-Identifier: GPL-2.0
/* Review reproducers for the io_uring OPAQUE_OBJ RFC v6 (origin/upstream/opaque-obj-v6). */
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

/* Submit without IORING_ENTER_GETEVENTS: DEFER_TASKRUN task work stays queued. */
static void submit_only(struct ring *r)
{
	unsigned int pending = *r->sq_tail - *r->sq_head;
	int ret = syscall(__NR_io_uring_enter, r->fd, pending, 0, 0, NULL, 0);

	require(ret >= 0, "submit");
}

static uint64_t cancel_stage(struct ring *r, uint64_t target)
{
	struct io_uring_sqe sqe = {
		.opcode = IORING_OP_ASYNC_CANCEL, .fd = -1,
		.addr = target, .user_data = next_tag++,
	};

	stage(r, sqe);
	return sqe.user_data;
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

static void grow_sndbuf(int fd)
{
	int big = 8U << 20;

	if (setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, &big, sizeof(big)))
		require(!setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big)), "grow sndbuf");
}

/* Read whatever arrives until the socket stays idle for idle_ms. */
static size_t drain_idle(int fd, unsigned char *buf, size_t cap, int idle_ms)
{
	size_t done = 0;

	while (done < cap) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		ssize_t n;

		if (poll(&p, 1, idle_ms) <= 0)
			break;
		n = recv(fd, buf + done, cap - done, MSG_DONTWAIT);
		if (n <= 0)
			break;
		done += n;
	}
	return done;
}

static uint64_t publish(struct ring *r, uint32_t ctx, uint64_t stream, int fd,
			uint64_t offset, const void *data, size_t length)
{
	struct transfer tx = { .fd = fd, .data = data, .length = length };
	struct event ev;
	pthread_t thr;
	uint64_t tag = cmd_stage(r, ctx, IORING_OPAQUE_RECV_OBJECT, stream, offset, length, NULL);

	require(!pthread_create(&thr, NULL, writer, &tx), "producer");
	require(wait_tag_to(r, tag, 30000, &ev) && ev.res == (int)length, "RECV_OBJECT");
	pthread_join(thr, NULL);
	return ev.extra[0];
}

/*
 * Q: H heads the destination queue, F1 and F2 wait behind it. Cancel F1 only.
 * Early: F1 is canceled while H is still blocked. Late: F1 is canceled after
 * H's final POLLOUT wakeup is queued but before the ring runs task work, so
 * H and F1 complete in the same batch and H is cleaned up first.
 */
static void test_follower_cancel(bool late)
{
	struct io_uring_opaque_config cfg = default_cfg();
	const size_t xlen = 300U << 10, total = xlen + 16;
	unsigned char *src = malloc(total), *seen = calloc(1, total + 4096);
	const char *name = late ? "Q-late" : "Q-early";
	struct event eh, e1, e2, ec;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, hx, h1, h2, th, t1, t2, tc;
	size_t got, i, nb = 0, nc = 0;
	int s[2], d[2];

	memset(src, 'A', xlen);
	memset(src + xlen, 'B', 8);
	memset(src + xlen + 8, 'C', 8);
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(s);
	tcp_pair_small(d, 16U << 10, 16U << 10);
	stream = attach(&r, ctx, s[1], COLLECTOR_TAG);
	hx = publish(&r, ctx, stream, s[0], 0, src, xlen);
	h1 = publish(&r, ctx, stream, s[0], xlen, src + xlen, 8);
	h2 = publish(&r, ctx, stream, s[0], xlen + 8, src + xlen + 8, 8);

	th = send_stage(&r, ctx, d[0], hx, 0, xlen);
	t1 = send_stage(&r, ctx, d[0], h1, 0, 8);
	t2 = send_stage(&r, ctx, d[0], h2, 0, 8);
	submit_only(&r);
	usleep(50000);
	if (!late) {
		tc = cancel_stage(&r, t1);
		require(wait_tag_to(&r, tc, 5000, &ec), "cancel CQE");
		require(wait_tag_to(&r, t1, 5000, &e1), "F1 CQE");
	}
	/* Let H's retry queue its whole remainder, then free the destination. */
	grow_sndbuf(d[0]);
	got = drain_idle(d[1], seen, total, 100);
	usleep(50000);
	if (late) {
		tc = cancel_stage(&r, t1);
		submit_only(&r);
	}
	require(wait_tag_to(&r, th, 10000, &eh), "H CQE");
	if (late) {
		require(wait_tag_to(&r, tc, 5000, &ec), "cancel CQE");
		require(wait_tag_to(&r, t1, 5000, &e1), "F1 CQE");
	}
	require(wait_tag_to(&r, t2, 10000, &e2), "F2 CQE");
	got += drain_idle(d[1], seen + got, total + 4096 - got, 300);
	for (i = 0; i < got; i++) {
		nb += seen[i] == 'B';
		nc += seen[i] == 'C';
	}
	printf("[%s] H=%d cancel=%d F1=%d F2=%d; peer got %zu bytes (%zu 'B', %zu 'C')\n",
	       name, eh.res, ec.res, e1.res, e2.res, got, nb, nc);
	printf("[%s] RESULT: %s\n", name,
	       eh.res == (int)xlen && e1.res == -ECANCELED && e2.res == 8 && nc == 8 && !nb ?
	       "ok" : e2.res == -ECANCELED ?
	       "BUG: canceling a waiting follower canceled the next one" : "unexpected");
	ring_exit(&r);
	close(s[0]); close(s[1]); close(d[0]); close(d[1]);
	free(src);
	free(seen);
}

/*
 * N1 (v6): a greedy stream fills the capture allowance with undecided bytes.
 * The application gets the pressure CQE and releases capacity as the errata
 * recommend, but the greedy collector takes it back before another
 * connection's framed reply is admitted.
 */
static void test_greedy_stream(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	struct io_uring_opaque_frame frame = {
		.size = sizeof(frame), .prefix = (uintptr_t)"HDR:", .prefix_length = 4,
	};
	struct burst_writer w = { .burst = 64U << 10, .bursts = 64 };
	struct io_uring_opaque_stat stat;
	struct io_uring_opaque_stream_stat ss;
	struct event ev, pressure = { 0 }, s1, s2;
	struct ring r;
	uint32_t ctx;
	uint64_t sa, sb, hb, hb2, t;
	unsigned int i, notes = 0;
	int a[2], b[2], dst[2];
	pthread_t thr;

	cfg.hard_limit = 256U << 10;
	cfg.compact_headroom = 64U << 10;
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(a);
	tcp_pair(b);
	tcp_pair(dst);
	sb = attach(&r, ctx, b[1], COLLECTOR_TAG + 1);
	hb = publish(&r, ctx, sb, b[0], 0, "request!", 8);
	hb2 = publish(&r, ctx, sb, b[0], 8, "cached!!", 8);
	sa = attach(&r, ctx, a[1], COLLECTOR_TAG);
	w.fd = a[0];
	require(!pthread_create(&thr, NULL, burst_writer_fn, &w), "flood");
	for (i = 0; i < 200 && !notes; i++) {
		if (wait_tag_to(&r, COLLECTOR_TAG, 20, &ev)) {
			if (ev.res == -ENOBUFS && (ev.flags & IORING_CQE_F_MORE)) {
				pressure = ev;
				notes++;
			}
		}
	}
	require(!command(&r, ctx, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res, "stat");
	require(!command(&r, ctx, IORING_OPAQUE_STREAM_STAT, sa, 0, sizeof(ss), &ss).res,
		"stream stat");
	printf("[N1] pressure CQE on greedy stream: res=%d more=%d token_ok=%d offset=%llu; "
	       "backing=%llu/%llu (headroom %llu), greedy undecided=%llu\n",
	       pressure.res, !!(pressure.flags & IORING_CQE_F_MORE), pressure.extra[0] == sa,
	       (unsigned long long)pressure.extra[1], (unsigned long long)stat.backing_bytes,
	       (unsigned long long)cfg.hard_limit, (unsigned long long)cfg.compact_headroom,
	       (unsigned long long)ss.undecided_bytes);
	/* Release capacity as the errata recommend: evict a cached object. */
	require(!command(&r, ctx, IORING_OPAQUE_FREE, hb2, 0, 0, NULL).res, "FREE cached");
	usleep(50000);
	t = frame_stage(&r, ctx, dst[0], hb, 8, &frame, IORING_OPAQUE_SEND_LAST, 0);
	require(wait_tag_to(&r, t, 5000, &s1), "framed reply on the other connection");
	t = frame_stage(&r, ctx, dst[0], 0, 0, &frame, 0, 0);
	require(wait_tag_to(&r, t, 5000, &s2), "generated-only reply");
	require(!command(&r, ctx, IORING_OPAQUE_STREAM_STAT, sa, 0, sizeof(ss), &ss).res,
		"stream stat");
	printf("[N1] after FREE: greedy undecided=%llu; other connection's framed SEND_LAST=%d "
	       "consumed=%d, generated-only=%d\n", (unsigned long long)ss.undecided_bytes,
	       s1.res, !!(s1.flags & IORING_CQE_F_OPAQUE_CONSUMED), s2.res);
	printf("[N1] RESULT: %s\n", s1.res == -ENOBUFS || s2.res == -ENOBUFS ?
	       "LIVENESS: released capacity is recaptured; replies on other connections fail" :
	       "ok");
	shutdown(a[0], SHUT_RDWR);
	pthread_join(thr, NULL);
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, sa, 0, 0, NULL);
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, sb, 0, 0, NULL);
	ring_exit(&r);
	close(a[0]); close(a[1]); close(b[0]); close(b[1]); close(dst[0]); close(dst[1]);
}

static void run(const char *name, void (*fn)(void))
{
	int status;
	pid_t pid = fork();

	if (!pid) {
		alarm(120);
		fn();
		_exit(0);
	}
	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status))
		printf("[%s] test process killed by signal %d\n", name, WTERMSIG(status));
	else if (WEXITSTATUS(status))
		printf("[%s] test process exited with %d\n", name, WEXITSTATUS(status));
}

static void test_q_early(void) { test_follower_cancel(false); }
static void test_q_late(void) { test_follower_cancel(true); }

int main(int argc, char **argv)
{
	const char *only = argc > 1 ? argv[1] : NULL;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (!only || strchr(only, 'Q')) {
		run("Q-early", test_q_early);
		run("Q-late", test_q_late);
	}
	if (!only || strchr(only, 'N'))
		run("N1", test_greedy_stream);
	return 0;
}
```
