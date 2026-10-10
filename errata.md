<!-- SPDX-License-Identifier: GPL-2.0 -->

# OPAQUE_OBJ errata and deferred follow-up

Recorded 2026-10-10 against branch `upstream/opaque-obj-v4`, implementation
commit `c5bcea485987e5faaba3a6cb1ad933a3344a2a0c`.

These seven findings concern resource exhaustion, failure recovery, teardown,
ownership and policy configuration.
Some are documented contracts that need better application support rather
than implementation bugs. They do not require an immediate redesign; revisit
them during KVS/forwarder integration and before freezing affected ABI
semantics. Quota exhaustion can be routine for a cache near capacity, so it
should be exercised in the application baseline work.

The receive-pressure part of E01 now follows the policy accepted below. Its
remaining efficiency work and E02-E07 remain **open, deferred follow-up work**.

This file records observed code behavior, its application consequences and
possible remedies. Except for E01's accepted receive-pressure follow-up,
proposed remedies and future checks remain unimplemented. It makes no
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

## E01: cache retention can prevent useful GET work

**Behavior at the recorded v4 implementation.** Retained payload, undecided
receive bytes and send framing snapshots share the ordinary capture allowance:
`hard_limit - compact_headroom`. If cached objects exhaust it, capturing a new
GET header that needs additional backing stalls the collector until capacity
is released. An INSPECT waiting for that header therefore remains pending.
A framed SEND of an existing object can instead fail with `ENOBUFS`, including
a generated-only response. A nonempty framing snapshot reserves a power-of-two
allocation of at least `PAGE_SIZE`, even for a one-byte prefix.

**Accepted policy and implemented follow-up.** Quota exhaustion is propagated
to the application. COLLECT posts a nonterminal `-ENOBUFS` CQE with `F_MORE`,
the stream token and first unread offset, coalesced until that offset advances.
Capacity is reserved before captured bytes are consumed from TCP; failed
admission leaves those bytes queued. The application decides how to release
capacity, after which collection resumes automatically. A known DISCARD range
still needs no backing reservation. If the pressure CQE cannot be posted,
collection terminates with `ENOSPC`, preserving the token and captured prefix
for explicit recovery/close. The policy provides visible backpressure rather
than guaranteeing GET progress while cached values exhaust the allowance.

**Application consequence and current mitigation.** A cache can hold the value
needed for a GET while lacking capacity to parse or answer the request. Keep
an application retention margin for receive progress and concurrent response
framing; free/evict values before exhausting it. Compaction headroom is not
available to these operations. A future DISCARD can bypass capture allocation
only when the application already knows the range to discard.

**Deferred follow-up.** Evaluate smaller allocations for small framing and the
application's eviction/admission policy under pressure. Any later change to
resource partitioning must preserve explicit bounds; receive progress requires
available capacity under the accepted policy.

**Evidence and future check.** In [opaque.c](io_uring/opaque.c), see
`io_opaque_charge()`, `io_opaque_capture()`, `io_opaque_recv()` and
`io_opaque_frame_import()`. The [ABI tests](tools/testing/selftests/io_uring/opaque_obj.c)
cover budget stall/resume and framing-quota failure without LAST consumption.
The receive-pressure cases verify a short GET remains on TCP, one notification
per unread offset, automatic resumption after a cross-ring release, mixed CQEs,
close while paused and failure to report pressure into a full CQ. The KVS
baseline should exercise its own admission/eviction response to this signal.

**Follow-up validation (2026-10-10).** The updated branch passes 23 KUnit tests
and 76 ABI groups under KASAN/lockdep. The PAGE_POOL/ZCRX-disabled debug build
passes the same 23 KUnit tests and 74 ABI groups, with two expected fault-test
skips. These runs include the three new pressure groups, 10,000 arrival/drain
cycles and 4 GiB of DISCARD traffic; they are functional checks, not performance
measurements.

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

**Evidence and future check.** See `io_opaque_auto_work()`,
`io_opaque_compact_start()` and `io_opaque_set_policy()` in
[opaque_compact.c](io_uring/opaque_compact.c). Exercise otherwise eligible
objects below, at and above the burst/temporary limits, then verify progress
under any revised policy without premature publication.
