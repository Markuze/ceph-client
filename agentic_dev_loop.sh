#!/usr/bin/env bash
set -euo pipefail

AGENTIC_ENV_FILE="${AGENTIC_ENV_FILE:-.agentic_env}"
DEFAULT_TASK_FILE="${DEFAULT_TASK_FILE:-task.md}"
REVIEW_FILE="${REVIEW_FILE:-review.md}"
TEST_RESULT_FILE="${TEST_RESULT_FILE:-test_result.txt}"
TESTING_DOC_FILE="${TESTING_DOC_FILE:-client_reset_dir/client_reset_testing.md}"
CONTEXT_FILE="${CONTEXT_FILE:-client_reset_dir/client_reset_context.md}"
REVIEW_BASE_COMMIT="${REVIEW_BASE_COMMIT:-6de23f81a5e08be8fbf5e8d7e9febc72a5b5f27f}"

WORKER_PROMPT_FILE="${WORKER_PROMPT_FILE:-.agentic_worker_prompt.txt}"
REVIEWER_PROMPT_FILE="${REVIEWER_PROMPT_FILE:-.agentic_reviewer_prompt.txt}"
ORCHESTRATOR_PROMPT_FILE="${ORCHESTRATOR_PROMPT_FILE:-.agentic_orchestrator_prompt.txt}"
WORKER_OUTPUT_FILE="${WORKER_OUTPUT_FILE:-agentic_worker_output.txt}"
ORCHESTRATOR_OUTPUT_FILE="${ORCHESTRATOR_OUTPUT_FILE:-agentic_orchestrator_output.txt}"
BUILD_LOG_FILE="${BUILD_LOG_FILE:-build.log}"
UPDATE_LOG_FILE="${UPDATE_LOG_FILE:-agentic_update.log}"
TEST_LOG_FILE="${TEST_LOG_FILE:-agentic_tests.log}"
AGENTIC_STATUS_LOG_FILE="${AGENTIC_STATUS_LOG_FILE:-agentic_loop.log}"

AGENT_SESSION_DIR="${AGENT_SESSION_DIR:-.agentic_sessions}"
WORKER_AGENT_NAME="${WORKER_AGENT_NAME:-agent-worker}"
REVIEWER_AGENT_NAME="${REVIEWER_AGENT_NAME:-agent-reviewer}"
ORCHESTRATOR_AGENT_NAME="${ORCHESTRATOR_AGENT_NAME:-agent-orchestrator}"
WORKER_SESSION_ID="${WORKER_SESSION_ID:-$WORKER_AGENT_NAME}"
REVIEWER_SESSION_ID="${REVIEWER_SESSION_ID:-$REVIEWER_AGENT_NAME}"
ORCHESTRATOR_SESSION_ID="${ORCHESTRATOR_SESSION_ID:-$ORCHESTRATOR_AGENT_NAME}"
WORKER_SESSION_FILE="${WORKER_SESSION_FILE:-${AGENT_SESSION_DIR}/worker.session}"
REVIEWER_SESSION_FILE="${REVIEWER_SESSION_FILE:-${AGENT_SESSION_DIR}/reviewer.session}"
ORCHESTRATOR_SESSION_FILE="${ORCHESTRATOR_SESSION_FILE:-${AGENT_SESSION_DIR}/orchestrator.session}"

WORKER_AGENT_MODEL="${WORKER_AGENT_MODEL:-opus}"
REVIEWER_AGENT_MODEL="${REVIEWER_AGENT_MODEL:-codex-5.3-spark}"
ORCHESTRATOR_AGENT_MODEL="${ORCHESTRATOR_AGENT_MODEL:-gpt-5.4-high}"
WORKER_AGENT_EFFORT="${WORKER_AGENT_EFFORT:-high}"
REVIEWER_AGENT_EFFORT="${REVIEWER_AGENT_EFFORT:-extra-high}"
ORCHESTRATOR_AGENT_EFFORT="${ORCHESTRATOR_AGENT_EFFORT:-extra-high}"

AGENT_BUILD_COMMAND="${AGENT_BUILD_COMMAND:-make -j$(nproc) LLVM=1 LLVM_IAS=1 CC='ccache clang' -s ARCH=x86_64}"
MAX_ITERATIONS="${MAX_ITERATIONS:-0}"
RETRY_DELAY="${RETRY_DELAY:-5}"
VERBOSE_CONFIG="${VERBOSE_CONFIG:-0}"
AGENTIC_STREAM_OUTPUT="${AGENTIC_STREAM_OUTPUT:-0}"

REMOTE_TEST_TARGET="${REMOTE_TEST_TARGET:-10.251.64.11}"
REMOTE_TEST_SSH_KEY="${REMOTE_TEST_SSH_KEY:-$HOME/.ssh/id_rsa_cloud}"
REMOTE_REBOOT_COMMAND="${REMOTE_REBOOT_COMMAND:-~/update_reboot.sh}"
REMOTE_MOUNT_COMMAND="${REMOTE_MOUNT_COMMAND:-~/mount.sh}"
REMOTE_READY_COMMAND="${REMOTE_READY_COMMAND:-hostname}"
REMOTE_LINUX_DIR="${REMOTE_LINUX_DIR:-~/linux}"
REMOTE_VALIDATION_COMMAND="${REMOTE_VALIDATION_COMMAND:-sudo -n ~/linux/tools/testing/selftests/filesystems/ceph/run_validation.sh --mount-point /mnt/mycephfs}"
REMOTE_REBOOT_WAIT_SECONDS="${REMOTE_REBOOT_WAIT_SECONDS:-120}"
REMOTE_POLL_INTERVAL_SECONDS="${REMOTE_POLL_INTERVAL_SECONDS:-5}"
REMOTE_READY_TIMEOUT_SECONDS="${REMOTE_READY_TIMEOUT_SECONDS:-900}"
REMOTE_TEST_ARTIFACTS_ROOT="${REMOTE_TEST_ARTIFACTS_ROOT:-client_reset_dir/.remote_test_artifacts}"
AGENTIC_REMOTE_TEST_WRAPPER="${AGENTIC_REMOTE_TEST_WRAPPER:-client_reset_dir/run_remote_client_reset_validation.sh}"

AGENTIC_RESUME_TEST_PROMPT="${AGENTIC_RESUME_TEST_PROMPT:-introduce yourself, what is your task}"
AGENTIC_RESUME_TEST_LOG="${AGENTIC_RESUME_TEST_LOG:-agentic_resume_test.log}"

RUN_MODE="loop"
RESET_SESSIONS=0

AGENTIC_CODE_READY="0"
AGENTIC_ACTION="continue"
AGENTIC_TASK_FILE="$DEFAULT_TASK_FILE"
AGENTIC_COMMIT_MESSAGE="agentic auto commit"
CURRENT_ITERATION="-"
CURRENT_STAGE="startup"
CURRENT_DETAIL_HINT="$AGENTIC_ENV_FILE"

run_update() {
  :
}

log() {
  local left=""
  local line=""
  local pad=1
  printf -v left '[%(%F %T)T] [i=%s s=%s] %s' -1 "$CURRENT_ITERATION" "$CURRENT_STAGE" "$*"
  if (( ${#left} < 50 )); then
    pad=$((50 - ${#left}))
  fi
  printf -v line '%s%*s%s
' "$left" "$pad" '' "detail=${CURRENT_DETAIL_HINT}"
  printf '%s' "$line" >&2
  mkdir -p "$(dirname "$AGENTIC_STATUS_LOG_FILE")"
  printf '%s' "$line" >>"$AGENTIC_STATUS_LOG_FILE"
}

set_loop_context() {
  CURRENT_ITERATION="${1:-$CURRENT_ITERATION}"
  CURRENT_STAGE="${2:-$CURRENT_STAGE}"
  if [[ $# -ge 3 && -n "${3:-}" ]]; then
    CURRENT_DETAIL_HINT="$3"
  fi
}

init_status_log() {
  mkdir -p "$(dirname "$AGENTIC_STATUS_LOG_FILE")"
  printf '=== agentic run start utc=%s pid=%s mode=%s reset=%s ===
'     "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$$" "$RUN_MODE" "$RESET_SESSIONS" >>"$AGENTIC_STATUS_LOG_FILE"
}

trim_whitespace() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "$value"
}

strip_wrapping_quotes() {
  local value="$1"
  if [[ "$value" == \"*\" && "$value" == *\" ]]; then
    value="${value:1:${#value}-2}"
  elif [[ "$value" == \'*\' ]]; then
    value="${value:1:${#value}-2}"
  fi
  printf '%s' "$value"
}

is_valid_uuid() {
  local value="$1"
  [[ "$value" =~ ^[0-9a-fA-F-]{36}$ ]]
}

lowercase() {
  printf '%s' "$1" | tr '[:upper:]' '[:lower:]'
}

normalize_env_values() {
  AGENTIC_CODE_READY="${AGENTIC_CODE_READY:-0}"
  AGENTIC_ACTION="$(lowercase "${AGENTIC_ACTION:-continue}")"
  AGENTIC_TASK_FILE="${AGENTIC_TASK_FILE:-$DEFAULT_TASK_FILE}"
  AGENTIC_COMMIT_MESSAGE="${AGENTIC_COMMIT_MESSAGE:-agentic auto commit}"
  if [[ -z "$AGENTIC_TASK_FILE" ]]; then
    AGENTIC_TASK_FILE="$DEFAULT_TASK_FILE"
  fi
}

write_agentic_env() {
  local code_ready="${1:-$AGENTIC_CODE_READY}"
  local action="${2:-$AGENTIC_ACTION}"
  local task_file="${3:-$AGENTIC_TASK_FILE}"
  local commit_message="${4:-$AGENTIC_COMMIT_MESSAGE}"

  mkdir -p "$(dirname "$AGENTIC_ENV_FILE")"
  cat >"$AGENTIC_ENV_FILE" <<EOF
AGENTIC_CODE_READY=${code_ready}
AGENTIC_ACTION=${action}
AGENTIC_TASK_FILE=${task_file}
AGENTIC_COMMIT_MESSAGE=${commit_message}
EOF
}

ensure_default_env_file() {
  if [[ ! -f "$AGENTIC_ENV_FILE" ]]; then
    write_agentic_env 0 continue "$DEFAULT_TASK_FILE" "agentic auto commit"
    log "Created control env file: ${AGENTIC_ENV_FILE}"
  fi
}

load_agentic_env() {
  local line=""
  local key=""
  local value=""

  ensure_default_env_file
  AGENTIC_CODE_READY="0"
  AGENTIC_ACTION="continue"
  AGENTIC_TASK_FILE="$DEFAULT_TASK_FILE"
  AGENTIC_COMMIT_MESSAGE="agentic auto commit"

  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%$'\r'}"
    if [[ -z "$(trim_whitespace "$line")" ]]; then
      continue
    fi
    if [[ "$line" =~ ^[[:space:]]*# ]]; then
      continue
    fi
    if [[ "$line" != *=* ]]; then
      log "Ignoring malformed control line: ${line}"
      continue
    fi

    key="${line%%=*}"
    value="${line#*=}"
    key="$(trim_whitespace "$key")"
    value="$(strip_wrapping_quotes "$(trim_whitespace "$value")")"

    case "$key" in
      AGENTIC_CODE_READY) AGENTIC_CODE_READY="$value" ;;
      AGENTIC_ACTION) AGENTIC_ACTION="$value" ;;
      AGENTIC_TASK_FILE) AGENTIC_TASK_FILE="$value" ;;
      AGENTIC_COMMIT_MESSAGE) AGENTIC_COMMIT_MESSAGE="$value" ;;
      *) log "Ignoring unknown control key: ${key}" ;;
    esac
  done <"$AGENTIC_ENV_FILE"

  normalize_env_values
}

show_usage() {
  cat <<'EOF'
Usage:
  ./agentic_dev_loop.sh             Run the live orchestration loop
  ./agentic_dev_loop.sh --reset     Reset saved sessions, then run the loop
  ./agentic_dev_loop.sh --test      Run the isolated bootstrap/resume smoke test
  ./agentic_dev_loop.sh -v          Dump config and env values, then run
  ./agentic_dev_loop.sh -h, --help  Show help

Notes:
  - Session resume is the silent default.
  - --reset clears saved agent sessions and resets .agentic_env.
  - MAX_ITERATIONS unset or 0 means unlimited.
EOF
}

dump_config() {
  ensure_default_env_file
  load_agentic_env
  cat <<EOF
AGENTIC_ENV_FILE=$AGENTIC_ENV_FILE
DEFAULT_TASK_FILE=$DEFAULT_TASK_FILE
REVIEW_FILE=$REVIEW_FILE
TEST_RESULT_FILE=$TEST_RESULT_FILE
TESTING_DOC_FILE=$TESTING_DOC_FILE
CONTEXT_FILE=$CONTEXT_FILE
REVIEW_BASE_COMMIT=$REVIEW_BASE_COMMIT
WORKER_PROMPT_FILE=$WORKER_PROMPT_FILE
REVIEWER_PROMPT_FILE=$REVIEWER_PROMPT_FILE
ORCHESTRATOR_PROMPT_FILE=$ORCHESTRATOR_PROMPT_FILE
WORKER_OUTPUT_FILE=$WORKER_OUTPUT_FILE
ORCHESTRATOR_OUTPUT_FILE=$ORCHESTRATOR_OUTPUT_FILE
BUILD_LOG_FILE=$BUILD_LOG_FILE
UPDATE_LOG_FILE=$UPDATE_LOG_FILE
TEST_LOG_FILE=$TEST_LOG_FILE
AGENT_SESSION_DIR=$AGENT_SESSION_DIR
WORKER_SESSION_ID=$WORKER_SESSION_ID
REVIEWER_SESSION_ID=$REVIEWER_SESSION_ID
ORCHESTRATOR_SESSION_ID=$ORCHESTRATOR_SESSION_ID
WORKER_SESSION_FILE=$WORKER_SESSION_FILE
REVIEWER_SESSION_FILE=$REVIEWER_SESSION_FILE
ORCHESTRATOR_SESSION_FILE=$ORCHESTRATOR_SESSION_FILE
WORKER_AGENT_NAME=$WORKER_AGENT_NAME
REVIEWER_AGENT_NAME=$REVIEWER_AGENT_NAME
ORCHESTRATOR_AGENT_NAME=$ORCHESTRATOR_AGENT_NAME
WORKER_AGENT_MODEL=$WORKER_AGENT_MODEL
REVIEWER_AGENT_MODEL=$REVIEWER_AGENT_MODEL
ORCHESTRATOR_AGENT_MODEL=$ORCHESTRATOR_AGENT_MODEL
WORKER_AGENT_EFFORT=$WORKER_AGENT_EFFORT
REVIEWER_AGENT_EFFORT=$REVIEWER_AGENT_EFFORT
ORCHESTRATOR_AGENT_EFFORT=$ORCHESTRATOR_AGENT_EFFORT
AGENT_BUILD_COMMAND=$AGENT_BUILD_COMMAND
MAX_ITERATIONS=$MAX_ITERATIONS
RETRY_DELAY=$RETRY_DELAY
VERBOSE_CONFIG=$VERBOSE_CONFIG
AGENTIC_STREAM_OUTPUT=$AGENTIC_STREAM_OUTPUT
AGENTIC_STATUS_LOG_FILE=$AGENTIC_STATUS_LOG_FILE
REMOTE_TEST_TARGET=$REMOTE_TEST_TARGET
REMOTE_TEST_SSH_KEY=$REMOTE_TEST_SSH_KEY
REMOTE_REBOOT_COMMAND=$REMOTE_REBOOT_COMMAND
REMOTE_MOUNT_COMMAND=$REMOTE_MOUNT_COMMAND
REMOTE_READY_COMMAND=$REMOTE_READY_COMMAND
REMOTE_LINUX_DIR=$REMOTE_LINUX_DIR
REMOTE_VALIDATION_COMMAND=$REMOTE_VALIDATION_COMMAND
REMOTE_REBOOT_WAIT_SECONDS=$REMOTE_REBOOT_WAIT_SECONDS
REMOTE_POLL_INTERVAL_SECONDS=$REMOTE_POLL_INTERVAL_SECONDS
REMOTE_READY_TIMEOUT_SECONDS=$REMOTE_READY_TIMEOUT_SECONDS
REMOTE_TEST_ARTIFACTS_ROOT=$REMOTE_TEST_ARTIFACTS_ROOT
AGENTIC_REMOTE_TEST_WRAPPER=$AGENTIC_REMOTE_TEST_WRAPPER
AGENTIC_CODE_READY=$AGENTIC_CODE_READY
AGENTIC_ACTION=$AGENTIC_ACTION
AGENTIC_TASK_FILE=$AGENTIC_TASK_FILE
AGENTIC_COMMIT_MESSAGE=$AGENTIC_COMMIT_MESSAGE
EOF
}

ensure_file_parent() {
  mkdir -p "$(dirname "$1")"
}

ensure_loop_files() {
  ensure_file_parent "$AGENTIC_TASK_FILE"
  ensure_file_parent "$REVIEW_FILE"
  ensure_file_parent "$TEST_RESULT_FILE"
  ensure_file_parent "$WORKER_OUTPUT_FILE"
  ensure_file_parent "$ORCHESTRATOR_OUTPUT_FILE"
  ensure_file_parent "$BUILD_LOG_FILE"
  touch "$AGENTIC_TASK_FILE" "$REVIEW_FILE" "$TEST_RESULT_FILE" "$WORKER_OUTPUT_FILE" "$ORCHESTRATOR_OUTPUT_FILE" "$BUILD_LOG_FILE"
}

file_has_non_whitespace() {
  local path="$1"
  [[ -f "$path" ]] && grep -q '[^[:space:]]' "$path"
}

remote_test_summary() {
  cat <<EOF
Remote validation sequence:
1. ssh -i ${REMOTE_TEST_SSH_KEY} ${REMOTE_TEST_TARGET} "${REMOTE_REBOOT_COMMAND}"
2. sleep ${REMOTE_REBOOT_WAIT_SECONDS}; then loop ssh -i ${REMOTE_TEST_SSH_KEY} ${REMOTE_TEST_TARGET} "${REMOTE_READY_COMMAND}" until it succeeds
3. ssh -i ${REMOTE_TEST_SSH_KEY} ${REMOTE_TEST_TARGET} "${REMOTE_MOUNT_COMMAND}"
4. ssh -i ${REMOTE_TEST_SSH_KEY} ${REMOTE_TEST_TARGET} "${REMOTE_VALIDATION_COMMAND}"
5. perform scripted validation-log review; if the agreed failure marker appears, fetch remote artifacts into test_results_<pass_id>
6. collect validation output and dmesg into ${TEST_RESULT_FILE}
EOF
}

init_agent_sessions() {
  mkdir -p "$AGENT_SESSION_DIR"

  if [[ -f "$WORKER_SESSION_FILE" ]]; then
    WORKER_SESSION_ID="$(cat "$WORKER_SESSION_FILE")"
    if ! is_valid_uuid "$WORKER_SESSION_ID"; then
      log "Invalid worker session id '$WORKER_SESSION_ID'; resetting."
      rm -f "$WORKER_SESSION_FILE"
      WORKER_SESSION_ID="$WORKER_AGENT_NAME"
    fi
  fi

  if [[ -f "$REVIEWER_SESSION_FILE" ]]; then
    REVIEWER_SESSION_ID="$(cat "$REVIEWER_SESSION_FILE")"
    if ! is_valid_uuid "$REVIEWER_SESSION_ID"; then
      log "Invalid reviewer session id '$REVIEWER_SESSION_ID'; resetting."
      rm -f "$REVIEWER_SESSION_FILE"
      REVIEWER_SESSION_ID="$REVIEWER_AGENT_NAME"
    fi
  fi

  if [[ -f "$ORCHESTRATOR_SESSION_FILE" ]]; then
    ORCHESTRATOR_SESSION_ID="$(cat "$ORCHESTRATOR_SESSION_FILE")"
    if ! is_valid_uuid "$ORCHESTRATOR_SESSION_ID"; then
      log "Invalid orchestrator session id '$ORCHESTRATOR_SESSION_ID'; resetting."
      rm -f "$ORCHESTRATOR_SESSION_FILE"
      ORCHESTRATOR_SESSION_ID="$ORCHESTRATOR_AGENT_NAME"
    fi
  fi
}

reset_agent_sessions() {
  rm -f "$WORKER_SESSION_FILE" "$REVIEWER_SESSION_FILE" "$ORCHESTRATOR_SESSION_FILE"
  WORKER_SESSION_ID="$WORKER_AGENT_NAME"
  REVIEWER_SESSION_ID="$REVIEWER_AGENT_NAME"
  ORCHESTRATOR_SESSION_ID="$ORCHESTRATOR_AGENT_NAME"
  write_agentic_env 0 continue "$DEFAULT_TASK_FILE" "agentic auto commit"
  log "Reset agent sessions and control state."
}

agentic_session_file() {
  case "$1" in
    worker) printf '%s\n' "$WORKER_SESSION_FILE" ;;
    reviewer) printf '%s\n' "$REVIEWER_SESSION_FILE" ;;
    orchestrator) printf '%s\n' "$ORCHESTRATOR_SESSION_FILE" ;;
    *) return 1 ;;
  esac
}

agentic_session_exists() {
  local session_file
  session_file="$(agentic_session_file "$1")"
  [[ -s "$session_file" ]]
}

run_agent_output() {
  local output_file="$1"
  shift

  if [[ "$AGENTIC_STREAM_OUTPUT" == "1" ]]; then
    "$@" 2>&1 | tee "$output_file"
  else
    "$@" >"$output_file" 2>&1
  fi
}

run_claude_bootstrap_agent() {
  local prompt="$1"
  local output_file="$2"
  local session_file="$3"
  local session_name="$4"
  local model="$5"
  local effort="$6"
  local session_id=""

  if ! command -v claude >/dev/null 2>&1; then
    log "claude command not available."
    return 1
  fi

  if command -v uuidgen >/dev/null 2>&1; then
    session_id="$(uuidgen)"
  elif [[ -r /proc/sys/kernel/random/uuid ]]; then
    session_id="$(cat /proc/sys/kernel/random/uuid)"
  else
    log "Unable to generate a UUID for claude session bootstrap."
    return 1
  fi

  printf '%s\n' "$session_id" >"$session_file"
  log "Worker bootstrap start: session_id=${session_id}"
  if ! run_agent_output "$output_file" claude -n "$session_name" --session-id "$session_id" --model "$model" --effort "$effort" --dangerously-skip-permissions -p "$prompt"; then
    rm -f "$session_file"
    return 1
  fi
  WORKER_SESSION_ID="$session_id"
}

run_claude_resume_agent() {
  local prompt="$1"
  local output_file="$2"
  local session_id="$3"
  local effort="$4"

  if ! command -v claude >/dev/null 2>&1; then
    log "claude command not available."
    return 1
  fi
  if [[ -z "$session_id" ]] || ! is_valid_uuid "$session_id"; then
    log "Worker resume start: invalid session id '${session_id}'"
    return 1
  fi

  log "Worker resume start: session_id=${session_id}"
  run_agent_output "$output_file" claude -r "$session_id" --effort "$effort" --dangerously-skip-permissions -p "$prompt"
}

run_cursor_bootstrap_agent() {
  local prompt="$1"
  local output_file="$2"
  local session_file="$3"
  local model="$4"
  local create_output=""
  local chat_id=""
  local rc=0

  if ! command -v cursor-agent >/dev/null 2>&1; then
    log "cursor-agent command not available."
    return 1
  fi

  mkdir -p .cursor
  cat >.cursor/cli.json <<'EOF'
{"permissions":{"allow":[],"deny":[]}}
EOF

  if create_output="$(cursor-agent create-chat 2>&1)"; then
    rc=0
  else
    rc=$?
  fi
  if [[ "$rc" -ne 0 ]]; then
    log "cursor-agent bootstrap failed: create-chat exited ${rc}"
    log "$create_output"
    return "$rc"
  fi

  chat_id="$(printf '%s\n' "$create_output" | tr -d '\r' | grep -Eo '[0-9a-fA-F-]{36}' | tail -n 1)"
  if [[ -z "$chat_id" ]]; then
    log "cursor-agent bootstrap failed: unable to parse chat id from create-chat output"
    log "$create_output"
    return 1
  fi

  printf '%s\n' "$chat_id" >"$session_file"
  log "Cursor bootstrap start: session_id=${chat_id}"
  run_agent_output "$output_file" cursor-agent --print --trust --yolo --model "$model" --resume "$chat_id" "$prompt"
}

run_cursor_resume_agent() {
  local prompt="$1"
  local output_file="$2"
  local session_file="$3"
  local model="$4"
  local chat_id=""

  if ! command -v cursor-agent >/dev/null 2>&1; then
    log "cursor-agent command not available."
    return 1
  fi

  if [[ -f "$session_file" ]]; then
    chat_id="$(cat "$session_file")"
  fi
  if [[ -z "$chat_id" ]] || ! is_valid_uuid "$chat_id"; then
    log "Cursor resume start: invalid or missing session id '${chat_id}'"
    return 1
  fi

  log "Cursor resume start: session_id=${chat_id}"
  run_agent_output "$output_file" cursor-agent --print --trust --yolo --model "$model" --resume "$chat_id" "$prompt"
}

run_agent() {
  local role="$1"
  local prompt_file="$2"
  local output_file="$3"
  local role_key="$4"
  local use_resume="$5"
  local prompt=""
  local session_file=""
  local session_id=""

  prompt="$(cat "$prompt_file")"

  case "$role_key" in
    worker)
      session_file="$WORKER_SESSION_FILE"
      if [[ "$use_resume" == "1" ]]; then
        run_claude_resume_agent "$prompt" "$output_file" "$WORKER_SESSION_ID" "$WORKER_AGENT_EFFORT"
      else
        run_claude_bootstrap_agent "$prompt" "$output_file" "$session_file" "$WORKER_AGENT_NAME" "$WORKER_AGENT_MODEL" "$WORKER_AGENT_EFFORT"
      fi
      ;;
    reviewer)
      session_file="$REVIEWER_SESSION_FILE"
      if [[ "$use_resume" == "1" ]]; then
        run_cursor_resume_agent "$prompt" "$output_file" "$session_file" "$REVIEWER_AGENT_MODEL"
      else
        run_cursor_bootstrap_agent "$prompt" "$output_file" "$session_file" "$REVIEWER_AGENT_MODEL"
      fi
      ;;
    orchestrator)
      session_file="$ORCHESTRATOR_SESSION_FILE"
      if [[ "$use_resume" == "1" ]]; then
        run_cursor_resume_agent "$prompt" "$output_file" "$session_file" "$ORCHESTRATOR_AGENT_MODEL"
      else
        run_cursor_bootstrap_agent "$prompt" "$output_file" "$session_file" "$ORCHESTRATOR_AGENT_MODEL"
      fi
      ;;
    *)
      log "Unknown role key for run_agent: ${role_key}"
      return 1
      ;;
  esac
}

worker_bootstrap_prompt() {
  cat <<EOF
You are the Worker agent.
Before doing anything else, read ${CONTEXT_FILE} and retain the relevant project context for later resume turns.
The authoritative local build command for this repo is:
${AGENT_BUILD_COMMAND}
If you decide to build manually inside the agent harness, use exactly that command.
Your standing work-turn prompt is:
read ${AGENTIC_TASK_FILE} and fix accordingly

Current turn:
read ${AGENTIC_TASK_FILE} and fix accordingly
EOF
}

worker_resume_prompt() {
  printf 'read %s and fix accordingly\n' "$AGENTIC_TASK_FILE"
}

# TODO: Feed explicit orchestrator rebuttals and project-specific design guidance back into Reviewer-1 on follow-up passes so review severity can converge across iterations.

reviewer_bootstrap_prompt() {
  local build_status="$1"
  local worker_status="$2"
  cat <<EOF
You are Reviewer-1.
Before doing anything else, read ${CONTEXT_FILE} and retain the relevant project context for later resume turns.
Your standing review prompt is:
review all code changes since ${REVIEW_BASE_COMMIT} and give an accurate assessment of the code quality and readiness
Write only the review text. The harness will capture your response in ${REVIEW_FILE}.
Consult the current codebase state directly, including uncommitted changes, and consider ${BUILD_LOG_FILE}.

Current turn:
review all code changes since ${REVIEW_BASE_COMMIT} and give an accurate assessment of the code quality and readiness
Write only the review text. The harness will capture your response in ${REVIEW_FILE}.
Worker status: ${worker_status}
Local build status: ${build_status}
Local build log: ${BUILD_LOG_FILE}
EOF
}

reviewer_resume_prompt() {
  local build_status="$1"
  local worker_status="$2"
  cat <<EOF
review all code changes since ${REVIEW_BASE_COMMIT} and give an accurate assessment of the code quality and readiness
Write only the review text. The harness will capture your response in ${REVIEW_FILE}.
Worker status: ${worker_status}
Local build status: ${build_status}
Local build log: ${BUILD_LOG_FILE}
EOF
}

orchestrator_bootstrap_prefix() {
  cat <<EOF
You are the Orchestrator.
Before doing anything else, read ${CONTEXT_FILE} and retain the relevant project context for later resume turns.
Consult ${TESTING_DOC_FILE} when the testing results need interpretation.
$(remote_test_summary)

When you update ${AGENTIC_ENV_FILE}, write only these keys:
AGENTIC_CODE_READY=0|1
AGENTIC_ACTION=continue|test|commit|stop
AGENTIC_TASK_FILE=${AGENTIC_TASK_FILE}
AGENTIC_COMMIT_MESSAGE=<text>
Be strict about vestigial code from obsolete manual-reset designs. The current tree should keep only code and comments needed by the latest v2 teardown/state-machine design. If you find dead reconnect-generation bookkeeping, stale comments, or other legacy artifacts that are not required by the latest design, prefer cleanup before advancing.
EOF
}

orchestrator_review_prompt_body() {
  local build_status="$1"
  local worker_status="$2"
  cat <<EOF
read ${REVIEW_FILE} and write detailed instructions to ${AGENTIC_TASK_FILE} consult the code if needed. if the code is ready for testing update the ${AGENTIC_ENV_FILE} file accordingly and leave no task instructions.
If no more fixes are needed after review and the branch should be committed and pushed before remote testing, set AGENTIC_ACTION=commit and provide AGENTIC_COMMIT_MESSAGE.
If the branch is already committed and pushed and is ready for remote testing, set AGENTIC_ACTION=test.
If more fixes are needed, set AGENTIC_ACTION=continue and write detailed instructions to ${AGENTIC_TASK_FILE}.
If the work is done and no further action is required, set AGENTIC_CODE_READY=1 and AGENTIC_ACTION=stop.
Current phase: post_review
Worker status: ${worker_status}
Local build status: ${build_status}
Review file: ${REVIEW_FILE}
Task file: ${AGENTIC_TASK_FILE}
EOF
}

orchestrator_test_prompt_body() {
  local test_status="$1"
  cat <<EOF
read ${TEST_RESULT_FILE} and write detailed instructions to ${AGENTIC_TASK_FILE} consult the code if needed. if the test results are good according to the testing scripts and docs update the ${AGENTIC_ENV_FILE} accordingly.
If more fixes are needed, set AGENTIC_ACTION=continue and write detailed instructions to ${AGENTIC_TASK_FILE}.
If the test should be rerun without new code changes, set AGENTIC_ACTION=test and leave no task instructions.
If the test results are good, set AGENTIC_CODE_READY=1 and AGENTIC_ACTION=stop and leave no task instructions.
Current phase: post_test
Recorded test status: ${test_status}
Testing docs: ${TESTING_DOC_FILE}
Test result file: ${TEST_RESULT_FILE}
Task file: ${AGENTIC_TASK_FILE}
EOF
}

build_orchestrator_review_prompt() {
  local use_resume="$1"
  local build_status="$2"
  local worker_status="$3"
  if [[ "$use_resume" == "1" ]]; then
    orchestrator_review_prompt_body "$build_status" "$worker_status"
  else
    {
      orchestrator_bootstrap_prefix
      echo
      orchestrator_review_prompt_body "$build_status" "$worker_status"
    }
  fi
}

build_orchestrator_test_prompt() {
  local use_resume="$1"
  local test_status="$2"
  if [[ "$use_resume" == "1" ]]; then
    orchestrator_test_prompt_body "$test_status"
  else
    {
      orchestrator_bootstrap_prefix
      echo
      orchestrator_test_prompt_body "$test_status"
    }
  fi
}

run_worker() {
  local iteration="$1"
  local use_resume=0

  if agentic_session_exists worker; then
    use_resume=1
    WORKER_SESSION_ID="$(cat "$WORKER_SESSION_FILE")"
  fi

  if [[ "$use_resume" == "1" ]]; then
    worker_resume_prompt >"$WORKER_PROMPT_FILE"
    log "Starting Worker with resume (session_id=${WORKER_SESSION_ID})"
  else
    worker_bootstrap_prompt >"$WORKER_PROMPT_FILE"
    log "Starting Worker with bootstrap (session_id=${WORKER_AGENT_NAME})"
  fi

  : >"$WORKER_OUTPUT_FILE"
  if ! run_agent "Worker" "$WORKER_PROMPT_FILE" "$WORKER_OUTPUT_FILE" worker "$use_resume"; then
    return 1
  fi

  if [[ "$use_resume" == "0" && -f "$WORKER_SESSION_FILE" ]]; then
    WORKER_SESSION_ID="$(cat "$WORKER_SESSION_FILE")"
  fi
}

run_reviewer() {
  local build_status="$1"
  local worker_status="$2"
  local use_resume=0

  if agentic_session_exists reviewer; then
    use_resume=1
    REVIEWER_SESSION_ID="$(cat "$REVIEWER_SESSION_FILE")"
  fi

  if [[ "$use_resume" == "1" ]]; then
    reviewer_resume_prompt "$build_status" "$worker_status" >"$REVIEWER_PROMPT_FILE"
    log "Starting Reviewer-1 with resume (session_id=${REVIEWER_SESSION_ID})"
  else
    reviewer_bootstrap_prompt "$build_status" "$worker_status" >"$REVIEWER_PROMPT_FILE"
    log "Starting Reviewer-1 with bootstrap"
  fi

  : >"$REVIEW_FILE"
  if ! run_agent "Reviewer-1" "$REVIEWER_PROMPT_FILE" "$REVIEW_FILE" reviewer "$use_resume"; then
    return 1
  fi
}

run_orchestrator_review() {
  local build_status="$1"
  local worker_status="$2"
  local use_resume=0

  if agentic_session_exists orchestrator; then
    use_resume=1
    ORCHESTRATOR_SESSION_ID="$(cat "$ORCHESTRATOR_SESSION_FILE")"
  fi

  build_orchestrator_review_prompt "$use_resume" "$build_status" "$worker_status" >"$ORCHESTRATOR_PROMPT_FILE"
  : >"$ORCHESTRATOR_OUTPUT_FILE"
  if [[ "$use_resume" == "1" ]]; then
    log "Starting Orchestrator with resume (session_id=${ORCHESTRATOR_SESSION_ID})"
  else
    log "Starting Orchestrator with bootstrap"
  fi

  run_agent "Orchestrator" "$ORCHESTRATOR_PROMPT_FILE" "$ORCHESTRATOR_OUTPUT_FILE" orchestrator "$use_resume"
}

run_orchestrator_post_test() {
  local test_status="$1"
  local use_resume=0

  if agentic_session_exists orchestrator; then
    use_resume=1
    ORCHESTRATOR_SESSION_ID="$(cat "$ORCHESTRATOR_SESSION_FILE")"
  fi

  build_orchestrator_test_prompt "$use_resume" "$test_status" >"$ORCHESTRATOR_PROMPT_FILE"
  : >"$ORCHESTRATOR_OUTPUT_FILE"
  if [[ "$use_resume" == "1" ]]; then
    log "Starting Orchestrator post-test with resume (session_id=${ORCHESTRATOR_SESSION_ID})"
  else
    log "Starting Orchestrator post-test with bootstrap"
  fi

  run_agent "Orchestrator" "$ORCHESTRATOR_PROMPT_FILE" "$ORCHESTRATOR_OUTPUT_FILE" orchestrator "$use_resume"
}

run_build() {
  local iteration="$1"
  log "Build iteration ${iteration}: ${AGENT_BUILD_COMMAND}"
  if [[ "$AGENTIC_STREAM_OUTPUT" == "1" ]]; then
    bash -lc "$AGENT_BUILD_COMMAND" 2>&1 | tee "$BUILD_LOG_FILE"
  else
    bash -lc "$AGENT_BUILD_COMMAND" >"$BUILD_LOG_FILE" 2>&1
  fi
}

should_skip_commit_path() {
  case "$1" in
    agentic_dev_loop.sh|client_reset_dir|client_reset_dir/*|.agentic_env|.agentic_sessions|.agentic_sessions/*|.agentic_*|task.md|review.md|test_result.txt|build.log|agentic_worker_output.txt|agentic_orchestrator_output.txt|agentic_update.log|agentic_tests.log)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

stage_commit_changes() {
  local path=""
  git add -u
  while IFS= read -r path; do
    if should_skip_commit_path "$path"; then
      log "Skipping DO NOT COMMIT path: ${path}"
      continue
    fi
    git add -- "$path"
  done < <(git ls-files --others --exclude-standard)
}

commit_and_push() {
  stage_commit_changes
  if [[ -n "$(git diff --cached --name-only)" ]]; then
    git commit -m "$AGENTIC_COMMIT_MESSAGE"
  else
    log "No staged changes to commit; pushing current branch only."
  fi
  git push
}

write_test_failure_result() {
  local summary="$1"
  cat >"$TEST_RESULT_FILE" <<EOF
# Remote Client Reset Test Result
result=fail
summary=${summary}
EOF
}

run_tests() {
  mkdir -p "$REMOTE_TEST_ARTIFACTS_ROOT"
  if [[ ! -f "$AGENTIC_REMOTE_TEST_WRAPPER" ]]; then
    log "Remote test wrapper missing: ${AGENTIC_REMOTE_TEST_WRAPPER}"
    write_test_failure_result "missing remote test wrapper ${AGENTIC_REMOTE_TEST_WRAPPER}"
    return 1
  fi

  if [[ "$AGENTIC_STREAM_OUTPUT" == "1" ]]; then
    bash "$AGENTIC_REMOTE_TEST_WRAPPER" --out-dir "$REMOTE_TEST_ARTIFACTS_ROOT/$(date +%Y%m%d-%H%M%S)" --test-result-file "$TEST_RESULT_FILE" 2>&1 | tee "$TEST_LOG_FILE"
  else
    bash "$AGENTIC_REMOTE_TEST_WRAPPER" --out-dir "$REMOTE_TEST_ARTIFACTS_ROOT/$(date +%Y%m%d-%H%M%S)" --test-result-file "$TEST_RESULT_FILE" >"$TEST_LOG_FILE" 2>&1
  fi
}

run_agent_bootstrap_probe() {
  local role="$1"
  local prompt_file="$2"
  local output_file="$3"
  local role_key="$4"
  local bootstrap_prompt="$5"

  printf '%s\n' "$bootstrap_prompt" >"$prompt_file"
  : >"$output_file"
  run_agent "$role" "$prompt_file" "$output_file" "$role_key" 0
}

run_agent_resume_probe() {
  local role="$1"
  local prompt_file="$2"
  local output_file="$3"
  local role_key="$4"
  local session_file="$5"
  local session_id=""

  if [[ ! -s "$session_file" ]]; then
    log "Resume probe for ${role} failed: missing session file ${session_file}"
    return 1
  fi
  session_id="$(cat "$session_file")"
  if ! is_valid_uuid "$session_id"; then
    log "Resume probe for ${role} failed: invalid session id '${session_id}'"
    return 1
  fi

  if [[ "$role_key" == "worker" ]]; then
    WORKER_SESSION_ID="$session_id"
  fi

  printf '%s\n' "$AGENTIC_RESUME_TEST_PROMPT" >"$prompt_file"
  : >"$output_file"
  run_agent "$role" "$prompt_file" "$output_file" "$role_key" 1
}

run_agent_resume_smoke_test() {
  local saved_agentic_env_file="$AGENTIC_ENV_FILE"
  local saved_default_task_file="$DEFAULT_TASK_FILE"
  local saved_review_file="$REVIEW_FILE"
  local saved_test_result_file="$TEST_RESULT_FILE"
  local saved_context_file="$CONTEXT_FILE"
  local saved_testing_doc_file="$TESTING_DOC_FILE"
  local saved_agent_session_dir="$AGENT_SESSION_DIR"
  local saved_worker_prompt_file="$WORKER_PROMPT_FILE"
  local saved_reviewer_prompt_file="$REVIEWER_PROMPT_FILE"
  local saved_orchestrator_prompt_file="$ORCHESTRATOR_PROMPT_FILE"
  local saved_worker_output_file="$WORKER_OUTPUT_FILE"
  local saved_orchestrator_output_file="$ORCHESTRATOR_OUTPUT_FILE"
  local saved_build_log_file="$BUILD_LOG_FILE"
  local saved_worker_session_file="$WORKER_SESSION_FILE"
  local saved_reviewer_session_file="$REVIEWER_SESSION_FILE"
  local saved_orchestrator_session_file="$ORCHESTRATOR_SESSION_FILE"
  local saved_worker_session_id="$WORKER_SESSION_ID"
  local saved_reviewer_session_id="$REVIEWER_SESSION_ID"
  local saved_orchestrator_session_id="$ORCHESTRATOR_SESSION_ID"
  local saved_stream_output="$AGENTIC_STREAM_OUTPUT"
  local saved_remote_test_artifacts_root="$REMOTE_TEST_ARTIFACTS_ROOT"
  local saved_test_log_file="$TEST_LOG_FILE"
  local test_root=""
  local worker_resume_output=""
  local reviewer_resume_output=""
  local orchestrator_resume_output=""
  local failures=0
  local rc=0

  test_root="$(mktemp -d "${PWD}/.agentic_resume_test.XXXXXX")"
  log "Agent resume smoke test artifacts: ${test_root}"

  AGENTIC_ENV_FILE="${test_root}/.agentic_env"
  DEFAULT_TASK_FILE="${test_root}/task.md"
  REVIEW_FILE="${test_root}/review.md"
  TEST_RESULT_FILE="${test_root}/test_result.txt"
  AGENT_SESSION_DIR="${test_root}/.agentic_sessions"
  WORKER_PROMPT_FILE="${test_root}/worker.prompt.txt"
  REVIEWER_PROMPT_FILE="${test_root}/reviewer.prompt.txt"
  ORCHESTRATOR_PROMPT_FILE="${test_root}/orchestrator.prompt.txt"
  WORKER_OUTPUT_FILE="${test_root}/worker.bootstrap.txt"
  ORCHESTRATOR_OUTPUT_FILE="${test_root}/orchestrator.bootstrap.txt"
  BUILD_LOG_FILE="${test_root}/build.log"
  TEST_LOG_FILE="${test_root}/agentic_resume_test.log"
  WORKER_SESSION_FILE="${AGENT_SESSION_DIR}/worker.session"
  REVIEWER_SESSION_FILE="${AGENT_SESSION_DIR}/reviewer.session"
  ORCHESTRATOR_SESSION_FILE="${AGENT_SESSION_DIR}/orchestrator.session"
  WORKER_SESSION_ID="$WORKER_AGENT_NAME"
  REVIEWER_SESSION_ID="$REVIEWER_AGENT_NAME"
  ORCHESTRATOR_SESSION_ID="$ORCHESTRATOR_AGENT_NAME"
  REMOTE_TEST_ARTIFACTS_ROOT="${test_root}/remote-test-artifacts"
  AGENTIC_STREAM_OUTPUT=1

  worker_resume_output="${test_root}/worker.resume.txt"
  reviewer_resume_output="${test_root}/reviewer.resume.txt"
  orchestrator_resume_output="${test_root}/orchestrator.resume.txt"

  ensure_default_env_file
  init_agent_sessions
  ensure_loop_files

  log "Agent resume smoke test: bootstrap pass"
  if ! run_agent_bootstrap_probe "Worker" "$WORKER_PROMPT_FILE" "$WORKER_OUTPUT_FILE" worker "You are the Worker agent.
Before doing anything else, read ${CONTEXT_FILE} and retain the relevant project context for later resume turns.
The authoritative local build command for this repo is:
${AGENT_BUILD_COMMAND}
Your task is to read ${DEFAULT_TASK_FILE} and fix accordingly.
For now, briefly introduce yourself and state your task."; then
    ((failures += 1))
  fi
  if ! run_agent_bootstrap_probe "Reviewer-1" "$REVIEWER_PROMPT_FILE" "$REVIEW_FILE" reviewer "You are Reviewer-1.
Before doing anything else, read ${CONTEXT_FILE} and retain the relevant project context for later resume turns.
Your task is to review all code changes since ${REVIEW_BASE_COMMIT} and give an accurate assessment of the code quality and readiness.
For now, briefly introduce yourself and state your task."; then
    ((failures += 1))
  fi
  if ! run_agent_bootstrap_probe "Orchestrator" "$ORCHESTRATOR_PROMPT_FILE" "$ORCHESTRATOR_OUTPUT_FILE" orchestrator "You are the Orchestrator.
Before doing anything else, read ${CONTEXT_FILE} and retain the relevant project context for later resume turns.
Your task is to read ${REVIEW_FILE} and ${TEST_RESULT_FILE}, write detailed instructions to ${DEFAULT_TASK_FILE}, and update ${AGENTIC_ENV_FILE} when the code is ready to test or stop.
For now, briefly introduce yourself and state your task."; then
    ((failures += 1))
  fi

  log "Agent resume smoke test: resume probe"
  if ! run_agent_resume_probe "Worker" "$WORKER_PROMPT_FILE" "$worker_resume_output" worker "$WORKER_SESSION_FILE"; then
    ((failures += 1))
  fi
  if ! run_agent_resume_probe "Reviewer-1" "$REVIEWER_PROMPT_FILE" "$reviewer_resume_output" reviewer "$REVIEWER_SESSION_FILE"; then
    ((failures += 1))
  fi
  if ! run_agent_resume_probe "Orchestrator" "$ORCHESTRATOR_PROMPT_FILE" "$orchestrator_resume_output" orchestrator "$ORCHESTRATOR_SESSION_FILE"; then
    ((failures += 1))
  fi

  if [[ "$failures" -ne 0 ]]; then
    rc=1
  fi

  {
    echo "# Agent resume smoke test results"
    echo "result=$(if [[ "$rc" -eq 0 ]]; then echo pass; else echo fail; fi)"
    echo "artifacts_dir=${test_root}"
    echo "task_file=${DEFAULT_TASK_FILE}"
    echo "review_file=${REVIEW_FILE}"
    echo "test_result_file=${TEST_RESULT_FILE}"
    echo "resume_prompt=${AGENTIC_RESUME_TEST_PROMPT}"
    echo "worker_bootstrap_output=${WORKER_OUTPUT_FILE}"
    echo "worker_resume_output=${worker_resume_output}"
    echo "reviewer_bootstrap_output=${REVIEW_FILE}"
    echo "reviewer_resume_output=${reviewer_resume_output}"
    echo "orchestrator_bootstrap_output=${ORCHESTRATOR_OUTPUT_FILE}"
    echo "orchestrator_resume_output=${orchestrator_resume_output}"
  } | tee "$AGENTIC_RESUME_TEST_LOG"

  AGENTIC_ENV_FILE="$saved_agentic_env_file"
  DEFAULT_TASK_FILE="$saved_default_task_file"
  REVIEW_FILE="$saved_review_file"
  TEST_RESULT_FILE="$saved_test_result_file"
  CONTEXT_FILE="$saved_context_file"
  TESTING_DOC_FILE="$saved_testing_doc_file"
  AGENT_SESSION_DIR="$saved_agent_session_dir"
  WORKER_PROMPT_FILE="$saved_worker_prompt_file"
  REVIEWER_PROMPT_FILE="$saved_reviewer_prompt_file"
  ORCHESTRATOR_PROMPT_FILE="$saved_orchestrator_prompt_file"
  WORKER_OUTPUT_FILE="$saved_worker_output_file"
  ORCHESTRATOR_OUTPUT_FILE="$saved_orchestrator_output_file"
  BUILD_LOG_FILE="$saved_build_log_file"
  WORKER_SESSION_FILE="$saved_worker_session_file"
  REVIEWER_SESSION_FILE="$saved_reviewer_session_file"
  ORCHESTRATOR_SESSION_FILE="$saved_orchestrator_session_file"
  WORKER_SESSION_ID="$saved_worker_session_id"
  REVIEWER_SESSION_ID="$saved_reviewer_session_id"
  ORCHESTRATOR_SESSION_ID="$saved_orchestrator_session_id"
  AGENTIC_STREAM_OUTPUT="$saved_stream_output"
  REMOTE_TEST_ARTIFACTS_ROOT="$saved_remote_test_artifacts_root"
  TEST_LOG_FILE="$saved_test_log_file"

  return "$rc"
}

main_loop() {
  local iteration=0
  local worker_status="skipped"
  local build_status="not_run"
  local test_status="not_run"
  local action=""

  set_loop_context "-" startup "$AGENTIC_ENV_FILE"
  log "Initializing main loop."
  ensure_default_env_file
  if [[ "$RESET_SESSIONS" == "1" ]]; then
    set_loop_context "-" reset "$AGENTIC_ENV_FILE"
    log "Resetting saved sessions."
    reset_agent_sessions
  fi
  init_agent_sessions

  while true; do
    set_loop_context "$iteration" load "$AGENTIC_ENV_FILE"
    load_agentic_env
    ensure_loop_files

    if [[ "$AGENTIC_CODE_READY" == "1" ]]; then
      log "AGENTIC_CODE_READY=1 in ${AGENTIC_ENV_FILE}; exiting."
      break
    fi

    ((iteration += 1))
    set_loop_context "$iteration" iter "$AGENTIC_TASK_FILE"
    log "Starting iteration ${iteration}."
    if [[ "$MAX_ITERATIONS" -ne 0 && "$iteration" -gt "$MAX_ITERATIONS" ]]; then
      log "Reached MAX_ITERATIONS=${MAX_ITERATIONS}; exiting."
      break
    fi

    worker_status="skipped"
    if file_has_non_whitespace "$AGENTIC_TASK_FILE"; then
      worker_status="pass"
      set_loop_context "$iteration" worker "$WORKER_OUTPUT_FILE"
      log "Running Worker."
      if ! run_worker "$iteration"; then
        worker_status="fail"
        log "Worker agent invocation failed during iteration ${iteration}."
      fi
    else
      set_loop_context "$iteration" skip "$AGENTIC_TASK_FILE"
      : >"$WORKER_OUTPUT_FILE"
      log "Task file ${AGENTIC_TASK_FILE} is empty; skipping Worker."
    fi

    build_status="fail"
    set_loop_context "$iteration" build "$BUILD_LOG_FILE"
    log "Running local build."
    if run_build "$iteration"; then
      build_status="pass"
    else
      log "Local build failed during iteration ${iteration}."
    fi

    set_loop_context "$iteration" review "$REVIEW_FILE"
    log "Running Reviewer-1."
    if ! run_reviewer "$build_status" "$worker_status"; then
      log "Reviewer failed during iteration ${iteration}."
      sleep "$RETRY_DELAY"
      continue
    fi

    set_loop_context "$iteration" orch_review "$ORCHESTRATOR_OUTPUT_FILE"
    log "Running Orchestrator on review results."
    if ! run_orchestrator_review "$build_status" "$worker_status"; then
      log "Orchestrator failed during post-review phase of iteration ${iteration}."
      sleep "$RETRY_DELAY"
      continue
    fi

    set_loop_context "$iteration" load "$AGENTIC_ENV_FILE"
    load_agentic_env
    action="$(lowercase "$AGENTIC_ACTION")"
    case "$action" in
      continue)
        set_loop_context "$iteration" continue "$AGENTIC_ENV_FILE"
        log "Orchestrator requested continue."
        ;;
      commit)
        set_loop_context "$iteration" commit "$AGENTIC_ENV_FILE"
        log "Orchestrator requested commit before remote testing."
        if commit_and_push; then
          test_status="pass"
          set_loop_context "$iteration" remote "$TEST_LOG_FILE"
          log "Running remote validation after commit."
          if ! run_tests; then
            test_status="fail"
          fi
        else
          log "Commit/push failed; remote testing skipped."
          test_status="fail"
          write_test_failure_result "commit or push failed before remote testing"
        fi
        set_loop_context "$iteration" orch_test "$ORCHESTRATOR_OUTPUT_FILE"
        log "Running Orchestrator on test results."
        if ! run_orchestrator_post_test "$test_status"; then
          log "Orchestrator failed during post-test phase after commit action."
        fi
        ;;
      test)
        set_loop_context "$iteration" remote "$TEST_LOG_FILE"
        log "Orchestrator requested remote testing."
        test_status="pass"
        if ! run_tests; then
          test_status="fail"
        fi
        set_loop_context "$iteration" orch_test "$ORCHESTRATOR_OUTPUT_FILE"
        log "Running Orchestrator on test results."
        if ! run_orchestrator_post_test "$test_status"; then
          log "Orchestrator failed during post-test phase after test action."
        fi
        ;;
      stop|halt|ready)
        set_loop_context "$iteration" stop "$AGENTIC_ENV_FILE"
        log "Orchestrator requested stop."
        break
        ;;
      *)
        set_loop_context "$iteration" unknown "$AGENTIC_ENV_FILE"
        log "Unknown AGENTIC_ACTION='${AGENTIC_ACTION}', continuing."
        ;;
    esac

    set_loop_context "$iteration" load "$AGENTIC_ENV_FILE"
    load_agentic_env
    if [[ "$AGENTIC_CODE_READY" == "1" ]]; then
      log "AGENTIC_CODE_READY=1 after orchestrator decision; exiting."
      break
    fi
  done
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --test)
      RUN_MODE="test"
      shift
      ;;
    --reset)
      RESET_SESSIONS=1
      shift
      ;;
    -v|--verbose)
      VERBOSE_CONFIG=1
      shift
      ;;
    -h|--help)
      show_usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      log "Unknown argument: $1"
      show_usage
      exit 1
      ;;
  esac
done

if [[ "$VERBOSE_CONFIG" == "1" ]]; then
  dump_config
fi

init_status_log

if [[ "$RUN_MODE" == "test" ]]; then
  set_loop_context "-" test "$AGENTIC_RESUME_TEST_LOG"
  log "Running isolated bootstrap/resume smoke test."
  run_agent_resume_smoke_test
else
  main_loop
fi
