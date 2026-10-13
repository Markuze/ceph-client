# OPAQUE_OBJ v9, round 1: gap-stall fix, changelogs and documentation (implementation guide)

**Base:** local branch `opaque-obj-v8-capture` at `de3962f1a12b`: five patches on
`af32da41b032`. All line numbers below refer to that commit.

**Goal.** Four changes, nothing else:

1. Fix a liveness bug in v8's collection gate (G1).
2. Add the regression selftest for it, and one missing positive check in an ABI test.
3. Replace the four changelogs that still describe the removed `capture_window`.
4. Correct and complete the user documentation.

This document is the complete, normative specification. Follow it literally. Every old
block below must be found exactly once; replace it with the new block exactly. Where it
says "do not change", leave that code alone even if it looks related. If something here
seems wrong or impossible, stop and report it instead of improvising.

**How this guide was checked.** I applied every change below to a private prototype of
`de3962f1a12b`, autosquashed into five patches with the exact messages of §5, and ran it
under UML with 2 vCPUs, KASAN, lockdep and PREEMPT:

- KUnit 27/27 and selftests 94/94 with no skips, including all 10,000 SMP SEND races, and
  no kernel diagnostics;
- the new selftest, run against an unfixed v8 kernel, bails out right after `ok 53` with
  `CQ wait deadline`;
- `checkpatch.pl --strict` reports only the missing `Signed-off-by` (and MAINTAINERS
  file-path warnings), and every commit message line is at most 75 columns.

The prototype isn't part of this document; implement from the text.

---

## 1. The bug (G1), so you understand the change

v8 computes `stream->hold_undecided` once per collection pass in `io_opaque_recv()` (line
1328), from `io_opaque_collect_demand()` (lines 899-906). Its R4b term asks whether the
**earliest** pending decision starts beyond `stream->next`.

- A held pass still collects decided bytes. When the earliest decision completes inside
  `io_opaque_actor()` (lines 1135-1139), the next decision becomes the earliest.
- If that decision starts beyond `stream->next`, R4b is now true. But the actor's
  undecided branch breaks on the stale flag (lines 1117-1123), so the gap and the
  decision's bytes stay on the socket.
- The pass ends in `IOU_RETRY`. No new TCP edge arrives for bytes already queued, and
  `io_opaque_wake_stream()` is only called from `io_opaque_range_start()`.
- So the later decision waits until the peer sends more or the application happens to
  issue another decision or a waiting INSPECT. In a request/response protocol, that is a
  deadlock.

**Repro.** Header1 [0,100) is INSPECTed and left undecided. The application declares
RECV_OBJECT [100,1100) and RECV_OBJECT [1200,2200), and the peer writes all 2100 bytes at
once. On v8 the first body completes and the second never does: 1100 bytes stay unread on
the socket. v7 completed both.

**The fix.** In the actor's undecided branch, `op != NULL` already means "the earliest
pending decision starts beyond `stream->next`". That is R4b, evaluated now rather than at
the start of the pass. So the branch breaks only when the gate is held **and** no
decision lies ahead:

- The pass then collects the gap as undecided, up to `op->offset`, then the decision's
  bytes through the decided branch, and holds again at the next undecided byte with
  nothing ahead.
- R1/R3 (the `undecided == 0` term) stay per pass, exactly as before.

---

## 2. Code change, patch 2/5: `io_uring/opaque.c`, `io_opaque_actor()` (lines 1117-1123)

Old:

```c
		} else {
			/* Undecided bytes stay on the socket until someone asks for them. */
			if (stream->hold_undecided)
				break;
			if (op)
				chunk = min_t(u64, chunk, op->offset - stream->next);
		}
```

New:

```c
		} else {
			/*
			 * Undecided bytes stay on the socket until someone asks for them.
			 * A decision further in the stream asks for the gap before it,
			 * including one that became the earliest during this pass.
			 */
			if (stream->hold_undecided && !op)
				break;
			if (op)
				chunk = min_t(u64, chunk, op->offset - stream->next);
		}
```

That is the whole kernel change. Do not change:

- `io_opaque_collect_demand()`, or where `io_opaque_recv()` sets `hold_undecided`;
- `io_opaque_range_start()`, `io_opaque_wake_stream()`, `io_opaque_queue_ready()`;
- any `IOU_RETRY`/`IOU_REQUEUE` logic.

Add no new wakeup. The actor fix makes the same pass collect the gap.

---

## 3. Tests

### 3.1 New selftest, patch 4/5: `tools/testing/selftests/io_uring/opaque_obj.c`

Insert this function after the end of `test_header_larger_than_old_window()` and before
`static size_t drain_idle(...)` (line 3553). Keep one blank line between functions. It
uses only helpers that already exist in v8 (`settle`, `unread_bytes`, `stream_undecided`,
`submit_only`, `cmd_stage`, `wait_tag_ms`).

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

### 3.2 Call it, and update the plan (same file)

In `main()`, line 4222, old:

```c
	test_header_larger_than_old_window(cfg);
```

New:

```c
	test_header_larger_than_old_window(cfg);
	test_gap_after_decision(cfg);
```

Line 3984: `ksft_set_plan(93);` becomes `ksft_set_plan(94);`.

### 3.3 Positive check in `test_watermark_config()` (same file, lines 3914-3918)

Old:

```c
		require(register_store(&r, &bad, &context) == -EINVAL,
			"reject invalid watermark configuration");
	}
	ring_exit(&r);
	ksft_test_result_pass("watermark and reply reserve ABI validation\n");
```

New:

```c
		require(register_store(&r, &bad, &context) == -EINVAL,
			"reject invalid watermark configuration");
	}
	require(!register_store(&r, &cfg, &context),
		"valid watermark configuration registers");
	ring_exit(&r);
	ksft_test_result_pass("watermark and reply reserve ABI validation\n");
```

This adds a `require`, not a test result, so it doesn't change the plan.

### 3.4 KUnit, patch 3/5

`io_uring/opaque_test.c` does not change. A KUnit case for G1 would need a completable
fake request, because a completing decision calls `io_opaque_queue_ready()`, which
dereferences `op->req`. Only patch 3/5's commit message changes (§5).

---

## 4. Documentation, patch 5/5: `Documentation/userspace-api/io_uring-opaque.rst`

Six replacements. Each old block is quoted exactly, line breaks included.

### 4.1 Configuration paragraph (lines 54-62)

Old:

```rst
The optional ``high_watermark``, ``low_watermark`` and ``reply_reserve``
fields define a memory-pressure policy at store creation.
Omitted fields are zero and preserve the shared capture/framing allowance.
Watermarks and reply reserve are page aligned. A nonzero high watermark
requires a lower low watermark and a nonzero reply reserve. The high
watermark cannot exceed
``hard_limit - compact_headroom - reply_reserve``. The reply reserve must be
smaller than ``hard_limit - compact_headroom``. A reply reserve may also be
enabled independently of watermarks.
```

New:

```rst
The optional ``high_watermark``, ``low_watermark`` and ``reply_reserve``
fields define a memory-pressure policy at store creation. Omitted fields are
zero and preserve the shared capture/framing allowance. Watermarks and reply
reserve are page aligned. A nonzero high watermark requires a lower low
watermark and a nonzero reply reserve. The high watermark cannot exceed
``hard_limit - compact_headroom - reply_reserve``. The reply reserve must be
smaller than ``hard_limit - compact_headroom``. A reply reserve may also be
enabled without watermarks. Reserved words must be zero.
```

### 4.2 Collector paragraph (lines 88-104)

Old:

```rst
Collection follows the application's capture policy. When collection is
permitted, one pass may take everything that has arrived, bounded by the
256 KiB batch and the store quota. Undecided bytes are retained in their
original receive buffers and are never copied; only INSPECT copies the
requested header bytes to userspace. While a stream holds undecided bytes,
no further bytes are collected for it: new bytes stay on the TCP socket and
TCP flow control applies until RECV_OBJECT or DISCARD decides the held
range. Two demands reopen collection. An INSPECT waiting for bytes past
what has been collected pulls the remainder of a header split across
arrivals. A decision declared beyond the first undecided byte makes the gap
before it collectable as undecided, so a body may be declared before its
header is decided. INSPECT returns bytes that are already collected; an
application needing more re-INSPECTs at the end offset, which waits and
makes the collector fetch more. There is no separate resume CQE. STREAM_STAT
reports capture and undecided offsets; bytes held on the socket are visible
with ``ioctl(SIOCINQ)``. The next inspection or range completion also
provides application progress.
```

New:

```rst
Collection follows the application's capture policy. When collection is
permitted, one pass may take everything that has arrived, bounded by the
256 KiB batch and the store quota. Undecided bytes stay in their original
receive buffers: the policy never copies them, and only INSPECT copies the
requested header bytes to userspace. The copy fallback described under
"Backing and memory management" still applies. While a stream holds
undecided bytes, no further bytes are collected for it: new bytes stay on
the TCP socket and TCP flow control applies until RECV_OBJECT or DISCARD
decides the held range. Decided bytes are always collected. Two demands
reopen collection. An INSPECT waiting for bytes past what has been
collected pulls the remainder of a header split across arrivals. A decision
that starts beyond the collected bytes makes the gap before it collectable
as undecided, so a body may be declared before its header is decided; this
also holds when that decision becomes the earliest pending one during a
collection pass. INSPECT returns bytes that are already collected; an
application needing more re-INSPECTs at the end offset, which waits and
makes the collector fetch more. While collection is held, a FIN queued
behind held bytes is not consumed, so EOF is reported only after those
bytes are decided or an INSPECT waits past them. Errors and urgent data are
observed on every collector run, held or not. There is no separate resume
CQE. STREAM_STAT reports capture and undecided offsets; bytes held on the
socket are visible with ``ioctl(SIOCINQ)``. The next inspection or range
completion also provides application progress.
```

### 4.3 Undecided backing (lines 366-370)

Old:

```rst
Undecided lookahead retains the original receive-buffer pages in both memory
modes; it does not copy them, and it does not bound private object assembly.
An unsuccessful assembly can restore already admitted bytes. Collection
waits for a consuming decision, a waiting INSPECT or a future-range decision
before adding more undecided bytes to that stream.
```

New:

```rst
Undecided bytes keep their original backing in both memory modes: they are
retained like object ranges when eligible and copied only by the fallback
above. Retained backing is charged by whole allocation, so a short header in
a large receive allocation charges the whole allocation, and many tiny
segments can charge more than their length; the store quota bounds the
total. This does not bound private object assembly. An unsuccessful
assembly restores its admitted bytes as undecided, and the stream then
waits for a decision, a waiting INSPECT or a decision further in the stream
before collecting more.
```

### 4.4 Reply reserve (lines 391-401)

Old:

```rst
The protected allowance permits bounded framed and generated-only replies
even when payload capacity is full. A flooding connection can no longer
refill capacity freed by the application: its undecided bytes hold further
collection until the application decides them. A single collection pass of
up to 256 KiB can still fill a small store, and that is what ``reply_reserve``
is for.
Concurrent snapshots can still exhaust the reply reserve, and allocation or
request admission can fail independently. Framing cannot consume compaction
headroom. With a zero reply reserve, snapshots share ordinary capture capacity;
eviction alone cannot guarantee reply admission because collection can refill
that capacity first.
```

New:

```rst
The protected allowance permits bounded framed and generated-only replies
even when payload capacity is full. A stream that holds undecided bytes
cannot refill capacity freed by the application; it stays held until the
application decides those bytes. Any stream whose collection is permitted
can still take one pass of up to 256 KiB, which can fill a small store
before a reply is admitted; that is what ``reply_reserve`` is for.
Concurrent snapshots can still exhaust the reply reserve, and allocation or
request admission can fail independently. Framing cannot consume compaction
headroom. With a zero reply reserve, snapshots share ordinary capture
capacity, so an eviction guarantees reply admission only while no permitted
collection pass takes the freed capacity first.
```

### 4.5 Selftest summary (lines 539-541)

Old:

```rst
inspection, forwarding-only ownership, stable classification through compaction,
bounded undecided retention and collection holds, split-header demand
collection, and protected replies under capture pressure.
```

New:

```rst
inspection, forwarding-only ownership, stable classification through compaction,
retained undecided bytes and held collection, split headers collected by a
waiting INSPECT, a decision past an uncollected gap, a flooding stream that
cannot refill freed capacity, and protected replies when one collection pass
fills the store.
```

### 4.6 KUnit summary (lines 573-575)

Old:

```rst
Memory-policy cases cover watermark transitions, separate reply accounting,
collection-gate admission and dense lookahead from tiny fragments, and
forwarding-only version replacement and send admission.
```

New:

```rst
Memory-policy cases cover watermark transitions, separate reply accounting,
the collection gate (retained undecided bytes, held passes and decided bytes
through a held gate), and forwarding-only version replacement and send
admission.
```

Change nothing else in the document. In particular, leave the INSPECT section (lines
195-206) alone; whether to change same-offset INSPECT behavior is a separate decision.

---

## 5. Commit messages (final text, verbatim)

- **Patch 1/5 is not touched.** It must keep SHA `fc0538f7d01a`.
- **Patches 2/5 to 5/5 get exactly the messages below,** byte for byte, including the
  blank lines and both trailers. `git log -1 --format=%B <commit>` must print the block
  followed by one empty line.
- **Don't add or remove trailers.** Your `Assisted-by: GLM:glm-5.3` is already present.

### 2/5

```text
io_uring: add opaque TCP stores, framed sends and lowmem forwarding

Register hardware-independent stores through native io_uring. Collect
eligible ordinary TCP backing by reference with bounded copy fallback;
applications own stream offsets, parsing, object boundaries and policy.

INSPECT returns available contiguous framing and arms asynchronously when
none exists. RECV_OBJECT privately assembles a declared complete range and
publishes one handle/readiness CQE; READ_OBJECT copies completed bytes.
Restore unsuccessful private prefixes under the stream lock before their
completion without publishing partial handles.

Collect only bytes the application can decide. A collection pass may take
everything queued when the stream has no undecided bytes; undecided bytes
stay in their receive buffers and only INSPECT copies them. While a stream
has undecided bytes, later bytes stay on the socket under TCP flow control
unless an INSPECT waits past the collected bytes or a decision starts
beyond them. Decided bytes are always collected, and a decision that
becomes the earliest during a pass collects the gap before it.

SEND covers a bounded copied prefix, object subrange and copied suffix
with one request, FIFO position, continuation cursor and progress CQE.
Generated-only replies share that queue. Snapshot, validation and quota
failures precede admission; ordinary sends pin a version and LAST transfers
the handle reference with an independent consumed flag. Only a send that
attempted TCP can fail admitted followers. Canceling an unstarted follower
must preserve its successors regardless of head/follower cleanup order.

Reserve before TCP capture, report nonterminal ENOBUFS with the token and
unread offset, and resume on eligible capacity release. Unadmitted bytes
remain on the socket; known DISCARD ranges can skip without backing.
Bound backing, allocator-bucket framing, metadata and pending requests.

Offer creation-time high/low watermarks and protected reply framing
capacity. Lowmem receive admissions return forwarding-only handles
automatically. Require LAST to transmit them; FREE discards them. Preserve
the classification across compaction and mode changes while existing
cacheable handles remain reusable. Temporary object assembly and
forwarding still obey the hard limit. Zero new fields retain the legacy
shared allowance.

Resolve handles at first issue and preserve old-version references across
FREE/LAST and manual or automatic compaction. Stores support descriptor
export/import and size-versioned registration, queries and policy. No
zcrx, mapped-area, page-pool or busy-poll dependency is introduced.

One delayed compaction dispatcher serializes manual and automatic copies.
Prune ineligible candidates, re-enroll on policy changes and coalesce
publication wakeups without resetting the paced scan deadline. Forwarding
objects are excluded from automatic compaction. Keep allocation retry
independent and drain accepted manual requests during shutdown.

Assisted-by: Codex:GPT-6
Assisted-by: GLM:glm-5.3
```

### 3/5

```text
io_uring: test opaque ownership, memory modes and compaction

Cover RX identity and copy exceptions, shared charges, capture bounds,
range rollback, generation retirement and stable-handle replacement.
Verify available inspection and reusable/LAST version references through
compaction races without restoring consumed handles.

Add watermark hysteresis and protected framing accounting. Check the
collection gate: retained undecided bytes are not copied, a held pass
takes no undecided bytes, and decided bytes pass a held gate. Verify
forwarding-only classification survives replacement and forbids reusable
send admission. The suite has 27 KUnit cases.

Introduce the KUnit option and build rule with the test source so enabling
KUNIT_ALL_TESTS cannot break the preceding implementation patch.

Assisted-by: Codex:GPT-6
Assisted-by: GLM:glm-5.3
```

### 4/5

```text
selftests/io_uring: exercise opaque lowmem, quota and SMP cancellation

Use raw 64-byte SQEs and CQE32/MIXED rings without liburing. Cover short
GET availability and arming races, declared future objects, complete
prefix/body/suffix sends, reusable/LAST ownership, generated-only replies,
snapshot faults and lifetime, compaction, backpressure and partial heads.

Cancel waiting followers before and after head cleanup on one and shared
rings; verify successors remain sendable and consumed handles stay stale.
Exercise watermark validation and hysteresis, lowmem header inspection,
direct DISCARD, immutable forwarding-only ownership and protected framing
when one collection pass fills the store.

Check the collection policy: undecided bytes are retained without copies
and hold later bytes on the socket until a decision; a waiting INSPECT
collects a split header; a decision past an uncollected gap completes
without another arrival; a flooding stream cannot refill capacity freed
by an eviction; and INSPECT sees a header longer than one page.

Keep 10,000 cross-ring SEND cancellation/promotion races with issuing rings
on different CPUs. Verify complete and partial heads, explicit cancel and
linked timeouts, unique completions, wire order and reference drain. Keep
10,000 separate receive arrival/drain cycles and 4 GiB DISCARD through TCP
sequence wrap. These establish correctness, not performance or measured
scheduler-wakeup counts.

Cover blocked IPv4/IPv6 sends during rejected disconnect, same-batch
receive cancellation and inspection, pressure coalescing and unread TCP
bytes, cross-ring resume, CQ pressure, versioned UAPI and fault injection.
The suite has 94 groups. Make its alarm configurable through
OPAQUE_TEST_TIMEOUT and propagate it through the diagnostic VM helper.

Assisted-by: Codex:GPT-6
Assisted-by: GLM:glm-5.3
```

### 5/5

```text
docs: describe opaque forwarding, memory and ownership contracts

Specify store sharing, available asynchronous inspection, complete-object
readiness and immutable reads. Document one-command framed SEND, LAST
admission and its independent consumed indication, partial progress and
queued-follower cancellation semantics.

Explain the collection policy: what one pass may take, retained undecided
bytes, held collection and its two demand exceptions, and EOF behind held
bytes. Explain optional high/low watermarks, protected framing capacity
and automatic forwarding-only admission. Preserve handle classification
through replacement and mode transitions. Describe actual backing
charges, including whole-allocation charges for retained undecided bytes,
temporary forwarding capacity and hard-limit backpressure without
promising unbounded header or forwarding progress.

Document nonterminal pressure, reservation before capture, coalescing and
resume conditions, and terminal CQ failure without a collector rearm API.
Explain stable-handle compaction, candidate pruning, paced publication
wakeups, worker lifetime and reproducible debug VM testing with adjustable
test and host timeouts.

Keep prototype provenance, performance hypotheses and baseline discussion
in the cover letter and separate research records.

Assisted-by: Codex:GPT-6
Assisted-by: GLM:glm-5.3
```

---

## 6. Do not change

- Patch 1/5 (`net/`, `include/net/`, `include/linux/net.h`).
- `io_uring/opaque_test.c`, `io_uring/opaque_internal.h`, `io_uring/opaque.h`,
  `io_uring/opaque_compact.c`, `io_uring/opaque_register.c`, and the UAPI header.
- Everything in `io_uring/opaque.c` except the block in §2.
- Every selftest except the three edits in §3, the plan count and the new function.
- The rest of the documentation, including the INSPECT section.
- The open v7 review items L (compaction latches LOWMEM) and R (`reply_reserve` is a cap).
  They are a later round.

---

## 7. Pitfalls

1. **The fix is `&& !op` in the actor, nothing more.** Don't change
   `io_opaque_collect_demand()` or re-evaluate the flag elsewhere. Don't add a wakeup in
   `io_opaque_queue_ready()` or in the actor, and don't return `IOU_REQUEUE`.
2. **Keep the `if (op)` chunk limit.** The gap must stop at `op->offset`, so the
   decision's own bytes go through the decided branch.
3. **The plan is 94.** The new function goes after
   `test_header_larger_than_old_window()`, and its call goes right after that test's call
   in `main()`.
4. **The messages are verbatim,** including trailers.
   - `git commit --amend --cleanup=verbatim -F <file>` with the exact text works.
   - Don't reflow it; checkpatch warns above 75 columns, and the text is already inside
     that limit.
5. **Diagnostics are reported, never suppressed.**
   - Every `BUG:`, `WARNING:`, `KASAN:`, `refcount_t:`, lockdep or kmemleak report must be
     quoted in your report and attributed from its backtrace.
   - Never disable a detector, rebuild without it and call the report gone.
   - The acceptance config does not need `DEBUG_KMEMLEAK`; don't enable it. If you do
     enable it, two things are known UML noise and need no investigation:
     - a boot "suspicious RCU usage ... Callback enqueued on offline CPU" splat from
       `kmemleak_free` via `os_start_secondary`;
     - about 180-192 kmemleak suspects allocated by `swapper/0` in `ssl_init` →
       `setup_one_line` / `parse_chan` / `pty_chan_init`.

     Anything else is yours to explain.
6. **KUnit runs at boot without `kunit.enable=1`,** because the config has
   `CONFIG_KUNIT_DEFAULT_ENABLED=y`. Adding it is harmless; don't claim it is required.

---

## 8. Acceptance criteria

1. **Shape.** `git range-diff af32da41b032..de3962f1a12b af32da41b032..HEAD` shows:
   - 1/5 `=`;
   - 2/5: the §2 hunk plus the message;
   - 3/5: the message only;
   - 4/5: the §3 hunks plus the message;
   - 5/5: the six §4 hunks plus the message.
2. **Builds.**
   - Every commit with `CONFIG_IO_URING_OPAQUE_OBJ=y`,
     `CONFIG_IO_URING_OPAQUE_OBJ_KUNIT_TEST=y` and `CONFIG_KUNIT_ALL_TESTS=y`, forced on at
     each commit where the symbol exists. `olddefconfig` silently drops it at 1/5.
   - `W=1` clean on `io_uring/` and the changed `net/` objects.
   - `CONFIG_IO_URING_OPAQUE_OBJ=n`.
   - `ARCH=i386` with `HIGHMEM4G`.
3. **checkpatch.** `scripts/checkpatch.pl --strict` reports only the missing
   `Signed-off-by` and the MAINTAINERS file-path warnings.
4. **KUnit.** `# Totals: pass:27 fail:0 skip:0 total:27`.
5. **Selftests.** `# Totals: pass:94 fail:0 xfail:0 xpass:0 skip:0 error:0` under KASAN,
   lockdep, PREEMPT and 2 vCPUs, with the `SEND races:` line printed and no kernel
   diagnostics.
6. **Fails before the fix.** The new selftest binary, run against a kernel built from
   `de3962f1a12b`, prints `ok 53 ...` and then `Bail out! CQ wait deadline`.
7. **Text checks.** Both must print nothing:

   ```sh
   grep -n -e 'dense lookahead' -e 'never copied' -e 'first undecided byte' \
        -e 'eviction alone' -e 'bounded undecided' \
        Documentation/userspace-api/io_uring-opaque.rst
   git log --format=%B af32da41b032..HEAD | \
        grep -n -i -e 'capture-window' -e 'dense' -e '90 groups' -e 'refills'
   ```
