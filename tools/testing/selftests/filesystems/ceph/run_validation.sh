#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CephFS client reset — single-command validation.
# Runs all test stages in sequence with per-stage timeouts.
# If any stage hangs (filesystem stuck, process blocked), the
# timeout kills it and reports failure.
#
# Usage:
#   sudo ./run_validation.sh --mount-point /mnt/mycephfs
#
# Expected output on success:
#
#   === CephFS Client Reset Validation ===
#   [stage 1/5] baseline         PASS  (60s, no resets)
#   [stage 2/5] corner_cases     PASS  (5/5 passed)
#   [stage 3/5] moderate         PASS  (120s, resets every 5-15s)
#   [stage 4/5] aggressive       PASS  (120s, resets every 1-5s)
#   [stage 5/5] status_check     PASS  (phase=idle, last_errno=0)
#
#   RESULT: 5/5 stages passed
#   Artifacts: /tmp/ceph_reset_validation_<timestamp>

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MOUNT_POINT=""
RUN_ID="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="/tmp/ceph_reset_validation_${RUN_ID}"
DEBUGFS_ROOT="/sys/kernel/debug/ceph"

# Timeout margins: stage runtime + cooldown + validation + safety buffer
STAGE1_TIMEOUT=120    # 60s run + 20s cooldown + 40s buffer
STAGE2_TIMEOUT=300    # 5 corner cases, 30s each worst case
STAGE3_TIMEOUT=240    # 120s run + 20s cooldown + 100s buffer
STAGE4_TIMEOUT=240    # 120s run + 20s cooldown + 100s buffer
STAGE5_TIMEOUT=10     # just reading debugfs

PASS=0
FAIL=0
TOTAL=5

usage()
{
	cat <<EOF
Usage: $0 --mount-point <cephfs_mount> [options]

Required:
  --mount-point PATH    CephFS mount point

Options:
  --out-dir PATH        Artifact directory (default: /tmp/ceph_reset_validation_<ts>)
  --debugfs-root PATH   Debugfs Ceph root (default: /sys/kernel/debug/ceph)
  --help                Show this message
EOF
}

stage_result()
{
	local num="$1"
	local name="$2"
	local status="$3"
	local detail="$4"

	if [[ "$status" == "PASS" ]]; then
		PASS=$((PASS + 1))
	else
		FAIL=$((FAIL + 1))
	fi
	printf '[stage %d/%d] %-16s %s  (%s)\n' "$num" "$TOTAL" "$name" "$status" "$detail"
}

# Run a command with a timeout. Returns 0 on success, 1 on failure/timeout.
# Sets RUN_TIMED_OUT=1 if killed by timeout.
RUN_TIMED_OUT=0

run_with_timeout()
{
	local timeout_sec="$1"
	local logfile="$2"
	shift 2

	RUN_TIMED_OUT=0

	# Run in background, capture pid
	"$@" > "$logfile" 2>&1 &
	local pid=$!

	# Watchdog: kill after timeout
	(
		sleep "$timeout_sec"
		if kill -0 "$pid" 2>/dev/null; then
			echo "TIMEOUT: stage exceeded ${timeout_sec}s, killing pid $pid" >> "$logfile"
			kill -TERM "$pid" 2>/dev/null
			sleep 2
			kill -KILL "$pid" 2>/dev/null
		fi
	) &
	local watchdog_pid=$!

	# Wait for the actual command
	wait "$pid" 2>/dev/null
	local rc=$?

	# Kill the watchdog if it's still running
	kill "$watchdog_pid" 2>/dev/null
	wait "$watchdog_pid" 2>/dev/null

	# Check if it was killed by timeout
	if grep -q "^TIMEOUT:" "$logfile" 2>/dev/null; then
		RUN_TIMED_OUT=1
		return 1
	fi

	return "$rc"
}

find_status_path()
{
	local entry
	for entry in "$DEBUGFS_ROOT"/*/; do
		if [[ -r "${entry}reset/status" ]]; then
			echo "${entry}reset/status"
			return 0
		fi
	done
	return 1
}

read_status_field()
{
	local status_path="$1"
	local field="$2"
	awk -F': ' -v key="$field" '$1 == key {print $2}' "$status_path" 2>/dev/null
}

# --- Parse arguments -------------------------------------------------------

while [[ $# -gt 0 ]]; do
	case "$1" in
	--mount-point)  MOUNT_POINT="$2"; shift 2 ;;
	--out-dir)      OUT_DIR="$2"; shift 2 ;;
	--debugfs-root) DEBUGFS_ROOT="$2"; shift 2 ;;
	--help|-h)      usage; exit 0 ;;
	*)              echo "Unknown option: $1" >&2; usage; exit 2 ;;
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

# Quick sanity: can we write to the mount?
if ! touch "$MOUNT_POINT/.validation_probe_$$" 2>/dev/null; then
	echo "Mount point is not writable: $MOUNT_POINT" >&2
	exit 2
fi
rm -f "$MOUNT_POINT/.validation_probe_$$"

mkdir -p "$OUT_DIR"

echo ""
echo "=== CephFS Client Reset Validation ==="
echo ""

# --- Stage 1: Baseline (no resets) -----------------------------------------

stage1_out="$OUT_DIR/stage1_baseline"
if run_with_timeout "$STAGE1_TIMEOUT" "$stage1_out.log" \
	"$SCRIPT_DIR/reset_stress.sh" \
	--mount-point "$MOUNT_POINT" \
	--profile baseline \
	--no-reset \
	--duration-sec 60 \
	--out-dir "$stage1_out"; then
	stage_result 1 "baseline" "PASS" "60s, no resets"
elif [[ "$RUN_TIMED_OUT" -eq 1 ]]; then
	stage_result 1 "baseline" "FAIL" "HUNG: killed after ${STAGE1_TIMEOUT}s"
else
	stage_result 1 "baseline" "FAIL" "see $stage1_out.log"
fi

# --- Stage 2: Corner cases -------------------------------------------------

stage2_out="$OUT_DIR/stage2_corner_cases"
mkdir -p "$stage2_out"
if run_with_timeout "$STAGE2_TIMEOUT" "$stage2_out/output.log" \
	"$SCRIPT_DIR/reset_corner_cases.sh" \
	--mount-point "$MOUNT_POINT"; then
	pass_line=$(grep -o '[0-9]*/[0-9]* passed' "$stage2_out/output.log" | tail -1)
	stage_result 2 "corner_cases" "PASS" "${pass_line:-all tests passed}"
elif [[ "$RUN_TIMED_OUT" -eq 1 ]]; then
	stage_result 2 "corner_cases" "FAIL" "HUNG: killed after ${STAGE2_TIMEOUT}s"
else
	fail_line=$(grep -c 'FAIL' "$stage2_out/output.log" 2>/dev/null || echo "?")
	stage_result 2 "corner_cases" "FAIL" "${fail_line} failures, see $stage2_out/output.log"
fi

# --- Stage 3: Moderate resets -----------------------------------------------

stage3_out="$OUT_DIR/stage3_moderate"
if run_with_timeout "$STAGE3_TIMEOUT" "$stage3_out.log" \
	"$SCRIPT_DIR/reset_stress.sh" \
	--mount-point "$MOUNT_POINT" \
	--profile moderate \
	--duration-sec 120 \
	--out-dir "$stage3_out"; then
	stage_result 3 "moderate" "PASS" "120s, resets every 5-15s"
elif [[ "$RUN_TIMED_OUT" -eq 1 ]]; then
	stage_result 3 "moderate" "FAIL" "HUNG: killed after ${STAGE3_TIMEOUT}s"
else
	stage_result 3 "moderate" "FAIL" "see $stage3_out.log"
fi

# --- Stage 4: Aggressive resets ---------------------------------------------

stage4_out="$OUT_DIR/stage4_aggressive"
if run_with_timeout "$STAGE4_TIMEOUT" "$stage4_out.log" \
	"$SCRIPT_DIR/reset_stress.sh" \
	--mount-point "$MOUNT_POINT" \
	--profile aggressive \
	--duration-sec 120 \
	--out-dir "$stage4_out"; then
	stage_result 4 "aggressive" "PASS" "120s, resets every 1-5s"
elif [[ "$RUN_TIMED_OUT" -eq 1 ]]; then
	stage_result 4 "aggressive" "FAIL" "HUNG: killed after ${STAGE4_TIMEOUT}s"
else
	stage_result 4 "aggressive" "FAIL" "see $stage4_out.log"
fi

# --- Stage 5: Post-run status check ----------------------------------------

status_path=""
if status_path=$(find_status_path); then
	phase=$(read_status_field "$status_path" "phase")
	last_errno=$(read_status_field "$status_path" "last_errno")
	drain_timed_out=$(read_status_field "$status_path" "drain_timed_out")
	sessions_reset=$(read_status_field "$status_path" "sessions_reset")
	pending=$(read_status_field "$status_path" "pending_reconnects")
	blocked=$(read_status_field "$status_path" "blocked_requests")

	# Save full status
	cat "$status_path" > "$OUT_DIR/final_status.txt" 2>/dev/null

	errors=""
	[[ "$phase" != "idle" ]] && errors="${errors}phase=$phase "
	[[ "$last_errno" != "0" ]] && errors="${errors}last_errno=$last_errno "
	[[ "$pending" != "0" ]] && errors="${errors}pending_reconnects=$pending "
	[[ "$blocked" != "0" ]] && errors="${errors}blocked_requests=$blocked "

	if [[ -z "$errors" ]]; then
		detail="phase=$phase, last_errno=$last_errno"
		[[ "$drain_timed_out" == "yes" ]] && detail="$detail, drain_timed_out=yes"
		[[ -n "$sessions_reset" ]] && detail="$detail, sessions_reset=$sessions_reset"
		stage_result 5 "status_check" "PASS" "$detail"
	else
		stage_result 5 "status_check" "FAIL" "$errors"
	fi
else
	stage_result 5 "status_check" "FAIL" "cannot read reset/status"
fi

# --- Summary ----------------------------------------------------------------

echo ""
if [[ "$FAIL" -eq 0 ]]; then
	echo "RESULT: $PASS/$TOTAL stages passed"
else
	echo "RESULT: $PASS/$TOTAL stages passed, $FAIL FAILED"
fi
echo "Artifacts: $OUT_DIR"
echo ""

exit "$FAIL"
