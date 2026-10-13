# Directory map

Where things live on this machine, and who owns them. Checked on 2026-10-13. Update this file when a directory appears, moves or changes owner.

**Machine:** 32 CPUs, 125 GiB RAM, host kernel `7.0.0-1012-ibm`, passwordless `sudo`. There is no QEMU, so test kernels run under UML. Never boot or install a test kernel on the host.

## /mnt/workspace (1 TB work disk)

| Path | Owner | What it is | Rules |
|---|---|---|---|
| `AGENTS.md` | owner | The older generic agent guide, a copy of `~/AGENTS_GENERIC.md`. opencode doesn't load it inside a repository, because its search stops at the git root. | Reference only. |
| `KERNEL_AGENTS.md` | owner | The generic kernel-agent guide that `ceph-client/opencode.json` loads. **Not on this machine yet**; the original is on the owner's Mac. | The owner copies it here. |
| `ceph-client/` | OPAQUE_OBJ project | The Linux tree for the OPAQUE_OBJ series, checked out on `osdi/opaque-obj`. Its `utils/` is a separate git repository for non-upstream tools, excluded from the tree. | Read its `AGENTS.md` first. |
| `current/` | all agents | Task ledgers, `<zellij session or task>.md`. `fuse.md` is the status board for zero-copy ceph-fuse; its workers add `fuse-<wp>.md`. | Ledgers stay outside every repository. |
| `fuse-zc/` | zero-copy ceph-fuse | The package starter `bin/fzc-start`, and `facts`, a symlink to `ceph-client/fuse_zc_facts/`. Worktrees and shared clones appear here as packages start (below). | Each worker uses only its own worktree and `build/<wp>/`. |
| `opaque-vm/` | OPAQUE_OBJ reviews and GLM rounds | UML kernel builds, root filesystems and logs (19 GB): `build-*` per task or commit, `root`, `patches`, `v9`, `v9r2`. | Keep; prune only when the owner says so. Never reuse another task's build directory. |
| `rust-mds/` | rust-mds | The Rust MDS project (`github.com/Markuze/rust-mds`, 17 GB), with its own `AGENTS.md`. | Don't modify. |
| `pjdfstest/` | rust-mds | A clone of `github.com/pjd/pjdfstest`, the POSIX filesystem test suite. rust-mds's ledger uses it. | Don't modify. |
| `devcluster-data/` | rust-mds | State of the rust-mds single-node Ceph cluster (packaged Ceph 20.2.0), created by `rust-mds/devcluster/start.sh` on 2026-10-11. | Don't use it for FUSE work; E1 runs its own cluster. |
| `lost+found/` | filesystem | ext4 recovery directory. | Leave it. |

## /mnt/workspace/fuse-zc

Most of these appear only when `fzc-start <WP>` starts a package (see `fuse_zc_workplan.md` §0 and §4).

| Path | Content |
|---|---|
| `bin/fzc-start` | Symlink to `ceph-client/utils/fuse-zc/fzc-start`, which the `utils/` repository versions. It creates a package's worktree and branch, writes its brief there as `AGENTS.md`, and prints the prompt for the agent. `--prompt`, `--print-brief` and `--dry-run` preview it without side effects. |
| `prompts/<wp>.txt` | The prompt for each package that has been started. |
| `facts` | Symlink to `/mnt/workspace/ceph-client/fuse_zc_facts/`. It dangles if `ceph-client` leaves `osdi/opaque-obj`. |
| `linux-<wp>/` | Kernel worktrees of `ceph-client` for K1–K4, on branches `fuse-zc/<…>`. |
| `libfuse/`, `libfuse-<wp>/` | The shared libfuse clone and the L1–L6 worktrees. |
| `ceph/`, `ceph-<wp>/` | The shared Ceph clone and the C1–C6 worktrees. FUSE work never uses `~/ceph`. |
| `liburing/` | liburing clone for K4. |
| `env/`, `t1/`, `b1/` | Script repositories for E1, T1 and B1. |
| `build/<wp>/` | Build output, UML roots and logs of one package. |
| `kheaders/`, `prefix/` | The integration kernel's UAPI headers and the installed patched libfuse (E1). |

## ceph-client branches

| Branch | What it is |
|---|---|
| `osdi/opaque-obj` | **The checkout.** The OSDI project branch: the five v9 series commits, then `aux:` commits holding every result. `git rev-list -1 --invert-grep --grep='^aux:' HEAD` prints the series tip. |
| `work/opaque-obj-v9` | The series plus the earlier single notes commit; pushed as `origin/work/opaque-obj-v9`. Don't rewrite it. |
| `opaque-obj-v9-round2` | v9 as posted (`origin/upstream/opaque-obj-v9`). |
| `opaque-obj-v9-round1`, `opaque-obj-v8-capture`, `opaque-obj-v7.1` | Local intermediate versions. v8 was never pushed; v7.1 is a reference implementation of the capture policy. |
| `opaqu_objects` | The kpass prototype, with the v1, v4 and v6 reviews (also on `origin`). |
| `master` | An upstream master snapshot. |
| `origin/upstream/opaque-obj*` | The posted versions: v1, v3–v7 and v9. |
| `origin/work/opaque-obj-v5` … `-v7` | The author's working branches, with `errata.md` and `optimizations.md`. |

The auxiliary files on `osdi/opaque-obj` are listed in the "Directory map" section of `AGENTS.md`.

## Outside /mnt/workspace

| Path | What it is |
|---|---|
| `~/ceph` | The rust-mds project's Ceph checkout (branch `wip-mds-rust-map`). Don't use it for FUSE work. |
| `~/.claude/projects/-mnt-workspace-ceph-client/memory/` | Claude Code's memory for this tree. |
| `/tmp/claude-1000/<project>/<session>/scratchpad/` | Per-session Claude scratch space. Temporary; durable notes must not point into it. |
| `~/.opencode/bin/opencode` (2.0.28), `~/.local/bin/codex` (0.162.0), `/usr/bin/crush` (0.98.1) | Agent command-line tools. `~/.config/opencode/` holds opencode's global configuration. |
| `/usr/bin/ceph*` | Packaged Ceph 20.2.0, used by the rust-mds cluster. |
