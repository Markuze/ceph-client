#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Run the real RX actor in a disposable x86-64 guest, never on the host.
set -euo pipefail

if [ "$#" -ne 1 ]; then
	echo "Usage: $0 <built-kernel-directory-with-CONFIG_KUNIT=y>" >&2
	exit 2
fi

kernel_build=$(realpath "$1")
kpass_src=$(cd -- "$(dirname -- "$0")/.." && pwd)
test_build="$kernel_build/kpass-rx-test"
root="$test_build/initramfs"
busybox=${BUSYBOX:-$(command -v busybox)}

grep -qx 'CONFIG_KUNIT=y' "$kernel_build/.config"
file "$busybox" | grep -q 'statically linked'
mkdir -p "$root"/{bin,proc,sys,dev,tmp}

# Kbuild requires a clean module source directory when using MO=.
make -C "$kernel_build" M="$kpass_src/kernel" clean
make -C "$kernel_build" M="$kpass_src/kernel" \
	MO="$test_build/module" KPASS_KUNIT_TEST=1 modules
cp "$test_build/module/ceph_kpass.ko" "$root/ceph_kpass.ko"
cp "$busybox" "$root/bin/busybox"
make -C "$kpass_src" tests/stream_objects
cp "$kpass_src/tests/stream_objects" "$root/stream_objects"
for app in sh mount ip insmod rmmod poweroff; do
	ln -sf busybox "$root/bin/$app"
done
cat > "$root/init" <<'INIT'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs devtmpfs /dev
ip link set lo up
insmod /ceph_kpass.ko
rc=$?
echo "KPASS_INSMOD_STATUS=$rc"
if [ "$rc" -eq 0 ]; then
	/stream_objects
	echo "KPASS_STREAM_STATUS=$?"
	rmmod ceph_kpass
	echo "KPASS_RMMOD_STATUS=$?"
fi
echo KPASS_TEST_FINISHED
poweroff -f
INIT
chmod +x "$root/init"
(
	cd "$root"
	find . -print0 | cpio --null -o --format=newc > "$test_build/initramfs.cpio"
)

# TCG needs no privileged access; use KPASS_QEMU_ACCEL=kvm where accessible.
timeout "${KPASS_TEST_TIMEOUT:-60}" "${QEMU:-qemu-system-x86_64}" \
	-accel "${KPASS_QEMU_ACCEL:-tcg}" -m 512M -smp 2 -nodefaults \
	-display none -serial stdio -monitor none -no-reboot \
	-kernel "$kernel_build/arch/x86/boot/bzImage" \
	-initrd "$test_build/initramfs.cpio" \
	-append 'console=ttyS0 rdinit=/init panic=-1 kunit.enable=1' \
	> "$test_build/console.log" 2>&1

tr -d '\r' < "$test_build/console.log" > "$test_build/results.log"
grep -E 'kpass-(rx|stream):|TCP RX:|PASS:|KPASS_.*STATUS=' "$test_build/results.log"
grep -Eq '# kpass-rx: pass:[1-9][0-9]* fail:0 skip:0 total:' "$test_build/results.log"
grep -Eq '# kpass-stream: pass:[1-9][0-9]* fail:0 skip:0 total:' "$test_build/results.log"
grep -qx 'KPASS_INSMOD_STATUS=0' "$test_build/results.log"
grep -qx 'KPASS_RMMOD_STATUS=0' "$test_build/results.log"
grep -qx 'KPASS_STREAM_STATUS=0' "$test_build/results.log"
grep -qx 'KPASS_STREAM_TEST_PASSED' "$test_build/results.log"
grep -qx 'KPASS_TEST_FINISHED' "$test_build/results.log"
if grep -Eq 'not ok|BUG:|WARNING:|Oops:|Kernel panic|refcount_t:' "$test_build/results.log"; then
	echo "Kernel failure: see $test_build/results.log" >&2
	exit 1
fi
echo "RX and stream/object tests passed. Log: $test_build/results.log"
