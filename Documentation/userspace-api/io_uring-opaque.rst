.. SPDX-License-Identifier: GPL-2.0

===================================
io_uring opaque TCP payload objects
===================================

OPAQUE_OBJ retains TCP payloads in the kernel. Applications inspect framing
through bounded copies, select complete payload ranges, and transmit or cache
those ranges through generation handles. The payload is not mapped into the
application's address space, and the application cannot change it. One
exception exists. A privileged process can bind a store to the area of a MAIO
zcrx instance. It maps the area, and it reads and writes the payload in place.
See "Stores in the area of a MAIO instance". Eligible ordinary RX pages are
retained; unsafe backing is copied into independently owned pages. An
allocation larger than the entire capture limit also uses this fallback so
that small stores can make progress.

Registration and sharing
========================

Enable ``CONFIG_IO_URING_OPAQUE_OBJ``. Create a ring with
``IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN`` and either
``IORING_SETUP_CQE32`` or ``IORING_SETUP_CQE_MIXED``. Ordinary 64-byte SQEs
suffice. A store that binds no MAIO instance needs no network device
configuration and no CAP_NET_ADMIN.

Register through ``IORING_REGISTER_OPAQUE_STORE`` with ``nr_args = 1`` and
``struct io_uring_opaque_register``. Set ``size`` to the argument size,
``flags = 0``, ``fd = -1``, ``store_id = 0``, ``config`` to the address of
``struct io_uring_opaque_config`` and ``config_size`` to its size. Reserved
fields are zero. The returned ``store_id`` selects the store in that ring.
The feature has no dependency on ZCRX, page-pool or busy-poll support. Only the
optional binding of a store to a MAIO instance needs ``CONFIG_IO_URING_ZCRX``.

Use the same registration operation with ``IORING_OPAQUE_REG_EXPORT``,
``fd = -1`` and an existing ``store_id`` to obtain a close-on-exec store
descriptor. To import, use ``IORING_OPAQUE_REG_IMPORT``, the descriptor in
``fd``, and ``store_id = 0``; the returned ID selects the shared store.
Export/import require zero ``config`` and ``config_size``. Combining the
flags or importing a different kind of file is rejected. Exported descriptors
can keep a store alive after the creating ring closes. No object gets its own
file descriptor.

Registration and configuration inputs are size-versioned. The initial known
prefixes are 32 and 40 bytes respectively; missing reserved bytes become zero.
Arguments may extend through one page: unknown trailing input bytes must be
zero, otherwise registration returns E2BIG. Known reserved fields and unknown
flags are rejected with EINVAL. Output copies respect the supplied argument
size, zero unknown trailing bytes and report the kernel's known registration
size in ``size``. Failed output copies unwind IDs, descriptors and references.

All configuration limits are explicit. ``hard_limit`` and
``compact_headroom`` are page aligned; ordinary capture cannot use headroom.
The hard limit includes headroom. Object size, object slots, stream slots,
extents and pending requests have separate bounds. Registering reserves the
hard-limit capacity against the registration owner's locked-memory accounting.
The payload is allocated or retained on demand.

The optional ``high_watermark``, ``low_watermark`` and ``reply_reserve``
fields define a memory-pressure policy at store creation. Omitted fields are
zero and preserve the shared capture/framing allowance. Watermarks and reply
reserve are page aligned. A nonzero high watermark requires a lower low
watermark and a nonzero reply reserve. The high watermark cannot exceed
``hard_limit - compact_headroom - reply_reserve``. The reply reserve must be
smaller than ``hard_limit - compact_headroom``. A reply reserve may also be
enabled without watermarks. Reserved words must be zero.

Registration IDs are ring local; stream and object handles, limits and policy
belong to the shared store. The last ring/export-descriptor user stops
collection and automatic work; ring references remain until requests drain
before the store is destroyed. A store descriptor supports export/import,
not payload mapping or refill queues. The area of a bound store maps through
its MAIO instance. No new syscall is introduced.

Receive and range decisions
===========================

Submit ``IORING_OP_OPAQUE_COLLECT`` on a connected TCP socket, selecting the
store in ``opaque_store_id``, with zero ``ioprio`` and length. Collection is
inherently multishot; ``IORING_RECV_MULTISHOT`` is not an accepted flag. The first
32-byte CQE has result zero, ``IORING_CQE_F_MORE``, and a stream token in its
first extra word. Stream offset zero denotes the first unread byte when
ownership is acquired. No bytes are consumed before this CQE can be posted.
A full completion queue rejects attachment with ENOSPC before acquiring the
receive claim or consuming TCP bytes. Drain completions before retrying attach.
``IORING_RECVSEND_POLL_FIRST`` waits for socket readiness before attachment.
A backing or extent quota stall produces a nonterminal 32-byte CQE with result
``-ENOBUFS`` and ``IORING_CQE_F_MORE``. Its extra words contain the stream token
and the absolute first unread TCP offset respectively. The notification is
coalesced until that offset advances. The application can release objects,
compact backing or cancel pending work to make capacity available; eligible
collectors resume automatically after release. No replacement COLLECT is needed.
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
Quota is reserved before captured bytes are consumed from TCP. A failed
admission leaves those bytes on the socket; an already admitted prefix may
have advanced the stream. An explicit DISCARD can still skip its declared
range without retaining backing. Pending inspection/range requests and the
stream token remain live during a budget pause, subject to their usual
cancellation and incomplete-object failure rules. Network errors still end
collection. If the pressure notification cannot fit in the CQ, collection
ends with ``ENOSPC``; the token and captured bytes remain available for explicit
STREAM_CLOSE/recovery and no further TCP bytes are collected.
This RFC has no operation to rearm collection on that token. Drain the CQ and
explicitly close the stream; retrying COLLECT while its claim remains held
does not resume the old stream. Stream close releases undecided bytes, so
applications must account for that loss when recovering their protocol state.
A terminal CQE ends the collector on EOF, error, cancellation or stream close.
The RFC collector rejects ``IOSQE_ASYNC`` and CQE suppression.

Every stream token must be released with STREAM_CLOSE, including after EOF
or a network error. The stream holds a socket file reference: closing the
application's descriptor alone does not release the token, receive claim or
connection. STREAM_CLOSE ends that ownership; store teardown also releases
all remaining streams. Disconnecting with ``connect(AF_UNSPEC)`` is rejected
with EBUSY while a stream owns the receive sequence.

The collector uses ordinary io_uring polling and ``tcp_read_sock``. TCP receive
ownership excludes ordinary recv, splice, other actors and zerocopy mappings
while the stream is claimed, including readers already asleep when it is
acquired. TCP ULP replacement, repair and BPF socket redirection cannot acquire
the claimed socket. Existing ULPs, repair and socket callbacks are rejected
when attaching a collector. No registered-page checks are introduced into the
TCP reader. The actor checks backing safety and preserves a successfully
captured prefix if a later capture fails.
Urgent TCP data is unsupported: an existing urgent indication prevents attach,
and a new indication terminates collection with EOPNOTSUPP.

For ``IORING_OP_OPAQUE_OBJ``, use ``fd = -1``, select the control operation in
``ioprio``, the store in ``opaque_store_id``, a handle/token in ``addr``, a byte
offset in ``off``, length in ``len``, and any user pointer in ``addr3``.
Other operation-specific fields are zero. Personality and ordinary io_uring
linking/cancellation retain their usual meaning. CQE suppression is rejected.

.. list-table:: Object controls
   :header-rows: 1
   :widths: 25 75

   * - Control
     - Meaning
   * - INSPECT
     - Copy available contiguous unclassified bytes, at most 64 KiB, without
       consuming them. Length is capacity. If no bytes are available at the
       requested offset, arm asynchronously for the first bytes. Result is
       the actual copied count, or zero at EOF. There is no WAITALL option.
   * - RECV_OBJECT
     - Consume a selected range into private assembly. Publish an object only
       when the entire range and its vector are complete. Result is length;
       the first extra CQE word holds the generation handle and the second
       holds ``IORING_OPAQUE_OBJECT_F_FORWARD_ONLY``,
       ``IORING_OPAQUE_OBJECT_F_MAPPED``, both, or zero.
   * - DISCARD
     - Consume unwanted buffered or future stream bytes. Future bytes can be
       skipped without allocating backing, even when the store is full.
   * - READ_OBJECT
     - Copy an object subrange of at most 64 KiB to the supplied address.
   * - FREE
     - Invalidate a published object handle and release its table reference.
       Already pinned backing versions and TCP page references remain valid.
   * - STAT / OBJECT_STAT / STREAM_STAT
     - Copy a size-versioned UAPI structure; ``len`` is the output buffer size.
       STAT uses a zero handle and offset. The flags of OBJECT_STAT include
       ``IORING_OPAQUE_OBJECT_F_MAPPED`` for an object in the area of a MAIO
       instance.
   * - STREAM_CLOSE
     - Release the receive claim and undecided bytes, terminate pending ranges,
       and remove the stream token. Published objects remain independent.
   * - COMPACT
     - Replace backing under the same object handle; result is zero on success.
       An object that has ``IORING_OPAQUE_OBJECT_F_MAPPED`` returns
       ``EOPNOTSUPP``.
   * - SET_POLICY
     - Replace automatic compaction policy from the supplied UAPI structure.
       Handle and offset are zero; ``len`` supplies the size-versioned input size.
       Automatic compaction skips an object that has
       ``IORING_OPAQUE_OBJECT_F_MAPPED``.
   * - OBJECT_EXTENTS
     - List where the bytes of an object are in the area of the MAIO instance
       that the store binds. The handle is in ``addr``, the buffer in
       ``addr3``, and its size in ``len``. The result is the number of bytes
       that the kernel wrote: one ``struct io_uring_opaque_extent`` for each
       extent. ``EOVERFLOW`` and the needed size in the first extra word
       mean that the buffer is too small. A store that binds no instance
       returns ``EOPNOTSUPP``.

Each statistics structure begins with ``size``, reporting the kernel's known
size. Statistics accept output buffers from four bytes through one page;
short buffers receive a prefix and longer buffers have their unknown tail
zeroed. SET_POLICY accepts its initial 32-byte prefix through one page,
zero-fills omitted reserved fields and rejects nonzero unknown extensions.
Reserved-field and per-structure flag validation applies to all inputs.

The application owns stream offsets and protocol parsing. It inspects available
headers, declares an object's start and length once its header defines them,
and continues processing other CQEs while the kernel assembles that range
privately. The RECV_OBJECT request's sole successful CQE is its object-ready
notification: user_data identifies the request, result is the complete length,
the first extra word contains the handle, and the second reports whether
lowmem admission made it forwarding-only and whether the object is in the area
of a MAIO instance. There is no per-fragment object
ID or second success notification. READ_OBJECT inspects an already complete
object and cannot wait for future TCP bytes.

For example, INSPECT with a 512-byte destination returns a received 22-byte GET;
it does not wait for 490 unrelated future bytes. An incomplete header is parsed
incrementally: inspect its next missing offset and resume on that CQE. While a
stream holds undecided bytes, inspecting the same offset again returns only the
bytes already collected, however large the destination; only an INSPECT that
waits past them makes the collector fetch more. Arming
and the initial availability check share the stream lock, so arrivals before
or after arming cannot lose a notification. Buffered bytes are returned before
EOF or a stored network error; an unavailable offset at EOF returns zero, a
consumed starting offset returns ENODATA, and a live unavailable offset remains
pending. Cancellation/close returns ECANCELED; a stored network error propagates
when no bytes are available.

RECV_OBJECT/DISCARD ranges do not overlap each other or the starting byte of a
pending INSPECT. Inspection capacity does not reserve a future byte range.
An available window stops at the first consumed gap. Nonconsuming inspections
may overlap each other. Decisions are indexed in stream
order; capture consults the earliest decision rather than scanning all pending
requests. A following header can be read independently of a preceding RECV_OBJECT.
Object slots and private assembly are reserved before a RECV_OBJECT can consume data.
Handles are opaque 64-bit values; applications must not decode their index or
generation. Slots are allocated through rotating bitmaps and the current
implementation uses 50 generation bits. Generation exhaustion retires a slot
instead of wrapping a stale handle.
RECV_OBJECT lengths exceeding either the configured object-size bound or the entire
capture allowance are rejected with EMSGSIZE. Capture falls back to packed
page copies when retaining another allocation would leave no room for dense
backing of the remaining private range. If physical backing or extent limits
still stop a partial RECV_OBJECT, it completes with ENOBUFS and restores its prefix;
it does not indefinitely pin quota waiting for its own completion. A range
with no captured prefix can wait for quota release, announced by the collector's
nonterminal ENOBUFS CQE. Deadlines and cancellation
remain available. Compaction operates on published objects.

EOF before completion produces ENODATA; network errors propagate. ASYNC_CANCEL,
linked timeouts, stream close and ring teardown terminate pending ranges.
An unsuccessful RECV_OBJECT restores its private prefix at the original stream
offsets before reporting completion, including cancellation, a linked timeout,
EOF, a network error or vector allocation failure. The application can retry
RECV_OBJECT, inspect that prefix with INSPECT, or DISCARD it. No incomplete object
is published. STREAM_CLOSE and store teardown explicitly release all remaining
stream bytes instead of preserving them for retry. DISCARD remains consuming:
bytes it already discarded are not restored if the rest of its range fails.

Transmission and ownership
==========================

``IORING_OP_OPAQUE_OBJ_SEND`` takes an ordinary destination socket descriptor,
the store ID in ``opaque_store_id``, an object handle in ``addr``, offset in ``off``
and body length in ``len``. Optional ``addr3`` points to a size-versioned
``struct io_uring_opaque_frame`` containing user prefix/suffix pointers and
lengths. Its minimum prefix is 32 bytes; its current size is 48 bytes. Inputs
may extend through one page, with zero unknown/reserved fields and zero flags.
Combined prefix and suffix are bounded by ``IORING_OPAQUE_FRAME_MAX`` (64 KiB),
and the complete prefix/body/suffix length must be positive and fit INT_MAX.
Zero addr3 selects a body-only send. With handle, offset and body length zero,
a nonempty frame sends a generated-only reply through the same queue; LAST is
invalid for that form. ``MSG_MORE`` and ``MSG_DONTWAIT`` are accepted
in ``msg_flags``. ``ioprio`` is zero for reusable sends or contains
``IORING_OPAQUE_SEND_LAST`` for a consuming send. Other operation-specific
fields are zero. SEND_LAST rejects ``IOSQE_CQE_SKIP_SUCCESS`` so ownership
transfer is always reported.

Ordinary SEND first issue pins the current immutable backing version, leaving
the handle available for caching, further reads and sends to other destinations.
SEND preparation validates SQE fields without resolving the handle. At first
issue, after destination validation, framing snapshot and TX queue allocation, SEND_LAST
atomically removes the handle and transfers the table reference into the
request. It uses the version current at that point, including any compaction
published since preparation. Admission behind an earlier send also takes
ownership before waiting for its turn; subsequent retries retain that reference.

The prefix and suffix are copied into private request storage before admission.
Descriptor and framing faults, invalid lengths, allocation failure or framing
quota failure therefore cannot consume LAST or emit a prefix. User framing
must remain valid until admission; conservatively retain it through completion
unless the application has another way to establish admission. Once admitted,
later mutation/unmapping cannot change queued framing. The snapshot remains
valid across socket backpressure, cancellation and worker retries.

The send CQE has ``IORING_CQE_F_OPAQUE_CONSUMED`` exactly when this request took
ownership, independently of its byte result. Before that point, validation,
admission failure or cancellation does not consume the object. A missing flag
reports that this request did not consume it; another request may still have
freed or consumed the shared handle. After that point the handle stays invalid,
including on short sends, I/O errors and cancellation. The unsent portion is
released with the request and is not restored. A subrange SEND_LAST consumes
the entire object, including bytes outside the selected range. Two prepared
SEND_LAST requests for the same handle cannot both claim it: the loser returns
ESTALE without the consumed flag. Earlier pinned reads and ordinary sends
continue using their version. A SEND_LAST awaiting its first issue has no pin
and fails with ESTALE if another operation removes the handle first.

One SQE, queue item, continuation cursor and CQE cover prefix, opaque body and
suffix. The body uses the retained bvec array with ``MSG_SPLICE_PAGES``; private
framing uses ordinary copied TCP sends. Internal send attempts advance across
these segments, setting MSG_MORE while segments remain. Partial progress resumes
at the saved whole-frame cursor. Without MSG_DONTWAIT,
the request continues until the complete frame is queued or an error/cancellation ends
it. Failure before progress returns a negative errno; failure after progress
returns the positive queued prefix. MSG_DONTWAIT permits a short result. An incomplete send marks link failure
even when its CQE reports a positive prefix, so a soft-linked follower is
canceled before issue and does not claim SEND_LAST ownership.

The store serializes admitted sends to each destination socket. If the head
attempted TCP transmission and ends before queuing its entire frame, already
admitted followers complete with
ECANCELED rather than inserting another object's bytes after that prefix.
A canceled follower that already claimed SEND_LAST still reports the consumed
flag. Canceling a waiting follower does not interrupt an earlier send or cancel
later followers, including when head cleanup promotes it before its own
cleanup runs. Blocking
TCP retries run in io-wq without holding the ring submission lock.

Each send has one application completion. This reports bytes queued to TCP,
including all prefix, body and suffix bytes, not peer delivery or acknowledgment.
An error/short result counts the prefix of the complete response, including
cases where no body byte was queued. The consumed flag is independent of that
total. No user-buffer reuse notification is
needed: userspace cannot overwrite the payload of a store that binds no MAIO
instance, and TCP owns ordinary page references. A process can overwrite the
payload of a bound store, see "Writes". FREE after queued sends can invalidate
the handle while TCP still retains those pages. SEND_LAST does this without a
separate FREE SQE or CQE and transfers a reference rather than adding and
dropping a send pin.
An ordinary SEND awaiting first issue has not acquired its backing reference;
users must order handle release accordingly.

Ordering is per shared store and destination, in admission order. It does
not serialize another store or ordinary writes to the same socket. Linked
chains add their normal ordering; independently submitted chains do not form
a single socket-wide chain. A protocol connection must route all its replies,
including generated-only replies, through one shared store's send queue and
admit them in the application's protocol order.

Control operations also resolve stream tokens and object handles at first
issue. A READ_OBJECT or SEND linked after FREE observes ESTALE; one linked after a
successful COMPACT observes the replacement backing. Already issued requests
keep their immutable version through subsequent handle release or compaction.

Caching and fanout applications use ordinary SEND and keep their handle until
eviction or explicit FREE. Forwarders use SEND_LAST for a payload's final use,
then inspect the single completion's byte result and consumed flag. A final
fanout send may also use SEND_LAST once earlier sends have acquired their pins.
Both application patterns use the same store, opcode, range fields and TCP
send machinery. The saved SQE and CQE do not imply a saved system call: SEND
and FREE can already be submitted together in one ``io_uring_enter``.

Object sends are FIFO per shared store and destination socket. Only the queue
head polls for write space; later requests reuse their own request state as
queue entries. This ordering does not cover plain writes or other stores.

Backing and memory management
=============================

References move from undecided stream extents into private RECV_OBJECT assembly and
then a published backing version. Reads, sends and compaction pin their
version. FREE drops the table reference; SEND_LAST transfers it into the send;
compaction replaces that reference. Old versions remain charged until their
store references disappear.

Capture combines backing and initial extent metadata in one allocation and
coalesces adjacent compatible extents. Successive captures from the same
compound allocation share its backing and charge even when another socket's
data separates their physical offsets. Splits also share the backing reference
and charge. Independent captures can still conservatively charge the same
physical allocation more than once. A retained compound allocation is charged
in full. Retained RX allocations also carry an explicit charge to the store
owner's memory cgroup until the final backing reference is released; the
original network allocation's accounting is independent. Owned copy and
compaction pages already use accounted allocation and are not charged twice.
Copy fallback appends into unused space in the previous owned page;
it does not allocate a whole page for every small fragment.
Undecided bytes keep their original backing in both memory modes: they are
retained like object ranges when eligible and copied only by the fallback
above. Retained backing is charged by whole allocation, so a short header in
a large receive allocation charges the whole allocation, and many tiny
segments can charge more than their length; the store quota bounds the
total. This does not bound private object assembly. An unsuccessful
assembly restores its admitted bytes as undecided, and the stream then
waits for a decision, a waiting INSPECT or a decision further in the stream
before collecting more.
Vectors are built once at object publication and are bounded by extent limits.
Releasing backing or metadata wakes eligible collectors suspended on the store
budget in bounded batches. Their native multishot polls remain armed for socket
errors and urgent data. Transient capture ENOMEM backs off for 50 milliseconds
and retries; it does not poison the stream or discard its captured prefix.
FREE, STAT, STREAM_CLOSE and SET_POLICY remain admitted at the request cap.
Request state uses a bounded per-ring io_uring allocation cache and a union
for operation-specific fields. Object and stream counts are maintained at
publication/removal, so STAT does not scan either table.

Framing snapshots use accounted allocations in the store owner's memory cgroup.
Their allocator bucket size is charged until request cleanup. Capture may use
``hard_limit - compact_headroom - reply_reserve``; replacement reservations
may use ``hard_limit - reply_reserve``. Framing first uses the capture
allowance and takes the separate ``reply_reserve`` allowance, which payload
capture and compaction cannot borrow, only when capture capacity is full. A
nonzero reply reserve is therefore a floor for framing, not a cap. Snapshot
quota failure returns ENOBUFS before admission. STAT exposes current
``framing_bytes`` and cumulative ``framing_copied_bytes`` separately from
retained backing and capture copies; ``backing_bytes`` includes the framing
charge. Per-request framing and pending request bounds also apply to
generated-only replies.
The protected allowance permits bounded framed and generated-only replies
even when payload capacity is full. A stream that holds undecided bytes
cannot refill capacity freed by the application; it stays held until the
application decides those bytes. Any stream whose collection is permitted
can still take one pass of up to 256 KiB, which can fill a small store
before a reply is admitted; that is what ``reply_reserve`` is for.
Concurrent snapshots can still exhaust the reply reserve once capture capacity
is full, and allocation or request admission can fail independently. Framing
cannot consume compaction headroom. With a zero reply reserve, snapshots share
ordinary capture capacity, so an eviction guarantees reply admission only
while no permitted collection pass takes the freed capacity first.

Watermarks and forwarding-only objects
--------------------------------------

With a nonzero high watermark, STAT reports ``memory_mode`` as
``IORING_OPAQUE_MEMORY_NORMAL`` or ``IORING_OPAQUE_MEMORY_LOWMEM``. The
watermarks measure payload held: cached objects, undecided bytes and objects
being assembled. Payload reaching the high watermark enters lowmem; return to
normal occurs at or below the low watermark. A compaction copy is not counted
until its request completes, so a compaction that lowers occupancy cannot
enter lowmem. Framing snapshots never change the mode. Importing rings
observe the same store mode.

RECV_OBJECT samples the mode when reserving its object slot. Normal admission
creates a cacheable object, even if its later assembly crosses the high
watermark. Lowmem admission marks the object forwarding-only. Headers remain
available through INSPECT, and DISCARD can skip future bodies without
backing. The complete-object notification reports the classification;
OBJECT_STAT also reports ``IORING_OPAQUE_OBJECT_F_FORWARD_ONLY``.

The classification is advisory. It tells the application that the object
arrived under memory pressure and should be forwarded and released promptly,
with SEND_LAST or FREE. The kernel does not restrict how the object is sent:
ordinary SEND, fan-out to several destinations and retry after a failed send
remain available, and READ_OBJECT remains a bounded copy. The classification
survives compaction and a return to normal mode. Automatic compaction skips
forwarding-only objects; manual compaction does not. No new syscall or
application submission flag is required.

Forwarding still needs temporary backing until an object is complete and its
consuming send or discard releases it. The kernel does not expire handles or
drop payloads automatically. Applications must promptly finish forwarding-only
objects; outstanding assemblies and sends remain subject to the hard limit,
and headers can encounter hard-limit backpressure when temporary capacity is
exhausted. Watermarks control admission classification, not an absolute cap on
cacheable bytes or a guarantee of unlimited forwarding progress.

The hard limit bounds store-owned backing, framing and reserved replacement capacity.
It is not a census or bound of all physical memory retained by networking.
TCP can retain pages after the store drops its last reference and accounting
charge. Socket accounting and shared page-pool/compound-page slack must be
considered separately in physical-memory evaluation. OBJECT_STAT's summed
extent charges can overestimate exclusive reclaimable backing when it is shared.

Compaction algorithm
====================

Manual and automatic compaction use the same copy and publication algorithm:

1. Pin the source version and claim the slot for compaction.
2. Reserve ``round_up(length, PAGE_SIZE)`` destination bytes under the hard
   limit while source backing remains charged.
3. Allocate chunks of at most 64 KiB. Choose the largest power of two within
   the remaining rounded size and fall back through smaller orders to base
   pages. Only the final page has byte slack; the tail is not oversized.
4. Copy with a persistent source cursor outside stream/table locks. Check
   cancellation and store shutdown between page-sized copies and yield
   between chunks. Replacement allocations use the registration owner's memcg.
5. Build the replacement vector, revalidate generation and source identity,
   and atomically replace the table's backing reference without changing the
   handle. Existing operations continue with their old version.
6. Release the old table reference and unused reservation. On failure, release
   all partial replacement backing and reservations, retaining the original.

Allocated destination bytes consume the reservation rather than adding a
second charge. Thus total accounting is source/other backing plus allocated
destination plus unallocated destination reservation. For a fixture holding
80,000 bytes in 40 independently charged 4 KiB pages, dense backing charges
81,920 bytes and peak source-plus-destination charge is 245,760 bytes. The
preferred allocations produce two extents; fallback may produce more.

Automatic compaction is opt-in through ``IORING_OPAQUE_POLICY_F_AUTO_COMPACT``.
Policy exposes only copy bytes per second, burst allowance and maximum
temporary backing. Candidate age, savings and fragmentation selection are
implementation heuristics, not ABI guarantees. The current implementation
waits at least 100 milliseconds and favors worthwhile backing savings or
multiple extents; these thresholds can change without changing the ABI. One
delayed compaction dispatcher per store handles manual requests and automatic
selection. An automatic scan examines at most 32 candidates and copies at
most one object, with the next scan paced independently of manual wakeups.
The single work item serializes copying without a second worker or copy mutex.
FREE and SEND_LAST remove
candidates; forwarding-only objects are excluded, dense versions are removed
after success, and failures back off. Publication queues the next paced scan
without resetting its deadline on every new object; manual requests can still
wake the dispatcher immediately.
Ordinary reception cannot consume configured compaction headroom. Neither
policy nor manual requests bypass the hard limit, and no objects are evicted.
Objects exceeding the burst or temporary allowance remain ineligible until
policy changes. Permanently ineligible candidates are removed from the scan
list so they do not keep an idle timer running; a policy update reconsiders
all non-dense objects. A transient token shortage or failed allocation keeps
its candidate for a later retry.

The dispatcher uses the shared unbound worker pool and processes at most
eight manual requests per invocation before checking automatic policy. Stores
do not create a private rescuer thread. Disabling automatic policy prevents
further automatic attempts without canceling accepted manual requests or
waiting under the submission lock; an already selected copy may finish.
Shutdown closes enrollment and drains every accepted manual request with
cancellation before freeing the store. A separate delayed allocation-retry
work item wakes receive waiters independently of long compaction copies;
ordinary quota release wakes them directly. Store destruction waits for its
own work items.

An in-flight compaction holds its own source reference. If SEND_LAST consumes
the handle before compaction publication, source/generation revalidation
rejects publication and replacement backing is released. A manual COMPACT
returns ESTALE; neither compaction path can restore the handle or overwrite
a reused slot. If compaction publishes first, SEND_LAST claims the replacement
under the same handle. Both send modes use the same compaction machinery.

Stores in the area of a MAIO instance
=====================================

A store can bind a MAIO zcrx instance. A MAIO instance owns an area of kernel
pages, and it installs a provider of readable pages on the receive queues of a
device. See ``Documentation/networking/iou-zcrx.rst``. A *bound* store keeps
every byte that it holds in the area. A process with ``CAP_NET_ADMIN`` maps the
area read-write. It then reads and writes the bytes of an object in place. The
object interface does not change. The binding is optional and needs
``CONFIG_IO_URING_ZCRX``. A store that is not bound behaves as the rest of this
document describes.

The word "immutable" in this document names the list of pages of a backing
version. It does not name the bytes of a bound store. The kernel never relies
on the bytes of an object for its own safety. It copies framing from user
memory before admission. It reads the bytes of an object only to copy or to
transmit them. It never reads a length or a checksum from them.

Binding
-------

Set ``IORING_OPAQUE_CFG_MAIO`` in ``flags`` of ``struct io_uring_opaque_config``
and the zcrx id of the instance in ``zcrx_id``. The instance belongs to the
ring that registers the store. It is a MAIO instance that this ring
registered or imported. Registration takes the instance under the lock of
the ring. These errors are possible:

* ``EINVAL``: ``zcrx_id`` is not 0 and the flag is not set, or a flag is
  unknown.
* ``ENXIO``: the ring has no MAIO instance with this id. A zcrx instance that
  is not a MAIO instance does not count.
* ``EOPNOTSUPP``: the kernel has no ``CONFIG_IO_URING_ZCRX``.

A configuration with ``IORING_OPAQUE_CFG_MAIO`` and a ``zcrx_id`` of 0 binds
the instance with id 0. One store binds one instance. The store registration
checks no capability. The ring needs the instance, and the registration or
the export of the instance carries the check for ``CAP_NET_ADMIN``.

A ring that imports the store through ``IORING_OPAQUE_REG_IMPORT`` shares the
binding of the creating ring. The importing ring does not need the instance.
It needs an import of the instance to map the area.

Lifetime
--------

The store holds a reference to the instance from registration until the
store is freed. The store is freed after its last object released its pages.
The instance can be unregistered first, for example when the ring that
registered it exits while another ring holds the imported store. The area
then stays, and the store keeps taking free pages from it. The objects keep
their pages, and READ_OBJECT, EXTENTS, new objects and FREE work. A ring that
has no instance cannot map the area. A mapping that exists stays valid.
The store can also end first. Its objects then release their pages into the
instance, and the owner of the instance sees the free pages again.

The provider of the instance can go away while a bound store holds pages. A
queue restart, the removal of the provider and the removal of the device do
that. A page pool that still has pages held by objects cannot finish its
destruction. The kernel can log a stalled pool shutdown for it. The pool ends
when the objects release the pages.

Capture
-------

A bound store decides for each page of a socket buffer how to hold it:

* A page that a page pool of the instance owns in the area is held by a pool
  reference. The socket buffer must be marked for page pool recycling
  (``skb->pp_recycle``). Only such a buffer holds a pool reference of its own,
  and that reference keeps the pool fields of the page stable. The pool
  reference of the store keeps the page out of the pool. When the last holder
  drops its reference, the page returns to the pool.
* Another page of the area is an *outsider page*. For example, the process
  can send its own area memory with ``SEND_ZC`` and receive it back over
  loopback. An outsider page, and a page that a pool owns but an unmarked
  socket buffer holds, is held by a plain page reference. The bytes are in
  the area, so the object counts as mapped. The page waits in limbo when a
  pool releases it while the store holds it.
* The store copies every other byte into free pages of the area. These are
  *copy pages*. The copied bytes include the bytes in pages outside the area.
  They include the bytes in fallback pages that a pool got from the page
  allocator. They also include the bytes that the capture copies today: those
  of shared and cloned socket buffers, and those over the capture limit. The
  store fills a copy page before it takes the next one.

``copied_bytes`` in STAT counts the bytes that the capture copied. A byte
that the store holds by reference is not in it. A bound store copies into area
pages, and an unbound store copies into pages of its own. The retention limit,
the extent limits, the watermarks and the forwarding-only classification
apply as before.

The area pages of a bound store are not charged to the memory cgroup of the
store. The instance accounted the area when it was registered. The bytes that
the store holds count against ``hard_limit`` as for any page. The store
registration reserves ``hard_limit`` against the locked memory limit, as for
an unbound store, on top of the pages of the area. The two charges can cover
the same pages.

The area can have no free page when the capture needs a copy page. The
capture then behaves as when a page allocation fails. RECV_OBJECT posts no
CQE. The collector waits 50 milliseconds and tries again. The stream stays
valid, and the captured prefix stays. The request finishes when objects are
freed and pages return. ``IORING_OP_ASYNC_CANCEL`` ends it with ``ECANCELED``
and restores its prefix. A RECV_OBJECT that needs more pages than the area can
ever give waits until the application cancels it. A quota failure is
different. It completes the request with ``ENOBUFS``.

Extents
-------

``IORING_OPAQUE_OBJECT_EXTENTS`` lists where the bytes of an object are in
the area. Set the handle in ``addr``, the size of the buffer in ``len`` and
the address of the buffer in ``addr3``. ``off`` is 0. The kernel writes one
``struct io_uring_opaque_extent`` for each extent, in object order::

  struct io_uring_opaque_extent {
    __u64 area_off;
    __u64 len;
  };

``area_off`` has the zcrx offset format: the area id in the bits from
``IORING_ZCRX_AREA_SHIFT`` and the byte offset in the area below. The mapping
of the area gives the bytes at ``area_off & ((1ULL << IORING_ZCRX_AREA_SHIFT)
- 1)``. An extent does not cross a page. The ``len`` values of all extents add
up to the length of the object. The result is the number of bytes written.

If the buffer is too small, nothing is written. The result is ``EOVERFLOW``
and the first extra CQE word holds the needed size. A request with ``len`` 0
and ``addr3`` 0 asks for the size in this way. A ``len`` that is not 0 with an
``addr3`` of 0, or an ``off`` that is not 0, gives ``EINVAL``. A stale handle
gives ``ESTALE``. A store without an instance gives ``EOPNOTSUPP``, also for a
stale handle. ``IORING_OPAQUE_EXTENT_UNMAPPED`` is the ``area_off`` of an
extent that is not in the area. A bound store has no such extent.

``IORING_OPAQUE_OBJECT_F_MAPPED`` says that every extent of the object is in
the area. The flag is in ``flags`` of OBJECT_STAT and in the second extra word
of the RECV_OBJECT completion, next to ``IORING_OPAQUE_OBJECT_F_FORWARD_ONLY``.
It is always set for an object of a bound store. An object of an unbound store
never has it.

Writes
------

The process can write the bytes of an object through the mapping. A write is
visible to every holder of the page. The holders are other objects that share
it, sends in flight, and sockets whose segments reference it. A send transmits
the bytes that are present when TCP builds the segment. A write that races a
send is a race of the application, as with ``SEND_ZC`` buffers. On a device
without checksum offload, TCP can sum the bytes when it builds the segment. A
write after that makes the sum wrong, and the peer drops the segment. This
race does not affect the safety of the kernel.

Compaction
----------

A dense copy would move the bytes out of the area. So a manual COMPACT of an
object with ``IORING_OPAQUE_OBJECT_F_MAPPED`` returns ``EOPNOTSUPP``. Automatic
compaction skips these objects. It does so for an object that exists when the
policy changes and for an object that arrives later. The same policy still
compacts the objects of an unbound store.

Validation and reproduction
===========================

``tools/testing/selftests/io_uring/opaque_obj`` exercises the raw SQE/CQE ABI
without liburing. It covers fragmented framing, complete publication, ranged
sends, shared stores, stale handles, cancellation and linked timeouts, manual
and automatic compaction, old-version sends during replacement and FREE,
SEND_LAST ownership reporting on rejection, short sends, errors and cancellation,
competing prepared final sends across rings, cache resends, memory-limit recovery,
receive ownership, EOF, teardown and unprivileged registration. The tests use
loopback TCP; they do not measure performance. Collector stress includes
10,000 separate arrival/drain cycles and 4 GiB of continuous DISCARD traffic,
crossing the TCP sequence-number wrap. Slow-window sends validate full byte
results and peer payload order; partial head sends cancel queued followers
and linked final sends without transferring unissued object ownership.
Tests also cover interleaved fragment charges, prefix restoration, batched
compaction cancellation, socket errors at full quota, IPv4/IPv6 disconnect
ownership, socket-file lifetime, POLL_FIRST and attach with a full CQ. Quota
notification tests check unread TCP bytes, coalescing, resumption after a
cross-ring release, and a full CQ while reporting pressure.
The SMP test runs 10,000 cross-ring SEND cancellation/promotion races with
heads and followers on different CPUs. It covers complete and partial heads,
explicit cancellation and linked timeouts, one completion per request, LAST
ownership, wire order and reference drain. It is skipped with fewer than two
available CPUs. Disconnect tests verify a blocked 16 MiB send and connected
socket state survive AF_UNSPEC rejection for IPv4 and IPv6. Same-batch
cancellation and inspection verify immediate receive-prefix restoration.
Additional tests cancel waiting followers before and after head cleanup, on
one ring and shared rings. They verify lowmem hysteresis, short header
inspection, advisory forwarding-only sends, stable classification through
compaction, compaction near the high watermark, retained undecided bytes and
held collection, split headers collected by a waiting INSPECT, same-offset
re-inspection, a decision past an uncollected gap, a flooding stream that
cannot refill freed capacity, protected replies when one collection pass
fills the store, and replies beside framing blocked on slow peers.

Build against the patched UAPI, using an already configured kernel build::

  build=/absolute/path/to/kernel-build
  make O="$build" headers_install INSTALL_HDR_PATH="$build/headers"
  make -C tools/testing/selftests/io_uring \
      KHDR_INCLUDES="-I$build/headers/include"

Run ``tools/testing/selftests/io_uring/opaque_obj`` on the patched kernel.
Registration permission can also depend on the system's io_uring policy.
The capabilities test needs a root fixture and is skipped otherwise.
The retained-page memory test needs a writable cgroup2 subtree with the memory
controller enabled. Targeted capture and publication ENOMEM tests need root,
debugfs, visible kernel symbols, FAILSLAB and the fault-injection stack filter.
They verify that a fault was actually injected and restore failslab settings.
The VM helper mounts these fixtures; enable the corresponding kernel options
to run the allocation-failure tests instead of skipping them.

The raw ABI tests also exercise short and extended registration/configuration,
zero and nonzero unknown extensions, versioned statistics and policy,
read-only registration output rollback, descriptor-only store lifetime, and
duplicate imports sharing invalidation. The TCP receive-claim configuration
belongs to networking (``CONFIG_SOCK_RX_OWNER``); the opaque feature selects
it and defaults off. The claim enforces stream semantics against competing
readers; it is not presented as a kernel memory-safety requirement.

``CONFIG_IO_URING_OPAQUE_OBJ_KUNIT_TEST`` adds capture identity/ownership,
compound allocation and oversized-copy, charge splitting, exact compaction
reservation, version lifetime, rollback, cancellation and generation-retirement
tests, plus final-send reference transfer and compaction publication races with
slot reuse, allocation rotation/exhaustion and generations beyond 32 bits.
Memory-policy cases cover watermark transitions, compaction copies excluded
from the watermarks, framing that takes the reply reserve only after capture
capacity, the collection gate (retained undecided bytes, held passes and
decided bytes through a held gate), and forwarding-only version replacement
with advisory send admission.
``CONFIG_KUNIT`` is required. Compaction and tests compile as separate source
files instead of being included by the collector. Lockdep and atomic-sleep
checks are useful for validating ownership and worker paths.

The selftest also binds stores to a MAIO instance. Most of these tests use
``ZCRX_REG_NODEV``, so every byte arrives by copy into the area. They need
``CAP_NET_ADMIN`` and ``CONFIG_IO_URING_ZCRX``. Two tests use a ``netdevsim``
port with a peer port in a second network namespace. The pool of the port
puts the payload into pages of the area, and the store holds them by pool
reference. These two tests need ``CONFIG_NETDEVSIM``, ``CONFIG_NET_NS`` and
root. A test skips when its requirement is missing. The KUnit tests of
bound stores also need ``CONFIG_IO_URING_ZCRX_KUNIT_TEST``.

To test without replacing the host kernel, build ``bzImage`` with loopback,
initramfs and serial-console support, build a static test, and use the optional
QEMU helper. It requires QEMU x86_64, cpio and static BusyBox::

  gcc -O2 -Wall -Wextra -Werror -static -pthread \
      -I"$build/headers/include" \
      -o "$build/opaque_obj.static" \
      tools/testing/selftests/io_uring/opaque_obj.c
  tools/testing/selftests/io_uring/run_opaque_vm.sh \
      "$build" "$build/opaque_obj.static"

The helper reports userspace and enabled KUnit results and rejects kernel
warnings. Its default TCG accelerator avoids any need for KVM permissions.
``BUSYBOX``, ``QEMU``, ``OPAQUE_VM_ACCEL`` and ``OPAQUE_VM_TIMEOUT`` can override
the executable paths, accelerator and host timeout. ``OPAQUE_TEST_TIMEOUT``
sets the userspace test's alarm in seconds (default 600); the helper passes it
into the VM. For slow SMP/KASAN emulation, increase both timeouts, for example
``OPAQUE_TEST_TIMEOUT=7200 OPAQUE_VM_TIMEOUT=7500``. Logs remain in the kernel
build's ``opaque-vm`` directory.
