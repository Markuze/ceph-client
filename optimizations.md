<!-- SPDX-License-Identifier: GPL-2.0 -->

# OPAQUE_OBJ flow optimizations

Updated 2026-10-11 for v5 preparation, tested implementation
`2cb5da9c7cda1eb0b349030260740e4af034f9f7` on `work/opaque-obj-v5`.

V6 source: `8fae2736b0626305c6dc8e70ad99f7685eb704bb`, identical to tested
development `d3ea2b3ddf5766ef9e7ed2676a42d3786b24a964`.
The implementation now combines manual/automatic compaction on one delayed
work item, removes the copy mutex and avoids timer retries for permanently
ineligible candidates. A policy update reconsiders those objects. Receive
allocation retries stay independent so compaction cannot delay their worker.
The cleanup adds policy-disable, shared-ring and queued-teardown coverage;
23 KUnit / 80 ABI groups pass on KASAN/lockdep, and 23 / 78 with two expected
fault skips on the normal kernel. This reduces redundant scheduling state;
application performance benefits remain unmeasured. O01-O06 keep their scope.

These opportunities simplify normal application flows. Application ownership,
performance and ease of integration are the criteria; kernel implementation
complexity is ours to manage. Fewer application decisions, requests or
allocations are useful hypotheses, but no throughput, latency or CPU advantage
has been measured for these proposals.

The [ABI documentation](Documentation/userspace-api/io_uring-opaque.rst)
describes the implemented interface. [errata.md](errata.md) records separate
failure, lifetime and policy issues. This file proposes work; it does not add
operations, change the ABI or implement a library.

| ID | Opportunity | Scope and status |
| --- | --- | --- |
| O01 | Forward an entire encoded message as one object | Existing ABI; application integration opportunity |
| O02 | Combine declared receive framing decisions | Proposed ABI extension; unimplemented |
| O03 | Parse and submit in batches | Existing ABI; library/application work |
| O04 | Reduce small framing and inspection allocation costs | Framing implemented; inspection and performance evaluation deferred |
| O05 | Send several object ranges in one complete response | Proposed SEND descriptor extension; unimplemented |
| O06 | Provide common asynchronous lifetime helpers | Library integration; unimplemented |

## O01: retain and forward the complete encoded message

When forwarding preserves the wire representation, the current ABI can express:

```text
INSPECT header -> RECV_OBJECT(entire message) -> SEND with LAST
```

The application parses enough header bytes to declare the complete message
range. RECV_OBJECT retains its header, payload and trailer together and reports
completion when that declared range is complete. Sending the object with LAST
transfers ownership using the existing consuming-send option, often described
as SEND_LAST in application flows.

This avoids reconstructing framing and issuing separate DISCARD operations for
bytes that must be forwarded anyway. It requires no new opcode. Protocol
validation remains application work; READ_OBJECT can inspect bytes of the
completed object when validation requires them.

Use this for unchanged forwarding. Rewritten headers or responses still need
the existing framed SEND. Retaining framing increases object size, and the
whole message must fit the object and store limits. A cache that reuses an
object should use ordinary SEND and manage its lifetime explicitly.

## O02: combine application-declared receive framing decisions

A value-only cache can currently need these decisions for a large SET:

```text
INSPECT header -> DISCARD header -> RECV_OBJECT body
               -> INSPECT trailer -> DISCARD trailer
```

These arrows describe parsing and range decisions, not mandatory sequential
waits: requests with known offsets can already be submitted asynchronously.
The application still has to manage separate body and trailer completions
before publishing a valid cache entry.

An optional RECV_OBJECT descriptor could specify an already parsed header
range to discard, a body range to retain, and a bounded trailer to copy into
userspace. One successful completion would return the object handle and make
the copied trailer available. The application would validate that trailer
before publishing the object in its cache.

The kernel would execute declared byte ranges, without parsing the protocol.
Stream offsets and message lengths remain application decisions. Completion
waits for the declared object and trailer; INSPECT retains its available-byte
behavior for discovering headers. No blocking header-read option is needed.

Potential benefits are fewer SQEs/CQEs and less application completion state.
Before adding this ABI, specify quota reservation before capture, descriptor
and trailer limits, buffer lifetime, cancellation, partial discard progress,
and which effects can be rolled back. Discarding a header is already an
irreversible action; an error must not imply that consumed bytes were restored.

## O03: parse and submit in batches

One INSPECT can return several complete short requests. A library can parse
the available window, combine adjacent parsed header ranges into a DISCARD,
and submit complete replies together in protocol order. Inspect again when
parsing needs bytes beyond the available window.

This uses the current ABI. Combining operations can reduce SQEs and CQEs;
batching submission can reduce ring-entry overhead. Those are separate effects
and should be counted separately.

Only merge adjacent ranges whose contents have been fully parsed and classified
as disposable. Do not discard unparsed bytes or cross a retained body. Keep
one parser cursor per connection, handle partial headers, and preserve response
order when requests complete out of order. INSPECT capacity remains a maximum
copy size, not an exact-byte completion threshold.

## O04: make small framing and inspection cheaper

V5 implements smaller framing allocations in
[opaque.c](io_uring/opaque.c). `io_opaque_frame_import()` reserves
`kmalloc_size_roundup(bytes)` before allocation and copying, replacing the
minimum `PAGE_SIZE` allocation for nonempty prefix/suffix snapshots. The
queued-framing regression verifies that a five-byte generated reply uses a
subpage charge alongside two full-size snapshots.

Store quota admission, owner memcg accounting, bounded framing size, cleanup
and snapshot-before-LAST admission are preserved. This reduces the reserved
charge for tiny replies without an ABI change; throughput, allocation CPU and
latency effects still need measurement. The store charge is an accounting bound,
not a census of all physical network memory.

`io_opaque_snapshot()` also allocates metadata for INSPECT so copying to
userspace can happen safely outside the stream lock. Review small inspections
for avoidable allocation or metadata work while preserving that safety. Measure
any extra copying or larger per-request state introduced by a fast path.

Smaller framing allocations may help near-capacity GET responses, but cannot
guarantee progress at a full quota. The accepted contract remains: report
capture pressure, reserve capacity before consuming captured TCP bytes, leave
failed-admission bytes queued, and resume when capacity is available. An
explicit DISCARD of a known range can proceed without backing allocation.

## O05: extend complete SEND to multiple object ranges

The current SEND already represents one complete prefix/body/suffix frame with
one queue position and one completion. MGET and multipart responses can instead
alternate generated framing and several cached opaque values.

A bounded segment-list extension to the same SEND descriptor could represent
that complete response in one instruction. Segments would describe generated
bytes or object ranges. Validate every segment, reserve framing capacity and
pin every referenced range before emitting any bytes or transferring ownership.

Define reusable and consuming ownership explicitly, including repeated handles,
all-or-nothing admission, cancellation, partial sends, and consumed flags. Keep
one response cursor and preserve the destination queue's ordering. One request
does not make TCP transmission atomic or remove partial-send recovery.

This is a larger ABI change. It is justified if MGET or multipart applications
otherwise need substantial response bookkeeping. Bound segment count and
metadata cost, and compare its overhead with the existing single-body path.

## O06: provide one asynchronous library contract for lifetimes

Provide helpers for cache leases, request/framing lifetimes, send completion,
consumed flags, pressure notifications and connection teardown. Applications
would retain their protocol parser, cache policy and response ordering; the
adapter would handle recurring request bookkeeping and completion dispatch.

Preserve the distinction between a staged SQE and an admitted operation. SEND
pins an object on first issue, not while the SQE is prepared. A cache lease must
protect staged reusable sends from concurrent eviction. Keep framing buffers
valid through completion under the current contract. LAST consumption is an
ownership event independent of byte progress, including cancellation or failure.

Use explicit completion dispatch within the application's event loop, without
hidden waits or consuming another user's CQEs. Teardown must reconcile pending
requests and explicitly release stream tokens even after EOF or descriptor
close. Cached objects may outlive their receive connection. Helpers should make
these contracts easier to use without adding another kernel ownership model.

## Priority and evaluation

Start with O03 batching and O06 lifetime helpers. Measure O04's smaller framing
path and examine the remaining inspection allocation work. Exercise O01 in the
forwarding example using the existing ABI.
These give application evidence before expanding the interface. O02 combined
receive framing and O05 multi-object SEND are the strongest subsequent ABI
candidates if the measurements justify them.

Use a shared KVS/forwarder harness and compare equivalent application behavior
with the ordinary copied path and applicable io_uring RECV_ZC/SEND_ZC paths.
Record setup and hardware requirements alongside results. Measure:

- Application ownership state, pending-request state and buffer lifetime rules.
- SQEs, CQEs, ring entries and actual wakeups per complete request/response.
- Allocations, charged bytes, framing/snapshot copies and retained memory.
- CPU per request/byte, useful throughput, and median and tail latency.

Cover unchanged forwarding, pipelined short GETs, large SETs with trailer
validation, MGET, concurrent cache eviction and sends, and quota pressure/resume.
Keep workload, framing, validation and ordering requirements comparable.
Functional stress results, including arrival/drain cycles and gigabytes of
traffic, establish correctness; they do not establish performance benefits.
