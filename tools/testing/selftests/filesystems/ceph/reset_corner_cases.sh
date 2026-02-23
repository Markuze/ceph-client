#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CephFS client reset corner case tests.
# Runs a checklist of targeted tests that exercise specific reset
# code paths not covered by the stress tests.
#
# Requires: mounted CephFS, debugfs access (root), flock(1) utility.

set -uo pipefail

MOUNT_POINT=""
DEBUGFS_ROOT="/sys/kernel/debug/ceph"
DEBUGFS_CLIENT=""
TRIGGER_PATH=""
STATUS_PATH=""
INJECT_PATH=""
TEMP_MNT=""
OUT_DIR=""

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
TOTAL=5

log()
{
	printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$1"
}

result()
{
	local num="$1"
	local name="$2"
	local status="$3"
	local detail="${4:-}"

	case "$status" in
	PASS) PASS_COUNT=$((PASS_COUNT + 1)) ;;
	FAIL) FAIL_COUNT=$((FAIL_COUNT + 1)) ;;
	SKIP) SKIP_COUNT=$((SKIP_COUNT + 1)) ;;
	esac

	if [[ -n "$detail" ]]; then
		printf '[%d/%d] %-30s %s  (%s)\n' "$num" "$TOTAL" "$name" "$status" "$detail"
	else
		printf '[%d/%d] %-30s %s\n' "$num" "$TOTAL" "$name" "$status"
	fi
}

read_status_field()
{
	local field="$1"
	awk -F': ' -v key="$field" '$1 == key {print $2}' "$STATUS_PATH" 2>/dev/null
}

wait_reset_done()
{
	local timeout="${1:-30}"
	local elapsed=0

	while [[ "$(read_status_field "in_progress")" == "yes" ]]; do
		sleep 1
		elapsed=$((elapsed + 1))
		if [[ "$elapsed" -ge "$timeout" ]]; then
			return 1
		fi
	done
	return 0
}

discover_debugfs()
{
	local candidates=()
	local entry

	if [[ -n "$DEBUGFS_CLIENT" ]]; then
		if [[ ! -d "$DEBUGFS_ROOT/$DEBUGFS_CLIENT/reset" ]]; then
			echo "reset debugfs not found for $DEBUGFS_CLIENT" >&2
			exit 2
		fi
		return 0
	fi

	for entry in "$DEBUGFS_ROOT"/*/; do
		entry="$(basename "$entry")"
		[[ -d "$DEBUGFS_ROOT/$entry/reset" ]] || continue
		[[ -w "$DEBUGFS_ROOT/$entry/reset/trigger" ]] || continue
		candidates+=("$entry")
	done

	if [[ ${#candidates[@]} -eq 0 ]]; then
		echo "No writable Ceph reset interface found under $DEBUGFS_ROOT" >&2
		exit 2
	fi

	if [[ ${#candidates[@]} -gt 1 ]]; then
		echo "Multiple Ceph clients found: ${candidates[*]}" >&2
		echo "Use --client-id to select one." >&2
		exit 2
	fi

	DEBUGFS_CLIENT="${candidates[0]}"
}

# --- Test 1: inject_error ---------------------------------------------------
#
# Arm the inject_error flag, trigger a reset. The reset should fail
# with -EIO. Then trigger a normal reset and verify recovery.

test_inject_error()
{
	local num=1
	local name="inject_error"
	local fc_before fc_after sc_before sc_after le

	fc_before="$(read_status_field "failure_count")"
	sc_before="$(read_status_field "success_count")"

	echo 1 > "$INJECT_PATH" 2>/dev/null || {
		result "$num" "$name" FAIL "cannot write to inject_error"
		return
	}

	echo "inject_error_test" > "$TRIGGER_PATH" 2>/dev/null || {
		result "$num" "$name" FAIL "cannot trigger reset"
		return
	}

	if ! wait_reset_done 15; then
		result "$num" "$name" FAIL "reset did not complete after inject"
		return
	fi

	fc_after="$(read_status_field "failure_count")"
	le="$(read_status_field "last_errno")"

	if [[ "$fc_after" -le "$fc_before" ]]; then
		result "$num" "$name" FAIL "failure_count did not increment ($fc_before -> $fc_after)"
		return
	fi

	if [[ "$le" != "-5" ]]; then
		result "$num" "$name" FAIL "last_errno=$le, expected -5 (EIO)"
		return
	fi

	echo "inject_error_recovery" > "$TRIGGER_PATH" 2>/dev/null || {
		result "$num" "$name" FAIL "recovery reset trigger failed"
		return
	}

	if ! wait_reset_done 30; then
		result "$num" "$name" FAIL "recovery reset did not complete"
		return
	fi

	sc_after="$(read_status_field "success_count")"
	le="$(read_status_field "last_errno")"

	if [[ "$sc_after" -le "$sc_before" ]]; then
		result "$num" "$name" FAIL "success_count did not increment after recovery"
		return
	fi

	if [[ "$le" != "0" ]]; then
		result "$num" "$name" FAIL "last_errno=$le after recovery, expected 0"
		return
	fi

	result "$num" "$name" PASS
}

# --- Test 2: ebusy_rejection ------------------------------------------------
#
# Trigger a reset, then immediately trigger a second one. The second
# should be rejected with EBUSY.

test_ebusy_rejection()
{
	local num=2
	local name="ebusy_rejection"
	local tc_before tc_after

	tc_before="$(read_status_field "trigger_count")"

	echo "ebusy_first" > "$TRIGGER_PATH" 2>/dev/null || {
		result "$num" "$name" FAIL "first trigger failed"
		return
	}

	if echo "ebusy_second" > "$TRIGGER_PATH" 2>/dev/null; then
		if ! wait_reset_done 30; then
			result "$num" "$name" FAIL "first reset never completed"
			return
		fi
		tc_after="$(read_status_field "trigger_count")"
		if [[ "$((tc_after - tc_before))" -ge 2 ]]; then
			result "$num" "$name" FAIL "second trigger was accepted (trigger_count +$((tc_after - tc_before)))"
			return
		fi
		result "$num" "$name" PASS "second trigger silently failed"
		return
	fi

	if ! wait_reset_done 30; then
		result "$num" "$name" FAIL "first reset never completed"
		return
	fi

	tc_after="$(read_status_field "trigger_count")"

	if [[ "$((tc_after - tc_before))" -ne 1 ]]; then
		result "$num" "$name" FAIL "expected trigger_count +1, got +$((tc_after - tc_before))"
		return
	fi

	result "$num" "$name" PASS
}

# --- Test 3: dirty_caps_at_reset --------------------------------------------
#
# Write to a file without fsync (dirty caps), trigger reset, then
# verify the file is not corrupt.

test_dirty_caps_at_reset()
{
	local num=3
	local name="dirty_caps_at_reset"
	local testfile="$MOUNT_POINT/.reset_corner_dirty_caps_$$"
	local content_before content_after line_count

	echo "line_1_before_dirty_write" > "$testfile"
	sync "$testfile"

	python3 -c "
import os, sys
fd = os.open('$testfile', os.O_WRONLY | os.O_APPEND)
os.write(fd, b'line_2_dirty_no_fsync\n')
# deliberately no fsync -- leave caps dirty
sys.stdout.write('written')
" 2>/dev/null || {
		result "$num" "$name" FAIL "dirty write failed"
		rm -f "$testfile"
		return
	}

	echo "dirty_caps_test" > "$TRIGGER_PATH" 2>/dev/null || {
		result "$num" "$name" FAIL "reset trigger failed"
		rm -f "$testfile"
		return
	}

	if ! wait_reset_done 30; then
		result "$num" "$name" FAIL "reset did not complete"
		rm -f "$testfile"
		return
	}

	sync "$testfile" 2>/dev/null || true
	content_after="$(cat "$testfile" 2>/dev/null)" || {
		result "$num" "$name" FAIL "cannot read file after reset"
		rm -f "$testfile"
		return
	}

	if [[ -z "$content_after" ]]; then
		result "$num" "$name" FAIL "file is empty after reset"
		rm -f "$testfile"
		return
	fi

	line_count="$(echo "$content_after" | wc -l)"
	if [[ "$line_count" -lt 1 ]]; then
		result "$num" "$name" FAIL "file has $line_count lines, expected >= 1"
		rm -f "$testfile"
		return
	fi

	echo "$content_after" | head -1 | grep -q "line_1_before_dirty_write" || {
		result "$num" "$name" FAIL "first line corrupted"
		rm -f "$testfile"
		return
	}

	le="$(read_status_field "last_errno")"
	if [[ "$le" != "0" ]]; then
		result "$num" "$name" FAIL "last_errno=$le, expected 0"
		rm -f "$testfile"
		return
	fi

	rm -f "$testfile"
	result "$num" "$name" PASS "file intact ($line_count lines)"
}

# --- Test 4: flock_reclaim --------------------------------------------------
#
# Take an exclusive flock, trigger reset, verify the lock survives
# the reconnect (from_reset=true path).

test_flock_reclaim()
{
	local num=4
	local name="flock_reclaim"
	local testfile="$MOUNT_POINT/.reset_corner_flock_$$"
	local lock_pid probe_rc

	echo "flock_test_content" > "$testfile"
	sync "$testfile"

	flock --exclusive --nonblock "$testfile" sleep 300 &
	lock_pid=$!
	sleep 0.5

	if ! kill -0 "$lock_pid" 2>/dev/null; then
		result "$num" "$name" FAIL "flock holder died immediately"
		rm -f "$testfile"
		return
	fi

	echo "flock_reclaim_test" > "$TRIGGER_PATH" 2>/dev/null || {
		kill "$lock_pid" 2>/dev/null; wait "$lock_pid" 2>/dev/null
		result "$num" "$name" FAIL "reset trigger failed"
		rm -f "$testfile"
		return
	}

	if ! wait_reset_done 30; then
		kill "$lock_pid" 2>/dev/null; wait "$lock_pid" 2>/dev/null
		result "$num" "$name" FAIL "reset did not complete"
		rm -f "$testfile"
		return
	fi

	if ! kill -0 "$lock_pid" 2>/dev/null; then
		wait "$lock_pid" 2>/dev/null
		result "$num" "$name" FAIL "flock holder died during reset"
		rm -f "$testfile"
		return
	fi

	probe_rc=0
	flock --exclusive --nonblock "$testfile" true 2>/dev/null && probe_rc=0 || probe_rc=$?
	if [[ "$probe_rc" -eq 0 ]]; then
		kill "$lock_pid" 2>/dev/null; wait "$lock_pid" 2>/dev/null
		result "$num" "$name" FAIL "lock was NOT reclaimed (probe acquired it)"
		rm -f "$testfile"
		return
	fi

	kill "$lock_pid" 2>/dev/null
	wait "$lock_pid" 2>/dev/null

	probe_rc=0
	flock --exclusive --nonblock "$testfile" true 2>/dev/null && probe_rc=0 || probe_rc=$?
	if [[ "$probe_rc" -ne 0 ]]; then
		result "$num" "$name" FAIL "lock stuck after holder exited"
		rm -f "$testfile"
		return
	fi

	rm -f "$testfile"
	result "$num" "$name" PASS
}

# --- Test 5: unmount_during_reset -------------------------------------------
#
# Mount a fresh CephFS, trigger reset, immediately unmount. The
# ceph_mdsc_destroy() path must wake blocked waiters with -ESHUTDOWN
# and not hang.

test_unmount_during_reset()
{
	local num=5
	local name="unmount_during_reset"
	local temp_mnt="/tmp/ceph_corner_mnt_$$"
	local mount_opts=""
	local mount_src=""
	local temp_trigger=""
	local temp_client=""
	local entry

	mount_src="$(awk -v mp="$MOUNT_POINT" '$2 == mp && $3 == "ceph" {print $1; exit}' /proc/mounts 2>/dev/null)"
	mount_opts="$(awk -v mp="$MOUNT_POINT" '$2 == mp && $3 == "ceph" {print $4; exit}' /proc/mounts 2>/dev/null)"

	if [[ -z "$mount_src" ]]; then
		result "$num" "$name" SKIP "cannot determine mount source from /proc/mounts"
		return
	fi

	mkdir -p "$temp_mnt"

	if ! mount -t ceph "$mount_src" "$temp_mnt" -o "$mount_opts" 2>/dev/null; then
		result "$num" "$name" SKIP "cannot mount additional CephFS instance"
		rmdir "$temp_mnt" 2>/dev/null
		return
	fi

	ls "$temp_mnt" > /dev/null 2>&1
	sync
	sleep 1

	for entry in "$DEBUGFS_ROOT"/*/; do
		entry="$(basename "$entry")"
		[[ -d "$DEBUGFS_ROOT/$entry/reset" ]] || continue
		[[ -w "$DEBUGFS_ROOT/$entry/reset/trigger" ]] || continue
		[[ "$entry" == "$DEBUGFS_CLIENT" ]] && continue
		temp_client="$entry"
		break
	done

	if [[ -z "$temp_client" ]]; then
		umount "$temp_mnt" 2>/dev/null || umount -l "$temp_mnt" 2>/dev/null
		rmdir "$temp_mnt" 2>/dev/null
		result "$num" "$name" SKIP "cannot find debugfs for temp mount"
		return
	fi

	temp_trigger="$DEBUGFS_ROOT/$temp_client/reset/trigger"

	echo "unmount_test" > "$temp_trigger" 2>/dev/null || true

	local umount_ok=0
	timeout 30 umount "$temp_mnt" 2>/dev/null && umount_ok=1

	if [[ "$umount_ok" -ne 1 ]]; then
		umount -l "$temp_mnt" 2>/dev/null || true
		rmdir "$temp_mnt" 2>/dev/null
		result "$num" "$name" FAIL "umount hung for >30s"
		return
	fi

	rmdir "$temp_mnt" 2>/dev/null

	ls "$MOUNT_POINT" > /dev/null 2>&1 || {
		result "$num" "$name" FAIL "original mount unhealthy after test"
		return
	}

	result "$num" "$name" PASS
}

# --- Main --------------------------------------------------------------------

usage()
{
	cat <<EOF
Usage: $0 --mount-point <path> [--client-id <id>] [--debugfs-root <path>]

Runs targeted corner-case tests for the CephFS client reset feature.
Requires root (debugfs access) and a mounted CephFS filesystem.

Options:
  --mount-point PATH     CephFS mount point (required)
  --client-id ID         Ceph debugfs client id (auto-detect if one client)
  --debugfs-root PATH    Debugfs ceph root (default: /sys/kernel/debug/ceph)
  --out-dir PATH         Directory for any artifacts (default: /tmp/ceph_corner_<pid>)
  --help                 Show this message
EOF
}

main()
{
	while [[ $# -gt 0 ]]; do
		case "$1" in
		--mount-point)   MOUNT_POINT="$2"; shift 2 ;;
		--client-id)     DEBUGFS_CLIENT="$2"; shift 2 ;;
		--debugfs-root)  DEBUGFS_ROOT="$2"; shift 2 ;;
		--out-dir)       OUT_DIR="$2"; shift 2 ;;
		--help|-h)       usage; exit 0 ;;
		*)               echo "Unknown option: $1" >&2; usage; exit 2 ;;
		esac
	done

	if [[ -z "$MOUNT_POINT" ]]; then
		echo "--mount-point is required" >&2
		usage
		exit 2
	fi

	if [[ ! -d "$MOUNT_POINT" ]]; then
		echo "Mount point does not exist: $MOUNT_POINT" >&2
		exit 2
	fi

	discover_debugfs
	TRIGGER_PATH="$DEBUGFS_ROOT/$DEBUGFS_CLIENT/reset/trigger"
	STATUS_PATH="$DEBUGFS_ROOT/$DEBUGFS_CLIENT/reset/status"
	INJECT_PATH="$DEBUGFS_ROOT/$DEBUGFS_CLIENT/reset/inject_error"

	log "CephFS client reset corner case tests"
	log "Mount: $MOUNT_POINT"
	log "Client: $DEBUGFS_CLIENT"
	echo ""

	test_inject_error
	test_ebusy_rejection
	test_dirty_caps_at_reset
	test_flock_reclaim
	test_unmount_during_reset

	echo ""
	echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped (of $TOTAL)"

	if [[ "$FAIL_COUNT" -gt 0 ]]; then
		exit 1
	fi
	exit 0
}

main "$@"
