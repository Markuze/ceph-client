# Review: io_uring opaque TCP payload objects, v7

**Series reviewed:** `origin/upstream/opaque-obj-v7` at `e6aef56bc0f6`, 5 patches on top of
`af32da41b032` ("Merge tag 'net-7.3-rc7'"):

| # | Commit | Subject |
|---|--------|---------|
| 1/5 | `fc0538f7d01a` | net: add an optional exclusive TCP receive claim |
| 2/5 | `ed543e9e1031` | io_uring: add opaque TCP stores, framed sends and lowmem forwarding |
| 3/5 | `1c50ae1c3cdd` | io_uring: test opaque ownership, memory modes and compaction |
| 4/5 | `0d334f5c455d` | selftests/io_uring: exercise opaque lowmem, quota and SMP cancellation |
| 5/5 | `e6aef56bc0f6` | docs: describe opaque forwarding, memory and ownership contracts |

Context:

- v7 is v6 plus one development commit (`057e2f53c3f4`, "Fix opaque SEND cancellation and
  add lowmem forwarding policy"), squashed into the five patches.
- Supporting material:
  - errata and optimization notes on `origin/work/opaque-obj-v7`;
  - the reviewer description, the v6 review response (`opaque_review_v6_response.md`)
    and the memory-policy design (`opaque_watermarks.md`) in `zc_proxy` at `a9c52c9`.

This review follows `opus_review_v6.md`; §3 maps each v6 finding to its v7 status. All
`file:line` references are to `e6aef56bc0f6`. As before, it is written from the
perspective of an io_uring maintainer reviewing an RFC, not an actual maintainer response.
The author asked for focus on cancellation and SEND_LAST ownership, accounting and mode
transitions under shared-store pressure, forwarding-only CQE/UAPI semantics, and
compaction lifetime. §4 covers those.

---

## 1. Verdict

**Converging, and the v6 findings are closed. Still not mergeable: the DCO is pending,
and the new memory-policy ABI needs design changes before it is frozen.**

v7 closes every v6 finding I raised:

- **Q (follower cancel) is fixed** with the `tx_started` rule from my prototype. My
  reproducer now passes 5/5 in both orderings. The series adds early/late tests on one
  ring and across rings.
- **B (bisect) is fixed.** All five commits build with `KUNIT_ALL_TESTS`.
- **N1 is fixed when configured.** Either the new `reply_reserve` or the new
  `capture_window` makes my two-connection reproducer pass. The default (zero) config
  keeps the documented legacy behavior.
- **The publication kick is fixed:** a paced `queue_delayed_work()` without resetting
  `auto_at`.
- **The rest is done too:** the dispatcher comment is corrected, the selftest timeout is
  configurable, and `Assisted-by` uses the documented format.
- **CQ-full collector termination is documented as E08,** with rearm deferred.

I found no memory-safety or lifetime bugs in v7. KASAN, lockdep, PREEMPT and 2-vCPU SMP
runs are clean throughout (§2).

The new memory-policy ABI is where the review effort should go. Four findings, all
reproduced except the forwarding-only semantics, which are by inspection with the
`EOPNOTSUPP` observed:

1. **L [medium]: compaction can switch the store into LOWMEM and leave it there (§4.1).**
   - Replacement reservations count toward `high_watermark`, and LOWMEM only exits at
     `low_watermark`.
   - A manual COMPACT that *lowered* occupancy from 3.37 MB to 3.31 MB flipped the store
     from NORMAL to LOWMEM.
   - The next object received was forwarding-only, as is every object admitted until
     occupancy falls to `low_watermark`.
2. **R [low to medium]: `reply_reserve` caps framing instead of guaranteeing it (§4.2).**
   With an 8 KiB reserve, two replies blocked on slow peers made a 4-byte-header reply
   on a healthy connection fail `-ENOBUFS`. The store was 13% full.
3. **Forwarding-only enforcement doesn't bound memory but does remove fan-out and retry
   (§4.3).** A LOWMEM object can be sent exactly once, and ownership moves at admission,
   so one failed or canceled SEND_LAST loses the payload. Meanwhile the handle can still
   be held indefinitely and read with READ_OBJECT.
4. **`capture_window` costs more than the doc says (§4.4):**
   - every byte captured before a decision is copied, so a 16 KiB message is copied in
     full (16,384 bytes copied vs. 0 without the window);
   - a header longer than the window needs a spill-to-object decision to make progress;
   - window-full reuses the store-pressure CQE, which makes the E08 CQ-full path routine.

Still blocking, unchanged: no `Signed-off-by:` (human DCO pending, per the cover letter),
plus the v4/v6 asks the author has explicitly deferred: the generic CQE flag, the
store-wide `tables` mutex, collector rearm, and benchmarks.

---

## 2. What I ran

Same setup as v6: UML with KASAN, PROVE_LOCKING, DEBUG_ATOMIC_SLEEP, failslab, PREEMPT and
`CONFIG_SMP=y`, booted with `seccomp=on ncpus=2 mem=2G`.

| Check | Result |
|-------|--------|
| UML build (SMP + PREEMPT + KASAN + lockdep) | builds, 0 warnings |
| Per-commit builds with `KUNIT_ALL_TESTS`, feature forced on from 2/5, KUnit option from 3/5 | **all 5 build.** 1/5 was built with the receive claim enabled through a temporary Kconfig prompt, since its selector only arrives in 2/5 |
| `W=1` on `io_uring/`, `net/ipv4/{tcp,af_inet,tcp_ulp}.o`, `net/core/{sock,skmsg}.o` (`TLS` → `NET_SOCK_MSG`) | 0 warnings |
| `CONFIG_IO_URING_OPAQUE_OBJ=n` | builds, 0 warnings |
| `ARCH=i386`, `HIGHMEM4G`, `W=1`, changed objects | 0 warnings; ELF32; no 64-bit libgcc division helpers |
| `checkpatch.pl --strict` | only `ERROR:MISSING_SIGN_OFF` on each patch (plus MAINTAINERS path warnings); the `Assisted-by` warning is gone |
| KUnit `io_uring-opaque` | **27/27** |
| `opaque_obj` selftest, 2-vCPU UML, `OPAQUE_TEST_TIMEOUT=7000` | **90/90, no skips, no kernel diagnostics.** This includes 10,000 cross-ring SEND races on SMP (5000 full and 5000 partial heads, 9618 followers canceled, 382 sent, 5000 linked-timeout expiries), 10,000 arrival/drain cycles and 4 GiB of DISCARD |
| `repro4` (v1/v4 cases) | A1, A2, B, D2, D3, E, F, N2 pass; N1 still fails under the default config, as documented |
| `repro6` (v6 cases) | **Q-early and Q-late both correct, 5/5 each**; v6 N1 still fails under the default config, as documented |
| `repro7` (new, Appendix A) | N1 passes with `reply_reserve` alone and with `capture_window` alone; **L, R, W reproduce**; C measured |

New reproducer results on `e6aef56`:

| Case | Configuration | Result |
|------|---------------|--------|
| N1, reserve | 256 KiB store, 64 KiB headroom, `reply_reserve` 64 KiB | greedy stream refills after the eviction; framed SEND_LAST = 12, generated-only = 4. **Fixed** |
| N1, window | same, `capture_window` 16 KiB, no reserve | greedy undecided capped at 16384; framed = 12, generated-only = 4. **Fixed** |
| L | 8 MiB store, 2 MiB headroom, 256 KiB reserve, 64 KiB window, high 4 MiB, low 2 MiB; 16 cached objects (3,375,104 bytes, NORMAL); COMPACT a 1 MiB object | COMPACT = 0; backing **3,375,104 → 3,309,568**; mode **NORMAL → LOWMEM**; next RECV_OBJECT flags `0x4` (FORWARD_ONLY); reusable SEND of it `-EOPNOTSUPP` |
| R, reserve | 8 MiB store, `reply_reserve` 8 KiB; two 3000-byte-prefix replies blocked on 4 KiB-buffer peers | framing 8192, backing 1,126,400 of 8 MiB; **4-byte-header reply on a healthy connection `-ENOBUFS`** |
| R, legacy | same, no reserve | the same reply succeeds (10 bytes) |
| W | `capture_window` 4096; peer sends a 6000-byte header and 100 bytes | INSPECT(0) = 4096; **INSPECT(4096) still pending after 1 s**; one pressure CQE; after RECV_OBJECT(0, 4096), INSPECT(4096) = 2004 |
| C | 16 KiB message arrives before the header is parsed; INSPECT then RECV_OBJECT of the whole message | window 64 KiB: **copied 16,384**, backing 16,384. No window: copied 0, backing 32,768 |

---

## 3. Status of the v6 review

| v6 item | v7 status | Evidence |
|---------|-----------|----------|
| Q: canceling a waiting SEND can cancel its successor | **Fixed** | `tx_started` (`opaque.c:2146-2147`, `:2200`); repro6 Q 5/5 in both orderings; new one-ring and cross-ring early/late tests |
| B: 2/5 doesn't build with the KUnit option | **Fixed** | option and rule moved to 3/5; all commits build with `KUNIT_ALL_TESTS` |
| N1: greedy capture blocks replies on other connections | **Fixed when configured**; legacy unchanged by design | `reply_reserve` (`:147-171`) and `capture_window` (`:1115-1126`); repro7 N1 variants pass; default config still fails, as documented |
| Pressure CQE: CQ-full ends collection for good | **Documented (E08)**, rearm deferred | doc and errata; with windows it becomes a routine path (§4.4) |
| Pressure CQE: negative `res` with `F_MORE`; no resume edge | Documented | doc |
| Publication resets `auto_at` and kicks the dispatcher | **Fixed** | `queue_delayed_work(..., msecs_to_jiffies(100))` without touching `auto_at` (`opaque.c:1474-1478`) |
| Dispatcher shutdown comment | **Fixed** | `opaque_compact.c:168` |
| Selftest `alarm(600)` | **Fixed** | `OPAQUE_TEST_TIMEOUT`, also propagated into the VM runner |
| `Assisted-by` format | **Fixed** | `Assisted-by: Codex:GPT-6` |
| `Signed-off-by` | **Still missing** | DCO pending |
| Split framed sends; generic CQE flag; `tables` mutex; `wake_budget()` scans; RECV_OBJECT fail-fast; benchmarks | **Deferred by the author** | v6 response, "Carried architecture and ABI asks" |

---

## 4. Findings in v7

### 4.1 L. [medium, design] Compaction can switch the store into LOWMEM and leave it there

- **Mechanism:**
  - `io_opaque_memory_mode()` (`opaque.c:101-121`) compares `store->bytes` with the
    watermarks after every charge and uncharge.
  - `store->bytes` includes compaction replacement reservations
    (`opaque_compact.c:80`, `io_opaque_charge(store, charge, false)`), and they may go
    up to `hard_limit - reply_reserve`, i.e. into the compaction headroom.
  - While a compaction runs, the old and new versions are both charged. A store just
    below `high_watermark` crosses it.
  - LOWMEM only exits at or below `low_watermark`. So once the compaction publishes and
    occupancy drops back, often below where it started, the store *stays* in LOWMEM
    until occupancy falls to `low_watermark`.
- **Reproduced (repro7 L):**
  1. 16 cached objects, 3,375,104 bytes, NORMAL, with high at 4 MiB.
  2. COMPACT one 1 MiB object.
  3. Backing falls to 3,309,568, but the mode is now LOWMEM.
  4. The next RECV_OBJECT returns a forwarding-only handle, and a reusable SEND of it
     fails `-EOPNOTSUPP`.
- **Why it matters:**
  - Compaction exists to *reduce* memory. Here it changes the classification of every
    object received until the application frees `high − low` worth of data: in the
    design note's example, 16 MiB.
  - For a cache, that means every new SET is uncacheable, triggered by its own memory
    optimization.
  - Automatic compaction charges its reservation through the same path
    (`opaque_compact.c:335` → `:80`). So the kernel's background policy can latch LOWMEM
    without the application doing anything. (By inspection; my fixture's objects weren't
    fragmented enough for automatic selection.)
  - The doc does say compaction reservations count ("Payload backing and compaction
    reservations reaching the high watermark enter lowmem"). It doesn't say they can
    latch the mode.
- **The same applies to other transient memory:**
  - undecided lookahead, up to `max_streams × capture_window`;
  - private assemblies in progress.
  - Neither represents retained cache data, but both move the mode, which then
    classifies long-lived objects.
- **Suggested fix:**
  - Evaluate the watermarks against retained payload, not against in-flight
    reservations. A `store->reserving` counter, added at `compact_build()` and dropped
    after publish or failure, subtracted in `io_opaque_memory_mode()`, is enough for the
    compaction case.
  - Whether lookahead and assemblies should count is a policy decision. Document it
    either way.

### 4.2 R. [low to medium, design] `reply_reserve` is a cap on framing, not a floor

- **Mechanism:**
  - With a nonzero reserve, `io_opaque_charge_frame()` (`opaque.c:147-164`) charges
    framing *only* against `reply_reserve`. It never uses the ordinary capture
    allowance, however empty that is.
  - Snapshots live until the send completes, so replies blocked on slow readers hold
    the reserve.
- **Reproduced (repro7 R):**
  - With an 8 KiB reserve, two replies with 3000-byte prefixes are blocked on peers that
    don't read.
  - A 4-byte-header reply on an unrelated, healthy connection then fails `-ENOBUFS`. The
    store held 1.1 MB of 8 MiB.
  - With the default config (no reserve) the same reply succeeds.
- **Why it matters:**
  - N1 was about other connections' traffic blocking replies. A cap re-creates that,
    with slow readers in place of fast writers.
  - The doc acknowledges that "Concurrent snapshots can still exhaust the reply
    reserve", but a reserve is expected to be a guarantee, not a ceiling.
- **Suggested fix:**
  - Charge framing to the ordinary allowance first (`io_opaque_charge(..., true)`), and
    fall back to the reserve only when that fails.
  - Capture still can't take the reserve, so the N1 protection is unchanged. The cap
    disappears, and the reserve is consumed only under pressure.
  - Framing in the ordinary allowance would then count toward the watermarks. Exclude
    `framing` in `io_opaque_memory_mode()` if that matters.

### 4.3 Forwarding-only objects: the enforcement doesn't bound memory, but it removes fan-out and retry

The semantics (`opaque.c:401-402`, `:1466-1467`, `:2030-2031`):

- An object admitted in LOWMEM can only be sent with SEND_LAST.
- A reusable SEND fails `EOPNOTSUPP` without consuming it.
- READ_OBJECT and FREE work.
- The flag survives mode changes, export/import and compaction.

What the kernel-side restriction achieves:

- **It doesn't bound memory.**
  - Handles don't expire.
  - An application can hold a forwarding-only handle indefinitely and serve it with
    READ_OBJECT.
  - The doc says exactly this ("Applications must promptly finish forwarding-only
    objects").
  - A cooperative application already sees the flag in the readiness CQE and can apply
    its own policy. An uncooperative one isn't constrained in the way that matters.
- **It removes fan-out.** A replicating forwarder (primary to N replicas) can't send a
  LOWMEM object to more than one destination: reusable SENDs are rejected, and the
  documented fan-out pattern (reusable sends, then a final SEND_LAST) is unavailable.
- **It removes retry.**
  - SEND_LAST transfers ownership at admission (E06).
  - So for a forwarding-only object, one canceled or failed SEND_LAST loses the payload
    (`IORING_CQE_F_OPAQUE_CONSUMED` with no bytes sent). There's no reusable send to try
    first.
  - A proxy that retries another backend on failure can't do that in LOWMEM without
    READ_OBJECT and a copy.

Two consistent alternatives:

- **Advisory classification:** keep the CQE flag and OBJECT_STAT, drop the
  `EOPNOTSUPP`, and leave policy to the application, which owns retention everywhere
  else in this design.
- **Enforce something that bounds memory:** for example, charge forwarding-only objects
  to their own budget, so an application that holds them stalls only new forwarding-only
  admissions, not headers; or expire unconsumed handles. With memory bounded that way,
  reusable sends, and with them fan-out, can be allowed again.

In either case, consider letting SEND_LAST transfer ownership on successful
transmission, or restoring the handle on zero-progress failure. E06 already lists
"retry-oriented forwarding", and LOWMEM makes it the only send mode.

### 4.4 `capture_window`: copy cost, header size and pressure-CQE semantics

- **Everything before a decision is copied** (`opaque.c:994-1003`).
  - With a window set, undecided bytes always use packed copies.
  - That covers more than "tiny headers": it includes any body bytes that arrive before
    the application arms RECV_OBJECT, which for request/response traffic is usually the
    whole small message.
  - repro7 C, 16 KiB message: 16,384 bytes copied with a 64 KiB window, 0 without
    (backing 16 KiB vs. 32 KiB). Watermarks *require* a window, so LOWMEM mode makes
    messages smaller than the window copy-based.
  - That is a reasonable density trade-off, but the doc should state it, and the KVS
    benchmark should measure it.
  - An alternative that keeps zero-copy for small messages: retain RX pages for
    lookahead, and copy only when a decision would leave a small fragment pinning a
    large allocation.
- **A header longer than the window can't be inspected** (repro7 W).
  - INSPECT sees exactly `capture_window` bytes past the decided offset.
  - The rest stays on TCP until a consuming decision, which the application can't make
    without parsing the header.
  - Workaround: RECV_OBJECT the window prefix and READ_OBJECT it back. That works (W),
    but it's a spill protocol every parser must implement.
  - The doc's example window is 4 KiB. Recommend at least the largest protocol header,
    or document the spill pattern.
- **Window-full and store-full share one CQE.**
  - A full window posts the same nonterminal `-ENOBUFS` with `F_MORE` as a full store
    (`opaque.c:1362`).
  - The application can tell them apart only by comparing the reported offset with its
    own decided offset.
  - With small windows, this CQE becomes routine flow control rather than an exceptional
    event. That makes E08 (CQ full during a pressure CQE ends collection on that token
    for good) a routine risk too.
  - I'd raise collector rearm from "deferred" to "needed with windows", or have
    window-full not post a CQE at all, since the application already knows its window.

### 4.5 Smaller UAPI notes

- **The config struct keeps `__u64 __resv[2]` in the middle** and appends the new fields
  after it (`include/uapi/linux/io_uring/opaque.h:32-37`). The struct is size-versioned
  through `copy_struct_from_user()`, so the reserved words buy nothing. Drop them before
  the ABI freezes, or use them.
- **`IORING_OPAQUE_OBJECT_F_FORWARD_ONLY` is defined with a space instead of a tab** and
  shares the `OBJECT_F` namespace with flags that never appear in the readiness CQE.
  Name the readiness `extra[1]` word explicitly in the UAPI header (for example
  `IORING_OPAQUE_READY_F_*`), or document that it carries the `OBJECT_F` subset.
- **Watermarks require both a reply reserve and a window.** That coupling is a policy
  choice. State the reason in the doc (header progress in LOWMEM), or drop it.

---

## 5. Design status (carried, explicitly deferred by the author)

- **Single-command framed SEND:** kept as an owner requirement. It's fine to keep; N1 is
  now addressed by configuration.
- **Generic CQE flag** `IORING_CQE_F_OPAQUE_CONSUMED`: unchanged.
- **Store-wide `tables` mutex and O(waiters) `wake_budget()`:** unchanged.
- **RECV_OBJECT fail-fast under shared pressure:** unchanged.
- **Collector rearm (E08):** see §4.4.
- **Benchmarks:** none. The reviewer description is explicit that throughput, CPU,
  latency and useful-capacity advantages over RECV_ZC, SEND_ZC or optimized copy are
  unmeasured. The KVS/relay pilot should include the window copy cost (§4.4) and the
  LOWMEM transition frequency (§4.1).

---

## 6. Smaller notes

- **1/5 can't be build-tested on its own:** `SOCK_RX_OWNER` is a hidden symbol whose only
  selector arrives in 2/5, so 1/5 compiles with the claim disabled. The author used a
  temporary selector, as I did. Consider giving the symbol a prompt under `EXPERT`, or
  say in 1/5's commit message how to test it.
- **The validation record corrects the earlier i386 claim** (the v6 "i386" config was
  actually 64-bit). That's appreciated. My i386 HIGHMEM compile of v7 (ELF32) is clean.
- **E01–E08 stay accurate.** L and R should be added there if not fixed.

---

## 7. What's good in v7

- **Every v6 finding is closed, with tests that fail on v6** (per the author) and pass
  here. Q and B were fixed exactly as suggested, without collateral changes.
- **The N1 fix is minimal and opt-in:** two config fields, no new opcode or worker, and
  legacy behavior preserved when they're zero. Both knobs independently fix my
  two-connection reproducer.
- **The memory-mode transition code is careful:** lock-free hysteresis that rechecks
  after a cmpxchg race, classification at slot admission under `tables`, and
  classification carried through compaction and export.
- **No regressions:** clean KASAN, lockdep, SMP and PREEMPT runs, 10,000 SMP SEND races,
  and a build matrix that now includes `KUNIT_ALL_TESTS` per commit and real i386.
- **The documentation is honest about limits** (temporary backing, no expiry, hard-limit
  stalls). The findings above are mostly about consequences the doc doesn't spell out.

---

## 8. Path to v8

1. **A human `Signed-off-by:` on every patch.**
2. **L:** exclude in-flight compaction reservations, and decide about lookahead and
   assemblies, when evaluating watermarks. Add a test that compacts near the high
   watermark.
3. **R:** make `reply_reserve` a floor. Fall back to it only when the ordinary
   allowance is full, and add a slow-reader test.
4. **Forwarding-only:** advisory flag, or enforcement that bounds memory. Either way,
   decide the retry and fan-out story, together with E06.
5. **Window:** document the copy cost and the header-size requirement or spill pattern,
   and separate window-full from store pressure in the CQE. Then reconsider collector
   rearm (E08).
6. **UAPI cleanup:** the reserved words in the config struct, and the readiness-flag
   naming.
7. **Benchmarks,** including the window copy fraction and LOWMEM transitions.

---

## Appendix A: v7 reproducer

`repro7.c` reuses the helper half of `repro6.c` (everything before its first test,
saved as `repro_common.h`) and the same `helpers.h` as the v4 and v6 reproducers. Build
and run it like `repro6` (Appendix B of `opus_review_v6.md`), against the v7 UAPI headers:

```sh
gcc -static -O2 -Wall -I$hdr/usr/include -o repro7 repro7.c -pthread
./linux mem=2G seccomp=on ncpus=2 rootfstype=hostfs rootflags=$root rw init=/init \
    TESTS=repro7 con=null con0=null,fd:1          # optionally ARGS=NLRWC
```

```c
// SPDX-License-Identifier: GPL-2.0
/* Review reproducers for the io_uring OPAQUE_OBJ RFC v7 (origin/upstream/opaque-obj-v7). */
#include "repro_common.h"

static struct io_uring_opaque_stat store_stat(struct ring *r, uint32_t ctx)
{
	struct io_uring_opaque_stat stat;

	require(!command(r, ctx, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res, "STAT");
	return stat;
}

static const char *mode_name(uint32_t mode)
{
	return mode == IORING_OPAQUE_MEMORY_LOWMEM ? "LOWMEM" : "NORMAL";
}

/* RECV_OBJECT that also returns the readiness CQE's object flags. */
static uint64_t publish_flags(struct ring *r, uint32_t ctx, uint64_t stream, int fd,
			      uint64_t offset, const void *data, size_t length, uint64_t *flags)
{
	struct transfer tx = { .fd = fd, .data = data, .length = length };
	struct event ev;
	pthread_t thr;
	uint64_t tag = cmd_stage(r, ctx, IORING_OPAQUE_RECV_OBJECT, stream, offset, length, NULL);

	require(!pthread_create(&thr, NULL, writer, &tx), "producer");
	require(wait_tag_to(r, tag, 30000, &ev) && ev.res == (int)length, "RECV_OBJECT");
	pthread_join(thr, NULL);
	*flags = ev.extra[1];
	return ev.extra[0];
}

/*
 * N1 with the v7 knobs: a greedy stream, an eviction, then a framed reply on
 * another connection. reserve/window select the configuration.
 */
static void test_greedy_configured(uint64_t reserve, uint32_t window)
{
	struct io_uring_opaque_config cfg = default_cfg();
	struct io_uring_opaque_frame frame = {
		.size = sizeof(frame), .prefix = (uintptr_t)"HDR:", .prefix_length = 4,
	};
	struct burst_writer w = { .burst = 64U << 10, .bursts = 64 };
	struct io_uring_opaque_stream_stat ss;
	struct event ev, s1, s2;
	struct ring r;
	uint32_t ctx;
	uint64_t sa, sb, hb, hb2, t;
	unsigned int i, notes = 0;
	int a[2], b[2], dst[2];
	pthread_t thr;

	cfg.hard_limit = 256U << 10;
	cfg.compact_headroom = 64U << 10;
	cfg.reply_reserve = reserve;
	cfg.capture_window = window;
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(a);
	tcp_pair(b);
	tcp_pair(dst);
	sb = attach(&r, ctx, b[1], COLLECTOR_TAG + 1);
	hb = publish(&r, ctx, sb, b[0], 0, "request!", 8);
	hb2 = publish(&r, ctx, sb, b[0], 8, "cached!!", 8);
	sa = attach(&r, ctx, a[1], COLLECTOR_TAG);
	w.fd = a[0];
	require(!pthread_create(&thr, NULL, burst_writer_fn, &w), "flood");
	for (i = 0; i < 200 && !notes; i++)
		if (wait_tag_to(&r, COLLECTOR_TAG, 20, &ev) && ev.res == -ENOBUFS)
			notes++;
	require(!command(&r, ctx, IORING_OPAQUE_FREE, hb2, 0, 0, NULL).res, "FREE cached");
	usleep(50000);
	t = frame_stage(&r, ctx, dst[0], hb, 8, &frame, IORING_OPAQUE_SEND_LAST, 0);
	require(wait_tag_to(&r, t, 5000, &s1), "framed reply");
	t = frame_stage(&r, ctx, dst[0], 0, 0, &frame, 0, 0);
	require(wait_tag_to(&r, t, 5000, &s2), "generated-only reply");
	require(!command(&r, ctx, IORING_OPAQUE_STREAM_STAT, sa, 0, sizeof(ss), &ss).res,
		"stream stat");
	printf("[N1-cfg reserve=%llu window=%u] pressure CQEs=%u greedy undecided=%llu; "
	       "framed SEND_LAST=%d generated-only=%d -> %s\n", (unsigned long long)reserve,
	       window, notes, (unsigned long long)ss.undecided_bytes, s1.res, s2.res,
	       s1.res == 12 && s2.res == 4 ? "ok" : "replies still blocked");
	shutdown(a[0], SHUT_RDWR);
	pthread_join(thr, NULL);
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, sa, 0, 0, NULL);
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, sb, 0, 0, NULL);
	ring_exit(&r);
	close(a[0]); close(a[1]); close(b[0]); close(b[1]); close(dst[0]); close(dst[1]);
}

static void test_n1_reserve(void) { test_greedy_configured(64U << 10, 0); }
static void test_n1_window(void) { test_greedy_configured(0, 16U << 10); }

/*
 * L: compaction is meant to save memory, but its replacement reservation
 * counts toward high_watermark. With hysteresis, the store stays in LOWMEM
 * after the compaction lowers occupancy, and new objects are forward-only.
 */
static void test_compaction_latch(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	const size_t big = 1U << 20, step = 128U << 10;
	unsigned char *data = calloc(1, big);
	struct io_uring_opaque_stat before, after;
	struct event ev, sx;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, off = 0, hbig, h, flags, t;
	unsigned int n = 0;
	int s[2], d[2];

	cfg.hard_limit = 8U << 20;
	cfg.compact_headroom = 2U << 20;
	cfg.reply_reserve = 256U << 10;
	cfg.capture_window = 64U << 10;
	cfg.high_watermark = 4U << 20;
	cfg.low_watermark = 2U << 20;
	require(data, "buffer");
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(s);
	tcp_pair(d);
	stream = attach(&r, ctx, s[1], COLLECTOR_TAG);
	hbig = publish_flags(&r, ctx, stream, s[0], off, data, big, &flags);
	off += big;
	for (;;) {
		before = store_stat(&r, ctx);
		if (before.backing_bytes + big >= cfg.high_watermark + step)
			break;
		publish_flags(&r, ctx, stream, s[0], off, data, step, &flags);
		off += step;
		n++;
	}
	printf("[L] %u+1 cached objects: backing=%llu high=%llu low=%llu mode=%s\n", n,
	       (unsigned long long)before.backing_bytes, (unsigned long long)cfg.high_watermark,
	       (unsigned long long)cfg.low_watermark, mode_name(before.memory_mode));
	ev = command(&r, ctx, IORING_OPAQUE_COMPACT, hbig, 0, 0, NULL);
	after = store_stat(&r, ctx);
	printf("[L] COMPACT(1 MiB object)=%d: backing %llu -> %llu, mode %s -> %s, compactions=%llu\n",
	       ev.res, (unsigned long long)before.backing_bytes,
	       (unsigned long long)after.backing_bytes, mode_name(before.memory_mode),
	       mode_name(after.memory_mode), (unsigned long long)after.compact_count);
	h = publish_flags(&r, ctx, stream, s[0], off, "new value", 9, &flags);
	off += 9;
	t = send_stage(&r, ctx, d[0], h, 0, 9);
	require(wait_tag_to(&r, t, 5000, &sx), "reusable SEND");
	printf("[L] next RECV_OBJECT flags=%#llx (FORWARD_ONLY=%d); reusable SEND of it=%d\n",
	       (unsigned long long)flags, !!(flags & IORING_OPAQUE_OBJECT_F_FORWARD_ONLY), sx.res);
	printf("[L] RESULT: %s\n", before.memory_mode == IORING_OPAQUE_MEMORY_NORMAL &&
	       after.memory_mode == IORING_OPAQUE_MEMORY_LOWMEM &&
	       after.backing_bytes <= before.backing_bytes ?
	       "compaction lowered occupancy but latched LOWMEM; new objects are forward-only" :
	       "no latch observed");
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL);
	ring_exit(&r);
	close(s[0]); close(s[1]); close(d[0]); close(d[1]);
	free(data);
}

/*
 * R: with reply_reserve set, framing can only use the reserve. Two replies
 * blocked on slow peers fill it, and a reply on a healthy connection fails
 * although the store is nearly empty.
 */
struct sink {
	int fd;
	size_t want;
};

static void *sink_thread(void *arg)
{
	struct sink *k = arg;
	unsigned char buf[16384];
	size_t done = 0;

	while (done < k->want) {
		ssize_t n = read(k->fd, buf, sizeof(buf));

		if (n <= 0)
			break;
		done += n;
	}
	return NULL;
}

static void test_reserve_cap(uint64_t reserve)
{
	struct io_uring_opaque_config cfg = default_cfg();
	static char prefix[3000];
	struct io_uring_opaque_frame big = {
		.size = sizeof(big), .prefix = (uintptr_t)prefix, .prefix_length = sizeof(prefix),
	};
	struct io_uring_opaque_frame small = {
		.size = sizeof(small), .prefix = (uintptr_t)"HDR:", .prefix_length = 4,
	};
	const size_t olen = 1U << 20;
	unsigned char *data = calloc(1, olen);
	struct io_uring_opaque_stat stat;
	struct event e1, e2, e3;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, hx, hy, t1, t2, t3;
	int s[2], p1[2], p2[2], d[2];

	memset(prefix, 'P', sizeof(prefix));
	cfg.hard_limit = 8U << 20;
	cfg.reply_reserve = reserve;
	require(data, "buffers");
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(s);
	tcp_pair_small(p1, 4096, 4096);
	tcp_pair_small(p2, 4096, 4096);
	tcp_pair(d);
	stream = attach(&r, ctx, s[1], COLLECTOR_TAG);
	hx = publish(&r, ctx, stream, s[0], 0, data, olen);
	hy = publish(&r, ctx, stream, s[0], olen, "reply!", 6);
	t1 = frame_stage(&r, ctx, p1[0], hx, olen, &big, 0, 0);
	t2 = frame_stage(&r, ctx, p2[0], hx, olen, &big, 0, 0);
	submit_only(&r);
	usleep(100000);
	stat = store_stat(&r, ctx);
	t3 = frame_stage(&r, ctx, d[0], hy, 6, &small, 0, 0);
	require(wait_tag_to(&r, t3, 5000, &e3), "healthy reply");
	printf("[R reserve=%llu] two replies blocked on slow peers: framing=%llu, backing=%llu of %llu; "
	       "4-byte-header reply on a healthy connection=%d\n", (unsigned long long)reserve,
	       (unsigned long long)stat.framing_bytes, (unsigned long long)stat.backing_bytes,
	       (unsigned long long)cfg.hard_limit, e3.res);
	{
		struct sink k1 = { .fd = p1[1], .want = olen + sizeof(prefix) };
		struct sink k2 = { .fd = p2[1], .want = olen + sizeof(prefix) };
		pthread_t r1, r2;

		require(!pthread_create(&r1, NULL, sink_thread, &k1) &&
			!pthread_create(&r2, NULL, sink_thread, &k2), "sinks");
		require(wait_tag_to(&r, t1, 30000, &e1) && wait_tag_to(&r, t2, 30000, &e2),
			"slow replies");
		pthread_join(r1, NULL);
		pthread_join(r2, NULL);
	}
	printf("[R reserve=%llu] slow replies completed: %d, %d; RESULT: %s\n",
	       (unsigned long long)reserve, e1.res, e2.res,
	       e3.res == -ENOBUFS ? "reply_reserve is a hard cap on framing" : "ok");
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL);
	ring_exit(&r);
	close(s[0]); close(s[1]); close(p1[0]); close(p1[1]);
	close(p2[0]); close(p2[1]); close(d[0]); close(d[1]);
	free(data);
}

static void test_r_reserve(void) { test_reserve_cap(8U << 10); }
static void test_r_legacy(void) { test_reserve_cap(0); }

/*
 * W: a header longer than capture_window. INSPECT sees only the window, the
 * rest never arrives until the application makes a consuming decision.
 */
static void test_window_header(void)
{
	struct io_uring_opaque_config cfg = default_cfg();
	unsigned char msg[6100], buf[16384];
	struct event ev, i1, i2, rk;
	struct ring r;
	uint32_t ctx;
	uint64_t stream, t;
	unsigned int notes = 0;
	int s[2];
	bool got;

	memset(msg, 'H', 6000);
	memset(msg + 6000, 'B', 100);
	cfg.capture_window = 4096;
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(s);
	stream = attach(&r, ctx, s[1], COLLECTOR_TAG);
	write_all(s[0], msg, sizeof(msg));
	usleep(100000);
	i1 = command(&r, ctx, IORING_OPAQUE_INSPECT, stream, 0, sizeof(buf), buf);
	t = cmd_stage(&r, ctx, IORING_OPAQUE_INSPECT, stream, i1.res > 0 ? i1.res : 0,
		      sizeof(buf), buf);
	got = wait_tag_to(&r, t, 1000, &i2);
	while (wait_tag_to(&r, COLLECTOR_TAG, 10, &ev))
		notes += ev.res == -ENOBUFS && (ev.flags & IORING_CQE_F_MORE);
	printf("[W] 6000-byte header, 4096-byte window: INSPECT(0)=%d; INSPECT(%d) %s; "
	       "pressure CQEs=%u\n", i1.res, i1.res, got ? "completed" : "still pending after 1 s",
	       notes);
	/* Spill the window into an object to make progress without losing bytes. */
	rk = command(&r, ctx, IORING_OPAQUE_RECV_OBJECT, stream, 0, i1.res, NULL);
	if (!got)
		got = wait_tag_to(&r, t, 5000, &i2);
	printf("[W] after RECV_OBJECT(0, %d)=%d: INSPECT(%d)=%d\n", i1.res, rk.res, i1.res,
	       got ? i2.res : 0);
	printf("[W] RESULT: %s\n", i1.res == 4096 && rk.res == 4096 && got && i2.res > 0 ?
	       "header beyond the window needs a spill-to-object decision" : "unexpected");
	command(&r, ctx, IORING_OPAQUE_FREE, rk.extra[0], 0, 0, NULL);
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL);
	ring_exit(&r);
	close(s[0]);
	close(s[1]);
}

/*
 * C: copy cost of the capture window. A 16 KiB message (100-byte header and
 * body) arrives before the application parses the header, as it usually
 * does. Count copied bytes after INSPECT + RECV_OBJECT of the whole message.
 */
static void test_window_copies(uint32_t window)
{
	struct io_uring_opaque_config cfg = default_cfg();
	const size_t len = 16U << 10;
	unsigned char *msg = calloc(1, len), hdr[128];
	struct io_uring_opaque_stat stat;
	struct event ins, obj;
	struct ring r;
	uint32_t ctx;
	uint64_t stream;
	int s[2];

	cfg.capture_window = window;
	require(msg != NULL, "buffer");
	require(!ring_init(&r, IORING_SETUP_CQE32), "ring");
	require(!register_store(&r, &cfg, &ctx), "store");
	tcp_pair(s);
	stream = attach(&r, ctx, s[1], COLLECTOR_TAG);
	write_all(s[0], msg, len);
	usleep(100000);
	ins = command(&r, ctx, IORING_OPAQUE_INSPECT, stream, 0, sizeof(hdr), hdr);
	obj = command(&r, ctx, IORING_OPAQUE_RECV_OBJECT, stream, 0, len, NULL);
	stat = store_stat(&r, ctx);
	printf("[C window=%u] 16 KiB message: INSPECT=%d RECV_OBJECT=%d copied_bytes=%llu backing=%llu\n",
	       window, ins.res, obj.res, (unsigned long long)stat.copied_bytes,
	       (unsigned long long)stat.backing_bytes);
	command(&r, ctx, IORING_OPAQUE_FREE, obj.extra[0], 0, 0, NULL);
	command(&r, ctx, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL);
	ring_exit(&r);
	close(s[0]);
	close(s[1]);
	free(msg);
}

static void test_c_window(void) { test_window_copies(64U << 10); }
static void test_c_legacy(void) { test_window_copies(0); }

static void run(const char *name, void (*fn)(void))
{
	int status;
	pid_t pid = fork();

	if (!pid) {
		alarm(120);
		fn();
		_exit(0);
	}
	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status))
		printf("[%s] test process killed by signal %d\n", name, WTERMSIG(status));
	else if (WEXITSTATUS(status))
		printf("[%s] test process exited with %d\n", name, WEXITSTATUS(status));
}

int main(int argc, char **argv)
{
	const char *only = argc > 1 ? argv[1] : NULL;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (!only || strchr(only, 'N')) {
		run("N1-reserve", test_n1_reserve);
		run("N1-window", test_n1_window);
	}
	if (!only || strchr(only, 'L'))
		run("L", test_compaction_latch);
	if (!only || strchr(only, 'R')) {
		run("R-reserve", test_r_reserve);
		run("R-legacy", test_r_legacy);
	}
	if (!only || strchr(only, 'W'))
		run("W", test_window_header);
	if (!only || strchr(only, 'C')) {
		run("C-window", test_c_window);
		run("C-legacy", test_c_legacy);
	}
	return 0;
}
```
