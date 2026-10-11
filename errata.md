<!-- SPDX-License-Identifier: GPL-2.0 -->

# OPAQUE_OBJ errata and deferred follow-up

Recorded 2026-10-10 against branch `upstream/opaque-obj-v4`, implementation
commit `c5bcea485987e5faaba3a6cb1ad933a3344a2a0c`.

The original seven findings concern resource exhaustion, failure recovery, teardown,
ownership and policy configuration.
Some are documented contracts that need better application support rather
than implementation bugs. They do not require an immediate redesign; revisit
them during KVS/forwarder integration and before freezing affected ABI
semantics. Quota exhaustion can be routine for a cache near capacity, so it
should be exercised in the application baseline work.

Updated 2026-10-11 for v5 preparation, tested implementation
`2cb5da9c7cda1eb0b349030260740e4af034f9f7` on `work/opaque-obj-v5`.
The receive-pressure part of E01 follows the accepted policy below, and small
framing allocations now use their allocator bucket. Saturation/resource-policy
work in E01 and E02-E07 remain **open, deferred follow-up work**.

V6 scheduling cleanup, 2026-10-11: published source
`8fae2736b0626305c6dc8e70ad99f7685eb704bb`, identical to tested development
`d3ea2b3ddf5766ef9e7ed2676a42d3786b24a964`. Manual and automatic compaction
share one dispatcher; receive allocation retry remains independent. Objects
ineligible under the current policy no longer keep an idle scan timer alive,
and policy changes re-enroll them. This does not remove E07's burst limit or
resolve the deferred application ownership/resource issues. Validation passes
23 KUnit / 80 ABI groups on KASAN/lockdep and 23 / 78 with two expected fault
skips on the normal kernel, including policy disable, shared-ring compaction
and queued teardown. No ABI or performance claim is added.

V7 review response, 2026-10-11: source
`e6aef56bc0f64b00261ade7985f159947b23fb1c` on `upstream/opaque-obj-v7`,
with the same tree as development `057e2f53c3f403713bd76fb7694bb9cfe53dff34`.
Fix Q: an unstarted canceled SEND follower cannot poison its successors when
cleanup order changes. Fix B: the KUnit option/build rule arrive with the
source in patch 3. Publication coalesces paced automatic-compaction wakeups.
E01 has an optional reply reserve, dense bounded undecided capture, and
normal/lowmem admission. Forwarding-only objects require LAST or FREE and
keep that classification through compaction and a return to normal.
Zero policy fields retain the legacy shared allowance. E08 records the
collector's missing rearm operation after terminal CQ pressure.

This file records observed code behavior, its application consequences and
possible remedies. E01 now includes optional protected framing and bounded
lookahead; its remaining isolation limits and E02-E08 follow-up remain open. It makes no
performance claims.
The [ABI documentation](Documentation/userspace-api/io_uring-opaque.rst)
remains the description of the implemented interface.

| ID | Finding | Classification |
| --- | --- | --- |
| E01 | Cache retention can exhaust header and response-framing capacity | Resource isolation |
| E02 | Failed DISCARD does not report irreversible partial progress | Completion/recovery contract |
| E03 | TX queue failure cancels followers but does not persist after draining | Failure recovery |
| E04 | Closing a socket descriptor does not release its stream token | Documented lifetime contract |
| E05 | Staging a SEND does not pin its object or establish framing admission | Documented admission contract |
| E06 | SEND_LAST may consume an object without sending bytes | Documented ownership contract |
| E07 | Auto-compaction burst size limits eligible object size | Policy limitation |
| E08 | A terminal collector cannot rearm its existing stream token | Recovery ABI |

## E01: cache retention can prevent useful GET work

**Current behavior.** With a zero reply reserve, payload, undecided bytes
and framing share `hard_limit - compact_headroom`. The v6 two-connection
reproducer confirms a greedy collector can recapture every evicted page
before another connection admits a framed or generated-only reply. An
application retention margin is not enforceable in that configuration.

V7 optionally partitions protected framing capacity. Capture cannot borrow
`reply_reserve`, and compaction cannot borrow it either. Frames reserve their
allocator bucket before snapshotting and LAST admission. Tests fill capture,
evict a cache object, observe greedy refill, and successfully queue framed
LAST and generated-only replies from the protected allowance. Concurrent
frames can exhaust that finite allowance independently.

With `capture_window`, undecided lookahead is dense and bounded per stream.
Only a consuming RECV_OBJECT or DISCARD decision reopens a full window; FREE
on another connection cannot make it greedily grow. High/low watermarks apply
hysteresis to payload backing and replacement reservations. New lowmem
objects are forwarding-only automatically; existing cached objects remain
usable. Returning to normal or compacting does not make a forwarding handle
cacheable. Ordinary SEND rejects it; SEND_LAST consumes it, FREE discards it.

**Accepted pressure policy.** COLLECT posts nonterminal `-ENOBUFS` with MORE,
token and unread offset, coalesced until that offset advances. Reserve before
capture; failed-admission bytes remain on TCP. Known DISCARD can skip future
bytes without backing. Eligible capacity release resumes capture, while a
full lookahead window needs a consuming decision. There is no resume CQE.
A full CQ can end collection with ENOSPC; see E08.

**Remaining limits.** The window bounds undecided bytes, not private object
assembly. Normal admission can later cross the high watermark; temporary
forwarding objects can exhaust the hard limit. They must complete and be
promptly consumed or discarded. There is no per-stream payload reservation,
separate header pool, automatic expiration or cache eviction. Shared-store
pressure can still fail a private receive that would fit alone. Protected
framing does not guarantee allocation success or unlimited replies. The
legacy zero-field policy retains the reproduced v6 failure by compatibility.

**Evidence and follow-up.** See `io_opaque_memory_mode()`,
`io_opaque_charge_frame()`, `io_opaque_actor()` and `io_opaque_send_acquire()`
in [opaque.c](io_uring/opaque.c). KUnit covers accounting/hysteresis, tiny
lookahead fragments and forwarding-only replacement. ABI tests cover lowmem
headers/direct discard, ownership, invalid policy, protected framing with
greedy refill, pressure coalescing, unread TCP bytes and resume. Measure
application policy and the bounded-lookahead copy cost in the KVS pilot.

## E02: DISCARD failure hides consumed progress

**Current behavior.** DISCARD permanently removes bytes as its range becomes
available. If its remainder encounters cancellation, EOF, a network error or
an allocation/extent failure, its CQE reports an error without reporting the
already discarded prefix. For example, a 1,000-byte discard can consume 400
bytes and then report `ECANCELED`.

**Application consequence and current mitigation.** Retrying the original
range encounters missing bytes. STREAM_STAT is not an atomic report of this
request's progress. Treat a failed discard as requiring application recovery;
closing the stream is the conservative option when the consumed boundary
cannot otherwise be established. Failed RECV_OBJECT has a different contract:
it restores its private prefix unless stream close/teardown releases it.

**Follow-up.** Report consumed length and final status together in one
completion, including cancellation and EOF, without restoring discarded bytes.
Specify which stream offset the application can safely resume from.

**Evidence and future check.** See `io_opaque_take()`, `io_opaque_actor()` and
`io_opaque_ready()` in [opaque.c](io_uring/opaque.c). Exercise partial future
DISCARD followed by cancellation and EOF; verify the exact consumed boundary
and successful continuation from that boundary under the revised contract.

## E03: TX failure handling changes when the queue drains

**Current behavior.** An admitted head that ends before queuing its entire
frame marks its per-store/destination queue failed and cancels admitted
followers. This also happens if the head queued zero bytes. The queue and its
failure state are freed when its requests drain; a subsequent SEND can create
a new queue for the same socket and append bytes after a truncated frame.
After partial progress, the head's positive CQE result reports queued bytes
and does not preserve the underlying errno. Link failure still applies.

**Application consequence and current mitigation.** Applications must compare
the result with the full prefix/body/suffix length and reconcile canceled
followers. Stop scheduling replies after an incomplete head and close the
connection, or explicitly recover the protocol before sending again. Admitted
LAST followers may already have consumed their objects; see E06.

**Follow-up.** Define an explicit recovery/close boundary after partial-frame
failure rather than silently forgetting it. Treat zero-progress failure
separately, while respecting protocols where skipping a reply is also invalid.
Consider retaining final error status alongside partial byte progress.

**V7 correction.** A canceled waiting follower no longer fails successors:
only a request that actually entered the TCP send attempt can fail its queue.
Tests cover early/late cleanup on one ring and shared rings. The broader
post-failure recovery boundary described above remains deferred.

**Evidence and future check.** See `io_opaque_tx_enter()`, `io_opaque_tx_put()`
and `io_opaque_send()` in [opaque.c](io_uring/opaque.c). Existing tests cover
partial-head cancellation of followers. Add zero-progress head cancellation
and a new submission after the failed queue drains, checking both peer bytes
and the application's recovery decision.

## E04: descriptor close and stream close have separate lifetimes

**Current behavior.** A stream retains its socket file reference and exclusive
receive claim. Closing the application's descriptor does not release them.
Every token requires STREAM_CLOSE, including after collector EOF/error;
store teardown is the other release path. Collector termination does not by
itself release the token.

**Application consequence and current mitigation.** Missing token cleanup can
retain a connection and consume a stream slot. Track tokens and issue
STREAM_CLOSE as part of connection teardown, including terminal/error paths.
Completed objects remain independent of that source connection.

**Follow-up.** Provide one library teardown operation that handles outstanding
requests, terminal CQEs, the token and the descriptor. Evaluate whether terminal
state can retain buffered-byte inspection/statistics without unnecessarily
retaining the socket/claim. Do not equate closing one duplicated descriptor
with ending a shared connection's ownership.

**Evidence and future check.** See the lifetime section of the
[ABI documentation](Documentation/userspace-api/io_uring-opaque.rst) and
`io_opaque_stream_close()`/`io_opaque_cleanup()` in [opaque.c](io_uring/opaque.c).
The ABI suite verifies descriptor-close retention and STREAM_CLOSE release;
exercise every connection teardown path in the application library.

## E05: staged sends need application lifetime protection

**Current behavior.** SEND preparation validates fields; first issue resolves
and pins the current immutable object version. FREE or another SEND_LAST
before that point can make the send fail with `ESTALE`. Framing is copied at
first issue before TX admission. There is no public admission notification
that tells the application when those inputs have been protected.

**Application consequence and current mitigation.** Cache eviction must
respect leases covering staged requests. Conservatively retain the cache
lease, frame descriptor and prefix/suffix source storage through completion.
Already admitted sends survive subsequent FREE or COMPACT.

**Follow-up.** Define this ownership rule in library helpers and measure its
application cost before adding an acquisition primitive or admission signal.
Preserve first-issue resolution and linked-operation semantics; preparation
must not consume LAST or resolve handles ahead of predecessor operations.

**Evidence and future check.** See the admission and linked-operation sections
of the [ABI documentation](Documentation/userspace-api/io_uring-opaque.rst).
Exercise delayed issue, eviction, shared-ring access and compaction in the
KVS adapter; verify that eviction respects its application leases.

## E06: LAST consumption is independent of send progress

**Current behavior.** SEND_LAST transfers ownership at admission, including
when queued behind an earlier send. A follower canceled after that earlier
send fails still reports `IORING_CQE_F_OPAQUE_CONSUMED`, even if it queued no
bytes. A subrange LAST consumes the entire object, including unselected bytes.
After admission, errors, short sends and cancellation do not restore it.

**Application consequence and current mitigation.** Check the consumed flag
independently of the byte result. Use reusable SEND while an application may
need retry/fanout, retaining the handle until its recovery policy permits
release. Reserve LAST for a known final logical use whose failure may release
the remaining backing. A missing consumed flag describes this request only;
another request may have freed or consumed the shared handle.

**Follow-up.** Make reusable versus consuming ownership explicit in library
helpers and examples. Evaluate whether retry-oriented forwarding needs an
additional contract, without weakening or silently changing LAST semantics.

**Evidence and future check.** See `io_opaque_send_acquire()` and
`io_opaque_tx_put()` in [opaque.c](io_uring/opaque.c). Existing ABI tests cover
admitted LAST follower cancellation and independent consumption reporting.
Exercise application forwarding failure and recovery with reusable and LAST
sends, including a selected subrange.

## E07: burst size is also an auto-compaction eligibility limit

**Current behavior.** Automatic compaction tokens are capped at `burst_bytes`,
and selection requires tokens for the object's entire length. Thus an otherwise
eligible fragmented 10 MiB object with a 1 MiB burst never qualifies, even at
a high configured byte rate. The age, savings and temporary-memory gates also
apply. Auto-policy rate/burst/temporary controls do not throttle manual
COMPACT, which remains subject to hard memory bounds and copy serialization.

**Application consequence and current mitigation.** Automatic compaction may
leave large sparse values unchanged. Set the burst and temporary-memory limit
large enough for intended objects, or issue manual compaction under an
application policy; do not assume the automatic rate applies to manual work.

**Follow-up.** Consider rate-limiting copy progress in chunks while publishing
the replacement atomically, or explicitly expose/document the maximum eligible
object size and the scope of policy controls.

**Evidence and future check.** See `io_opaque_auto_tick()`,
`io_opaque_compact_start()` and `io_opaque_set_policy()` in
[opaque_compact.c](io_uring/opaque_compact.c). Exercise otherwise eligible
objects below, at and above the burst/temporary limits, then verify progress
under any revised policy without premature publication.

## E08: terminal collector pressure has no rearm operation

**Current behavior.** A pressure notification that cannot fit in the CQ ends
COLLECT with ENOSPC. The token, exclusive receive claim and captured prefix
remain live, but the existing token cannot restart collection. A new COLLECT
on that claimed socket fails EBUSY. This also matters after explicit collector
cancellation. Negative ENOBUFS with MORE is a pause, not this terminal state.

**Application consequence.** Drain completions, reconcile pending ranges and
explicitly STREAM_CLOSE. Closing releases undecided bytes, so application
protocol recovery must account for that loss or close the connection. Do not
retry the old token or rely on a resume edge CQE. Published objects survive.

**Deferred follow-up.** A collector rearm ABI could attach a new COLLECT to
an existing token without resetting offsets or dropping captured bytes. It
needs ownership, cancellation, lifetime and CQ-admission rules. That is a
larger recovery change, kept out of the v7 fixes. Existing full-CQ tests and
the ABI documentation record the current behavior.
