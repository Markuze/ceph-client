# CephFS Client Reset Test Suite -- HOWTO

## Prerequisites

All tests require:

- Linux kernel with the CephFS client reset feature (this branch)
- A running Ceph cluster with at least one MDS
- Root access (debugfs requires it)
- Python 3 (for validators)
- flock utility (for lock tests, usually in util-linux)

Multi-client tests additionally require multiple CephFS mount instances
(on the same node or across nodes).

---

## Test 1: Single-Client Stress (reset_stress.sh)

Baseline. Runs I/O and rename workloads on a single mount while
triggering periodic resets.

### Quick start

    sudo ./reset_stress.sh --mount-point /mnt/cephfs --profile moderate

### Profiles

    baseline   - no resets, 1 IO + 1 rename, 600s
    moderate   - reset every 5-15s, 2 IO + 1 rename, 900s
    aggressive - reset every 1-5s, 4 IO + 2 rename, 900s
    soak       - reset every 5-15s, 2 IO + 1 rename, 3600s

### Key options

    --mount-point PATH   CephFS mount point (required)
    --profile NAME       baseline|moderate|aggressive|soak
    --duration-sec N     Override profile runtime
    --no-reset           Disable reset injection
    --client-id ID       Debugfs client id (auto-detected if one)
    --out-dir PATH       Artifact directory

---

## Test 2: Corner Cases (reset_corner_cases.sh)

Five targeted tests for specific reset code paths.

### Quick start

    sudo ./reset_corner_cases.sh --mount-point /mnt/cephfs

### Test checklist

    [1/5] inject_error          inject_error flag, error propagation + recovery
    [2/5] ebusy_rejection       Second reset rejected while first in-flight
    [3/5] dirty_caps_at_reset   Reset with unflushed dirty caps
    [4/5] flock_after_reset     Stale lock EIO + fresh lock after holder exit
    [5/5] unmount_during_reset  umount during active reset (ESHUTDOWN path)

Test 5 SKIPs if it cannot create a separate mount instance.

### Options

    --mount-point PATH   CephFS mount point (required)
    --client-id ID       Debugfs client id (auto-detected if one)

---

## Test 3: Multi-Client Stress

Three scripts, run independently. Coordination through files on CephFS.
No coordinator, no SSH.

### Roles

- **Client** (client_stress.sh) -- does private + shared I/O, resets itself
- **Monkey** (cap_monkey.sh) -- hammers shared files for cap revocation
- **Validator** (validate_concurrent.py) -- checks consistency

### Setup: two mounts on one machine

    sudo mount -t ceph mon1:/ /mnt/cephfs1 -o name=admin,secret=KEY
    sudo mount -t ceph mon1:/ /mnt/cephfs2 -o name=admin,secret=KEY

### Running (2 clients + 1 monkey)

Pick a shared run ID (same for all participants):

    RUN=test_001

**Terminal 1** -- Client c1 (originator creates the tree):

    sudo ./client_stress.sh \
        --mount-point /mnt/cephfs1 \
        --run-id $RUN --client-id c1 --originator \
        --duration-sec 600

**Terminal 2** -- Client c2 (joins after originator):

    sudo ./client_stress.sh \
        --mount-point /mnt/cephfs2 \
        --run-id $RUN --client-id c2 \
        --duration-sec 600

**Terminal 3** -- Monkey m1 (waits for clients, creates chaos):

    sudo ./cap_monkey.sh \
        --mount-point /mnt/cephfs1 \
        --run-id $RUN --monkey-id m1 \
        --min-clients 2 --duration-sec 500

### Sequence of events

1. c1 (originator) creates directory tree and seeds files
2. c2 waits for c1 sentinel, seeds its own private zone
3. Both advertise readiness in coord/
4. Monkey waits for 2 clients, hammers shared/ with writes, renames, flocks
5. Both clients do I/O on private + shared zones, resetting periodically
6. Monkey finishes, writes coord/monkey_m1_done.json
7. Clients detect monkey done, stop shared I/O and resets
8. Clients collect files back, self-validate

### Cross-mount validation (after all finish)

    python3 ./validate_concurrent.py \
        --mode cross \
        --root-dir /mnt/cephfs1/reset_test_$RUN \
        --mount-points /mnt/cephfs1/reset_test_$RUN,/mnt/cephfs2/reset_test_$RUN

### Client options

    --mount-point PATH         CephFS mount point (required)
    --run-id ID                Shared run identifier (required)
    --client-id ID             Unique client name (required)
    --originator               This client creates the tree (exactly one)
    --file-count N             Files per zone (default: 64)
    --duration-sec N           Workload runtime (default: 900)
    --reset-interval-min N     Min seconds between resets (default: 5)
    --reset-interval-max N     Max seconds between resets (default: 15)
    --io-workers N             I/O workers per zone (default: 2)
    --rename-workers N         Rename workers per zone (default: 1)
    --min-monkeys N            Wait for N monkeys to finish (default: 1)

### Monkey options

    --mount-point PATH         CephFS mount point (required)
    --run-id ID                Same run ID as clients (required)
    --monkey-id ID             Unique monkey name (required)
    --duration-sec N           How long to stress (default: 600)
    --io-workers N             I/O workers (default: 4)
    --rename-workers N         Rename workers (default: 2)
    --flock-workers N          Flock contention workers (default: 1)
    --min-clients N            Wait for N clients before starting (default: 2)

### What it checks

Per-client (automatic):

- Private zone: namespace invariant, content integrity
- Shared zone: namespace invariant, content integrity
- Recovery: reset/status is clean
- dmesg: no hung tasks
- Monkey error logs: no corruption detected during run

Cross-mount (manual):

- Every shared file has identical SHA-256 from each mount point

---

## Filesystem layout

Single-client (reset_stress.sh):

    mount/ceph_reset_stress_TIMESTAMP/
        A/file_00000..N
        B/

Multi-client (client_stress.sh + cap_monkey.sh):

    mount/reset_test_RUN_ID/
        coord/
            root_ready
            client_c1_ready.json
            monkey_m1_done.json
        clients/
            c1/A/ c1/B/
            c2/A/ c2/B/
        shared/
            A/file_00000..N
            B/
        logs/
            c1/ c2/ m1/

---

## Troubleshooting

**No writable Ceph reset interface found:**
Kernel lacks the reset feature, debugfs not mounted, or not root.
Check: ls /sys/kernel/debug/ceph/*/reset/

**Multiple Ceph clients found:**
Use --client-id or --debugfs-client to select one.
List: ls /sys/kernel/debug/ceph/

**Client hangs waiting for monkeys:**
Start a monkey, or Ctrl-C the client.

**Cross-mount hash mismatch:**
One client sees stale cached data. Likely a cache coherence bug
in the reset reconnect path. Check dmesg on both nodes.

**Test 5 unmount_during_reset SKIP:**
Cannot create second mount. Check mount.ceph is installed.
