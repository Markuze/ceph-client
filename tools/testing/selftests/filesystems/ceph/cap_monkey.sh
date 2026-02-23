#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# CephFS multi-client reset stress test -- cap revocation monkey.
#
# Discovers client directories via the coordination directory on CephFS,
# then hammers the shared file zone with writes, renames, hash checks,
# and flock operations to force cap revocation across clients.
#
# Does NOT trigger resets -- that's the client's job. The monkey just
# creates contention.

set -uo pipefail

MOUNT_POINT=""
RUN_ID=""
MY_MONKEY_ID=""
DURATION_SEC=600
IO_WORKERS=4
RENAME_WORKERS=2
FLOCK_WORKERS=1
FILE_COUNT=64
MIN_CLIENTS=2
POLL_INTERVAL=5
HASH_CHECK_INTERVAL=100

ROOT_DIR=""
COORD_DIR=""
SHARED_DIR=""
LOG_DIR=""
WORKLOAD_FLAG=""

declare -a IO_PIDS=()
declare -a RENAME_PIDS=()
declare -a FLOCK_PIDS=()

now_ms()
{
	date +%s%3N
}

log()
{
	printf '[%s] [monkey=%s] %s\n' "$(date -u +%H:%M:%S)" "$MY_MONKEY_ID" "$1"
}

die()
{
	log "FATAL: $1"
	exit 2
}

# --- I/O worker with periodic hash checks ------------------------------------

io_worker()
{
	local tag="$1"
	local data_dir="$2"
	local flag_file="$3"
	local log_file="$4"
	local err_log="$5"
	local file_count="$6"
	local check_interval="$7"
	local seq=0
	local id relpath abspath payload result_line ts

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
check = ($seq % $check_interval == 0)
try:
    fd = os.open(path, os.O_RDWR | os.O_APPEND)
except FileNotFoundError:
    sys.exit(1)
try:
    os.write(fd, (payload + '\n').encode())
    os.fsync(fd)
    os.lseek(fd, 0, os.SEEK_SET)
    d = hashlib.sha256()
    corrupt = False
    while True:
        c = os.read(fd, 1 << 20)
        if not c: break
        d.update(c)
        if check:
            for line in c.decode('utf-8', errors='replace').splitlines():
                line = line.strip()
                if not line:
                    continue
                # every line should contain '=' (key=value payload format)
                if '=' not in line:
                    corrupt = True
    status = 'CORRUPT' if corrupt else 'ok'
    print(d.hexdigest() + ' ' + status)
finally:
    os.close(fd)
" 2>/dev/null)" || { sleep 0.02; continue; }

		local hash="${result_line%% *}"
		local status="${result_line#* }"

		ts="$(now_ms)"
		printf '%s,%s,%s,%s,%s,%s\n' "$ts" "$tag" "$seq" "$id" "$relpath" "$hash" >> "$log_file"

		if [[ "$status" == "CORRUPT" ]]; then
			printf '%s,%s,%s,%s,CORRUPT\n' "$ts" "$tag" "$id" "$relpath" >> "$err_log"
		fi

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

# --- Flock worker -------------------------------------------------------------

flock_worker()
{
	local tag="$1"
	local data_dir="$2"
	local flag_file="$3"
	local log_file="$4"
	local file_count="$5"
	local seq=0
	local id relpath abspath ts rc hold_ms

	while [[ -f "$flag_file" ]]; do
		id="$(printf '%05d' $((RANDOM % file_count)))"

		if [[ -f "$data_dir/A/file_$id" ]]; then
			relpath="A/file_$id"
		elif [[ -f "$data_dir/B/file_$id" ]]; then
			relpath="B/file_$id"
		else
			sleep 0.1
			continue
		fi

		abspath="$data_dir/$relpath"
		hold_ms=$((100 + RANDOM % 400))

		ts="$(now_ms)"
		rc=0
		if flock --exclusive --timeout 2 "$abspath" sleep "0.${hold_ms}" 2>/dev/null; then
			rc=0
		else
			rc=$?
		fi
		printf '%s,%s,%s,%s,%s,%s,%s\n' "$ts" "$tag" "$seq" "$id" "$relpath" "$rc" "$hold_ms" >> "$log_file"
		seq=$((seq + 1))
		sleep 0.1
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

# --- Main --------------------------------------------------------------------

usage()
{
	cat <<'EOF'
Usage: cap_monkey.sh --mount-point <path> --run-id <id> --monkey-id <id> [options]

Required:
  --mount-point PATH       CephFS mount point
  --run-id ID              Shared run identifier (same as clients)
  --monkey-id ID           Unique identifier for this monkey (e.g. m1)

Options:
  --duration-sec N         How long to stress (default: 600)
  --io-workers N           I/O workers on shared zone (default: 4)
  --rename-workers N       Rename workers on shared zone (default: 2)
  --flock-workers N        Flock contention workers (default: 1)
  --file-count N           Expected files per zone (default: 64)
  --min-clients N          Wait for this many clients before starting (default: 2)
  --poll-interval N        Seconds between coord dir polls (default: 5)
  --hash-check-interval N  Check content integrity every N ops (default: 100)
  --help                   Show this message
EOF
}

main()
{
	local i

	while [[ $# -gt 0 ]]; do
		case "$1" in
		--mount-point)          MOUNT_POINT="$2"; shift 2 ;;
		--run-id)               RUN_ID="$2"; shift 2 ;;
		--monkey-id)            MY_MONKEY_ID="$2"; shift 2 ;;
		--duration-sec)         DURATION_SEC="$2"; shift 2 ;;
		--io-workers)           IO_WORKERS="$2"; shift 2 ;;
		--rename-workers)       RENAME_WORKERS="$2"; shift 2 ;;
		--flock-workers)        FLOCK_WORKERS="$2"; shift 2 ;;
		--file-count)           FILE_COUNT="$2"; shift 2 ;;
		--min-clients)          MIN_CLIENTS="$2"; shift 2 ;;
		--poll-interval)        POLL_INTERVAL="$2"; shift 2 ;;
		--hash-check-interval)  HASH_CHECK_INTERVAL="$2"; shift 2 ;;
		--help|-h)              usage; exit 0 ;;
		*)                      die "unknown option: $1" ;;
		esac
	done

	[[ -n "$MOUNT_POINT" ]]   || die "--mount-point is required"
	[[ -n "$RUN_ID" ]]        || die "--run-id is required"
	[[ -n "$MY_MONKEY_ID" ]]  || die "--monkey-id is required"
	[[ -d "$MOUNT_POINT" ]]   || die "mount point does not exist: $MOUNT_POINT"

	ROOT_DIR="$MOUNT_POINT/reset_test_${RUN_ID}"
	COORD_DIR="$ROOT_DIR/coord"
	SHARED_DIR="$ROOT_DIR/shared"
	LOG_DIR="$ROOT_DIR/logs/$MY_MONKEY_ID"

	# --- Wait for originator ---
	log "Waiting for originator (root_ready)..."
	local elapsed=0
	while [[ ! -f "$COORD_DIR/root_ready" ]]; do
		sleep "$POLL_INTERVAL"
		elapsed=$((elapsed + POLL_INTERVAL))
		if [[ "$elapsed" -ge 300 ]]; then
			die "timed out waiting for root_ready"
		fi
	done
	log "Originator detected"

	# --- Wait for minimum clients ---
	log "Waiting for $MIN_CLIENTS client(s)..."
	while true; do
		local client_count
		client_count="$(ls -1 "$COORD_DIR"/client_*_ready.json 2>/dev/null | wc -l)"
		if [[ "$client_count" -ge "$MIN_CLIENTS" ]]; then
			log "$client_count client(s) ready"
			break
		fi
		sleep "$POLL_INTERVAL"
	done

	# --- Setup logs ---
	mkdir -p "$LOG_DIR"
	local io_log="$LOG_DIR/io.log"
	local rename_log="$LOG_DIR/rename.log"
	local flock_log="$LOG_DIR/flock.log"
	local err_log="$LOG_DIR/errors.log"
	: > "$io_log"
	: > "$rename_log"
	: > "$flock_log"
	: > "$err_log"

	WORKLOAD_FLAG="$LOG_DIR/workload.running"
	touch "$WORKLOAD_FLAG"

	# --- Launch workers ---
	for ((i = 0; i < IO_WORKERS; i++)); do
		io_worker "monkey_${MY_MONKEY_ID}_io$i" "$SHARED_DIR" "$WORKLOAD_FLAG" \
			"$io_log" "$err_log" "$FILE_COUNT" "$HASH_CHECK_INTERVAL" &
		IO_PIDS+=("$!")
	done

	for ((i = 0; i < RENAME_WORKERS; i++)); do
		rename_worker "monkey_${MY_MONKEY_ID}_rn$i" "$SHARED_DIR" "$WORKLOAD_FLAG" \
			"$rename_log" "$FILE_COUNT" &
		RENAME_PIDS+=("$!")
	done

	for ((i = 0; i < FLOCK_WORKERS; i++)); do
		flock_worker "monkey_${MY_MONKEY_ID}_fl$i" "$SHARED_DIR" "$WORKLOAD_FLAG" \
			"$flock_log" "$FILE_COUNT" &
		FLOCK_PIDS+=("$!")
	done

	log "Workers launched: ${IO_WORKERS} io, ${RENAME_WORKERS} rename, ${FLOCK_WORKERS} flock"
	log "Stressing shared zone for ${DURATION_SEC}s"

	# --- Run ---
	sleep "$DURATION_SEC"

	# --- Stop ---
	rm -f "$WORKLOAD_FLAG"
	log "Stopping workers"
	stop_pid_array IO_PIDS
	stop_pid_array RENAME_PIDS
	stop_pid_array FLOCK_PIDS

	# --- Signal done ---
	local err_count
	err_count="$(wc -l < "$err_log" 2>/dev/null || echo 0)"

	printf '{"monkey_id":"%s","shared_dir":"shared","duration_sec":%d,"errors":%d,"reason":"duration_elapsed"}\n' \
		"$MY_MONKEY_ID" "$DURATION_SEC" "$err_count" > "$COORD_DIR/monkey_${MY_MONKEY_ID}_done.json"

	log "Done. Errors detected during run: $err_count"

	if [[ "$err_count" -gt 0 ]]; then
		log "Error log: $err_log"
	fi
}

main "$@"
