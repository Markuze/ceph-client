You are a Linux kernel developer. Implement one specified change in an io_uring patch
series, exactly as specified, then prove it with builds and tests. You did rounds v8 and
v9-round-1 of this series; this is round 2.

## Repository

- This machine, repo `/mnt/workspace/ceph-client`.
- Start from your local branch `opaque-obj-v9-round1` at commit `19e616d733c8`. It holds
  five patches on top of `af32da41b032`:
  1. `fc0538f7d01a` net: add an optional exclusive TCP receive claim
  2. `178cb28c3d39` io_uring: add opaque TCP stores, framed sends and lowmem forwarding
  3. `0c5c6bba8e4f` io_uring: test opaque ownership, memory modes and compaction
  4. `a6c63d178e80` selftests/io_uring: exercise opaque lowmem, quota and SMP cancellation
  5. `19e616d733c8` docs: describe opaque forwarding, memory and ownership contracts
- Work on a new branch, `opaque-obj-v9-round2`. Don't modify `opaque-obj-v9-round1` or
  `opaque-obj-v8-capture`.

## Specification

`opus_v9_round2_spec.md` in the repo root is the complete, normative specification.
Read all of it before changing anything.

- It was applied to a private prototype, and every result it states was observed there.
- Follow it literally. Every old block appears exactly once; replace it with the new
  block exactly.
- Its §6 commit messages are final text.
- If an instruction seems wrong, ambiguous or impossible, stop and ask. Don't guess.
- If you notice anything else that looks stale or wrong, list it in your report. Don't
  edit it.

What the change does, the owner's four memory-policy decisions:

- Watermarks measure payload held: in-flight compaction copies and reply framing are
  excluded, behind a seqcount.
- `reply_reserve` becomes a floor: framing uses ordinary capacity first.
- Same-offset INSPECT behaviour is documented and pinned by a selftest.
- Forwarding-only becomes an advisory flag: ordinary SEND is allowed.

## Rules

1. **Keep exactly five patches, and put each change in the patch that owns the file:**
   - `io_uring/opaque.c`, `io_uring/opaque.h`, `io_uring/opaque_internal.h` and
     `io_uring/opaque_compact.c` in patch 2;
   - `io_uring/opaque_test.c` in patch 3;
   - the selftest in patch 4;
   - the documentation in patch 5.

   Patch 1 must not change: it keeps SHA `fc0538f7d01a`.
2. **Commit the changes as fixups,** with `git commit --fixup=<sha>`, then
   `GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash af32da41b032`.
3. **Set the four commit messages** byte for byte from spec §6, with
   `git commit --amend --cleanup=verbatim -F <file>`, as in round 1.
4. **Don't add `Signed-off-by:` lines,** and don't add or remove trailers.
5. **Change nothing outside the specification.** No refactoring, renames, new features or
   new UAPI.
6. **Never suppress a diagnostic.** Quote every `BUG:`, `WARNING:`, `KASAN:`,
   `refcount_t:`, lockdep or kmemleak report in your report, attributed from its
   backtrace. Never disable a detector to make a report go away.
7. **Don't claim anything you didn't run.** Keep `/mnt/workspace/opaque-vm` from earlier
   rounds intact. Put all new build and root directories under
   `/mnt/workspace/opaque-vm/v9r2/`.

## Verification (required)

Build and test under User-Mode Linux, exactly as in round 1.

1. **Kernel config.** Copy `/mnt/workspace/opaque-vm/build-final/.config`, run
   `make ARCH=um olddefconfig` with `O=` set to a new directory, then build with
   `make ARCH=um O=<dir> linux`.
2. **Headers and selftest.** `headers_install` (the UAPI does not change), then:
   `gcc -static -O2 -Wall -Wextra -I<hdr>/usr/include -Itools/testing/selftests/io_uring -o opaque_obj tools/testing/selftests/io_uring/opaque_obj.c -pthread`.
   It must compile without warnings.
3. **Root filesystem and boot.** The same `/init` as round 1. Boot with:
   `./linux mem=2G seccomp=on ncpus=2 rootfstype=hostfs rootflags=<root> rw init=/init OPAQUE_TEST_TIMEOUT=7000 con=null con0=null,fd:1`.
4. **Fail-before check.** Boot the round-1 kernel with the new `opaque_obj` binary in a
   second root directory: `/mnt/workspace/opaque-vm/v9/build-kernel/linux`, whose
   `include/config/kernel.release` is `7.3.0-rc6-g19e616d733c8`. It must print `ok 46 ...`
   and then `Bail out! forwarding-only classification is advisory for SEND`.
5. **Build matrix.** Same as before:
   - each of the five commits builds with the opaque options and `KUNIT_ALL_TESTS=y`
     forced on;
   - `W=1` on `io_uring/` plus the changed `net/` objects;
   - `CONFIG_IO_URING_OPAQUE_OBJ=n`;
   - native `ARCH=i386` with `HIGHMEM4G`;
   - `checkpatch.pl --strict` on the five exported patches.

## Acceptance

All of spec §8:

- KUnit `# Totals: pass:28 fail:0 skip:0 total:28`, including `opaque_compact_watermarks`;
- selftest `# Totals: pass:97 fail:0 xfail:0 xpass:0 skip:0 error:0`, with the
  `SEND races:` line;
- the fail-before bail-out;
- no unexplained kernel diagnostic;
- every build in the matrix;
- checkpatch with only the missing `Signed-off-by` and MAINTAINERS file-path warnings;
- all three §8.7 text checks printing nothing.

## Report back with

1. The final commit IDs, and
   `git range-diff af32da41b032..19e616d733c8 af32da41b032..HEAD`.
2. Copied from the console logs:
   - the KUnit totals line and the `opaque_compact_watermarks` result;
   - the selftest totals line;
   - the `SEND races:` line;
   - the `ok 97 ...` line;
   - from the fail-before run, the `ok 46 ...` and `Bail out!` lines.
3. The build-matrix results, checkpatch results and the output of the §8.7 text checks.
4. Every kernel diagnostic you saw in any run, quoted, with its attribution.
5. Anything you could not do or could not verify, or did differently from this prompt,
   stated plainly.
