# Kernel fact sheet: OPAQUE_OBJ v9 for the ceph-fuse zero-copy READ plan

Baseline: `origin/upstream/opaque-obj-v9` = 875cf36709ca (5 patches on af32da41b032, 7.3-rc7 merge).
All `file:line` references are to that tree. Code was read with `git show`, and nothing in
/mnt/workspace/ceph-client was modified. "rst" means
Documentation/userspace-api/io_uring-opaque.rst (the v9 userspace contract).

Patches:
- 1/5 fc0538f7d01a net: optional exclusive TCP receive claim (SOCK_RX_OWNER)
- 2/5 fb4031f775b2 io_uring: opaque TCP stores, framed sends, lowmem forwarding
- 3/5 e7b8e47b15ce KUnit (io_uring/opaque_test.c)
- 4/5 bcf1a650e436 selftests (tools/testing/selftests/io_uring/opaque_obj.c, run_opaque_vm.sh)
- 5/5 875cf36709ca docs

Main files: io_uring/opaque.c (2309 lines), opaque_compact.c (363), opaque_register.c (152),
opaque_internal.h (217), opaque.h (143), include/uapi/linux/io_uring/opaque.h (131).

---------------------------------------------------------------------------------------------

## 1. Control-op architecture (IORING_OP_OPAQUE_OBJ)

### Registration in opdef
- io_uring/opdef.c:609-619
  ```c
  [IORING_OP_OPAQUE_OBJ] = { .audit_skip = 1, .ioprio = 1,
      .async_size = sizeof(struct io_opaque_req), .prep = io_opaque_prep, .issue = io_opaque_issue,
  ```
  It has no `.needs_file`, so `fd` must be -1. Cold def: opdef.c:896-899 (`.name = "OPAQUE_OBJ", .cleanup = io_opaque_cleanup`).
- `.ioprio = 1` is required because io_init_req rejects a nonzero ioprio otherwise:
  io_uring.c:1805 `if (!def->ioprio && sqe->ioprio) return io_init_fail_req(req, -EINVAL);`
- UAPI opcodes: include/uapi/linux/io_uring.h:322-324 (`IORING_OP_OPAQUE_OBJ, IORING_OP_OPAQUE_OBJ_SEND, IORING_OP_OPAQUE_COLLECT`).
  SQE field `__u32 opaque_store_id` is at io_uring.h:94 (union with splice_fd_in/file_index).

### ioprio dispatch
- Sub-ops: `enum io_uring_opaque_op` at include/uapi/linux/io_uring/opaque.h:41-56:
  INSPECT, RECV_OBJECT, DISCARD, READ_OBJECT, FREE, STAT, OBJECT_STAT, STREAM_STAT, STREAM_CLOSE, COMPACT, SET_POLICY.
- opaque.c:1585 `u16 cmd = READ_ONCE(sqe->ioprio);`
- opaque.c:1590-1594 (prep validation):
  ```c
  if (READ_ONCE(sqe->fd) != -1 || READ_ONCE(sqe->rw_flags) ||
      READ_ONCE(sqe->buf_index) || READ_ONCE(sqe->__pad2[0]) ||
      cmd > IORING_OPAQUE_SET_POLICY ||
      (req->flags & (REQ_F_CQE_SKIP | REQ_F_FIXED_FILE)))
          return -EINVAL;
  ```
  So nonzero `rw_flags` and nonzero `buf_index` are rejected for every sub-op ("buf_index rejection"), and CQE skip
  and fixed files are rejected too. IOSQE_ASYNC (REQ_F_FORCE_ASYNC) is **not** rejected here, unlike COLLECT (opaque.c:1235).
- The store is looked up at prep only in the ring-local xarray: opaque.c:1582 / 1568-1571 `xa_load(&req->ctx->opaque_stores, id)`.
  No store lock is taken at prep.
- Field mapping (opaque.c:1605-1608): `handle = sqe->addr`, `offset = sqe->off`, `length = sqe->len`, `addr = sqe->addr3`.

### Request cap and accounting
- opaque.c:1595-1597:
  ```c
  control = cmd == IORING_OPAQUE_FREE || cmd == IORING_OPAQUE_STAT ||
            cmd == IORING_OPAQUE_STREAM_CLOSE || cmd == IORING_OPAQUE_SET_POLICY;
  ```
- opaque.c:646-649 (io_opaque_req_alloc, called at prep):
  ```c
  if (!control && atomic_inc_return(&store->requests) > store->config.max_requests) {
          atomic_dec(&store->requests);
          return ERR_PTR(-EAGAIN);
  ```
  Counted: INSPECT, RECV_OBJECT, DISCARD, **READ_OBJECT**, OBJECT_STAT, STREAM_STAT, COMPACT, every COLLECT
  (opaque.c:1237; one slot for each attached stream for its lifetime), and every SEND (opaque.c:2112). The counter
  `atomic_t requests` (opaque_internal.h:140) is **store-wide**, so all importing rings share it. It is released at request
  cleanup (opaque.c:1985-1986), which runs at request free, just after the CQE is posted.
  Config bound: `max_requests` from 1 to 4096 (opaque.c:488).
- How the -EAGAIN surfaces: it is a **prep** failure, so io_submit_fail_init → io_queue_sqe_fallback posts a CQE with res = -EAGAIN.
  Without IORING_SETUP_SUBMIT_ALL, the submit loop **stops at that SQE** (io_uring.c:2054-2060: `if (unlikely(io_submit_sqe(...)) && !(ctx->flags & IORING_SETUP_SUBMIT_ALL)) { left--; break; }`).
  Later SQEs stay in the SQ, and io_uring_enter returns early (io_uring.c:2648-2651).
- Other prep checks: length 1..INT_MAX for INSPECT/RECV_OBJECT/DISCARD/READ_OBJECT (1611-1613);
  INSPECT/READ_OBJECT `length > IO_OPAQUE_READ_MAX || !addr` → -EINVAL (1614-1616), with `IO_OPAQUE_READ_MAX SZ_64K`
  at opaque_internal.h:15 (**the 64 KiB cap**); RECV_OBJECT/DISCARD must have addr3 == 0 (1617-1618); RECV_OBJECT length must not exceed
  `max_object_size` or `io_opaque_rx_limit()`, else -EMSGSIZE (1619-1622); offset+len overflow → -EOVERFLOW (1609-1610).
- The store being dead gives -ESHUTDOWN at prep (opaque.c:644-645) and at issue (1763-1765).

### Issue: locks
- io_opaque_issue, opaque.c:1756-1861:
  ```c
  io_ring_submit_lock(req->ctx, issue_flags);     /* 1762 */
  ...
  io_ring_submit_unlock(req->ctx, issue_flags);   /* 1859 */
  ```
  The whole control issue, including READ_OBJECT's copy_to_user, runs with **ctx->uring_lock held**. Inline submission
  already holds it (io_uring.c:2647 `mutex_lock(&ctx->uring_lock); ret = io_submit_sqes(...)`). In io-wq, issue_flags =
  `IO_URING_F_UNLOCKED | IO_URING_F_IOWQ` (io_uring.c:1479), and io_ring_submit_lock does `mutex_lock` (io_uring.h:400-411).
  (v4 cited 1590-1685; in v9 this is 1762-1859.)
- Store and stream locks are taken only briefly. They are never held across copies or I/O, as the comment at
  opaque_internal.h:107 says: "never held during copies or I/O".
  - io_opaque_resolve (1639-1689) takes `store->tables` (1648-1674). Stream ops look up the token and take a stream ref
    plus the socket file (1649-1660). READ_OBJECT/OBJECT_STAT pin the version (1661-1672). The mutex is then dropped.
  - Range ops (INSPECT/RECV_OBJECT/DISCARD) take `stream->lock` in io_opaque_range_start (1697 `guard(mutex)(&stream->lock)`).
  - FREE: `tables` for detach, then data_put outside the lock (1780-1788).
  - STAT takes `tables` to read nr_objects/nr_streams (1749-1752). This is an uncounted op that still takes the store mutex.
- Task-work completions (io_opaque_ready, 1494-1566) run with uring_lock held: `io_tw_lock()` is just
  `lockdep_assert_held(&ctx->uring_lock)` (io_uring/tw.h:101-104).

### READ_OBJECT
- Version pinning happens at first issue (not at prep), opaque.c:1661-1672:
  ```c
  slot = io_opaque_lookup(store, op->handle);
  if (!slot || !slot->data) ret = -ESTALE;
  else { op->data = slot->data; refcount_inc(&op->data->refs);
         if (... op->offset > op->data->length || op->length > op->data->length - op->offset) ret = -ERANGE; }
  ```
- Copy: opaque.c:1776-1778 `ret = io_opaque_copy(op->data, op->offset, op->length, op->addr);`
- Helper io_opaque_copy, opaque.c:1457-1492. It walks `data->extents` (not the bvec), and for each page does
  `kmap_local_page(extent->backing->page + (pos >> PAGE_SHIFT))` followed by `copy_to_user(...)`. A fault gives -EFAULT and a gap gives -ENODATA.
  Compound backing works because extent offsets may exceed PAGE_SIZE.
- The pin is dropped at cleanup: io_opaque_cleanup → `io_opaque_data_put(store, op->data)` (1979).
- The copy is synchronous; READ_OBJECT never waits.

### -EAGAIN and io-wq behaviour
- io_opaque_issue never returns -EAGAIN. Every negative result becomes the CQE (`io_req_set_res(req, ret, 0); ret = IOU_COMPLETE;`, 1853-1858).
  The only -EAGAIN is the prep-time `max_requests` one.
- A control op reaches io-wq only through IOSQE_ASYNC or drain (REQ_F_FORCE_ASYNC: drain at io_uring.c:1785-1796). The op has no
  pollin/pollout, so io-wq issues it once in blocking mode (io_uring.c:1524-1556).
- COLLECT: POLL_FIRST returns -EAGAIN to arm poll (1358-1361). Attach from io-wq is requeued to task context (1363-1367),
  because io_req_post_cqe32 asserts `!io_wq_current_is_worker()` (io_uring.c:902). A budget stall returns IOU_RETRY (1427-1439).
- SEND: with a nonblocking -EAGAIN it returns -EAGAIN to the core, which arms pollout (2293-2295). It **drops uring_lock around
  sock_sendmsg** (2279-2282: "io-wq may block in TCP; other requests must retain access to the ring").

---------------------------------------------------------------------------------------------

## 2. SEND_LAST: the model for FILL with LAST

- Registration: opdef.c:620-633
  ```c
  [IORING_OP_OPAQUE_OBJ_SEND] = { .audit_skip = 1, .ioprio = 1, .needs_file = 1,
      .unbound_nonreg_file = 1, .pollout = 1, .async_size = sizeof(struct io_opaque_req),
      .prep = io_opaque_send_prep, .issue = io_opaque_send,
  ```
  Cold def: opdef.c:900-905 `.cleanup = io_opaque_cleanup, .fail = io_opaque_fail`.
- UAPI flag: opaque.h:59 `#define IORING_OPAQUE_SEND_LAST (1U << 0)`, carried in `sqe->ioprio`.
- Prep, opaque.c:2097-2129. Prep does not resolve the handle. opaque.c:2107-2108 rejects LAST with CQE_SKIP:
  `((send_flags & IORING_OPAQUE_SEND_LAST) && (req->flags & REQ_F_CQE_SKIP))` → -EINVAL. `buf_index` must be 0 (2109).
- Order at first issue (io_opaque_send, 2226-2309):
  1. socket and type checks (2237-2240)
  2. store dead (2241-2244)
  3. io_opaque_frame_import (2013-2070), which snapshots and charges the framing **before** ownership can be taken
  4. io_opaque_tx_enter (2131-2179), which takes `guard(mutex)(&store->tables)` (2137), finds or allocates the per-socket
     TX queue, then calls io_opaque_send_acquire (2159)
- The atomic remove-and-transfer is io_opaque_send_acquire, opaque.c:2072-2095:
  ```c
  if (store->dead) return -ESHUTDOWN;
  if (!slot || !slot->data) return -ESTALE;                 /* the loser */
  if (op->offset > slot->data->length || op->length > slot->data->length - op->offset)
          return -ERANGE;
  if (op->send_flags & IORING_OPAQUE_SEND_LAST) {
          op->data = io_opaque_detach(store, slot);  op->consumed = true;
  } else { op->data = slot->data; refcount_inc(&op->data->refs); }
  ```
  It runs under `store->tables`, so two LASTs serialize: the second sees `slot->data == NULL` and gets **-ESTALE**
  without the flag. The KUnit check is opaque_test.c:600-645 (`io_opaque_send_acquire(&other) == -ESTALE`); the cross-ring
  selftest is opaque_obj.c:2050-2102 ("prepared SEND_LAST requests have one winner across rings").
- io_opaque_detach, opaque.c:413-429 ("Drop handle visibility, transferring the table reference to the caller"):
  `data = slot->data; slot->data = NULL; store->nr_objects--; io_opaque_release_slot(...); slot->compacting = false; list_del_init(&slot->candidate); return data;`
  An in-flight compaction then fails to publish (opaque_compact.c:155 `slot->data == op->data` check).
- CONSUMED reporting:
  - UAPI: include/uapi/linux/io_uring.h:550 `#define IORING_CQE_F_OPAQUE_CONSUMED (1U << 6)`, documented at 538-539 as
    "OPAQUE_OBJ_SEND took ownership of the entire object handle. Also valid on short or failed completions." This is a **generic** CQE flag bit.
  - Normal completion: opaque.c:2304 `io_req_set_res(req, ret, op->consumed ? IORING_CQE_F_OPAQUE_CONSUMED : 0);`
    and the task-work completion opaque.c:1564.
  - Generic failure path: io_req_defer_failed (io_uring.c:957-969) overwrites `cqe.flags`
    (`io_req_set_res(req, res, io_put_kbuf(...))`) and then calls `def->fail`. io_opaque_fail (opaque.c:1992-2002) restores
    `IORING_CQE_F_OPAQUE_CONSUMED` (and the byte progress). **A FILL that can fail through the generic path after taking ownership
    needs the same .fail hook, or it must always return IOU_COMPLETE with its own io_req_set_res.**
- After LAST, the object is released when the request is cleaned up: io_opaque_cleanup → io_opaque_data_put (1979).

---------------------------------------------------------------------------------------------

## 3. RECV_OBJECT: publication, CQE, representation, crc placement

### Flow
1. Prep: opaque.c:1601-1622 (described above).
2. First issue, io_opaque_resolve: stream token lookup under `tables` (1649-1660); `io_opaque_data_alloc()` (1677-1681);
   `io_opaque_slot_reserve()` (1682-1686, body 431-457 under `guard(mutex)(&store->tables)`), which reserves an index and
   generation and samples the memory mode:
   ```c
   if (op->data) op->data->forward_only =
           io_opaque_memory_mode(store) == IORING_OPAQUE_MEMORY_LOWMEM;     /* 450-452 */
   ```
   If no object slot is free, the result is **-ENOSPC** (441-442).
3. io_opaque_range_start (1691-1721) runs under `stream->lock`. It inserts the decision into an rb-tree (io_opaque_range_insert
   880-905; overlap gives -EBUSY), enrolls the waiter (io_opaque_wait), runs io_opaque_stream_process (907-940; io_opaque_take
   781-832 moves already-captured undecided extents into `op->data`), and wakes the collector.
4. Collector: io_opaque_actor (1149-1200). It appends directly into the private assembly of the active RECV_OBJECT
   (io_opaque_capture_keep 960-968, io_opaque_append 978-1048). When the range is complete it calls `io_opaque_queue_ready(op, op->length)` (1191-1195).
5. Publication runs as task work on the **submitting ring**: io_opaque_ready, opaque.c:1509-1538:
   ```c
   ret = op->data->length == op->length ? io_opaque_vector(op->data) : -ENODATA;   /* 1510 */
   ...
   mutex_lock(&store->tables);                                                   /* 1514 */
   ... slot->data = op->data;                                                    /* 1519 */
   if (op->data->forward_only) object_flags = IORING_OPAQUE_OBJECT_F_FORWARD_ONLY;  /* 1520-1521 */
   store->nr_objects++; ... slot->born = jiffies;
   if (!slot->data->forward_only && (store->policy.flags & IORING_OPAQUE_POLICY_F_AUTO_COMPACT)) {
           list_add_tail(&slot->candidate, &store->candidates);
           queue_delayed_work(system_dfl_wq, &store->compact_work, msecs_to_jiffies(100)); }
   ```
6. CQE: opaque.c:1561-1562
   ```c
   if (ret >= 0 && op->op == IORING_OPAQUE_RECV_OBJECT)
           io_req_set_res32(req, ret, 0, op->handle, object_flags);
   ```
   **extra1 = handle, extra2 = object_flags**, where the flags are 0 or `IORING_OPAQUE_OBJECT_F_FORWARD_ONLY` (`1U << 2`,
   uapi opaque.h:77). The documentation agrees (rst:161-165, 196-200: "the second [extra word] holds IORING_OPAQUE_OBJECT_F_FORWARD_ONLY or zero").
   In v4 this call was `io_req_set_res32(req, ret, 0, op->handle, 0)` at v4 opaque.c:1393.
   The helper `io_req_set_res32` (io_uring.h:343-350) adds `IORING_CQE_F_32` on CQE_MIXED rings. `io_fill_cqe_req`
   (io_uring.h:297-317) copies big_cqe when the ring is CQE32 or the flag is set. **Failure CQEs are 16-byte** on mixed rings
   (they use io_req_set_res, 1564).

### Free space for a crc channel
- extra2 is a u64 in the OBJECT_STAT flag namespace (opaque.h:75-77): bit0 COMPACTED and bit1 COMPACTING are never set at
  publication, and bit2 is FORWARD_ONLY. **Bits 3..63 are unused.** extra1 is fully used by the opaque handle
  (generation << 14 | index, opaque.c:369-373; the docs forbid decoding it).
- `res` is the length. Generic CQE flags bits 16..31 hold the buffer ID (IORING_CQE_BUFFER_SHIFT), and bit 6 is already
  CONSUMED. Neither is a good channel.
- A workable layout: `extra2 = (u64)crc32c << 32 | object_flags`, plus a new flag such as
  `IORING_OPAQUE_OBJECT_F_CRC32C (1U << 3)` meaning "crc valid". The fuse_zc §9.1.2 plan to put "charge in pages in the
  high 32 bits" then no longer fits. Admission can recalibrate from STAT instead (backing_bytes/extents/objects, uncounted
  but takes `tables`) or OBJECT_STAT (`backing_bytes = data->charge`, `extents = data->nr`, opaque.c:1795-1801; counted).
- Opt-in channel: `sqe->rw_flags` is rejected when nonzero today (opaque.c:1590), so a new RECV flag bit there is
  backward-compatible: an old kernel returns -EINVAL.

### Object representation and versions (opaque_internal.h)
- `struct io_opaque_backing` (24-31): `refs, page (compound head), charge (= page_size(compound head)), filled, copied, accounted`.
- `struct io_opaque_extent` (33-40): `list, backing, start (object-relative after publication), offset (within the compound backing page; may exceed PAGE_SIZE), length, embedded`.
- `struct io_opaque_data` (48-57) is a **version**: `refs, extents list, bvec*, length, charge, nr, dense, forward_only`.
- The bvec vector is built once at publication: io_opaque_vector (opaque.c:345-367),
  `bvec_set_page(&data->bvec[nr++], extent->backing->page, extent->length, extent->offset)`. There is one bvec per extent;
  bv_page is the compound head and bv_offset can exceed PAGE_SIZE.
- Slot: `struct io_opaque_slot` (59-67) holds `data` (the current version), `generation`, `reserved`, `compacting`, and candidate/born/retry.
- Pins: READ_OBJECT/OBJECT_STAT (1661-1667), ordinary SEND (2090-2091) and compaction (opaque_compact.c:226-227) take
  `refcount_inc(&data->refs)`. COMPACT publish swaps `slot->data` to a new dense version (opaque_compact.c:146-171), and
  pinned old versions stay valid until put.

### Are payload bytes touched by the CPU during capture?
- **Retained path: no.** io_opaque_capture (1050-1147) takes page references to the skb head page or frag page:
  `io_opaque_append(..., take_ref=true, copied=false)` then `get_page(page)` (1028-1029).
- **Copy fallback: yes.** The label `copy:` at 1109-1142 runs `skb_copy_bits(skb, offset, mapped + pos, chunk)` (1134)
  into owned pages, appending into the previous owned page when that page has room. Triggers:
  - `copy_frags = shared || skb_cloned(skb) || skb_has_shared_frag(skb);` (1062) for frags. Loopback TCP delivers clones,
    so the UML/loopback test runs are mostly copy fallback.
  - The head is shared or `skb_head_is_locked()` (kmalloc'd or cloned head) (1067, 1076-1077).
  - A compound page larger than the capture limit (1072-1073, 1092).
  - `io_opaque_append` returns -ENOBUFS (extent cap at `max_extents - 2` (1003), or no room for dense copies of a private
    KEEP's remainder (1019-1024)) (1079-1080, 1096-1097).
  - Non-readable (devmem) frags return -EIO (1063).
- Other byte touches: INSPECT does copy_to_user in io_opaque_ready (1507-1508), READ_OBJECT copies, and compaction does a memcpy
  (opaque_compact.c:20-53).

### Where a crc32c could be computed
- **Recommended: at publication**, in io_opaque_ready after `io_opaque_vector()` (opaque.c:1510) and before
  `mutex_lock(&store->tables)` (1514). At that point:
  - the vector is ready;
  - only the submitting ring's uring_lock is held (task work, sleepable, so cond_resched between pages is allowed);
  - no store, stream or socket lock is held;
  - the cost lands on the messenger ring's thread, which computes it in userspace today.
- The crc is an extra full read of the payload, because the retained path never touches it.
- API: `u32 crc32c(u32 crc, const void *p, size_t len);` at include/linux/crc32.h:82 ("does *not* invert the CRC at the
  beginning or end"). It requires `select CRC32` in the IO_URING_OPAQUE_OBJ Kconfig (io_uring/Kconfig:18-27 has no select
  today; the round-2 UML .config had CRC32=y by accident).
- Iterate `data->bvec` per page with kmap_local_page, because bvecs can span compound pages.
- Store the crc in `io_opaque_data` if it should survive into OBJECT_STAT. Compaction must then copy it, as it copies
  `forward_only` (opaque_compact.c:92).
- Not recommended: incremental computation in io_opaque_take/append. That code runs under `stream->lock` and, in the
  collector, `lock_sock`.
- Reference check: crc32c("123456789") with seed ~0 and a final xor is 0xE3069283. Without the final xor it is
  ~0xE3069283 = 0x1CF96D7C, which matches fuse_zc §9.1.2.

---------------------------------------------------------------------------------------------

## 4. Memory modes, FORWARD_ONLY, pressure CQE, ENOSPC, capture hold

### Config (uapi opaque.h:23-38; validation opaque.c:477-499)
- `hard_limit` (page-aligned, ≤ 1 TiB), `compact_headroom`, `max_object_size ≤ INT_MAX`, `max_objects ≤ 16384`,
  `max_streams ≤ 1024`, `max_extents` from 4 to 65536, `max_requests ≤ 4096` (480-489). These lines were 375-384 in v4.
- New: `high_watermark`, `low_watermark`, `reply_reserve` (490-499). A nonzero high watermark **requires a nonzero
  reply_reserve** (493-494), requires low < high, and requires high ≤ hard_limit − compact_headroom − reply_reserve.
- Capture limit: `io_opaque_rx_limit() = hard_limit - compact_headroom - reply_reserve` (opaque_internal.h:166-170).
- Registration charges the **whole hard_limit** up front against locked memory: opaque.c:516
  `io_account_mem(ctx->user, ctx->mm_account, cfg.hard_limit >> PAGE_SHIFT)`. That means RLIMIT_MEMLOCK unless the task has
  CAP_IPC_LOCK (io_uring.c:3024-3025, rsrc.c:114-129), plus `pinned_vm`.
- Retained RX pages are also charged to the registering task's memcg objcg (opaque.c:234-242, 262-265, 523-531). Copies
  use GFP_KERNEL_ACCOUNT under `set_active_memcg(store->memcg)` (1385).

### LOWMEM watermarks
- io_opaque_memory_mode, opaque.c:101-128. With `high_watermark == 0` the mode is always NORMAL.
  `used = bytes - transient` is read under the `transient_seq` seqcount. Unpublished compaction copies and ordinary-pool
  framing are "transient" and excluded (opaque_internal.h:130-138).
  NORMAL→LOWMEM at `used >= high`; LOWMEM→NORMAL at `used <= low`; the transition is a cmpxchg on `store->memory_mode`.
  The mode is re-evaluated on charge (141), uncharge (150), release_transient (187), STAT (1744) and slot reserve (452).
  STAT reports `memory_mode` (uapi opaque.h:100).

### FORWARD_ONLY (advisory since round 2)
- It is set only at RECV_OBJECT slot reservation (opaque.c:450-452). It is reported in CQE extra2 (1520-1521, 1562) and
  OBJECT_STAT (1804-1805).
- It **is not enforced anywhere**. Grepping `forward_only` finds only 451, 1520, 1528, 1804, opaque_compact.c:92 and 271.
  Ordinary SEND, fan-out, retry and READ_OBJECT are all allowed (rst:436-443: "The classification is advisory").
- It is excluded from **automatic** compaction: at publication (1528) and at policy re-enrolment (opaque_compact.c:270-274).
  Manual COMPACT is allowed, and the replacement inherits the flag (opaque_compact.c:92).

### Automatic compaction rate (matters for ObjectCacher staging)
- opaque_compact.c:292-363:
  - pacing `store->auto_at = jiffies + msecs_to_jiffies(100)` (306);
  - a scan of at most 32 candidates (314);
  - minimum age `slot->born + msecs_to_jiffies(100)` (324);
  - **at most one object per scan** (`break;` at 343).
- So automatic compaction does **at most about 10 objects per second per store**, and the token bucket applies on top.
  Manual COMPACT costs one counted request per object.

### Collector pressure CQE (nonterminal -ENOBUFS)
- io_opaque_budget_cqe, opaque.c:1330-1346:
  ```c
  u64 extra[2] = { stream->token, stream->next };
  if (op->budget_reported && op->offset == stream->next) return 0;     /* coalesced per offset */
  cqe[0].res = -ENOBUFS;  cqe[0].flags = IORING_CQE_F_MORE | ctx_cqe32_flags(op->req->ctx);
  if (!io_req_post_cqe32(op->req, cqe)) return -ENOSPC;
  ```
  extra1 is the stream token and extra2 is the absolute first unread stream offset (rst:81-86).
- It is posted from io_opaque_recv when `ret == -ENOBUFS && !stream->error && !stream->eof` (1416-1424).
- The collector then enrolls on `store->budget_waits` and returns IOU_RETRY (1427-1439). It is woken by
  io_opaque_wake_budget (40-76) when capacity is released, from any ring, through
  `wake_up_interruptible_poll(sk_sleep(sock->sk), EPOLLIN)` (72).
- A partial private RECV_OBJECT that hits -ENOBUFS restores its prefix and completes -ENOBUFS (1405-1415, 926-929, 936-937).
- Transient ENOMEM backs off 50 ms (IO_OPAQUE_ALLOC_RETRY_MS, 1430-1431, retry_work 74-75).

### ENOSPC termination
- If the pressure CQE cannot be posted (CQ full): `stream->error = err` (-ENOSPC), and io_opaque_stream_process fails the
  pending ranges (1417-1423). The collector then completes with `ret = stream->error` (1425-1426), terminal.
- The token and captured bytes remain. There is no re-arm; STREAM_CLOSE is required (rst:116-122).
- A collector cleanup closes the stream only if `!eof && !closed && !error` (1958-1961). After a terminal error or EOF,
  the **receive claim stays until STREAM_CLOSE**.
- Attach with a full CQ is a separate -ENOSPC, taken before the claim (1304-1307).

### Capture hold rule
- `stream->hold_undecided = !io_opaque_collect_demand(stream);` is evaluated once per pass (1384).
- io_opaque_collect_demand (950-958):
  ```c
  return !stream->undecided || !list_empty(&stream->reads) || (op && op->offset > stream->next);
  ```
- io_opaque_actor (1162-1179): bytes inside the earliest decision are always taken. Otherwise:
  `if (stream->hold_undecided && !op) break; if (op) chunk = min(chunk, op->offset - stream->next);`
  This is the G1 fix: a gap before a later decision is collectable even in a held pass.
- INSPECT returns only bytes already collected (io_opaque_snapshot 746-779 / io_opaque_available 726-744). An INSPECT at an
  offset with nothing available stays on `stream->reads` (915-916) and therefore reopens collection. **Re-INSPECT at the same
  offset never grows on a held stream; INSPECT at the end offset to pull more** (rst:101-103, 205-208).
- A FIN queued behind held bytes is not consumed until they are decided (rst:103-105).
- One pass is bounded by `IO_OPAQUE_RX_BATCH SZ_256K` (opaque_internal.h:16; desc.count at 1353).

### What matters for a consumer that copies into FUSE pages (FILL) rather than sending
- FORWARD_ONLY is advisory, so FILL must accept such objects. Treat the flag as "deliver now or FREE; do not stage in
  ObjectCacher". It also means the object will never be auto-compacted.
- `reply_reserve` serves only framed/generated SEND framing (charge_frame 193-212). A read-only consumer never uses it, yet
  watermarks require it to be nonzero (493-494). Configure it minimally (PAGE_SIZE). It reduces capture capacity.
- Watermarks only classify. Nothing caps cacheable bytes except hard_limit (rst:449-451).
- FILL with LAST and FREE release backing → `io_opaque_uncharge` → wake_budget. That **wakes collectors on other rings**:
  a collector re-runs as task work on its own ring, and DEFER_TASKRUN local work signals the registered eventfd
  (io_uring/tw.c:174-177).
- The pressure CQE and ENOSPC arrive on the messenger ring that owns COLLECT. Size those CQs.
- The hold rule constrains messenger framing (DISCARD protocol bytes promptly) and does not affect FILL.

---------------------------------------------------------------------------------------------

## 5. Handle lookup: data structures and locks

- Ring-local store IDs: `struct xarray opaque_stores` (XA_FLAGS_ALLOC), include/linux/io_uring_types.h:497-500,
  initialized at io_uring.c:293-296. xa_alloc at registration (opaque_register.c:116); xa_load at prep (opaque.c:1570, 1224).
  No opaque lock is involved.
- Store tables (opaque_internal.h:103-155), **all under one store-wide `struct mutex tables`** (107-118):
  - `objects[]` is a kvcalloc'd array of `max_objects` slots (opaque.c:506-507), with `object_used` bitmap and rotating `object_cursor`;
  - `streams[]` plus `stream_used` and `stream_cursor`;
  - `txs` is an rb-tree of per-socket send queues;
  - `candidates` list.
- Handle = `generation << 14 | index` (opaque.c:369-378; IO_OPAQUE_INDEX_BITS 14 at opaque_internal.h:20). There are 50
  generation bits, and an exhausted generation retires the slot (394-398, 445-446).
- Lookup is O(1) and asserts the mutex: opaque.c:400-411
  ```c
  lockdep_assert_held(&store->tables);
  if (!handle || index >= store->config.max_objects) return NULL;
  if (store->objects[index].generation != handle >> IO_OPAQUE_INDEX_BITS) return NULL;
  ```
  **There is no RCU and no xarray for handles.**
- Users of `tables` (any ring that imports the store, plus workers):
  - resolve (1648)
  - slot reserve (437)
  - publication (1514)
  - COMPACT end (1542)
  - FREE (1780)
  - STAT (1749)
  - OBJECT_STAT (1806)
  - STREAM_CLOSE (1828)
  - cleanup of a reserved slot (1967)
  - SEND tx_enter (2137) and tx_put (2190)
  - attach (1270)
  - compaction start, publish and auto scan (opaque_compact.c:216, 153, 301, 351)
  - set_policy (258), which walks every object slot
  - stop (561/573/589)
- So **yes: concurrent control ops from many rings serialize on one sleeping mutex.** Hold times are short, and the
  mutex is never held across copies.
- Other shared, store-wide points:
  - `spinlock_t wait_lock` is taken by io_opaque_wake_budget (50), and **on every extent free** through
    io_opaque_extent_free → wake_budget (302-312), directly or via uncharge. Releasing an object therefore costs O(extents)
    wait_lock acquisitions, each walking the budget waiter list (O(waiters)).
  - `transient_lock` plus seqcount (155-190) for framing and compaction.
  - Atomics `bytes` (cmpxchg loop 130-145), `extents`, `requests`, `memory_mode`.
- Per stream: `struct mutex lock` (opaque_internal.h:84). Per ring: `uring_lock`, `opaque_cache` (io_uring_types.h:412-414)
  and `opaque_waits` (424-426).
- The memory note records the `tables` mutex and the O(waiters) wake as author-deferred items.

---------------------------------------------------------------------------------------------

## 6. TCP claim (patch 1/5)

- Config: net/Kconfig:29 `config SOCK_RX_OWNER` (bool, depends on INET). It is selected by IO_URING_OPAQUE_OBJ (io_uring/Kconfig:22).
- Type: include/linux/net.h:203 `struct sock_rx_owner { sk_read_actor_t actor; void *data; };`. Field: include/net/sock.h:578
  `struct sock_rx_owner *sk_rx_owner;`. Helpers at sock.h:622-642: `sock_rx_owned()` and `sock_rx_owner_conflict(sk, desc, actor)`.
- Set by attach under lock_sock and `sk_callback_lock`: opaque.c:1309 `WRITE_ONCE(sock->sk->sk_rx_owner, &stream->owner);`.
  Cleared in io_opaque_stream_close (1209-1213).
- What userspace sees while the socket is claimed:
  - **recv/recvmsg/read returns -EBUSY**: tcp.c:2682-2683 at tcp_recvmsg_locked entry, `if (sock_rx_owned(sk)) return -EBUSY;`.
    This includes MSG_PEEK and nonblocking calls. A reader already asleep in sk_wait_data is woken by attach
    (opaque.c:1323 wake; sock.c:3324-3325 the wait condition includes `sock_rx_owned(sk)`) and gets the bytes it copied so far
    or -EBUSY (tcp.c:2724-2727).
  - MSG_ERRQUEUE still works; it is handled before the lock in tcp_recvmsg.
  - splice and other `tcp_read_sock` actors get -EBUSY (tcp.c:1743-1744, 1754-1755).
  - tcp_read_skb (sockmap) gets -EBUSY (1764-1765); TCP_ZEROCOPY_RECEIVE gets -EBUSY (2222-2223).
  - TCP_REPAIR setsockopt gets -EBUSY (4003-4004); TCP_ULP gets -EBUSY (tcp_ulp.c:167-168); psock init gets -EBUSY (skmsg.c:757-760).
  - `sk_clone` clears the claim (sock.c:2589).
- **sendmsg is unaffected.** Patch 1 has no hunk in the TX path; the only tcp.c hunks are the ones listed above.
- **tcp_poll is unchanged** (tcp.c:532-626). EPOLLIN comes from `tcp_stream_is_readable(sk, target)` (597-598) and RCV_SHUTDOWN
  (583-584). Held undecided bytes sit in `sk_receive_queue` while the stream holds (rst:107-108 "visible with ioctl(SIOCINQ)"),
  so a **userspace epoll still sees EPOLLIN**.
- The opaque code also issues `wake_up_interruptible_poll(sk_sleep, EPOLLIN)` on attach (1323) and budget wake (72), and
  `EPOLLIN | EPOLLHUP` on close (1218).
- Ceph's EventCenter must therefore remove EVENT_READABLE. Keeping EPOLLOUT is fine: epoll filters wakeups by key
  (fs/eventpoll.c:1519, `if (pollflags && !(pollflags & epi->event.events))`). EPOLLHUP and EPOLLERR are always in the mask
  (eventpoll.c:2694/2714), so stream close produces one benign writable/hup event.
- **connect(AF_UNSPEC) returns -EBUSY**: af_inet.c:652-653 `if (sock_rx_owned(sk)) return -EBUSY;`, before
  `sk_disconnects++`. The tcp_disconnect backstop is at tcp.c:3386-3387.
- Attach preconditions, opaque.c:1246-1327 (v4 cited 1138-1160):
  - not TCP SOCK_STREAM → -EOPNOTSUPP (1255-1256)
  - an existing rx owner, a ULP (`sk_prot != sk_prot_creator`), repair or urgent data → -EBUSY (1272-1276)
  - state not ESTABLISHED or CLOSE_WAIT → -ENOTCONN (1277-1280)
  - no stream slot → -ENOSPC (1253, 1284-1287)
  - `sk_user_data` set → -EBUSY (1294-1297)
  - full CQ → -ENOSPC, before the claim (1304-1307)
  - The token CQE has res 0, `IORING_CQE_F_MORE`, and the token in extra1 (1301-1304).
  - The stream holds `get_file()` on the socket (1266); closing the fd alone releases nothing (rst:126-131).

---------------------------------------------------------------------------------------------

## 7. Registered-buffer API in this base

- Signature: include/linux/io_uring/cmd.h:97-100
  ```c
  int io_buffer_register_bvec(struct io_uring_cmd *cmd, const struct bio_vec *bvs, unsigned int nr_bvecs,
                              void (*release)(void *), void *priv, u8 dir, unsigned int index, unsigned int issue_flags);
  ```
  Body: io_uring/rsrc.c:1099-1132 (function at 1102). "bvs is copied internally." It runs under `io_ring_submit_lock(ctx, issue_flags)` and calls
  io_kernel_buffer_init (1018-1059): -EBUSY if the slot is occupied; `imu->ubuf = 0`, `imu->len = total_bytes`,
  `imu->dir = dir`, `imu->flags = IO_REGBUF_F_KBUF`. Unregister is io_buffer_unregister (1134-1165); it drops the table's node
  reference and NULLs the slot. Also io_buffer_register_request (1063-1097) for ublk.
- FUSE use: fs/fuse/dev_uring.c:799-841 (fuse_uring_set_up_zero_copy):
  ```c
  if (req->args->out_pages) ddir |= IO_BUF_DEST;      /* READ  */
  if (req->args->in_pages)  ddir |= IO_BUF_SOURCE;    /* WRITE */
  bvs[i].bv_page = folio_page(ap->folios[i], 0); bvs[i].bv_offset = ap->descs[i].offset; bvs[i].bv_len = ap->descs[i].length;
  err = io_buffer_register_bvec(ent->cmd, bvs, ap->num_folios, fuse_zero_copy_release, zc_bvs, ddir, ent->zero_copy_index, issue_flags);
  ```
  The release does folio_put on each folio (788-797). The slot is unregistered at request end: fuse_uring_req_end →
  zero_copy_unregister (136-150, 172). It is registered in the buffer table of the **FUSE ring entry's ctx**
  (`cmd_to_io_kiocb(cmd)->ctx`), so FILL must be issued on that same ring.
- Direction constants: include/linux/io_uring_types.h:57-60 `IO_BUF_DEST = 1 << ITER_DEST, IO_BUF_SOURCE = 1 << ITER_SOURCE`.
  ITER_DEST = 0 and ITER_SOURCE = 1 (include/linux/uio.h:34-35).
- User-registered buffers are always `IO_BUF_DEST | IO_BUF_SOURCE` with `ubuf = user VA` (rsrc.c:908, 915). **Direction rejection
  can only be exercised with a kernel buffer.**
- Lookup: io_find_buf_node, rsrc.c:1244-1265
  ```c
  if (req->flags & REQ_F_BUF_NODE) return req->buf_node;
  req->flags |= REQ_F_BUF_NODE;
  io_ring_submit_lock(ctx, issue_flags);
  node = io_rsrc_node_lookup(&ctx->buf_table, req->buf_index);
  if (node) { node->refs++; req->buf_node = node; ... }
  ```
  The buffer table and `node->refs` (a plain int) are protected by **ctx->uring_lock** (io_put_rsrc_node asserts it, rsrc.h).
  `req->buf_index` must be set by the op's prep (READ_FIXED: rw.c:260 `req->buf_index = READ_ONCE(sqe->buf_index);`).
  `req->buf_node` is in a union with `req->kbuf` (io_uring_types.h:792-797). IOSQE_BUFFER_SELECT is rejected for ops without
  `.buffer_select` (io_uring.c:1778-1782).
- Import: io_import_reg_buf, rsrc.c:1267-1277 (no node gives -EFAULT), then io_import_fixed (1192-1242):
  - validate_fixed_range (1167-1180): within `[ubuf, ubuf+len)` and `len <= MAX_RW_COUNT`, else -EFAULT
  - direction check, rsrc.c:1205-1206: `if (!(imu->dir & (1 << ddir))) return -EFAULT;`
  - KBUF branch (1214-1215 → io_import_kbuf 1182-1190): `iov_iter_bvec(iter, ddir, imu->bvec, imu->nr_bvecs, len + offset); iov_iter_advance(iter, offset);`
    For kernel buffers `buf_addr` is therefore a byte offset.
- **`io_req_assign_buf_node` does not exist in this base.** `git grep io_req_assign_buf_node` on v9 io_uring/include finds
  nothing. io_find_buf_node + REQ_F_BUF_NODE replaced it.
- Lifetime across async completion:
  - The node reference taken at import is dropped only at request free: io_req_put_rsrc_nodes, io_uring.c:1082-1092
    (`if (req->flags & REQ_F_BUF_NODE) io_put_rsrc_node(ctx, req->buf_node);`).
  - The last reference triggers io_free_rsrc_node → io_buffer_unmap → `imu->release(imu->priv)` (rsrc.c:586-604, 210-226).
  - So an in-flight request keeps the FUSE folios alive even after FUSE unregisters the slot.
  - READ_FIXED imports once (rw.c:359-371; skipped on retry when `io->bytes_done`), and the saved iter is reused on retries.
  - SEND_ZC imports into the **notif** request (net.c:1507-1512 `notif->buf_index = req->buf_index; io_import_reg_buf(notif, ...)`),
    so the buffer outlives the send CQE until the zerocopy notification.
- How a new op imports an ITER_DEST buffer and keeps it:
  1. In prep, set `req->buf_index = READ_ONCE(sqe->buf_index)`.
  2. In issue, call `io_import_reg_buf(req, &iter, dest_addr, len, ITER_DEST, issue_flags)`.
  3. Nothing else is needed: REQ_F_BUF_NODE pins the node until the request is freed.
  - If the op already holds the submit lock, pass `issue_flags & ~IO_URING_F_UNLOCKED`. This has precedent at
    io_uring/poll.c:949 (`io_poll_add(preq, issue_flags & ~IO_URING_F_UNLOCKED)` inside io_ring_submit_lock at 925).
    Calling with IO_URING_F_UNLOCKED while already holding it self-deadlocks on the non-recursive mutex.
  - Alternatively, import before taking the lock.
- Copy primitives:
  - `copy_page_to_iter(page, offset, bytes, iter)` (lib/iov_iter.c:361-387) handles compound pages through page_copy_sane
    (338-357) and requires an ITER_DEST destination.
  - `iterate_bvec` (include/linux/iov_iter.h:108-143) handles multi-page destination bvecs (large folios).
  - `copy_page_from_iter` (lib/iov_iter.c:423) and `import_ubuf` (lib/iov_iter.c:1445) are the FIXED_COPY counterparts.
- FUSE flushes the dcache at commit even when the copy is skipped (fs/fuse/dev.c:1322-1323 `if (folio && !cs->write) flush_dcache_folio(folio);`).
  For short replies the commit zeroes outside `[offset, offset+count)` (dev.c:1273-1275).

---------------------------------------------------------------------------------------------

## 8. Adding a new opcode

- UAPI: append to `enum io_uring_op` before `IORING_OP_LAST` (include/uapi/linux/io_uring.h:322-327; the last is OPAQUE_COLLECT at 324).
- opdef.c:
  - Add an `io_issue_defs[]` entry inside `#ifdef CONFIG_IO_URING_OPAQUE_OBJ`, with `.prep = io_eopnotsupp_prep` in the else
    branch, as in 595-633.
  - Add an `io_cold_defs[]` entry with `.name`, `.cleanup`, and optionally `.fail` (892-906).
  - io_uring_optable_init (opdef.c:924-938) BUILD_BUG_ONs the array sizes against IORING_OP_LAST, BUG_ONs a missing prep or issue,
    and WARNs on a missing name.
  - Everything else (restrictions bitmap, bpf filter, probe/query `nr_request_opcodes`, fdinfo) is sized by IORING_OP_LAST
    (io_uring_types.h:271, bpf_filter.c, query.c:23, fdinfo.c:110).
- Fields to set:
  - `.ioprio = 1` if ioprio carries flags (io_uring.c:1805)
  - `.audit_skip = 1` as the opaque ops do
  - no `.needs_file` for fd = -1
  - `.async_size = sizeof(struct io_opaque_req)` if the op reuses the opaque request state (cache: io_opaque_cache_init, opaque.c:24-28)
- Cancellation hooks (v9 io_uring/cancel.c):
  - cancel.c:122-127: io_try_cancel tries `io_opaque_cancel()` after io_poll_cancel.
  - cancel.c:565: io_uring_try_cancel_requests calls `io_opaque_cancel_all()`.
  - Both walk `ctx->opaque_waits` (opaque.c:1904-1932). Requests enroll there only through io_opaque_wait (669-676) while
    waiting (RANGE_WAIT, BUDGET_POLL, SEND_WAIT, COPY_WORK).
  - **A synchronous op (FILL, FIXED_COPY) needs no cancellation hook.**
  - Ring teardown: io_terminate_opaque (io_uring.c:2330, under uring_lock in io_ring_exit_work) and io_unregister_opaque
    (io_uring.c:2172).
- CQE32/CQE_MIXED requirement: checked **only** at store registration and import, opaque_register.c:71-73
  ```c
  if (!(ctx->flags & IORING_SETUP_DEFER_TASKRUN) ||
      !(ctx->flags & (IORING_SETUP_CQE32 | IORING_SETUP_CQE_MIXED)))
          return -EINVAL;
  ```
  DEFER_TASKRUN itself requires SINGLE_ISSUER (io_uring.c:2839-2841). Big CQEs are posted with io_req_set_res32 /
  io_req_post_cqe32 plus `ctx_cqe32_flags()` (io_uring.h:336-350; io_uring.c:898-917).
  A FILL CQE needs only 16 bytes, but a FUSE ring must still be CQE32 or CQE_MIXED to import the store.
- Store import: `IORING_REGISTER_OPAQUE_STORE = 38` (uapi io_uring.h:733-734; register.c:960-965). Export and import use an anon
  inode fd "[io_uring-opaque]" (opaque_register.c:29-60, 92-100).

---------------------------------------------------------------------------------------------

## 9. FIXED_COPY precedent; READ_FIXED from /dev/zero into a kernel buffer

- **No existing op copies user memory into a registered buffer.** The v9 opcode list (opdef.c cold names): NOP, READV, WRITEV,
  FSYNC, READ_FIXED, WRITE_FIXED, …, SEND_ZC, SENDMSG_ZC, READ_MULTISHOT, …, RECV_ZC, EPOLL_WAIT, READV_FIXED, WRITEV_FIXED, PIPE,
  NOP128, URING_CMD128, OPAQUE_*.
  - The registered-buffer destinations are READ_FIXED/READV_FIXED (file → buffer), RECV with IORING_RECVSEND_FIXED_BUF
    (socket → buffer, net.c:1216-1219 ITER_DEST), and uring_cmd driver imports (uring_cmd.c:299).
  - User memory reaches a slot only through an intermediate: pipe, socketpair, memfd or tmpfs, then READ_FIXED. That is 2 copies.
  - /proc/self/mem has no read_iter, so it goes through loop_rw_iter, which rejects KBUF (rw.c:705-707).
- READ_FIXED on /dev/zero with a kernel-registered DEST bvec **works**:
  - rw.c:1226-1235 io_read_fixed → io_init_rw_fixed (rw.c:359-371) → `io_import_reg_buf(req, &io->iter, rw->addr, rw->len, ITER_DEST, issue_flags)`
    → io_import_kbuf (bvec, ITER_DEST)
  - → io_read → __io_read (913+) → io_iter_do_read (830-839) uses `f_op->read_iter`
  - → read_iter_zero (drivers/char/mem.c:450-472) → `iov_iter_zero` (lib/iov_iter.c:465-469), which handles bvecs via iterate_bvec.
  - /dev/zero has FMODE_NOWAIT (mem.c:701, applied at 723-724) and `zero_fops.read_iter` (mem.c:666-672).
  - loop_rw_iter, which refuses KBUF, is not used because read_iter exists.
- **But it can complete short.**
  - read_iter_zero returns the bytes written so far on `signal_pending()` (463-464) or on `need_resched()` with IOCB_NOWAIT
    (465-468). The inline attempt sets IOCB_NOWAIT.
  - __io_read then returns the short count, because `!need_complete_io(req)` holds for a char device (rw.c:842-846, 976-980).
  - kiocb_done → __io_complete_rw_common sets REQ_F_FAIL and `cqe.res = short` (rw.c:541-551, 649-672).
  - So the "all-or-error, never short" assumption fails for ZERO pieces. Resubmit the remainder.
- FIXED_COPY implementation shape:
  1. In prep, set `req->buf_index`.
  2. In issue, call `io_import_reg_buf(req, &dst, addr3, len, ITER_DEST, issue_flags)` and `import_ubuf(ITER_SOURCE, u64_to_user_ptr(addr), len, &src)`.
  3. For each destination bvec segment, call `copy_page_from_iter(bv_page, bv_offset + skip, n, &src)`. Shortfall means -EFAULT.
  - There is no iter→iter helper.
  - The source can fault. Inline issue may block on a major fault; an alternative is pagefault-disabled first, then -EAGAIN
    to io-wq (design choice).
  - It handles only DEST slots. FUSE WRITE slots are SOURCE-only and would need the reverse direction.

---------------------------------------------------------------------------------------------

## 10. Test infrastructure

- KUnit:
  - io_uring/opaque_test.c, suite "io_uring-opaque" (972-977), 28 KUNIT_CASEs (940-970).
  - Built by io_uring/Makefile:21 `obj-$(CONFIG_IO_URING_OPAQUE_OBJ_KUNIT_TEST) += opaque_test.o`. The Kconfig at io_uring/Kconfig:34-41
    is a bool depending on KUNIT && IO_URING_OPAQUE_OBJ, built-in only.
  - Internal functions are exported to it via `VISIBLE_IF_KUNIT` and declared at opaque_internal.h:192-216.
  - Fixtures: opaque_test_store (9-32, a fake store with 4 slots and a 128-page limit); opaque_test_frag (44-53, a skb with an
    rx frag); opaque_sparse_data (405-427, an 80,000-byte object in 40 extents of 2000 bytes each, byte value = extent index,
    good for crc and FILL copy tests); opaque_test_copy (430-448).
  - The SEND_LAST ownership model test is opaque_send_last_ownership (600-645).
  - A FILL copy test can run in KUnit against a hand-built ITER_DEST bvec iterator (no ring needed).
- Selftest:
  - tools/testing/selftests/io_uring/opaque_obj.c (4475 lines). It uses raw syscalls with no liburing: ring_init with
    SINGLE_ISSUER|DEFER_TASKRUN (60-92), stage/enter/peek/wait_tag (103-173), cmd_stage/command (175-198),
    register/export/import_store (200-233), tcp_pair (269), and send_stage_flags/frame_stage (315-352).
  - main (4167-4474) runs 97 results (`ksft_set_plan(97)` at 4202) and then calls the test_* functions (4428-4473).
  - Timeout: `OPAQUE_TEST_TIMEOUT` env var, default 600 s (4150-4165).
  - Makefile: `TEST_GEN_PROGS := opaque_obj`, `CFLAGS += $(KHDR_INCLUDES)`, `-pthread`. config: IO_URING, INET, IO_URING_OPAQUE_OBJ.
  - Model for a FILL LAST race: opaque_obj.c:2050-2102. Two rings each get a linked INSPECT head plus LAST. Writing one byte
    releases both, and the test checks one winner with CONSUMED and an -ESTALE loser.
- run_opaque_vm.sh needs QEMU x86_64, cpio and static busybox, which are not on this box. It builds an initramfs whose
  /init mounts proc, sys, devtmpfs, debugfs and cgroup2 (+memory), brings lo up, runs /opaque_obj, and greps the KUnit
  "io_uring-opaque: pass:N fail:0" line.
- UML recipe (memory file plus earlier session artifacts):
  - Build with `make ARCH=um`. The round-2 config (scratchpad b491e9fd…/build-r2/.config) had UML, SMP, NR_CPUS=4, HOSTFS, MEMCG,
    KASAN, PROVE_LOCKING, LOCKDEP, DEBUG_ATOMIC_SLEEP, KUNIT, FAULT_INJECTION, FAILSLAB, CRC32,
    IO_URING_OPAQUE_OBJ=y and IO_URING_OPAQUE_OBJ_KUNIT_TEST=y. It had **no FUSE_FS**.
  - Use a hostfs root directory holding static busybox, the static opaque_obj and the same /init.
  - Boot: `./linux [mem=2G seccomp=on ncpus=2] rootfstype=hostfs rootflags=<root> rw init=/init [TESTS=<prog> ARGS=<args>] [kunit.enable=0] con=null con0=null,fd:1`.
    SMP needs `seccomp=on`; without `mem=` UML OOMs. Use `OPAQUE_TEST_TIMEOUT=7000` for SMP+KASAN.
  - Earlier logs show KUnit at boot "# io_uring-opaque: pass:28 fail:0 skip:0 total:28" and selftests "# Totals: pass:97 fail:0".
  - For per-commit builds, force IO_URING_OPAQUE_OBJ on: olddefconfig at 1/5 drops it.
  - Known noise: kmemleak `ssl_init`/`parse_chan` boot objects, and an RCU splat at UML SMP boot.
- Kernel-buffer tests (FILL or FIXED_COPY into a KBUF, and direction rejection) need a kernel-buffer provider in the test
  kernel: FUSE_FS + FUSE_IO_URING with a minimal raw FUSE-over-io_uring server, or BLK_DEV_UBLK (tools/testing/selftests/ublk/kublk
  uses io_buffer_register_request). Neither is in the current UML config. Whether either runs under UML is UNVERIFIED.
  User-registered buffers (IORING_REGISTER_BUFFERS) cover offsets and bounds but not direction.

---------------------------------------------------------------------------------------------

## Appendix A: opaque.c function index (v9)

```
40  io_opaque_wake_budget        101 io_opaque_memory_mode        130 io_opaque_charge
147 io_opaque_uncharge           155 io_opaque_charge_transient   178 io_opaque_release_transient
193 io_opaque_charge_frame       252 io_opaque_extent_new         293 io_opaque_extent_free
335 io_opaque_data_put           345 io_opaque_vector             370 io_opaque_handle
400 io_opaque_lookup             414 io_opaque_detach             431 io_opaque_slot_reserve
469 io_opaque_alloc              554 io_opaque_stop               638 io_opaque_req_alloc
669 io_opaque_wait               678 io_opaque_queue_ready        746 io_opaque_snapshot
781 io_opaque_take               835 io_opaque_restore            880 io_opaque_range_insert
907 io_opaque_stream_process     951 io_opaque_collect_demand     978 io_opaque_append
1050 io_opaque_capture           1149 io_opaque_actor             1202 io_opaque_stream_close
1222 io_opaque_recv_prep         1246 io_opaque_attach            1330 io_opaque_budget_cqe
1348 io_opaque_recv              1457 io_opaque_copy              1494 io_opaque_ready
1580 io_opaque_prep              1639 io_opaque_resolve           1691 io_opaque_range_start
1730 io_opaque_stat              1756 io_opaque_issue             1863 io_opaque_cancel_req
1904 io_opaque_cancel            1921 io_opaque_cancel_all        1934 io_opaque_cleanup
1992 io_opaque_fail              2004 io_opaque_send_iter         2013 io_opaque_frame_import
2072 io_opaque_send_acquire      2097 io_opaque_send_prep         2131 io_opaque_tx_enter
2181 io_opaque_tx_put            2226 io_opaque_send
```

## Appendix B: fuse_zc.md kernel assumptions vs v9

| fuse_zc.md | v9 status |
|---|---|
| §3 ring setup and opaque_register.c:71-72 | Correct (71-73). DEFER_TASKRUN implies SINGLE_ISSUER (io_uring.c:2839-2841). |
| §1.3/§6.1 sendmsg allowed on an attached socket | Correct; there is no TX hunk. |
| §5.1 caps at opaque.c:375-384 | Now 480-489. New watermark and reply_reserve fields at 490-499. Capture limit = hard − headroom − reply_reserve. |
| §5.1 "charged to the owner's memcg" | Incomplete. hard_limit is also charged up front to RLIMIT_MEMLOCK/pinned_vm (opaque.c:516) unless CAP_IPC_LOCK. |
| §5.1 "store stops reading every socket" | Refine. Each collector stalls with a nonterminal -ENOBUFS CQE {token, next} and auto-resumes. Streams holding undecided bytes stop regardless. A full CQ on the pressure CQE gives terminal ENOSPC. |
| §5.2(2) auto compaction | At most 1 object per 100 ms per store (opaque_compact.c:306, 314-343); skips FORWARD_ONLY. |
| §5.2(3)/§9.1.3 per-stream reserve | Absent in v9. The hold rule covers most of the "per-stream cap"; the reserve guarantee is still missing; reply_reserve is SEND framing only. |
| §6.1 "stream slot and reserve available" | No reserve. Attach errors: -ENOTCONN, -ENOSPC (no slot or full CQ), -EBUSY, -EOPNOTSUPP. COLLECT rejects IOSQE_ASYNC and CQE_SKIP and is counted in max_requests. |
| §6.1 step 1 (tcp_poll unchanged, recv -EBUSY) | Correct (tcp.c:532-626, 2682). |
| §6.1 attach checks at opaque.c:1138-1160 | Now 1255-1307. |
| §6.1 teardown | Correct, and stronger: after a terminal error, EOF or ENOSPC the claim stays until STREAM_CLOSE (opaque.c:1958-1961). STREAM_CLOSE drops undecided bytes, so the connection must fault (no userspace fallback mid-stream). |
| §6.2 INSPECT framing | Add the hold rule: DISCARD inspected protocol bytes promptly; re-INSPECT at the end offset, not the same offset; a FIN behind held bytes waits. |
| §6.3/§9.1.2 crc in extra2 low 32, charge in high 32 (cites opaque.c:1393) | Wrong for v9. extra2 = object flags (FORWARD_ONLY) at opaque.c:1562. Use extra2[63:32] for the crc plus a new flag bit; drop the charge (use STAT/OBJECT_STAT). Compute after io_opaque_vector (1510); `select CRC32`. |
| §9.1.1 submit lock at opaque.c:1590-1685 | Now io_opaque_issue 1762-1859. The deadlock exists only with IO_URING_F_UNLOCKED (io-wq). Fix by importing before the lock, or with `issue_flags & ~IO_URING_F_UNLOCKED` (poll.c:949). |
| §9.1.1 LAST semantics | Matches io_opaque_send_acquire/io_opaque_detach. Add: validate and import the destination before detach. Keep CONSUMED on the generic failure path (.fail or IOU_COMPLETE). |
| §9.1.1 max_requests / -EAGAIN | The -EAGAIN comes at prep and halts the submit batch unless SUBMIT_ALL. Exempting FILL (it is synchronous) is a design option. |
| §9.1.4 "Prep and lookup take tables" | Prep doesn't; first-issue resolve does. The wait_lock spinlock per extent free is a second hotspot. STAT takes tables. |
| §9.2 READ_FIXED /dev/zero | Works with KBUF but **can complete short** (mem.c:463-468, rw.c:976-980). |
| §9.2 FIXED_COPY | Confirmed novel. A per-segment copy_page_from_iter loop is required. |
| §10 "no slot" | It is -ENOSPC. Also add -ESHUTDOWN, terminal ENOSPC, nonterminal -ENOBUFS (not a failure) and short READ_FIXED. |
