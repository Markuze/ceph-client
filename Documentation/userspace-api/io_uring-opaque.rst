.. SPDX-License-Identifier: GPL-2.0

===================================
io_uring opaque TCP payload objects
===================================

OPAQUE_OBJ retains immutable TCP payloads in the kernel. Applications inspect
framing through bounded copies, select complete payload ranges, and transmit
or cache those ranges through generation handles. The payload is never mapped
into the application's address space. Eligible ordinary RX pages are retained;
unsafe backing is copied into independently owned pages. An allocation larger
than the entire capture limit also uses this fallback so that small stores can
make progress.

This RFC originates from the kpass stream/object prototype, revision
``a436cfe75e2c`` on its ``opaqu_objects`` branch. It replaces the device command
transport with native io_uring operations and ordinary socket descriptors.

Registration and sharing
========================

Enable ``CONFIG_IO_URING_OPAQUE_OBJ``. Create a ring with
``IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN`` and either
``IORING_SETUP_CQE32`` or ``IORING_SETUP_CQE_MIXED``. Ordinary 64-byte SQEs
suffice. No network device configuration or CAP_NET_ADMIN is required.

Register through ``IORING_REGISTER_ZCRX_IFQ`` with ``ZCRX_REG_OPAQUE_OBJ`` and
``opaque_config`` pointing to ``struct io_uring_opaque_config``. Set the device,
receive queue, payload area, refill region, event descriptor and all other
reserved fields to zero. The returned ``zcrx_id`` selects the store.

All configuration limits are explicit. ``hard_limit`` and
``compact_headroom`` are page aligned; ordinary capture cannot use headroom.
The hard limit includes headroom. Object size, object slots, stream slots,
extents and pending requests have separate bounds. Registering reserves the
hard-limit capacity against the registration owner's locked-memory accounting.
The payload is allocated or retained on demand.

``ZCRX_CTRL_EXPORT`` returns a store descriptor. Import it into another ring
with ``ZCRX_REG_IMPORT | ZCRX_REG_OPAQUE_OBJ`` and the descriptor in ``if_idx``.
Context IDs are ring local; stream and object handles, limits and policy belong
to the shared store. Mapping and refill controls reject opaque stores. The
last ring/export-descriptor user stops collection and automatic work; request
references drain before the store is destroyed.

Receive and range decisions
===========================

Submit ``IORING_OP_RECV_ZC`` on a connected TCP socket, selecting the store in
``zcrx_ifq_idx``, with ``IORING_RECV_MULTISHOT`` and a zero length. The first
32-byte CQE has result zero, ``IORING_CQE_F_MORE``, and a stream token in its
first extra word. Stream offset zero denotes the first unread byte when
ownership is acquired. No bytes are consumed before this CQE can be posted.
A terminal CQE ends the collector on EOF, error, cancellation or stream close.
The RFC collector rejects ``IOSQE_ASYNC`` and CQE suppression.

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
``ioprio``, the store in ``zcrx_ifq_idx``, a handle/token in ``addr``, a byte
offset in ``off``, length in ``len``, and any user pointer in ``addr3``.
Other operation-specific fields are zero. Personality and ordinary io_uring
linking/cancellation retain their usual meaning. CQE suppression is rejected.

.. list-table:: Object controls
   :header-rows: 1
   :widths: 25 75

   * - Control
     - Meaning
   * - READ_STREAM
     - Copy a complete unclassified stream range, at most 64 KiB, without
       consuming it. Future reads wait for all requested bytes.
   * - KEEP
     - Consume a selected range into private assembly. Publish an object only
       when the entire range and its vector are complete. Result is length;
       the first extra CQE word holds the generation handle.
   * - DISCARD
     - Consume unwanted buffered or future stream bytes. Future bytes can be
       skipped without allocating backing, even when the store is full.
   * - READ
     - Copy an object subrange of at most 64 KiB to the supplied address.
   * - FREE
     - Invalidate a published object handle and release its table reference.
       Already pinned backing versions and TCP page references remain valid.
   * - STAT / OBJECT_STAT / STREAM_STAT
     - Copy the corresponding UAPI structure; ``len`` is its exact size.
       STAT uses a zero handle and offset.
   * - STREAM_CLOSE
     - Release the receive claim and undecided bytes, terminate pending ranges,
       and remove the stream token. Published objects remain independent.
   * - COMPACT
     - Replace backing under the same object handle; result is zero on success.
   * - SET_POLICY
     - Replace automatic compaction policy from the supplied UAPI structure.
       Handle and offset are zero; ``len`` is the structure's exact size.

KEEP/DISCARD ranges do not overlap each other or pending READ_STREAM ranges.
Nonconsuming reads may overlap each other. Decisions are indexed in stream
order; capture consults the earliest decision rather than scanning all pending
requests. A following header can be read independently of a preceding KEEP.
Object slots and private assembly are reserved before a KEEP can consume data.
Generation exhaustion retires a slot instead of wrapping a stale handle.
KEEP lengths exceeding either the configured object-size bound or the entire
capture allowance are rejected with EMSGSIZE. Capture falls back to packed
page copies when retaining another allocation would leave no room for dense
backing of the remaining private range. If physical backing or extent limits
still stop a partial KEEP, it completes with ENOBUFS and restores its prefix;
it does not indefinitely pin quota waiting for its own completion. A range
with no captured prefix can wait for quota release. Deadlines and cancellation
remain available. Compaction operates on published objects.

EOF before completion produces ENODATA; network errors propagate. ASYNC_CANCEL,
linked timeouts, stream close and ring teardown terminate pending ranges.
An unsuccessful KEEP restores its private prefix at the original stream
offsets before reporting completion, including cancellation, a linked timeout,
EOF, a network error or vector allocation failure. The application can retry
KEEP, inspect that prefix with READ_STREAM, or DISCARD it. No incomplete object
is published. STREAM_CLOSE and store teardown explicitly release all remaining
stream bytes instead of preserving them for retry. DISCARD remains consuming:
bytes it already discarded are not restored if the rest of its range fails.

Transmission and ownership
==========================

``IORING_OP_OPAQUE_OBJ_SEND`` takes an ordinary destination socket descriptor,
the store ID in ``zcrx_ifq_idx``, an object handle in ``addr``, offset in ``off``
and positive length in ``len``. ``MSG_MORE`` and ``MSG_DONTWAIT`` are accepted
in ``msg_flags``. ``ioprio`` is zero for reusable sends or contains
``IORING_OPAQUE_SEND_LAST`` for a consuming send. Other operation-specific
fields are zero. SEND_LAST rejects ``IOSQE_CQE_SKIP_SUCCESS`` so ownership
transfer is always reported.

Ordinary SEND first issue pins the current immutable backing version, leaving
the handle available for caching, further reads and sends to other destinations.
SEND preparation validates SQE fields without resolving the handle. At first
issue, after destination validation and TX queue allocation, SEND_LAST
it atomically removes the handle and transfers the table reference into the
request. It uses the version current at that point, including any compaction
published since preparation. Admission behind an earlier send also takes
ownership before waiting for its turn; subsequent retries retain that reference.

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

Issue uses the retained bvec array and saved iterator with ``MSG_SPLICE_PAGES``.
One ``sock_sendmsg`` call is made per issue attempt, including the whole remaining
range. Partial progress resumes at the saved cursor. Without MSG_DONTWAIT,
the request continues until the range is queued or an error/cancellation ends
it. Failure before progress returns a negative errno; failure after progress
returns the positive queued prefix. MSG_DONTWAIT permits a short result.

The store serializes admitted sends to each destination socket. If the head
ends before queuing its entire range, already admitted followers complete with
ECANCELED rather than inserting another object's bytes after that prefix.
A canceled follower that already claimed SEND_LAST still reports the consumed
flag. Canceling a follower alone does not interrupt an earlier send. Blocking
TCP retries run in io-wq without holding the ring submission lock.

Each send has one application completion. This reports bytes queued to TCP,
not peer delivery or acknowledgment. No user-buffer reuse notification is
needed: the payload cannot be overwritten by userspace, and TCP owns ordinary
page references. FREE after queued sends can invalidate the handle while TCP
still retains those pages. SEND_LAST does this without a separate FREE SQE or
CQE and transfers a reference rather than adding and dropping a send pin.
An ordinary SEND awaiting first issue has not acquired its backing reference;
users must order handle release accordingly.

Control operations also resolve stream tokens and object handles at first
issue. A READ or SEND linked after FREE observes ESTALE; one linked after a
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

References move from undecided stream extents into private KEEP assembly and
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
Vectors are built once at object publication and are bounded by extent limits.
Releasing backing or metadata wakes eligible collectors suspended on the store
budget in bounded batches. Their native multishot polls remain armed for socket
errors and urgent data. Transient capture ENOMEM backs off for 50 milliseconds
and retries; it does not poison the stream or discard its captured prefix.
FREE, STAT, STREAM_CLOSE and SET_POLICY remain admitted at the request cap.

The hard limit bounds store-owned backing and reserved replacement capacity.
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

Automatic compaction is opt-in. Policy supplies minimum object age, saving,
slack percentage and extent count, copy bytes per second, burst allowance and
maximum temporary backing. One delayed worker per store scans at most 32
candidates per invocation and copies at most one object. A store mutex
serializes its copies with manual requests. FREE and SEND_LAST remove
candidates; dense versions are removed after success; failures back off.
Ordinary reception cannot consume configured compaction headroom. Neither
policy nor manual requests bypass the hard limit, and no objects are evicted.
Objects exceeding the burst or temporary allowance remain ineligible until
policy changes.

Manual and automatic copies use the shared unbound worker pool, with one
active copy per store and a bounded manual dispatcher. Stores do not create
a private rescuer thread. Disabling automatic policy prevents further
automatic attempts without waiting under the submission lock; an already
selected copy may finish. Store destruction waits for its own workers.

An in-flight compaction holds its own source reference. If SEND_LAST consumes
the handle before compaction publication, source/generation revalidation
rejects publication and replacement backing is released. A manual COMPACT
returns ESTALE; neither compaction path can restore the handle or overwrite
a reused slot. If compaction publishes first, SEND_LAST claims the replacement
under the same handle. Both send modes use the same compaction machinery.

Claims and comparison boundaries
================================

The code supports these architectural claims:

* Hardware-independent operation on ordinary connected TCP sockets.
* No mapped payload area, application RX-chunk refill queue, or user-buffer
  overwrite/reuse protocol.
* Complete object publication and stable generation handles across backing
  changes, with object-level application lifetime management.
* One application completion per object send, without SEND_ZC's separate
  notification request and notification task work.
* Opt-in final-send ownership transfer removes one FREE SQE, CQE and request,
  while reusable sends retain the ordinary caching and fanout lifetime.
* Retention of eligible ordinary RX pages instead of a bulk receive copy.
* Reusable kernel scatter vectors and optional dense backing for cached or
  repeatedly transmitted payloads.

These mechanisms provide performance hypotheses, not measured speedups. The
strongest candidates are fragmented payloads that are forwarded, cached or
sent to multiple destinations while the application inspects only framing.
For an illustrative object delivered in N receive completions and sent in S
requests, a typical mapped receive/send-ZC flow produces N + 2S CQEs. An opaque
flow with H bounded reads and D discards produces H + 1 KEEP + S SEND + 1 FREE
+ D CQEs, or H + 1 KEEP + S SEND + D when the final send uses SEND_LAST,
excluding setup/terminal CQEs and failures. Refill entries are a separate cost,
not additional CQEs. Small N or extra reads can erase this gain;
CQE batching means the count does not predict syscall or wakeup counts.

An optimized comparison must include fixed-buffer and vectored SEND_ZC.
Fixed buffers already avoid repeated user-page pinning. Its managed fragment
references can avoid per-fragment page refcount operations that our ordinary
splice path performs. Vectored sends can already transmit a scattered payload
with one submission. Hardware RECV_ZC already avoids the payload receive copy
and uses preallocated receive resources; its ordinary-page fallback copies
into the registered area. Mapped headers can be inspected without our copy
operations. Objects add handle lookup, assembly, metadata, quota and compaction
work; simpler application management does not imply a smaller whole-kernel
implementation or universally fewer instructions.

No throughput, latency, CPU-efficiency or physical-NIC superiority is claimed
by this RFC. Compare ordinary RECV/SEND_ZC, device-less RECV_ZC/SEND_ZC and
hardware RECV_ZC/fixed-buffer SEND_ZC with matched batching, framing, payload
sizes and retention. Measure copied bytes, cycles, CQEs, allocations, page
references and actual retained memory with compaction off, manual and automatic.

Validation and reproduction
===========================

``tools/testing/selftests/io_uring/opaque_obj`` exercises the raw SQE/CQE ABI
without liburing. It covers fragmented framing, complete publication, ranged
sends, shared stores, stale handles, cancellation and linked timeouts, manual
and automatic compaction, old-version sends during replacement and FREE,
SEND_LAST ownership reporting on rejection, short sends, errors and cancellation,
competing prepared final sends across rings, cache resends, memory-limit recovery,
receive ownership, EOF, teardown and unprivileged registration. The tests use
loopback TCP; they do not measure performance.

Build against the patched UAPI, using an already configured kernel build::

  build=/absolute/path/to/kernel-build
  make O="$build" headers_install INSTALL_HDR_PATH="$build/headers"
  make -C tools/testing/selftests/io_uring \
      KHDR_INCLUDES="-I$build/headers/include"

Run ``tools/testing/selftests/io_uring/opaque_obj`` on the patched kernel.
Registration permission can also depend on the system's io_uring policy.
The capabilities test needs a root fixture and is skipped otherwise.

``CONFIG_IO_URING_OPAQUE_OBJ_KUNIT_TEST`` adds capture identity/ownership,
compound allocation and oversized-copy, charge splitting, exact compaction
reservation, version lifetime, rollback, cancellation and generation-retirement
tests, plus final-send reference transfer and compaction publication races with
slot reuse. ``CONFIG_KUNIT`` is required. Lockdep and atomic-sleep checks are
useful for validating ownership and worker paths.

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
the executable paths, accelerator and timeout. Logs remain in the kernel
build's ``opaque-vm`` directory.
