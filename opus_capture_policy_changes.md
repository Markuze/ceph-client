# OPAQUE_OBJ: capture-policy change log (implementation guide)

**Base:** `upstream/opaque-obj-v7` at `e6aef56bc0f6`, five patches on `af32da41b032`.
All line numbers below refer to that commit.

**Goal:** make the collector follow the owner's capture policy. Today, undecided bytes
are either collected without limit (default config) or copied (`capture_window` config).
Both violate the policy.

This document is the complete specification. Follow it literally. Where it says "do not
change", leave that code alone even if it looks related. If something here seems wrong
or impossible, stop and report it instead of improvising.

**How this guide was checked.** I implemented every change in §3 and every test change in
§4 as a private prototype on `e6aef56bc0f6`, and ran it under UML with 2 vCPUs, KASAN,
lockdep and PREEMPT:

- the code snippets below are the ones that ran;
- the reference tests in the appendices fail on v7 where stated and pass on the
  prototype;
- the full edited suite result: KUnit 27/27 (including `opaque_collect_gate`),
  selftests 93/93 (including all 10,000 SMP SEND races, 10,000 arrival/drain cycles and
  4 GiB of DISCARD), and no kernel diagnostics.

The prototype isn't part of this document; implement from the text.

---

## 1. The policy (normative)

Terms:

- **collect:** take bytes off the TCP socket with `tcp_read_sock()`. In this code that is
  `io_opaque_recv()` calling `io_opaque_actor()`.
- **undecided byte:** a collected byte that no RECV_OBJECT or DISCARD covers. The byte
  count is `stream->undecided`, and the bytes live on `stream->extents`.
- **decided byte:** a byte covered by a RECV_OBJECT or DISCARD range on
  `stream->decisions`.
- **`stream->next`:** the absolute stream offset of the first byte still on the socket.

Rules:

1. **R1. Collecting what has arrived is allowed.** When collection is permitted, one
   collection pass may take everything queued on the socket. One pass is bounded by the
   existing `IO_OPAQUE_RX_BATCH` (256 KiB) and by store quota.
2. **R2. Undecided bytes are not copied.** They stay in the RX buffers they arrived in,
   retained exactly as the default (no-window) configuration retains them today.
   - The only copy of undecided bytes is INSPECT copying the requested header bytes to
     userspace.
   - The existing *safety* copy fallback stays: shared or cloned skbs, a locked skb
     head, shared frags, and compound pages larger than the capture limit are still
     copied. Only the policy copy (the `capture_window` "dense lookahead") is removed.
3. **R3. While a stream has undecided bytes, nothing more is collected for it.** New bytes
   stay on the socket, so TCP flow control applies to that connection. This holds until
   the application decides with DISCARD or RECV_OBJECT, unless R4 applies.
4. **R4. Demand exceptions.** Collection of undecided bytes is also permitted while:
   - (a) an INSPECT is waiting because no collected bytes exist at its offset. Without
     this, a header split across two arrivals deadlocks: the application can't parse it,
     so it can't decide anything.
   - (b) the earliest pending decision starts beyond `stream->next`, i.e. the application
     declared a range further in the stream. The gap before it is collected as undecided,
     which keeps today's ability to declare a body before deciding its header.

The collection gate:

```
collect_undecided = (stream->undecided == 0)                      /* R1/R3 */
                 || (an INSPECT is waiting on stream->reads)       /* R4a  */
                 || (earliest decision offset > stream->next)      /* R4b  */
```

- **Evaluate the gate once per collection pass,** at the start of `io_opaque_recv()`, not
  per chunk. That way a permitted pass takes the whole arrival (R1), and the next pass is
  held (R3).
- **Decided bytes are always collected,** gate or no gate.

These rules replace the `capture_window` mechanism entirely.

---

## 2. Current v7 behavior (evidence)

UML, 2 vCPUs, KASAN and lockdep. "Socket" is `ioctl(SIOCINQ)` on the TCP socket.
Values are bytes.

| Case | Scenario | v7 now | Required |
|------|----------|--------|----------|
| P, window 64 KiB | 16 KiB arrives; INSPECT 128 B; nothing decided | socket 0, undecided 16384, **copied 16384** | copied 0 |
| P, no window | same | socket 0, undecided 16384, copied 0, backing 32768 | same (retained) |
| G | as P, then a **second** 16 KiB arrives; then DISCARD the first 16 KiB | second arrival **collected at once**: socket 0, undecided 32768 | second arrival stays on socket (16384) until the DISCARD; then socket 0, undecided 16384, copied 0 |
| S | 100-byte header part arrives; INSPECT(0); second 100-byte part arrives; INSPECT(100) | second part collected at once | second part stays on socket (100) until INSPECT(100) waits; then INSPECT(100) = 100 |
| N1 | two connections, 256 KiB store; flooding connection A; FREE an object; framed reply on B; **default config** | B's replies `-ENOBUFS`: A recaptures the freed space | B's replies succeed: A is held by R3 |

A private prototype of exactly the changes in §3 produced the "Required" column for all
five rows, with no kernel diagnostics.

---

## 3. Code changes

### 3.1 `io_uring/opaque_internal.h`: add the per-pass gate flag

In `struct io_opaque_stream`, after `bool closed;`, add:

```c
	/* Collection pass may not take undecided bytes off the socket. */
	bool hold_undecided;
```

- **The polarity matters.** Zero means "may collect", so zero-initialized streams in
  KUnit fixtures keep today's behavior.
- Do not name it `collect_undecided` with the inverted meaning.

### 3.2 `io_uring/opaque.c`: the gate predicate

Add this function immediately **before** `io_opaque_capture_keep()` (line 899). It needs
`io_opaque_next_decision()`, which is defined just above it. It is `VISIBLE_IF_KUNIT`
because the KUnit test in §4.1 calls it.

```c
/* Undecided bytes are collected only when nothing is pending or a request needs later bytes. */
VISIBLE_IF_KUNIT bool io_opaque_collect_demand(struct io_opaque_stream *stream)
{
	struct io_opaque_req *op = io_opaque_next_decision(stream);

	lockdep_assert_held(&stream->lock);
	return !stream->undecided || !list_empty(&stream->reads) ||
	       (op && op->offset > stream->next);
}
```

Declare it in the `#if IS_ENABLED(CONFIG_KUNIT)` block at the end of
`io_uring/opaque_internal.h`, right after the `io_opaque_actor()` declaration:

```c
bool io_opaque_collect_demand(struct io_opaque_stream *stream);
```

Why `!list_empty(&stream->reads)` means "an INSPECT is waiting":

- `stream->reads` holds only INSPECT requests that are still waiting.
- `io_opaque_stream_process()` removes an INSPECT as soon as bytes are available or it
  fails.
- So every entry is waiting for bytes at or beyond `stream->next`.

### 3.3 `io_uring/opaque.c`: set the flag once per pass, in `io_opaque_recv()`

In `io_opaque_recv()` (starts at line 1295), find:

```c
	desc.arg.data = stream;
	old = set_active_memcg(op->store->memcg);
```

Change it to:

```c
	desc.arg.data = stream;
	stream->hold_undecided = !io_opaque_collect_demand(stream);
	old = set_active_memcg(op->store->memcg);
```

`stream->lock` is held here, which satisfies the lockdep assertion. Change nothing else
in `io_opaque_recv()`. In particular:

- the `-ENOBUFS` and `-ENOMEM` handling, the pressure CQE (`io_opaque_budget_cqe()`), the
  budget wait list, the KEEP fail-fast and `IOU_REQUEUE` all stay as they are;
- a held pass is **not** an error: `tcp_read_sock()` simply returns fewer bytes, and the
  function ends in its existing `IOU_RETRY` branch.

### 3.4 `io_uring/opaque.c`: the actor, `io_opaque_actor()` (line 1090)

Replace this block (lines 1103-1126):

```c
		if (op) {
			if (stream->next < op->offset) {
				chunk = min_t(u64, chunk, op->offset - stream->next);
			} else {
				chunk = min_t(u64, chunk, op->offset + op->length - stream->next);
				if (op->op == IORING_OPAQUE_DISCARD) {
					op->progress += chunk;
					ret = chunk;
					goto consumed;
				}
			}
		}
		if (stream->store->config.capture_window && !io_opaque_capture_keep(stream)) {
			u32 window = stream->store->config.capture_window;

			if (stream->undecided >= window) {
				/* Only a range decision can reopen this stream's window. */
				stream->need_bytes = U64_MAX;
				ret = -ENOBUFS;
				desc->error = ret;
				break;
			}
			chunk = min_t(u64, chunk, window - stream->undecided);
		}
```

with:

```c
		if (op && stream->next >= op->offset) {
			chunk = min_t(u64, chunk, op->offset + op->length - stream->next);
			if (op->op == IORING_OPAQUE_DISCARD) {
				op->progress += chunk;
				ret = chunk;
				goto consumed;
			}
		} else {
			/* Undecided bytes stay on the socket until someone asks for them. */
			if (stream->hold_undecided)
				break;
			if (op)
				chunk = min_t(u64, chunk, op->offset - stream->next);
		}
```

Leave everything else in the function unchanged, including the `consumed:` label, the
decision completion, the `-EOVERFLOW` check and `return consumed ?: ret;`.

Why this is correct, and what not to "fix":

- **When held, the actor returns the bytes it already consumed (possibly 0) and sets no
  error.**
- **`tcp_read_sock()` handles that** (`net/ipv4/tcp.c`, `__tcp_read_sock()`):
  - A positive partial return makes it call the actor again for the rest of the same
    skb. The actor then returns 0, and `tcp_read_sock()` stops with the remaining bytes
    still queued.
  - A first return of 0 stops it immediately.
- **Do not set `desc->error`.** Do not return `-ENOBUFS` or touch `stream->need_bytes`.
  Do not post a pressure CQE or add the collector to `budget_waits`. A held stream isn't
  out of memory; it's waiting for the application.
- **The `ret` local is 0** when the first iteration breaks, so `consumed ?: ret` returns
  0.

### 3.5 `io_uring/opaque.c`: no policy copies, in `io_opaque_capture()` (line 989)

Make three edits:

1. Delete line 994:
   ```c
   	bool bounded = stream->store->config.capture_window && !io_opaque_capture_keep(stream);
   ```
2. Replace lines 1002-1003:
   ```c
   	/* Dense lookahead prevents tiny headers from pinning entire RX allocations. */
   	copy_frags = bounded || shared || skb_cloned(skb) || skb_has_shared_frag(skb);
   ```
   with:
   ```c
   	copy_frags = shared || skb_cloned(skb) || skb_has_shared_frag(skb);
   ```
3. In the head path (line 1008), replace
   `if (!bounded && !shared && !skb_head_is_locked(skb)) {` with
   `if (!shared && !skb_head_is_locked(skb)) {`.

Do not change the `goto copy` fallback, `retain_limit`, or `io_opaque_append()`.

### 3.6 `io_uring/opaque.c`: a waiting INSPECT must wake the collector, in `io_opaque_range_start()` (line 1637)

Find:

```c
	io_opaque_wait(op, IO_OPAQUE_RANGE_WAIT);
	io_opaque_stream_process(stream);
	/* A consuming decision reopens a bounded undecided capture window. */
	if (op->op != IORING_OPAQUE_INSPECT)
		io_opaque_wake_stream(stream);
```

and change it to:

```c
	io_opaque_wait(op, IO_OPAQUE_RANGE_WAIT);
	io_opaque_stream_process(stream);
	/* A decision, or an INSPECT still waiting for bytes, may let collection continue. */
	if (op->op != IORING_OPAQUE_INSPECT || !list_empty(&op->read))
		io_opaque_wake_stream(stream);
```

- After `io_opaque_stream_process()`, a satisfied INSPECT has been removed from
  `stream->reads` with `list_del_init()`. So `!list_empty(&op->read)` means it is still
  waiting.
- This runs under the `guard(mutex)(&stream->lock)` at the top of the function, which
  `io_opaque_wake_stream()` asserts.
- Do not move the wake outside the lock.

That is the whole kernel logic change. The only gate inputs are these wake-ups:

- decisions and waiting INSPECTs wake the collector here;
- new data wakes it through its multishot poll;
- errors, EOF and urgent data are still observed on every collector run, held or not.

### 3.7 Remove `capture_window` from the ABI

`capture_window` no longer has a meaning. Keep the struct layout and size, and turn the
field into a reserved word.

1. `include/uapi/linux/io_uring/opaque.h`: in `struct io_uring_opaque_config`, rename
   `__u32 capture_window;` to `__u32 __resv2;`. Keep its position and `__u32 __resv1;`
   after it.
2. `io_uring/opaque.c`, `io_opaque_alloc()` (validation at lines 427 and 439-450):
   - add `cfg.__resv2` to the reserved-field check at line 427, which rejects nonzero
     values with `-EINVAL`: `if (cfg.flags || cfg.__resv1 || cfg.__resv2 || ...)`;
   - delete the `cfg.capture_window` range check
     (`cfg.capture_window && (... < PAGE_SIZE || ... > IO_OPAQUE_READ_MAX)`);
   - in the watermark requirements, delete `!cfg.capture_window ||`. Watermarks still
     require a nonzero, page-aligned `reply_reserve` and `low < high <= rx limit`.
3. Every v7 reference to the window, all of which must go or change:
   - `include/uapi/linux/io_uring/opaque.h:36`
   - `io_uring/opaque.c:441-442, 445, 994, 1002, 1115-1116, 1663`
   - `io_uring/opaque_test.c:793, 803, 869`
   - `tools/testing/selftests/io_uring/opaque_obj.c:3524, 3634, 3655-3656, 3719, 3741,
     3744, 3754`
   - `Documentation/userspace-api/io_uring-opaque.rst:55, 58, 88, 354, 526, 559`

   When you're done, `grep -rn capture_window io_uring include/uapi/linux/io_uring
   tools/testing/selftests/io_uring Documentation/userspace-api/io_uring-opaque.rst` must
   return nothing.

### 3.8 Do not change

The following stay exactly as they are:

- the send path, `tx_put()` / `tx_started`, framing and `reply_reserve`;
- watermarks, `memory_mode` and forwarding-only objects;
- compaction and the dispatcher;
- `io_opaque_restore()` and the RECV_OBJECT fail-fast;
- the pressure CQE and the budget wait list;
- INSPECT semantics. INSPECT still completes immediately with bytes that are already
  collected; it doesn't force a collection first. If none are available at its offset,
  it waits, and R4a makes the collector fetch more.
- STREAM_STAT and STAT layouts. `undecided_bytes` keeps its meaning (collected,
  undecided). Bytes held on the socket are visible with `ioctl(SIOCINQ)`.

---

## 4. Tests

### 4.1 KUnit (`io_uring/opaque_test.c`, patch 3/5)

Replace `opaque_capture_window()` (lines 793-830) with `opaque_collect_gate()`, and rename
its `KUNIT_CASE` entry (line 869). Build skbs with the existing `opaque_test_frag()`
helper (page-frag skbs).

- **Don't use a linear `alloc_skb()` + `skb_put()` skb for the no-copy check.** Its head
  is kmalloc'd, so the safety rule in `io_opaque_capture()` copies it, and a
  `copied == 0` assertion would fail for reasons unrelated to this change.
- **`opaque_test_stream()` zeroes the stream**, so `hold_undecided` starts false (collect).

The test must check:

1. **Open gate, retention:**
   - Feed one frag skb of `PAGE_SIZE` bytes to `io_opaque_actor()`.
   - Expect a return of `PAGE_SIZE`, `stream.next == PAGE_SIZE`,
     `stream.undecided == PAGE_SIZE`, `desc.error == 0` and
     `atomic64_read(&store->copied) == 0`.
2. **Held gate:**
   - Set `stream.hold_undecided = true` and feed another frag skb.
   - Expect a return of 0, `stream.next` and `stream.undecided` unchanged, and
     `desc.error == 0`.
3. **Decided bytes pass a held gate:**
   - With `hold_undecided` still true, insert a DISCARD with
     `io_opaque_range_insert()`: a stack `struct io_opaque_req` with `.store`, `.stream`,
     `.op = IORING_OPAQUE_DISCARD`, `.offset = stream.next`, `.length = 2 * PAGE_SIZE`.
   - Feed one `PAGE_SIZE` frag skb. Expect a return of `PAGE_SIZE`,
     `op.progress == PAGE_SIZE`, `stream.next` advanced by `PAGE_SIZE`, and `undecided`
     unchanged.
   - **Never let the KUnit DISCARD complete.** In KUnit `op.req` is NULL, and completion
     calls `io_opaque_queue_ready()`, which dereferences it. Keep the length larger than
     what you feed, and `rb_erase_cached(&op.decision, &stream.decisions)` before cleanup.
4. **The predicate:**
   - `io_opaque_collect_demand()` must be `VISIBLE_IF_KUNIT` and declared in the
     `#if IS_ENABLED(CONFIG_KUNIT)` block of `opaque_internal.h`.
   - It asserts `stream->lock`, so in the test call `mutex_init(&stream.lock)` and
     `mutex_lock()` around the calls. `opaque_test_stream()` does not initialize the
     mutex, and lockdep complains about an uninitialized lock.
   - Expect true with `undecided == 0`.
   - Expect true with `undecided > 0` and a list entry added to `stream.reads`. Use a
     `struct list_head` on the stack; remove it after.
   - Expect true with a pending decision whose offset is greater than `stream.next`.
   - Expect false with `undecided > 0`, no reads and no decision.

Free every skb, page and extent (`io_opaque_extents_free(store, &stream.extents)`), and
check that `atomic64_read(&store->bytes)` returns to 0, as the existing tests do.
Appendix C has the version I ran.

### 4.2 Selftests (`tools/testing/selftests/io_uring/opaque_obj.c`, patch 4/5)

Tests that encode the old behavior and must change:

- **`test_lowmem()`** (line 3499): delete `cfg.capture_window = ...` (line 3524). Nothing
  else should need changing; the flow is INSPECT header, then DISCARD body.
- **`test_reply_reserve(cfg, windowed)`** (line 3616) **must be rewritten, not
  loosened.**
  - Its non-windowed variant asserts that the greedy collector *refills* the capture
    allowance after an eviction (`"greedy collector refills capture allowance"`,
    around line 3672). That is exactly the N1 behavior R3 forbids.
  - Both variants also wait for a pressure CQE that a held stream no longer produces.
    On the prototype both fail at that wait ("CQ wait deadline").
  - Replace the function with the single-variant version in Appendix B, and delete the
    second call `test_reply_reserve(cfg, true);` from `main()`.
  - That version queues the whole flood on TCP before the collector's first pass, so the
    pass fills the allowance deterministically. It then asserts the stream does **not**
    refill after the FREE, and that framed replies use the reserve.
  - I ran it: it passes 3/3 on the prototype and fails 3/3 on v7, at the refill
    assertion.
- **`test_watermark_config()`** (line 3710):
  - delete `cfg.capture_window = ...` from the valid base configuration (line 3719); a
    valid watermark config without any window must register successfully;
  - cases 5 and 6 (`capture_window = 0` / `65537`, lines 3741 and 3744) become one case,
    `bad.__resv2 = 1` → `-EINVAL`; renumber the cases and the loop bound;
  - change the pass message (line 3754) to drop "capture window".
- **Nothing else.** I ran every other existing test against a prototype of §3, each test
  function in its own process so one failure couldn't hide another. All of them pass
  unchanged, with no kernel diagnostics. That includes the 14 inline tests,
  `test_budget*`, `test_collector_*` (10,000 arrival/drain cycles and 4 GiB of DISCARD),
  `test_send_cancel_race`, `test_compact_*`, `test_memcg`, `test_disconnect`,
  `test_full_cq` and `test_allocation_failure`. Don't touch them.

New tests to add. Appendix A has reference implementations using the selftest's own
helper names. I ran them against unmodified v7 and against a prototype of §3:

- the first three **fail on v7**, each at the assertion that encodes the policy, and
  **pass on the prototype**;
- the fourth passes on both and is a regression guard: a header longer than one page
  needs no spill;
- no kernel diagnostics in either run.

1. **`test_capture_gate()`**, the G scenario of §2: first arrival retained and uncopied;
   second arrival held on the socket until the first is decided; then collected.
2. **`test_split_header()`**, the S scenario: the second part is held until an INSPECT
   waits for it; then the INSPECT returns it.
3. **`test_greedy_default_config()`**, the N1 scenario of §2 with a zero memory policy:
   FREE on connection B must let B's framed SEND_LAST and generated-only reply succeed.
   - Flooding connection A must not refill: its `undecided_bytes` stays the same after
     the FREE.
   - Note: with a store smaller than one collection pass, A's first pass can still fill
     the store before the FREE. That's allowed by R1. The test must FREE first and reply
     after, as described.
4. **`test_header_larger_than_old_window()`:** a 6000-byte header written in one
   `write()` is fully visible to INSPECT (result 6000) with no RECV_OBJECT spill. This is
   a regression guard against reintroducing a byte window.

Update the plan: `ksft_set_plan(90)` becomes `ksft_set_plan(93)` (one reply-reserve
variant removed, four tests added).

### 4.3 Documentation (`Documentation/userspace-api/io_uring-opaque.rst`, patch 5/5)

- **Remove every `capture_window` reference:**
  - the configuration paragraph (lines 54-62);
  - the collector paragraph that begins "When a configured undecided capture window
    fills" (line 88);
  - the lookahead paragraph (lines 354-360);
  - the validation summaries (lines 526 and 559).
- **Describe collection** with the four rules of §1 in plain words: what is collected,
  that undecided bytes are retained and not copied, that further bytes stay on the
  socket while undecided bytes exist, and the two demand exceptions.
- **State the consequence for INSPECT:** it returns bytes already collected. If the
  application needs bytes beyond them, it re-INSPECTs at the end offset, which waits and
  makes the collector fetch more.
- **State the consequence for decisions:** a decision beyond the first undecided byte
  makes the gap collectable as undecided.
- **Update the N1 / reply-reserve text.** A flooding connection can no longer refill
  capacity freed by the application. A single collection pass of up to 256 KiB can still
  fill a small store, and that is what `reply_reserve` is for.
- **Update the watermark validation text:** watermarks no longer require a window.

---

## 5. Pitfalls

1. **Don't evaluate the gate inside the actor per chunk.** The first undecided chunk
   makes `undecided > 0` and would stop the pass after one skb, violating R1. Use the
   per-pass flag.
2. **Don't report a held pass as `-ENOBUFS`.** That posts pressure CQEs and parks the
   collector on the budget list, where budget releases can never wake it. This exact
   mistake is what v7's window did.
3. **Don't drop the R4 demand exceptions.** Without R4a, any header split across arrivals
   deadlocks. Without R4b, an application that declares a body ahead of its header
   decision deadlocks.
4. **Don't remove the safety copies** in `io_opaque_capture()`. Only the `bounded` term
   goes.
5. **Keep the flag's polarity** (`hold_undecided`, zero means collect), or every KUnit
   fixture that calls `io_opaque_actor()` directly silently stops capturing.
6. **`range_start()` must read `op->read` while holding `stream->lock`,** right after
   `io_opaque_stream_process()`.
7. **No UAPI churn** beyond renaming the field to a reserved word. Don't change
   structure sizes or add flags.
8. **Keep bisectability.** Put the kernel change in 2/5, the KUnit change in 3/5, the
   selftests in 4/5 and the docs in 5/5. Every patch must build with
   `CONFIG_IO_URING_OPAQUE_OBJ=y` and `CONFIG_KUNIT_ALL_TESTS=y`.

---

## 6. Known consequences (document them; not part of this change)

- **One collection pass can still fill a small store.** R1 allows up to 256 KiB per pass,
  so v4's single-store N1 case (192 KiB capture allowance) still fails under the default
  config. `reply_reserve` covers it.
- **Retained undecided fragments are charged by whole backing page.** A peer that sends
  many tiny segments in distinct pages makes one pass charge many pages. The store quota
  and the pass batch bound it. A charge-based per-pass cap is a possible follow-up; do
  not add it now.
- **EOF is reported only once the socket queue is empty.** A stream held by R3 with a
  FIN queued reports EOF after the application decides the held bytes or an INSPECT
  demands more.

---

## 7. Acceptance criteria

1. **Builds:** every commit with `CONFIG_IO_URING_OPAQUE_OBJ=y`,
   `CONFIG_IO_URING_OPAQUE_OBJ_KUNIT_TEST=y` and `CONFIG_KUNIT_ALL_TESTS=y`; `W=1` clean on
   `io_uring/` and the changed `net/` objects; `CONFIG_IO_URING_OPAQUE_OBJ=n`; and `i386`
   with `HIGHMEM4G`.
2. **checkpatch:** `scripts/checkpatch.pl --strict` reports nothing new (only the
   expected missing Signed-off-by).
3. **KUnit:** all cases pass, including `opaque_collect_gate`.
4. **Selftests:** all pass under KASAN, lockdep and SMP (2+ CPUs), including the 10,000
   SMP SEND races, the 10,000 arrival/drain cycles and 4 GiB DISCARD, plus the four new
   tests.
5. **No kernel diagnostics:** no `BUG:`, `WARNING:`, `KASAN:`, `refcount_t:` or
   lockdep report in the console log.
6. **Reproducer values:** the "Required" column of §2 is met exactly.

---

## Appendix A: reference implementations of the new selftests

These use the helpers already in v7's `opaque_obj.c`: `attach`, `command`, `cmd_stage`,
`wait_tag`, `wait_tag_ms`, `enter`, `write_all`, `tcp_pair`, `register_store`,
`receive_complete`, `frame_stage`, `ring_init` and `ring_exit`. They compile unchanged
when placed after `receive_complete()` in that file. `<sys/ioctl.h>` is already included.

- **Call them from `main()`** after `test_reply_reserve()`, passing the same `cfg` the
  other tests receive.
- **Add 4 to the plan count.**
- **`settle()` matters.** The ring is `DEFER_TASKRUN`, so the collector only runs when
  the test enters the ring with GETEVENTS. That is what `settle()` does after each
  `write()`.

```c
static int unread_bytes(int fd)
{
	int unread;

	require(!ioctl(fd, FIONREAD, &unread), "TCP unread count");
	return unread;
}

static uint64_t stream_undecided(struct ring *r, uint32_t context, uint64_t stream)
{
	struct io_uring_opaque_stream_stat stat;

	require(!command(r, context, IORING_OPAQUE_STREAM_STAT, stream, 0,
			 sizeof(stat), &stat).res, "stream stat");
	return stat.undecided_bytes;
}

/* Let the collector's task work run (DEFER_TASKRUN needs a GETEVENTS enter). */
static void settle(struct ring *r)
{
	usleep(100000);
	enter(r, 0);
}

/* G: undecided bytes are retained uncopied and hold further collection. */
static void test_capture_gate(struct io_uring_opaque_config cfg)
{
	const size_t length = 16U << 10;
	unsigned char *data = calloc(1, length), header[128];
	struct io_uring_opaque_stat stat;
	struct ring r;
	uint32_t context;
	uint64_t stream;
	int pair[2];

	require(data && !ring_init(&r, IORING_SETUP_CQE32) &&
		!register_store(&r, &cfg, &context), "capture gate store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	write_all(pair[0], data, length);
	settle(&r);
	require(!unread_bytes(pair[1]) && stream_undecided(&r, context, stream) == length,
		"first arrival collected while nothing was undecided");
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, sizeof(header),
			header).res == sizeof(header), "inspect first arrival");
	write_all(pair[0], data, length);
	settle(&r);
	require(unread_bytes(pair[1]) == (int)length &&
		stream_undecided(&r, context, stream) == length,
		"second arrival stays on TCP while bytes are undecided");
	require(command(&r, context, IORING_OPAQUE_DISCARD, stream, 0, length,
			NULL).res == (int)length, "decide first arrival");
	settle(&r);
	require(!unread_bytes(pair[1]) && stream_undecided(&r, context, stream) == length,
		"deciding the first arrival lets the collector take the second");
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		!stat.copied_bytes, "undecided bytes are retained, not copied");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"capture gate close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	free(data);
	ksft_test_result_pass("undecided bytes are retained and hold further collection\n");
}

/* S: a header split across arrivals; a waiting INSPECT pulls the second part. */
static void test_split_header(struct io_uring_opaque_config cfg)
{
	unsigned char part[100], buf[512];
	struct ring r;
	uint32_t context;
	uint64_t stream, tag;
	int pair[2];

	memset(part, 'h', sizeof(part));
	require(!ring_init(&r, IORING_SETUP_CQE32) && !register_store(&r, &cfg, &context),
		"split header store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	write_all(pair[0], part, sizeof(part));
	settle(&r);
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, sizeof(buf),
			buf).res == sizeof(part), "inspect first header part");
	write_all(pair[0], part, sizeof(part));
	settle(&r);
	require(unread_bytes(pair[1]) == sizeof(part), "second part stays on TCP");
	tag = cmd_stage(&r, context, IORING_OPAQUE_INSPECT, stream, sizeof(part),
			sizeof(buf), buf);
	require(wait_tag_ms(&r, tag, 5000).res == sizeof(part),
		"waiting INSPECT collects the second part");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"split header close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("a waiting INSPECT collects a split header\n");
}

/* N1 with a zero memory policy: a flooding stream cannot refill freed capacity. */
static void test_greedy_default_config(struct io_uring_opaque_config cfg)
{
	struct io_uring_opaque_frame frame = { .size = sizeof(frame),
		.prefix = (uintptr_t)"HDR:", .prefix_length = 4 };
	unsigned char *flood = calloc(1, 256U << 10);
	struct ring r;
	struct event event;
	uint32_t context;
	uint64_t greedy, responder, request, cached, before;
	int a[2], b[2], target[2];

	cfg.hard_limit = 256U << 10;
	cfg.compact_headroom = 64U << 10;
	cfg.reply_reserve = 0;
	cfg.high_watermark = 0;
	cfg.low_watermark = 0;
	require(flood && !ring_init(&r, IORING_SETUP_CQE32) &&
		!register_store(&r, &cfg, &context), "default policy store");
	tcp_pair(a);
	tcp_pair(b);
	tcp_pair(target);
	responder = attach(&r, context, b[1], next_tag++);
	request = receive_complete(&r, context, responder, b[0], 0, "request!", 8).extra[0];
	cached = receive_complete(&r, context, responder, b[0], 8, "cached!!", 8).extra[0];
	greedy = attach(&r, context, a[1], next_tag++);
	write_all(a[0], flood, 256U << 10);
	settle(&r);
	before = stream_undecided(&r, context, greedy);
	require(!command(&r, context, IORING_OPAQUE_FREE, cached, 0, 0, NULL).res,
		"evict cached object");
	settle(&r);
	require(stream_undecided(&r, context, greedy) == before && unread_bytes(a[1]) > 0,
		"flooding stream does not refill freed capacity");
	event = wait_tag(&r, frame_stage(&r, context, target[0], request, 8, &frame,
					 IORING_OPAQUE_SEND_LAST, 0));
	require(event.res == 12 && (event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
		"framed reply after eviction");
	event = wait_tag(&r, frame_stage(&r, context, target[0], 0, 0, &frame, 0, 0));
	require(event.res == 4, "generated-only reply after eviction");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, greedy, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, responder, 0, 0, NULL).res,
		"default policy close");
	ring_exit(&r);
	close(a[0]); close(a[1]); close(b[0]); close(b[1]);
	close(target[0]); close(target[1]);
	free(flood);
	ksft_test_result_pass("default policy: flooding stream cannot starve replies\n");
}

/* A header longer than the old 4 KiB window is fully visible to INSPECT. */
static void test_header_larger_than_old_window(struct io_uring_opaque_config cfg)
{
	unsigned char header[6000], buf[8192];
	struct ring r;
	uint32_t context;
	uint64_t stream;
	int pair[2];

	memset(header, 'H', sizeof(header));
	require(!ring_init(&r, IORING_SETUP_CQE32) && !register_store(&r, &cfg, &context),
		"large header store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	write_all(pair[0], header, sizeof(header));
	settle(&r);
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, sizeof(buf),
			buf).res == sizeof(header), "inspect a 6000-byte header");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"large header close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("INSPECT sees a header larger than one page\n");
}
```

## Appendix B: replacement for `test_reply_reserve()`

This replaces both variants. The new signature is `test_reply_reserve(cfg)`; update the
single remaining call in `main()` and reduce the plan count by one.

```c
/*
 * Reply reserve under capture pressure. The flood is fully queued on TCP before
 * the collector's first pass, so that pass fills the ordinary allowance. After
 * an eviction the flooding stream is held (R3) and must not refill.
 */
static void test_reply_reserve(struct io_uring_opaque_config cfg)
{
	struct io_uring_opaque_frame frame = { .size = sizeof(frame),
		.prefix = (uintptr_t)"HDR:", .prefix_length = 4 };
	struct io_uring_opaque_stat stat;
	struct io_uring_opaque_stream_stat before, after;
	struct ring r;
	struct event event;
	uint32_t context;
	uint64_t greedy, responder, request, cached, collector = next_tag++, tag;
	unsigned char *flood = calloc(1, 256U << 10), wire[16];
	unsigned int attempt;
	int a[2], b[2], target[2], unread = 0, big = 1 << 20;
	uint64_t limit;

	cfg.hard_limit = 256U << 10;
	cfg.compact_headroom = 64U << 10;
	cfg.reply_reserve = sysconf(_SC_PAGESIZE);
	limit = cfg.hard_limit - cfg.compact_headroom - cfg.reply_reserve;
	require(flood && !ring_init(&r, IORING_SETUP_CQE32) && !register_store(&r, &cfg, &context),
		"reply reserve store");
	tcp_pair(a);
	tcp_pair(b);
	tcp_pair(target);
	require(!setsockopt(a[1], SOL_SOCKET, SO_RCVBUF, &big, sizeof(big)),
		"greedy receive buffer");
	responder = attach(&r, context, b[1], next_tag++);
	request = receive_complete(&r, context, responder, b[0], 0, "request!", 8).extra[0];
	cached = receive_complete(&r, context, responder, b[0], 8, "cached!!", 8).extra[0];
	greedy = attach(&r, context, a[1], collector);
	write_all(a[0], flood, 256U << 10);
	/* The collector runs only when the ring is entered: let TCP queue the flood first. */
	for (attempt = 0; attempt < 5000; attempt++) {
		require(!ioctl(a[1], FIONREAD, &unread), "TCP unread count");
		if (unread >= (int)limit + 4096)
			break;
		usleep(1000);
	}
	require(unread >= (int)limit + 4096, "flood queued on TCP before collection");
	event = wait_tag_ms(&r, collector, 5000);
	require(event.res == -ENOBUFS && (event.flags & IORING_CQE_F_MORE) &&
		event.extra[0] == greedy, "first collection pass fills the allowance");
	require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, greedy, 0,
			 sizeof(before), &before).res &&
		!ioctl(a[1], FIONREAD, &unread) && unread > 0, "excess data stays on TCP");
	require(!command(&r, context, IORING_OPAQUE_FREE, cached, 0, 0, NULL).res,
		"evict cached object");
	usleep(100000);
	enter(&r, 0);
	require(!command(&r, context, IORING_OPAQUE_STREAM_STAT, greedy, 0,
			 sizeof(after), &after).res &&
		!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		after.undecided_bytes == before.undecided_bytes && stat.backing_bytes < limit,
		"held flooding stream does not refill freed capacity");
	{
		struct io_uring_opaque_frame large = { .size = sizeof(large),
			.prefix = (uintptr_t)flood, .prefix_length = cfg.reply_reserve + 1 };

		tag = frame_stage(&r, context, target[0], request, 8, &large,
				  IORING_OPAQUE_SEND_LAST, 0);
		event = wait_tag_ms(&r, tag, 5000);
		require(event.res == -ENOBUFS && !(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
			"reply reserve exhaustion preserves LAST ownership");
	}
	tag = frame_stage(&r, context, target[0], request, 8, &frame, IORING_OPAQUE_SEND_LAST, 0);
	event = wait_tag_ms(&r, tag, 5000);
	require(event.res == 12 && (event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
		"other connection admits framed reply under capture pressure");
	tag = frame_stage(&r, context, target[0], 0, 0, &frame, 0, 0);
	require(wait_tag_ms(&r, tag, 5000).res == 4, "generated reply under capture pressure");
	read_all(target[1], wire, sizeof(wire));
	require(!memcmp(wire, "HDR:request!HDR:", sizeof(wire)) &&
		!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, greedy, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, responder, 0, 0, NULL).res &&
		!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		!stat.backing_bytes && !stat.framing_bytes,
		"reserved reply wire bytes and release");
	ring_exit(&r);
	close(a[0]);
	close(a[1]);
	close(b[0]);
	close(b[1]);
	close(target[0]);
	close(target[1]);
	free(flood);
	ksft_test_result_pass("two connections retain reply progress with reserved framing\n");
}
```

## Appendix C: reference KUnit test

This is the `opaque_collect_gate()` I ran, at 27/27 with no lockdep or KASAN reports. It
replaces `opaque_capture_window()`. In the `kunit_case` array, change
`KUNIT_CASE(opaque_capture_window),` to `KUNIT_CASE(opaque_collect_gate),`.

```c
static void opaque_collect_gate(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_stream stream;
	struct io_opaque_req discard = {
		.store = store, .stream = &stream, .op = IORING_OPAQUE_DISCARD,
	};
	read_descriptor_t desc = { .count = 4 * PAGE_SIZE, .arg.data = &stream };
	struct page *page = alloc_page(GFP_KERNEL);
	struct list_head waiting;
	struct sk_buff *skb;

	KUNIT_ASSERT_NOT_NULL(test, page);
	opaque_test_stream(&stream, store);
	mutex_init(&stream.lock);
	/* Open gate: undecided bytes are retained, not copied. */
	skb = opaque_test_frag(test, page, 0, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, PAGE_SIZE), (int)PAGE_SIZE);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, desc.error, 0);
	KUNIT_EXPECT_EQ(test, stream.next, (u64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, stream.undecided, (u64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->copied), 0LL);
	/* Held gate: no further undecided bytes leave the socket. */
	stream.hold_undecided = true;
	skb = opaque_test_frag(test, page, 0, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, PAGE_SIZE), 0);
	KUNIT_EXPECT_EQ(test, desc.error, 0);
	KUNIT_EXPECT_EQ(test, stream.next, (u64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, stream.undecided, (u64)PAGE_SIZE);
	/* Decided bytes pass a held gate; the DISCARD must not complete here. */
	discard.offset = stream.next;
	discard.length = 2 * PAGE_SIZE;
	KUNIT_ASSERT_EQ(test, io_opaque_range_insert(&discard), 0);
	KUNIT_EXPECT_EQ(test, io_opaque_actor(&desc, skb, 0, PAGE_SIZE), (int)PAGE_SIZE);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, discard.progress, (u64)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, stream.next, (u64)2 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, stream.undecided, (u64)PAGE_SIZE);
	rb_erase_cached(&discard.decision, &stream.decisions);
	/* The predicate. */
	mutex_lock(&stream.lock);
	KUNIT_EXPECT_FALSE(test, io_opaque_collect_demand(&stream));
	list_add(&waiting, &stream.reads);
	KUNIT_EXPECT_TRUE(test, io_opaque_collect_demand(&stream));
	list_del(&waiting);
	discard.offset = stream.next + PAGE_SIZE;
	discard.length = PAGE_SIZE;
	KUNIT_ASSERT_EQ(test, io_opaque_range_insert(&discard), 0);
	KUNIT_EXPECT_TRUE(test, io_opaque_collect_demand(&stream));
	rb_erase_cached(&discard.decision, &stream.decisions);
	io_opaque_extents_free(store, &stream.extents);
	stream.undecided = 0;
	KUNIT_EXPECT_TRUE(test, io_opaque_collect_demand(&stream));
	mutex_unlock(&stream.lock);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	put_page(page);
}
```
