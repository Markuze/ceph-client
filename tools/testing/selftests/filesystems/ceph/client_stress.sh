#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CephFS multi-client reset stress test -- client actor.
#
# Each client instance:
#   1. Seeds its private zone (and shared zone if --originator)
#   2. Runs I/O + rename workers on private and shared zones
#   3. Resets itself periodically via debugfs
#   4. When all monkeys signal done, stops shared I/O + resets
#   5. Collects shared files back to canonical location
#   6. Validates private zone + shared zone integrity
#
# Coordination with other clients and monkeys happens through files
# on the shared CephFS mount. No SSH, no sockets.

set -uo pipefail

MOUNT_POINT=""
RUN_ID=""
MY_CLIENT_ID=""
ORIGINATOR=0
FILE_COUNT=64
DURATION_SEC=900
RESET_MIN_SEC=5
RESET_MAX_SEC=15
IO_WORKERS=2
RENAME_WORKERS=1
MIN_MONKEYS=1
DEBUGFS_ROOT="/sys/kernel/debug/ceph"
DEBUGFS_CLIENT=""
COOLDOWN_SEC=20
POLL_INTERVAL=3

ROOT_DIR=""
COORD_DIR=""
PRIVATE_DIR=""
SHARED_DIR=""
LOG_DIR=""
WORKLOAD_FLAG=""
SHARED_FLAG=""
RESET_FLAG=""

TRIGGER_PATH=""
STATUS_PATH=""

RESET_PID=0
STATUS_PID=0
declare -a PRIVATE_IO_PIDS=()
declare -a PRIVATE_RENAME_PIDS=()
declare -a SHARED_IO_PIDS=()
declare -a SHARED_RENAME_PIDS=()

now_ms()
{
	date +%s%3N
}

log()
{
	printf '[%s] [client=%s] %s\n' "$(date -u +%H:%M:%S)" "$MY_CLIENT_ID" "$1"
}

die()
{
	log "FATAL: $1"
	exit 2
}

# --- Debugfs -----------------------------------------------------------------

discover_debugfs()
{
	local candidates=()
	local entry

	if [[ -n "$DEBUGFS_CLIENT" ]]; then
		[[ -d "$DEBUGFS_ROOT/$DEBUGFS_CLIENT/reset" ]] || die "debugfs not found for $DEBUGFS_CLIENT"
		return 0
	fi

	for entry in "$DEBUGFS_ROOT"/*/; do
		entry="$(basename "$entry")"
		[[ -d "$DEBUGFS_ROOT/$entry/reset" ]] || continue
		[[ -w "$DEBUGFS_ROOT/$entry/reset/trigger" ]] || continue
		candidates+=("$entry")
	done

	[[ ${#candidates[@]} -gt 0 ]] || die "no writable Ceph reset interface under $DEBUGFS_ROOT"
	[[ ${#candidates[@]} -eq 1 ]] || die "multiple Ceph clients found (${candidates[*]}), use --debugfs-client"

	DEBUGFS_CLIENT="${candidates[0]}"
}

read_status_field()
{
	awk -F': ' -v key="$1" '$1 == key {print $2}' "$STATUS_PATH" 2>/dev/null
}

# --- I/O worker --------------------------------------------------------------

io_worker()
{
	local tag="$1"
	local data_dir="$2"
	local flag_file="$3"
	local log_file="$4"
	local file_count="$5"
	local seq=0
	local id relpath abspath payload result_line hash ts

	while [[ -f "$flag_file" ]]; do
		id="$(printf '%05d' $((RANDOM % file_count)))"

		if [[ -f "$data_dir/A/file_$id" ]]; then
			relpath="A/file_$id"
		elif [[ -f "$data_dir/B/file_$id" ]]; then
			relpath="B/file_$id"
		else
			sleep 0.02
			continue
		fi

		abspath="$data_dir/$relpath"
		payload="${tag} seq=${seq} id=${id} ts=$(now_ms)"

		result_line="$(python3 -c "
import hashlib, os, sys
path = '$abspath'
payload = '$payload'
try:
    fd = os.open(path, os.O_RDWR | os.O_APPEND)
except FileNotFoundError:
    sys.exit(1)
try:
    os.write(fd, (payload + '\n').encode())
    os.fsync(fd)
    os.lseek(fd, 0, os.SEEK_SET)
    d = hashlib.sha256()
    while True:
        c = os.read(fd, 1 << 20)
        if not c: break
        d.update(c)
    print(d.hexdigest())
finally:
    os.close(fd)
" 2>/dev/null)" || { sleep 0.02; continue; }

		ts="$(now_ms)"
		printf '%s,%s,%s,%s,%s,%s\n' "$ts" "$tag" "$seq" "$id" "$relpath" "$result_line" >> "$log_file"
		seq=$((seq + 1))
		sleep 0.02
	done
}

# --- Rename worker ------------------------------------------------------------

rename_worker()
{
	local tag="$1"
	local data_dir="$2"
	local flag_file="$3"
	local log_file="$4"
	local file_count="$5"
	local seq=0
	local id src_rel dst_rel rc ts

	while [[ -f "$flag_file" ]]; do
		id="$(printf '%05d' $((RANDOM % file_count)))"

		if [[ -f "$data_dir/A/file_$id" ]]; then
			src_rel="A/file_$id"
			dst_rel="B/file_$id"
		elif [[ -f "$data_dir/B/file_$id" ]]; then
			src_rel="B/file_$id"
			dst_rel="A/file_$id"
		else
			sleep 0.02
			continue
		fi

		ts="$(now_ms)"
		rc=0
		mv -T "$data_dir/$src_rel" "$data_dir/$dst_rel" 2>/dev/null || rc=$?
		printf '%s,%s,%s,%s,%s,%s,%s\n' "$ts" "$tag" "$seq" "$id" "$src_rel" "$dst_rel" "$rc" >> "$log_file"
		seq=$((seq + 1))
		sleep 0.02
	done
}

# --- Reset injector -----------------------------------------------------------

reset_injector()
{
	local trigger="$1"
	local flag_file="$2"
	local log_file="$3"
	local seq=0
	local ts reason rc

	while [[ -f "$flag_file" ]]; do
		local span=$((RESET_MAX_SEC - RESET_MIN_SEC + 1))
		sleep $((RESET_MIN_SEC + RANDOM % span))
		[[ -f "$flag_file" ]] || break

		ts="$(now_ms)"
		reason="client_${MY_CLIENT_ID}_reset_${seq}"
		rc=0
		echo "$reason" > "$trigger" 2>/dev/null || rc=$?
		printf '%s,%s,%s,%s\n' "$ts" "$seq" "$reason" "$rc" >> "$log_file"
		seq=$((seq + 1))
	done
}

# --- Process management -------------------------------------------------------

stop_pid()
{
	local pid="$1"
	local timeout="${2:-20}"
	local waited=0

	[[ "$pid" -gt 0 ]] || return 0
	while kill -0 "$pid" 2>/dev/null; do
		if (( waited >= timeout )); then
			kill -TERM "$pid" 2>/dev/null || true
			sleep 1
			kill -KILL "$pid" 2>/dev/null || true
			wait "$pid" 2>/dev/null || true
			return 1
		fi
		sleep 1
		waited=$((waited + 1))
	done
	wait "$pid" 2>/dev/null || true
	return 0
}

stop_pid_array()
{
	local -n arr=$1
	local i
	for i in "${!arr[@]}"; do
		stop_pid "${arr[$i]}" 20
	done
	arr=()
}

# --- Coordination -------------------------------------------------------------

wait_for_file()
{
	local path="$1"
	local timeout="${2:-300}"
	local elapsed=0

	while [[ ! -f "$path" ]]; do
		sleep "$POLL_INTERVAL"
		elapsed=$((elapsed + POLL_INTERVAL))
		if [[ "$elapsed" -ge "$timeout" ]]; then
			return 1
		fi
	done
	return 0
}

count_coord_files()
{
	local pattern="$1"
	ls -1 "$COORD_DIR"/$pattern 2>/dev/null | wc -l
}

wait_all_monkeys_done()
{
	local monkey_count=0
	local done_count=0

	log "Waiting for monkeys to finish..."

	while true; do
		monkey_count="$(count_coord_files "monkey_*_done.json")"
		monkey_count=$((monkey_count + $(count_coord_files "monkey_*_ready.json")))

		done_count="$(count_coord_files "monkey_*_done.json")"

		if [[ "$monkey_count" -gt 0 && "$done_count" -ge "$monkey_count" ]]; then
			log "All $done_count monkey(s) done"
			return 0
		fi

		if [[ "$done_count" -ge "$MIN_MONKEYS" && "$done_count" -ge "$monkey_count" ]]; then
			log "$done_count monkey(s) done (>= min_monkeys=$MIN_MONKEYS)"
			return 0
		fi

		sleep "$POLL_INTERVAL"
	done
}

# --- Collect phase ------------------------------------------------------------

collect_back()
{
	local dir="$1"
	local i name

	log "Collecting files back to A/ in $dir"
	for i in $(seq 0 $((FILE_COUNT - 1))); do
		name="file_$(printf '%05d' "$i")"
		if [[ -f "$dir/B/$name" ]]; then
			mv -f "$dir/B/$name" "$dir/A/$name" 2>/dev/null || true
		fi
	done
}

# --- Init ---------------------------------------------------------------------

seed_zone()
{
	local dir="$1"
	local count="$2"
	local i

	mkdir -p "$dir/A" "$dir/B"
	for ((i = 0; i < count; i++)); do
		printf 'seed id=%05d client=%s ts=%s\n' "$i" "$MY_CLIENT_ID" "$(now_ms)" \
			> "$dir/A/file_$(printf '%05d' "$i")"
	done
}

init_originator()
{
	log "Originator: creating directory tree"
	mkdir -p "$ROOT_DIR" "$COORD_DIR" "$SHARED_DIR" "$LOG_DIR/$MY_CLIENT_ID"
	mkdir -p "$PRIVATE_DIR"

	seed_zone "$SHARED_DIR" "$FILE_COUNT"
	seed_zone "$PRIVATE_DIR" "$FILE_COUNT"

	echo '{"originator": "'"$MY_CLIENT_ID"'"}' > "$COORD_DIR/root_ready"
	log "Originator: root_ready written"
}

init_joiner()
{
	log "Waiting for originator..."
	if ! wait_for_file "$COORD_DIR/root_ready" 300; then
		die "timed out waiting for root_ready"
	fi
	log "Originator detected, seeding private zone"
	mkdir -p "$PRIVATE_DIR" "$LOG_DIR/$MY_CLIENT_ID"
	seed_zone "$PRIVATE_DIR" "$FILE_COUNT"
}

# --- Main --------------------------------------------------------------------

usage()
{
	cat <<'EOF'
Usage: client_stress.sh --mount-point <path> --run-id <id> --client-id <id> [options]

Required:
  --mount-point PATH       CephFS mount point
  --run-id ID              Shared run identifier (same across all participants)
  --client-id ID           Unique identifier for this client (e.g. c1, c2)

Options:
  --originator             This client creates the shared directory tree
  --file-count N           Files per zone (default: 64)
  --duration-sec N         How long to run workloads (default: 900)
  --reset-interval-min N   Min seconds between resets (default: 5)
  --reset-interval-max N   Max seconds between resets (default: 15)
  --io-workers N           I/O workers per zone (default: 2)
  --rename-workers N       Rename workers per zone (default: 1)
  --min-monkeys N          Minimum monkeys before validation (default: 1)
  --debugfs-client ID      Debugfs client id (auto-detect if one)
  --debugfs-root PATH      Debugfs ceph root (default: /sys/kernel/debug/ceph)
  --cooldown-sec N         Drain time after stopping (default: 20)
  --help                   Show this message
EOF
}

main()
{
	local final_rc=0
	local i

	while [[ $# -gt 0 ]]; do
		case "$1" in
		--mount-point)        MOUNT_POINT="$2"; shift 2 ;;
		--run-id)             RUN_ID="$2"; shift 2 ;;
		--client-id)          MY_CLIENT_ID="$2"; shift 2 ;;
		--originator)         ORIGINATOR=1; shift ;;
		--file-count)         FILE_COUNT="$2"; shift 2 ;;
		--duration-sec)       DURATION_SEC="$2"; shift 2 ;;
		--reset-interval-min) RESET_MIN_SEC="$2"; shift 2 ;;
		--reset-interval-max) RESET_MAX_SEC="$2"; shift 2 ;;
		--io-workers)         IO_WORKERS="$2"; shift 2 ;;
		--rename-workers)     RENAME_WORKERS="$2"; shift 2 ;;
		--min-monkeys)        MIN_MONKEYS="$2"; shift 2 ;;
		--debugfs-client)     DEBUGFS_CLIENT="$2"; shift 2 ;;
		--debugfs-root)       DEBUGFS_ROOT="$2"; shift 2 ;;
		--cooldown-sec)       COOLDOWN_SEC="$2"; shift 2 ;;
		--help|-h)            usage; exit 0 ;;
		*)                    die "unknown option: $1" ;;
		esac
	done

	[[ -n "$MOUNT_POINT" ]]  || die "--mount-point is required"
	[[ -n "$RUN_ID" ]]       || die "--run-id is required"
	[[ -n "$MY_CLIENT_ID" ]] || die "--client-id is required"
	[[ -d "$MOUNT_POINT" ]]  || die "mount point does not exist: $MOUNT_POINT"

	ROOT_DIR="$MOUNT_POINT/reset_test_${RUN_ID}"
	COORD_DIR="$ROOT_DIR/coord"
	PRIVATE_DIR="$ROOT_DIR/clients/$MY_CLIENT_ID"
	SHARED_DIR="$ROOT_DIR/shared"
	LOG_DIR="$ROOT_DIR/logs"

	discover_debugfs
	TRIGGER_PATH="$DEBUGFS_ROOT/$DEBUGFS_CLIENT/reset/trigger"
	STATUS_PATH="$DEBUGFS_ROOT/$DEBUGFS_CLIENT/reset/status"

	log "Starting client stress: run=$RUN_ID duration=${DURATION_SEC}s"
	log "Private: $PRIVATE_DIR  Shared: $SHARED_DIR"

	# --- Init ---
	if [[ "$ORIGINATOR" -eq 1 ]]; then
		init_originator
	else
		init_joiner
	fi

	printf '{"client_id":"%s","private_dir":"clients/%s","shared_dir":"shared"}\n' \
		"$MY_CLIENT_ID" "$MY_CLIENT_ID" > "$COORD_DIR/client_${MY_CLIENT_ID}_ready.json"
	log "Advertised readiness"

	# --- Flag files ---
	WORKLOAD_FLAG="$LOG_DIR/$MY_CLIENT_ID/workload.running"
	SHARED_FLAG="$LOG_DIR/$MY_CLIENT_ID/shared.running"
	RESET_FLAG="$LOG_DIR/$MY_CLIENT_ID/reset.running"
	touch "$WORKLOAD_FLAG" "$SHARED_FLAG" "$RESET_FLAG"

	# --- Launch private zone workers ---
	for ((i = 0; i < IO_WORKERS; i++)); do
		io_worker "priv_${MY_CLIENT_ID}_io$i" "$PRIVATE_DIR" "$WORKLOAD_FLAG" \
			"$LOG_DIR/$MY_CLIENT_ID/io_private.log" "$FILE_COUNT" &
		PRIVATE_IO_PIDS+=("$!")
	done
	for ((i = 0; i < RENAME_WORKERS; i++)); do
		rename_worker "priv_${MY_CLIENT_ID}_rn$i" "$PRIVATE_DIR" "$WORKLOAD_FLAG" \
			"$LOG_DIR/$MY_CLIENT_ID/rename_private.log" "$FILE_COUNT" &
		PRIVATE_RENAME_PIDS+=("$!")
	done

	# --- Launch shared zone workers ---
	for ((i = 0; i < IO_WORKERS; i++)); do
		io_worker "shared_${MY_CLIENT_ID}_io$i" "$SHARED_DIR" "$SHARED_FLAG" \
			"$LOG_DIR/$MY_CLIENT_ID/io_shared.log" "$FILE_COUNT" &
		SHARED_IO_PIDS+=("$!")
	done
	for ((i = 0; i < RENAME_WORKERS; i++)); do
		rename_worker "shared_${MY_CLIENT_ID}_rn$i" "$SHARED_DIR" "$SHARED_FLAG" \
			"$LOG_DIR/$MY_CLIENT_ID/rename_shared.log" "$FILE_COUNT" &
		SHARED_RENAME_PIDS+=("$!")
	done

	# --- Launch reset injector ---
	reset_injector "$TRIGGER_PATH" "$RESET_FLAG" \
		"$LOG_DIR/$MY_CLIENT_ID/reset.log" &
	RESET_PID=$!

	log "All workers launched, running for ${DURATION_SEC}s"

	# --- Run for duration ---
	sleep "$DURATION_SEC"

	# --- Stop resets ---
	rm -f "$RESET_FLAG"
	stop_pid "$RESET_PID" 20
	log "Reset injector stopped"

	# --- Wait for all monkeys to finish before stopping shared I/O ---
	wait_all_monkeys_done

	# --- Stop shared zone workers ---
	rm -f "$SHARED_FLAG"
	log "Stopping shared zone workers"
	stop_pid_array SHARED_IO_PIDS
	stop_pid_array SHARED_RENAME_PIDS

	# --- Cooldown ---
	log "Cooldown ${COOLDOWN_SEC}s"
	sleep "$COOLDOWN_SEC"

	# --- Stop private zone workers ---
	rm -f "$WORKLOAD_FLAG"
	log "Stopping private zone workers"
	stop_pid_array PRIVATE_IO_PIDS
	stop_pid_array PRIVATE_RENAME_PIDS

	# --- Collect files back ---
	collect_back "$PRIVATE_DIR"
	collect_back "$SHARED_DIR"

	# --- Capture final state ---
	cat "$STATUS_PATH" > "$LOG_DIR/$MY_CLIENT_ID/status.final" 2>/dev/null || true
	dmesg --since "@$(date +%s -d "${DURATION_SEC} seconds ago")" \
		> "$LOG_DIR/$MY_CLIENT_ID/dmesg.log" 2>/dev/null || \
		dmesg > "$LOG_DIR/$MY_CLIENT_ID/dmesg.log" 2>/dev/null || true

	# --- Self-validate ---
	log "Running validation"
	local script_dir
	script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

	if python3 "$script_dir/validate_concurrent.py" --mode client \
		--root-dir "$ROOT_DIR" \
		--client-id "$MY_CLIENT_ID" \
		--file-count "$FILE_COUNT"; then
		log "PASS"
	else
		log "FAIL"
		final_rc=1
	fi

	exit "$final_rc"
}

main "$@"
