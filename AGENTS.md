# OPAQUE_OBJ kernel tree: agent guide

This directory is a Linux kernel tree: 7.3-rc6 plus the io_uring OPAQUE_OBJ RFC series. It is the core directory for three kinds of work:

- kernel work on the series, including performance optimization;
- userspace utilities and tests for the series;
- integration of the series into ceph-fuse (zero-copy reads).

Read these first, in this order:

1. `Documentation/userspace-api/io_uring-opaque.rst`. It defines the ABI and its ownership, memory and ordering contracts. It is the authority for current behavior.
2. `Documentation/process/coding-assistants.rst`. It holds the kernel's rules for AI agents, including the procedure for finding and fixing bugs.
3. The Status section below, then your task ledger (see Working rules).

General engineering rules are in `/mnt/workspace/KERNEL_AGENTS.md`. opencode loads that file through `opencode.json` in this directory. Other agents must read it explicitly.

This file, `opencode.json`, `.opencode/agents/` and the notes in the directory map live in *auxiliary commits* on the local branch `osdi/opaque-obj`. That is the OSDI project branch, where all project results land. Every auxiliary commit's subject starts with `aux:`, and auxiliary commits sit above the series. They must never enter the series or an `upstream/` branch. `DIRMAP.md` maps the directories on this machine.

## The mechanism

OPAQUE_OBJ keeps received TCP payload in the kernel as *opaque objects*. An opaque object is an immutable byte range that userspace names by a *handle*. Userspace never maps the payload. It inspects header bytes and declares byte ranges. Then it sends, holds or frees objects by handle. Research notes call the mechanism *kpass*.

| Term | Meaning |
|---|---|
| Store | A pool of objects with a byte budget. `IORING_REGISTER_OPAQUE_STORE` creates it. Rings can export and import a store to share it. |
| Collector | `IORING_OP_OPAQUE_COLLECT`. It claims a TCP socket exclusively (patch 1) and pulls bytes with `tcp_read_sock()`. |
| Undecided bytes | Collected bytes that no INSPECT, RECV_OBJECT or DISCARD has decided. While a stream holds them, the collector takes no more bytes from TCP. Exceptions: a waiting INSPECT, or a decision further ahead in the stream. |
| Controls | `IORING_OP_OPAQUE_OBJ`, with the operation in `sqe->ioprio`: INSPECT (copy available bytes without consuming them), RECV_OBJECT (one CQE when the declared range is complete), DISCARD, READ_OBJECT, FREE, STAT, OBJECT_STAT, STREAM_STAT, STREAM_CLOSE, COMPACT, SET_POLICY. |
| Framed send | `IORING_OP_OPAQUE_OBJ_SEND`. It sends a generated prefix, an object and a generated suffix as one request with one CQE. With `IORING_OPAQUE_SEND_LAST` the send consumes the handle, and the CQE carries `IORING_CQE_F_OPAQUE_CONSUMED`. |
| Memory policy | `high_watermark` and `low_watermark` switch the store between NORMAL and LOWMEM mode. The watermarks count payload held; they exclude in-flight compaction copies and reply framing. `reply_reserve` is a floor for reply framing. `IORING_OPAQUE_OBJECT_F_FORWARD_ONLY` is advisory. |
| Compaction | COMPACT copies a fragmented object into dense pages under the same handle. `compact_headroom` bounds the copies. |

## The patch series

The series is five patches on `af32da41b032` (the net-7.3-rc7 merge). Keep exactly five. Each file belongs to one patch:

| # | v9 SHA | Subject | Files |
|---|---|---|---|
| 1 | `fc0538f7d01a` | net: add an optional exclusive TCP receive claim | `include/linux/net.h`, `include/net/sock.h`, `net/` |
| 2 | `fb4031f775b2` | io_uring: add opaque TCP stores, framed sends and lowmem forwarding | the UAPI headers, `include/linux/io_uring_types.h`, `io_uring/opaque*` except the test, `io_uring/{cancel,io_uring,opdef,register}.c`, the feature's Kconfig and Makefile entries |
| 3 | `e7b8e47b15ce` | io_uring: test opaque ownership, memory modes and compaction | `io_uring/opaque_test.c` (KUnit) and its Kconfig and Makefile entries |
| 4 | `bcf1a650e436` | selftests/io_uring: exercise opaque lowmem, quota and SMP cancellation | `tools/testing/selftests/io_uring/`, `tools/testing/selftests/Makefile`, `MAINTAINERS` |
| 5 | `875cf36709ca` | docs: describe opaque forwarding, memory and ownership contracts | `Documentation/userspace-api/` |

Branches. The remote `origin` is `git@github.com:Markuze/ceph-client.git`.

- `osdi/opaque-obj` (local) is the working branch and the OSDI project branch. It holds the five series commits, then the auxiliary (`aux:`) commits. Keep the checkout on it, so that this file and the results stay present.
- `work/opaque-obj-v9` holds the series plus the earlier single notes commit. It is pushed as `origin/work/opaque-obj-v9`. Don't rewrite it.
- `upstream/opaque-obj-v9` is the current posted version. The local branch `opaque-obj-v9-round2` tracks it.
- Earlier posted versions: `upstream/opaque-obj` (v1) and `upstream/opaque-obj-v3` to `upstream/opaque-obj-v7`. The author's notes are on `work/opaque-obj-v5` to `work/opaque-obj-v7`.
- Local only: `opaque-obj-v7.1`, `opaque-obj-v8-capture`, `opaque-obj-v9-round1`. v8 was never pushed.

To change the series:

1. Work on `osdi/opaque-obj`, or on a new branch from it. Do not rewrite a branch that you did not create. The five commits below the first `aux:` commit are the series; `git rev-list -1 --invert-grep --grep='^aux:' HEAD` prints the series tip.
2. Commit each change as a fixup of the patch that owns the file: `git commit --fixup=<sha>`.
3. Squash: `GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash af32da41b032`. The auxiliary commits stay on top.
4. If a patch changes behavior, update its message with `git commit --amend --cleanup=verbatim -F <file>`. State the problem, the root cause and the fix. Wrap lines at 75 columns.
5. Build and test every commit (see Build matrix).

Notes and results go into auxiliary commits.
- Commit a new result as its own commit whose subject starts with `aux:`.
- Change an existing one with `git commit --fixup=<aux sha>` and the same autosquash.
- Auxiliary commits always stay above the series. Never put an `aux:` commit between series commits.

Trailers and pushes:

- Agents never add `Signed-off-by:`. The owner adds it.
- The attribution rule in this tree is `Assisted-by: LLM [TOOL1] [TOOL2]` (commit `816d9992d9ed`). The v9 commits carry the older form, `Assisted-by: Codex:GPT-6` and `Assisted-by: GLM:glm-5.3`. Do not change trailers until the owner decides.
- Do not push, force-push or delete a branch unless the owner asks.

## Build and test

All validation runs under User-Mode Linux (UML). This machine has no QEMU, so the series' `run_opaque_vm.sh` cannot run here. Put every build directory, root directory and log under `/mnt/workspace/opaque-vm/<task>/`. Do not delete the existing subdirectories. Their logs are the evidence for earlier rounds.

The reference config is `/mnt/workspace/opaque-vm/build-final/.config`. It enables UML SMP (`NR_CPUS=4`), PREEMPT, KASAN, PROVE_LOCKING, MEMCG, KUnit, `IO_URING_OPAQUE_OBJ` and `IO_URING_OPAQUE_OBJ_KUNIT_TEST`.

```sh
T=/mnt/workspace/opaque-vm/<task>
mkdir -p $T/build-kernel && cp /mnt/workspace/opaque-vm/build-final/.config $T/build-kernel/
make ARCH=um O=$T/build-kernel olddefconfig
make ARCH=um O=$T/build-kernel -j32 linux
make ARCH=x86_64 O=$T/build-hdr headers_install INSTALL_HDR_PATH=$T/build-hdr/usr
gcc -static -O2 -Wall -Wextra -I$T/build-hdr/usr/include -Itools/testing/selftests/io_uring \
    -o $T/opaque_obj tools/testing/selftests/io_uring/opaque_obj.c -pthread
cp -a /mnt/workspace/opaque-vm/v9r2/root $T/root && cp $T/opaque_obj $T/root/opaque_obj
$T/build-kernel/linux mem=2G seccomp=on ncpus=2 rootfstype=hostfs rootflags=$T/root rw \
    init=/init OPAQUE_TEST_TIMEOUT=7000 con=null con0=null,fd:1 > $T/run.log 2>&1
```

The root's `/init` does these steps:

1. It mounts proc, sysfs, devtmpfs, debugfs and cgroup2, and brings up `lo`.
2. It runs `/opaque_obj` and prints `OPAQUE_SELFTEST_STATUS=<rc>`.
3. It powers off.

KUnit runs during boot. The selftest must compile without warnings.

Pass criteria in `run.log` for v9:

- The KUnit suite `io_uring-opaque` prints `# Totals: pass:28 fail:0 skip:0 total:28`.
- The selftest prints `# Totals: pass:97 fail:0 xfail:0 xpass:0 skip:0 error:0`, a `# SEND races:` line and `OPAQUE_SELFTEST_STATUS=0`.
- No kernel diagnostic appears other than the known noise below.

Test rules:

- A bug fix needs a test that fails on the kernel before the fix. Run the new test binary on the old kernel and record the failing line (for example in `$T/fail-before.log`).
- When you add a selftest, update `ksft_set_plan()` in `main()` and the test count in the commit message.
- The selftest uses raw SQEs (no liburing) on `IORING_SETUP_DEFER_TASKRUN` rings, and only loopback TCP.

Build matrix. Run all of it before you call a series final:

- **Every commit builds.** Use one `O=` directory per commit and start each from the reference config. At every commit, run `scripts/config --file <O>/.config -e IO_URING_OPAQUE_OBJ -e KUNIT_ALL_TESTS` before `olddefconfig`. At patch 1 the symbol does not exist yet, and `olddefconfig` silently drops it.
- **W=1** on `io_uring/` and the changed `net/` objects gives no new warnings.
- **Feature off:** the tree builds with `CONFIG_IO_URING_OPAQUE_OBJ=n`.
- **i386:** a native `ARCH=i386` build with `HIGHMEM4G` succeeds.
- **checkpatch:** run `./scripts/checkpatch.pl --strict` on `git format-patch af32da41b032..$(git rev-list -1 --invert-grep --grep='^aux:' HEAD)`. The only allowed reports are the missing `Signed-off-by` errors and the MAINTAINERS file-path warnings.

Diagnostics:

- Quote every `BUG:`, `WARNING:`, `KASAN:`, `refcount_t:`, lockdep and kmemleak report. Attribute each report from its backtrace. Never disable a detector to hide a report.
- Known noise under UML:
  - With kmemleak on, about 180-192 boot objects from `ssl_init`, `setup_one_line`, `parse_chan` and `pty_chan_init`.
  - With SMP, one RCU "Callback enqueued on offline CPU" splat from `os_start_secondary`.

  Neither involves opaque code.

## Status (2026-10-13)

v9 is sound to build on. All findings from the v1-v8 reviews are closed. The reviews are the `opus_review*.md` files in this directory.

Limits of the evidence:

- Validation covers UML with loopback TCP on 2 vCPUs only.
- Nothing has run on a real NIC driver, on more CPUs, under KCSAN, or on a non-debug kernel.
- There are no performance numbers yet.

The owner must still add `Signed-off-by:` and decide the trailer format.

Known limits. These are the first optimization targets:

| Limit | Where | Effect |
|---|---|---|
| Store-wide `tables` mutex | `io_opaque_resolve()`, `io_opaque_ready()`, FREE in `io_opaque_issue()` | On a shared store, every handle lookup, publication and FREE serializes on one mutex. |
| Budget wakeup walks every waiter | `io_opaque_wake_budget()` | Each budget release costs O(waiting streams) under memory pressure. |
| No collector rearm (errata E08) | Documentation: "If the pressure notification cannot fit in the CQ" | A full CQ ends collection on that stream with `ENOSPC`. Applications must size the CQ for the worst case. |
| Generic CQE flag | `IORING_CQE_F_OPAQUE_CONSUMED` is bit 6 in `include/uapi/linux/io_uring.h` | Upstream will probably ask for another encoding. Keep application code behind one helper layer. |
| Copy fallback | `io_opaque_capture()` | Cloned or shared skbs are copied, not referenced. The copied share on real drivers is unknown. |

Before you optimize:

1. Define the metric.
2. Measure it on the unchanged code.
3. Change one thing.
4. Measure again on the same setup.

A change that alters behavior also needs the full test suite and the build matrix.

## Current work

1. **Applications and evaluation.**
   - The plan of record is in the research repository `git@github.com:Markuze/zc_proxy.git` (SSH only): `workplan.md`, `experiments/kv_store.md` and `KV_CACHE.md`.
   - The target is an OSDI '27 paper: abstract on 2026-12-01, full paper on 2026-12-08.
   - The workplan orders the first steps:
     1. A performance kernel config and a testbed.
     2. A small relay as an API smoke test.
     3. A key-value store with a copy backend.
     4. The opaque backend and the baselines: SEND_ZC, RECV_ZC, sendfile, splice.
   - zc_proxy's `KPASS.md` and `experiments/kv_store.md` still describe v7. Use the in-tree documentation for v9 behavior.
2. **FUSE integration.** See below.
3. **Kernel optimization.** See the known limits above.

## FUSE integration: zero-copy ceph-fuse reads

The design is `fuse_zc.md` in this directory. It is complete for implementation, but it was written against v4 (`c5bcea485987`). No code exists yet.

`fuse_zc_workplan.md` turns the design into work packages for worker agents against v9. Where it differs from `fuse_zc.md`, the work plan wins. It is complete:
- §0: the dispatch table and waves;
- §3: the rules for every worker;
- §4: worktrees and environment;
- §5: interface contracts;
- §6: packages K1–K4 (kernel), L1–L6 (libfuse), C1–C6 (Ceph), E1 (UML integration environment), T1 (integration tests) and B1 (benchmarks);
- §8: owner decisions, with the defaults workers use.

The verified code facts behind it are in `/mnt/workspace/ceph-client/fuse_zc_facts/`.

- **Goal.** READ payload from an OSD stays an opaque object from the socket until it reaches the pages of the FUSE request. One kernel copy remains, into those pages. Today's ceph-fuse copies the payload 2 to 3 times.
- **Mechanism.** FUSE io_uring zero-copy (`43f8343858eb`, already in this base) registers the pages of each FUSE request in the server ring's buffer table. A new opcode, `IORING_OP_OPAQUE_OBJ_FILL`, copies an object into that registered buffer.

The owner's decisions:

| # | Decision |
|---|---|
| D1 | Deliver data only through FUSE io_uring zero-copy slots. No splice and no `/dev/fuse` data path. |
| D2 | One store per ceph-fuse process, shared by all rings. |
| D3 | ObjectCacher stays, and it stages objects until FUSE asks for them. |
| D4 | Payloads of at least 512 B (`ms_opaque_keep_min_bytes`) stay objects. Smaller payloads are copied. |
| D5 | The hand-off to FUSE consumes the object, as SEND_LAST does. |
| D6 | Extend libfuse with generic APIs inside its existing io_uring code. |
| D7 | Each change goes upstream to the project that owns it. |
| D8 | Reads only. Zero-copy writes are a separate project that changes FUSE itself. |

The work by project (the work plan's §0 has dependencies and waves):

| Packages | Project | Work |
|---|---|---|
| K1, K2 | this series (add-on patches) | `IORING_OP_OPAQUE_OBJ_FILL` with `LAST`; crc32c in RECV_OBJECT |
| K3 | this series | `tables`/`wait_lock` scalability, only if B1 shows contention |
| K4 | io_uring core, liburing | `IORING_OP_FIXED_COPY` |
| L1–L6 | libfuse | defer-taskrun ring mode with reply hand-off and queue hooks, zero-copy queues, `fuse_reply_uring_ops`, a worker pool, interrupts for ring requests, example and tests |
| C1–C6 | Ceph | `raw_opaque`, `OpaqueStore` and messenger rings, the receive engine, Objecter opt-in and admission, ObjectCacher hand-off and Client, ceph-fuse |
| E1, T1, B1 | all | UML integration environment, integration tests, benchmarks |

v9 against the design (`fuse_zc_workplan.md` §1 has the details):

- v9 has no FILL and no crc.
- Four v9 rules differ from v4: the capture policy, the `reply_reserve` floor, the advisory forwarding-only flag and the watermark rules.
- The capture hold bounds each stream's undecided bytes, so the work plan drops the per-stream receive reserve (`fuse_zc.md` §9.1.3).

Sources and environment:

- **Ceph:** don't work in `~/ceph`; the rust-mds project uses that checkout. Use the shared clone and per-package worktrees under `/mnt/workspace/fuse-zc/` (work plan §4). Never commit to a branch that you did not create.
- **libfuse:** a shared clone at `/mnt/workspace/fuse-zc/libfuse`, with per-package worktrees (work plan §4). The facts were taken from master `cb2e761c7635`.
- **Tests:** everything that needs the patched kernel runs under UML:
  - the series and K1–K4: as above;
  - libfuse zero copy: a v9-based UML kernel with FUSE enabled;
  - Ceph and integration tests: the E1 environment, where a UML guest reaches a vstart cluster on the host over a tap device.
  - Benchmarks (B1) need a machine with a real NIC that boots the kernel; there is none yet.

## Directory map

| Path | Content |
|---|---|
| `io_uring/opaque.c` | requests, collector, capture, sends, statistics |
| `io_uring/opaque_compact.c` | compaction workers |
| `io_uring/opaque_register.c` | ring-local store registration, export and import |
| `io_uring/opaque.h`, `io_uring/opaque_internal.h` | internal types and declarations |
| `io_uring/opaque_test.c` | KUnit tests |
| `include/uapi/linux/io_uring/opaque.h` | UAPI |
| `tools/testing/selftests/io_uring/opaque_obj.c` | selftest |
| `DIRMAP.md` | map of this machine's directories: trees, branches, build and test areas, ledgers, tools (aux) |
| `opus_review.md`, `opus_review_v4.md`, `opus_review_v6.md` … `opus_review_v8.md` | reviews of v1, v4, v6, v7 and v8 (aux) |
| `opus_*spec.md`, `opus_capture_policy_changes.md`, `glm_prompt*.md` | specifications and the prompts that handed them to GLM (aux) |
| `errata.md`, `optimizations.md` | the author's errata and flow-optimization notes, from `work/opaque-obj-v7` (aux) |
| `fuse_zc.md`, `fuse_zc_workplan.md`, `fuse_zc_facts/` | FUSE design, its work plan and the verified code facts behind it (aux) |
| `.opencode/agents/` | opencode roles (aux): `orchestrator` coordinates; `investigator` and `reviewer` are read-only; `implementer` (GLM-5.3) edits |
| `.crush/crush.db` | transcripts of GLM sessions run through Crush (SQLite; ignored, never commit) |
| `utils/` | non-upstream utilities (create on first use; see Working rules) |
| `/mnt/workspace/opaque-vm/` | UML builds, roots and logs, one subdirectory per task |
| `/mnt/workspace/current/` | task ledgers |
| `/mnt/workspace/fuse-zc/` | zero-copy ceph-fuse work: package starter, worktrees and shared clones (see `DIRMAP.md`) |
| `~/ceph` | Ceph source (shared with rust-mds) |
| `/mnt/workspace/rust-mds/` | a different project; do not modify it |

## Working rules for this tree

- **Ledger.** Keep your task ledger in `/mnt/workspace/current/<name>.md`. If `$ZELLIJ_SESSION_NAME` is set, use it as the name. Otherwise use the task name. The absolute path keeps ledgers out of every patch.
- **Staging.** Stage explicit paths only (`git add <path>`). Never run `git add -A` or `git add .`. Build output and tool state must stay out of every commit: `.crush/` and everything in `.opencode/` except `agents/`. The kernel's `.gitignore` ignores dot directories, so `.opencode/agents/*.md` needs `git add -f`.
- **Where utilities go.**
  - A test that belongs upstream goes into the series: KUnit in patch 3, or the selftest in patch 4.
  - Everything else goes into `utils/`: benchmarks, harness scripts and application prototypes.
  - `utils/` is its own git repository. On first use, run `git init utils` and add `utils/` to `.git/info/exclude`.
- **Separate directories.** Several agents work on this machine. Each agent uses its own build directories. Do not reuse another agent's `O=` directory, root directory or log.
- **Handoffs.** When the owner hands a task to another agent, do not implement that task yourself. Prepare material, answer questions, and audit the result from its logs and transcript.
- **The host kernel.** The host runs `7.0.0-1012-ibm`. Never install or boot this tree's kernel on the host.
- **Keep this file current.** When the series, the status or the commands change, update this file.

## Gotchas

- UML SMP needs `seccomp=on`. Without `mem=2G`, UML runs out of memory.
- Under UML SMP with KASAN, the selftest's default timeout is too short. Pass `OPAQUE_TEST_TIMEOUT=7000`.
- A send's framing charge is released when its request is freed, which happens slightly after its CQE. Tests must poll STAT, not read it once.
- `tail` on this machine is uutils. Write `tail -n 5`, not `tail -5`.
- `grep` on this machine is ugrep. A very long alternation pattern can fail with "exceeds complexity limits".
- Every `sudo` call prints a harmless smartctl warning from ceph-base's sudoers drop-in.
