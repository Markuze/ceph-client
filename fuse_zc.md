# Zero-copy ceph-fuse reads with io_uring opaque objects

Status: **design.** No code has been written yet. 2026-10-11.

> **Superseded in part (2026-10-13).** The baseline has moved from v4 to OPAQUE_OBJ v9 (`875cf36709ca`). The implementation work plan is `fuse_zc_workplan.md`; where the two differ, the work plan wins. Its §1 lists the v9 changes. The main ones:
> - the per-stream receive reserve (§9.1.3) is dropped;
> - the RECV_OBJECT crc moves to the high half of the second extra word, because v9 uses that word for FORWARD_ONLY;
> - INSPECT must wait at the end offset;
> - pressure arrives as a nonterminal `-ENOBUFS` collector CQE;
> - an aborted frame is dropped, not faulted;
> - only requests flagged `FUSE_URING_ENT_ZERO_COPY` have a slot;
> - `READ_FIXED` from `/dev/zero` can complete short.
>
> The verified code facts are in `/mnt/workspace/fuse-zc/facts/`.

## Scope and sources

**Targets**
- The OPAQUE_OBJ RFC **v4**: `origin/upstream/opaque-obj-v4` at `c5bcea485987`. It is five patches on `af32da41b032` (7.3-rc6+). That base already contains FUSE io_uring zero-copy (`Documentation/filesystems/fuse/fuse-io-uring.rst`, "Zero-copy").
- libfuse master (`cb2e761c7635`).
- Ceph master in `~/ceph`.

Line references are to v4 unless stated otherwise.

**Related branches and reviews**
- `opus_review.md` reviews v1 (`ce7be6c326f8`).
- `opus_review_v4.md` reviews v4. Of its open items:
  - N1 (framing charged to the receive budget) and R (send-cancel race) affect only the send path. They matter for the future write path (§13).
  - N2 (`connect(AF_UNSPEC)`) does not apply to Ceph.

**Not covered elsewhere.** No other document covers FUSE use of opaque objects.

## Decisions

| # | Decision |
|---|---|
| D1 | Deliver data to FUSE only through FUSE io_uring zero-copy slots. No splice and no `/dev/fuse` data path. |
| D2 | One opaque store per ceph-fuse process, shared by every messenger ring and every FUSE ring. |
| D3 | ObjectCacher stays enabled. For opaque data it is a staging area until FUSE asks for the data. |
| D4 | READ payloads of at least `ms_opaque_keep_min_bytes` (runtime option, default **512 B**) are kept as objects. Smaller payloads are copied. |
| D5 | **Hand-off.** Giving an object to FUSE ends libceph's ownership. The kernel retires the object during delivery, the same way v4's `SEND_LAST` does, and the data then lives in FUSE's destination. |
| D6 | **Extend libfuse** with generic APIs and integrate into its existing io_uring infrastructure. ceph-fuse does not get its own loop. |
| D7 | **Everything goes upstream**, each change to the project that owns it: the opaque series, io_uring core, libfuse, Ceph. The object chunk size is a compile-time constant. |
| D8 | **Reads only.** Zero-copy opens are O_RDONLY, and writes keep today's copying path. Zero-copy writes are a separate project, in which FUSE itself is changed to use opaque objects (§13, writes). |

---

## 1. Overall flow

### 1.1 One sentence

The kernel keeps the bytes of every OSD read reply. libceph sees only headers and passes around opaque handles. When FUSE asks for the data, ceph-fuse tells the kernel which handle fills which part of the request, and the kernel copies the bytes straight into the page cache and drops the handle.

### 1.2 Sequence

```
 App        FUSE kernel      libfuse queue thr   ceph-fuse worker   Client/OC/Objecter   msgr worker+store     OSD
  │ read()      │                    │                  │                   │                    │               │
  │────────────▶│ READ req, folios   │                  │                   │                    │               │
  │             │ registered at slot │                  │                   │                    │               │
  │             │───────────────────▶│ dispatch ───────▶│ ll_read_zc ──────▶│ OC hit? use staged │               │
  │             │                    │                  │                   │ miss: MOSDOp (opt- │               │
  │             │                    │                  │                   │ in, admitted) ────▶│ sendmsg ─────▶│
  │             │                    │                  │                   │                    │◀── reply ─────│
  │             │                    │                  │                   │                    │ collector keeps│
  │             │                    │                  │                   │                    │ RX pages; INSPECT
  │             │                    │                  │                   │                    │ headers; RECV_OBJECT
  │             │                    │                  │                   │◀── MOSDOpReply ────│ READ range → H,crc
  │             │                    │                  │◀── bufferlist ────│ (opaque raws)      │               │
  │             │                    │◀── fill plan ────│ {OBJ H,ZERO,MEM}  │ ranges leave OC    │               │
  │             │◀── FILL(H→slot,LAST), READ_FIXED(/dev/zero), FIXED_COPY(mem) — 1 kernel copy  │               │
  │             │◀── COMMIT_AND_FETCH (after all fill CQEs)                 │                    │               │
  │◀── data ────│ page cache owns data; H retired by kernel                 │                    │               │
```

### 1.3 Step by step

**Setup (mount time)**

1. **Store.** ceph-fuse creates one opaque store. Every messenger worker creates its own ring on its own thread and imports the store into it. Each libfuse queue thread does the same, through a libfuse setup hook.
2. **FUSE queues.** libfuse starts FUSE io_uring queues in zero-copy mode, with one registered-buffer slot per ring entry.
3. **Opens.** When an application opens an eligible file read-only, ceph-fuse marks that open zero-copy. Eligible means the feature is on, the file is not encrypted (fscrypt) and does not store its data inline.

**Connecting to an OSD**

4. The msgr2 handshake runs in userspace as today. At its last step, the messenger attaches the socket to the store with `OPAQUE_COLLECT`. From then on the kernel owns receive on that socket. Transmit is unchanged: libceph still sends with `sendmsg`, which v4 still allows on an attached socket. For reads this costs nothing, because the client sends only small requests and acks, never file data. Writes do send file data; zero-copy writes are a separate project (D8).

**A read**

5. **Request.** The application reads. FUSE registers the destination pages of the READ request at the ring entry's slot. libfuse dispatches the request to a ceph-fuse worker thread.
6. **Client.** ceph-fuse runs a zero-copy Client read.
   - On an ObjectCacher hit, the data is already staged.
   - On a miss, Objecter sends the OSD read marked zero-copy, after a non-blocking admission check against the store's budget.
7. **Reply capture.** The OSD reply's pages are captured by the store. The messenger works only on headers:
   - It `INSPECT`s the headers.
   - It `RECV_OBJECT`s exactly the READ op's payload, as file-aligned chunk objects. Each completion returns a handle and a crc.
   - It verifies the segment crc from the kernel's values and checks `late_status`.
   - It `DISCARD`s the protocol bytes.

   The MOSDOpReply then flows through Objecter and ObjectCacher as a bufferlist of opaque buffers.
8. **Fill plan.** The Client returns the requested range as pieces: OBJECT (a handle range), ZERO (a hole) or MEM (dirty or small data in memory). Delivered ranges leave ObjectCacher. ceph-fuse turns the pieces into a fill plan and hands it to libfuse.
9. **Fill and commit.** On its own ring, the libfuse queue thread submits one op per piece:
   - `OPAQUE_OBJ_FILL` copies an object into the slot, with `LAST` when this is the final reference;
   - `READ_FIXED` from `/dev/zero` fills a hole;
   - `FIXED_COPY` copies a MEM piece.

   After every op has completed, the thread commits the READ. The kernel's copy into the page cache is the only copy. Consumed objects are retired by the kernel.
10. **Cleanup.** Objects that are never delivered are freed by the opaque buffer's destructor: evicted readahead, errors, teardown. Connection teardown issues `STREAM_CLOSE`.

**Fallbacks.** Each of these takes the normal copying path for that op, frame, connection or file, never an error:

| Fallback | What takes the copying path |
|---|---|
| Admission denied | that op |
| `RECV_OBJECT` failure | that frame |
| Attach failure | that connection |
| Ineligible file | that file's FUSE io_uring traffic |

---

## 2. Zero-copy, defined

File data never crosses the user/kernel boundary in ceph-fuse. Copies per byte for a buffered READ miss:

| Step | Today | This design | In-kernel CephFS |
|---|---|---|---|
| Socket → libceph | copy | none | — |
| libceph prefetch | copy (part) | none | — |
| libceph → FUSE | copy | none | — |
| Into the FUSE destination | steal or copy | **1 kernel copy** | **1 copy** |
| crc pass | userspace | kernel | kernel |

The one remaining copy cannot be avoided. FUSE's destination is aligned 4 KiB page-cache folios, or the application's buffer. The payload sits in about 1448 B pieces across shared NIC pages, which the page cache cannot adopt. The kernel CephFS client pays the same copy.

---

## 3. Components, rings and threads

```
                     one opaque store (D2): registered once, exported, imported into every ring
                     ┌──────────────────────────────────────────────────────────────────┐
 OSD ─TCP─▶ socket   │ OPAQUE_COLLECT ─▶ undecided extents (RX page refs)                │
 (TX by sendmsg)     │ INSPECT / RECV_OBJECT(+crc) / DISCARD ─▶ objects {handle}         │
                     │ OPAQUE_OBJ_FILL [LAST] ─▶ registered buffer; FREE; STREAM_CLOSE   │
                     └──────────────────────────────────────────────────────────────────┘
       ▲ messenger rings (one per AsyncMessenger worker)       ▲ FUSE queue rings (one per CPU)
```

**Ring setup**
- Every ring that imports the store is created with `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN`, which must be set explicitly together.
- It also needs `CQE32` or `CQE_MIXED`. Registration and import check this (`io_uring/opaque_register.c:71-72`).
- FUSE rings also need `SQE128`, or `SQE_MIXED` with `URING_CMD128`.

**Ownership**
- `SINGLE_ISSUER` binds a ring to the task that creates it, or to the task that enables an `R_DISABLED` ring.
- Every submit and every `io_uring_register` must come from that owner. That includes store import, eventfd and buffer tables.
- Rings are therefore created on their owner thread:
  - messenger rings in `Worker::initialize()`, because `EventCenter::init` runs on the main thread today;
  - FUSE rings in libfuse's queue threads.

**Wakeup**
- With `DEFER_TASKRUN`, completions are queued as local task work, so an epoll on the ring fd does not wake the owner.
- Each messenger ring registers an eventfd (`IORING_REGISTER_EVENTFD`) that EventCenter's epoll watches.
- On wakeup the worker calls `io_uring_enter(GETEVENTS)` and then reaps CQEs.

---

## 4. Object lifecycle and hand-off (D5)

| State | Owner | Exit |
|---|---|---|
| Undecided stream bytes | store | `RECV_OBJECT`, `DISCARD`, or `INSPECT` + `DISCARD` |
| Object in a reply being assembled | libceph (reply bufferlist) | crc and `late_status` ok → staged or delivered; otherwise FREE (§10) |
| Staged in ObjectCacher | libceph (BufferHead) | delivered, or evicted (FREE) |
| **Delivered** | **FUSE** | kernel retires it at FILL completion; libceph does nothing |

**Chunking**
- A READ payload is received as consecutive `RECV_OBJECT` chunks, aligned to file offsets.
- Each chunk is at most `CEPH_OPAQUE_OBJECT_MAX_BYTES`, a compile-time constant, default 128 KiB.
- Most FUSE READs therefore consume whole objects. A READ that straddles two chunks consumes part of each, and the neighbouring sequential READs consume the rest.

**`LAST`**
- It mirrors `SEND_LAST`: at first issue the FILL takes the handle out of the table, and the object is released when the copy completes.
- libceph sets `LAST` on a plan's final FILL from an object only when the plan holds every remaining reference to it. It checks the raw's reference count after the delivered ranges have left ObjectCacher.
- Otherwise it issues a plain FILL, and the `raw_opaque` destructor issues FREE when the last reference drops, unless the object was consumed.
- This is safe by construction; the worst case is one extra FREE.

**No caching after delivery.** FUSE's page cache owns delivered data, and ObjectCacher no longer holds a second copy of it. Today ceph-fuse opens with `keep_cache` and holds the same bytes in ObjectCacher.

---

## 5. Store and flow control

### 5.1 Budget dimensions

The store caps five things (`io_uring/opaque.c:375-384`). Each is charged to the owner's memcg, so the ceph-fuse memory limit must cover them:

| Dimension | Cap |
|---|---|
| bytes | `hard_limit` |
| extents | ≤ 65536, about one per TCP segment. At 1448 B per extent that is roughly 90 MiB of payload, so this cap binds first. |
| object slots | ≤ 16384 |
| streams | ≤ 1024. Connections beyond this stay on the normal path. |
| requests | ≤ 4096 |

When the bytes or extents run out, the store stops reading **every** socket until memory is released. That is normal flow control. The design has to make sure the memory can always be released without more network input. Otherwise, for example, half of a read spread over two OSDs could hold the budget while the other half can never arrive.

`objecter_inflight_op_bytes` does **not** bound the CephFS client: `Client` never enables the balanced budget. libceph therefore needs its own limit.

### 5.2 Liveness rules

1. **Admission** (libceph, non-blocking, at Objecter op submit).
   - Each opted-in READ reserves, from a libceph-side account:
     - its expected charge: bytes × overhead, starting at 3× and recalibrated from the actual charge returned by `RECV_OBJECT` and from `STAT`;
     - its expected extents (`len / 1448`, rounded up);
     - its object slots.
   - If the reservation fails, the op loses its zero-copy flag and goes through the copy path. Nothing ever blocks under `client_lock`.
   - The reservation is reserved once per Op, not per resend. It becomes the actual charge at `RECV_OBJECT` and is released at hand-off or FREE.
2. **Staging is bounded and evicted first.**
   - Staged opaque data counts toward `client_oc_size`.
   - Under admission pressure, a Client thread trims staged readahead first.
   - Optional automatic compaction (`SET_POLICY`) of objects staged for longer than about 100 ms packs them into dense pages of up to 64 KiB per extent, which lowers extent use.
3. **Per-stream receive reserve: dropped for v9.** v9 stops collecting from a stream while it holds undecided bytes, so TCP flow control bounds each stream. Liveness now rests on admission, staging eviction on the pressure CQE, and the store sizing rule in the work plan (§5.3).

---

## 6. Receive path (libceph messenger)

### 6.1 Attach as an explicit protocol state

**Where.** In `ProtocolV2::handle_server_ident()` the worker submits `IORING_OP_OPAQUE_COLLECT` (store selected by `opaque_store_id`) **instead of** `return ready()`.

**When.** Only if all of these hold:
- the peer is an OSD;
- the policy is lossy;
- the connection mode is crc;
- the frame revision is rev1;
- no compression is in use;
- `fuse_zero_copy` is on;
- a stream slot and reserve are available.

**Steps.**
1. Remove `EVENT_READABLE` for the fd and keep `EVENT_WRITABLE`. Do **not** run `CONTINUE(read_frame)`. The series leaves `tcp_poll` unchanged while a userspace `recv` would return `-EBUSY`, which would fault the connection.
2. When the first CQE carries the stream token, set `can_write`. From then on, only ring CQEs drive framing.
3. If attach fails, re-add `EVENT_READABLE` and continue with the normal `ready()` path.
4. If the connection faults between submit and token, cancel the collector and send `STREAM_CLOSE` for a late token.

**Preconditions.** AsyncConnection's prefetch buffer must be empty. That holds because OSDs send nothing before the first request. Attach also fails on ULPs, repair mode, urgent data or `sk_user_data` (`opaque.c:1138-1160`).

**Teardown.** `STREAM_CLOSE` is sent on every teardown path: `shutdown_socket`/`_stop`, replacement, and any terminal collector CQE. The stream holds its own reference to the socket file, so skipping this leaks the socket, a stream slot and the undecided bytes.

### 6.2 Framing

`INSPECT` returns the bytes that are available, up to 64 KiB, and never waits for a full window. Headers are parsed incrementally: inspect the next missing offset, then resume on its CQE.

For each frame:

1. **Headers.** Read the preamble, header2 (its inline crc is checked in userspace), front and middle. Segments larger than 64 KiB are read in pieces; an MOSDMap front can be up to 10 MiB.
2. **Data.** For an MOSDOpReply whose tid opted in (§6.4):
   - Compute the READ op's outdata range from the front's per-op `payload_len`.
   - If the range is at least `ms_opaque_keep_min_bytes`, `RECV_OBJECT` it in chunks (§4), with crc requested. Submit them together with the epilogue `INSPECT`.
   - All other data-segment bytes are `INSPECT`ed as ordinary bytes. That includes SplitOp's `GET_INTERNAL_VERSIONS` outdata, which SplitOp decodes.
3. **Protocol bytes.** `DISCARD` them once parsed.
4. **Acceptance.** Objects are usable only when `late_status` does not abort the frame and the segment crc matches (§6.3).

### 6.3 Integrity

- msgr2 crc mode carries crc32c(-1), with no final xor, of the data segment in `epilogue.crc_values[2]`. A client cannot disable it: `ms_crc_data` couples TX and RX, and the kernel client has no msgr2 nocrc.
- `RECV_OBJECT` returns the crc of each chunk (§9.1.2). libceph seeds each opaque raw's crc cache with it (`raw::set_crc`).
- `bufferlist::crc32c` over the segment then combines the cached chunk crcs with the crcs of the `INSPECT`ed bytes, using the existing re-seed math, and never reads opaque bytes. Each opaque ptr must cover exactly its cached range.
- **Out of scope:** secure mode, fscrypt and inline data. Client↔OSD compression is never negotiated.

### 6.4 Per-op opt-in without lock inversion

The ProtocolV2 hook runs under `AsyncConnection::lock`. Objecter calls `mark_down()` while holding its `rwlock` and the session locks. So the hook must not call into Objecter.

Instead:
- `Objecter::_send_op` records opted-in tids on the `Connection` in a set guarded by a leaf lock. Every send passes through `_send_op`: redirect, EAGAIN, kick after reset, SplitOp sub-ops.
- They are removed in `_finish_op`, `_session_op_remove` and `op_cancel`.
- The messenger looks up `header2.tid` locally.

This follows the disabled `post_rx_buffer`/`revoke_rx_buffer` precedent.

---

## 7. libceph internals

### 7.1 `buffer::raw_opaque`

**Fields.** `{store, handle, length, crc, consumed}`, with `data == nullptr`.

**Guards**
- `get_data()` is non-virtual. Add a `ceph_assert` on an opaque flag to `ptr::c_str`, `raw_c_str`, `copy_out`, `append` and `zero`, so misuse aborts with a diagnosable message.
- Opaque ptrs enter bufferlists only through non-carriage overloads: `push_back(const ptr&)`/`push_back(ptr&&)`, `append(ptr, off, len)`, `substr_of`, `claim_append`. A substr ptr has an unused tail, so it must never become the append carriage.
- Opaque raws use their own mempool.

**Destructor.** If the object was not consumed, the destructor batches a FREE to the creating worker's ring through `EventCenter::dispatch_event_external`. Any ring that imports the store may free any handle.

**Byte consumers.** These must never see opaque raws, or must first materialize them with `READ_OBJECT` (at most 64 KiB per op) after `client_lock` is dropped:
- `crc32c` on a cache miss;
- `maybe_rebuild_buffer`;
- SplitOp's and Objecter's copies into a pre-sized `outbl`;
- fscrypt.

### 7.2 Plumbing

- **Objecter op flag.** `Objecter::Op` gains a zero-copy opt-in flag, set for READ ops on eligible inodes.
- **ObjectCacher misses.** `WritebackHandler::read` gains a zero-copy flag. Today `ObjecterWriteback::read` has no inode and passes `op_flags = 0`.
- **Client.**
  - `_read()` gains `allow_opaque`; the sync path opts in only when it is set.
  - A new `Client::ll_read_zc()` returns the pieces.
  - Every other libcephfs consumer gets materialized bytes, copied at the API boundary.

### 7.3 ObjectCacher (D3)

ObjectCacher keeps all its roles: readahead (up to 16 MiB per file), write-back and coherence.

| Area | Behavior |
|---|---|
| Fill | `bh_read` replies are stored as opaque raws by reference. Split, merge, truncate, `_readx` and `C_RetryRead` already work by reference. |
| Read hit with hand-off | A new `file_read_ex(..., handoff)` mode removes the delivered ranges from the cache while `client_lock` is held. |
| Rebuild | `maybe_rebuild_buffer` skips buffers that contain opaque raws. |
| Trim | Today `trim()` is private. Add a public opaque-aware trim with its own target (store STAT bytes and extents against the staging share). It runs on a Client thread posted by the pressure event; a messenger worker must never take `client_lock`. Whole-object eviction uses a per-raw BufferHead count. |
| Dirty data | Unchanged. Written data stays in memory, so reads that hit dirty or in-flight BufferHeads return MEM pieces. |
| Coherence | Unchanged. Kernel invalidation notifications still go over `/dev/fuse`. |

---

## 8. FUSE delivery (D1)

### 8.1 Setup

- **Kernel and privilege.** `enable_uring=1` on the fuse module, and `CAP_SYS_ADMIN` for the ceph-fuse daemon.
- **Queue setup.** libfuse (§9.3) creates zero-copy queues:
  - `ADD_QUEUE` with `FUSE_URING_ZERO_COPY`;
  - `ADD_BUFPOOL`;
  - a sparse buffer table holding the bufpool and one slot per entry;
  - `REGISTER` with `ent_zero_copy_buf_index`.
- **Per-queue registrations.** In each queue thread's setup hook, ceph-fuse imports the store and registers `/dev/zero` as a fixed file.
- **Zero-copy opens.** `FOPEN_IO_URING_ZERO_COPY` is set only on **O_RDONLY** opens of eligible inodes. Only requests flagged `FUSE_URING_ENT_ZERO_COPY` carry a slot. READs on such a file without the flag (kvec direct I/O, `/dev/fuse` before the ring is ready, non-zero-copy queues) are answered normally.

### 8.2 Per-READ flow

1. The kernel registers the request folios as a DEST buffer at the slot and sets `FUSE_URING_ENT_ZERO_COPY`. libfuse dispatches the request to a worker.
2. ceph-fuse calls `Client::ll_read_zc()` and builds the fill plan: ordered `(slot offset, length, source)` entries, with `LAST` marked where §4 allows.
3. ceph-fuse calls `fuse_reply_uring_ops()` (§9.3). On its own ring the queue thread prepares and submits one SQE per plan entry, through ceph-fuse's prep callback. Entry types:
   - `OPAQUE_OBJ_FILL` for OBJECT;
   - `READ_FIXED` from the registered `/dev/zero`, with `addr` set to the slot offset, for ZERO;
   - `FIXED_COPY` for MEM.
4. When every CQE has completed in full, libfuse commits with `payload_sz` set to the sum of the lengths. Otherwise it commits `-EIO`, after retrying `-EAGAIN`.
   - A short commit means EOF and shrinks `i_size`, so fills are all-or-error.
   - FUSE does not wait for buffer release, so the commit comes strictly after every fill.
5. libfuse calls ceph-fuse's completion callback with the CQEs, on the queue thread. ceph-fuse marks consumed objects (consumed flag) and drops the plan's references.

---

## 9. Specifications by upstream project (D7)

### 9.1 Linux: opaque object series (next revision)

#### 9.1.1 `IORING_OP_OPAQUE_OBJ_FILL`

A new opcode, modelled on `IORING_OP_OPAQUE_OBJ_SEND`, with a registered buffer as the destination.

| SQE field | Value |
|---|---|
| `fd` | -1 |
| `opaque_store_id` | store registration ID on this ring |
| `addr` | object handle |
| `off` | offset in the object |
| `len` | bytes; > 0, ≤ INT_MAX, no 64 KiB cap |
| `buf_index` | destination registered buffer |
| `addr3` | destination address within that buffer. For kernel buffers (`ubuf == 0`) this is the byte offset. |
| `ioprio` | 0, or `IORING_OPAQUE_FILL_LAST` |
| other fields | 0. `IOSQE_CQE_SKIP_SUCCESS` is rejected with `LAST`. |
| CQE `res` | `len` on success, or -errno; never short |
| CQE flags | `IORING_CQE_F_OPAQUE_CONSUMED` exactly when this request took ownership |

Semantics:
- **Prep.** FILL has its own prep, which stores `buf_index` in `req->buf_index`. The handle is resolved at first issue, as v4 does for sends.
- **`LAST`.** At first issue it atomically removes the handle and takes over the table reference, exactly like `SEND_LAST`. Two LASTs cannot both claim the handle; the loser gets `-ESTALE`. A failure before ownership consumes nothing. After ownership, the object stays consumed even if the copy fails.
- **Copy.**
  - The FILL pins the current backing version and imports the destination with `io_import_reg_buf(..., ITER_DEST)`. The direction is checked; FUSE READ slots are DEST.
  - It copies page by page outside the store locks, as `READ_OBJECT` does.
  - Control issue holds the ring submit lock (`opaque.c:1590-1685`) and the buffer import takes it again. So FILL looks up the buffer node before taking that lock, or it deadlocks when issued from io-wq.
- **Request cap.** FILL counts against `max_requests` like `READ_OBJECT`. libceph and libfuse retry `-EAGAIN`.

#### 9.1.2 crc32c in `RECV_OBJECT`

> v9 puts FORWARD_ONLY in the second extra word. The crc moves to its high 32 bits, flagged by `IORING_OPAQUE_OBJECT_F_CRC32C`, and the charge-in-pages field is dropped (work plan §5.1, K2).

- **Opt-in.** A new `sqe->rw_flags` bit, `IORING_OPAQUE_RECV_F_CRC32C`.
- **Computation.** At publication the kernel computes crc32c over the object's bytes with seed `~0` and no final xor, using lib `crc32c()`; the crypto shash inverts the result. Reference vector: `"123456789"` → `0x1CF96D7C`.
- **CQE.** The low 32 bits of the second extra word, which is free today (`io_req_set_res32(req, ret, 0, op->handle, 0)`, `opaque.c:1393`), carry the crc. The high 32 bits carry the object's charge in pages, used for admission recalibration.

#### 9.1.3 Per-stream receive reserve

> Dropped for v9: the capture hold provides per-stream backpressure (work plan §1).

Size-versioned configuration fields:
- `stream_reserve_bytes` and `stream_reserve_extents`, reserved at attach. Attach fails with `-ENOBUFS` if the reserve is unavailable.
- `stream_undecided_max`.

Semantics:
- **Reserve.** Undecided capture on a stream may always use that stream's reserve, even when the shared pool is exhausted.
- **Per-stream cap.** Undecided bytes beyond `stream_undecided_max` stop capture on that stream only, as TCP backpressure.
- **Charge transfer.** When `RECV_OBJECT` takes bytes, their charge moves from the stream reserve to the shared pool. If the pool cannot absorb it, the `RECV_OBJECT` completes with `-ENOBUFS` and restores its prefix (v4 already restores).
- **Wakeup.** Releasing pool capacity wakes the suspended streams.

#### 9.1.4 Scalability with one shared store

Prep and lookup take the store-wide `tables` mutex. Change handle lookup to RCU or per-object references, so FUSE-ring FILLs and messenger-ring `RECV_OBJECT`s do not serialize.

### 9.2 Linux: io_uring core

**`IORING_OP_FIXED_COPY`** copies user memory into a registered buffer. It is generic.

| SQE field | Value |
|---|---|
| `fd` | -1 |
| `addr` | user source address |
| `len` | bytes |
| `buf_index` | destination registered buffer |
| `addr3` | destination address within that buffer (offset for kernel buffers) |
| other fields | 0 |
| CQE `res` | `len`, or -errno; never short |

Implementation: `io_import_reg_buf(..., ITER_DEST)`, then copy from a ubuf source iterator.

**Upstream rationale.** Any server filling kernel-registered destination buffers, such as FUSE zero-copy or ublk, needs to place data that already lives in user memory. Today that bounces through a pipe or memfd at 2 copies, and the libfuse zero-copy draft does exactly that.

**Zero fill needs no new ABI.** `IORING_OP_READ_FIXED` from a registered `/dev/zero` reaches `read_iter_zero` through a bvec destination.

### 9.3 libfuse (D6: generic APIs, upstream PRs)

All of this builds on master's existing io_uring code: bufpool, the single-issuer mode, and the per-queue eventfd.

1. **Queue setup hook.**
   - API: `fuse_session_set_uring_hooks(se, {queue_init, queue_exit}, userdata)`.
   - `queue_init(qid, struct io_uring *ring, …)` runs on the queue thread after the ring is created and before `REGISTER`. That is where the application imports stores and registers files.
2. **Ring mode.**
   - Option: `-o io_uring_single_issuer` creates queue rings with `SINGLE_ISSUER | DEFER_TASKRUN | CQE_MIXED | SQE128`.
   - Replies from other threads go through a per-queue hand-off queue. The queue thread wakes on an eventfd armed with a multishot poll.
   - This covers **all** replies, not only zero-copy: metadata replies and errors too.
3. **Zero-copy queues.**
   - Option: `-o io_uring_zero_copy` requires bufpool.
   - Setup: `ADD_QUEUE(FUSE_URING_ZERO_COPY)`, a sparse table with the bufpool plus per-entry slots, and `REGISTER` with `ent_zero_copy_buf_index`. The zero_copy_v7 draft must be ported onto master's bufpool path.
4. **Per-open flag.** A `fuse_file_info` bit, emitted in `fill_open`, that sets `FOPEN_IO_URING_ZERO_COPY`.
5. **Per-request info.**
   - API: `fuse_req_zero_copy(req, &buf_index, &length)`.
   - It reports `FUSE_URING_ENT_ZERO_COPY`, the slot and the requested length.
6. **Replies built from application ops.**
   - API: `int fuse_reply_uring_ops(fuse_req_t req, unsigned nr_ops, size_t payload_sz, prep_cb, complete_cb, void *arg)`.
   - On the queue thread, libfuse gets `nr_ops` SQEs and calls `prep_cb(sqe, i, buf_index, queue_ctx, arg)` for each.
   - libfuse tags `user_data`, submits, and collects the CQEs. It commits `payload_sz` if every op succeeded in full, and `-EIO` otherwise, after retrying `-EAGAIN`.
   - It then calls `complete_cb(req, cqes, nr_ops, arg)`.
   - libfuse knows nothing about opaque objects; any application can fill slots with any ops, for example a `READ_FIXED` from a backing file.
7. **Application CQEs.**
   - Tagged `user_data`, so they are never cast to `fuse_ring_ent`. Today a `commit_id` of 0 calls `abort()`.
   - Errors are handled per request, not with `fuse_session_exit`.
   - The SQ and CQ are sized for depth × the maximum ops per reply.
   - The CQ is advanced with `io_uring_cqe_nr()` under `CQE_MIXED`.
8. **Worker dispatch.**
   - Option: `-o io_uring_workers=N`.
   - The queue threads hand requests to a worker pool, so blocking handlers never stall a per-CPU queue. Replies return through item 2.
9. **`/dev/fuse` traffic and interrupts.**
   - The `/dev/fuse` reader stays for FORGET, BATCH_FORGET, INTERRUPT and NOTIFY_REPLY. Notifications are still written to `/dev/fuse`.
   - **INTERRUPT is matched to ring requests by `unique`.** Today libfuse answers ENOSYS, which disables interrupts for the connection and breaks ceph-fuse's `switch_interrupt_cb` for blocking `flock`/`F_SETLKW`.

### 9.4 Ceph

**Messenger** (`src/msg/async/`). A new opaque transport layer:
- `OpaqueStore`: a process-wide singleton handling store registration, the export descriptor, admission accounting and statistics.
- `OpaqueRing`: one per worker, created in `Worker::initialize()`, with the eventfd registered in EventCenter. It handles SQE submission and CQE dispatch.
- `OpaqueStream`: one per attached `AsyncConnection`, holding the attach state, the INSPECT/`RECV_OBJECT`/DISCARD framing state machine and `STREAM_CLOSE`.
- `ProtocolV2` changes: the attach state (§6.1), the decision at DATA (§6.2), crc and `late_status` (§6.3).
- `AsyncConnection` changes: readable-event removal, and teardown hooks.

**buffer.** `raw_opaque` and its guards (§7.1).

**osdc.**
- `Objecter`: the opt-in flag, the per-Connection tid set filled in `_send_op` (§6.4), admission at `op_submit`, SplitOp propagation.
- `WritebackHandler`: the zero-copy flag on `read`.

**Client and ObjectCacher.** `ll_read_zc`, `allow_opaque`, the hand-off read mode, skipping rebuild, the public opaque trim, the pressure event (§7.2–7.3).

**ceph-fuse** (`fuse_ll.cc`).
- Enable io_uring with the zero-copy, single-issuer and worker options.
- Queue hooks: store import and `/dev/zero` registration.
- `fi` zero-copy flag on eligible O_RDONLY opens.
- `fuse_ll_read`: if `fuse_req_zero_copy()`, run `ll_read_zc`, build the plan and call `fuse_reply_uring_ops`.
- The prep callback emits FILL, `READ_FIXED` and `FIXED_COPY` using the queue's store ID.
- The completion callback records consumed flags.
- Thread options map onto `io_uring_workers`.

**Perf counters.** `opaque_kept_bytes`, `opaque_copied_bytes` (fallback), `opaque_fills`, `opaque_consumed`, `opaque_frees`, `opaque_admission_denied`, `opaque_recv_failures`, `opaque_crc_errors`, `opaque_attached_streams`.

---

## 10. Failure handling

| Event | Action |
|---|---|
| Admission denied | The op loses its zero-copy flag and uses the copy path. Counted. |
| Attach fails (stream cap, reserve, ULP, urgent data) | That connection stays on the normal path |
| `RECV_OBJECT` `-ENOBUFS`/`-EAGAIN`/`-EMSGSIZE`/no slot (prefix restored by v4) | Read this frame's payload through the copy path (`INSPECT` pieces + `DISCARD`), and back off admissions |
| crc mismatch | FREE the objects and fault the connection, as msgr2 does today |
| `late_status` abort | Drop the frame (objects freed) and continue, as msgr2 does today |
| Stream EOF, reset or `-ECANCELED` | Fault the connection; Objecter reopens it; `STREAM_CLOSE` |
| FILL / FIXED_COPY `-EAGAIN` | Retried by libfuse |
| Any other fill error | Commit `-EIO` for that READ. Objects consumed by `LAST` stay consumed; others are released normally. |
| Connection teardown (any path) | `STREAM_CLOSE`; outstanding objects are freed by their raws |
| Store full while staging | Trim staged readahead first (Client thread); admissions fall back meanwhile |

---

## 11. Configuration

| Name | Kind | Default | Meaning |
|---|---|---|---|
| `fuse_zero_copy` | runtime, `mds-client.yaml.in`, bool, startup | false | Enable the feature: libfuse io_uring zero-copy, single-issuer and worker mode, and the opaque store. `fuse_splice_*` become no-ops. |
| `ms_opaque_keep_min_bytes` | runtime, `global.yaml.in`, size | **512** | Minimum READ payload kept as objects (D4) |
| `ms_opaque_store_limit` | runtime, size, startup | TBD by benchmark | Store `hard_limit`. Other caps use the series maxima. |
| `ms_opaque_stream_reserve` | runtime, size, startup | TBD | Per-stream reserve (§9.1.3) |
| `CEPH_OPAQUE_OBJECT_MAX_BYTES` | compile-time (CMake) | 131072 | Chunk size (§4) |

---

## 12. Test plan

**Kernel** (selftests in `tools/testing/selftests/io_uring/opaque_obj.c` and KUnit). Run under UML per the existing recipe; the series' VM helper needs QEMU, which this machine lacks.
- FILL into user-registered and kernel-registered (DEST) buffers: offsets, bounds and direction rejection.
- `LAST` ownership: a race between two LASTs, LAST against FREE and SEND_LAST, and the consumed flag on failure.
- FILL issued from io-wq, checking for no recursive lock.
- RECV_OBJECT crc: against reference vectors, against Ceph's `ceph_crc32c`, and chunked recombination.
- Per-stream reserve: one stream holding the shared pool does not stall another stream's header capture; restoring on transfer failure.
- `FIXED_COPY`: correctness, bounds, and `-EFAULT`.

**libfuse**
- `example/passthrough_hp` with zero-copy replies built from application ops: `READ_FIXED` from the backing file into the slot.
- Tests for setup hooks, single-issuer reply hand-off from worker threads, application CQE tagging, CQE_MIXED, and interrupt matching (`test_interrupt`-style).

**Ceph unit tests**
- `raw_opaque` guards as death tests.
- bufferlist crc combination with seeded raws against a full computation.
- The ProtocolV2 opaque framing state machine against a scripted peer, covering split headers, a large MOSDMap front, `late_status` abort and crc mismatch.
- Admission accounting.
- SplitOp ranges.

**Integration**, on a vstart cluster with a ceph-fuse zero-copy mount:
- Data integrity: `fsx` and `fio --verify` with sequential and random reads.
- Read-after-write through ObjectCacher, and holes and EOF.
- OSD restarts and connection resets.
- A small store limit forcing fallbacks.
- `flock` interrupt.
- Teardown: no leaked streams or handles (`STAT`).

**Benchmarks**
- Compare against today's ceph-fuse at matched readahead and thread settings.
- Measure CPU per GiB, copied bytes, CQEs per READ, store charge and extents per payload byte, fallback rate, and page-cache re-reads.
- Use the results to tune D4 and `CEPH_OPAQUE_OBJECT_MAX_BYTES`.

---

## 13. Implementation plan (for Codex)

> Superseded by `fuse_zc_workplan.md` (work packages K1–K4, L1–L6, C1–C6, E1, T1, B1). The table below is kept for history.

Each phase lands as upstream-style patches or PRs in its own project. The phases can run in parallel only where the dependencies below allow.

| Phase | Project | Work | Depends on |
|---|---|---|---|
| P1 | Linux opaque series | FILL (§9.1.1), RECV crc (§9.1.2), per-stream reserve (§9.1.3), lookup scalability (§9.1.4), selftests and KUnit | — |
| P2 | Linux io_uring core | `FIXED_COPY` (§9.2) and selftests | — |
| P3 | libfuse | §9.3 items 1–9, tests, the `passthrough_hp` example | 7.3 FUSE zero-copy (in the base); P2 for the MEM path in examples |
| P4 | Ceph messenger and buffer | `OpaqueStore`, `OpaqueRing`, `OpaqueStream`, `raw_opaque`, ProtocolV2 attach and framing, crc. These can be tested with materialized reads before FUSE exists. | P1 |
| P5 | Ceph osdc and Client | opt-in plumbing, admission, ObjectCacher staging, hand-off and trim, `ll_read_zc` | P4 |
| P6 | ceph-fuse | hooks, zero-copy opens, fill plans, configuration, integration tests | P3, P5 (P2 for MEM pieces) |
| P7 | All | benchmarks, tuning, docs | P6 |

**Writes (separate project, D8).**

This design covers reads only. Zero-copy writes need FUSE itself to use opaque objects. FUSE would hand the WRITE data to the server as an object in the shared store, with its handle and crc in the ring entry, instead of registering it at a slot. That removes the slot's ring-local scope, and FUSE decides between copying the pages and holding them until the server releases the handle.

These Ceph-side constraints hold however the object is created:
- **Transmit (in v4).** Framed `OPAQUE_OBJ_SEND` carries msgr2 framing: the preamble, header, front and middle go in the prefix, the object is the body and the epilogue is the suffix. The body goes out by `MSG_SPLICE_PAGES`, so transmit does not copy. Review items N1 and R must be fixed first.
- **Ordinary SEND, not `SEND_LAST`.** Objecter resends an op after a fault or an OSD map change until it commits, so the object must outlive the first send. It is freed once the op commits and ObjectCacher drops the data.
- **All transmit through the store queue.** v4 orders object sends per store and socket, but not against plain `sendmsg`. A connection that sends objects routes all of its transmit through the store's send queue, using generated-only frames for plain messages.
- **Sub-range crcs.** ObjectCacher splits can leave ranges whose crc arrived only for the whole object. Those ranges need a kernel crc op.
- **Copies today.** A buffered ceph-fuse write is copied four times: into the FUSE page cache, into ceph-fuse, by `bl.append()` in `Client::ll_write`, and by `sendmsg`. The kernel client copies once.
