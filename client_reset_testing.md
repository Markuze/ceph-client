# CephFS Client Reset — Test Plan & Expected Output

## Quick start

```bash
sudo ./tools/testing/selftests/filesystems/ceph/run_validation.sh \
    --mount-point /mnt/mycephfs
```

Runtime: ~6 minutes. Exit code 0 = all pass.

## Test stages and feature mapping

### Stage 1: Baseline (no resets)

**Command:** `reset_stress.sh --profile baseline --no-reset --duration-sec 60`

**What it validates:**
- CephFS mount is functional (read/write/rename)
- Test harness works (io_worker, rename_worker, validator)
- No pre-existing filesystem corruption

**Feature code exercised:** None — this is the control.

**Expected output:**
```
PASS: consistency validation succeeded
```

**If it fails:** The problem is the mount or the test harness, not the
reset feature. Fix the environment before proceeding.

### Stage 2: Corner cases

**Command:** `reset_corner_cases.sh --mount-point /mnt/mycephfs`

**What it validates:**

| Test | Feature code path | What it proves |
|------|-------------------|----------------|
| `inject_error` | `inject_error` flag in `reset_state`, error propagation via `last_errno` | Fault injection works, errors propagate to blocked callers, recovery after error |
| `ebusy_rejection` | `phase != IDLE` check in `ceph_mdsc_schedule_reset()` | Overlapping reset triggers are rejected with EBUSY |
| `dirty_caps_at_reset` | Drain phase: `ceph_flush_dirty_caps()` + bounded wait | Non-stuck dirty caps are flushed during drain before teardown |
| `flock_after_reset` | `remove_session_caps()` → lock loss, `CEPH_I_ERROR_FILELOCK` | Locks ARE lost after teardown (not reclaimed), can be re-acquired |
| `unmount_during_reset` | Destroy path: `phase → IDLE`, `last_errno = -ESHUTDOWN`, `cancel_work_sync` | Clean shutdown during active reset, no hangs |

**Expected output:**
```
[1/5] inject_error                    PASS
[2/5] ebusy_rejection                 PASS
[3/5] dirty_caps_at_reset             PASS  (file intact, N lines)
[4/5] flock_after_reset               PASS  (lock lost and re-acquirable after reset)
[5/5] unmount_during_reset            PASS

5/5 passed, 0 failed, 0 skipped
```

**If `flock_after_reset` fails with "lock was NOT released":** The
teardown did not remove caps/locks — check that the v2 teardown path
(`__unregister_session` + `remove_session_caps`) actually ran.

**If `dirty_caps_at_reset` fails with "file is empty":** The drain
phase didn't flush and the teardown dropped everything — check that
`ceph_flush_dirty_caps` is being called and sessions are alive during
drain.

### Stage 3: Moderate stress

**Command:** `reset_stress.sh --profile moderate --duration-sec 120`

**What it validates:**
- Session teardown under concurrent I/O (2 workers) and renames (1 worker)
- Resets every 5-15s — exercises the full SM lifecycle repeatedly:
  `IDLE → QUIESCING → DRAINING → TEARDOWN → IDLE`
- Data integrity after multiple reset cycles
- Recovery SLO: workload resumes within 30s of each reset

**Feature code exercised:**
- `ceph_mdsc_schedule_reset()` — trigger acceptance
- `ceph_mdsc_reset_workfn()` — drain + teardown loop
- `ceph_mdsc_wait_for_reset()` — request blocking during reset
- `send_flush_mdlog()` — MDS journal flush
- `ceph_flush_dirty_caps()` — dirty cap flush
- Race guard (`mdsc->sessions[mds] != sessions[i]`)
- `kick_requests()` — re-dispatch after teardown

**Expected dmesg pattern:**
```
manual session reset scheduled (reason="stress_0_...")
manual session reset executing (sessions=1, reason="stress_0_...")
draining (want_flush=..., 1 sessions)
drain completed successfully
mds0 resetting session
mds0 session reset complete
```

**Bad dmesg signs:**
- `reconnect start` / `reconnect denied` — old v1 path, should not appear
- `hung task` — deadlock in teardown or drain
- `BUG:` / `WARNING:` — kernel assertion failure
- `session already torn down, skipping` — not fatal but frequent
  occurrences mean heavy racing with `mds_peer_reset`

**Expected validator output:**
```
PASS: consistency validation succeeded
```

**What the validator checks:**
- Namespace invariant: each file exists exactly once in A/ or B/
- Rename invariant: final location matches last logged rename
- Data invariant: file hash matches last logged write
- Recovery: `phase=idle`, `pending_reconnects=0`, `last_errno=0`
- Recovery SLO: first successful I/O within 30s of each reset
- No hung tasks in dmesg

### Stage 4: Aggressive stress

**Command:** `reset_stress.sh --profile aggressive --duration-sec 120`

**What it validates:**
- Same as stage 3 but under extreme pressure:
  4 I/O workers, 2 rename workers, resets every 1-5s
- Race safety in the teardown loop (concurrent resets overlapping
  with session operations)
- Drain timeout behavior (resets fire faster than drain can complete)

**Expected differences from moderate:**
- `drain_timed_out: yes` is likely — resets fire before caps finish flushing
- More `dropping unsafe request` / `dropping dirty+flushing` in dmesg
- Higher chance of `session already torn down, skipping` in dmesg

**Expected output:** Same `PASS` from the validator. Data integrity
must hold even under aggressive resets.

### Stage 5: Status check

**What it validates:** Final debugfs state after all tests complete.

**Expected `reset/status` output:**
```
phase: idle
trigger_count: <N>
success_count: <N>
failure_count: 0
last_start_ms_ago: <N>
last_finish_ms_ago: <N>
last_errno: 0
last_reason: stress_<N>_<timestamp>
inject_error_pending: no
drain_timed_out: yes|no
sessions_reset: 1
pending_reconnects: 0
blocked_requests: 0
```

**Key fields:**
- `phase: idle` — no reset stuck in progress
- `last_errno: 0` — last reset succeeded
- `pending_reconnects: 0` — always 0 for v2 manual reset
- `blocked_requests: 0` — no requests stuck waiting
- `drain_timed_out` — either value is acceptable

## Timeout behavior

Each stage runs under a watchdog. If a stage hangs:

```
[stage 3/5] moderate         FAIL  (HUNG: killed after 240s)
```

| Stage | Timeout | Rationale |
|-------|---------|-----------|
| baseline | 120s | 60s run + 20s cooldown + buffer |
| corner_cases | 300s | 5 tests × 30s worst case each |
| moderate | 240s | 120s run + 20s cooldown + buffer |
| aggressive | 240s | 120s run + 20s cooldown + buffer |
| status_check | 10s | debugfs read only |

A HUNG result means the filesystem is stuck — likely the exact
scenario the reset feature is designed to recover from. Check dmesg
for hung task warnings or cap stalemate indicators.

## Artifacts

All output goes to `/tmp/ceph_reset_validation_<timestamp>/`:

```
stage1_baseline/          # stress test artifacts (io.log, rename.log, etc.)
stage1_baseline.log       # stage stdout/stderr
stage2_corner_cases/
  output.log              # corner case test output
stage3_moderate/          # stress test artifacts
stage3_moderate.log
stage4_aggressive/        # stress test artifacts
stage4_aggressive.log
final_status.txt          # debugfs reset/status snapshot
```

On failure, the `.log` file for the failed stage contains the details.
For HUNG failures, the log ends with `TIMEOUT: stage exceeded Ns`.

## State machine lifecycle coverage

The test stages together exercise every SM transition:

```
IDLE → QUIESCING    ceph_mdsc_schedule_reset()     stages 2,3,4
QUIESCING → DRAINING    workfn entry               stages 2,3,4
DRAINING → TEARDOWN     after drain completes/times out    stages 2,3,4
TEARDOWN → IDLE         ceph_mdsc_reset_complete()  stages 2,3,4
any → IDLE (ESHUTDOWN)  ceph_mdsc_destroy()         stage 2 (unmount_during_reset)
IDLE → IDLE (EBUSY)     schedule_reset overlap      stage 2 (ebusy_rejection)
IDLE → IDLE (EIO)       inject_error                stage 2 (inject_error)
```
