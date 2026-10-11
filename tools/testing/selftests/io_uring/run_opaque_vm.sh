#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Boot a built kernel and a static selftest without changing the host kernel.
set -euo pipefail

if [ "$#" -ne 2 ]; then
	echo "Usage: $0 <kernel-build-directory> <static-opaque_obj-binary>" >&2
	exit 2
fi
kernel_build=$(realpath "$1")
test_binary=$(realpath "$2")
test_dir="$kernel_build/opaque-vm"
test_root="$test_dir/initramfs"
test_timeout=${OPAQUE_TEST_TIMEOUT:-600}
if [[ ! $test_timeout =~ ^[1-9][0-9]{0,8}$ ]]; then
	echo "OPAQUE_TEST_TIMEOUT must be a positive number of seconds" >&2
	exit 2
fi
busybox_binary=${BUSYBOX:-$(command -v busybox)}
file "$test_binary" | grep -q 'statically linked'
file "$busybox_binary" | grep -q 'statically linked'
mkdir -p "$test_root"/{bin,proc,sys,dev,tmp}
cp "$test_binary" "$test_root/opaque_obj"
cp "$busybox_binary" "$test_root/bin/busybox"
printf 'export OPAQUE_TEST_TIMEOUT=%s\n' "$test_timeout" > "$test_root/test.env"
for app in sh mount mkdir ip poweroff sleep; do
	ln -sf busybox "$test_root/bin/$app"
done
cat > "$test_root/init" <<'INIT'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
mkdir -p /sys/kernel/debug /sys/fs/cgroup
mount -t debugfs none /sys/kernel/debug
mount -t cgroup2 none /sys/fs/cgroup
echo +memory > /sys/fs/cgroup/cgroup.subtree_control
ip link set lo up
. /test.env
/opaque_obj
echo "OPAQUE_SELFTEST_STATUS=$?"
sleep 2
echo OPAQUE_VM_FINISHED
poweroff -f
INIT
chmod +x "$test_root/init"
(
	cd "$test_root"
	find . -print0 | cpio --null -o --format=newc > "$test_dir/initramfs.cpio"
)
timeout "${OPAQUE_VM_TIMEOUT:-360}" "${QEMU:-qemu-system-x86_64}" \
	-accel "${OPAQUE_VM_ACCEL:-tcg}" -m 768M -smp 2 -nodefaults \
	-display none -serial stdio -monitor none -no-reboot \
	-kernel "$kernel_build/arch/x86/boot/bzImage" \
	-initrd "$test_dir/initramfs.cpio" \
	-append 'console=ttyS0 rdinit=/init panic=-1 kunit.enable=1' \
	> "$test_dir/console.log" 2>&1
tr -d '\r' < "$test_dir/console.log" > "$test_dir/results.log"
grep -E 'io_uring-opaque:|^ok |^not ok |OPAQUE_' "$test_dir/results.log"
grep -qx 'OPAQUE_SELFTEST_STATUS=0' "$test_dir/results.log"
grep -qx 'OPAQUE_VM_FINISHED' "$test_dir/results.log"
if grep -qx 'CONFIG_IO_URING_OPAQUE_OBJ_KUNIT_TEST=y' "$kernel_build/.config"; then
	grep -Eq 'io_uring-opaque: pass:[1-9][0-9]* fail:0 skip:0 total:' \
		"$test_dir/results.log"
fi
if grep -Eq 'not ok|BUG:|WARNING:|Oops:|Kernel panic|refcount_t:|deprecated workqueue' \
		"$test_dir/results.log"; then
	echo "Kernel or selftest failure: $test_dir/results.log" >&2
	exit 1
fi
echo "Opaque VM tests passed: $test_dir/results.log"
