# libfuse fact sheet for ceph-fuse zero-copy READ (wf5)

Read-only investigation. Nothing in /mnt/workspace/ceph-client was modified (only `git show` / `git grep`).

Path roots used below:

- **M** = libfuse master, `scratchpad/libfuse-src`
- **Z** = joannekoong `zero_copy_v7` draft, `scratchpad/libfuse-zc-v7`
- **K** = kernel `origin/upstream/opaque-obj-v9` (`875cf36709ca`). Kernel files were extracted with `git show` into `scratchpad/k_*.c`, so the line numbers are blob line numbers.

---

## 0. Sources and versions (verified)

| Tree | Fact |
|---|---|
| M | `.git` present (shallow, 1 commit). HEAD `cb2e761c763559f40e41090aabf008f665f27889`, 2026-10-07, "build(deps): bump github/codeql-action/upload-sarif". `meson.build:2` `version: '3.19.0-rc0'`. `ChangeLog.rst:1` "Unreleased Changes", so 3.19 is not yet released. The last release is 3.18.0 (2025-12-18). |
| Z | `.git` present (shallow). HEAD `42950a86e7df` "draft: sqe optimization improvement" (2026-04-01), on `38f78863fd22` "draft (bufpool and zero-copy)" (2025-11-24), on base `36a1c49fcb89` (2025-10-13). Z `meson.build:2` is `3.18.0-rc0`. |
| K | `43f8343858eb` "fuse: add zero-copy over io-uring" (2026-08-14) is an ancestor of v9. The series is `6330b1f61ed1` (decouple ring creation), `ebed9ea5b469` (ADD_QUEUE), `b45aaabc628b` (bufpools), `96caf2496e15` (registered bufpools) and `43f8343858eb` (ZC). After it, `1f59015e9581` makes the FUSE_OVER_IO_URING reply flag mandatory for any uring cmd. |
| ABI | K `include/uapi/linux/fuse.h` is **byte-identical** to M `include/fuse_kernel.h` (`diff` is empty). `FUSE_KERNEL_MINOR_VERSION 46` (M fuse_kernel.h:286). The 7.46 changelog (fuse_kernel.h:244-250) adds ADD_QUEUE, FUSE_HAS_IO_URING_BUFPOOL, the bufpool struct, the `offset` field, and FUSE_URING_ZERO_COPY / FUSE_URING_ENT_ZERO_COPY / FOPEN_IO_URING_ZERO_COPY. |

---

## 1. How master creates and runs io_uring queues (item 1)

### 1.1 Files and structs
- `lib/fuse_uring.c` (1595 lines) holds the queue machinery.
- `lib/fuse_uring_i.h` (92 lines) holds the internal API and the `#ifndef HAVE_URING` stubs (46-90).
- Defaults are at fuse_uring_i.h:22-24: `SESSION_DEF_URING_ENABLE (0)`, `..._Q_DEPTH (8)`, `..._BUFPOOL (0)`.
- `struct fuse_session_uring` (M lib/fuse_i.h:84-98) has `enabled` ("the wish until FUSE_INIT is negotiated, the result afterwards"), `q_depth`, `bufpool`, `pool` and `fsu_test_teardown_waiting`.
- `struct fuse_ring_ent` (fuse_uring.c:48-63):
  - `ring_queue`;
  - the **embedded** `struct fuse_req req`;
  - `req_header`, `op_payload`, `req_payload_sz`, `req_commit_id`, `last_cmd`;
  - `iov[2]`.
- `struct fuse_ring_queue` (65-112):
  - `qid`, `tid`, `eventfd`, `struct io_uring ring`, `payload_mem`/`payload_mem_sz`;
  - `bufpool_buf_index` (-1 or 0), `use_bufpool`;
  - `ring_lock`, `_Atomic cqe_processing`, `_Atomic draining`, `_Atomic ref_cnt`;
  - `ent[]`.
- `struct fuse_ring_pool` (117-164) has `single_issuer` ("mirror of se->conn.io_uring_single_issuer, fixed at ring creation"), `nr_queues`, `queue_depth`, `max_req_payload_sz`, `payload_chunk_sz`, `init_sem`, thread-start cond/mutex, and `queues`.
- `#define FUSE_URING_BUFPOOL_BUF_INDEX 0` (46), commented: "A queue's bufpool is the only buffer the queue registers with io_uring, so it always lands at index 0".

### 1.2 Ring setup: `fuse_queue_setup_io_uring()` (fuse_uring.c:472-537)
- `depth += 1; /* for the eventfd poll SQE */` (480).
- `params.flags = IORING_SETUP_SQE128` (482), `|= IORING_SETUP_SUBMIT_ALL` (487), `|= IORING_SETUP_CQSIZE; params.cq_entries = depth * 2;` (490-491).
- **SINGLE_ISSUER, DEFER_TASKRUN, TASKRUN_FLAG and COOP_TASKRUN sit inside `if (0) { ... }`** (493-505). The comment says: "These flags should help to increase performance, but actually make it a bit slower". Master never sets kernel SINGLE_ISSUER.
- `io_uring_queue_init_params(depth, ...)` (507). The kernel rounds SQ to a power of two and CQ to a power of two (K io_uring.c:2881, 2893). With the default q_depth 8 that gives SQ 16 and CQ 32.
- `io_uring_register_files(ring, files, 1)` (514). Only `/dev/fuse` (`se->fd`) is registered, at fixed index 0. `files[1]` = eventfd is **not** registered (count 1). The table is non-sparse with size 1.
- In single_issuer mode only: `io_uring_register_ring_fd(ring)` (523-534), non-fatal.

### 1.3 Threads and queues
- `nr_queues = get_nprocs_conf()` (928). There is one pthread per queue (`fuse_uring_start_ring_threads` 1504-1517), named `fuse-ring-%d` (1397).
- Each thread pins itself to CPU `qid` with `sched_setaffinity` (`fuse_uring_set_thread_core` 1247-1257, called at 1401).
- Kernel side: `nr_queues = num_possible_cpus()` (K dev_uring.c:305). A request goes to `queue = ring->queues[task_cpu(current)]` (K dev_uring.c:1770-1786).

### 1.4 Startup order relative to FUSE_INIT
1. `_do_init` (M fuse_lowlevel.c:2846) runs `se->op.init` (3020). Then `fuse_uring_start(se)` (3167) runs **before** the INIT reply.
2. Each queue thread then:
   - runs `fuse_uring_init_queue()` (1290-1353): eventfd (1297), ring (1306), page-aligned header and payload `alloc_local` (mmap + `mbind(MPOL_LOCAL)` + `MADV_POPULATE_WRITE`, 1275-1285), best-effort bufpool registration (1323-1324), entry init (1326-1347);
   - signals started (1404-1409);
   - blocks on `init_sem` (1417).
3. `fuse_uring_start` waits for all threads (1556-1563).
   - Any failure returns `-EADDRNOTAVAIL`. The INIT reply then drops FUSE_OVER_IO_URING (3169-3175) and the session continues on `/dev/fuse` (a graceful fallback).
4. The INIT reply is sent (3227). Then `fuse_uring_wake_ring_threads(se)` runs (3238-3239). With sync-init this happens after mount instead (5389-5390).
5. Each thread runs `fuse_uring_start_queue()` (1361-1386).
   - If `use_bufpool`, it sends ADD_QUEUE then ADD_BUFPOOL. On failure it logs a warning, emits `EMIT_INIT_STATUS_LINE("io_uring_bufpool=off:qid=%d:%s")` and falls back to per-entry payloads (`fuse_uring_disable_bufpool` 828-834).
   - It then prepares the REGISTER SQEs.
   - A failure here is **fatal**: `se->error = err; fuse_session_exit(se)` (1428-1435).
6. Multi-issuer flushes the REGISTER SQEs at once (1442-1443). Single-issuer flushes them in its first `submit_and_wait`.

### 1.5 REGISTER
- `fuse_uring_sqe_prepare` (208-231): `IORING_OP_URING_CMD`, `IOSQE_FIXED_FILE`, `fd = 0`, `user_data = ent`, `cmd_op`.
- `fuse_uring_sqe_set_bufpool` (239-247): if the pool is registered, `uring_cmd_flags |= IORING_URING_CMD_FIXED; buf_index = bufpool_buf_index`.
- `fuse_uring_sqe_set_req_data` (191-206) **memsets the 80-byte cmd area**, then sets qid and commit_id. The comment at 196-201 already says "The kernel rejects a REGISTER whose ent_zero_copy_buf_index is set on a queue that is not a zero-copy one".
- `fuse_uring_register_ent` (836-885) sets:
  - `iov[0]` = header (page-rounded `sizeof(fuse_uring_req_header)` = 288 bytes);
  - `iov[1]` = `NULL/0` for a bufpool, or `op_payload`/`payload_chunk_sz` per entry;
  - `sqe->addr = iov; sqe->len = 2`.
- `fuse_uring_register_queue` (887-923) prepares `depth` REGISTERs plus one POLL_ADD on the eventfd.
- A `-EAGAIN`/`-EINTR` CQE leads to `fuse_uring_resubmit` (1025-1077), which rebuilds REGISTER or COMMIT from `ent->last_cmd`. It calls `fuse_uring_sqe_set_req_data` (memset) at 1058-1059.

### 1.6 Reply path (which thread)
- `send_reply()` (M fuse_lowlevel.c:423-437): `if (req->flags.is_uring) return send_reply_uring(...)`.
- `fuse_reply_none` (510-517) calls `send_reply_uring(req, 0, NULL, 0)`.
- `fuse_reply_data` (1123-1131) calls `fuse_reply_data_uring`.
- The iov paths go through `fuse_send_msg` (349-387) to `fuse_send_msg_uring` (380-381).
- `send_reply_uring` (fuse_uring.c:362-394):
  1. `memcpy(op_payload, arg, argsize)` unless `arg == op_payload`;
  2. `ent_in_out->payload_sz = argsize`, `out->error`, `out->unique = req->unique`;
  3. `fuse_uring_commit_sqe`, then `fuse_free_req`.
- `fuse_reply_data_uring` (396-427) calls `fuse_buf_copy` into `op_payload`. `fuse_send_msg_uring` (432-470) concatenates `iov[1..]`.
- `fuse_uring_commit_sqe` (274-336):
  - `locked = !ring_pool->single_issuer || draining` (278-279);
  - takes an SQE, sets COMMIT_AND_FETCH, the bufpool flags and the commit_id;
  - submits **immediately on the calling thread** through `fuse_uring_submit(queue, 0)` (328-330), unless `draining` or `cqe_processing`. In those cases the SQE stays queued and the ring thread flushes it.
- `fuse_uring_submit` (249-272) loops on `io_uring_submit_and_wait`.
  - `-EINTR` is retried; `-EAGAIN` is retried up to 100 × 10 ms.
  - **Any other error sets `se->error` and calls `fuse_session_exit`.**
- The reply runs on whichever thread calls `fuse_reply_*`.
  - Multi-issuer: that thread runs `io_uring_enter` itself, under `ring_lock`.
  - Single-issuer: the application has promised to reply on the ring thread. There is no lock and no check (there is no `pthread_self` test anywhere in fuse_uring.c).
- `fuse_free_req` (263-300) skips the `se->list` bookkeeping for uring requests (`/* XXX: for now no support for interrupts with io-uring ... would add a lock across ring queues */` 273-276). It then calls `fuse_uring_req_done` (fuse_uring.c:630-636), which drops the queue ref.

### 1.7 CQE loop and dispatch
- `fuse_uring_thread` (1388-1502) loops:
  - if `stopped`, run `fuse_uring_drain` until no entries are held;
  - single-issuer calls `fuse_uring_submit(queue, 1)` (submit_and_wait(1)); multi-issuer calls `io_uring_wait_cqe` (1467-1473);
  - `cqe_processing = true`, run `fuse_uring_queue_handle_cqes`, then false;
  - `err < 0` leads to `fuse_session_exit`;
  - multi-issuer then flushes under the lock (1493-1497).
- `fuse_uring_queue_handle_cqes` (1138-1200):
  - It uses `io_uring_for_each_cqe` with `num_completed++` per CQE and finishes with `io_uring_cq_advance(num_completed)` (1196-1197).
  - `res > 0 && user_data == (unsigned)queue->eventfd` means a teardown wake, and the loop `break`s (1155-1159).
  - `-EAGAIN`/`-EINTR` resubmits the entry; a NULL entry is skipped (1162-1176).
  - Any other non-zero `res` sets `se->error = res` (except `-ENOTCONN`) and `break`s, which exits the session (1181-1190).
  - `res == 0` calls `fuse_uring_handle_cqe`.
- `fuse_uring_handle_cqe` (1079-1136):
  - `ent = user_data`; `commit_id == 0` calls **`abort()`** (1097-1105);
  - bufpool mode sets `op_payload = payload_mem + ent_in_out->offset` with a range check (1107-1121);
  - **`ent_in_out->flags` is never read** (FUSE_URING_ENT_ZERO_COPY is ignored);
  - it then resets req flags/u/interrupted, takes a queue ref, and calls `fuse_session_process_uring_cqe(se, req, in, &rrh->op_in, op_payload, payload_sz)` (1134-1135).
- `fuse_session_process_uring_cqe` (M fuse_lowlevel.c:4292-4354) dispatches **synchronously on the ring thread** to `fuse_ll_ops2[opcode].func`.
- There is **no worker pool** for ring requests.

### 1.8 Master's "single-issuer mode" is a libfuse-level promise
- `FUSE_CONN_FLAG_SINGLE_ISSUER` (M fuse_common.h:1189-1194) is set through `fuse_set_conn_flag` (fuse_lowlevel.c:2777-2789) into `conn->io_uring_single_issuer` (fuse_common.h:733-742): "The filesystem promises that every reply to an io-uring request is sent from the same thread that received the request (the per-queue io-uring worker), i.e. replies are never deferred to another thread."
- Effects:
  - no `ring_lock` on commit;
  - replies batched during `cqe_processing` and flushed by the next `submit_and_wait`;
  - `io_uring_register_ring_fd`.
- The kernel SINGLE_ISSUER and DEFER_TASKRUN flags are **not set**. `example/passthrough_hp.cc:214-215` sets the conn flag.
- Multi-issuer (the default): `ring_lock` serialises SQ access, and each replier submits for itself.
- **Draining** (`fuse_uring_drain` 1209-1241): repliers only queue SQEs under `ring_lock`; the ring thread submits for everyone and advances CQEs without dispatching them, until `ref_cnt == 1`.

### 1.9 Eventfd
- One per queue: `eventfd(0, EFD_CLOEXEC)` (1297).
- **Single-shot** `io_uring_prep_poll_add(sqe, queue->eventfd, POLLIN)` with `user_data = eventfd number` (910-918).
- It is written only in `fuse_session_destruct_uring` (646-666), for teardown. It is never re-armed and is not a fixed file. `IORING_REGISTER_EVENTFD` is not used anywhere.

### 1.10 Bufpool (ADD_QUEUE / ADD_BUFPOOL)
- Option `-o io_uring_bufpool` (fuse_lowlevel.c:4366; help at 4387). Env `FUSE_URING_BUFPOOL` (4807-4809). `bufpool ⇒ enabled` (4815-4816).
- INIT negotiation:
  - the cap is set if the kernel sends `FUSE_HAS_IO_URING_BUFPOOL` (2957-2958);
  - `LL_SET_DEFAULT(se->uring.bufpool, FUSE_CAP_IO_URING_BUFPOOL)` (3007), so the application's `init()` can unset it;
  - the reply carries `FUSE_HAS_IO_URING_BUFPOOL` (3184-3185). The kernel ignores this reply bit; it only checks `FUSE_OVER_IO_URING` (K inode.c:1408).
- `fuse_create_ring` (925-1023):
  - `use_bufpool = want_ext & FUSE_CAP_IO_URING_BUFPOOL` (930);
  - `chunk = ROUND_UP(max_write, page)`, at least `FUSE_MIN_READ_BUFFER` (953-955);
  - if `q_depth * chunk > UINT32_MAX`, fall back to per-entry payloads and unset the cap (958-965).
- `payload_mem` is a single mapping of depth × chunk (1318-1321) in both modes.
- `fuse_uring_register_bufpool` (788-805) calls `io_uring_register_buffers(ring, &iov, 1)` as best effort (RLIMIT_MEMLOCK). On success `bufpool_buf_index = 0`.
- ADD_QUEUE (`fuse_uring_add_queue` 752-762) sends flags 0. ADD_BUFPOOL (764-780) sends `bufpool.uaddr`/`len` plus the fixed flags. Both are synchronous through `fuse_uring_submit_setup_sqe` (700-719).
- `fuse_req_get_payload` doc (M fuse_lowlevel.h:2554-2577): ops with no data reply have no buffer, "and with an io-uring buffer pool it may belong to another request in flight".

### 1.11 Teardown
- Pool refs are at 545-578 and queue refs at 606-625.
- `fuse_session_destruct_uring` (638-691) sets `stopping`, posts `init_sem`, writes each eventfd, joins the threads and puts the queues.
- `fuse_uring_queue_destroy` (580-604) does `close(eventfd)`, **`io_uring_queue_exit`** and the munmaps. It runs on the thread that calls `fuse_uring_stop`, **not** on the queue thread.

---

## 2. What zero_copy_v7 adds, and porting to master (item 2)

### 2.1 Contents of the draft
1. **Options**: `-o io_uring_bufpool`, `-o io_uring_registered_buffers`, `-o io_uring_zero_copy` (Z fuse_lowlevel.c:3792-3794, help 3815-3817). The implication chain is zero_copy ⇒ registered_buffers ⇒ bufpool ⇒ enable (Z 4175-4185).
2. **Session fields** `use_bufpool`, `use_registered_buffers`, `use_zero_copy` (Z fuse_i.h:68-73). **Request flag** `fuse_req.flags.is_zero_copied` (Z fuse_i.h:45) is **never used**.
3. **Per-open bit** `fuse_file_info.zerocopy : 1`, with padding 23→22 (Z fuse_common.h:99). `fill_open` emits `FOPEN_IO_URING_ZERO_COPY` (Z fuse_lowlevel.c:503-504).
4. **Buffer table** (Z fuse_uring.c:371-412):
   - with zero copy: `io_uring_register_buffers_sparse(ring, nr_bufs + 1)` and then `io_uring_register_buffers_update_tag(ring, 0, &iov, NULL, 1)`, which puts the bufpool at index 0;
   - otherwise: `io_uring_register_buffers(ring, &iov, 1)`.
   - `nr_bufs = queue_depth`.
5. **Slot assignment**: `ring_ent->zero_copy_index = idx + 1; /* 0th index is registered buffers index */` (Z 920-922).
6. **Commands** (Z `fuse_uring_sqe_set_req_data` 148-170):
   - ADD_QUEUE gets `flags |= FUSE_URING_ZERO_COPY`;
   - ADD_BUFPOOL gets `uaddr`/`len`;
   - REGISTER gets `ent_zero_copy_buf_index = ent->zero_copy_index`;
   - every FUSE SQE gets `IORING_URING_CMD_FIXED` + `buf_index = 0` when buffers are registered (Z 191-194).
7. **Ordering**: ADD_QUEUE and ADD_BUFPOOL are issued in `fuse_uring_init_queue` (`fuse_queue_setup_bufpool`, Z 805-859). The draft therefore moved the INIT reply ahead of `fuse_uring_start` (Z `_do_init` hunk). Master instead issues them after INIT, in `fuse_uring_start_queue`.
8. **Public API**: `int fuse_do_zero_copy(fuse_req_t req, int fd, void *buf, off_t off, size_t len, bool read)` and `bool fuse_should_do_zero_copy(fuse_req_t req)` (Z fuse_lowlevel.h:2388-2390). The versionscript puts them in `FUSE_3.18` (Z :220-221).
9. **`fuse_uring_do_zero_copy`** (Z fuse_uring.c:1089-1130):
   - takes an SQE on the queue ring with no lock;
   - read with `buf == NULL`: READ_FIXED(fd, len, off) into the slot;
   - read with `buf != NULL`: `pipe()`, `write(pipe, buf, len)`, then READ_FIXED(pipe);
   - write: WRITE_FIXED(fd) from the slot;
   - `sqe->buf_index = ent->zero_copy_index`, `user_data = ent`, `cmd_in_flight = true`.
   - Submission is deferred to the end of CQE handling (Z 767-768, commit 42950a8).
10. **Completion**: `handle_inflight_cmd_cqe` (Z 633-672) sets `out->error = res<0 ? res : 0`.
    - A read sets `payload_sz = max(res, 0)`, so a short READ_FIXED becomes a short reply.
    - A write builds `fuse_write_out{size=res}`.
    - It then commits and frees the request.
11. **Example**: passthrough_hp's `sfs_open` sets `fi->zerocopy` when `fuse_should_do_zero_copy` (Z passthrough_hp.cc:1080-1081). `do_read` and `do_write_buf` branch on `fi->zerocopy` (Z 1168-1171, 1220-1223).

### 2.2 Draft defects that matter for the port
- **`fi->zerocopy` is never set on READ or WRITE.** libfuse rebuilds `fi` per request with `memset(&fi, 0, ...)` (Z fuse_lowlevel.c:1720; M 1828). So the example's zero-copy branch is dead code. The per-request `FUSE_URING_ENT_ZERO_COPY` is never read.
- There is no guard against a normal reply to a ZC READ (see §5.4: offset 0 aliases bufpool buffer 0).
- The `buf != NULL` path:
  - leaks the pipe fds;
  - blocks in `write()` when `len` exceeds the pipe capacity (64 KiB by default);
  - copies twice.
- There is no locking: it assumes the reply comes from the ring thread.
- Setup errors call `exit(1)` (Z 409-411, 856-859).
- There is no fallback when ADD_QUEUE(ZC) fails (`-EPERM`).
- Application CQEs are told apart only by `ent->cmd_in_flight`; there is no tag.
- `fuse_should_do_zero_copy` returns the session option, not the queue's actual state.

### 2.3 Port onto master (the draft cannot be cherry-picked)
Master rewrote this area. It has `alloc_local`, one `payload_mem`, the bufpool fallback, ADD_* after INIT, refcounts, drain, `ring_lock`, `cqe_processing` and `draining`. Port the zero-copy parts by hand:

1. `fuse_i.h:84-98`: add `bool zero_copy`. Add `SESSION_DEF_URING_ZERO_COPY` in fuse_uring_i.h. Add `LL_OPTION("io_uring_zero_copy", uring.zero_copy, 1)` after fuse_lowlevel.c:4366, a help line near 4387, and env `FUSE_URING_ZERO_COPY` near 4807. Add `zero_copy ⇒ bufpool ⇒ enabled` next to 4815.
2. `fuse_ring_queue`: add the actual per-queue `zero_copy` state. `fuse_ring_ent`: add `zc_buf_index` and a per-request `zc` flag.
3. `fuse_uring_init_queue` / `fuse_uring_register_bufpool` (788-805), for zero copy:
   - `io_uring_register_buffers_sparse(ring, 1 + depth)`, then `io_uring_register_buffers_update_tag(ring, 0, &iov, NULL, 1)`;
   - keep the RLIMIT_MEMLOCK best-effort: on failure set `bufpool_buf_index = -1` but **keep the sparse table** for the slots;
   - slot = `idx + 1`, because index 0 stays reserved (FUSE_URING_BUFPOOL_BUF_INDEX);
   - at most `IORING_MAX_REG_BUFFERS = 16384` (K rsrc.c:80); `ent_zero_copy_buf_index` is a u16.
4. `fuse_uring_add_queue` (752-762):
   - send `flags = FUSE_URING_ZERO_COPY`;
   - on `-EPERM` or `-EINVAL`, retry without the flag. The queue is not created on those errors (K dev_uring.c:1555-1559). Mark the queue non-ZC and emit a status line.
   - **Do not** use `fuse_uring_disable_bufpool` once a ZC ADD_QUEUE has succeeded. The kernel rejects per-entry REGISTER on ZC queues (K 1470-1474), and ADD_QUEUE again returns `-EEXIST` (K 382-388).
5. Set `ent_zero_copy_buf_index` after `fuse_uring_sqe_set_req_data` in **both** `fuse_uring_register_ent` (881) **and** `fuse_uring_resubmit` (REGISTER case, 1055-1060). The memset there would otherwise send index 0, which collides with the bufpool and gives `-EBUSY` on every ZC request of that entry.
6. `fuse_uring_handle_cqe` (1079-1136):
   - record `ent_in_out->flags & FUSE_URING_ENT_ZERO_COPY`;
   - for a ZC READ set `op_payload = NULL`, because no pool buffer was assigned (see §5.3).
7. Guard `send_reply_uring` (362-394), `fuse_reply_data_uring` (396-427), `fuse_send_msg_uring` (432-470) and `fuse_req_get_payload` (338-360) on ZC READs: either refuse (commit `-EIO`) or bounce into the slot.
8. Add an INIT status line, for example `FUSE_INIT: io_uring_zero_copy=on|off:<reason>`, so the test runner can assert it (see §6).

---

## 3. Per-open flag (item 3)
- `fill_open` (M fuse_lowlevel.c:584-604) is the only place `fuse_file_info` becomes FOPEN flags. It is used by `fuse_reply_open` (683-690) and `fuse_reply_create` (622-636). The high-level `lib/fuse.c` only passes `fi` through.
- `struct fuse_file_info` (M fuse_common.h:50-133): the last bit is `uint32_t kill_suidgid : 1;` (105), followed by `uint32_t padding : 22;` (108). Add `uint32_t io_uring_zero_copy : 1;` and change padding to 21. `test/test_abi.c:10` checks `sizeof(struct fuse_file_info) == 64`, which does not change.
- `FOPEN_IO_URING_ZERO_COPY (1 << 8)` is **already in M fuse_kernel.h:409**. Its doc (394-399) reads: "Honored only when the serving io-uring queue was set up for zero-copy (FUSE_URING_ZERO_COPY) and the request carries page payload. Otherwise reads/writes fall back to copying."
- Kernel: `args->zero_copy = ff->open_flags & FOPEN_IO_URING_ZERO_COPY` in `fuse_read_args_fill` (K file.c:608) and `fuse_write_args_fill` (K file.c:1157). The open flag is not validated anywhere else.
- `can_zero_copy_req` (K dev_uring.c:123-134) requires `queue->zero_copy && args->zero_copy`, opcode FUSE_READ or FUSE_WRITE, and `in_pages || out_pages`.
- **INIT carries no ZC capability.**
  - The kernel advertises `FUSE_OVER_IO_URING | FUSE_HAS_IO_URING_BUFPOOL` iff `enable_uring` (K inode.c:1483-1484).
  - ZC is detected only through `ADD_QUEUE(FUSE_URING_ZERO_COPY)`: `-EINVAL` if the flag is unknown, `-EPERM` without CAP_SYS_ADMIN.
- READs on a ZC open that are **not** zero-copied:
  - kvec direct I/O, which has no pages (K file.c:1573-1587);
  - requests served over `/dev/fuse` (before the ring is ready, or after a ring fallback);
  - non-ZC queues.
- An O_RDONLY open never joins `fi->write_files`. That needs `FMODE_WRITE` (K file.c:231-232) or a shared `VM_MAYWRITE` mmap (K 2441-2442). So it never carries WRITE or writeback.

---

## 4. `/dev/fuse` traffic, INTERRUPT and uniques (item 4)

**The `/dev/fuse` reader keeps running.** The workers of `fuse_session_loop_mt` (M fuse_loop_mt.c:129-200) and the 3.0 loop (fuse_loop.c:19-48) read until exit. The ring is stopped afterwards (fuse_loop_mt.c:421-422).

**Kernel routing in ring mode** (`fuse_io_uring_ops`, K dev_uring.c:1895-1905):
- `.send_forget = fuse_dev_queue_forget`: FORGET and BATCH_FORGET use `/dev/fuse`.
- `.send_interrupt = fuse_dev_queue_interrupt`: INTERRUPT uses `/dev/fuse`.
- `.send_req = fuse_uring_queue_fuse_req`: everything else uses the ring once `ring->ready` (K 1384-1392).
  - That includes **NOTIFY_REPLY**: `fuse_chan_send_notify_reply` → `fuse_send_one` → `send_req` (K dev.c:928-947).
  - M handles NOTIFY_REPLY in `fuse_session_process_uring_cqe` (fuse_lowlevel.c:4341-4345). The retrieve args use a zero-size arg0, so `op_payload` starts with `fuse_notify_retrieve_in` (K notify.c:302-306).
- Requests queued before the ring is ready stay on `/dev/fuse`.

**Notifications** (unique 0) are rejected on the ring (K dev_uring.c:626-629). libfuse writes them to `/dev/fuse` (`send_notify_iov`, M 3338-3352).

**INTERRUPT in master**:
- `err = ENOSYS` (4196), then `if ((se->conn.no_interrupt || se->uring.enabled) && in->opcode == FUSE_INTERRUPT) ... goto reply_err;` (4208-4214).
- Ring requests are never put on `se->list`: `fuse_free_req` skips it (273-291), and `process_uring_cqe` has no `list_add_req`/`check_interrupt`.

**What the kernel does with that ENOSYS**:
- `fuse_dev_do_write` searches **only `fud->pq`** (`struct fuse_pqueue *fpq = &fud->pq;` K dev.c:1894; `fuse_request_find(fpq, oh.unique & ~FUSE_INT_REQ_BIT)` 1938).
- Ring requests live in `queue->fpq` (K dev_uring.c:1078-1088).
- So the write fails with `-ENOENT`, which libfuse silently ignores (M 338-343). `fch->no_interrupt = 1` (K dev.c:1954-1955) is **not** reached.
- **Net**: the kernel keeps sending one INTERRUPT per interrupted ring request, and libfuse drops each one. Interrupts are disabled connection-wide only when the interrupted request was a `/dev/fuse` one.
- A ring request can never receive an interrupt reply: `fuse_uring_out_header_has_err` WARNs on `FUSE_INT_REQ_BIT` (K 648-653).

**Uniques**:
- `commit_id = req->in.h.unique` (K 857-860); libfuse sets `req->unique` in `fuse_session_in2req`.
- An INTERRUPT carries `arg.unique = original` and `ih.unique = unique | FUSE_INT_REQ_BIT` (K dev.c:1404-1415).
- The kernel queues an interrupt only for `FR_SENT` requests (K dev.c:715-719). The ring sets FR_SENT in `fuse_uring_prepare_send` (K dev_uring.c:1059), before the CQE is reaped.

**Master's interrupt matching**: `find_interrupted` (2340-2378) and `check_interrupt`/EAGAIN parking (2417-2438) work on `se->list` and depend on the kernel requeueing after an EAGAIN reply. That reply is also looked up in `fud->pq`, so it **cannot work for ring requests**.

---

## 5. Kernel ABI on v9 (item 5)

### 5.1 Layouts (compiled against M fuse_kernel.h)

`struct fuse_uring_cmd_req`, 40 bytes, inside the 80-byte cmd area:

| Field | Offset | Size |
|---|---|---|
| `flags` | 0 | u64 |
| `commit_id` | 8 | u64 |
| `qid` | 16 | u16 |
| `padding` | 18 | 6 |
| union: `bufpool.uaddr` | 24 | u64 |
| union: `bufpool.len` | 32 | u32 |
| union: `bufpool.reserved` | 36 | u32 |
| union: `ent_zero_copy_buf_index` | 24 | u16 |

`struct fuse_uring_ent_in_out`, 32 bytes:

| Field | Offset | Size |
|---|---|---|
| `flags` | 0 | u64; bit 0 = `FUSE_URING_ENT_ZERO_COPY` |
| `commit_id` | 8 | u64 |
| `payload_sz` | 16 | u32 |
| `offset` | 20 | u32 |
| `reserved` | 24 | u64 |

`struct fuse_uring_req_header`, 288 bytes: `in_out[128]` @0, `op_in[128]` @128, `ring_ent_in_out` @256.

`fuse_read_in.size` is at offset 16. Constants: `FUSE_URING_ZERO_COPY (1<<0)` is an ADD_QUEUE flag; commands are REGISTER=1, COMMIT_AND_FETCH=2, ADD_QUEUE=3, ADD_BUFPOOL=4.

### 5.2 Commands (K fs/fuse/dev_uring.c)
- **`fuse_uring_cmd`** (1645-1729):
  - needs `IO_URING_F_SQE128` (1658-1659): an SQE128 ring, or `IORING_OP_URING_CMD128` on SQE_MIXED (K uring_cmd.c:257-259);
  - returns `-EAGAIN` until `fch->initialized` (1673-1674);
  - returns `-EOPNOTSUPP` unless the INIT **reply** set FUSE_OVER_IO_URING (1682-1691, commit 1f59015e9581);
  - a REGISTER error sets `fch->io_uring = 0` (1695-1702).
- **ADD_QUEUE** (1537-1566):
  - flags must be a subset of `FUSE_URING_ADD_QUEUE_FLAGS = FUSE_URING_ZERO_COPY` (25, 1555);
  - `if (zero_copy && !capable(CAP_SYS_ADMIN)) return -EPERM;` (1558-1559). `capable()` checks the init user namespace;
  - returns `-EEXIST` if the queue already exists.
- **ADD_BUFPOOL** (1568-1639):
  - `flags` must be 0 and `bufpool.reserved` must be 0;
  - the queue must exist;
  - `buf_size = ring->max_payload_sz = max(FUSE_MIN_READ_BUFFER, max_write, max_pages*PAGE_SIZE)` (317-318), and `nr_bufs = len / buf_size` must be at least 1;
  - the pool is *registered* iff `IORING_URING_CMD_FIXED` is set, with `registered_index = sqe->buf_index` (1582, 1623-1626);
  - `payload_mode` must still be UNSET.
- **REGISTER** (1500-1535 and 1422-1494):
  - a non-zero `ent_zero_copy_buf_index` on a non-ZC queue returns `-EINVAL` (1443-1445);
  - the header iov must be at least 288 bytes;
  - on a bufpool queue the payload iov must be NULL/0, and a registered pool needs `IORING_URING_CMD_FIXED` with `buf_index == pool index` (68-76, 1457-1462);
  - a ZC queue without a bufpool returns `-EINVAL` "Can only use zero copy with bufpools" (1470-1474);
  - **the slot index is not checked** against the ring's buffer table at REGISTER time.
- **COMMIT_AND_FETCH** (1234-1327): checks the fixed index (1269), finds the request in `queue->fpq` by `commit_id` (1279), commits (1311), then fetches the next request (1324-1325).

### 5.3 How a ZC request is delivered
- `fuse_uring_args_to_ring` (847-918): if `can_zero_copy_req`, it sets `flags |= FUSE_URING_ENT_ZERO_COPY` and calls `fuse_uring_set_up_zero_copy` (799-842):
  - direction `IO_BUF_DEST` for `out_pages` (READ) or `IO_BUF_SOURCE` for `in_pages` (WRITE) (808-812);
  - one bvec per folio desc, with `folio_get` (823-828);
  - `io_buffer_register_bvec(ent->cmd, bvs, n, fuse_zero_copy_release, zc_bvs, ddir, ent->zero_copy_index, issue_flags)` (830-833). This goes into the buffer table of **the ring that submitted the entry's cmd**.
- A registration failure **fails the FUSE request with that errno; there is no copy fallback** (prepare_send → `fuse_uring_req_end`, 1051-1075). Causes: `index >= table size` gives `-EINVAL`, an occupied slot gives `-EBUSY` (K rsrc.c:1030-1035), plus `-ENOMEM`.
- **ZC READ**:
  - the only in-arg is `fuse_read_in` (op_in), so `payload_sz = 0`;
  - no pool buffer is selected (`fuse_uring_req_has_copyable_payload`, 952-971), so `ent_in_out.offset` stays 0 (912-914), which **looks the same as pool buffer 0**;
  - the requested length is `fuse_read_in.size`, which equals the registered length Σ desc.length (K file.c:598-607).
- **ZC WRITE**: `payload_sz` includes the page bytes (899-910), and a pool buffer is assigned for `fuse_write_out`.
- **Registered buffer**:
  - `imu->ubuf = 0` (rsrc.c:1047) and `len = Σ bv_len`;
  - a fixed op's `addr` is the **byte offset**, and `validate_fixed_range` (1167-1179) checks `off + len ≤ imu->len` and `len ≤ MAX_RW_COUNT`;
  - the direction is enforced: `if (!(imu->dir & (1 << ddir))) return -EFAULT;` (1205). A READ slot is write-only for the server.

### 5.4 ZC READ reply contract
1. The server writes `out.error`, `out.unique` (= `req->unique`) and **`ring_ent_in_out.payload_sz` = bytes delivered**, then submits COMMIT_AND_FETCH (with the bufpool fixed flags if the pool is registered).
2. The kernel runs `fuse_uring_commit` (1153-1173), checks the out header (620-658), calls `fuse_uring_copy_from_ring` (763-786), then `fuse_copy_out_args(cs, args, payload_sz)` (K dev.c:1854-1880):
   - `payload_sz > read size` returns **`-EINVAL`, and the READ fails**;
   - `payload_sz < size` is **allowed** (`out_argvar = true`, K file.c:605);
   - with `skip_folio_copy` nothing is copied; each folio is only zeroed outside `[desc.offset, desc.offset+count)` (K dev.c:1262-1278), then `flush_dcache_folio`.
3. **A short READ means EOF.** The buffered read paths call `fuse_short_read`, then `fuse_read_update_size`, which shrinks `i_size` when `!writeback_cache` (K file.c:829-842, 878-879, 1041-1042).
4. **The folios must be filled before COMMIT.** `fuse_request_end` runs immediately: readahead marks the folios uptodate and unlocks them, and direct I/O unpins the user pages.
5. **At commit**: `fuse_uring_req_end` → `zero_copy_unregister` → `io_buffer_unregister` (136-149, 172; rsrc.c:1134-1165) removes the node from the table.
   - The release callback `fuse_zero_copy_release` (folio_put + kvfree, 788-797) runs when the **last node ref** drops (`io_buffer_unmap`, rsrc.c:210-226). In-flight ops hold node refs (`io_find_buf_node`, rsrc.c:1244-1265).
   - **`fuse_request_end` does not wait for the release.**
6. An error commit still unregisters. A wrong `unique` gives the reader `-ENOENT`.
7. **On abort** with requests in userspace, `fuse_uring_entry_teardown` (411-443) ends the request without unregistering. The slot stays occupied until the ring is destroyed (stated in the 43f8343858eb commit message).

### 5.5 io_uring constraints for a FUSE ring that imports the opaque store
- Store register/import requires `DEFER_TASKRUN` and `CQE32 | CQE_MIXED` (K io_uring/opaque_register.c:71-73).
- `DEFER_TASKRUN` requires `SINGLE_ISSUER` (io_uring.c:2839-2841).
- The owner is the creating task unless `R_DISABLED` (io_uring.c:3067-3069). A non-owner `io_uring_register` gets `-EEXIST` (register.c:765-766).
- CQE32 with CQE_MIXED, and SQE128 with SQE_MIXED, are mutually exclusive (io_uring.c:2847-2856).
- **CQE_MIXED**:
  - 32-byte CQEs carry `IORING_CQE_F_32` and take 2 slots;
  - the kernel posts `IORING_CQE_F_SKIP` padding CQEs with `user_data = 0`, `res = 0` (io_uring.c:706-715);
  - liburing's `io_uring_for_each_cqe` yields SKIP CQEs (liburing.h:455-476); only peek/wait skip them (1824-1837);
  - FUSE uring_cmd CQEs are 16 bytes (`io_uring_cmd_done`, cmd.h:205-209).
- **`READ_FIXED` from `/dev/zero` can complete short.** `read_iter_zero` stops on `signal_pending` or on `IOCB_NOWAIT && need_resched` (K drivers/char/mem.c:450-472). io_uring re-issues short reads only for regular files and block devices (`need_complete_io`, rw.c:842-846).
- CQ overflow goes to an overflow list. A CQE is dropped only when allocation fails, and `io_uring_enter` then returns `-EBADR` (io_uring.c:636-655, 1213-1224). Master's `fuse_uring_submit` treats that as fatal.

---

## 6. libfuse conventions (item 6)

**Headers**
- io_uring helpers live in `include/fuse_lowlevel.h` (2549-2577).
- `include/fuse_common.h` has the caps (`FUSE_CAP_IO_URING_BUFPOOL (1ULL << 34)` is the highest, 541-552), the conn flags (1189-1206), `fuse_file_info` (50-133) and `fuse_conn_info.io_uring_single_issuer`/`padding : 30` (733-745).
- New functions are declared unconditionally (for example `fuse_req_get_payload`, `fuse_set_conn_flag`). Only *changed* semantics use `#if FUSE_USE_VERSION >= FUSE_MAKE_VERSION(3, 19)` (fuse_lowlevel.h:2336-2342).
- APIs that take a struct also take an `op_size` (`fuse_session_new` 2233; `fuse_session_custom_io_317` 2240).
- Setters say "must be called before fuse_session_mount()" (2525).

**Builds without io_uring**
- `HAVE_URING` is private (meson.build:258-265; lib/meson.build:32-35).
- New functions need stubs: the `fuse_uring_i.h` pattern (46-90), or `#ifndef HAVE_URING` in fuse_lowlevel.c (3776-3787) returning `-ENOTSUP`.
- liburing is a **private** dependency and is not in the pkg-config `Requires` (lib/meson.build:72-79). Public headers must not include `liburing.h`; forward-declare instead.

**Symbol versioning**
- `lib/fuse_versionscript`: `FUSE_3.19 { ... } FUSE_3.18;` (225-265) is the current, unreleased node. Add new symbols there, or in a new `FUSE_3.20` node if 3.19 ships first.
- `FUSE_SYMVER` is used for versioned variants.

**Session options**
- The `fuse_ll_opts` LL_OPTION table (fuse_lowlevel.c:4359-4368) and `fuse_lowlevel_help` (4377-4389).
- Test env overrides in `fuse_session_new_versioned` (4800-4816).
- Status lines: `EMIT_INIT_STATUS_LINE` (fuse_i.h:32-38) and `report_init_test_status` (fuse_lowlevel.c:2807-2841).

**Meson**
- `option('enable-io-uring', ...)` (meson_options.txt:28-29).
- The liburing probe checks only `IORING_SETUP_SQE128` and `IORING_URING_CMD_FIXED` (meson.build:237-265). Add probes for DEFER_TASKRUN, CQE_MIXED, `io_uring_cqe_nr`, `register_buffers_sparse` and `poll_multishot`. The local liburing is 2.14 and has them all.

**Tests**
- `test/run-tests.py` discovers `test/cases/**/*.sh` (see `test/cases/README.md`).
- Transport flags `--io-uring`, `--io-uring-queue-depth`, `--io-uring-bufpool` set env `FUSE_URING_ENABLE`/`FUSE_URING_QUEUE_DEPTH`/`FUSE_URING_BUFPOOL` (run-tests.py:896-914, 1645-1655). The runner asserts the `FUSE_INIT: io_uring=on` and bufpool lines (1159-1198).
- meson registers `fuse-tests` and `fuse-tests-io-uring` (test/meson.build:94-111).
- C tests are built in test/meson.build:1-66 and wrapped by `test/cases/ctests/*.sh`. For example, `deferred-reply-teardown.sh` wraps `test_deferred_reply_teardown.c`, which touches `se->uring` internals.
- Preflight checks are in `test/lib/checks.py` (bufpool at 90-100). Root-only tests use `_require_root`.
- **No interrupt-delivery test exists.** `FUSE_CONN_FLAG_NO_INTERRUPT` appears only in hello_ll.c:68-69 and passthrough_hp.cc:268.

**Example `example/passthrough_hp.cc`**
- `sfs_init` sets `FUSE_CONN_FLAG_SINGLE_ISSUER` (214-215) and `FUSE_CONN_FLAG_NO_INTERRUPT` (268).
- `do_read` writes through `fuse_req_get_payload` (1425-1458). On a ZC READ that would write into pool buffer 0.
- Options are parsed with cxxopts (1760-1790); `-o` options are passed to libfuse.

**Docs**: `doc/README.fuse-io-uring`, `doc/ChangeLog-API.rst`, and "Unreleased Changes" in `ChangeLog.rst`.

---

## 7. fuse_zc.md §8 / §9.3: wrong or underspecified

1. **§9.3 preamble** ("builds on ... the single-issuer mode, and the per-queue eventfd") is wrong.
   - Master's single-issuer mode is a promise to reply on the ring thread. It never sets kernel `SINGLE_ISSUER` or `DEFER_TASKRUN` (fuse_uring.c:493-505).
   - The eventfd is a single-shot POLL_ADD used only for teardown (910-918, 1155-1159).
   - Both need rework, not reuse.
2. **Item 2's name `-o io_uring_single_issuer`** collides with `FUSE_CONN_FLAG_SINGLE_ISSUER`, which has the opposite meaning (no replies from other threads).
   - With kernel SINGLE_ISSUER, master's multi-issuer submit from a replier gets `-EEXIST`, which leads to `fuse_session_exit` (249-272).
   - Master's `draining` path already shows the alternative to a separate hand-off queue: repliers fill SQEs under `ring_lock` and the owner submits.
   - Keep `SUBMIT_ALL` and `CQSIZE`.
3. **CQE_MIXED needs more than `io_uring_cqe_nr()`.**
   - Two places count CQEs: `handle_cqes` (1148-1197) and `drain` (1226-1229).
   - `IORING_CQE_F_SKIP` CQEs (user_data 0) must also be skipped.
   - `CQE32` also satisfies the store import and needs no change to the CQ accounting, at the cost of twice the CQ memory.
4. **Item 1's hook timing is ambiguous.** The ring is created before the INIT reply; REGISTER happens after it.
   - A pre-INIT hook can fail gracefully (fall back to `/dev/fuse`).
   - A post-INIT failure is fatal (1428-1435).
   - `queue_exit` must run on the queue thread. Master tears rings down from the joining thread (580-604), where `io_uring_register` returns `-EEXIST` under SINGLE_ISSUER.
   - Passing `struct io_uring *` couples libfuse's and ceph-fuse's liburing ABI.
5. **§8.1 "/dev/zero as a fixed file" does not fit master.** The file table has exactly one non-sparse entry (514). libfuse would have to reserve file slots. Alternatively ceph-fuse can use a plain fd, since READ_FIXED needs a fixed *buffer*, not a fixed file.
6. **Item 3 / §8.1 queue setup leaves out several requirements.**
   - The bufpool stays at index 0 and the slots are 1..depth.
   - A bad or occupied slot fails the READ with `-EINVAL`/`-EBUSY`; there is no fallback.
   - The sparse table must exist even when the pool's own registration fails.
   - `fuse_uring_resubmit` must set the slot again.
   - Fallback: if ADD_QUEUE(ZC) returns `-EPERM` or `-EINVAL`, retry without the flag. ADD_BUFPOOL failure after a ZC ADD_QUEUE is fatal for that queue.
   - The bufpool preconditions also apply: the kernel must offer FUSE_HAS_IO_URING_BUFPOOL, `init()` must not unset FUSE_CAP_IO_URING_BUFPOOL, and depth × chunk must be below 4 GiB (958-965).
7. **Item 4 / §8.1 per-open flag.**
   - There is no ZC capability at INIT.
   - The flag is already in master's `fuse_kernel.h`, so no header sync is needed.
   - **"Every READ on such a file must be filled through the slot" is wrong.** Only requests flagged `FUSE_URING_ENT_ZERO_COPY` have a slot. Other READs on the same open (kvec DIO, the `/dev/fuse` path, non-ZC queues) need the normal reply.
   - **"The flag also covers WRITE and writeback" is moot:** an O_RDONLY file never carries WRITE or writeback.
8. **Item 5.**
   - The length to report is `fuse_read_in.size`; `payload_sz` is 0 for READ.
   - libfuse must also clear `op_payload` for ZC READs and refuse or bounce `fuse_reply_buf`/`_data`/`_iov` and `fuse_req_get_payload`. Otherwise the data lands in another request's pool buffer, and the READ "succeeds" with whatever the folio held before (skip_folio_copy). Master's own doc warns about this aliasing.
9. **Item 6 / §8.2 step 4, `fuse_reply_uring_ops`.**
   - (a) "Every op in full, else -EIO" breaks two cases. A short READ_FIXED from `/dev/zero` is possible, so the remainder must be resubmitted. A short READ_FIXED from a backing file at EOF is legitimate, so the application (or the sum of the `res` values) must choose the size committed.
   - (b) `payload_sz` must not exceed `fuse_read_in.size`, or the reader gets `-EINVAL`.
   - (c) `complete_cb(req, ...)` after the commit receives a request that is already freed or reusable. `send_reply_uring` frees right after queueing the commit. The ordering must be defined.
   - (d) The SQ is `q_depth+1` today (480, 507), and under SINGLE_ISSUER a replier cannot flush it. SQ sizing and backpressure must be defined.
   - (e) `fuse_uring_drain` must count application ops still in flight.
   - (f) Define which thread runs `prep_cb`.
10. **Item 7.** In master an application CQE is mishandled three ways:
    - `-EAGAIN`/`-EINTR` triggers a FUSE command resubmit on a garbage entry (1162-1176);
    - `res > 0` is fatal (1181-1190);
    - `res == 0` is cast to an entry.
    - The tag must avoid NULL and the raw eventfd number.
11. **Item 8.**
    - Master has no worker pool for ring requests, and the `/dev/fuse` `loop_mt` workers cannot be reused.
    - The queue threads are pinned to one CPU each.
    - Under DEFER_TASKRUN, FUSE delivery only advances while the owner is inside `io_uring_enter`, so the queue thread must never block anywhere else.
12. **Item 9.**
    - (a) NOTIFY_REPLY uses the ring, not `/dev/fuse`, and master already handles it there.
    - (b) **ENOSYS does not disable interrupts for ring requests.** The reply cannot be matched (`fud->pq` only), returns `-ENOENT`, and `no_interrupt` stays 0. The kernel keeps sending INTERRUPTs and libfuse drops them. Interrupts are disabled connection-wide only after a `/dev/fuse` request is interrupted.
    - (c) Matching must happen entirely in userspace: a cross-queue unique→req table, locked against the `/dev/fuse` reader. Parked interrupts need a check at dispatch and an expiry rule, because EAGAIN requeue does not work for ring requests.
    - (d) `test_interrupt`-style refers to a test that does not exist; a new one is needed.
13. **§8.1 CAP_SYS_ADMIN** is `capable()` in the init user namespace. A containerised ceph-fuse without it gets `-EPERM`.
14. **§8.2 FUSE does not wait for buffer release:** confirmed. Add that on abort the slot stays registered until the ring is destroyed.
15. **MEM pieces** need FIXED_COPY (P2). Without it they bounce through a pipe or memfd (2 copies). The draft's pipe bounce leaks fds and blocks above the pipe capacity.

## 8. UNVERIFIED
- The ENOSYS→`-ENOENT` behaviour for ring-request INTERRUPTs was established by reading the code only; it has not been run.
- `io_uring_queue_exit` from a non-owner thread with a registered ring fd under SINGLE_ISSUER (liburing source is not available locally).
- Minimum liburing versions for `io_uring_cqe_nr`, `poll_multishot` and `register_buffers_sparse`. Only 2.14 was checked.
- Whether 3.19 is released before these PRs land, which decides the version node.
- Master's "SINGLE_ISSUER is slower" comment; not re-measured.
- Whether `get_nprocs_conf()` equals `num_possible_cpus()` on the target hosts. If it is smaller, the ring never becomes ready.
- The impact of `_do_notify_reply` never committing an unmatched ring NOTIFY_REPLY (M 3325-3326).
