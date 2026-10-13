You are a Linux kernel developer. Implement one specified change in an io_uring patch
series, exactly as specified, then prove it with builds and tests. You did the previous
round of this series (v8); this is the follow-up.

## Repository

- This machine, repo `/mnt/workspace/ceph-client`.
- Start from the local branch `opaque-obj-v8-capture` at commit `de3962f1a12b`. It is not
  on GitHub. It holds five patches on top of `af32da41b032`:
  1. `fc0538f7d01a` net: add an optional exclusive TCP receive claim
  2. `24604c740774` io_uring: add opaque TCP stores, framed sends and lowmem forwarding
  3. `18ba715bb8dd` io_uring: test opaque ownership, memory modes and compaction
  4. `07aa0f788365` selftests/io_uring: exercise opaque lowmem, quota and SMP cancellation
  5. `de3962f1a12b` docs: describe opaque forwarding, memory and ownership contracts
- Work on a new branch, `opaque-obj-v9-round1`. Don't modify `opaque-obj-v8-capture`.

## Specification

`opus_v9_round1_spec.md` in the repo root is the complete, normative specification.
Read all of it before changing anything.

- It was applied to a private prototype, and every result it states was observed there.
- Follow it literally. Every old block appears exactly once; replace it with the new
  block exactly.
- The commit messages in its §5 are final text. Use them byte for byte.
- If an instruction seems wrong, ambiguous or impossible, stop and ask. Don't guess.
- If you notice anything else that looks stale or wrong, list it in your report. Don't
  edit it.

What the change does:

- A one-line fix in `io_opaque_actor()` for a stall in v8's collection gate.
- A regression selftest and one positive ABI check.
- Replacement changelogs for patches 2-5, which still describe the removed capture window.
- Six documentation corrections.

## Rules

1. **Keep exactly five patches, and put each change in the patch that owns the file:**
   - `io_uring/opaque.c` in patch 2;
   - the selftest in patch 4;
   - the documentation in patch 5.

   Patch 3 changes only its commit message. Patch 1 must not change: it keeps SHA
   `fc0538f7d01a`.
2. **Commit the file changes as fixups,** with `git commit --fixup=<sha>`, then
   `GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash af32da41b032`.
3. **Set the four commit messages.** Write each §5 message to a file and set it with
   `git commit --amend --cleanup=verbatim -F <file>`, for example from a
   `git rebase --exec` script that selects the file by subject. Afterwards
   `git log -1 --format=%B <commit>` must print the §5 block followed by one empty line.
4. **Don't add `Signed-off-by:` lines** and don't add or remove trailers. The existing
   `Assisted-by: Codex:GPT-6` and `Assisted-by: GLM:glm-5.3` are already in the §5 text.
5. **Change nothing outside the specification.** No refactoring, renames, new features,
   new UAPI or extra wakeups.
6. **Never suppress a diagnostic.**
   - Quote every `BUG:`, `WARNING:`, `KASAN:`, `refcount_t:`, lockdep or kmemleak report
     in your report, and attribute it from its backtrace.
   - Never disable a detector, rebuild without it and treat the report as gone.
   - Spec §7.5 lists the only known UML noise.
7. **Don't claim anything you didn't run.** Keep `/mnt/workspace/opaque-vm` from v8
   intact; put all new build and root directories under `/mnt/workspace/opaque-vm/v9/`.

## Verification (required)

Build and test under User-Mode Linux, the same way as in v8.

1. **Kernel config.** Copy `/mnt/workspace/opaque-vm/build-final/.config`, your v8
   acceptance config. It already has `SMP`, `NR_CPUS=4`, `PREEMPT`, `KASAN`,
   `PROVE_LOCKING`, `DEBUG_ATOMIC_SLEEP`, `FAILSLAB`, `FAULT_INJECTION_DEBUG_FS`,
   `FAULT_INJECTION_STACKTRACE_FILTER`, `IPV6`, `MEMCG`, `KUNIT`,
   `IO_URING_OPAQUE_OBJ`, `IO_URING_OPAQUE_OBJ_KUNIT_TEST` and `HOSTFS`, and no
   `DEBUG_KMEMLEAK`.
   - Run `make ARCH=um olddefconfig` with `O=` set to a new directory.
   - Build with `make ARCH=um O=<dir> linux`.
2. **Headers.** `make ARCH=x86_64 O=<hdr> headers_install INSTALL_HDR_PATH=<hdr>/usr`.
3. **Selftest.**
   `gcc -static -O2 -Wall -Wextra -I<hdr>/usr/include -Itools/testing/selftests/io_uring -o opaque_obj tools/testing/selftests/io_uring/opaque_obj.c -pthread`.
   It must compile without warnings.
4. **Root filesystem.** Same as v8: a hostfs root with a static busybox and an `/init`
   that does the following, then powers off:
   - mounts proc, sysfs, devtmpfs, debugfs and cgroup2;
   - runs `echo +memory > /sys/fs/cgroup/cgroup.subtree_control`;
   - brings up `lo`;
   - runs `/opaque_obj` and prints its exit status.
5. **Boot.**
   `./linux mem=2G seccomp=on ncpus=2 rootfstype=hostfs rootflags=<root> rw init=/init OPAQUE_TEST_TIMEOUT=7000 con=null con0=null,fd:1`.
   KUnit runs at boot; `kunit.enable=1` is not required.
6. **Fail-before check.** Put the new `opaque_obj` binary in a second root directory and
   boot the v8 kernel with it: `/mnt/workspace/opaque-vm/build-final/linux`, whose
   `include/config/kernel.release` is `7.3.0-rc6-gde3962f1a12b`. The run must print
   `ok 53 ...` and then `Bail out! CQ wait deadline`.
7. **Build matrix.** Same as v8:
   - each of the five commits builds with the opaque options and `KUNIT_ALL_TESTS=y`
     forced on;
   - `W=1` on `io_uring/` plus the changed `net/` objects;
   - `CONFIG_IO_URING_OPAQUE_OBJ=n`;
   - native `ARCH=i386` with `HIGHMEM4G`;
   - `checkpatch.pl --strict` on the five exported patches.

## Acceptance

All of spec §8:

- KUnit `# Totals: pass:27 fail:0 skip:0 total:27`;
- selftest `# Totals: pass:94 fail:0 xfail:0 xpass:0 skip:0 error:0`, with the
  `SEND races:` line;
- the fail-before bail-out;
- no unexplained kernel diagnostic;
- every build in the matrix;
- checkpatch with only the missing `Signed-off-by` and MAINTAINERS file-path warnings;
- both §8.7 text checks printing nothing.

## Report back with

1. The final commit IDs, and
   `git range-diff af32da41b032..de3962f1a12b af32da41b032..HEAD`.
2. Copied from the console logs:
   - the KUnit totals line;
   - the selftest totals line;
   - the `SEND races:` line;
   - the `ok 94 ...` line;
   - from the fail-before run, the `ok 53 ...` and `Bail out!` lines.
3. The build-matrix results, checkpatch results and the output of the §8.7 text checks.
4. Every kernel diagnostic you saw in any run, quoted, with its attribution.
5. Anything you could not do or could not verify, or did differently from this prompt,
   stated plainly.
