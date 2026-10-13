# Review: io_uring opaque TCP payload objects, v8 (capture policy)

**Series reviewed:** local branch `opaque-obj-v8-capture` at `de3962f1a12b`, 5 patches on top
of `af32da41b032` ("Merge tag 'net-7.3-rc7'"):

| # | Commit | Subject |
|---|--------|---------|
| 1/5 | `fc0538f7d01a` | net: add an optional exclusive TCP receive claim (unchanged from v7) |
| 2/5 | `24604c740774` | io_uring: add opaque TCP stores, framed sends and lowmem forwarding |
| 3/5 | `18ba715bb8dd` | io_uring: test opaque ownership, memory modes and compaction |
| 4/5 | `07aa0f788365` | selftests/io_uring: exercise opaque lowmem, quota and SMP cancellation |
| 5/5 | `de3962f1a12b` | docs: describe opaque forwarding, memory and ownership contracts |

Context:

- v8 is v7 (`e6aef56bc0f6`) plus the capture-policy change specified in
  `opus_capture_policy_changes.md`, squashed into the owning patches. GLM-5.3 implemented
  it from `glm_prompt.md` in one Crush session (2026-10-12, 14:39-15:54 UTC; transcript in
  `.crush/crush.db`, build and test artifacts in `/mnt/workspace/opaque-vm`). Appendix B
  assesses that work.
- My unsquashed reference implementation of the same spec is the local branch
  `opaque-obj-v7.1` (`307cb8dc7b07` + `d0ef89ccecea`). Its kernel code and KUnit test are
  identical to v8. Its selftests differ only in placement and one extra assertion, and
  its documentation differs in wording.
- All `file:line` references are to `de3962f1a12b`. As before, this is written as an
  io_uring maintainer reviewing an RFC would write it.

---

## 1. Verdict

**The capture policy works for the cases it was designed around. But v8 has one new
liveness bug, inherited from the spec, and its changelogs and docs are out of date. Not
postable as is. The fix is one line.**

What v8 gets right:

- **N1 is fixed under the default config.** No new knob is needed. With a zero memory
  policy, the flooding stream stops at 32,741 undecided bytes and posts no pressure CQE.
  Framed SEND_LAST returns 12 and the generated-only reply returns 4. v7 failed this case
  by design.
- **The `capture_window` costs are gone.** Its copies, header-size limit and window-full
  pressure CQEs are removed, and the field is a validated reserved word. A 16 KiB
  undecided arrival is retained with 0 bytes copied, and a 6000-byte header is fully
  visible to INSPECT.
- **The kernel change is small and contained.** It adds one flag, one predicate, a
  restructured actor branch and one wake condition. The send, compaction and watermark
  paths are untouched, which I checked by diff.
- **Error, EOF and urgent-data handling is preserved for held streams.** RST is observed
  while bytes are held (terminal CQE `-ECONNRESET`). EOF is reported once the held bytes
  are decided.

What needs work:

1. **G1 [high]: a decision past an uncollected gap can wait forever (§4.1).** A held pass
   that completes the earliest decision does not notice that the next decision now makes
   the gap collectable. The remaining bytes are already queued, so no wakeup comes.
   - This is R4b's own use case ("declare a body before deciding its header"), with two
     bodies.
   - v7 completes it; v8 hangs until the peer sends more or the application issues an
     unrelated INSPECT.
   - One-line fix, verified with the full suite and a new regression test.
2. **D1-D3 [low to medium, docs] (§4.2-4.4).**
   - The reply-reserve paragraph contradicts itself.
   - "Never copied" is overstated: the safety copies remain.
   - The KUnit summary still lists "dense lookahead".
   - Two of the spec's three documented consequences are missing: EOF behind held bytes,
     and whole-allocation charging.
3. **D4 [medium for posting]: four changelogs still describe the removed window and say
   "90 groups" (§4.5).**
4. **API sharp edge (§4.6).** Re-INSPECTing the same offset with a larger buffer never
   returns more bytes; only an INSPECT at the end offset pulls more. The doc recommends
   the end-offset idiom, but the other idiom now silently stops working, which is a
   change from v7.

Carried from v7, unchanged and still reproducing on v8: **L** (compaction latches LOWMEM)
and **R** (`reply_reserve` is a cap, not a floor). There is still no `Signed-off-by:`.

I found no memory-safety problems. KASAN, lockdep, PREEMPT and 2-vCPU SMP runs are clean,
and kmemleak attributes nothing to io_uring or opaque in v8 or v7 (§4.7).

---

## 2. What I ran

| Check | Result |
|-------|--------|
| GLM's build matrix, checked from its artifacts | each build tree's `kernel.release` is the matching SHA. `IO_URING_OPAQUE_OBJ=y` from 2/5, the KUnit option from 3/5, `KUNIT_ALL_TESTS=y`, 0 warnings. `=n`, i386 `HIGHMEM4G` (native i386, minimal config) and `W=1` objects for `io_uring/` and the changed `net/` files exist |
| `checkpatch.pl --strict`, re-run | only `Missing Signed-off-by` on every patch, plus MAINTAINERS warnings on 2, 3 and 5 |
| v8 and v7 UML builds (GLM's acceptance config plus `DEBUG_KMEMLEAK`, manual scans) | build, 0 warnings |
| KUnit `io_uring-opaque`, v8 | **27/27**; kmemleak scan after boot and KUnit: **0 suspects** |
| `opaque_obj` selftest, v8, kmemleak kernel | **93/93, skip 0**; KUnit 27/27; SEND races full=5000 partial=5000 canceled=9657 sent=343 expired=5000. The only diagnostic is the boot-time RCU splat that kmemleak causes on UML SMP (`kmemleak_free` from `os_start_secondary`; v7 shows it too). Final kmemleak scan: 192 suspects, **all UML serial-line allocations from boot** (`ssl_init` → `setup_one_line`/`parse_chan`/`pty_chan_init`, pid 1); **none from io_uring, opaque, TCP or the test processes** |
| Same, v7 baseline | 90/90, KUnit 27/27, the same boot splat. Final scan: 181 suspects, the same UML boot allocations; none from io_uring or opaque |
| Probes P1-P4 (§4.1, §4.6, Appendix A), v8 vs v7 | P1 **stalls on v8**, completes on v7; P2-P4 as tabulated below |
| `repro7` ported to the v8 UAPI (N1, L, R) | **N1 default: fixed**; N1 with reserve: ok; **L, R: still reproduce** |
| v8 + G1 fix (`hold_undecided && !op`), GLM's acceptance config | P1 passes. **KUnit 27/27, selftests 93/93, skip 0**; SEND races full=5000 partial=5000 canceled=9565 sent=435 expired=5000; no kernel diagnostics |
| New regression test `test_gap_after_decision()` (Appendix A) | **fails on v8** ("CQ wait deadline"); **passes with the fix** |

Probe results, as bytes and completion results:

| Case | v7 | v8 | v8 + fix |
|------|----|----|----------|
| P1: header1 [0,100) undecided; RECV_OBJECT [100,1100) and [1200,2200); peer writes 2100 bytes at once | body2 = 1000, unread 0, undecided 200 | body1 = 1000, **body2: no CQE in 3 s**; unread 1100, undecided 100, `rx_next` 1100. An INSPECT(1100) unsticks it | body2 = 1000, unread 0, undecided 200 |
| P2: header 100 + 100 bytes in two arrivals; INSPECT(0, 512) twice | 100, then 200 | **100, then 100** (100 unread; never grows) | same as v8 (by design) |
| P3: 16 KiB undecided + 16 KiB held; peer RST | terminal `-ECONNRESET` | 16,384 held; terminal `-ECONNRESET` | - |
| P4: as P3, but FIN instead of RST | EOF before any decision | **no EOF while held**; DISCARD(32 KiB) = 32768, then terminal CQE res 0 | - |
| repro7 N1, default config | fails (v7 review) | framed 12, generated-only 4; flooding stream holds at 32,741 undecided, 0 pressure CQEs | - |
| repro7 L | latches | backing 3,346,432 → 3,280,896, **NORMAL → LOWMEM**; next object `FORWARD_ONLY`, reusable SEND `-EOPNOTSUPP` | - |
| repro7 R, 8 KiB reserve | fails | framing 8192, backing 1,114,112 of 8 MiB; **healthy 4-byte reply `-ENOBUFS`**. Legacy (no reserve): 10 | - |

---

## 3. Status of the v7 review

| v7 item | v8 status | Evidence |
|---------|-----------|----------|
| N1 under the default config ("documented legacy behavior") | **Fixed** by R3: a stream with undecided bytes can't refill freed capacity | repro7 N1 default; new selftest `test_greedy_default_config` |
| `capture_window` copy cost (C), header > window (W), window-full pressure CQE | **Resolved by removal**; `__resv2` must be zero (`opaque.c:427-428`) | new selftests 50 and 53 (copied 0; 6000-byte header) |
| L: compaction latches LOWMEM | **Open**, unchanged | repro7 L, §2 |
| R: `reply_reserve` caps framing | **Open**, unchanged | repro7 R, §2 |
| Forwarding-only removes fan-out/retry without bounding memory | **Open**, unchanged | code unchanged |
| UAPI reserved words | Partly: `capture_window` → `__resv2`, validated | `include/uapi/linux/io_uring/opaque.h:36` |
| `Signed-off-by:` | **Still missing** | checkpatch |
| Generic CQE flag, `tables` mutex, collector rearm (E08), benchmarks | Deferred by the author | - |

---

## 4. Findings in v8

### 4.1 G1. [high, correctness] A held pass doesn't collect the gap before a decision that becomes the earliest during that pass

**Mechanism.**

- `io_opaque_recv()` computes `hold_undecided` once per pass (`opaque.c:1328`) from
  `io_opaque_collect_demand()` (`opaque.c:899-906`). The R4b term asks whether the
  *earliest* pending decision starts beyond `stream->next`.
- A held pass still collects decided bytes. In `io_opaque_actor()`, when the earliest
  decision completes (`opaque.c:1135-1139`), the next decision becomes the earliest.
- If that decision starts beyond `stream->next`, R4b now holds. But the else branch breaks
  on the stale flag (`opaque.c:1117-1120`), so the gap and everything after it stay on the
  socket.
- The pass returns the decided bytes and ends in `IOU_RETRY` (`opaque.c:1387-1388`). The
  bytes are already queued, so no new TCP edge arrives.
- The only other collector wakeup is `io_opaque_wake_stream()`, and its only caller is
  `io_opaque_range_start()` (`opaque.c:1661-1663`).
- The later decision therefore waits until the peer sends more or the application issues
  another decision or a waiting INSPECT. In a request/response protocol the peer is
  waiting for the reply, so the connection deadlocks.

**Repro.** P1 / `test_gap_after_decision()` in Appendix A:

1. Header1 [0,100) is INSPECTed and left undecided.
2. The application declares RECV_OBJECT [100,1100) and RECV_OBJECT [1200,2200). The second
   header [1100,1200) is the gap.
3. The peer writes all 2100 bytes in one `write()`.

v8 completes the first body and stalls the second, with 1100 bytes unread on the socket.
v7 completes both. This is the R4b scenario from spec §1 ("keeps today's ability to
declare a body before deciding its header"), with two bodies instead of one.

**Fix.** Keep R1/R3 per pass, but apply R4b per chunk. In this branch, `op != NULL`
already means "the earliest decision starts beyond `stream->next`":

```diff
 		} else {
-			/* Undecided bytes stay on the socket until someone asks for them. */
-			if (stream->hold_undecided)
+			/*
+			 * Undecided bytes stay on the socket until someone asks for them.
+			 * A decision further in the stream asks for the gap before it,
+			 * including one that became the earliest during this pass.
+			 */
+			if (stream->hold_undecided && !op)
 				break;
 			if (op)
 				chunk = min_t(u64, chunk, op->offset - stream->next);
 		}
```

With the fix:

- The pass collects the gap as undecided, then the decision, then holds again at the next
  undecided byte with no decision ahead.
- The KUnit `opaque_collect_gate` expectations are unchanged: its held step has no
  decision, and its DISCARD starts at `stream->next`.
- Verified: P1 and the new selftest pass, and the full suite is unchanged (KUnit 27/27,
  selftests 93/93, SEND races, no diagnostics; §2).

A KUnit case would need a completable fake request, because a completing decision calls
`io_opaque_queue_ready()`. The selftest is the better regression test.

**Root cause.** The spec's "evaluate the gate once per collection pass" rule and its
pitfall 1. Both are right for the R1/R3 term (`undecided == 0`), which must not be
re-evaluated after the first undecided chunk. Neither applies to R4b, which can only
become true during a pass. I wrote that spec; v8 implements it faithfully.

### 4.2 D1. [low to medium, docs] The reply-reserve paragraph contradicts itself

`Documentation/userspace-api/io_uring-opaque.rst:391-401` now says both:

- "A flooding connection can no longer refill capacity freed by the application", and
- the retained v7 sentence: "With a zero reply reserve, ... eviction alone cannot guarantee
  reply admission because collection can refill that capacity first."

The second sentence is still true in a narrower sense: an idle stream's first pass, an
R4 demand, or another stream can take the freed capacity. As written, though, it
describes the old N1 behavior. Suggested replacement: "A held stream cannot refill
capacity released by an eviction, but one collection batch, on any stream whose
collection is permitted, can still fill a small store before a reply is admitted; that is
what `reply_reserve` is for."

### 4.3 D2. [low, docs] Overstatements and stale text

- **"Never copied" is too strong.** `rst:90-91` says "are never copied; only INSPECT
  copies", and `rst:366-367` says "it does not copy them". The safety copy fallback still
  copies undecided bytes:
  - shared or cloned skbs and shared frags (`opaque.c:1010`);
  - locked heads (`opaque.c:1015`);
  - compound pages over the limit (`opaque.c:1020`, `:1040`);
  - quota and extent fallback (`opaque.c:1027-1028`, `:1044-1045`).

  Suggested wording: "not copied by the capture policy; only the safety fallback described
  below copies them".
- **The R4b sentence names the wrong boundary.** `rst:97` says "A decision declared beyond
  the first undecided byte"; the code tests `stream->next`, the first byte *not yet
  collected* (`opaque.c:905`). Spec §4.3 used the same imprecise phrase; spec §1 is
  correct.
- **The KUnit summary is stale.** `rst:574` still lists "dense lookahead from tiny
  fragments", but `opaque_capture_window` was replaced by `opaque_collect_gate`.
- **"Bounded" is misleading.** `rst:540` says "bounded undecided retention"; there is no
  per-stream byte bound besides the batch and the quota. Suggested: "retained undecided
  bytes and held collection".

### 4.4 D3. [low, docs] The spec's §6 consequences are only partly documented

The spec asked for three consequences to be documented. Only the first is in the rst:

1. **One pass can fill a small store.** Documented (`rst:394-396`).
2. **EOF behind held bytes.** Missing. P4 shows no terminal CQE while a FIN is queued
   behind held bytes; it arrives after the DISCARD. An application that waits for EOF
   before deciding will wait forever. Say that it must decide, or INSPECT at the end
   offset.
3. **Retained undecided backing is charged by whole allocation.** Missing. A 1-byte header
   in a page-frag compound page charges the whole page. This matters for LOWMEM because
   v7's window used to pack headers there. The quota still bounds it; say so, and note
   that a charge-based per-pass cap is a possible follow-up.

### 4.5 D4. [medium for posting] Four changelogs describe the removed window

The fixups were squashed, but the messages were only amended to add a trailer:

- **2/5:** "... protected reply framing capacity and dense bounded undecided lookahead."
  There is no paragraph on the collection gate (R1-R4) or on reserving `capture_window`.
- **3/5:** "... capture-window admission and packed lookahead from tiny fragments."
- **4/5:** "... protected framing after a greedy collector refills an evicted cache object's
  capacity". The test now asserts the opposite, and "The suite has 90 groups" should be 93.
- **5/5:** "... bounded dense lookahead ...".

A reviewer comparing changelog and diff will flag all four.

### 4.6 API note [design]: INSPECT at the same offset never pulls more bytes

P2: a header arrives as 100 + 100 bytes.

- `INSPECT(0, 512)` returns 100 both times on v8, with the second part held on the socket.
- On v7 the second call returns 200.
- On v8, only an INSPECT at the end offset (100) waits, which triggers R4a and pulls the
  rest.

This is deliberate. The INSPECT section has always recommended the right idiom ("inspect
its next missing offset and resume on that CQE", `rst:199-200`), and the collector
paragraph now says so too (`rst:99-101`).

What changed silently is the other idiom. Re-INSPECTing the header start with a larger
length used to work eventually, because v7 collected everything; now it returns the same
bytes forever, with no error and no CQE that says why. Consumers to audit are the
zc_proxy experiments and the planned ceph-fuse read path.

Options for the owner:

- add one sentence to the INSPECT section: "while a stream has undecided bytes,
  re-inspecting an offset never returns more than is already collected"; or
- treat a short INSPECT on a held stream as a one-pass demand.

The second option weakens R3 for applications that always INSPECT with large buffers.

### 4.7 kmemleak

**No leaks attributable to the series.** I rebuilt v8 and v7 with GLM's acceptance config
plus `DEBUG_KMEMLEAK` (automatic scanning off). In each run, init scanned after boot and
KUnit, cleared, ran the full selftest, then scanned twice and dumped the report.

| Kernel | Boot + KUnit scan | After the full selftest |
|--------|-------------------|-------------------------|
| v8 | 0 | 192 suspects, all from boot: 64 × `kstrdup` in `setup_one_line`, 64 × `parse_chan`, 64 × `pty_chan_init`, all `ssl_init`, pid 1 |
| v7 | 0 | 181 suspects of the same three kinds (53 + 64 + 64) |

None of the suspects has an io_uring, opaque, TCP or skb frame in its allocation trace.
The objects are UML's serial-line channel setup. Whether a given scan flags them varies
from run to run (181 vs 192), so they are noise, not a regression.

This also settles GLM's first run (Appendix B). It printed "119 new suspected memory leaks"
and later "60 new" during the suite, from a kernel with automatic scanning on. Those 179
are consistent with the same ≤192 boot objects. So nothing was hidden, but GLM did not
establish that.

---

## 5. Smaller notes

- **Cosmetic reflows.** The reserved-field check was rewrapped to 80 columns
  (`opaque.c:427-428`), which is fine. The config paragraph reflow leaves a short line
  (`rst:58-59`).
- **The watermark ABI test has no positive case.** `test_watermark_config()` lost the v7
  "valid config needs a window" angle, and nothing in it registers the valid base
  configuration. `test_lowmem()` covers registration of a windowless watermark config, so
  coverage exists, just not in the ABI test. My reference adds
  `require(!register_store(&r, &cfg, &context), ...)` after the loop.
- **Expect more collector runs on held streams.** Each new arrival triggers a short
  collector pass (gate, `lock_sock`, actor returns 0). No new wakeup sources exist, so
  this can't spin, and TCP flow control bounds it.

---

## 6. What's good in v8

- **The policy is implemented without new UAPI.** One field becomes reserved and is
  validated; nothing else changes.
- **Default-config N1 is fixed**, and the window's copy and header-size costs are gone
  before that ABI could freeze.
- **The safety copy fallback is intact,** and the KUnit gate test checks that retained
  undecided bytes are not copied.
- **The new selftests encode the policy.** G, S, N1-default and the long header each fail
  on v7 at the policy assertion (per the spec) and pass on v8.
- **The series is still clean:** KASAN, lockdep, PREEMPT, SMP, 10,000 SEND races,
  bisectable with `KUNIT_ALL_TESTS`, `W=1`, `=n` and i386.

---

## 7. Path to v9

1. **G1:** apply the one-line actor change and add `test_gap_after_decision()`
   (Appendix A). The plan count becomes 94.
2. **D4:** rewrite the 2/5-5/5 changelogs:
   - 2/5: describe R1-R4 and the reserved field;
   - 3/5: say "collection gate";
   - 4/5: list the four new tests and the rewritten reply-reserve test, and say 93 (94
     with G1).
3. **D1-D3:** fix the reply-reserve paragraph, the "never copied" claims and the stale
   summaries. Document EOF behind held bytes and whole-allocation charging.
4. **§4.6:** decide the same-offset INSPECT behavior; at minimum add the one-sentence
   warning to the INSPECT section.
5. **L and R** from v7; then the human `Signed-off-by:`.

---

## Appendix A: G1 regression test

Insert it after `test_header_larger_than_old_window()` in
`tools/testing/selftests/io_uring/opaque_obj.c`, call it from `main()` after that test,
and change `ksft_set_plan(93)` to `ksft_set_plan(94)`. It uses only the helpers v8 already
has. On v8 it fails at "CQ wait deadline"; with the fix it passes.

```c
/* A decision that becomes the earliest during a held pass collects the gap before it. */
static void test_gap_after_decision(struct io_uring_opaque_config cfg)
{
	unsigned char header[100], wire[2100], buf[128];
	struct ring r;
	uint32_t context;
	uint64_t stream, first, second;
	int pair[2];

	memset(header, 'H', sizeof(header));
	memset(wire, 'B', sizeof(wire));
	require(!ring_init(&r, IORING_SETUP_CQE32) && !register_store(&r, &cfg, &context),
		"gap decision store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	write_all(pair[0], header, sizeof(header));
	settle(&r);
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, sizeof(buf),
			buf).res == sizeof(header), "inspect undecided header");
	/* Two bodies with an uncollected header between them; all bytes arrive at once. */
	first = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 100, 1000, NULL);
	second = cmd_stage(&r, context, IORING_OPAQUE_RECV_OBJECT, stream, 1200, 1000, NULL);
	submit_only(&r);
	write_all(pair[0], wire, sizeof(wire));
	settle(&r);
	require(wait_tag_ms(&r, first, 5000).res == 1000, "first declared body");
	require(wait_tag_ms(&r, second, 5000).res == 1000,
		"later body completes without another arrival");
	require(!unread_bytes(pair[1]) && stream_undecided(&r, context, stream) == 200,
		"gap collected as undecided");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"gap decision close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("a decision past an uncollected gap completes in a held pass\n");
}
```

The P2-P4 probes are the same pattern: P2 re-INSPECTs offset 0 after a second 100-byte
arrival; P3 and P4 hold 16 KiB behind 16 KiB undecided and then send RST
(`SO_LINGER` 0) or `shutdown(SHUT_WR)`.

---

## Appendix B: how well GLM-5.3 did the v7 → v8 change

Sources:

- the Crush transcript (`.crush/crush.db`, session "Reading glm_promp.md File", model
  `glm-5.3`, 14:39-15:54 UTC);
- GLM's artifacts in `/mnt/workspace/opaque-vm`: per-commit build trees, logs, the
  first-run console output stored in the transcript, and `final-run.log`;
- my own re-runs (§2).

**Summary.** A fast, literal and honest executor of a tightly specified change with a
real verification habit. It is weaker wherever judgment was needed and the spec was
silent: diagnostic triage, prose, and changelogs.

| Area | What happened | Grade |
|------|---------------|-------|
| Spec fidelity | Kernel code matches §3 exactly (one disclosed 80-column rewrap). KUnit and selftests are byte-identical to Appendices A-C, placed where the spec said, and every §3.7 `capture_window` reference is gone. No unrequested changes | excellent |
| Workflow | Read the whole spec and every cited code site before editing. Used `--fixup`, then autosquash. Added `Assisted-by: GLM:glm-5.3` only to the four patches it changed (via `rebase --exec`), and no Signed-off-by | excellent |
| Verification | Ran everything in the acceptance list, and the artifacts confirm it: per-commit trees at the right SHAs with the feature forced on, `W=1` objects, `=n`, i386 `HIGHMEM4G` (minimal config, disclosed), checkpatch, range-diff, and UML SMP/KASAN/lockdep with KUnit 27/27, selftests 93/93 and the SEND-race line. My re-runs agree | very good |
| Self-correction | Caught its own stray `DEBUG_KMEMLEAK`. Refused to report 90 pass / 3 skip as done; enabled `IPV6` and `FAULT_INJECTION_STACKTRACE_FILTER` to reach skip:0 | good |
| Diagnostic triage | The first full run printed "119 new suspected memory leaks", then "60 new". GLM never read a backtrace and concluded "with kmemleak off (final build), gone". The final report mentions only the boot RCU splat, not the leak reports. My kmemleak runs (§4.7) show the reports were UML boot noise, so nothing was hidden. That was luck: the reasoning was invalid, and leaving the reports out of the report was wrong | poor |
| Factual claims | "The spec's cmdline alone never runs KUnit" is false: the config has `KUNIT_DEFAULT_ENABLED=y`. Harmless, but asserted without checking | minor |
| Prose | Doc edits were driven by `grep window\|lookahead\|greedy`. They kept a sentence contradicting the new N1 text, left "dense lookahead" in the KUnit summary, overclaimed "never copied", and documented one of the three §6 consequences | fair |
| Changelogs | It amended all four messages to add its trailer, but left them describing the removed window and "90 groups", and did not flag the conflict with "change nothing outside the spec" | poor |
| Design critique | Did not find G1. It was told to follow the spec literally, and the spec's author missed G1 too, so this is not held against it | n/a |
| Speed | Implementation took about 8 minutes (14:39:34 to 14:47:59), and the full verification about 66, with parallel worktree builds. It needed no clarifying questions | excellent |

**For future handoffs:**

- Keep handing over executable specs (code, tests, verification recipe, acceptance
  numbers); GLM-5.3 executes them exactly and verifies honestly.
- Specify the parts it won't infer:
  - the changelog text, or a rule to rewrite every sentence the change invalidates;
  - exact replacement paragraphs for the docs;
  - a diagnostic rule: "every kmemleak/WARN/KASAN report must be attributed by
    backtrace; never disable a detector to make a report go away".
- Keep an independent review pass after it. This one found G1 and D1-D4.
