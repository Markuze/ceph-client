# CephFS Client Reset Test Suite

Test suite for the CephFS kernel client manual session reset feature.
Three test tiers, each independent.

See `HOWTO.md` for full usage instructions, examples, and troubleshooting.

## Test inventory

| Test | Script(s) | What it covers | Requirements |
|------|-----------|----------------|--------------|
| Single-client stress | `reset_stress.sh` | I/O + resets + data integrity on one mount | 1 CephFS mount |
| Corner cases | `reset_corner_cases.sh` | inject_error, EBUSY, dirty caps, flock reclaim, unmount-during-reset | 1 CephFS mount |
| Multi-client stress | `client_stress.sh` + `cap_monkey.sh` | Cap revocation, cross-client cache coherence, lock contention under reset | 2+ CephFS mounts |

## Quick start

Single-client baseline (one terminal):

    sudo ./reset_stress.sh --mount-point /mnt/cephfs --profile moderate

Corner cases (one terminal):

    sudo ./reset_corner_cases.sh --mount-point /mnt/cephfs

Multi-client (three terminals, same run ID):

    # Terminal 1: originator client
    sudo ./client_stress.sh --mount-point /mnt/cephfs1 \
        --run-id run1 --client-id c1 --originator --duration-sec 600

    # Terminal 2: second client
    sudo ./client_stress.sh --mount-point /mnt/cephfs2 \
        --run-id run1 --client-id c2 --duration-sec 600

    # Terminal 3: chaos monkey
    sudo ./cap_monkey.sh --mount-point /mnt/cephfs1 \
        --run-id run1 --monkey-id m1 --min-clients 2 --duration-sec 500

## Files

| File | Role |
|------|------|
| `reset_stress.sh` | Single-client stress test runner |
| `validate_consistency.py` | Single-client post-run validator |
| `reset_corner_cases.sh` | Corner case harness (5 sequential tests) |
| `client_stress.sh` | Multi-client: per-client actor |
| `cap_monkey.sh` | Multi-client: shared-file chaos agent |
| `validate_concurrent.py` | Multi-client: per-client + cross-mount validator |
| `HOWTO.md` | Detailed usage guide |
