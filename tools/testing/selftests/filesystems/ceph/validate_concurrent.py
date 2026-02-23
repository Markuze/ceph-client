#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Validator for multi-client CephFS reset stress test.
#
# Two modes:
#   --mode client   Per-client self-validation (run by client_stress.sh)
#   --mode cross    Cross-mount shared file agreement (run manually)

import argparse
import hashlib
import re
import json
from pathlib import Path


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def check_content_integrity(path):
    problems = []
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for line_no, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                if "=" not in line:
                    problems.append(
                        "%s:%d: malformed line: %s" % (path, line_no, line[:80])
                    )
    except Exception as exc:
        problems.append("%s: read error: %s" % (path, exc))
    return problems


def parse_status_file(path):
    status = {}
    if not path.exists():
        return status
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or ":" not in line:
                continue
            key, value = line.split(":", 1)
            status[key.strip()] = value.strip()
    return status


def to_int(value, default=0):
    try:
        return int(value)
    except Exception:
        return default


def validate_namespace(data_dir, file_count, issues):
    for i in range(file_count):
        name = "file_%05d" % i
        in_a = (data_dir / "A" / name).exists()
        in_b = (data_dir / "B" / name).exists()
        if in_a and in_b:
            issues.append(
                "namespace: %s in BOTH A/ and B/ under %s" % (name, data_dir)
            )
        elif not in_a and not in_b:
            issues.append(
                "namespace: %s MISSING from A/ and B/ under %s" % (name, data_dir)
            )


def validate_integrity(data_dir, file_count, issues):
    for i in range(file_count):
        name = "file_%05d" % i
        for subdir in ("A", "B"):
            path = data_dir / subdir / name
            if path.exists():
                problems = check_content_integrity(path)
                issues.extend(problems)


def validate_recovery(log_dir, client_id, issues):
    status_file = log_dir / client_id / "status.final"
    if not status_file.exists():
        issues.append("recovery: status.final missing for client %s" % client_id)
        return

    status = parse_status_file(status_file)

    in_progress = status.get("in_progress", "unknown")
    if in_progress.lower() != "no":
        issues.append("recovery: in_progress=%s, expected no" % in_progress)

    pending = to_int(status.get("pending_reconnects", "0"), default=-1)
    if pending != 0:
        issues.append("recovery: pending_reconnects=%d, expected 0" % pending)

    blocked = to_int(status.get("blocked_requests", "0"), default=-1)
    if blocked != 0:
        issues.append("recovery: blocked_requests=%d, expected 0" % blocked)

    last_errno = to_int(status.get("last_errno", "0"), default=1)
    if last_errno != 0:
        issues.append("recovery: last_errno=%d, expected 0" % last_errno)


def validate_dmesg(log_dir, client_id, issues):
    dmesg_file = log_dir / client_id / "dmesg.log"
    if not dmesg_file.exists():
        return
    try:
        content = dmesg_file.read_text(encoding="utf-8", errors="ignore")
    except Exception:
        return
    if re.search(r"hung task", content, flags=re.IGNORECASE):
        issues.append("dmesg: hung task detected for client %s" % client_id)


def validate_monkey_errors(log_dir, issues):
    for err_file in log_dir.glob("*/errors.log"):
        try:
            content = err_file.read_text(encoding="utf-8").strip()
        except Exception:
            continue
        if content:
            line_count = len(content.splitlines())
            monkey_id = err_file.parent.name
            issues.append(
                "monkey %s: %d corruption error(s)" % (monkey_id, line_count)
            )


def run_client_mode(args):
    root_dir = Path(args.root_dir)
    client_id = args.client_id
    file_count = args.file_count

    private_dir = root_dir / "clients" / client_id
    shared_dir = root_dir / "shared"
    log_dir = root_dir / "logs"

    issues = []

    if private_dir.exists():
        validate_namespace(private_dir, file_count, issues)
        validate_integrity(private_dir, file_count, issues)
    else:
        issues.append("private dir missing: %s" % private_dir)

    if shared_dir.exists():
        validate_namespace(shared_dir, file_count, issues)
        validate_integrity(shared_dir, file_count, issues)
    else:
        issues.append("shared dir missing: %s" % shared_dir)

    validate_recovery(log_dir, client_id, issues)
    validate_dmesg(log_dir, client_id, issues)
    validate_monkey_errors(log_dir, issues)

    return issues


def run_cross_mode(args):
    mount_points = [p.strip() for p in args.mount_points.split(",") if p.strip()]
    if len(mount_points) < 2:
        print("SKIP: need at least 2 mount points for cross-mount mode")
        return []

    issues = []

    first_mount = Path(mount_points[0])
    shared_dir = first_mount / "shared"

    if not shared_dir.exists():
        issues.append("shared dir not found at %s" % shared_dir)
        return issues

    files = sorted(
        list((shared_dir / "A").glob("file_*"))
        + list((shared_dir / "B").glob("file_*"))
    )

    if not files:
        issues.append("no shared files found for cross-mount comparison")
        return issues

    for f in files:
        rel = f.relative_to(first_mount)
        hashes = {}

        for mp_str in mount_points:
            mp = Path(mp_str)
            candidate = mp / rel
            if not candidate.exists():
                issues.append("cross-mount: %s missing from %s" % (rel, mp_str))
                continue
            hashes[mp_str] = sha256_file(candidate)

        unique_hashes = set(hashes.values())
        if len(unique_hashes) > 1:
            detail = ", ".join(
                "%s=%s" % (mp, h[:16]) for mp, h in hashes.items()
            )
            issues.append("cross-mount: %s hash mismatch: %s" % (rel, detail))

    return issues


def main():
    parser = argparse.ArgumentParser(
        description="Validate multi-client CephFS reset stress test"
    )
    parser.add_argument("--mode", required=True, choices=["client", "cross"])
    parser.add_argument("--root-dir", required=True)
    parser.add_argument("--client-id", required=False, default="")
    parser.add_argument("--file-count", required=False, type=int, default=64)
    parser.add_argument("--mount-points", required=False, default="")
    parser.add_argument("--report-json", required=False, default="")
    args = parser.parse_args()

    if args.mode == "client":
        if not args.client_id:
            parser.error("--client-id required for client mode")
        issues = run_client_mode(args)
    elif args.mode == "cross":
        if not args.mount_points:
            parser.error("--mount-points required for cross mode")
        issues = run_cross_mode(args)
    else:
        issues = ["unknown mode: %s" % args.mode]

    report = {"mode": args.mode, "issues": issues}

    if args.report_json:
        Path(args.report_json).write_text(
            json.dumps(report, indent=2, sort_keys=True), encoding="utf-8"
        )

    if issues:
        print("FAIL: %d issue(s)" % len(issues))
        for issue in issues:
            print("  - %s" % issue)
        raise SystemExit(1)

    print("PASS")


if __name__ == "__main__":
    main()
