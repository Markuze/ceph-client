#!/usr/bin/env bash
set -euo pipefail

REMOTE_TEST_TARGET="${REMOTE_TEST_TARGET:-10.251.64.11}"
REMOTE_TEST_SSH_KEY="${REMOTE_TEST_SSH_KEY:-$HOME/.ssh/id_rsa_cloud}"
REMOTE_REBOOT_COMMAND="${REMOTE_REBOOT_COMMAND:-~/update_reboot.sh}"
REMOTE_MOUNT_COMMAND="${REMOTE_MOUNT_COMMAND:-~/mount.sh}"
REMOTE_READY_COMMAND="${REMOTE_READY_COMMAND:-hostname}"
REMOTE_VALIDATION_COMMAND="${REMOTE_VALIDATION_COMMAND:-sudo -n ~/linux/tools/testing/selftests/filesystems/ceph/run_validation.sh --mount-point /mnt/mycephfs}"
REMOTE_DMESG_COMMAND="${REMOTE_DMESG_COMMAND:-sudo -n dmesg -T}"
REMOTE_REBOOT_WAIT_SECONDS="${REMOTE_REBOOT_WAIT_SECONDS:-120}"
REMOTE_POLL_INTERVAL_SECONDS="${REMOTE_POLL_INTERVAL_SECONDS:-5}"
REMOTE_READY_TIMEOUT_SECONDS="${REMOTE_READY_TIMEOUT_SECONDS:-900}"
VALIDATION_SUCCESS_MARKER="${VALIDATION_SUCCESS_MARKER:-RESULT: 5/5 stages passed}"
VALIDATION_FAILURE_MARKER="${VALIDATION_FAILURE_MARKER:-FAILED}"
OUT_DIR="${OUT_DIR:-}"
TEST_RESULT_FILE="${TEST_RESULT_FILE:-test_result.txt}"

log() {
  printf '[%(%F %T)T] %s
' -1 "$*" >&2
}

usage() {
  cat <<'EOF'
Usage:
  run_remote_client_reset_validation.sh --out-dir PATH --test-result-file PATH
EOF
}

run_remote_capture() {
  local label="$1"
  local logfile="$2"
  local remote_command="$3"
  local rc=0

  {
    printf '# %s
' "$label"
    printf 'remote_target=%s
' "$REMOTE_TEST_TARGET"
    printf 'remote_command=%s
' "$remote_command"
    printf 'start_utc=%s

' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } >"$logfile"

  set +e
  ssh -i "$REMOTE_TEST_SSH_KEY" "$REMOTE_TEST_TARGET" "$remote_command" 2>&1 | tee -a "$logfile"
  rc=${PIPESTATUS[0]}
  set -e

  {
    printf '
remote_exit_code=%s
' "$rc"
    printf 'finish_utc=%s
' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } >>"$logfile"
  return "$rc"
}

wait_for_remote_ready() {
  local logfile="$1"
  local deadline=$((SECONDS + REMOTE_READY_TIMEOUT_SECONDS))
  local attempt=0
  local rc=1

  : >"$logfile"
  printf '# waiting for remote host to answer %s
' "$REMOTE_READY_COMMAND" >>"$logfile"
  while (( SECONDS < deadline )); do
    attempt=$((attempt + 1))
    printf '[attempt %d] %s
' "$attempt" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$logfile"
    set +e
    ssh -i "$REMOTE_TEST_SSH_KEY" "$REMOTE_TEST_TARGET" "$REMOTE_READY_COMMAND" >>"$logfile" 2>&1
    rc=$?
    set -e
    if [[ "$rc" -eq 0 ]]; then
      printf 'ready_attempt=%d
' "$attempt" >>"$logfile"
      return 0
    fi
    sleep "$REMOTE_POLL_INTERVAL_SECONDS"
  done

  printf 'ready_timeout_seconds=%s
' "$REMOTE_READY_TIMEOUT_SECONDS" >>"$logfile"
  return 1
}

append_file_section() {
  local title="$1"
  local path="$2"
  {
    printf '
## %s
' "$title"
    if [[ -f "$path" ]]; then
      cat "$path"
    else
      printf 'missing file: %s
' "$path"
    fi
  } >>"$TEST_RESULT_FILE"
}

append_tail_section() {
  local title="$1"
  local path="$2"
  local lines="$3"
  {
    printf '
## %s
' "$title"
    if [[ -f "$path" ]]; then
      tail -n "$lines" "$path"
    else
      printf 'missing file: %s
' "$path"
    fi
  } >>"$TEST_RESULT_FILE"
}

derive_validation_artifacts() {
  local logfile="$1"
  local value=""

  value="$(grep -E '^Artifacts:' "$logfile" | tail -n 1 | sed 's/^Artifacts:[[:space:]]*//' || true)"
  if [[ -n "$value" ]]; then
    printf '%s
' "$value"
    return 0
  fi

  value="$(grep -Eo '/tmp/ceph_reset_validation_[^[:space:]]+' "$logfile" | tail -n 1 || true)"
  if [[ -n "$value" ]]; then
    printf '%s
' "$value"
  fi
}

scripted_validation_review() {
  local logfile="$1"
  local summary=""

  summary="$(grep -E '^RESULT:' "$logfile" | tail -n 1 || true)"
  if [[ "$summary" == "$VALIDATION_SUCCESS_MARKER" ]]; then
    printf 'pass
%s
' "$summary"
    return 0
  fi
  if [[ -n "$summary" && "$summary" == *"$VALIDATION_FAILURE_MARKER"* ]]; then
    printf 'fail
%s
' "$summary"
    return 0
  fi
  if grep -q "$VALIDATION_FAILURE_MARKER" "$logfile" 2>/dev/null; then
    summary="$(grep "$VALIDATION_FAILURE_MARKER" "$logfile" | tail -n 1)"
    printf 'fail
%s
' "$summary"
    return 0
  fi
  if grep -q '^remote_exit_code=[^0]' "$logfile" 2>/dev/null; then
    summary="remote command exited non-zero without explicit summary marker"
    printf 'fail
%s
' "$summary"
    return 0
  fi
  printf 'unknown
%s
' 'validation summary marker not found'
}

fetch_remote_results() {
  local remote_path="$1"
  local local_dir="$2"

  if [[ -z "$remote_path" ]]; then
    return 1
  fi

  mkdir -p "$local_dir"
  set +e
  scp -r -i "$REMOTE_TEST_SSH_KEY" "$REMOTE_TEST_TARGET:$remote_path" "$local_dir/" 2>&1 | tee "$local_dir/scp.log"
  local rc=${PIPESTATUS[0]}
  set -e
  return "$rc"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out-dir)
      OUT_DIR="$2"
      shift 2
      ;;
    --test-result-file)
      TEST_RESULT_FILE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$OUT_DIR" ]]; then
  echo "--out-dir is required" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
mkdir -p "$(dirname "$TEST_RESULT_FILE")"

pass_id="$(basename "$OUT_DIR")"
local_results_dir="$(dirname "$OUT_DIR")/test_results_${pass_id}"
update_log="$OUT_DIR/update.txt"
ready_log="$OUT_DIR/ready_probe.txt"
mount_log="$OUT_DIR/mount.txt"
validation_log="$OUT_DIR/validation.txt"
dmesg_log="$OUT_DIR/dmesg.txt"

update_rc=0
ready_rc=0
mount_rc=0
validation_rc=0
dmesg_rc=0
fetch_rc=0
result=pass
validation_artifacts=""
validation_summary=""
scripted_validation_result="unknown"
fetched_results_dir=""

log "Remote update step"
if ! run_remote_capture "update" "$update_log" "$REMOTE_REBOOT_COMMAND"; then
  update_rc=$?
  result=fail
fi

log "Waiting ${REMOTE_REBOOT_WAIT_SECONDS}s before ready probe"
sleep "$REMOTE_REBOOT_WAIT_SECONDS"

log "Remote ready probe"
if ! wait_for_remote_ready "$ready_log"; then
  ready_rc=$?
  result=fail
fi

if [[ "$ready_rc" -eq 0 ]]; then
  log "Remote mount step"
  if ! run_remote_capture "mount" "$mount_log" "$REMOTE_MOUNT_COMMAND"; then
    mount_rc=$?
    result=fail
  fi
else
  printf 'skipped because remote host never became ready
' >"$mount_log"
  mount_rc=1
fi

if [[ "$mount_rc" -eq 0 ]]; then
  log "Remote validation step"
  if ! run_remote_capture "validation" "$validation_log" "$REMOTE_VALIDATION_COMMAND"; then
    validation_rc=$?
  fi
else
  printf 'skipped because mount step failed
' >"$validation_log"
  validation_rc=1
fi

validation_artifacts="$(derive_validation_artifacts "$validation_log")"
mapfile -t scripted_review < <(scripted_validation_review "$validation_log")
scripted_validation_result="${scripted_review[0]:-unknown}"
validation_summary="${scripted_review[1]:-}"

if [[ "$scripted_validation_result" != "pass" ]]; then
  result=fail
  if [[ -n "$validation_artifacts" ]]; then
    log "Validation marked failed; fetching remote artifacts from ${validation_artifacts}"
    if fetch_remote_results "$validation_artifacts" "$local_results_dir"; then
      fetched_results_dir="$local_results_dir/$(basename "$validation_artifacts")"
    else
      fetch_rc=$?
      result=fail
      fetched_results_dir="$local_results_dir"
    fi
  fi
fi

log "Remote dmesg collection"
if ! run_remote_capture "dmesg" "$dmesg_log" "$REMOTE_DMESG_COMMAND"; then
  dmesg_rc=$?
  result=fail
fi

cat >"$TEST_RESULT_FILE" <<EOF
# Remote Client Reset Test Result
result=${result}
pass_id=${pass_id}
remote_target=${REMOTE_TEST_TARGET}
out_dir=${OUT_DIR}
local_results_dir=${local_results_dir}
fetched_results_dir=${fetched_results_dir}
update_log=${update_log}
ready_log=${ready_log}
mount_log=${mount_log}
validation_log=${validation_log}
dmesg_log=${dmesg_log}
update_exit_code=${update_rc}
ready_exit_code=${ready_rc}
mount_exit_code=${mount_rc}
validation_exit_code=${validation_rc}
dmesg_exit_code=${dmesg_rc}
fetch_exit_code=${fetch_rc}
validation_artifacts=${validation_artifacts}
validation_summary=${validation_summary}
scripted_validation_result=${scripted_validation_result}
validation_success_marker=${VALIDATION_SUCCESS_MARKER}
validation_failure_marker=${VALIDATION_FAILURE_MARKER}
EOF

append_file_section "Update Output" "$update_log"
append_file_section "Ready Probe Output" "$ready_log"
append_file_section "Mount Output" "$mount_log"
append_file_section "Validation Output" "$validation_log"
append_tail_section "Dmesg Tail" "$dmesg_log" 400
if [[ -n "$fetched_results_dir" ]]; then
  append_file_section "Fetched SCP Log" "$local_results_dir/scp.log"
fi

if [[ "$result" != "pass" ]]; then
  exit 1
fi
