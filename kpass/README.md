# kpass development on Linux 7.2

The `opaqu_objects` branch carries the existing kpass TCP forwarding
prototype as the starting point for opaque objects and their evaluation.
Its kernel module captures page references with `tcp_read_sock` and sends
with `MSG_SPLICE_PAGES`; the application coordinates buffer handles.

## Source baseline

| Component | Pinned revision |
| --- | --- |
| Linux stable | `v7.2.9`, `5fce161649b4d779d1b76d9fcd52dc77779774b8` |
| Original kpass branch | `origin/kpass`, `f55aa3a997652da7bd5ecd7daa326a65177b4d2d` |
| Imported kpass patch | `59ae49afff3337936fe556422e46a610652cea72` |
| Import on this branch | `ae376b138283` |
| Initial Linux 7.2 port | `9c2a31ff39d7` |
| Userspace dependency | liburing `2.15`, `d41bf9220ec39277ff235379e9089d9e0fd6c2a5` |

The local `master` was fast-forwarded to `v7.2.9` before creating this
branch. The import retains its original author and a cherry-pick provenance
line. Its `.gitignore` conflict was resolved by retaining both upstream
exclusions and kpass build exclusions.

Use 7.2 stable releases for this work. Upgrade to 7.3 after the final
release is available, and validate that upgrade before changing an
evaluation baseline. Keep the import and port fixes as separate commits.

## Reproduce the build

Run these commands from the Linux repository root. Required build tools
include GCC, binutils, make, flex, bison, bc, pkg-config, and the OpenSSL and
libelf development headers. The initial build used GCC 11.4.0 on x86-64.

Build liburing in a local prefix; the host's liburing 2.0 is too old for
this command transport. The build currently requires liburing 2.15 or
newer, and 2.15 is the pinned dependency for this baseline.

```sh
research=$(cd .. && pwd)
uring_src="$research/deps/liburing-2.15"
uring_prefix="$research/deps/liburing-2.15-install"
build="$research/build/opaqu_objects-7.2.9"

# First-time dependency setup; reuse this checkout on subsequent builds.
mkdir -p "$research/deps"
git clone --depth 1 --branch liburing-2.15 \
    https://github.com/axboe/liburing.git "$uring_src"
(
    cd "$uring_src"
    ./configure --prefix="$uring_prefix" \
        --libdir="$uring_prefix/lib" --libdevdir="$uring_prefix/lib"
    make -j4 library
    make install
)

export PKG_CONFIG_PATH="$uring_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$uring_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# First-time kernel configuration.
mkdir -p "$build"
make O="$build" x86_64_defconfig
scripts/kconfig/merge_config.sh -m -O "$build" \
    "$build/.config" kpass/configs/x86_64-build.config
make O="$build" olddefconfig

# Repeat these commands for incremental builds.
make -j16 O="$build" bzImage modules
make -C kpass -j4 lib demos check
make -C kpass KDIR="$build" module
```

The profile is an initial compile baseline based on `x86_64_defconfig`.
It is not a hardware-specific deployment configuration or the eventual
debug/performance evaluation configuration. Build the kernel before the
external module so that module symbol checking uses its `Module.symvers`.

Outputs:

- Kernel image: `$build/arch/x86/boot/bzImage`.
- Kernel configuration and symbols: `$build/.config`, `$build/Module.symvers`.
- Module: `kpass/kernel/ceph_kpass.ko`.
- Libraries: `kpass/lib/libceph_kpass.{a,so}`.
- Demos: `kpass/demo/echo_server`, `kpass/demo/test_client`.

## Verification and remaining work

On 2026-10-05, the kernel image and configured modules, external kpass
module, library, and demos built successfully. `make -C kpass check`
passed without loading a module. It checks real library command
preparation with two SQE128 entries: opcode, device FD, payload fields,
entry isolation, and rejection of undersized SQEs. The same test failed
against the imported library because command preparation returned NULL.

Port fixes use the 7.2 socket address types and io_uring SQE accessor,
require SQE128 for the 64-byte command, remove an invalid preprocessor
test of an enum value, and correct the liburing command helper's argument
order. The build resolves liburing through pkg-config.

The RX changes retain eligible page-backed linear heads using Linux splice's
`skb_head_is_locked` rule, walk chained skbs, and copy unsafe backing. They
also fix receive bounds, page-boundary handling, and capture reference
release. See [KPASS.md](KPASS.md#rx-path-tcp_read_sock--actor) for the rules.

A separate 7.2.9 kernel with KUnit and `DEBUG_VM` was booted in QEMU. The
module loaded, all 13 RX tests passed, and the module unloaded. An initial
regression test failed against the original actor: it copied a page-backed
head instead of retaining its page. The complete suite includes a live
loopback TCP test through `tcp_read_sock`; its 4,224 bytes were captured with
zero RX fallback bytes on this x86-64 test configuration. This measures the
actor's copies, not copies performed by the sender or other network layers.

Full forwarding through the userspace/io_uring API and physical NIC traffic
have not been tested. Before evaluation, address TX ranges, partial sends,
pool reuse while TCP retains pages, connect/EOF completion, completion
context, and cancellation and teardown. There are no performance results
yet. The new object API is not implemented here.

## RX kernel tests

The test module includes `kernel/ceph_kpass_test.c` only when built with
`KPASS_KUNIT_TEST=1`. Tests call the actual actor with constructed skbs to
check page identity, reference lifetime after skb release, cloned/slab
fallbacks, cross-page heads/fragments, nested `frag_list`, mutable shared
fragments, limits, unreadable data, and release/reuse. A real page pool checks
recycling behavior. A loopback TCP connection checks a short read followed
by capture of the remaining stream and retention after socket release.

Build a separate kernel so the normal build profile stays available:

```sh
research=$(cd .. && pwd)
test_build="$research/build/opaqu_objects-rx-test"
mkdir -p "$test_build"
cp "$research/build/opaqu_objects-7.2.9/.config" "$test_build/.config"
scripts/config --file "$test_build/.config" --enable KUNIT \
    --disable KUNIT_ALL_TESTS --enable DEBUG_VM \
    --set-str LOCALVERSION -opaqu-rx-test
make O="$test_build" olddefconfig
make -j16 O="$test_build" bzImage modules
kpass/tests/run_rx_kunit.sh "$test_build"
```

The runner requires QEMU (`qemu-system-x86_64`), a statically linked BusyBox,
cpio, file, and timeout. Set `BUSYBOX` if the static binary is not the default.
It builds the test module, creates a minimal initramfs, enables guest
loopback, loads the module to run KUnit, unloads it, and powers down the VM.
It uses unprivileged QEMU TCG by default; set `KPASS_QEMU_ACCEL=kvm` if the
current user can access `/dev/kvm`. It never loads the module on the host.
The first regression run used KVM; the complete 13-test run also passed with
TCG using this runner.

The runner cleans generated files in `kpass/kernel` to satisfy Kbuild's
separate-output requirement. Its module and logs are under
`$test_build/kpass-rx-test/`; `results.log` records every case and the TCP
copy counts. The runner fails for failed/skipped tests, a missing completion
marker, failed module load/unload, or kernel warning/oops/panic. Rebuild the
normal module with the normal `KDIR` when needed.

## Design references

[KPASS.md](KPASS.md) documents the imported prototype. The detailed source
review and proposed object/evaluation design live in the sibling
`zc_proxy` paper repository: `KPASS.md`, `OBJECTS.md`, `KV_CACHE.md`, and
`workplan.md`.
