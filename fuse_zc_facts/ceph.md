# Ceph code facts for zero-copy ceph-fuse READ (wf5)

Verified 2026-10-13 by reading the trees (no edits, no checkouts).

- **Ceph**: `~/ceph` @ `8db74a411f9` (branch `wip-mds-rust-map`, "mds-rust-map: add C++ MDS project map and feature catalog"). Every `src/...` path below is relative to this tree.
- **Kernel v9 contract**: `/mnt/workspace/ceph-client` @ `875cf36709ca` (branch `opaque-obj-v9-round2`): `Documentation/userspace-api/io_uring-opaque.rst` ("rst"), `include/uapi/linux/io_uring/opaque.h`, `io_uring/opaque.c`.
- **libfuse**: scratchpad `libfuse-src` @ `cb2e761` (meson version `3.19.0-rc0`).
- Supersedes `wf4/ceph.md`, which was an issue list. Its citations are refreshed here; where wf4 was off, it is noted as "(wf4: …)".

Notation: `file:N` is a line, `file:N-M` is a range. Quotes are short excerpts.

---

## 0. v9 kernel anchors used by the Ceph side

| Fact | Where |
|---|---|
| COLLECT first CQE: `res 0`, `F_MORE`, extra1 = stream token. "No bytes are consumed before this CQE can be posted." Offset 0 = first unread byte at attach. | opaque.c:1300-1303; rst:72-79 |
| Attach errors: `-EOPNOTSUPP` (not TCP stream) 1254-1255; `-EBUSY` if already owned, ULP (`sk_prot != sk_prot_creator`), repair, urgent data 1272-1276, or `sk_user_data` 1294-1297; `-ENOTCONN` unless ESTABLISHED/CLOSE_WAIT 1277-1280; `-ENOSPC` when there is no stream slot (initial `ret = -ENOSPC` 1252, loop break 1286-1287) or the CQ is full 1304-1307 | io_uring/opaque.c |
| Held collection: "While a stream holds undecided bytes, no further bytes are collected for it: new bytes stay on the TCP socket and TCP flow control applies until RECV_OBJECT or DISCARD decides the held range." Two exceptions: an INSPECT waiting past the collected bytes, and a decision that starts beyond them. Kernel predicate `io_opaque_collect_demand()` | rst:92-101; opaque.c:950-957 |
| "INSPECT returns bytes that are already collected; an application needing more re-INSPECTs at the end offset". At most 64 KiB, never WAITALL. Re-inspecting the same offset returns only the bytes already collected. | rst:101-103, 156-160, 203-208 |
| Pressure: nonterminal `-ENOBUFS` CQE with `F_MORE`; extra1 = token, extra2 = absolute first unread TCP offset; coalesced until that offset advances; collectors resume automatically. If it cannot be posted, collection ends with `ENOSPC`, the token stays, and there is no rearm, so the stream must be closed. | rst:81-86, 116-122; `io_opaque_budget_cqe` opaque.c:1329-1345, 1416-1424 |
| RECV_OBJECT success CQE: `res = len`, extra1 = handle, extra2 = `IORING_OPAQUE_OBJECT_F_FORWARD_ONLY` or 0. **v9 returns no crc.** | opaque.c:1521, 1562 (`io_req_set_res32(req, ret, 0, op->handle, object_flags)`); uapi opaque.h:77; rst:161-165, 196-200 |
| Lowmem / FORWARD_ONLY: advisory, meaning "should be forwarded and released promptly, with SEND_LAST or FREE". Automatic compaction skips these objects. | rst:417-451 |
| A recv on an owned socket returns `-EBUSY`, including readers that slept before ownership was taken. `tcp_poll` is unchanged (no `sock_rx_owned` in 532-637). | net/ipv4/tcp.c:2682-2683, 2724-2727 |
| Every token needs STREAM_CLOSE. The stream holds a reference to the socket *file* (`stream->file = get_file(op->req->file)`). | opaque.c:1268; rst:126-131 |
| Ring needs `DEFER_TASKRUN` and `CQE32` or `CQE_MIXED` | io_uring/opaque_register.c:71-72 |

---

## 1. Messenger

### 1.1 AsyncConnection (`src/msg/async/AsyncConnection.{h,cc}`)

**Members** (AsyncConnection.h): `cs` 180; `policy` 183; `outgoing_bl` 191; `open_write` 192; `write_lock` 194; `lock` 196; handlers 197-201; `recv_buf` 202; `recv_max_prefetch` 203; `recv_start`/`recv_end` 204-205; `register_time_events` 206; `last_active` 208; `state_offset` 225; `worker` 226; `center` 227; `protocol` 229; `readCallback`/`pendingReadLen`/`read_buffer` 232-234. States enum: 153-161.

**RX entry**
- `C_handle_read::do_request` calls `process()` (AsyncConnection.cc:74-82).
- `process()` 388-503:
  - takes `lock` (389) and sets `last_active` (390);
  - in STATE_CONNECTING it closes any old `cs` (415-418), connects (429), and adds `EVENT_READABLE` (435);
  - STATE_ACCEPTING adds `EVENT_READABLE` (470);
  - in STATE_CONNECTION_ESTABLISHED a pending read continues through `pendingReadLen` (482-494);
  - otherwise it calls `protocol->read_event()` (499).

**How bytes are read today (to be replaced on attached sockets)**
- `read(len, buf, cb)` 195-207 calls `read_until`. If bytes are still missing it stores `readCallback`/`pendingReadLen`.
- `read_until` 218-289:
  - Drains the prefetch first (232-245).
  - If the remainder exceeds `recv_max_prefetch`, it calls `read_bulk` straight into the caller's buffer (249-263).
  - Otherwise it calls `read_bulk(recv_buf+recv_end, recv_max_prefetch)` and memcpys (264-284), so it can read **past** the current frame into `recv_buf`.
- `read_bulk` 293-314: `cs.read`. `-EAGAIN` maps to 0. Any other error, or 0 (EOF), returns -1, and the caller faults.
- Prefetch size: `ms_tcp_prefetch_max_size` default **64_K** (src/common/options/global.yaml.in:1176-1181). `recv_buf = new char[2*recv_max_prefetch]` (148).

**TX**
- `write(bl, cb, more)` 316-327: claim into `outgoing_bl`, then `_try_send`.
- `_try_send` 331-371:
  - `ceph_assert(center->in_thread())` (340); `cs.send` (347);
  - adds `EVENT_WRITABLE` while data is queued (357-360);
  - deletes it and dispatches `write_callback_handler` once drained (362-368).
  - So **`EVENT_WRITABLE` is registered only while TX is backlogged.**
- `handle_write` 745-760 calls `protocol->write_event()` without `lock`. If SHUTTING_DOWN and drained, it calls stop and queue_reset.
- `handle_write_callback` 762-774 sets `last_active` (764).

**Timers**
- `wakeup_from` 828-834 calls `process()`.
- `tick` 836-884:
  - shutdown timeout calls `fault` (842-849);
  - connect timeout when `!is_connected()` calls `fault` (856-866). `ms_connection_ready_timeout` is 10 s (global.yaml.in:1324-1329).
  - idle timeout calls `fault` (870-880). `ms_connection_idle_timeout` is 900 s (global.yaml.in:1330-1335).
- **`last_active` is updated only in `process()` (390) and `handle_write_callback()` (764).**

**Fault injection**: `ms_inject_socket_failures` calls `cs.shutdown()` in `read_until` (223-228) and `_try_send` (333-338).

### 1.2 ProtocolV2 (`src/msg/async/ProtocolV2.{h,cc}`)

**States** (ProtocolV2.h:18-45), in order:
- handshake: NONE, START_CONNECT, BANNER_CONNECTING, HELLO_CONNECTING, AUTH_CONNECTING, AUTH_CONNECTING_SIGN, COMPRESSION_CONNECTING, SESSION_CONNECTING, SESSION_RECONNECTING;
- accept side: START_ACCEPT … SESSION_ACCEPTING;
- data: READY, THROTTLE_MESSAGE, THROTTLE_BYTES, THROTTLE_DISPATCH_QUEUE, THROTTLE_DONE, READ_MESSAGE_COMPLETE;
- other: STANDBY, WAIT, CLOSED.
- `_fault()` branches on the range `[START_CONNECT..SESSION_RECONNECTING]` (343, 389).

**Members** (ProtocolV2.h): `session_stream_handlers` 78 (null rx/tx in crc mode); `session_compression_handlers` 79; `state` 83; `can_write` 94; `out_queue` 103; `tx_frame_asm`/`rx_frame_asm` 113-114; `rx_preamble` 116; `rx_epilogue` 117; `rx_segments_data` 118 (`segment_bls_t`); `next_tag` 119; `pre_auth` 124-128.

**Continuation machinery**
- `Ct`, `CtFun`, `CtRxNode`, `CONTINUE`, `CONTINUATION_RUN` (src/msg/async/Protocol.h:21-96).
- `run_continuation` (ProtocolV2.cc:46-65) turns `buffer::error` and `MsgAuthError` into `_fault()`.
- `READ(L,C)` 69 and `READ_RXBUF(B,C)` 71 call `ProtocolV2::read` 815-842, which calls `connection->read(len, buf, cb)`.

**Other facts**
- `rx_frame_asm` is built with `with_data_crc = cct->_conf->ms_crc_data` (104-107).
- `is_connected()` returns `can_write` (126).
- `can_write` is set true only in `ready()` (1351-1357). It is cleared in `reset_session` (186), `stop` (204), `_fault` (352) and `reuse_connection` (2863).
- `read_event()` 493-518: START_CONNECT and START_ACCEPT run the banner; READY runs `read_frame`; THROTTLE_* resume their throttle.

**RX frame state machine (the part a CQE-driven opaque engine replaces)**

1. `read_frame` 1126-1138 clears `rx_preamble`, `rx_epilogue` and `rx_segments_data`, then `READ(get_preamble_onwire_len())`.
2. `handle_read_frame_preamble_main` 1140-1186:
   - `disassemble_preamble` (1155-1163);
   - MESSAGE requires `state == READY` (1175-1179), then sets `recv_stamp` and THROTTLE_MESSAGE (1180-1182);
   - control frames go straight to `read_frame_segment` (1184).
3. Throttles:
   - `throttle_message` 1600-1627 (`policy.throttler_messages->get_or_fail`);
   - `throttle_bytes` 1629-1660;
   - `throttle_dispatch_queue` 1662-1690 (`dispatch_throttler.get_or_fail(cur_msg_size)`).
   - On failure each arms `ms_client_throttle_retry_time_interval` through `wakeup_handler`, which leads back to `process()` and `read_event()`.
   - `get_current_msg_size()` is the sum of segment logical lengths except the header (293-301).
4. `read_frame_segment` 1228-1253 allocates `create_aligned(onwire_len, align)` (1241-1242), then `READ_RXBUF`.
   - `handle_read_frame_segment` 1255-1266 does `push_back(std::move(rx_buffer))`, the carriage-setting overload.
   - `_handle_read_frame_segment` 1268-1279 reads the epilogue after the last segment (1271-1275).
5. `handle_read_frame_epilogue_main` 1372-1384, then `_handle_read_frame_epilogue_main` 1386-1407:
   - `rx_frame_asm.disassemble_segments(...)` (1389) checks crcs and late_status;
   - FrameError or MsgAuthError leads to `_fault` (1390-1396);
   - **`!ok` (aborted) leads to `reset_throttle(); state = READY; CONTINUE(read_frame)` (1401-1405). The frame is dropped, not faulted.**
6. `handle_read_frame_dispatch` 1188-1226: control tags go to `handle_frame_payload` 1281-1336; MESSAGE goes to `handle_message`.

**Frames a client can receive after READY**
- MESSAGE.
- KEEPALIVE2: `handle_keepalive2` 1692-1723 appends KEEPALIVE2_ACK under `write_lock`.
- KEEPALIVE2_ACK: 1725-1740.
- ACK: 1742-1755.
- Every other tag faults on its state check.

**Every site that re-enters `read_frame` after READY**: `ready()` 1369; `handle_message` 1471, 1596; `handle_keepalive2` 1722; `handle_keepalive2_ack` 1739; `handle_message_ack` 1754; abort path 1404. Throttle wakeups also re-enter `read_frame_segment` through `read_event()`.

**Message decode and dispatch: `handle_message` 1409-1597**
- `ceph_assert(state == THROTTLE_DONE)` (1411).
- `MessageFrame::Decode(rx_segments_data)` (1414).
- Builds `ceph_msg_header` from header2, with tid at 1431 (1430-1442).
- `decode_message(cct, 0 /*crcflags*/, header, footer, front, middle, data, connection)` (1446-1450). A decode failure faults (1451-1454).
- seq check (1492-1509); `in_seq` (1528).
- `ms_fast_preprocess` runs under the lock (1557).
- Fast dispatch **drops `connection->lock`** (1571-1577). After re-locking, `if (state != READY) return nullptr;` (1580-1583).
- Slow path: `enqueue` (1584-1587).
- `decode_message` (src/msg/Message.cc:320-1034) checks crcs only when `crcflags` is set (334-381), and msgr2 passes 0. It then calls `m->set_payload`/`set_middle`/`set_data` (1010-1012) and `m->decode_payload()` (1015).
- `Message::set_data` is `data.share(bl)` (src/msg/Message.h:452-458); `claim_data` is `bl = std::move(data)` (462-466).
- MOSDOpReply path:
  - `DispatchQueue::fast_dispatch` calls `msgr->ms_fast_dispatch` (src/msg/DispatchQueue.cc:71-76);
  - Objecter accepts OPREPLY for fast dispatch (src/osdc/Objecter.h:2785-2793); `ms_fast_dispatch2` 2794-2796;
  - `ms_dispatch2` (Objecter.cc:1067-1075) calls `handle_osd_op_reply`.
  - **So reply processing runs on the messenger worker thread, without `AsyncConnection::lock`.**

**Handshake end**
- `handle_auth_done` 1876-1920: `auth_meta->con_mode` (1907); crypto handler pair, rev1-aware (1908-1910).
- `finish_client_auth` 1922-1929: COMPRESSION is negotiated if the peer supports it.
- `handle_compression_done` 2223-2241, then `start_session_connect` 1947-1959, then `send_client_ident` 1961-1998 (`CEPH_MSG_CONNECT_LOSSY` 1969-1971).
- rev1 is chosen at the banner: `set_is_rev1` on the tx and rx assemblers (1013-1016).
- **`handle_server_ident` 2154-2207**:
  - `state != SESSION_CONNECTING` faults (2159-2162);
  - address check (2178-2182);
  - `server_cookie` (2184); peer addrs, features and `peer_global_seq` (2186-2190);
  - **`connection->policy.lossy = server_ident.flags() & CEPH_MSG_CONNECT_LOSSY` (2192)**;
  - `queue_connect` + `ms_deliver_handle_fast_connect` (2203-2204);
  - **`return ready();` (2206)**.
- `handle_reconnect_ok` 2122-2152 also ends in `ready()`. That path is lossless reconnect only.
- **`ready()` 1338-1370**:
  - clears `reconnecting`/`replacing`;
  - re-arms the tick with `inactive_timeout_us` (1344-1349);
  - **sets `can_write = true` and dispatches `write_handler` if `out_queue` is non-empty (1351-1357)**;
  - `maybe_start_delay_thread` (1359); `state = READY` (1361); `INTERCEPT(15)` (1367);
  - **`return CONTINUE(read_frame);` (1369)**.
- **Client↔OSD compression is never negotiated.** The OSD answers with `comp_registry.get_mode(peer_type=CLIENT)` (ProtocolV2.cc:3105-3118), which is `COMP_NONE` for non-OSD peers (src/msg/compressor_registry.cc:122-127).
- ceph-fuse policies: default `lossy_client(0)`, MDS `lossless_client(0)` (src/ceph_fuse.cc:276-279; src/msg/Policy.h:84-89).

### 1.3 Teardown paths (STREAM_CLOSE hook points)

**Single funnel**: `AsyncConnection::shutdown_socket()` 656-668. It deletes time events, then, if `cs`: `delete_file_event(fd, READABLE|WRITABLE)`, `cs.shutdown()` (SHUT_RDWR), `cs.close()`. It runs on the connection's EventCenter thread (`delete_file_event` asserts `in_thread`, src/msg/async/Event.cc:303).

| Caller chain | Lines |
|---|---|
| `AsyncConnection::fault()` calls `shutdown_socket()`, resets the prefetch (`recv_start = recv_end = 0`) and clears `outgoing_bl` | AsyncConnection.cc:624-636 |
| `_stop()` (unregister_conn 641, release_worker 642, `STATE_CLOSED` 644) dispatches `C_clean_handler`, which calls `cleanup()` and then `shutdown_socket()` | 638-650; 102-110; 815-826 |
| `ProtocolV2::stop()` calls `reset_recv_state()`, `discard_out_queue()` and `connection->_stop()` | ProtocolV2.cc:189-206 |
| `ProtocolV2::_fault()`: a **lossy** connection outside `[START_CONNECT..SESSION_RECONNECTING]` gets `stop()` + `queue_reset` (342-348). Otherwise it calls `connection->fault()` (367) and reconnects (389-405) or backs off (406-429). | ProtocolV2.cc:334-432 |
| `mark_down()`: `lock` + `protocol->stop()`. Objecter calls it from `_reopen_session` (Objecter.cc:2010) and `close_session` (2026). | AsyncConnection.cc:738-743 |
| `stop(queue_reset)`. Called by AsyncMessenger `shutdown_connections` 1021-1051, `mark_down_addrs` 1053-1063, `mark_down_all` (AsyncMessenger.h:191-193) and `shutdown()` 483-499 | AsyncConnection.cc:776-782 |
| Graceful `shutdown()` calls `ProtocolV2::shutdown` (128-136), then the write handler, then stop when drained (751-758) or a tick-timeout fault (842-849) | AsyncConnection.cc:784-813 |
| Write-side faults: `write_event` 663-665, 744-746, 765-767; write-callback failure 871-878 | ProtocolV2.cc |
| Other closes: STATE_CONNECTING closes a stale `cs` (415-418; `shutdown_socket` already ran in `fault()`). Server-side `reuse_connection` calls `existing->shutdown_socket()` (2907) or `cs.close()` (2918-2921). `~ConnectedSocket` closes the fd (src/msg/async/Stack.h:93-96). | |

**Lossy client after READY**: every fault runs `stop()`, then `C_clean_handler`, then `cleanup()` and `shutdown_socket()`. `queue_reset` then reaches `Objecter::ms_handle_reset` (Objecter.cc:4944-4975), which calls `_reopen_session` (new connection, 2000-2017) and `_kick_requests` (resend in tid order, 2173-2223).

### 1.4 Locks
- `AsyncConnection::lock` is held across `process()`, `read_event()` and every RX continuation (AsyncConnection.cc:389). `write_event` runs under `write_lock` only (ProtocolV2.cc:656).
- Objecter calls `mark_down()` with `rwlock` held unique plus the session lock ("rwlock is locked unique / s->lock is locked", Objecter.cc:2002-2003, 2010; `ms_handle_reset` 4949-4966).
- `handle_osd_op_reply` takes `rwlock` shared (3855), then `con->get_priv()` (Connection::lock, 3862), then `s->lock` (3870).
- Dispatcher contract: fast-path hooks "may be called while the Messenger holds internal locks … any locks it takes must be at the very bottom of the hierarchy" (src/msg/Dispatcher.h:97-112).
- `Connection::lock` is a leaf `ceph::mutex` (src/msg/Connection.h:41). It is used by `set_priv`/`get_priv`/`clear_priv` (70-83) and the keepalive stamps (232-247), and is already taken under `AsyncConnection::lock` (`handle_keepalive2` calls `set_last_keepalive`, ProtocolV2.cc:1716).
- **Precedent for per-tid RX state on a Connection**:
  - `rx_buffers` (Connection.h:55-56); `post_rx_buffer`/`revoke_rx_buffer`, both `#if 0` (217-230);
  - users in Objecter, also `#if 0`: `_send_op` 3695-3711, `op_cancel` 2747-2753, `handle_osd_op_reply` 3986-3989;
  - `Op::con` "for rx buffer only" (Objecter.h:2001).

---

## 2. Event loop and worker threads

**EventCenter** (src/msg/async/Event.h:92-264)
- `MAX_EVENTCENTER = 24` (95).
- Entry points:
  - `init` 196; `set_owner` 197;
  - `create_file_event`/`create_time_event`/`delete_file_event`/`delete_time_event` 204-207;
  - `process_events` 208; `wakeup` 209;
  - `dispatch_event_external` 212; `in_thread` 213-215.
- `submit_to(i, f, always_async)` 247-263. With `always_async`, a heap `C_submit_event` deletes itself after running (218-244); otherwise it runs inline when `in_thread`, or blocks waiting.

**`EventCenter::init`** (Event.cc:123-204)
- Creates the `EpollDriver` (136-137) and the notify eventfd (169-176).
- **It runs on the thread that constructs the stack**: `NetworkStack::create` calls `w->center.init(5000, worker_id, t)` (src/msg/async/Stack.cc:97-104).

**`set_owner`** (Event.cc:235-251)
- Sets `owner = pthread_self()` and publishes the center in the per-cct `global_centers` (239-244).
- Registers the notify eventfd with `C_handle_notify` (245-249).
- `C_handle_notify` reads the eventfd (Event.cc:50-76). It is the template for an io_uring eventfd handler.

**File events**
- `create_file_event(fd, mask, cb)` (Event.cc:253-299) asserts `in_thread()` (255) and grows the table.
- `delete_file_event` (301-334) asserts `in_thread()` (303).
- **The epoll driver is edge-triggered** (`ee.events = EPOLLET`, src/msg/async/EventEpoll.cc:66, 92). An eventfd callback must therefore read the eventfd and reap the CQ until empty before returning.
- EPOLLERR and EPOLLHUP raise both READABLE and WRITABLE (EventEpoll.cc:136-137).

**`process_events`** (Event.cc:423-507)
- epoll_wait (453); file callbacks (455-479); timers (481-482); external events, swapped under `external_lock` (484-497); pollers when idle (499-502).
- It blocks only when there are no pollers and no external events (445). Pollers turn the loop into busy-polling, so the io_uring wakeup should use an eventfd, not a Poller.

**`dispatch_event_external`** (Event.cc:509-524)
- Pushes the `EventCallbackRef`, which **the center does not own**.
- Dedups only when the last queued pointer is the same callback (514-516).
- Wakes the center only on the first event and when not `in_thread` (520-521).
- `~EventCenter` runs leftover external events with `do_request(0)` (Event.cc:206-216).

**Worker thread entry: `NetworkStack::add_thread`** (Stack.cc:37-61)
- `rename_thread`;
- **`w->center.set_owner()` (42)**;
- **`w->initialize()` (44)**;
- `w->init_done()` (45);
- loop on `process_events(30 s)` (46-57);
- then `w->reset(); w->destroy();` (58-59).

**Worker** (Stack.h:240-354)
- `virtual void initialize() {}` (325) and `virtual void destroy() {}` (323).
- `PosixWorker::initialize()` is declared at src/msg/async/PosixStack.h:30 and is **empty** (src/msg/async/PosixStack.cc:271-273).
- Per-worker perf counters: `l_msgr_*` enum (Stack.h:208-238); builder in the constructor (259-307).

**NetworkStack**
- `create` (Stack.cc:63-107) builds `ms_async_op_threads` workers (default 3, max 24; global.yaml.in:1438-1449).
- `start` 113-132; `get_worker` (least references) 134-158; `stop` 160-170; `drain` 191-204.

**One stack per process**
- There is one stack per CephContext and transport: the `StackSingleton` named "AsyncMessenger::NetworkStack::posix" (src/msg/async/AsyncMessenger.cc:258-270, 381-386).
- Its threads start inside the **AsyncMessenger constructor** (`stack->start()` 385).
- ceph-fuse builds its only messenger at src/ceph_fuse.cc:276. That happens after config parsing and before `Client` init (302) and mount (318). MonClient and Client share it.
- Connection→worker assignment: `create_connect` takes `stack->get_worker()` (AsyncMessenger.cc:861-863).

**Socket fd access**
- `ConnectedSocket` (Stack.h:83-148): `fd()` 137-139; `shutdown()` 123-125; `close()` 131-134 resets `_csi`, so `operator bool` becomes false; the destructor closes (93-96).
- `PosixConnectedSocketImpl` (PosixStack.cc:41-218):
  - `read` is `::read` (67-76);
  - `send` is `sendmsg` over `pb->c_str()` iovecs (117-159, c_str at 132);
  - `shutdown` is `SHUT_RDWR` (204-206); `close` 207-209; `fd` 213-215.

---

## 3. frames_v2 (`src/msg/async/frames_v2.{h,cc}`)

**Structures and constants**
- `Tag` enum: frames_v2.h:46-69 (MESSAGE=17, KEEPALIVE2=18, KEEPALIVE2_ACK=19, ACK=20).
- `segment_t {le32 length; le16 alignment}` 71-80. `PAGE_SIZE_ALIGNMENT` 4096 (74).
- `SegmentIndex::Msg` HEADER=0, FRONT=1, MIDDLE=2, DATA=3 (82-93). `MAX_NUM_SEGMENTS = 4` (97).
- `preamble_block_t` 104-121: tag, num_segments, 4× segment_t, flags, reserved, le32 crc. That is **32 bytes**.
- `epilogue_crc_rev0_block_t {late_flags; crc_values[4]}` 125-128: 17 bytes.
- **`epilogue_crc_rev1_block_t {late_status; crc_values[3]}` 131-134: 13 bytes.** `crc_values[i-1]` is segment i, so **`crc_values[2]` is DATA**.
- Secure epilogues: 137-150.
- `FRAME_CRC_SIZE = 4` 152; `FRAME_PREAMBLE_INLINE_SIZE = 48` 153.
- late flags: `FRAME_LATE_FLAG_ABORTED` 168; `FRAME_LATE_STATUS_ABORTED 0x1` 176; `COMPLETE 0xe` 177; `ABORTED_MASK 0xf` 178.
- `FRAME_EARLY_DATA_COMPRESSED` (preamble flags) 188.
- `ceph_msg_header2` (src/include/msgr.h:207-223) is packed, **41 bytes**: seq, tid (offset 8), type, priority, version, data_pre_padding_len, data_off, ack_seq, flags, compat_version, reserved.

**FrameAssembler** (frames_v2.h:194-414)
- `set_is_rev1` 202-206; `get_is_rev1` 208-210; `get_num_segments` 212-215; `get_segment_logical_len` 217-220; `get_segment_align` 222-225.
- `get_preamble_onwire_len` 236-241: 32 in crc mode.
- `get_segment_onwire_len` 252-268: **rev1 crc mode makes segment 0 `logical_len + 4` (inline crc)**; the others are their logical length.
- `get_epilogue_onwire_len` 301-312: 0 for a single-segment rev1 frame, else 13 in rev1 crc mode.
- **Public API**: `disassemble_preamble` (321) and `disassemble_segments` (323-325).
- **Private** (section from 327): `disassemble_first_segment` / `disassemble_remaining_segments` (385-388). The comment at 357-384 describes exactly the split flow wanted here: read the first segment, look up user buffers by `header2::tid`, read the rest directly, then disassemble the remaining segments. Making those two methods public, or adding a wrapper, is required.
- Members: `m_descs` 408; `m_flags` 409; `m_is_rev1` 411; `m_with_data_crc` 412.

**Verification functions** (frames_v2.cc)
- **Preamble crc**: `ceph_crc32c(0, preamble, sizeof - 4)`, checked in `disassemble_preamble` 275-326 (crc 295-301). It also rejects `num_segments` outside [1,4] (304-308) and an empty last segment (309-312), fills `m_descs` (314-318), sets `m_flags` (320), and asserts that a compressed frame has an rx compressor (323).
- **Segment crc**: `check_segment_crc` = `segment_bl.crc32c(-1)` compared with expected, else `FrameError` (45-52).
- **late_status**: `check_epilogue_late_status` 56-63. A low nibble other than ABORTED or COMPLETE throws; it returns `== COMPLETE`.
- **rev1 segment 0 inline crc**: `disasm_first_crc_rev1` 361-377. It decodes the trailing 4-byte crc, splices it off (367-370), and checks it only `if (m_with_data_crc)` (371-373).
- `disasm_remaining_crc_rev1` 379-392 asserts `segment_bls[i].length() == logical_len` (386), checks `crc_values[i-1]` if `m_with_data_crc` (387-389), and returns the late_status result (391).
- Dispatchers: `disassemble_segments` 437-448 (first, then remaining, then decompress); `disassemble_first_segment` 450-462; `disassemble_remaining_segments` 464-482.
- TX for reference: `asm_crc_rev1` 147-175 sets `late_status |= COMPLETE` and writes crc = `m_with_data_crc ? crc32c(-1) : 0`. `calc_num_segments` drops trailing empty segments (34-43).
- **crc variant**: `ceph_crc32c` is raw reflected CRC32C with no pre/post inversion. This was verified against `src/test/common/test_crc32c.cc:23-24` (`ceph_crc32c(0,"foo bar baz") = 4119623852`) with an independent bitwise implementation. So `crc32c(-1, "123456789") = 0x1CF96D7C`, which matches the Linux `crc32c(~0, …)` convention.
- Seeding a NULL buffer is fast: `ceph_crc32c(crc, NULL, len)` maps to `ceph_crc32c_zeros` for len > 16 (src/include/crc32c.h:43-51; O(log len) table method, src/common/crc32c.cc:241-262).
- `ms_crc_data` (global.yaml.in:1198-1203) gates both TX crc emission and RX checking.

**Frame classes**: `Frame<>` 416-439; `ControlFrame` 441-517 (`Decode` 512); `KeepAliveFrame` 765; `KeepAliveFrameAck` 781; `AckFrame` 793; `segment_bls_t` = `static_vector<bufferlist,4>` 805-806; `MessageFrame` 811-875 (alignments 8, 8, 8, 4096 at 812-816; `Decode` 834-842; `header()` reads `hdrbl.c_str()` at 844-847).

**Typical rev1 crc-mode MOSDOpReply on the wire**:

```
[preamble 32][header2 41][crc 4][front F][middle 0][data D][epilogue 13 = late_status, crc(front), crc(middle), crc(data)]
```

When D = 0, `num_segments` is 2, but the epilogue is still 13 bytes.

---

## 4. Buffers (`src/include/buffer.h`, `src/include/buffer_raw.h`, `src/common/buffer.cc`)

### 4.1 raw and its subclasses

**`buffer::raw`** (buffer_raw.h:30-117)
- `bptr_storage` 34-36; `data` 38; `len` 39; `nref` (atomic) 41; `mempool` 42;
- crc cache `last_crc_offset` / `last_crc_val` 44-45 under `crc_spinlock` 47.
- Constructors: `raw(unsigned l, int mempool)` leaves `data = nullptr` (49-52); `raw(char*, l, mempool)` 53-56. Both adjust mempool counts by `len`.
- `virtual ~raw()` 57-60. **This is the only virtual method.**
- **`get_data()` is non-virtual: `return data;` (91-93)** (wf4: 87-89).
- Crc cache API, all non-virtual:
  - **`get_crc`** 97-105 hits only when `last_crc_offset == fromto` (an exact range);
  - **`set_crc`** 106-111 is a single-entry overwrite;
  - **`invalidate_crc`** 112-116.

**Subclasses** (all in buffer.cc): `raw_combined` 94-146; `raw_zeros` (mprotect read-only, used by `append_zero2`) 148-179; `raw_malloc` 181-202; `raw_posix_aligned` 205-228; `raw_hack_aligned` 232-250; `raw_claimed_char` 256-268; `raw_static` 270-276; **`raw_claim_buffer` (takes a custom `deleter`) 278-284**. Forward declarations: buffer.h:125-134. Factories: buffer.cc:286-363; `claim_buffer` 320-324.

**Adding `raw_opaque`**
- Subclass `raw` with `raw(len, mempool_buffer_opaque)`, which leaves `data = nullptr`.
- Override the destructor for the FREE batch.
- Add an opaque marker flag, since `get_data()` cannot be overridden.
- Mempool list: src/include/mempool.h:154-184 (add `f(buffer_opaque)`).
- Diagnostics precedent: `ptr::is_zero_fast()` identifies a raw type with `dynamic_cast` (buffer.cc:572-575).

### 4.2 ptr (buffer.h:167-351; buffer.cc)

- Constructors share by reference: `ptr(const ptr&, o, l)` buffer.cc:397-404; ptr from `unique_leakable_ptr<raw>` 365-372.
- `release()` (459-486) deletes the raw on its last reference, on whichever thread drops it.
- `raw_nref()` (542) exposes the reference count.
- `ptr_node::create(unique_leakable_ptr<raw>)` is not hypercombined ("currently disabled": buffer.cc:2277-2303).

**Byte-touching ptr members (every one must be guarded or kept unreachable)**

| Member | Line |
|---|---|
| `c_str()` const / non-const | buffer.cc:506-513 |
| `end_c_str()` | 514-521 |
| `operator[]` | 527-538 |
| `raw_c_str()` | 540 |
| `copy_out` | 544-550 |
| `cmp` (memcmp of `c_str`) | 557-570 |
| `is_zero` (`mem_is_zero(c_str())`) | 577-580 |
| `append(char)` | 582-590 |
| `append(const char*, l)` | 592-600 |
| `append_zeros` | 602-611 |
| `copy_in` (invalidates crc) | 613-622 |
| `zero()` / `zero(o,l)` (invalidate crc) | 624-639 |
| `ptr::iterator_impl` constructor | buffer.h:184-191 |
| **Address-only** `is_aligned` (`(uintptr_t)c_str()`) | buffer.h:269-271 |
| **`operator<<(ostream, ptr)` prints `(void*)bp.c_str()` and `raw_c_str()`** | buffer.cc:2317-2327 |

The last row means a `ceph_assert` inside `c_str()` would abort any `ldout << bl` that touches an opaque ptr. Special-case it there.

- `unused_tail_length()` = `raw len − (off+len)` (523-526), so **a substr ptr into an opaque raw has a non-zero tail**.

### 4.3 list (bufferlist)

**Carriage rules** (buffer.h)
- Constructors start with `_carriage = &always_empty_bptr` (947-966). A copy resets the carriage (980-988); a move carries it (968-974, 989-996). `clear()` resets it (1048-1053).
- `push_back(const ptr&)` leaves the carriage alone (1054-1060).
- `push_back(ptr&&)` sets the carriage to `always_empty` (1061-1068).
- **`push_back(unique_ptr<ptr_node>)` sets the carriage to the node (1072-1077)**. ProtocolV2 uses this overload (1149, 1264).
- **`push_back(unique_leakable_ptr<raw>)` sets the carriage to the node (1079-1084)**.
- `share()` 1111-1121; deprecated `claim(list&)` is `*this = std::move(bl)` (1102-1104); `get_append_buffer_unused_tail_length()` is the carriage's tail (1007-1009).

**By-reference operations (safe for opaque raws)**

| Operation | Line |
|---|---|
| `claim_append` (splice; destination carriage unchanged) | buffer.cc:1294-1303 |
| `append(const ptr&)` → `push_back` | 1417-1420 |
| **`append(const ptr&, off, len)`** (extends the tail ptr when it is the same raw and contiguous) | 1427-1443 |
| `append(const list&)` | 1445-1452 |
| `substr_of` (starts with `clear()`, so carriage = always_empty) | 1580-1617 |
| `splice(off, len, claim_by)` (by reference; special-cases `curbuf == _carriage`) | 1620-1693 |
| `iterator::copy(len, list&)` → `dest.append(*p, p_off, howmuch)` | 804-821 |
| `iterator::get_current_ptr` | 731-737 |
| `copy_shallow` (shares when contiguous, **else copies**) | 785-802 |
| `append_zero2` (shared `raw_zeros` refs) | 1494-1504 |

**Byte-touching list operations**

| Operation | Line |
|---|---|
| `iterator::operator*` | 713-719 |
| `iterator::copy(len, char*)` | 750-766 |
| `copy_deep` / `copy(len, ptr&)` | 768-784 |
| `copy(len, string&)` | 823-841 |
| `copy_all` | 843-858 |
| `get_ptr_and_advance` | 860-879 |
| `iterator::crc32c` | 881-894 |
| `iterator::copy_in` | 911-945 |
| `contents_equal` | 957-1020 |
| `is_provided_buffer` | 1022-1028 |
| `is_aligned` / `is_aligned_size_and_memory` (address only) | 1030-1060 |
| `is_zero` | 1062-1069 |
| `zero()` / `zero(o,l)` | 1071-1107 |
| `rebuild()` / `rebuild(nb)` (copy via `node.c_str()`, then `invalidate_crc`) | 1172-1206 |
| `rebuild_aligned_size_and_memory` | 1213-1276 |
| `append(char)` / `append(const char*,len)` / `append_hole` / `append_zero` (write into the **carriage** tail) | 1305-1325, 1359-1384, 1465-1483, 1506-1528 |
| `obtain_contiguous_space` | 1386-1415 |
| `operator[]` | 1534-1547 |
| **`c_str()`** (rebuild when not contiguous) | 1552-1567 |
| `to_str` | 1569-1578 |
| `write` | 1695- |
| base64 | 1706-1722 |
| `write_fd` / `send_fd` | 2017-2070 |
| `prepare_iovs` | 2073-2098 |
| `write_stream` | 2161-2168 |
| `hexdump` | 2171- |
| `prepare_iov` | buffer.h:1215-1229 |
| `operator==` / `operator<` | buffer.h:1273-1305 |
| `copy_bufferlist_to_iovec` | buffer.h:1318-1333 |

- `is_contiguous()` is `_num <= 1` and touches no bytes (buffer.cc:1109-1112).
- `get_wasted_space` uses raw lengths only (1146-1170).

**`bufferlist::crc32c(crc)`** (buffer.cc:2100-2147)
- For each node it looks up the raw cache with `(node.offset(), node.offset()+node.length())`.
- On a hit with the same seed it uses the cached value (2112-2115).
- On a hit with a different seed: **`crc = ccrc.second ^ ceph_crc32c(ccrc.first ^ crc, NULL, node.length())`** (2117-2125).
- **On a miss it computes over `node.c_str()` and calls `set_crc` (2128-2133).**
- `list::invalidate_crc` 2149-2156.

---

## 5. MOSDOpReply decode and delivery of read data

**MOSDOpReply** (src/messages/MOSDOpReply.h:38-357)
- `HEAD_VERSION 8`, `COMPAT 2` (40-41).
- `claim_op_out_data` 105-110; `claim_ops` (swap) 111-114.
- `encode_payload` 177-233: `merge_osd_op_vector_out_data(ops, data)` (180), so DATA is the concatenation of each op's outdata in op order.
- **`decode_payload` 234-329**, v8 (239-264): oid, pgid, flags (i64), result, bad_replay_version, osdmap_epoch, `num_ops`, `ops[i].op` (251), `retry_attempt`, rvals (254-255), **`OSDOp::split_osd_op_vector_out_data(ops, data)` (257)**, then replay/user version, redirect, trace. v2-7: 281-327 (split only when version ≥ 4, 300-305).
- `print` 333-352 shows only `ops`, and OSDOp printing shows only outdata **length** (src/osd/osd_types.cc:7484-7583).
- `ceph_osd_op` (src/include/rados.h:600-684) is packed; **`payload_len` is the last field (683)**.
- **`split_osd_op_vector_out_data`** (osd_types.cc:7588-7596): `datap.copy(ops[i].op.payload_len, ops[i].outdata)` for ops with `payload_len > 0`, by reference. `merge_…` is at 7598-7606.
- Decoding the front alone is impossible today: `decode_payload` calls `split` against `data`, and the iterator copy throws `end_of_buffer` when data is short (buffer.cc:810-811). A front-only peek that returns `payload_len[]` is needed.

**Objecter delivery** (src/osdc/Objecter.cc)
- `handle_osd_op_reply` 3848-4028:
  - tid from header (3853); `rwlock` shared (3855);
  - `con->get_priv()` must equal `s->con` (3861-3868);
  - **`s->ops.find(tid)` under `s->lock` (3870-3881)**; a miss is logged as "stray" and dropped;
  - retry-writes path (3893-3905);
  - **stale attempt ignored when `retry_attempt != attempts-1` (3907-3917)**;
  - redirect resubmits (3926-3944); EAGAIN resubmits (3955-3973).
- **Data**:
  - when `op->outbl` is set and `outbl->length() == bl.length() && bl.get_num_buffers() <= 1`, it **copies into the caller's pre-sized buffer** with `t.invalidate_crc(); bl.begin().copy(len, t.c_str())` (3991-4003);
  - otherwise **`m->claim_data(*op->outbl)`** (4005).
- Then `claim_ops` (4012) and `process_op_reply_handlers` (3771-3844): `**pb = p->outdata` (3802; a share), handler `(*ph)(e, rval, outdata)` (3818).
- `complete_op_reply` 4030-4067 serializes completions with a per-object completion lock.
- Client buffers are always empty when passed in (`&onfinish->bl`, `&tbl`), so the copy branch at 3991-4003 is not reached for ceph-fuse. Assert it.
- Multi-extent reads: `C_SGRead` (Objecter.h:4023-4039) calls `_sg_read_finish` (Objecter.cc:4902-4931). That uses `Striper::StripedReadResult::add_partial_result` (splice; src/osdc/Striper.cc:388-420) and `assemble_result(bl, zero_tail)`, which does `claim_append` plus `append_zero` (462-485). Everything is by reference. `assemble_result(char*)` copies (487-521); Client does not use it.

---

## 6. Objecter (`src/osdc/Objecter.{h,cc}`)

**Structures**
- `ObjectOperation` (Objecter.h:91-…): `out_bl`/`out_handler`/`out_rval`/`out_ec` 96-103; `add_op` 169-181; `add_data` 182-187.
- **`read(off, len, pbl, prval, ctx)` 501-509**: `add_data(CEPH_OSD_OP_READ…)`, `out_bl[p] = pbl`.
- `get_internal_versions` 1511-1517 sets the second op's `out_bl`.
- **`Objecter::Op`** (1995-2170):
  - `session` 1996; `target` (op_target_t, flags) 1999; **`con` "for rx buffer only" 2001**; `features` 2002; `ops` 2004;
  - **`outbl` 2010**; `out_bl`/`out_handler`/`out_rval`/`out_ec` 2011-2018; `onfinish` 2032-2033; `timeout_deadline` 2040;
  - **`tid` 2042; `split_op_tids` 2043; `attempts` 2044**;
  - `budget` 2053; `should_resend` 2056; `ctx_budgeted` 2062;
  - constructors size `out_*` (2099-2140); `pass_thru_op` 2146-2164.
  - A zero-copy opt-in flag and its admission reservation belong here, as client-local fields.
- **`OSDSession`** (2498-2531): `ops` map tid→Op 2500; `lock` (std::shared_mutex) 2514; `incarnation` 2516; **`con` 2517**. `rwlock` is at 1828.

**Flags**
- Message flags `CEPH_OSD_FLAG_*` (src/include/rados.h:456-492): `EC_DIRECT_READ` 487; `FAIL_ON_EAGAIN` 488; `FORCE_OSD` 489; `CEPH_OSD_FLAGS_DIRECT_READ` 493.
- Per-op `CEPH_OSD_OP_FLAG_*` (rados.h:495-510): FADVISE_* 0x4-0x40; `WITH_REFERENCE` 0x80; `SCRUB` 0x100. These go on the wire.
- `get_read_flags` adds `global_op_flags | extra_read_flags | READ` (Objecter.h:3005-3013; `global_op_flags` 1738).
- **A zero-copy marker must not be a wire bit.**

**Read entry points**
- `prepare_read_op` 3142-3164 (`o->outbl = pbl`, or `out_bl[0]` for a single op 3155-3157); `read` 3165-3180 and 3194-3219.
- `read(oid, oloc, off, len, …, op_flags)` 3425-3435.
- **`read_trunc` 3468-3488**: a single `CEPH_OSD_OP_READ` op with `op.flags = op_flags` (3481) and **`o->outbl = pbl` (3484)**, then `op_submit`.
- `sg_read_trunc` 4041-4060 issues one `read_trunc` per extent and gathers them through `C_SGRead`.
- `Filer::read_trunc` (src/osdc/Filer.cc:64-81) calls `objecter->sg_read_trunc(…, op_flags)`.

**Submit**
- `op_submit` 2435-2444 (`rwlock` shared), then `_op_submit_with_budget` 2474-2515:
  - **`_take_op_budget` (2487-2494)**;
  - `timeout_deadline` (2503-2505);
  - **`SplitOp::create(op, *this, sul, cct)` (2508)**, then `_op_submit` (2513).
- `_op_submit` 2609-2731: `_calc_target`, `_get_session`, `_send_op_account` (2673); tid is assigned under `s->lock` (2690-2692); `_session_op_assign` (2700); **`_send_op` (2713-2715)**.
- **`_send_op` 3648-3719** (rwlock + session lock held, 3650-3651):
  - backoff check (3653-3675); `_prepare_osd_op` (3678; MOSDOp tid = `op->tid` at 3608);
  - **`ConnectionRef con = op->session->con` (3692)**; `#if 0` rx_buffer block (3695-3711);
  - `op->incarnation` (3713); `con->send_message(m)` (3718).

**All `_send_op` callers**: linger ping 783; osdmap resend 1438; `_kick_requests` 2198; `_op_submit` 2714; backoff unblock 4145. `_op_submit` re-entries: `op_post_split_op_complete` EAGAIN 2430; retry-writes 3902; redirect 3941; EAGAIN 3970; SplitOp sub-ops (src/osdc/SplitOp.cc:1106). **`_send_op` is the single send choke point.**

**Removal points**
- `_session_op_remove` 3432-3447 erases from `s->ops`. It is called by `_finish_op` (3575-3577), `close_session` (2046), redirect (3930), EAGAIN (3959), retry-writes (3899), split EAGAIN (2426) and osdmap re-targeting.
- `op_cancel(s, tid…)` (2733-2786) calls `_finish_op` (2782). For split parents it cancels the child tids (2756-2774).
- `_finish_op` 3561-3587 puts the budget back (3567-3570).
- **Hooking tid removal in `_session_op_remove` covers all of these.**

**Throttles**
- `_take_op_budget` (Objecter.h:2693-2704) calls `_throttle_op` (blocking) **only when `keep_balanced_budget`**, else `op_throttle_bytes.take()` / `op_throttle_ops.take(1)`.
- `Throttle::take` never blocks (src/common/Throttle.cc:138-152).
- `op_throttle_bytes`/`ops` are sized by `objecter_inflight_op_bytes` (100_M) / `objecter_inflight_ops` (1_K) (Objecter.h:2712-2717; global.yaml.in:2478-2489).
- `set_balanced_budget` (2757) has callers only in librados (src/librados/RadosClient.cc:267), neorados (src/neorados/RADOSImpl.cc:51) and a test. **The CephFS client is not throttled.**
- `calc_op_budget` 3721-3737; `_throttle_op` 3739-3764.

**Reset**: `ms_handle_reset` 4944-4975 (`rwlock` unique, session lock, `_reopen_session`, `_kick_requests`). `_reopen_session` 2000-2017 (`set_priv(NULL)`, `mark_down`, `connect_to_osd`, `incarnation++`); `close_session` 2019-…; `_get_session` creates sessions with `connect_to_osd` (1943-1978).

**SplitOp** (src/osdc/SplitOp.cc)
- `create` 965-1117 rejects snapshots (980-983), pools without `FLAG_CLIENT_SPLIT_READS` (986-989) and pools without non-primary reads (992-995).
- The single-op case calls `prepare_single_op` (899-928). That is an **EC direct read**: the original op gets `EC_DIRECT_READ|FORCE_OSD` and a shard target, with no extra ops.
- For a split, the parent is added to `splitop_session` (1068; Objecter.cc:2463-2472) and `protect_torn_reads` (629-640) **appends GET_INTERNAL_VERSIONS to every sub-read** (1070).
- Sub-ops are built with `prepare_read_op` (1088-1090) and submitted with **`objecter._op_submit` directly (1106)**, which skips `op_submit` and the budget.
- `version_mismatch` decodes `internal_version->bl` (ECSplitOp 439-455; Replica 488-495).
- `complete` 526-620:
  - `assemble_buffer_read` (EC 133-162 and replica 312-…) uses `substr_of` + `append`, by reference;
  - if `orig_op->outbl` is set: when the lengths are equal it **copies** (588-601), else it **rebuilds** if `outbl->length() != 0` (606-608), then `substr_of` (609).

---

## 7. ObjectCacher (`src/osdc/ObjectCacher.{h,cc}`)

**Types** (ObjectCacher.h)
- `OSDRead {extents, snap, bl*, fadvise_flags}` 70-77.
- **BufferHead** 108-239: states 111-117 (MISSING, CLEAN, ZERO, DIRTY, RX, TX, ERROR); `ex` 123-125; `dontneed`/`nocache` 126-127; **`bl` 131**; `last_read_tid` 133; `waitfor_read` 139.
- `Object` 242-386: `data` map 260; `reads` 268; `split`/`merge_left`/`try_merge_bh`/`maybe_rebuild_buffer` 355-359; `map_read` 363-367.
- **`ObjectSet {parent (Inode*), ino, truncate_seq/size, poolid, objects}` 389-406.**
- Fields: `lock` (= `client_lock`) 416; `max_size` 418; `waitfor_read` 430; `bh_lru_dirty`/`bh_lru_rest` 435; `ob_lru` 436; stats 470-479.
- `touch_bh` 493-502; `bottouch_ob` 506-508; `bh_read` 543-544.
- **`trim()` 550, in the private section that runs from 411 to `public:` at 577.** The `public:` at 443 belongs to the nested FlusherThread.
- `_readx` 572-574; `bh_read_finish` 579-582 (public).
- `readx` 625-627.
- **`set_max_size()` 686-688 does not trim.**
- `file_read` 708-715; **`file_read_ex(…, std::vector<ObjHole> *holes, …)` already exists (717-725)** and is used by `Client::_read_async`.
- **`operator<<(BufferHead)` prints `" firstbyte=" << (int)bh.bl[0]` (766)**. That calls `bufferlist::operator[]` and then `ptr::operator[]`, which reads byte 0 of the raw. ObjectCacher.cc prints `*bh` at about 40 sites: 119, 171-172, 181, 213, 230, 342-392, 814 (level 7), 940, 1002, 1339, 1598, 1635, …. The `objectcacher` subsystem gathers levels 0-5 by default (src/common/subsys.h:44), so this triggers at `debug_objectcacher >= 7`.

**Functions** (ObjectCacher.cc)
- `C_ReadFinish` 50-83 (`bl` 62; `finish` calls `bh_read_finish`); `C_RetryRead` 85-113.
- `split` 115-174 (`substr_of` 148-154); `merge_left` 177-214 (`claim_append` 193); `try_merge_bh` 227-255 (skips rx; calls `maybe_rebuild_buffer` 254).
- **`maybe_rebuild_buffer` 257-267** rebuilds when `wasted*2 > len && wasted > 4096`. Callers: 254, 489, 496, 500, 510, 589, 635, 643.
- `map_read` 317-402: complete objects give ZERO hits.
- `map_write` 442-…: a write replaces the BH data (`bh->bl.swap(frag)` / `claim_append` 1813-1815 in `writex`), so dirty BHs never hold read data.
- `truncate` 575-607; `discard` 609-676 (an rx BH is cleared and marked zero, 653-661).
- **`bh_read` 810-838**: asserts `lock` (813); `mark_rx`; `last_read_tid`; **`writeback_handler.read(oid, objno, oloc, start, len, snap, &onfinish->bl, trunc_size, trunc_seq, op_flags, trace, onfinish)` (831-835)**.
- **`bh_read_finish` 840-1016**:
  - pads a short read with `bl.append_zero` (855-860), which gives **real zero bytes, not `raw_zeros`**;
  - ENOENT handling 870-924;
  - **`bh->bl.substr_of(bl, bh->start()-start, bh->length())` + `mark_clean` (996-999)**;
  - logs `*bh` (1002); `try_merge_bh` (1004);
  - `finish_contexts` (1011); `retry_waiting_reads` (1012).
- **`trim()` 1323-1366**: clean bytes over `max_size`, or clean BH count over `max_size >> 12`, expire `bh_lru_rest`; objects over `max_objects`. **Only callers: end of `_readx` (1731) and `writex` (1849).**
- **`_readx` 1422-1734**:
  - `dontneed`/`nocache` from `fadvise_flags` (1434-1435);
  - **rx back-pressure: `!waitfor_read.empty() || (stat_rx > 0 && rx_bytes > max_size)` parks a `C_RetryRead` (1535-1551)**;
  - `bh_read(bh, rd->fadvise_flags)` (1554); last-bh waiter (1561-1567); rx waiters (1571-1583);
  - hits: bottouch for nocache/dontneed (1603-1614);
  - assembly: a **ZERO BH becomes `append_zero` + `holes->push_back({opos, len})` with object offsets (1643-1647)**, else `substr_of` + `claim_append` (1649-1652);
  - result `claim_append` (1705-1716); readahead without a buffer (1718-1722); `trim()` (1731).
- `release` 2412-2452; `bh_add` 2795-2814 (dontneed inserted at the LRU bottom); `bh_remove` 2816-2835.
- Perf counters: `perf_start` 725-758 (enum ObjectCacher.h:27-53).

**WritebackHandler and ObjecterWriteback**
- `WritebackHandler::read(…, int op_flags, trace, onfinish)` (src/osdc/WritebackHandler.h:17-21).
- **`ObjecterWriteback::read`** (src/client/ObjecterWriteback.h:18-28):
  - calls `m_objecter->read_trunc(oid, oloc, off, len, snapid, pbl, 0, trunc_size, trunc_seq, new C_OnFinisher(new C_Lock(m_lock, onfinish), m_finisher))`;
  - **drops `op_flags` and passes message flags 0**;
  - completion runs on the `objecter_finisher` thread under `client_lock`.
- The inode is reachable without new arguments: `bh->ob->oset->parent`.

**Readahead**
- Uses `Readahead` (src/common/Readahead.h:21-…; `update` 59; `set_*` 81-109).
- `Client::do_readahead` (src/client/Client.cc:11914-11934) calls `objectcacher->file_read(&in->oset, &in->layout, in->snapid, off, len, NULL /*bl*/, 0, C_Readahead)`.
- Limits are set in `_create_fh` (11021-11035): min `client_readahead_min` 128_K; max = min(`client_readahead_max_bytes` (0 = unlimited), period × `client_readahead_max_periods` (4)), which is **16 MiB** for 4 MiB objects (src/common/options/mds-client.yaml.in:56-87).

---

## 8. Client (`src/client/Client.{h,cc}`, `Inode.h`, `Fh.h`)

**Setup**
- `ObjecterWriteback(objecter, &objecter_finisher, &client_lock)` and `ObjectCacher(…, client_lock, …, client_oc_size, max_objects, max_dirty, target_dirty, max_dirty_age, true)` (Client.cc:462-472).
- `filer` (655). Config observer keys 19040-19056 (sorted); `handle_conf_change` 19061-… (`client_oc_size` calls `set_max_size` 19082-19084).

**`_read(Fh*, off, size, bl, onfinish=nullptr, read_for_write=false)`** (declared Client.h:2166-2167; defined Client.cc:11679-11892)
- Asserts `client_lock` (11682).
- Fetches inline state when `inline_version == 0` (11718-11725).
- `get_caps(FILE_RD, want FILE_CACHE[|LAZYIO])` (11727-11738); O_DIRECT strips the cache caps (11739-11740).
- **Inline data when `in->inline_version < CEPH_INLINE_NONE` (11742-11763)** (`CEPH_INLINE_NONE` src/include/ceph_fs.h:820).
- **CASE 1** (`!client_debug_force_sync_read && client_oc && have & (FILE_CACHE|LAZYIO)`) calls `_read_async` (11790-11816).
- CASE 2 (non-blocking sync) uses `C_Read_Sync_NonBlocking` (11817-11837).
- CASE 3 calls `_read_sync`, with the checkeof loop (11838-11867).

**Callers of `_read`**: `Client::read` 11503 (then **`bl.begin().copy(len, buf)` after `lock.unlock()` 11505-11507**); `_preadv_pwritev_locked` 12317 (then **`copy_bufferlist_to_iovec` after unlocking 12324-12326**); `WriteEncMgr::read` 12556 (fscrypt RMW, `read_for_write`); `ll_read` 17307.

**`_read_async`** (11956-12088)
- `fscrypt->prepare_data_read(…, &fscrypt_denc)` (11976-11982); `get_cap_ref(FILE_CACHE)` (11989).
- **`objectcacher->file_read_ex(&in->oset, &in->layout, in->snapid, read_start, read_len, bl, 0 /*flags*/, &holes, io_finish)` (12039-12040)**.
- Blocking wait drops `client_lock` (12057-12065); decrypt (12067-12076); `do_readahead` (12085).
- The async finisher `C_Read_Async_Finisher::finish` decrypts and runs readahead (11936-11954).

**`_read_sync`** (12090-12209)
- **`filer->read_trunc(in->ino, &in->layout, in->snapid, pos, left, &tbl, 0, trunc_size, trunc_seq, &onfinish)` with `client_lock` held (12180-12183)**, then unlock, wait, relock (12184-12186).
- `wait_and_copy` does `claim_append` (12145) and pads short reads with **real zeroed memory** (`ptr_node::create(some)` + `zero()`, 12155-12157).
- fscrypt decrypt (12193-12204).

**`ll_read`** (17275-17311): clamps the length (17282-17294); **`std::scoped_lock lock(client_lock)` across `_read` (17295-17307)**. `ll_open` 17074-…; `_create_fh` 11008-11038.

**Inode fields** (src/client/Inode.h)
- `layout` 152; `truncate_seq`/`size` 154-155;
- **`inline_version` 174, `inline_data` 175;**
- **`fscrypt_auth` 177, `fscrypt_file` 178, `fscrypt_ctx` 183, `fscrypt_key_validator` 184;**
- `effective_size()` 186; **`is_fscrypt_enabled()` = `!!fscrypt_auth.size()` (195-197)**;
- **`oset` 264**.

**Fh** (src/client/Fh.h:14-62): `inode` 15; `flags` 16; `pos` 25; `mode` 26; `readahead` 31.

**Perf counters**: enum `l_c_*` (Client.h:91-108); registered in `Client::_finish_init` (Client.cc:677-700, `PerfCountersBuilder plb(cct, "client", l_c_first, l_c_last)`).

---

## 9. ceph-fuse (`src/client/fuse_ll.cc`, `src/ceph_fuse.cc`)

**CephFuse::Handle** (fuse_ll.cc:166-203)
- `se` 184; `opts` (`fuse_cmdline_opts`) 186; `conn_opts` 187;
- `fuse_req_key` (thread-specific current request, used by `switch_interrupt_cb` and `umask_cb`) 198-200;
- `fuse_ll_req_prepare` 327-332.

**`fuse_ll_open`** (842-876)
- Calls `client->ll_open(in, fi->flags, &fh, perms)` (857); `fi->fh = fh` (859).
- **`fuse_disable_pagecache` sets `direct_io`; otherwise `fuse_use_invalidate_cb` (default true) sets `keep_cache = 1` (861-868)**. Then `fuse_reply_open` (870).
- `fuse_ll_create` sets the same flags (1292-1302).

**`fuse_ll_read`** (878-911)
- `ll_read(fh, off, size, &bl)` (884).
- **`if (bl.get_num_buffers() > IOV_MAX) bl.rebuild()` (890-891); `bl.prepare_iov(&iov)` (893)**.
- `fuse_reply_data(req, bufv, FUSE_BUF_SPLICE_MOVE)` (903), with `fuse_reply_iov` as fallback (907-908).

**Interrupts and locks**
- `fuse_ll_interrupt` 1366-1370; `switch_interrupt_cb` 1372-1381 (`fuse_req_interrupt_func`).
- EDEADLK guards on `!fuse_multithreaded`: `fuse_ll_setlk` 1355-1360, `fuse_ll_flock` 1391-1396.
- Client side: `_do_filelock` (Client.cc:13742) calls `switch_interrupt_cb` (13790-13796).

**`do_init`** (1466-1511)
- `fuse_apply_conn_info_opts` (1472).
- **Sets `conn->want |= FUSE_CAP_SPLICE_MOVE`, `IOCTL_DIR`, `DONT_MASK`, `EXPORT_SUPPORT` through the legacy 32-bit `want` (1477-1490).** 64-bit caps such as `FUSE_CAP_IO_URING_BUFPOOL` (bit 34) need `fuse_set_feature_flag` / `want_ext` (libfuse include/fuse_common.h:521, 552, 1170).

**Ops table**
- `fuse_ll_oper` (1513-1565), positional GNU initializers.
- `init: do_init` 1514; `open` 1528; `read` 1529; `write` 1530; `release` 1532; `create` 1544; `flock` 1560.

**`init()`** (1609-1741)
- Builds argv: `-f`; `-o allow_other`/`default_permissions`/`max_write`/`atomic_o_trunc`; **`-o splice_read`/`splice_write`/`splice_move` from `fuse_splice_*` (1673-1684)**; `-d`.
- `fuse_parse_cmdline` (1697); `fuse_parse_conn_info_opts` (1709).
- Registers callbacks: `ino_invalidate_cb` only if `fuse_use_invalidate_cb`, `dentry_invalidate_cb`, `switch_interrupt_cb`, `remount_cb`, `umask_cb` (1721-1738).
- **This argv is where `-o io_uring` and the new options would be added.**

**`start()`** (1743-1796): `fuse_session_new(&args, &fuse_ll_oper, sizeof, this)` (1760), `fuse_set_signal_handlers` (1781), `fuse_session_mount` (1787).

**`loop()`** (1798-1833)
- If `fuse_multithreaded`, with FUSE ≥ 3.12: `fuse_loop_cfg_create`, `set_clone_fd(opts.clone_fd)`, `set_idle_threads(opts.max_idle_threads)`, `set_max_threads(opts.max_threads)`, `fuse_session_loop_mt(se, conf)` (1803-1816).
- Older versions use 1817-1829. Single-threaded is `fuse_session_loop` (1831).

**`main`** (src/ceph_fuse.cc): MonClient (266); **messenger (276)**; policies (277-279); `StandaloneClient` (281); `cfuse->init` (288); `messenger->start` (295); `client->init` (302); `mount` (318); `cfuse->start` (329); tester thread (336-337); **`cfuse->loop()` (338)**.

**libfuse build requirements**
- `option(WITH_FUSE … ON)` + **`find_package(FUSE REQUIRED)` with no version floor** (CMakeLists.txt:259-263).
- `cmake/modules/FindFUSE.cmake:1-81`: prefers `fuse3` through pkg-config (13-14, 22-41); otherwise parses `FUSE_{MAJOR,MINOR}_VERSION` from `fuse_common.h` (52-57); defines the `FUSE::FUSE` target (71-80).
- Version macros feed `config-h.in.cmake:100,103` (`CEPH_FUSE_MAJOR/MINOR_VERSION`).
- **`FUSE_USE_VERSION 312`** (src/include/ceph_fuse.h:23-25). `FUSE_VERSION` is redefined from pkg-config (36-39).
- ceph-fuse target: src/CMakeLists.txt:1011-1022.
- libfuse added "fuse-over-io-uring communication" in **3.18.0** (libfuse ChangeLog.rst:48-54). Bufpool is unreleased (3.19 dev). The session option is `-o io_uring` (lib/fuse_lowlevel.c:4364). In io_uring mode INTERRUPT is refused (4209).

---

## 10. Configuration and perf counters

**Option files** (src/common/options/)
- `global.yaml.in` holds the `ms_*` options (48-1628) and `objecter_*` (2465-2512).
- `mds-client.yaml.in` holds `client_*` and `fuse_*` (services `mds_client`):
  - `client_readahead_*` 56-87; `client_oc*` 164-229; `client_debug_force_sync_read` 238-245;
  - `fuse_*` 340-460: `fuse_use_invalidate_cb` 340, `fuse_disable_pagecache` 347, `fuse_splice_read/write/move` 378-398, `fuse_multithreaded` 433-439, `fuse_set_user_groups` 454-460.

Example entry:

```yaml
- name: ms_tcp_prefetch_max_size      # global.yaml.in:1176-1181
  type: size
  level: advanced
  desc: Maximum amount of data to prefetch out of the socket receive buffer
  default: 64_K
  with_legacy: true
```

- Runtime and startup flags appear as `flags: [runtime]` (`client_oc_size` mds-client.yaml.in:180-181) and `flags: [startup]` (`fuse_default_permissions` 376-377).
- New options need no `with_legacy`. Read them with `cct->_conf.get_val<T>("name")` (fuse_ll.cc:1627-1646).

**Perf counter registration patterns**

| Owner | Enum | Builder |
|---|---|---|
| Objecter | Objecter.cc:112-197 (`l_osdc_first = 123200`) | `Objecter::init` 283-420 (`PerfCountersBuilder pcb(cct, "objecter", …)` 288; create/add 419-420) |
| Client | Client.h:91-108 (`l_c_first = 20000`) | `_finish_init` Client.cc:677-700 |
| ObjectCacher | ObjectCacher.h:27-53 | `perf_start` ObjectCacher.cc:725-758 |
| Messenger worker | Stack.h:208-238 (`l_msgr_first = 94000`; labeled 231-238) | `Worker` constructor Stack.h:259-307 |

A new opaque logger (e.g. `"opaque"`) should get its own enum range and builder, owned by `OpaqueStore` and registered once.

---

## 11. Worker-ready pointers (Ceph side)

1. **Ring and store bootstrap**
   - Worker threads start inside the first AsyncMessenger constructor (AsyncMessenger.cc:385, called from ceph_fuse.cc:276). Config is already parsed then.
   - Create the `OpaqueStore` as a CephContext singleton (pattern: `cct->lookup_or_create_singleton_object<T>(name, true, …)`, AsyncMessenger.cc:381-382 and Event.cc:240-242).
   - Create each `OpaqueRing` in `PosixWorker::initialize()` (PosixStack.cc:271-273; runs on the worker after `set_owner`, Stack.cc:42-44). Alternatively create it lazily on the worker with `center.submit_to(id, f, true)`.
   - Register the ring eventfd with `create_file_event(efd, EVENT_READABLE, cb)` on that thread. EPOLLET means the callback must drain completely.
   - Tear down in `Worker::destroy()` (Stack.h:323; called at Stack.cc:59).
2. **Attach point**: replace `return ready();` at ProtocolV2.cc:2206. Preconditions, all evaluated after line 2192:
   - `connection->peer_is_osd()` (Connection.h:188) and `connection->policy.lossy`;
   - `auth_meta->con_mode == CEPH_CON_MODE_CRC` and `!session_stream_handlers.rx`;
   - `rx_frame_asm.get_is_rev1()`; `!session_compression_handlers.rx`;
   - `cct->_conf->ms_crc_data`;
   - **`connection->recv_start == connection->recv_end`** and `!connection->pendingReadLen` (otherwise prefetched bytes would be lost from the stream);
   - then `delete_file_event(cs.fd(), EVENT_READABLE)` and submit COLLECT.
   - On the token CQE, run `ready()` minus `CONTINUE(read_frame)`. On failure, re-add `EVENT_READABLE` and `CONTINUE(ready)`.
   - **Place the new attach state outside `[START_CONNECT..SESSION_RECONNECTING]`**, or `_fault()` takes the reconnect branch (343, 389) instead of `stop()`.
3. **Neutralise the userspace read path on attached connections.** `read_event()` (493-518) still runs on `wakeup_from` and `dispatch_event_external(read_handler)`. In state READY it calls `read_frame`, a userspace read, `-EBUSY`, `read_bulk -1`, and a fault. Route READY and THROTTLE_* to the opaque engine. Cover every `CONTINUE(read_frame)` site listed in §1.2.
4. **Keep `last_active` fresh.** It is updated only in `process()` and `handle_write_callback()` (AsyncConnection.cc:390, 764). The CQE handler must set `connection->last_active = now` (and `recv_start_time`), or the idle tick faults busy connections after 900 s (870-880).
5. **CQE ownership.** The opaque CQE handler runs on the worker thread and must take `connection->lock`, as `process()` does. CQE `user_data` must reference a refcounted `OpaqueStream`, not the AsyncConnection: `_stop` and `cleanup` free the handlers asynchronously (638-650, 815-826) while collector and STREAM_CLOSE CQEs are still in flight.
6. **Framing engine.** INSPECT (≤64 KiB) the preamble (32), header (41+4), front and middle, copying into user bufferlists, and **DISCARD each copied range promptly**. While undecided bytes are held, nothing new is collected.
   - Verify the preamble with `rx_frame_asm.disassemble_preamble`.
   - Verify the header crc through a public wrapper around `disasm_first_crc_rev1`.
   - Read the tid from header2 offset 8 and require type == `CEPH_MSG_OSD_OPREPLY` (43, src/include/ceph_fs.h:179).
   - Parse the front for `ops[i].op.payload_len` with a front-only peek; `decode_payload` can't be reused, see §5.
   - Build the DATA bufferlist as `[INSPECTed bytes][opaque raw ptrs, each covering exactly its object][INSPECTed tail]` using `push_back(ptr&&)` or `append(ptr,off,len)`. Then call `disassemble_remaining_segments` and `handle_message`.
   - Throttle states must still be honoured. Pausing is safe because held collection backpressures TCP.
7. **crc**: seed each raw with `set_crc({off, off+len}, {seed, crc})`. `bufferlist::crc32c(-1)` then combines without touching bytes (buffer.cc:2117-2125). **v9 has no kernel crc (§0)**, so this step depends on the kernel crc change.
8. **Per-connection opt-in set.** Add `std::set<ceph_tid_t>` (or a flat set) plus a leaf mutex to `Connection`, in place of the dead `rx_buffers`. Insert in `_send_op` (Objecter.cc:3692, `op->session->con`). Erase in `_session_op_remove` (3432-3447, `from->con`). Treat it as a hint, because a stale attempt is ignored at 3907-3917.
9. **Admission.** Put it in `_op_submit_with_budget` (2474-2515), not in `_op_submit` (re-entered on redirect and EAGAIN). Copy the flag and reservation onto SplitOp sub-ops at SplitOp.cc:1088-1106; they bypass `op_submit`. For SplitOp sub-reads, receive only the READ op's range as objects; GET_INTERNAL_VERSIONS stays INSPECTed bytes.
10. **ObjectCacher**:
    - pass the zero-copy intent through `WritebackHandler::read`. The inode is `bh->ob->oset->parent`. `ObjecterWriteback::read` must not leak any private bit into the wire `op.flags`.
    - skip `maybe_rebuild_buffer` for opaque raws (257-267).
    - make `operator<<(BufferHead)` (ObjectCacher.h:766) opaque-safe.
    - add a public opaque trim (`trim()` is private, 550).
    - consider making the short-read padding `append_zero2` (`raw_zeros`, detectable with `is_zero_fast`) so it becomes ZERO pieces (bh_read_finish 859).
    - `holes` positions are **object offsets** (1643-1647). Generate ZERO pieces during `_readx` assembly, where the buffer offset is known.
11. **Copy consumers that must materialize, or never receive opaque raws**:
    - `Client::read` 11505-11507;
    - `_preadv_pwritev_locked` 12324-12326;
    - `fuse_ll_read` 890-893 (copy path for non-zero-copy opens of an inode whose ObjectCacher holds opaque BHs, because ObjectCacher is per inode, `in->oset`);
    - fscrypt `decrypt_bl` 11941, 12070, 12197 (excluded by eligibility);
    - `Objecter` / `SplitOp` pre-sized `outbl` copies (Objecter.cc:3991-4003; SplitOp.cc:588-608), unreachable with empty client buffers; assert it;
    - `PosixConnectedSocketImpl::send` (`c_str` per ptr, PosixStack.cc:132): never send opaque raws.
12. **FORWARD_ONLY** (v9). Keep the flag on `raw_opaque`. Never stage FORWARD_ONLY objects in ObjectCacher as readahead: deliver them to a waiting FUSE READ or FREE them. If they must enter a BH, mark it dontneed (`bh_add` inserts at the bottom, 2804-2805).
13. **Pressure CQE** (v9 `-ENOBUFS`, nonterminal). Post a Client-thread event (Finisher or tick) that takes `client_lock` and runs the opaque trim. Never take `client_lock` on the messenger worker; Objecter `mark_down` lock order is in §1.4.

---

## 12. Where fuse_zc.md §6, §7 and §9.4 are wrong, stale or underspecified

| # | fuse_zc.md | Problem | Evidence |
|---|---|---|---|
| E1 | §6.3, §9.1.2: "RECV_OBJECT returns the crc of each chunk"; crc in the low 32 bits of extra2 | **Stale.** In v9, extra2 carries `IORING_OPAQUE_OBJECT_F_FORWARD_ONLY`; there is no crc. The proposed layout collides with the flags word. Re-specify (for example flags in the low bits and crc in the high 32 bits, or a separate mechanism) before P4. | opaque.c:1521, 1562; opaque.h:77 |
| E2 | §6.2: "INSPECT returns the bytes that are available … inspect the next missing offset" | **Underspecified for v9.** INSPECT returns only collected bytes. While undecided bytes are held, nothing else is collected; re-INSPECT at the end offset. The engine must DISCARD each copied chunk promptly. A 10 MiB MOSDMap front (or any >64 KiB segment) is an INSPECT/copy/DISCARD loop; otherwise undecided bytes are charged by whole allocation and held. | rst:92-103, 203-208, 374-382 |
| E3 | §6.1 attach-failure handling; §5.2 rule 3; §9.1.3 per-stream reserve | **Missing in v9.** There is no per-stream receive reserve. v9 has `reply_reserve` (send framing only), watermarks/lowmem, held collection, and a nonterminal `-ENOBUFS` pressure CQE that the design never handles. v9 attach errors are `-EBUSY`/`-ENOTCONN`/`-ENOSPC`/`-EOPNOTSUPP`, not "-ENOBUFS reserve". | rst:54-61, 81-86; opaque.c:1252-1307, 1329-1345 |
| E4 | §6.1: "Attach also fails on ULPs … (opaque.c:1138-1160)" | Stale line numbers (v4). v9: 1254-1255, 1272-1280, 1294-1297, 1304-1307. | |
| E5 | §6.1 Steps 1: "Remove EVENT_READABLE … keep EVENT_WRITABLE" | Imprecise. `EVENT_WRITABLE` is registered only while TX is backlogged, so there is nothing to "keep". Also `read_event()` still runs `read_frame` on any later `process()` (time-event wakeups, `dispatch_event_external(read_handler)`). Removing the epoll interest is not enough; the READY/THROTTLE read entry points must be rerouted. | AsyncConnection.cc:357-368, 828-834; ProtocolV2.cc:493-518 + the `read_frame` sites in §1.2 |
| E6 | §6.1 (new attach state) | **Underspecified.** `_fault()` behaviour depends on the state range. A lossy fault in `[START_CONNECT..SESSION_RECONNECTING]` reconnects instead of stopping, so the attach state's position in the enum matters. `ready()` side effects (tick re-arm, `can_write`, write kick, delay thread) must run on the token CQE. | ProtocolV2.cc:342-348, 389-405, 1338-1370 |
| E7 | §6.1 Preconditions: "prefetch buffer must be empty … because OSDs send nothing" | Make it a runtime check: `recv_start == recv_end && !pendingReadLen`. `read_until` prefetches up to 64 KiB past the frame. | AsyncConnection.cc:232-284; global.yaml.in:1176-1181 |
| E8 | §6 / §9.4 (missing) | **`last_active` starvation.** With RX off the userspace path, `process()` no longer runs, so `tick()` idle-faults busy connections after `ms_connection_idle_timeout` (900 s). The CQE path must update `last_active`. | AsyncConnection.cc:390, 764, 870-880 |
| E9 | §6.1 Teardown: "shutdown_socket/_stop, replacement, terminal CQE" | Mostly right. `_stop` reaches `shutdown_socket` only through `C_clean_handler` (async), so `shutdown_socket()` is the single funnel. Replacement is server-side only, irrelevant to ceph-fuse. **Unstated:** the AsyncConnection may be freed before the collector and STREAM_CLOSE CQEs arrive, so `user_data` must reference a refcounted stream object. `cs.close()` does not release the socket (the stream holds the file). `cs.shutdown()` (SHUT_RDWR) wakes the collector, but EOF can be deferred behind held bytes (rst:103-106; opaque.c:1397-1399). Only STREAM_CLOSE reliably releases the claim, the file reference and the undecided bytes. | AsyncConnection.cc:638-668, 815-826; opaque.c:1268 |
| E10 | §6.2 Data: "compute … from the front's per-op payload_len" | Right, but `MOSDOpReply::decode_payload` cannot be reused on a front alone: it splits DATA inline and the iterator copy throws on short data. A front-only peek helper is needed, and header2 type and version checks (v8 vs v2-7 layouts). | MOSDOpReply.h:234-329; buffer.cc:810-811 |
| E11 | §6.3 / §10: "late_status abort → FREE the objects and fault the connection, as msgr2 does today" | **Wrong.** msgr2 drops an aborted frame and continues (`!ok` → reset_throttle, READY, `read_frame`). Only a malformed late_status throws FrameError and faults. | ProtocolV2.cc:1398-1405; frames_v2.cc:56-63 |
| E12 | §6.3 (missing) | `FrameAssembler::disassemble_first_segment` and `disassemble_remaining_segments` are **private**, and the split-read flow the design needs is exactly the one described in the comment at frames_v2.h:357-384. They must be made public or wrapped. The data segment bufferlist must equal `logical_len` exactly (asserted). Respect `m_with_data_crc` (`ms_crc_data`). | frames_v2.h:321-325, 385-388; frames_v2.cc:371-373, 386-389 |
| E13 | §6.4 removal sites: "_finish_op, _session_op_remove and op_cancel" | Redundant. `_finish_op` and `op_cancel` both go through `_session_op_remove`. Hooking that one function also covers `close_session`, redirect, EAGAIN, retry-writes, split-EAGAIN and osdmap re-target. Insert site: `_send_op` (single choke point: callers 783, 1438, 2198, 2714, 4145). The lookup should also check message type (OPREPLY) and treat the result as a hint (stale attempts are ignored). | Objecter.cc:3432-3447, 3561-3587, 2733-2786, 3907-3917 |
| E14 | §7.1 Guards list (`c_str`, `raw_c_str`, `copy_out`, `append`, `zero`) | **Incomplete.** Also `end_c_str`, `operator[]`, `cmp`, `is_zero`, `append_zeros`, `copy_in`, the ptr iterator constructor, `is_aligned` (address only), and **`operator<<(ptr)`, which calls `c_str()`/`raw_c_str()`**. Asserting in `c_str()` would abort any debug print of an opaque bufferlist. The list-level byte users are in §4.3. Mempool: add `f(buffer_opaque)` at mempool.h:154-184. | §4.2 |
| E15 | §7.1 "Byte consumers" | **Missing: `operator<<(BufferHead)` reads `bh.bl[0]` ("firstbyte")**, printed at about 40 ObjectCacher log sites (level 7 and up). Also missing: `fuse_ll_read`'s `rebuild`/`prepare_iov`, `Client::read`, `_preadv_pwritev_locked`, and `PosixConnectedSocketImpl::send`. | ObjectCacher.h:766; fuse_ll.cc:890-893; Client.cc:11507, 12325; PosixStack.cc:132 |
| E16 | §7.1 Destructor "batches a FREE … through EventCenter::dispatch_event_external" | Underspecified. `dispatch_event_external` takes a non-owned callback pointer and dedups only consecutive identical pointers. Use one persistent per-worker callback plus a locked queue, or `submit_to(id, f, true)`. The destructor runs on any thread (Finisher, FUSE workers, the worker itself) and possibly after workers stop (`~EventCenter` drains external events). Handle a dead store. | Event.cc:206-216, 509-524; Event.h:247-263; buffer.cc:459-486 |
| E17 | §7.1 "materialize them with READ_OBJECT … after client_lock is dropped" | **Ring unspecified.** The copying callers (FUSE `/dev/fuse` loop threads, libcephfs callers) own no ring, and `SINGLE_ISSUER` rings accept submits only from their owner. Hop to a worker with `submit_to` and wait, or use a dedicated materialization ring or thread. | Event.h:247-263 |
| E18 | §7.2 "WritebackHandler::read gains a zero-copy flag. Today ObjecterWriteback::read … passes op_flags = 0" | Slightly wrong. It **drops `op_flags` entirely** and passes message flags 0. The inode is already reachable as `bh->ob->oset->parent`. `ObjectCacher::bh_read` passes `rd->fadvise_flags` as `op_flags`, and Client always passes 0. | ObjecterWriteback.h:18-28; ObjectCacher.cc:831-835, 1554; ObjectCacher.h:389-406; Client.cc:12039-12040 |
| E19 | §7.3 "A new file_read_ex(..., handoff)" | **Name collision.** `file_read_ex(…, holes, onfinish)` already exists and `_read_async` uses it. Also, `holes` are object offsets, not buffer offsets. | ObjectCacher.h:717-725, Client.cc:12039; ObjectCacher.cc:1643-1647 |
| E20 | §7.3 (missing) | Short-read padding in `bh_read_finish` and `_read_sync` creates **real zero bytes**, so those become MEM pieces, not ZERO pieces, unless they are switched to `append_zero2`/`raw_zeros`. | ObjectCacher.cc:855-860; Client.cc:12155-12157 |
| E21 | §9.4 "OpaqueRing … created in Worker::initialize()" | Feasible but underspecified. The NetworkStack is a per-CephContext singleton started in the AsyncMessenger constructor (ceph_fuse.cc:276), so the store must exist or be created lazily by the first worker. `PosixWorker::initialize()` is the override point (empty today). Teardown goes in `Worker::destroy()`. The epoll driver is EPOLLET. | AsyncMessenger.cc:258-270, 381-386; Stack.cc:37-61; PosixStack.cc:271-273; EventEpoll.cc:66 |
| E22 | §9.4 "admission at op_submit, SplitOp propagation" | SplitOp sub-ops bypass `op_submit` (`_op_submit` at SplitOp.cc:1106) and `_op_submit` is re-entered on redirect and EAGAIN. Admit once in `_op_submit_with_budget`, propagate explicitly in `SplitOp::create`, and treat an EC direct read (`prepare_single_op`) as an ordinary single op. | Objecter.cc:2474-2515, 3941, 3970; SplitOp.cc:899-928, 1088-1106 |
| E23 | §9.4 ProtocolV2 changes (attach, DATA decision, crc, late_status) | Underspecified. Also needed: KEEPALIVE2 / KEEPALIVE2_ACK / ACK frames on the opaque path, the three throttle states with timer resume, the fast-dispatch unlock and `state != READY` re-check after dispatch, and `reset_recv_state()` resetting the engine. | ProtocolV2.cc:1188-1226, 1571-1583, 1600-1690, 1692-1755, 262-291 |
| E24 | §8.1 / §9.4 ceph-fuse (libfuse requirement) | Not stated. The build has no libfuse version floor (`find_package(FUSE REQUIRED)`) and `FUSE_USE_VERSION 312`. io_uring needs libfuse ≥ 3.18; bufpool and the new APIs are unreleased. Gate with `FUSE_VERSION`/CMake checks. `do_init` sets caps through the legacy 32-bit `conn->want`; 64-bit caps need `fuse_set_feature_flag`. | CMakeLists.txt:259-263; ceph_fuse.h:23-25; fuse_ll.cc:1477-1490 |

**Verified correct as stated**
- msgr2 data crc is `crc32c(-1)` with no final xor, in `epilogue.crc_values[2]` (frames_v2.h:131-134; frames_v2.cc:385-389; `ceph_crc32c` is raw).
- The re-seed math (buffer.cc:2117-2125).
- `get_data()` is non-virtual.
- The `trim()` privacy claim.
- `objecter_inflight_op_bytes` does not bound Client.
- Client↔OSD compression is never negotiated.
- `recv` returns `-EBUSY` while `tcp_poll` is unchanged (v9).
- `SINGLE_ISSUER`/`DEFER_TASKRUN` ring-on-owner placement (`EventCenter::init` runs on the creating thread; `set_owner`/`initialize` run on the worker).
- The §6.4 lock-inversion rationale (Objecter.cc:2002-2010, 3855-3870; AsyncConnection.cc:389, 738-742; Dispatcher.h:97-112).

**UNVERIFIED** (not checked in this pass)
- The libfuse claims beyond the cited lines.
- Whether any other libcephfs consumer outside src/client reads ceph-fuse ObjectCacher buffers. ceph-fuse links only the client library, so this should not arise.
