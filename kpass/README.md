# kpass development on Linux 7.2

The `opaqu_objects` branch implements TCP stream accumulation, header reads,
range assembly and completed opaque objects on top of the kpass prototype.
The module retains pages with `tcp_read_sock` and sends them with
`MSG_SPLICE_PAGES`. Userspace chooses stream ranges and receives object IDs
only after those ranges are complete. The original buffer API remains
available for the older demos.

## Source baseline

| Component | Pinned revision |
| --- | --- |
| Linux stable | `v7.2.9`, `5fce161649b4d779d1b76d9fcd52dc77779774b8` |
| Original kpass branch | `origin/kpass`, `f55aa3a997652da7bd5ecd7daa326a65177b4d2d` |
| Imported kpass patch | `59ae49afff3337936fe556422e46a610652cea72` |
| Import on this branch | `ae376b138283` |
| Initial Linux 7.2 port | `9c2a31ff39d7` |
| RX capture and page ownership | `14ccd3ca674b` |
| Independent RX fallback page lifetimes | `9f207f6ddc2e` |
| Userspace dependency | liburing `2.15`, `d41bf9220ec39277ff235379e9089d9e0fd6c2a5` |

The local `master` was fast-forwarded to `v7.2.9` before creating this
branch. The import retains its original author and a cherry-pick provenance
line. Its `.gitignore` conflict was resolved by retaining both upstream
exclusions and kpass build exclusions.

Use 7.2 stable releases for this work. Upgrade to 7.3 after the final
release is available, and validate that upgrade before changing an
evaluation baseline. Keep the import and port fixes as separate commits.

## Reproduce the build

Run these commands from the Linux repository root. Required build tools
include GCC, binutils, make, flex, bison, bc, pkg-config, and the OpenSSL and
libelf development headers. The initial build used GCC 11.4.0 on x86-64.

Build liburing in a local prefix; the host's liburing 2.0 is too old for
this command transport. The build currently requires liburing 2.15 or
newer, and 2.15 is the pinned dependency for this baseline.

```sh
research=$(cd .. && pwd)
uring_src="$research/deps/liburing-2.15"
uring_prefix="$research/deps/liburing-2.15-install"
build="$research/build/opaqu_objects-7.2.9"

# First-time dependency setup; reuse this checkout on subsequent builds.
mkdir -p "$research/deps"
git clone --depth 1 --branch liburing-2.15 \
    https://github.com/axboe/liburing.git "$uring_src"
(
    cd "$uring_src"
    ./configure --prefix="$uring_prefix" \
        --libdir="$uring_prefix/lib" --libdevdir="$uring_prefix/lib"
    make -j4 library
    make install
)

export PKG_CONFIG_PATH="$uring_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$uring_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# First-time kernel configuration.
mkdir -p "$build"
make O="$build" x86_64_defconfig
scripts/kconfig/merge_config.sh -m -O "$build" \
    "$build/.config" kpass/configs/x86_64-build.config
make O="$build" olddefconfig

# Repeat these commands for incremental builds.
make -j16 O="$build" bzImage modules
make -C kpass -j4 lib demos check
make -C kpass KDIR="$build" module
```

The profile is an initial compile baseline based on `x86_64_defconfig`.
It is not a hardware-specific deployment configuration or the eventual
debug/performance evaluation configuration. Build the kernel before the
external module so that module symbol checking uses its `Module.symvers`.

Outputs:

- Kernel image: `$build/arch/x86/boot/bzImage`.
- Kernel configuration and symbols: `$build/.config`, `$build/Module.symvers`.
- Module: `kpass/kernel/ceph_kpass.ko`.
- Libraries: `kpass/lib/libceph_kpass.{a,so}`.
- Demos: `kpass/demo/echo_server`, `kpass/demo/test_client`.

## Verification and remaining work

On 2026-10-05, the kernel image and configured modules, external kpass
module, library, and demos built successfully. `make -C kpass check`
passed without loading a module. It checks real library command
preparation with two SQE128 entries: opcode, device FD, payload fields,
entry isolation, and rejection of undersized SQEs. The same test failed
against the imported library because command preparation returned NULL.

Port fixes use the 7.2 socket address types and io_uring SQE accessor,
require SQE128 for the 64-byte command, remove an invalid preprocessor
test of an enum value, and correct the liburing command helper's argument
order. The build resolves liburing through pkg-config.

The RX changes retain eligible page-backed linear heads using Linux splice's
`skb_head_is_locked` rule, walk chained skbs, and copy unsafe backing. They
also fix receive bounds, page-boundary handling, and capture reference
release. Fallback copies now use separately allocated pages whose references
can outlive their buffer handle or session. See
[KPASS.md](KPASS.md#rx-path-tcp_read_sock--actor) for the rules.

A separate 7.2.9 kernel with KUnit and `DEBUG_VM` was booted in QEMU. The
module loaded, all 15 RX tests passed, and the module unloaded. An initial
regression test failed against the original actor: it copied a page-backed
head instead of retaining its page. The complete suite includes a live
loopback TCP test through `tcp_read_sock`; its 4,224 bytes were captured with
zero RX fallback bytes on this x86-64 test configuration. This measures the
actor's copies, not copies performed by the sender or other network layers.

Two further regressions reproduced the old fallback-page overwrite before
the fix and now pass. One holds spliced pages across receive replacement,
free/reallocation of the same handle, and session destruction. The other
sends a copied capture twice through the actual TX worker over loopback TCP,
reposts the receive handle, and checks that the peer still reads both
original payloads.

The stream/object API now passes end-to-end userspace/io_uring tests in the
same VM. The suite checks automatic accumulation, split headers, future
ranges, independent next-header reads, multiple objects from one receive,
discard, stale IDs, EOF/reset, cancellation, close, budget stall/resume and
ring teardown. It compacts a 4 MiB object while a send is blocked, then sends
it again using the new backing. It checks every byte of both copies, releases
the handle while TX is pending, and verifies both cumulative send results. A
pending-operation limit test drains 1,024 terminal completions through a
smaller CQ. Accepted sockets, object lifetime after socket close, and legacy
INIT with CQE16 are covered.

Six additional KUnit tests check the stream's page identity, reference
sharing across splits, incomplete assembly, EOF cleanup, budget handling and
generation retirement and callback restoration. The callback regression
failed before the fix: accepted sockets restored inherited module callbacks
and the listener's private pointer was cloneable.

Six compaction KUnit tests cover packing, base-page backing, temporary memory
limits, cancellation before/after the copy, freed/reused handles, and cleanup
after a partial copy hits the extent cap. The packing fixture reduces 40
extents to 2 and the backing charge from 163,840 to 81,920 bytes for an
80,000-byte object, with identical bytes and a stable handle. The base-page
case forces that representation; allocator failure itself is not injected.
Userspace tests exercise the actual worker, ranged reads/sends across chunk
boundaries, insufficient budget, repeat compaction, explicit cancellation and
ring teardown during compaction. All temporary references drain. The total
is 27 passing kernel tests plus ten userspace test groups. The module loads
and unloads without kernel warnings in the test guest. Physical NIC traffic
and performance have not been tested.
The older buffer/POKE API retains the other limitations described in
[KPASS.md](KPASS.md); the new object send path has its own range and cursor
handling.

## Kernel and userspace VM tests

The test module includes `kernel/ceph_kpass_test.c`,
`kernel/ceph_kpass_stream_test.c` and `kernel/ceph_kpass_compact_test.c` only
when built with `KPASS_KUNIT_TEST=1`. Tests call the actual actor with
constructed skbs to check page identity, reference lifetime after skb release, cloned/slab
fallbacks, cross-page heads/fragments, nested `frag_list`, mutable shared
fragments, limits, unreadable data, and release/reuse. Copied pages are checked
for independence from the source skb and the demo pool, balanced references,
and stable TX contents across handle reuse. A real page pool checks
recycling behavior. A loopback TCP connection checks a short read followed
by capture of the remaining stream and retention after socket release.

Build a separate kernel so the normal build profile stays available:

```sh
research=$(cd .. && pwd)
test_build="$research/build/opaqu_objects-rx-test"
mkdir -p "$test_build"
cp "$research/build/opaqu_objects-7.2.9/.config" "$test_build/.config"
scripts/config --file "$test_build/.config" --enable KUNIT \
    --disable KUNIT_ALL_TESTS --enable DEBUG_VM \
    --set-str LOCALVERSION -opaqu-rx-test
make O="$test_build" olddefconfig
make -j16 O="$test_build" bzImage modules
kpass/tests/run_rx_kunit.sh "$test_build"
```

The runner requires QEMU (`qemu-system-x86_64`), a statically linked BusyBox,
cpio, file, timeout, and the liburing 2.15 build environment above. Set
`BUSYBOX` if the static binary is not the default. It builds the test module
and a statically linked `tests/stream_objects` binary, creates a minimal
initramfs, enables guest loopback, loads the module to run KUnit, exercises
the real device through io_uring, unloads it, and powers down the VM.
It uses unprivileged QEMU TCG by default; set `KPASS_QEMU_ACCEL=kvm` if the
current user can access `/dev/kvm`. It never loads the module on the host.
The first regression run used KVM; the complete 15-test run passed with
TCG using this runner.

The runner cleans generated files in `kpass/kernel` to satisfy Kbuild's
separate-output requirement. Its module and logs are under
`$test_build/kpass-rx-test/`; `results.log` records every case and the TCP
copy counts. The runner fails for failed/skipped tests, a missing completion
marker, failed module load/unload, or kernel warning/oops/panic. Rebuild the
normal module with the normal `KDIR` when needed.

## Stream/object API

Open `/dev/ceph_kpass` and create an io_uring with
`IORING_SETUP_SQE128 | IORING_SETUP_CQE32`. New sessions use stream mode
without an INIT command or a preallocated payload pool. The legacy INIT
command selects the old buffer API before creating any sockets. Device info
reports version 3. Stream and legacy buffer operations cannot share a session.

`include/uapi/ceph_kpass.h` defines the commands; `lib/kpass_objects.h`
provides preparation helpers. These helpers neither submit nor wait, and do
not consume CQEs. The caller owns the ring and supplies a full 64-bit
`user_data` tag, unique among its pending commands. The existing
`lib/ceph_kpass.h` API and demos continue to select legacy mode.

Commands retain the existing 64-byte payload and socket ID fields.
`cmd_op`, command flags and reserved fields are zero. The `stream` union
contains `object`, `offset`, `length` and `addr` (a user pointer).
The CQE's `res` holds a result or negative errno. Only successful KEEP
returns an object ID, in `big_cqe[0]`; other new commands return zero there.
CQE suppression is rejected so a created handle cannot lose its ready result.

| Command | Arguments and completion |
| --- | --- |
| SOCK_CREATE / CONNECT / LISTEN / ACCEPT / CLOSE | Existing IPv4 socket fields. CONNECT waits for establishment. ACCEPT returns the new socket ID. CLOSE cancels pending commands for that socket. |
| READ_STREAM | Socket, absolute byte offset, length, destination. Waits for the exact window, copies it before completion, leaves stream bytes available, and returns no object ID. |
| KEEP | Socket, offset, length. Moves received extents into a private assembly and appends future extents by reference. Completes with length and a handle only when the entire range has arrived. |
| DISCARD | Socket, offset, length. Releases buffered bytes and skips future bytes without retaining them. Completes with length once the full range has been consumed. |
| OBJECT_READ | Handle, object offset, length, destination. Explicit copy; the operation holds the object until completion. |
| OBJECT_FREE | Handle. Removes it from the table immediately; in-flight reads/sends retain their references. A compaction still waiting to publish fails with ESTALE. |
| OBJECT_SEND | Socket, handle, object offset, length. Queues that exact range. Each operation has an independent cursor; sends to one socket remain ordered and complete with the total length once queued to TCP. |
| OBJECT_COMPACT (0x59) | Handle; offset, length and addr zero. Copies to packed backing on a background worker and publishes it under the same handle; completes with 0. |
| OBJECT_STAT (0x5a) | Handle and `stream.addr` pointing to `struct kpass_object_stat`. Copies length, conservative backing charge, extent count and COMPACTED/COMPACTING flags; completes with 0. |
| LIMIT | `stream.length` sets the session's retained-backing limit, at least PAGE_SIZE. A value below the current charge fails with EBUSY. |
| STREAM_STAT | Socket and `stream.addr` pointing to `struct kpass_stream_stat`. Returns stream end, undecided bytes, session backing charge/limit, RX fallback copies, object/operation/extent counts, EOF and error. The pending count includes the STAT itself. |
| CANCEL | `stream.object` is the full tag of a pending command on the same ring and session. The target completes with ECANCELED; CANCEL returns 0 or ENOENT. |

Offsets start at zero for each socket and count TCP payload bytes, including
discarded bytes. They are independent of packet boundaries and raw TCP
sequence numbers. Ranges are half-open; the command carries start and length.
Stream ranges must be nonempty and at most INT_MAX bytes; overflow fails with
EINVAL. Object reads/sends allow a zero length and check bounds by subtraction.

Typical framing sequence, with H header bytes and O total object bytes:

```c
/* Preparation helpers require SQE128 + CQE32; the caller submits and waits. */
kpass_prep_range(&ring, sqe, fd, KPASS_OP_READ_STREAM, sock,
                0, p, H, header, header_tag);
/* After header_tag completes: parse O, then queue both commands. */
kpass_prep_range(&ring, keep_sqe, fd, KPASS_OP_KEEP, sock,
                0, p, O, NULL, object_tag);
kpass_prep_range(&ring, next_sqe, fd, KPASS_OP_READ_STREAM, sock,
                0, p + O, H, next_header, next_header_tag);
/* object_tag's successful CQE supplies kpass_cqe_object(cqe). */
```

For body-only objects, KEEP selects the body and DISCARD handles framing.
Reads may overlap other reads. A consuming decision overlapping a pending
read or another pending decision fails with EBUSY. Reading or selecting
already consumed bytes fails with ENODATA. The usual read-header, then
KEEP-object sequence has no overlap conflict because the header read has
already completed. Undecided bytes beyond an object stay in the accumulator.

Handles encode `generation << 32 | (slot + 1)`; zero is invalid. Reusing a
table slot increments its generation. An exhausted generation retires the
slot. Object commands return ESTALE for released handles. Completed objects
outlive their source sockets; sending again uses their current backing.
Compaction changes backing while preserving the bytes, length and handle.
Ordinary references protect pages; there is no separate sealing step.

EOF before the requested end returns ENODATA, and socket errors return their
errno, without a partial object ID. Cancellation or a failed KEEP releases
its assembled prefix; those consumed bytes are not restored to the stream.
Canceling DISCARD cannot restore bytes already dropped. Remaining undecided
bytes stay available until classified or the socket closes. A canceled or
failed send may already have queued a prefix to TCP.

On this pinned kernel, marking a uring command cancelable supplies ring/task
teardown cancellation; individual `IORING_OP_ASYNC_CANCEL` does not dispatch
to this driver. Use the explicit CANCEL command for individual operations.
Pending network and compaction commands retain the session file through io_uring.
Destination memory must remain valid until its read CQE.

### Background compaction

Compaction is explicit per completed object. `kpass_prep_compact(&ring, sqe,
fd, handle, tag)` prepares the command; the caller submits it and waits for
the CQE. No replacement handle is returned. Pending assemblies cannot be
compacted because they have no public handle.

`kernel/ceph_kpass_compact.c` implements one ordered compaction workqueue per
session, separate from the stream worker. It gathers scattered extents into
private compound-page allocations, preferring 64 KiB physically contiguous
chunks on the x86-64 baseline. Allocation falls back through smaller orders
to a base page. Each allocation is one zero-offset extent. Only the final
page has unused space; the total destination charge is the object length
rounded up to PAGE_SIZE. Large objects use multiple contiguous chunks.

The worker pins the source and reserves the full destination charge under
the session mutex before allocating. It copies outside that mutex, checks
cancellation between base-page copies and yields between chunks. In io_uring
completion task work, it checks the handle again and swaps its table entry
to the completed copy. The generation and object count stay unchanged.
Reads and sends that already pinned the old object finish on its pages;
later operations acquire the new backing. TCP also holds its own references
for queued bytes. Source socket closure does not cancel compaction.

Old and new backing coexist during the copy and while old operations still
need it. Both, including reserved destination bytes, count toward
`STREAM_STAT.backing_bytes` and LIMIT. If they cannot fit, COMPACT fails with
ENOBUFS and leaves the original object intact. Allocation failure returns
ENOMEM; the extent or pending-operation cap returns ENOSPC. Failure and
cancellation release partial output and unused reservations. A freed or
reused handle returns ESTALE at publication and cannot be resurrected.

A second compaction while one is pending returns EBUSY. Compaction of an
already compacted object succeeds without copying, even when the backing
budget is full. COMPACTING describes the current source while the request is
pending; COMPACTED describes the published packed backing. OBJECT_STAT sums
that object's extent charges; it excludes the replacement under construction
and older backing retained only by operations. STREAM_STAT includes those
session charges. Neither statistic is a unique physical-memory census.

The implementation has no automatic age/slack policy or copy-rate throttle.
Userspace chooses the objects and submission rate. Compaction deliberately
copies payload once in the kernel; its copies are separate from the RX
fallback byte counter. Throughput, CPU cost, scheduling effects and physical
NIC behavior remain to be measured.

### Backing, limits and execution

`kernel/ceph_kpass_stream.c` reuses the tested RX walker. It holds a page
reference per captured extent, shares that reference when splitting extent
metadata, and moves selected extents into pending objects. Header inspection
copies only the requested window. Unsafe RX backing uses the same independent
fallback pages as the original actor.

The default session backing limit is 64 MiB. Each capture charges the full
compound page it retains; capturing the same allocation several times can
overcount it. Splits and header snapshots share that capture's charge.
The charge follows undecided data, pending assemblies, completed objects,
active object operations and compaction output/reservations. TCP owns its own
references after a send. This is a conservative retention budget, not an
exact physical-memory measurement.
Metadata has separate fixed caps: 65,536 extents, 4,096 published objects,
and 1,024 pending I/O operations. Control commands remain available at the
pending-I/O cap. A metadata allocation/cap failure can fail the operation.

At the backing or capture-metadata limit, RX stops consuming TCP; freeing
backing, discarding bytes, or raising the limit wakes collection again.
Known future DISCARD ranges can still be consumed at the limit. An object
larger than the remaining budget waits until other data is released or the
limit is raised.

This implementation uses one session mutex, a stream work item and a separate
ordered compaction workqueue.
Socket callbacks schedule work; all user copies and object publication run
in io_uring task work. Each pending request owns copied command arguments,
independent of later SQE reuse. Socket callbacks are detached under the
callback lock before socket state is freed. The listener's private pointer
is marked non-copyable, and accepted sockets restore native callbacks.

The broader [object design](https://github.com/Markuze/zc_proxy/blob/main/OBJECTS.md)
also proposes BPF hooks, SPLIT/SPLICE between completed objects, WRITE,
framed/vector sends, socket import/namespaces, exact accounting, notifications
and automatic compaction policy. Those remain future work. This implementation
covers the userspace stream/assembly contract, object read/send/release and
explicit background compaction.

## Design references

[KPASS.md](KPASS.md) documents the imported prototype. The detailed source
review and proposed object/evaluation design live in the sibling
[`zc_proxy` paper repository](https://github.com/Markuze/zc_proxy): `KPASS.md`,
`OBJECTS.md`, `KV_CACHE.md`, and `workplan.md`.
