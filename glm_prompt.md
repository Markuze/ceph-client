You are a Linux kernel developer. Implement one specified change in an io_uring patch
series, exactly as specified, then prove it with builds and tests.

## Repository

- Repo: `git@github.com:Markuze/ceph-client.git`
- Start from branch `upstream/opaque-obj-v7` at commit `e6aef56bc0f6`. It holds five
  patches on top of `af32da41b032`:
  1. `fc0538f7d01a` net: add an optional exclusive TCP receive claim
  2. `ed543e9e1031` io_uring: add opaque TCP stores, framed sends and lowmem forwarding
  3. `1c50ae1c3cdd` io_uring: test opaque ownership, memory modes and compaction
  4. `0d334f5c455d` selftests/io_uring: exercise opaque lowmem, quota and SMP cancellation
  5. `e6aef56bc0f6` docs: describe opaque forwarding, memory and ownership contracts
- Work on a new branch, `opaque-obj-v8-capture`.

## Specification

The attached file `opus_capture_policy_changes.md` is the complete, normative
specification. Read all of it before changing anything, including the appendices.

- Its code snippets and reference tests were already run on a private prototype and
  pass.
- Follow it literally: same function names, field name, polarity and placement.
- Don't redesign anything or "improve" code it says not to change.
- If an instruction seems wrong, ambiguous or impossible, stop and ask. Don't guess.

What the change does, so you know the goal:

- The collector (`io_opaque_actor()` via `tcp_read_sock()`) may take whatever has
  arrived off the socket when the stream has no undecided bytes. Undecided means
  collected but not yet covered by a RECV_OBJECT or DISCARD.
- Undecided bytes stay in their original RX buffers. They are never copied, except by
  INSPECT copying header bytes to userspace.
- While a stream has undecided bytes, nothing more is collected for it, so new bytes
  stay on the TCP socket. There are two exceptions: an INSPECT waiting for bytes past
  what has been collected, and a decision declared beyond the first uncollected byte.
- The `capture_window` mechanism is removed. Its UAPI field becomes `__u32 __resv2`,
  which must be zero.

## Rules

1. **Keep exactly five patches, and put each change in the patch that owns the file:**
   - kernel code and UAPI in patch 2;
   - KUnit in patch 3;
   - selftests in patch 4;
   - documentation in patch 5.

   Use `git commit --fixup=<sha>` for each change, then
   `GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash af32da41b032`.
2. **Every patch must build on its own** with `CONFIG_IO_URING_OPAQUE_OBJ=y`,
   `CONFIG_IO_URING_OPAQUE_OBJ_KUNIT_TEST=y` and `CONFIG_KUNIT_ALL_TESTS=y`.
3. **Change nothing outside what the specification lists.** No refactoring, no
   renames, no new features, no new UAPI.
4. **Don't add `Signed-off-by:` lines**; a human adds those. Keep the existing
   `Assisted-by:` trailers, and add your own in the format
   `Assisted-by: GLM:<model-version>`.
5. **The most common mistakes, from the specification's §5:**
   - Evaluating the gate per chunk inside the actor. It must be one flag set once per
     pass in `io_opaque_recv()`.
   - Reporting a held pass as `-ENOBUFS`, or posting a pressure CQE for it. A held pass
     is not an error; the actor just returns the bytes it consumed, possibly 0.
   - Dropping the two demand exceptions, which deadlocks split headers.
   - Removing the safety copies in `io_opaque_capture()`. Only the `bounded` term goes.
   - Inverting the flag. It is `hold_undecided`, and zero means "may collect".

## Verification (required)

Build and test under User-Mode Linux.

1. **Kernel:** `make ARCH=um` with `CONFIG_SMP=y`, `CONFIG_NR_CPUS=4`, `CONFIG_PREEMPT=y`,
   `CONFIG_KASAN=y`, `CONFIG_PROVE_LOCKING=y`, `CONFIG_DEBUG_ATOMIC_SLEEP=y`,
   `CONFIG_FAILSLAB=y`, `CONFIG_FAULT_INJECTION_DEBUG_FS=y`, `CONFIG_MEMCG=y`,
   `CONFIG_KUNIT=y`, `CONFIG_IO_URING_OPAQUE_OBJ=y`,
   `CONFIG_IO_URING_OPAQUE_OBJ_KUNIT_TEST=y` and `CONFIG_HOSTFS=y`.
2. **Headers:** `make ARCH=x86_64 O=<hdr> headers_install INSTALL_HDR_PATH=<hdr>/usr`.
3. **Selftest:** build it statically:
   `gcc -static -O2 -Wall -Wextra -I<hdr>/usr/include -Itools/testing/selftests/io_uring -o opaque_obj tools/testing/selftests/io_uring/opaque_obj.c -pthread`.
4. **Root filesystem:** a hostfs root directory with a static busybox and an `/init`
   that:
   - mounts proc, sysfs, devtmpfs, debugfs (`/sys/kernel/debug`) and cgroup2
     (`/sys/fs/cgroup`), then runs `echo +memory > /sys/fs/cgroup/cgroup.subtree_control`;
   - brings up `lo`;
   - runs the test binary and powers off.
5. **Boot:**
   `./linux mem=2G seccomp=on ncpus=2 rootfstype=hostfs rootflags=<root> rw init=/init OPAQUE_TEST_TIMEOUT=7000 con=null con0=null,fd:1`.
   SMP under UML needs `seccomp=on`, and without `mem=` the tests run out of memory.

Acceptance:

- Every result in §7 of the specification.
- KUnit `io_uring-opaque`: `# Totals: pass:27 fail:0`, including `opaque_collect_gate`.
- Selftest: `# Totals: pass:93 fail:0 ... skip:0`.
- No `BUG:`, `WARNING:`, `KASAN:`, `refcount_t:` or lockdep report anywhere in the
  console log.
- Each of the five commits builds with the options in rule 2.
- `make W=1` on `io_uring/` adds no warnings.
- `CONFIG_IO_URING_OPAQUE_OBJ=n` builds.
- `scripts/checkpatch.pl --strict` on the five exported patches reports only the
  expected missing `Signed-off-by` (plus MAINTAINERS file-path warnings).

## Report back with

1. The final commit IDs, and `git range-diff af32da41b032..e6aef56bc0f6
   af32da41b032..HEAD`.
2. The KUnit totals line, the selftest totals line and the `SEND races:` summary line,
   copied from the console log.
3. The build-matrix and checkpatch results.
4. Anything you could not do or could not verify, stated plainly. Don't claim a test
   passed unless you ran it and saw the output.
