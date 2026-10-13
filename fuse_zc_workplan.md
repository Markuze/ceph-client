# ceph-fuse zero-copy READ: work plan for worker agents

**Status, 2026-10-13: design only.** Nothing is implemented in any repository: no kernel branch, no libfuse branch, no Ceph branch. This plan turns the design into work packages that workers can run in parallel.

**Baseline: OPAQUE_OBJ v9.** `origin/upstream/opaque-obj-v9` at `875cf36709ca`, five patches on `af32da41b032` (7.3-rc7 merge). That base also contains FUSE io_uring zero-copy (`43f8343858eb`). The design in `fuse_zc.md` was written against v4; §1 lists what v9 changes. **Where this plan and `fuse_zc.md` differ, this plan wins.**

**Scope: reads only (D8).** Zero-copy opens are O_RDONLY. Writes keep today's copying path. Zero-copy writes are a separate project in which FUSE itself uses opaque objects.

**How to brief a worker.** Give them §1–§5 and their own work package from §6. Each package names its inputs, interfaces, files, steps, tests and acceptance criteria. §0 tells the dispatcher what can run in parallel.

---

## 0. For the dispatcher

| WP | Work | Project | Depends on | Size |
|---|---|---|---|---|
| K1 | `IORING_OP_OPAQUE_OBJ_FILL` with `LAST` | Linux (opaque add-on) | — | M |
| K2 | crc32c in RECV_OBJECT | Linux (opaque add-on) | — (rebase onto K1) | S |
| K3 | Store lock scalability | Linux | B1 numbers; proposal only | — |
| K4 | `IORING_OP_FIXED_COPY` | Linux (io_uring) + liburing | — | S–M |
| L1 | Ring mode, reply hand-off, hooks, CQE tags | libfuse | — | M |
| L2 | Zero-copy queues and slots | libfuse | — (rebase onto L1) | M |
| L3 | `fuse_reply_uring_ops` | libfuse | L1, L2 | M |
| L4 | Worker pool for ring requests | libfuse | L1 | S–M |
| L5 | Interrupts for ring requests | libfuse | — | S–M |
| L6 | Example, tests, docs | libfuse | L1–L5 | S |
| C1 | `buffer::raw_opaque` | Ceph | — | M |
| C2 | `OpaqueStore`, messenger rings | Ceph | C1 | M–L |
| C3 | Messenger receive engine | Ceph | C2 (K2 optional) | L |
| C4 | Objecter opt-in, admission, SplitOp | Ceph | C2 | M |
| C5 | ObjectCacher and Client | Ceph | C1, C4 (fakes before C3) | L |
| C6 | ceph-fuse integration | Ceph | C2–C5, L1–L3 | M |
| E1 | UML integration environment | scripts | — | M |
| T1 | Integration tests | all | C6, E1, K1, K2 | M |
| B1 | Benchmarks and tuning | all | T1, hardware (O8) | M |

**Plan map.** `══` marks the critical path. Every line ends at the packages that consume it.

```
KERNEL   K1 FILL ──────────────────────────────▶ C6, T1
         K2 crc ───────────────────────────────▶ C3   (C3 has a fallback until K2 lands)
         K4 FIXED_COPY ────────────────────────▶ C6   (memfd fallback without it)

LIBFUSE  L1 ring mode ──┬──▶ L3 reply ops ─────▶ C6
         L2 ZC queues ──┘
         L1 ──────────────▶ L4 workers ────────▶ C6
         L5 interrupts ────────────────────────▶ L6 example and tests (after L1–L5), T1

CEPH     C1 ══▶ C2 ══▶ C3 ══▶ C5 ══▶ C6 ══▶ T1 ══▶ B1 ──▶ K3 (only if B1 shows contention)
                 └──▶ C4 ──▶┘

ENV      E1 UML environment ───────────────────▶ T1, and test kernels for C2, C3 and libfuse

Wave 1 (now): K1 K2 K4 L1 L2 L5 C1 E1    Wave 2: C2 L3 L4    Wave 3: C3 C4 L6 (C5 on fakes)
Wave 4: C5 → C6                          Wave 5: T1           Wave 6: B1 → K3
```

**Starting a worker.** `/mnt/workspace/fuse-zc/bin/fzc-start <WP>` creates the package's worktree and branch, writes its brief there as `AGENTS.md` (Codex and opencode load it), and prints the prompt to give the agent, also saved as `/mnt/workspace/fuse-zc/prompts/<wp>.txt`. `fzc-start <WP> --prompt` prints only the prompt. Start the agent in that worktree with that prompt. The status board is `/mnt/workspace/current/fuse.md`.

**Waves.**
1. **Now, in parallel (8 workers):** K1, K2, K4, L1, L2, L5, C1, E1.
2. C2 (after C1), L3 (after L1 and L2), L4 (after L1).
3. C3 and C4 (after C2), L6, and C5 against fakes.
4. C5 completion, then C6.
5. T1.
6. B1, then K3 if B1 shows contention.

**Critical path:** C1 → C2 → C3 → C5 → C6 → T1. Kernel and libfuse work finish well before C6 needs them.

**Integration owner.** The E1 worker also maintains the `fuse-zc/kernel-int` branch, and installs the libfuse branches into `/mnt/workspace/fuse-zc/prefix` as they land.

---

## 1. What v9 changes

v9 adds a capture policy, memory-pressure modes and fixes; it adds no operation that this design needs. FILL and crc are still missing.

| v9 change | Effect on this design | Work package |
|---|---|---|
| **Capture hold.** While a stream holds undecided bytes, nothing more is collected for it; new bytes stay on the socket under TCP flow control. A waiting INSPECT past the collected bytes, or a decision further ahead, reopens collection. Decided bytes are always collected. | The per-stream receive reserve (`fuse_zc.md` §9.1.3) is **dropped**: the kernel now bounds each stream's undecided bytes. The messenger must decide header bytes promptly. | K-series, C3 |
| **INSPECT returns only collected bytes.** Re-inspecting the same offset never returns more; an INSPECT at the end offset waits and pulls more. | The framing loop always inspects the next missing offset, never the same offset again. | C3 |
| **Pressure CQE.** A backing or extent stall posts a nonterminal collector CQE: `res = -ENOBUFS`, `IORING_CQE_F_MORE`, extra1 = stream token, extra2 = first unread TCP offset. It is coalesced, and collection resumes by itself after a release. | Pressure drives ObjectCacher trimming. No STAT polling loop is needed for that. | C2, C3, C5 |
| **CQ full while reporting pressure** ends the collector with `-ENOSPC`. There is no re-arm on that token. | Size completion queues. A terminal `-ENOSPC` faults the connection and closes the stream. | C2, C3 |
| **Watermarks and LOWMEM.** Objects admitted while the store is in LOWMEM are flagged `IORING_OPAQUE_OBJECT_F_FORWARD_ONLY` in the RECV_OBJECT CQE (extra2) and in OBJECT_STAT. The flag is advisory: "forward and release promptly". A nonzero high watermark requires a nonzero `reply_reserve`. | FORWARD_ONLY objects are delivered once and never staged; readahead stops opting in while the store is in LOWMEM. The crc can no longer use extra2 as planned (K2 packs it into the high 32 bits). | K2, C2, C4, C5 |
| **Memory accounting.** Registration reserves `hard_limit` against the registration owner's locked-memory accounting. Retained RX allocations are charged to the owner's memcg. | ceph-fuse needs `RLIMIT_MEMLOCK` of at least the store limit or `CAP_IPC_LOCK`, and memcg headroom. | C2, C6 |
| **Review items N1, N2 and R are closed** (framing quota, `connect(AF_UNSPEC)`, cross-ring SEND cancel). | Not used by reads. They unblock the separate write project. | — |
| **Unchanged:** INSPECT and READ_OBJECT copy at most 64 KiB, `buf_index` is rejected, there is no crc, the store-wide `tables` mutex remains (the author deferred it). | FILL (K1) and crc (K2) are still required. Lookup scalability is measured first (K3). | K1–K3 |
| **Copy fallback for cloned or shared skbs.** Loopback TCP delivers shared skbs. | Loopback and UML runs show a kernel copy on capture. Zero-copy capture must be measured on a real NIC. | E1, B1 |
| **All series validation ran under UML with loopback TCP.** | The first run on a real NIC must repeat the series' KUnit and selftests. | B1 |

---

## 2. End-to-end flow (summary)

The full narrative is `fuse_zc.md` §1. In short:

1. **Setup.** ceph-fuse creates one opaque store and opens `/dev/zero` once. Every messenger worker and every libfuse queue thread creates its ring on its own thread and imports the store. Eligible O_RDONLY opens are marked zero-copy.
2. **Connect.** After the msgr2 handshake, the messenger attaches each OSD socket with `OPAQUE_COLLECT`. Receive belongs to the kernel; transmit stays on `sendmsg`.
3. **Read.** FUSE registers the READ destination folios at the request's slot. A ceph-fuse worker runs `Client::ll_read_zc()`. An ObjectCacher hit returns staged objects. A miss sends an admitted, opted-in OSD read.
4. **Receive.** The messenger INSPECTs headers and RECV_OBJECTs the READ payload as file-aligned chunk objects, each with a kernel crc. It verifies the frame crc and `late_status`, then DISCARDs the protocol bytes.
5. **Deliver.** The Client returns pieces (OBJECT, ZERO, MEM). On its own ring, the libfuse queue thread fills the slot with `OPAQUE_OBJ_FILL` (with `LAST` when this is the final reference), `READ_FIXED` from `/dev/zero`, or `FIXED_COPY`, and commits after every fill has completed. The kernel's copy into the page cache is the only copy.
6. **Release.** Objects consumed by `LAST` are retired by the kernel. Undelivered objects are freed by their buffer's destructor. Teardown closes every stream.

Every failure falls back to today's copying path for that op, frame, connection or file.

---

## 3. Rules for every worker

1. **Implement only your work package.** No refactoring, renames or features outside it.
2. **The interfaces in §5 are contracts.** Code against them exactly. If one is wrong or impossible, stop and ask; don't change it on your own.
3. **If an instruction is wrong, ambiguous or impossible, stop and ask.** Don't guess. List anything else that looks stale or wrong in your report, but don't fix it.
4. **Trailers.** Don't add `Signed-off-by:`; the owner adds it before posting. Add an `Assisted-by:` trailer naming your agent and model, in the series' format (`Assisted-by: Codex:GPT-6`, `Assisted-by: GLM:glm-5.3`; Linux `Documentation/process/coding-assistants.rst`). Use the same trailer in libfuse and Ceph.
5. **Follow each project's upstream style.**
   - Linux: `scripts/checkpatch.pl --strict` clean apart from the missing `Signed-off-by`, no new warnings with `W=1` in the directories you touch, commit message lines of at most 75 columns, subject prefix of the subsystem (`io_uring:`, `io_uring/opaque:`).
   - libfuse: its `checkpatch.pl`, meson build with `-Dwerror=true`, existing naming and symbol-versioning rules.
   - Ceph: subject prefix of the component (`msg/async:`, `common/buffer:`, `osdc:`, `client:`), unit tests under `src/test/`, no new compiler warnings.
6. **Never suppress a diagnostic.** Quote every `BUG:`, `WARNING:`, KASAN, UBSAN, lockdep, `refcount_t:` or kmemleak report, and every ASan/TSan report in userspace, attributed from its backtrace. UML kmemleak boot noise (`ssl_init`, `parse_chan`) and the UML SMP boot RCU splat from `os_start_secondary` are known noise; say so when you see them.
7. **Don't claim anything you didn't run.** Report exact commands and results.
8. **Don't touch other work on this machine.**
   - The checked-out worktree at `/mnt/workspace/ceph-client` (other sessions use it).
   - `~/ceph`, which another project uses (`wip-mds-rust-map`). Work in the shared clone instead (§4).
   - `/mnt/workspace/opaque-vm` and `/mnt/workspace/devcluster-data`.
   - Use your own worktree under `/mnt/workspace/fuse-zc/` (§4).
9. **Commit locally on your branch, one commit per patch in your deliverable list,** unless your tool's rules forbid commits (opencode's `implementer` role does). In that case leave the work uncommitted and say so; the owner commits after review. **Never push.**
10. **Report back with:** branch and commits, files changed, every test command with its result, deviations from this plan, and open questions.

---

## 4. Repositories, branches and environment

**Machine.** 32 CPUs, 125 GiB RAM, about 900 GiB free on `/mnt/workspace`, passwordless `sudo`, GitHub over HTTPS works. There is **no QEMU**; the host kernel is 7.0 without the series. Every kernel-side test therefore runs under **UML**. Installed: liburing 2.14 (`liburing-dev`), fuse3 3.18.2 runtime (no headers), Ceph 20.2.0 packages in `/usr/bin` (not used by this plan).

**Owner prerequisites (sudo, once).** Workers never use sudo, so the owner installs these before the packages that need them:
- libfuse (L*): `sudo apt install meson`. meson is not installed; ninja and liburing 2.14 are.
- Ceph (C*): run `sudo ./install-deps.sh` in the shared Ceph clone, and `sudo apt install ccache`. Almost no Ceph build dependencies are installed.
- E1 and T1: `sudo ip tuntap add dev fzc0 mode tap user ubuntu && sudo ip addr add 10.77.0.1/24 dev fzc0 && sudo ip link set fzc0 up`.

**Worktrees.** `fzc-start <WP>` (§0) creates them as described below; a worker that starts by hand follows the same layout under `/mnt/workspace/fuse-zc/`:

| Project | Create | Base |
|---|---|---|
| Linux, opaque additions (K1, K2, K3) | `git -C /mnt/workspace/ceph-client worktree add /mnt/workspace/fuse-zc/linux-<wp> -b fuse-zc/<wp> origin/upstream/opaque-obj-v9` | v9 `875cf36709ca` |
| Linux, io_uring core (K4) | same command with base `af32da41b032` | 7.3-rc7 merge, so the patch can be posted without the series |
| Linux, integration | branch `fuse-zc/kernel-int`: v9 + K1 + K2 + K4, maintained by the E1 worker | — |
| libfuse (L1–L6) | One shared clone, made by whoever comes first: `git clone https://github.com/libfuse/libfuse /mnt/workspace/fuse-zc/libfuse`. Each worker then runs `git -C /mnt/workspace/fuse-zc/libfuse worktree add /mnt/workspace/fuse-zc/libfuse-<wp> -b fuse-zc/<wp> origin/master` | master; `9572e08f8a2c` on 2026-10-13. Zero copy is not upstream yet. The analysed snapshot is `cb2e761c7635`, four small commits earlier. |
| Ceph (C1–C6) | One shared clone that leaves `~/ceph` untouched: `git clone --reference ~/ceph --dissociate https://github.com/ceph/ceph /mnt/workspace/fuse-zc/ceph`. Each worker then runs `git -C /mnt/workspace/fuse-zc/ceph worktree add /mnt/workspace/fuse-zc/ceph-<wp> -b wip-fuse-zc-<wp> origin/main` and `git submodule update --init --recursive` in its worktree | Ceph `main` |

**Ceph build.** In the worktree: `./do_cmake.sh -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_MGR_DASHBOARD_FRONTEND=OFF -DWITH_RBD=OFF -DWITH_RADOSGW=OFF`, then `ninja -C build <targets>`. Build only what your package needs: `unittest_bufferlist`, `ceph-fuse`, the `vstart` target for integration. To link ceph-fuse against the patched libfuse, install libfuse into `/mnt/workspace/fuse-zc/prefix` and export `PKG_CONFIG_PATH=/mnt/workspace/fuse-zc/prefix/lib/x86_64-linux-gnu/pkgconfig` before `do_cmake.sh`.

**UML kernel.** Follow the series recipe, which the owner has used for every round:
- `make ARCH=um O=<build> ...`. Force `CONFIG_IO_URING_OPAQUE_OBJ=y` (and its KUnit option when you need KUnit) at every commit, because `olddefconfig` drops the symbol at patch 1/5.
- Boot: `./linux mem=2G rootfstype=hostfs rootflags=<root> rw init=/init TESTS=<prog> ARGS=<args> con=null con0=null,fd:1`. For SMP add `CONFIG_SMP=y` and `seccomp=on ncpus=2`. Without `mem=`, UML runs out of memory.
- The selftest's default timeout is too short under UML SMP+KASAN: pass `OPAQUE_TEST_TIMEOUT=7000` on the command line.
- Debug options for every test kernel: KASAN, lockdep (`PROVE_LOCKING`), `DEBUG_ATOMIC_SLEEP`, kmemleak where useful.
- For FUSE (E1 and later) also enable `CONFIG_FUSE_FS=y`, `CONFIG_FUSE_IO_URING=y`, `CONFIG_UML_NET_VECTOR=y`, and boot with `fuse.enable_uring=1`.

**Cluster for integration (E1).** A vstart cluster built from the Ceph worktree, with memstore OSDs, runs on the host. The UML guest reaches it over a tap device: `sudo ip tuntap add dev fzc0 mode tap user $USER`, host address `10.77.0.1/24`, guest `vec0:transport=tap,ifname=fzc0` with `10.77.0.2/24`. Never use `/mnt/workspace/devcluster-data`, which belongs to another project.

---

## 5. Interface contracts (normative)

Userspace never hard-codes opcode numbers. ceph-fuse and libfuse build against the integration kernel's installed UAPI headers: `make O=<build> headers_install INSTALL_HDR_PATH=/mnt/workspace/fuse-zc/kheaders`, then `-I/mnt/workspace/fuse-zc/kheaders/include` ahead of `/usr/include`. Features are detected at runtime with `IORING_REGISTER_PROBE` and by the registration result, never by kernel version.

### 5.1 Kernel UAPI

**New opcodes** (`include/uapi/linux/io_uring.h`, `enum io_uring_op`), appended in merge order:
- `IORING_OP_OPAQUE_OBJ_FILL` (K1), after `IORING_OP_OPAQUE_COLLECT`.
- `IORING_OP_FIXED_COPY` (K4).

**New flags** (`include/uapi/linux/io_uring/opaque.h`):

```c
/* IORING_OP_OPAQUE_OBJ_FILL: sqe->ioprio contains these flags. */
#define IORING_OPAQUE_FILL_LAST		(1U << 0)

/* IORING_OPAQUE_RECV_OBJECT: sqe->rw_flags contains these flags. */
#define IORING_OPAQUE_RECV_F_CRC32C	(1U << 0)
```

**`IORING_OP_OPAQUE_OBJ_FILL` SQE** (K1):

| Field | Value |
|---|---|
| `fd` | -1 |
| `opaque_store_id` | the store's registration ID on this ring |
| `addr` | object handle |
| `off` | byte offset in the object |
| `len` | bytes to copy: > 0, and `off + len` within the object |
| `buf_index` | destination registered buffer |
| `addr3` | destination address inside that buffer: a user address for user-registered buffers, the byte offset for kernel-registered buffers (as `READ_FIXED` takes `addr`) |
| `ioprio` | 0, or `IORING_OPAQUE_FILL_LAST` |
| `rw_flags` and every other field | 0 |
| `IOSQE_CQE_SKIP_SUCCESS` | rejected together with `LAST`, so ownership transfer is always reported |

**FILL CQE.** `res` is `len` on success or a negative errno; a FILL is never short. `IORING_CQE_F_OPAQUE_CONSUMED` is set exactly when this request took ownership, on failures too. It is a 16-byte CQE. Errors:
- `-EINVAL` for bad fields;
- `-ESTALE` for a stale handle or a lost `LAST` race;
- `-ERANGE` for a range outside the object;
- `-EFAULT` for a destination out of bounds or registered without the DEST direction;
- `-ESHUTDOWN` for a dead store;
- `-ENOMEM`.

FILL is synchronous and does **not** count against `max_requests`, like FREE and STAT. In v9 a request-cap failure is an `-EAGAIN` at prep, and without `IORING_SETUP_SUBMIT_ALL` it stops the whole submit batch at that SQE (§8, decision O3).

**`LAST` semantics** mirror `SEND_LAST` in v9: at first issue the request atomically removes the handle and takes over the table reference. Two `LAST` requests for one handle cannot both claim it; the loser gets `-ESTALE` without the flag. A failure before ownership consumes nothing. After ownership, the object stays consumed even if the copy fails. A subrange `LAST` consumes the whole object.

**RECV_OBJECT crc** (K2). With `IORING_OPAQUE_RECV_F_CRC32C` in `rw_flags`, a successful RECV_OBJECT CQE carries `extra2 = ((u64)crc << 32) | IORING_OPAQUE_OBJECT_F_CRC32C | object_flags`, where

```c
/* RECV_OBJECT CQE only: bits 63..32 of the second extra word hold the crc. */
#define IORING_OPAQUE_OBJECT_F_CRC32C	(1U << 3)
```

Without the flag the CQE is unchanged: object flags only, high half zero. Failure CQEs are 16 bytes and carry no crc. The crc is crc32c with seed `0xFFFFFFFF` and **no final inversion**, i.e. Ceph's `ceph_crc32c(-1, data, len)` and the kernel's `crc32c(~0, data, len)`. Test vector: `"123456789"` → `0x1CF96D7C`. A kernel without K2 rejects the flag with `-EINVAL`; that is how userspace detects it.

**`IORING_OP_FIXED_COPY` SQE** (K4, generic io_uring):

| Field | Value |
|---|---|
| `fd` | -1 |
| `addr` | user source address |
| `len` | bytes, > 0 |
| `buf_index` | destination registered buffer |
| `addr3` | destination address inside that buffer, same convention as FILL |
| `rw_flags` and every other field | 0 (reserved for a direction flag later) |

**FIXED_COPY CQE.** `res` is `len` or a negative errno, never short. `-EFAULT` covers a bad source, a destination out of bounds, or a destination without the DEST direction.

**Zero fill needs no new ABI.** `IORING_OP_READ_FIXED` from `/dev/zero` (a plain fd works) writes zeros into the slot. **It can complete short**: `read_iter_zero()` stops on a pending signal, or on `need_resched()` under `IOCB_NOWAIT`, and the CQE then reports the short count. The submitter resubmits the remainder (§5.2).

### 5.2 libfuse public API

Everything is new API in `include/fuse_lowlevel.h` and `include/fuse_common.h`, versioned in the unreleased `FUSE_3.19` node of `lib/fuse_versionscript` (or `FUSE_3.20` if 3.19 ships first). Builds without io_uring get stubs that return `-ENOTSUP` (the `lib/fuse_uring_i.h` pattern). liburing is a private dependency, so public headers only forward-declare `struct io_uring_sqe` (from `<linux/io_uring.h>`) and never include `liburing.h`.

```c
/* include/fuse_common.h, struct fuse_file_info: after kill_suidgid;
 * padding : 22 becomes padding : 21. sizeof stays 64 (test/test_abi.c). */
uint32_t io_uring_zero_copy : 1;   /* fill_open() emits FOPEN_IO_URING_ZERO_COPY */

/* include/fuse_lowlevel.h */
struct io_uring_sqe;

struct fuse_uring_queue_info {
	size_t size;              /* sizeof(struct fuse_uring_queue_info) */
	unsigned int qid;
	int ring_fd;              /* this queue's io_uring fd, for io_uring_register(2) */
	unsigned int ring_flags;  /* IORING_SETUP_* used to create the ring */
};

struct fuse_uring_hooks {
	size_t size;              /* sizeof(struct fuse_uring_hooks) */
	/* Queue thread, after the ring exists, before the FUSE_INIT reply.
	 * Nonzero: the session falls back to /dev/fuse (graceful). */
	int (*queue_init)(void *userdata, const struct fuse_uring_queue_info *qi);
	/* Queue thread, after its last request finished, before the ring
	 * is destroyed. */
	void (*queue_exit)(void *userdata, const struct fuse_uring_queue_info *qi);
};

/* Before fuse_session_mount(). 0 or -errno. */
int fuse_session_set_uring_hooks(struct fuse_session *se,
				 const struct fuse_uring_hooks *hooks,
				 void *userdata);

#define FUSE_REQ_ZC_DEST	1	/* the slot is written by the server (READ) */

struct fuse_req_zc {
	size_t size;              /* sizeof(struct fuse_req_zc) */
	unsigned int qid;
	unsigned int buf_index;   /* slot index in the queue ring's buffer table */
	uint64_t length;          /* bytes registered; for READ, fuse_read_in.size */
	unsigned int dir;         /* FUSE_REQ_ZC_DEST */
};

/* 1: the request carries a slot (FUSE_URING_ENT_ZERO_COPY); 0: it does not;
 * -errno on error. */
int fuse_req_zero_copy(fuse_req_t req, struct fuse_req_zc *zc);

struct fuse_uring_op_result {
	int32_t res;              /* res of the op's last CQE */
	uint32_t flags;           /* flags of the op's last CQE */
	uint64_t done;            /* bytes completed by this op in total */
	uint64_t extra1, extra2;  /* big-CQE words of the last CQE, else 0 */
};

/* Queue thread. Prepare op @idx; @done is bytes already completed for it.
 * Return 1 if @sqe was prepared, 0 if the op is finished, -errno to fail it. */
typedef int (*fuse_uring_prep_fn)(void *arg, unsigned int idx, uint64_t done,
				  const struct fuse_req_zc *zc,
				  struct io_uring_sqe *sqe);

/* Queue thread, once, after every op finished or failed, before the commit.
 * Return the payload bytes to commit (0..zc->length) or -errno. The request
 * is committed and freed after this returns. */
typedef ssize_t (*fuse_uring_done_fn)(void *arg,
				      const struct fuse_uring_op_result *res,
				      unsigned int nr_ops);

/* Any thread. Only for requests with a slot. 0 or -errno (then nothing ran
 * and the caller must still reply). */
int fuse_reply_uring_ops(fuse_req_t req, unsigned int nr_ops,
			 fuse_uring_prep_fn prep, fuse_uring_done_fn done,
			 void *arg);
```

**`fuse_reply_uring_ops` semantics.**
1. The work moves to the request's queue thread.
2. libfuse calls `prep(arg, i, 0, zc, sqe)` for every op, tags `user_data` and submits.
3. Per CQE:
   - `res == -EAGAIN`: call `prep` again with the same `done` and resubmit (bounded; after `FUSE_URING_OP_RETRIES` attempts the op fails with `-EAGAIN`);
   - `res < 0`: the op failed;
   - `res >= 0`: add it to `done` and call `prep` again. A return of 1 resubmits (a continuation, for example a short `READ_FIXED` from `/dev/zero`); 0 finishes the op.
4. When every op is finished, call `done()`, then commit its result (`payload_sz` or the error), then free the request.
5. SQ space is managed by libfuse: ops wait in a per-queue backlog when the SQ is full. In-flight application ops per queue never exceed the CQ headroom.

**Rules for slot requests.**
- `fuse_reply_buf`, `fuse_reply_data` and `fuse_reply_iov` on a request with a slot commit `-EIO` and log once. They never write a pool buffer.
- `fuse_req_get_payload` returns NULL for them.
- `fuse_reply_err` works as usual.

**Session options** (each also gets a test environment variable and an INIT status line):
- `-o io_uring_defer_taskrun`: queue rings use `SINGLE_ISSUER | DEFER_TASKRUN | CQE32`. Required for importing an opaque store.
- `-o io_uring_zero_copy`: zero-copy queues; implies bufpool.
- `-o io_uring_workers=N`: dispatch ring requests to N workers.

The name `io_uring_single_issuer` is **not** used: master's `FUSE_CONN_FLAG_SINGLE_ISSUER` already means "the application replies on the ring thread".

### 5.3 Ceph internal interfaces

These are the seams between the Ceph work packages. Names are normative; signatures may gain `const` or `noexcept` where Ceph style needs it.

```cpp
// src/include/buffer.h (C1)
namespace ceph::buffer {
class opaque_owner {                // implemented by OpaqueStore (C2)
public:
  // Exactly once per unconsumed opaque raw, from any thread.
  virtual void opaque_release(uint64_t handle) noexcept = 0;
protected:
  ~opaque_owner() = default;
};
struct opaque_desc {
  opaque_owner* owner;   // non-null; outlives every raw it owns
  uint64_t handle;       // kernel object handle
  uint32_t length;       // object length, > 0
  uint32_t crc;          // ceph_crc32c(-1, bytes), no final xor
  uint32_t flags;        // IORING_OPAQUE_OBJECT_F_* from the RECV_OBJECT CQE
};
ptr create_opaque(const opaque_desc& d);   // ptr over [0, length); crc cache seeded
}
// class ptr:  bool is_opaque() const noexcept;
//             const opaque_desc* opaque() const noexcept;  // nullptr if not opaque
//             void mark_opaque_consumed() const noexcept;  // the kernel consumed it
// class list: bool has_opaque() const noexcept;
```

```cpp
// src/msg/async/opaque/OpaqueStore.h (C2)
class OpaqueStore final : public ceph::buffer::opaque_owner {
public:
  static int create(CephContext* cct);         // before the first messenger; 0 or -errno
  static OpaqueStore* get(CephContext* cct);    // nullptr when absent
  int store_fd() const;                         // for IORING_OPAQUE_REG_IMPORT
  bool crc_supported() const;                   // K2 present (probed)
  bool fixed_copy_supported() const;            // K4 present (probed)

  struct Reservation { uint64_t bytes = 0; };
  bool try_admit(uint64_t payload_bytes, Reservation* r);  // never blocks
  void unadmit(Reservation* r);                            // idempotent

  using PressureFn = std::function<void()>;    // runs on a messenger worker:
  int add_pressure_listener(PressureFn fn);     // must not block or take client_lock
  void remove_pressure_listener(int id);
  bool lowmem() const;                          // last observed memory mode

  // Copy opaque data into ordinary memory. Blocking; any thread except a
  // messenger worker. Used by every non-zero-copy consumer.
  int materialize(const ceph::bufferlist& in, ceph::bufferlist* out);

  void opaque_release(uint64_t handle) noexcept override;  // batched FREE
};
```

```cpp
// src/msg/Connection.h (written by C4, read by C3)
struct OpaqueReadHint {
  // READ ops of an opted-in MOSDOp: op index and object offset of each.
  std::vector<std::pair<uint32_t, uint64_t>> reads;
};
void Connection::opaque_hint_add(ceph_tid_t tid, OpaqueReadHint h);  // leaf lock
void Connection::opaque_hint_remove(ceph_tid_t tid);
std::optional<OpaqueReadHint> Connection::opaque_hint_get(ceph_tid_t tid) const;
```

```cpp
// osdc (C4, C5): a client-local opt-in, never encoded on the wire
enum : unsigned { OBJECTER_LOCAL_OPAQUE_OK = 1u << 0 };
// New trailing parameter `unsigned local_flags = 0` on Objecter::read_trunc,
// Objecter::sg_read_trunc, Filer::read_trunc and WritebackHandler::read.
// Objecter::Op gains: bool opaque_ok = false; OpaqueStore::Reservation opaque_resv;

// src/osdc/ZcPiece.h (C5)
struct ZcPiece {
  enum Type : uint8_t { OBJECT, ZERO, MEM } type;
  uint64_t len;
  ceph::bufferptr bp;    // OBJECT: opaque ptr selecting the object range; MEM: memory
  bool last = false;     // OBJECT: this plan holds every remaining reference
};

// ObjectCacher (C5)
bool ObjectSet::opaque_ok;   // set by Client while the inode has a zero-copy open
int ObjectCacher::file_read_handoff(ObjectSet* oset, file_layout_t* layout,
                                    snapid_t snapid, loff_t off, uint64_t len,
                                    std::vector<ZcPiece>* pieces, int flags,
                                    Context* onfinish);
void ObjectCacher::trim_opaque(uint64_t target_staged_bytes);

// Client (C5)
struct ZcReadPlan { std::vector<ZcPiece> pieces; uint64_t length = 0; };  // Σ len == length
bool Client::ll_zero_copy_eligible(Inode* in, int open_flags);
int64_t Client::ll_read_zc(Fh* fh, loff_t off, loff_t len, ZcReadPlan* plan);
```

**Ceph configuration** (C2 adds the `ms_*` options to `src/common/options/global.yaml.in`; C6 adds `fuse_*` to `src/common/options/mds-client.yaml.in`):

| Option | Type, flags | Default | Meaning |
|---|---|---|---|
| `fuse_zero_copy` | bool, startup | false | Enable zero-copy reads in ceph-fuse |
| `fuse_io_uring_workers` | uint, startup | 16 | libfuse `io_uring_workers` |
| `ms_opaque_store_limit` | size, startup | 512_M | Store `hard_limit` (tune in B1) |
| `ms_opaque_keep_min_bytes` | size, runtime | 512 | Smallest READ payload kept as objects (D4) |
| `ms_opaque_inflight_max` | size, runtime | 128_M | Admission budget for opted-in reads in flight |
| `ms_opaque_high_watermark` | float, startup | 0.8 | High watermark as a fraction of the capture limit |
| `ms_opaque_low_watermark` | float, startup | 0.6 | Low watermark, same base |
| `ms_opaque_ring_entries` | uint, startup | 256 | SQ entries per messenger ring (CQ is 4×) |
| `ms_opaque_auto_compact` | bool, startup | false | `IORING_OPAQUE_POLICY_F_AUTO_COMPACT` |
| `CEPH_OPAQUE_OBJECT_MAX_BYTES` | CMake cache variable | 131072 | Chunk size of received objects |

**Store sizing rule** (documented with the options): `ms_opaque_store_limit` should cover `charge_factor × (ms_opaque_inflight_max + client_oc_size + FUSE queues × queue depth × max read) + attached streams × 256 KiB`. The charge factor starts at 2 and B1 measures it, because v9 charges retained receive allocations whole.

---

## 6. Work packages

Line references are to v9 (`875cf36709ca`) for the kernel, libfuse master `cb2e761c7635` (master `9572e08f8a2c` adds four small commits on top, so a few `fuse_lowlevel.c` and `fuse_common.h` lines shift) and Ceph `8db74a411f9` (recent main), unless stated otherwise. Re-check them against your base; code moves. The verified fact sheets behind them, with quotes, are in `/mnt/workspace/ceph-client/fuse_zc_facts/` (`kernel_v9.md`, `libfuse.md`, `ceph.md`). Read the one for your project before starting.

### K1. `IORING_OP_OPAQUE_OBJ_FILL` (Linux, opaque series add-on)

**Branch.** `fuse-zc/k1-fill` on v9. **Depends on:** nothing. **Needed by:** C6, E1, T1.

**Deliverable.** Four patches on top of v9, mirroring the series layout (decision O1):
1. `io_uring/opaque: copy objects into registered buffers`: UAPI, `opdef.c`, `opaque.c`, `opaque.h`.
2. `io_uring/opaque: test FILL ownership and copies`: `opaque_test.c`.
3. `selftests/io_uring: exercise opaque FILL`: `opaque_obj.c`.
4. `docs: describe opaque FILL`: `Documentation/userspace-api/io_uring-opaque.rst`.

**Specification.** The SQE, CQE and `LAST` semantics are the §5.1 contract. Implement it as follows.

1. **UAPI.** Add `IORING_OP_OPAQUE_OBJ_FILL` after `IORING_OP_OPAQUE_COLLECT` (`include/uapi/linux/io_uring.h:322-324`). Add `IORING_OPAQUE_FILL_LAST` next to `IORING_OPAQUE_SEND_LAST` (`include/uapi/linux/io_uring/opaque.h:59`).
2. **opdef.**
   - In `io_issue_defs[]`, inside `#ifdef CONFIG_IO_URING_OPAQUE_OBJ` as for the other opaque ops (`opdef.c:595-633`): `.audit_skip = 1`, `.ioprio = 1`, `.async_size = sizeof(struct io_opaque_req)`, `.prep = io_opaque_fill_prep`, `.issue = io_opaque_fill`. No `.needs_file`. The `#else` branch uses `io_eopnotsupp_prep`.
   - In `io_cold_defs[]` (`opdef.c:892-906`): `.name = "OPAQUE_OBJ_FILL"`, `.cleanup = io_opaque_cleanup`, and a `.fail` hook that restores `IORING_CQE_F_OPAQUE_CONSUMED`. `io_req_defer_failed()` overwrites `cqe.flags` (`io_uring.c:957-969`); SEND solves the same problem with `io_opaque_fail()` (`opaque.c:1992-2002`).
3. **Prep, `io_opaque_fill_prep()`.**
   - Reject with `-EINVAL`: `fd != -1`, nonzero `rw_flags` or `__pad2[0]`, any `ioprio` bit other than `LAST`, `REQ_F_FIXED_FILE`, `LAST` together with `REQ_F_CQE_SKIP` (as `opaque.c:2107-2108`), and `len` outside 1..`INT_MAX`.
   - `off + len` overflow gives `-EOVERFLOW`.
   - Look the store up in the ring-local xarray as control prep does (`opaque.c:1568-1571`, `1582`).
   - Allocate the request state as `io_opaque_req_alloc()` does, but **uncounted**: treat FILL as `control` in the sense of `opaque.c:1595-1597`.
   - Set `req->buf_index = READ_ONCE(sqe->buf_index)`. Save handle (`addr`), offset (`off`), length (`len`), destination (`addr3`) and the `LAST` flag.
4. **Issue, `io_opaque_fill()`**, in this order.
   1. A dead store gives `-ESHUTDOWN` (as `opaque.c:1763-1765`).
   2. **Import the destination first:** `io_import_reg_buf(req, &iter, dest, len, ITER_DEST, issue_flags)`. It takes `uring_lock` itself through `io_find_buf_node()` (`rsrc.c:1244-1265`), so FILL must not hold `uring_lock` when it calls it. If you do call it with `uring_lock` held, pass `issue_flags & ~IO_URING_F_UNLOCKED` (precedent: `poll.c:949`). An import failure (bounds or direction, `rsrc.c:1205`) completes the request with that error and consumes nothing.
   3. **Resolve under `guard(mutex)(&store->tables)`**, like `io_opaque_send_acquire()` (`opaque.c:2072-2095`):
      - a missing slot or `!slot->data` gives `-ESTALE`;
      - a range outside the object gives `-ERANGE`;
      - with `LAST`: `op->data = io_opaque_detach(store, slot); op->consumed = true;`
      - without: pin with `refcount_inc(&slot->data->refs)`.
   4. **Copy outside every store and stream lock.** Walk `op->data->extents` from `op->offset`, as `io_opaque_copy()` does (`opaque.c:1457-1492`), but write with `copy_page_to_iter(extent->backing->page, extent->offset + skip, n, &iter)`. Backing pages can be compound heads with offsets beyond `PAGE_SIZE`, and `copy_page_to_iter()` handles that (`lib/iov_iter.c:361-387`). A shortfall gives `-EFAULT`; a gap gives `-ENODATA`, as for READ_OBJECT.
   5. Complete: `io_req_set_res(req, ret, op->consumed ? IORING_CQE_F_OPAQUE_CONSUMED : 0); return IOU_COMPLETE;`. Cleanup drops the version (`io_opaque_cleanup()` → `io_opaque_data_put()`, `opaque.c:1979`). For `LAST` that frees the object.
5. **Never return `-EAGAIN` from issue.** FILL never waits, so it needs no cancellation hook: `cancel.c:122-127` and `565` walk only waiting requests.
6. **Interactions to preserve.**
   - Compaction: a pinned FILL keeps its old version. A `LAST` detach makes an in-flight compaction fail to publish (`opaque_compact.c:155`), exactly as for `SEND_LAST`.
   - FORWARD_ONLY objects can be filled. `FILL_LAST` counts as forwarding.
   - Don't touch READ_OBJECT: its 64 KiB cap and its `buf_index` rejection stay.

**Tests.** Every new test must fail on v9 (or not build) and pass on K1.
- **KUnit** (`opaque_test.c`). Use the `opaque_sparse_data` fixture (`opaque_test.c:405-427`: 80,000 bytes in 40 extents of 2,000 bytes, where each byte is its extent index).
  - Copy the whole object and subranges that cross extents into a hand-built `ITER_DEST` bvec iterator over three non-contiguous pages; compare bytes.
  - Ownership, modelled on `opaque_send_last_ownership` (`opaque_test.c:600-645`): a second `LAST` gets `-ESTALE` without the flag; `LAST` against FREE; `LAST` against `SEND_LAST`; `LAST` after a compaction published claims the replacement.
- **Selftests** (`opaque_obj.c`), with user-registered buffers (`IORING_REGISTER_BUFFERS`):
  - offsets 0, middle and last byte;
  - out-of-bounds destination (`-EFAULT`), range outside the object (`-ERANGE`), stale handle (`-ESTALE`);
  - FILL after `LAST` gives `-ESTALE`; FREE after a plain FILL succeeds;
  - a cross-ring `LAST` race, modelled on `opaque_obj.c:2050-2102`: one winner with the flag, one `-ESTALE` loser;
  - FILL with `IOSQE_ASYNC`, which runs in io-wq (lockdep must stay silent);
  - `LAST` with `IOSQE_CQE_SKIP_SUCCESS` gives `-EINVAL`;
  - FILL succeeds while `max_requests` is exhausted;
  - FILL of a FORWARD_ONLY object (drive LOWMEM with watermarks);
  - update `ksft_set_plan()` and the documentation's test summary.
- **Kernel-registered buffers and the DEST direction** need a FUSE server. They are covered by T1 on the E1 kernel. A raw FUSE-over-io_uring test server for the selftest is optional (decision O5).
- **Full run** under UML with KASAN, lockdep and `DEBUG_ATOMIC_SLEEP`, single CPU and `ncpus=2`: all KUnit cases (28 plus yours) and all selftests (97 plus yours) pass, with no diagnostics.

**Documentation.** A "Delivery into registered buffers" section: SQE fields, `LAST` semantics, CQE, the request-cap exemption, the DEST direction rule, and "forwarders use `SEND_LAST` or `FILL_LAST`".

**Acceptance.** The §5.1 contract exactly; `checkpatch --strict` clean except `Signed-off-by`; `W=1` clean on `io_uring/`; the UML runs above pass, with the output quoted in the report.

**Pitfalls.**
- Calling `io_import_reg_buf()` with `IO_URING_F_UNLOCKED` while holding `uring_lock` deadlocks on the non-recursive mutex.
- Validate and import the destination **before** `LAST` detaches the handle. Otherwise a bad buffer consumes the object.
- Without the `.fail` hook, a generic failure after ownership loses the CONSUMED flag.
- Don't kmap a single page for a whole extent: extents can span compound pages.

### K2. crc32c in RECV_OBJECT (Linux, opaque series add-on)

**Branch.** `fuse-zc/k2-crc` on v9. Before integration, rebase onto K1; the conflicts are in UAPI defines and docs only. **Depends on:** nothing. **Needed by:** C3 (which has a fallback until then), E1.

**Deliverable.** Four patches, as in K1: code with UAPI and Kconfig; KUnit; selftest; docs.

**Specification.** The §5.1 contract (`IORING_OPAQUE_RECV_F_CRC32C`, `IORING_OPAQUE_OBJECT_F_CRC32C`, the extra2 layout).
1. **Prep** (`opaque.c:1590-1594` rejects any `rw_flags` today): accept `rw_flags == IORING_OPAQUE_RECV_F_CRC32C` for RECV_OBJECT only. Any other bit, and any `rw_flags` on another sub-op, still gives `-EINVAL`. Save the flag in the request state.
2. **Kconfig.** `select CRC32` under `IO_URING_OPAQUE_OBJ` (`io_uring/Kconfig:18-27`).
3. **Compute at publication**, in `io_opaque_ready()` after `io_opaque_vector()` (`opaque.c:1510`) and before `mutex_lock(&store->tables)` (`1514`), only when requested and the assembly succeeded.
   - This is task work on the submitting ring: only that ring's `uring_lock` is held, and it may sleep.
   - Walk `data->bvec`. Each bvec can span pages of a compound backing, so map page by page with `kmap_local_page()` and accumulate `crc = crc32c(crc, p, n)` from `~0U`, with no final inversion (`include/linux/crc32.h:82`).
   - `cond_resched()` between pages.
4. **CQE** (`opaque.c:1561-1562`): `io_req_set_res32(req, ret, 0, op->handle, object_flags | (crc_requested ? IORING_OPAQUE_OBJECT_F_CRC32C | ((u64)crc << 32) : 0))`.
5. Don't store the crc in `io_opaque_data`. OBJECT_STAT is unchanged.
6. Don't compute incrementally during capture: `io_opaque_take()` and `io_opaque_append()` run under `stream->lock`, and the collector also holds `lock_sock`.

**Tests.**
- **KUnit:** the helper over `opaque_sparse_data` equals `crc32c(~0, expected bytes)`; `"123456789"` gives `0x1CF96D7C`.
- **Selftests** (with a userspace crc32c in the test file):
  - RECV_OBJECT with the flag: the crc matches what was sent;
  - a payload received as several consecutive objects: the per-object crcs are correct;
  - without the flag: no `F_CRC32C` and a zero high half;
  - the flag on INSPECT, or an unknown bit, gives `-EINVAL`.
- **Full UML run** as in K1.

**Acceptance.** As in K1.

### K3. Store lock scalability (Linux; measurement first)

**Not started until B1 has numbers.**
- **Why.** v9 still serializes every importing ring on the store-wide `tables` mutex: handle resolve, publish, FREE, STAT, send queueing and attach (`io_opaque_lookup()` asserts it, `opaque.c:400-411`). Every extent free also takes `wait_lock`, and `io_opaque_wake_budget()` is O(waiters). The author deferred this.
- **Input.** B1's lock-contention profile: `perf lock contention`, or lockstat on `tables` and `wait_lock`, at full read load with N messenger rings and per-CPU FUSE rings.
- **Deliverable.** A short proposal with numbers, for example RCU-protected `slot->data` with `refcount_inc_not_zero()` for FILL, READ_OBJECT and FREE, and batched FREE. No patch without the owner's approval (decision O4).

### K4. `IORING_OP_FIXED_COPY` (Linux, generic io_uring; liburing)

**Branch.** `fuse-zc/k4-fixed-copy` on `af32da41b032`, so it can be posted without the opaque series. **Depends on:** nothing. **Needed by:** C6, which has a fallback without it.

**Deliverable.**
1. A kernel patch, `io_uring: add FIXED_COPY to fill registered buffers from user memory`, with UAPI and `opdef` entries. Put the code in `io_uring/rw.c` or a new small file, whichever the io_uring maintainers' style favours (report your choice).
2. A liburing patch series in a clone of `https://github.com/axboe/liburing`: `io_uring_prep_fixed_copy()`, a man page, and `test/fixed-copy.c`. liburing is where upstream io_uring tests live.
3. A cover-letter paragraph on the use case: FUSE and ublk zero-copy servers that must place data they already hold in memory into a kernel-registered buffer. Today that bounces through a pipe or memfd: two copies.

**Specification.** The §5.1 contract.
- **Prep.** Reject with `-EINVAL`: `fd != -1`, nonzero `rw_flags`, `len` of 0 or above `MAX_RW_COUNT`, buffer select, fixed files. Set `req->buf_index`.
- **Issue.**
  1. `io_import_reg_buf(req, &dst, addr3, len, ITER_DEST, issue_flags)`; it fails with `-EFAULT` on bounds or direction.
  2. `import_ubuf(ITER_SOURCE, u64_to_user_ptr(addr), len, &src)`.
  3. For each destination bvec segment, `copy_page_from_iter()`. There is no iterator-to-iterator helper, and segments can be multi-page.
  4. Under `IO_URING_F_NONBLOCK`, copy with page faults disabled. On a fault, return `-EAGAIN` so that io-wq retries in blocking mode, restarting from the beginning. In blocking mode a shortfall is `-EFAULT`.
- **CQE.** `res = len`.

**Tests** (liburing `test/fixed-copy.c`, run under UML on the K4 kernel):
- user-registered destination at several offsets;
- out-of-bounds destination;
- unmapped source (`-EFAULT`);
- a source that needs a page-in (a file-backed mmap after `MADV_DONTNEED`), which exercises the io-wq retry.
- Kernel-buffer destinations are covered by T1 through FUSE.

**Fallback if upstream rejects FIXED_COPY.** C6 detects it with `IORING_REGISTER_PROBE`. MEM pieces then go through a per-queue memfd: `pwrite` into the memfd, then `READ_FIXED` from it into the slot. That is two copies, on a rare path.

**Testing libfuse on a 7.3 kernel.** The host kernel (7.0) has no FUSE bufpool or zero-copy, so libfuse work packages test under UML on a v9-based kernel with the E1 configuration. Until E1 publishes its scripts, build that kernel yourself from §4.

### L1. Ring mode, reply hand-off, queue hooks, CQE tags (libfuse)

**Branch.** `fuse-zc/l1-ring-mode` on master. **Depends on:** nothing. **Needed by:** L3, L4, C6.

**Facts** (libfuse master `lib/fuse_uring.c` unless noted):
- One CPU-pinned queue thread per CPU (`get_nprocs_conf()`, 928).
- Rings use `SQE128 | SUBMIT_ALL | CQSIZE` (482-491). Kernel `SINGLE_ISSUER`/`DEFER_TASKRUN` sit in an `if (0)` block (493-505) because they measured slower.
- Replies submit from whichever thread replies (`fuse_uring_commit_sqe` 274-336 → `fuse_uring_submit` 249-272). Under kernel `SINGLE_ISSUER` that returns `-EEXIST`, which ends the session.
- The eventfd is a single-shot poll used for teardown, with `user_data` equal to the raw fd number (910-918, 1155-1159).
- Ring teardown (`io_uring_queue_exit`) runs on the joining thread (580-604).

**Deliverable.** Commits:
1. `fuse_uring: add an io_uring_defer_taskrun queue mode`
2. `fuse_uring: tear queues down on their own thread`
3. `fuse_lowlevel: add io_uring queue hooks`
4. tests and docs

**Specification.**
1. **Option** `-o io_uring_defer_taskrun`:
   - an `LL_OPTION` after `fuse_lowlevel.c:4366`, help near 4387, env `FUSE_URING_DEFER_TASKRUN` near 4807;
   - it implies `enabled`;
   - add the field to `struct fuse_session_uring` (`fuse_i.h:84-98`) and `struct fuse_ring_pool`.
2. **Ring flags.** In this mode `fuse_queue_setup_io_uring()` (472-537) adds `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CQE32` to the existing flags. The default mode does not change. The ring is already created on the queue thread (`fuse_uring_init_queue`, 1290-1353); keep it there.
3. **Owner-only submission.** In this mode only the queue thread calls `io_uring_enter` or `io_uring_register` on its ring.
   - A reply on another thread fills the entry's header and payload as today (`send_reply_uring`, 362-394), pushes the entry onto a per-queue multi-producer pending list, and writes the eventfd when the list was empty.
   - The queue thread turns pending entries into COMMIT_AND_FETCH SQEs and submits them.
   - Replies on the queue thread commit directly.
   - Reusing master's draining path is acceptable instead: repliers prepare SQEs under `ring_lock`, and the queue thread then flushes the SQ under `ring_lock`. Report which you chose.
4. **Wakeup.** The eventfd becomes a multishot `POLL_ADD`, re-armed when a CQE arrives without `IORING_CQE_F_MORE`. Teardown keeps using it (`fuse_session_destruct_uring`, 646-666), with a stop flag.
5. **Queue thread loop** (`fuse_uring_thread`, 1388-1502). In this mode always call `io_uring_submit_and_wait(ring, 1)`; its GETEVENTS runs the deferred task work. The queue thread never blocks anywhere else.
6. **CQE tags.** The low 3 bits of `user_data` are a tag:
   - 0: ring entry pointer (8-byte aligned, never NULL);
   - 1: application op (L3);
   - 2: eventfd poll;
   - 3: reserved.

   `user_data == 0` is never valid: count it and ignore it. Replace the raw fd comparison (1155-1159). Application-op CQEs are never cast to `fuse_ring_ent`, and a positive `res` on them is normal (today `res > 0` is fatal, 1181-1190).
7. **CQE32, not CQE_MIXED.** CQE_MIXED would need changes at two CQ-advance sites plus skipping `IORING_CQE_F_SKIP` padding CQEs. Check the advance count in `fuse_uring_queue_handle_cqes` (1196-1197) under CQE32.
8. **Teardown on the queue thread.** The queue thread runs the `queue_exit` hook and `io_uring_queue_exit` itself; the joining thread only joins and frees memory. `io_uring_queue_exit` may unregister a registered ring fd, which fails off the owner thread.
9. **Hooks** (§5.2).
   - `queue_init` runs in `fuse_uring_init_queue` right after ring setup (1306) and before the thread signals that it started (1404-1409).
   - A nonzero return makes `fuse_uring_start` fail, which takes the existing `-EADDRNOTAVAIL` path (`fuse_lowlevel.c:3169-3175`): the whole session falls back to `/dev/fuse`.
   - `queue_exit` runs in step 8, after in-flight application ops have drained.
10. `FUSE_CONN_FLAG_SINGLE_ISSUER` keeps working. In this mode it just means no hand-off is needed.
11. **Status line:** `FUSE_INIT: io_uring_defer_taskrun=on|off[:reason]`.
12. **meson:** probe `IORING_SETUP_DEFER_TASKRUN`, `IORING_SETUP_CQE32` and `IORING_POLL_ADD_MULTI` (`meson.build:237-265`).

**Tests.**
- A `--io-uring-defer-taskrun` flag in `test/run-tests.py` (896-914, 1645-1655). The whole io_uring matrix passes with it.
- A new C test (`test/cases/ctests/`): READ replies sent from another thread in this mode, 10,000 requests, no `-EEXIST`, no session exit.
- A hook test: each hook runs once per queue, on the queue thread (compare `gettid()`). A failing `queue_init` falls back to `/dev/fuse` (status line).
- The existing deferred-reply teardown test passes in this mode.
- The default mode is unchanged: every existing test passes. TSan clean (`tsan_suppressions.txt`).

### L2. Zero-copy queues and per-request slots (libfuse)

**Branch.** `fuse-zc/l2-zero-copy` on master. Rebase onto L1 when it lands; the `fuse_uring.c` conflicts are L2's to resolve. **Depends on:** nothing. **Needed by:** L3, L6, C6.

**Background.** The zero_copy_v7 draft (`joannekoong/libfuse`, `42950a86e7df`) cannot be cherry-picked; port its ideas by hand.
- In the draft, `fi->zerocopy` is never set on READ, because `fi` is rebuilt per request.
- The per-request `FUSE_URING_ENT_ZERO_COPY` flag is never read.
- The pipe bounce leaks fds and blocks above 64 KiB.

**Specification.**
1. **Option** `-o io_uring_zero_copy` (env `FUSE_URING_ZERO_COPY`); it implies bufpool, which implies enabled.
2. **State.** Per-queue zero-copy state, per-entry `zc_buf_index = idx + 1` (index 0 stays the bufpool, `FUSE_URING_BUFPOOL_BUF_INDEX`, 46), and a per-request zero-copy flag.
3. **Buffer table.** In zero-copy mode call `io_uring_register_buffers_sparse(ring, 1 + depth)`, then `io_uring_register_buffers_update_tag(ring, 0, &iov, NULL, 1)` for the pool.
   - If the pool registration fails (`RLIMIT_MEMLOCK`), keep the sparse table and set `bufpool_buf_index = -1`; the pool still exists through ADD_BUFPOOL.
   - `1 + depth` must not exceed 16384, and the index must fit a u16.
4. **ADD_QUEUE** (`fuse_uring_add_queue`, 752-762) with `FUSE_URING_ZERO_COPY`.
   - On `-EPERM` (no `CAP_SYS_ADMIN` in the init user namespace) or `-EINVAL` (old kernel), retry without the flag; the queue was not created. Mark the queue non-zero-copy and emit `FUSE_INIT: io_uring_zero_copy=off:qid=N:<errno>`.
   - Once a zero-copy ADD_QUEUE has succeeded, **never** call `fuse_uring_disable_bufpool` (828-834). The kernel rejects per-entry REGISTER on zero-copy queues, and a second ADD_QUEUE returns `-EEXIST`.
5. **REGISTER.** Set `ent_zero_copy_buf_index` after `fuse_uring_sqe_set_req_data` (which memsets) in **both** `fuse_uring_register_ent` (881) and the REGISTER case of `fuse_uring_resubmit` (1055-1060). Index 0 collides with the pool and fails every zero-copy request of that entry with `-EBUSY`.
6. **CQE handling** (`fuse_uring_handle_cqe`, 1079-1136).
   - Record `ent_in_out->flags & FUSE_URING_ENT_ZERO_COPY` on the request.
   - For a zero-copy READ, set `op_payload = NULL`. Its `offset` of 0 would otherwise alias pool buffer 0.
7. **Guards** (§5.2 rules). `send_reply_uring` (362-394), `fuse_reply_data_uring` (396-427) and `fuse_send_msg_uring` (432-470) commit `-EIO` and log once for slot requests. `fuse_req_get_payload` (338-360) returns NULL for them.
8. **Per-open bit** `fuse_file_info.io_uring_zero_copy` (`fuse_common.h:105-108`). `fill_open` (`fuse_lowlevel.c:584-604`) emits `FOPEN_IO_URING_ZERO_COPY` when the bit is set and at least one queue is zero-copy.
9. **`fuse_req_zero_copy()`** (§5.2) fills in `qid`, `buf_index`, `length` (from `fuse_read_in.size` in `op_in` for READ) and `dir`.
10. **Document** that only requests flagged `FUSE_URING_ENT_ZERO_COPY` have a slot. READs on a zero-copy open can still arrive without one (kvec direct I/O, `/dev/fuse` before the ring is ready, non-zero-copy queues) and are answered normally.

**Tests.**
- `--io-uring-zero-copy` in `run-tests.py`; root only (`_require_root`), otherwise skip. Assert the on and off status lines. As non-root, the option falls back with `EPERM` and the session works.
- A C test: a filesystem that marks opens zero-copy and answers READ with `fuse_reply_buf`. The reader gets `EIO`, and concurrent non-zero-copy requests still see intact payloads.
- Every existing test is unaffected.

### L3. Replies built from application ops (libfuse)

**Branch.** `fuse-zc/l3-reply-ops` on L1 + L2. **Needed by:** L6, C6.

**Specification.** `fuse_reply_uring_ops()` and the callbacks, exactly as in §5.2.
- **Tracking.** One state block per call, holding each op's `done`, `res`, `flags` and extras. Application-op `user_data` is that pointer with tag 1.
- **From other threads** the work item travels through L1's pending list.
- **Ops of one request** run concurrently and are not linked; their destination ranges are independent.
- **Capacity.** In-flight application ops per queue stay at or below `cq_entries − 2 × depth − 2`. The rest wait in a per-queue backlog. Size the CQ with `IORING_SETUP_CQSIZE` accordingly. A CQ overflow returns `-EBADR`, which master treats as fatal.
- **`-EAGAIN`.** Resubmit at once, at most `FUSE_URING_OP_RETRIES` (16) times. The queue thread never sleeps.
- **Commit.** After `done()`, commit `payload_sz` (checked against `zc->length`; above it means commit `-EIO` and log) or the error, then free the request. `done()` runs **before** the commit, because the request is freed after it.
- **Teardown.** Wait for in-flight ops (they are short), call `done()` (canceled ops report `-ECANCELED`), then commit; a commit may fail with `-ENOTCONN`.

**Tests.** A C test filesystem that answers zero-copy READs through `fuse_reply_uring_ops`:
- `READ_FIXED` from a backing file, split into three ops;
- a source that completes short (a pipe), to exercise continuations;
- a failing op: `done()` returns `-EIO` and the reader gets `EIO`;
- calls from a non-queue thread.

Verify the data with `cmp`.

### L4. Worker pool for ring requests (libfuse)

**Branch.** `fuse-zc/l4-workers` on L1. **Needed by:** C6.

**Why.** Ring requests are dispatched synchronously on the queue thread (`fuse_session_process_uring_cqe`, `fuse_lowlevel.c:4292-4354`). ceph-fuse handlers block (`client_lock`, OSD round trips). Under `DEFER_TASKRUN` the queue thread must never block outside `io_uring_enter`.

**Specification.**
- `-o io_uring_workers=N`. 0 keeps today's behaviour.
- The queue thread decodes the request as today and runs the op handler on a shared FIFO pool of N threads. Replies come back through L1's hand-off.
- The option is incompatible with `FUSE_CONN_FLAG_SINGLE_ISSUER`; reject the combination at INIT with a status line.
- Works in both ring modes.

**Tests.** The io_uring matrix with `workers=4` in both modes. A handler that sleeps 100 ms does not delay other requests on the same queue.

### L5. Interrupts for ring requests (libfuse)

**Branch.** `fuse-zc/l5-interrupt` on master. **Depends on:** nothing. It is useful upstream on its own. **Needed by:** T1 (`flock` interrupt).

**Facts.**
- INTERRUPT always arrives on `/dev/fuse` (kernel `fuse_dev_queue_interrupt`).
- With io_uring enabled, master answers it with `ENOSYS` (`fuse_lowlevel.c:4196`, 4208-4214). The kernel looks that reply up only in `fud->pq` and fails it with `ENOENT`, which libfuse ignores. So interrupts are never disabled and keep arriving, and ring requests never see them.
- Ring requests are never on `se->list` (`fuse_free_req`, 273-291).
- The kernel marks a ring request sent before userspace reaps its CQE (`dev_uring.c:1059`), so an INTERRUPT can overtake its request.

**Specification.**
1. Keep a session-wide hash of in-flight ring requests by `unique`. Insert in `fuse_session_process_uring_cqe` before the handler runs; remove in `fuse_free_req`. Protect it with one mutex (it is shared across queues).
2. On an INTERRUPT whose target is in the hash, mark the request interrupted and run its interrupt callback (`fuse_req_interrupt_func`), as `find_interrupted` (2340-2378) does. Send **no** reply; the `EAGAIN` requeue cannot work for ring requests.
3. If the target is not there yet, park the interrupt until a request with that `unique` is inserted, or for 5 s, whichever comes first.
4. Stop sending `ENOSYS` for interrupts of ring requests. `/dev/fuse` requests keep today's behaviour.

**Tests.** No interrupt-delivery test exists today. Add a C test whose `flock`/`SETLKW` handler waits until interrupted:
- a blocked caller gets `SIGINT`;
- the handler sees the interrupt and replies `EINTR`;
- run it with `--io-uring`, with workers, and with an artificially delayed dispatch (the early-interrupt race).

### L6. Example, tests and docs (libfuse)

**Branch.** `fuse-zc/l6-example` on L1–L5 merged.
- **`example/passthrough_hp.cc`.**
  - `--zero-copy` enables `-o io_uring_zero_copy -o io_uring_defer_taskrun`.
  - O_RDONLY opens are marked zero-copy.
  - `do_read` (1425-1458) answers slot requests with `fuse_reply_uring_ops` and `READ_FIXED` from the backing fd. `prep` continues while `done < len` and `res > 0`; `res == 0` is EOF.
  - Don't set `FUSE_CONN_FLAG_SINGLE_ISSUER` (214-215) when workers are on.
- **Tests:** zero-copy reads through `passthrough_hp` (large files, random offsets, EOF), with and without workers, with status lines.
- **Docs:** `doc/README.fuse-io-uring`, `ChangeLog.rst` (Unreleased) and `doc/ChangeLog-API.rst`.

### C1. `buffer::raw_opaque` (Ceph, common)

**Branch.** `wip-fuse-zc-c1-buffer` on Ceph main. **Depends on:** nothing. **Needed by:** C2–C6.

**Specification** (§5.3).
1. **The raw.** `raw_opaque` derives from `buffer::raw` in `src/common/buffer.cc`, constructed with `raw(len, mempool::mempool_buffer_opaque)`, which leaves `data == nullptr`. Add `f(buffer_opaque)` to `src/include/mempool.h:154-184`. It holds an `opaque_desc` and an atomic `consumed`. Its destructor calls `owner->opaque_release(handle)` unless consumed.
2. **Marker.** `get_data()` is non-virtual (`buffer_raw.h:91-93`), so add a cheap marker, for example a bool in `raw` set by the subclass constructor. Report the size impact on `raw`.
3. **`create_opaque()`** returns a ptr over `[0, length)` and seeds the crc cache: `set_crc({0, length}, {0xffffffff, crc})` (`buffer_raw.h:106-111`, an exact-range single entry).
4. **Guards.** Every byte-touching member aborts with `ceph_abort_msg("opaque buffer: <function>")` on an opaque raw.
   - ptr (`buffer.cc`): `c_str` (506-513), `end_c_str` (514-521), `operator[]` (527-538), `raw_c_str` (540), `copy_out` (544-550), `cmp` (557-570), `is_zero` (577-580), the `append` variants (582-600), `append_zeros` (602-611), `copy_in` (613-622), `zero` (624-639), and the `ptr::iterator_impl` constructor (`buffer.h:184-191`).
   - list operations that copy, rebuild, write or hash bytes (`rebuild*`, `write_fd`, `prepare_iovs`, `c_str`, `iterator::copy` into memory, `crc32c` on a cache miss, and the others in the fact-sheet table): check that each one reaches a guarded ptr member, and guard it explicitly where it doesn't.
   - **Exceptions:** `operator<<(ostream&, const ptr&)` (2317-2327) prints `opaque(handle=…, len=…)` instead of addresses. `is_aligned` (address only) returns false.
5. **Carriage.** An opaque raw must never become `_carriage`.
   - Assert in the carriage-setting `push_back(std::unique_ptr<ptr_node>)` (`buffer.h:1072-1077`) and `push_back(unique_leakable_ptr<raw>)` (1079-1084).
   - Assert in `append(const char*, …)`, `append_hole` and `obtain_contiguous_space` when the carriage is opaque.
   - Opaque ptrs enter lists only through `push_back(const ptr&)`, `push_back(ptr&&)`, `append(const ptr&, off, len)`, `claim_append`, `substr_of` and `splice`.
6. **Accessors:** `ptr::is_opaque()`, `ptr::opaque()`, `ptr::mark_opaque_consumed()`, `list::has_opaque()`.
7. **Cost.** No measurable cost on ordinary buffers: the guard is one predictable branch.

**Tests** (`src/test/bufferlist.cc`, `unittest_bufferlist`).
- Death tests for every guard.
- By-reference operations keep `nref` and never abort.
- crc: the list `[memory A][opaque O seeded with crc(B)][memory C]` gives `crc32c(-1)` equal to `ceph_crc32c` over A+B+C, with several seeds (the re-seed math at `buffer.cc:2117-2125`).
- The destructor calls `opaque_release` exactly once, and never after `mark_opaque_consumed`.
- Printing an opaque list does not abort.
- A release from another thread is safe.

### C2. `OpaqueStore` and messenger rings (Ceph, msg/async)

**Branch.** `wip-fuse-zc-c2-store` on C1. **Needed by:** C3–C6.

**Specification.**
1. **Build.** A CMake option `WITH_OPAQUE_OBJ`, on when `<linux/io_uring.h>` (from the integration headers, §5) defines `IORING_REGISTER_OPAQUE_STORE` and liburing is found; otherwise off and the code compiles out. Use liburing and set the opaque SQE fields by hand. New files go in `src/msg/async/opaque/`.
2. **`OpaqueStore::create(cct)`**, a CephContext singleton (pattern: `AsyncMessenger.cc:381-382`):
   1. Create a temporary ring (`SINGLE_ISSUER | DEFER_TASKRUN | CQE32`) on the calling thread.
   2. Register the store:
      - `hard_limit = ms_opaque_store_limit`, page aligned;
      - `max_object_size = CEPH_OPAQUE_OBJECT_MAX_BYTES`;
      - object, stream, extent and request caps at the series maxima (verify against `opaque.c:480-489`);
      - watermarks = the option fractions × the capture limit, page aligned;
      - `reply_reserve` = 64 KiB. It is unused by reads, but v9 requires it to be nonzero whenever watermarks are set (`opaque.c:493-494`).
   3. Export the store fd (`IORING_OPAQUE_REG_EXPORT`), then close the temporary ring.
   - **Errors.** Registration charges `hard_limit` to `RLIMIT_MEMLOCK` unless the process has `CAP_IPC_LOCK` (`opaque.c:516`). That failure, and `-EINVAL`/`-EOPNOTSUPP` on kernels without the series, are logged once and turn the feature off.
   - **Probes.**
     - `crc_supported()`: a RECV_OBJECT with `IORING_OPAQUE_RECV_F_CRC32C` on an invalid token. `-EINVAL` means unsupported, because prep rejects unknown `rw_flags` before the token is looked up. Confirm the error K2 gives instead, and document it.
     - `fixed_copy_supported()`: `IORING_REGISTER_PROBE`.
   - **Perf counters.** An `"opaque"` logger with its own enum range (patterns at `Objecter.cc:283-420` and `Stack.h:259-307`).
3. **`OpaqueRing`, one per Worker.**
   - **Creation.** In `PosixWorker::initialize()` (`PosixStack.cc:271-273`, which runs on the worker after `set_owner`, `Stack.cc:42-44`). Flags `SINGLE_ISSUER | DEFER_TASKRUN | CQE32`; SQ `ms_opaque_ring_entries`, CQ four times that.
   - **Store and eventfd.** Import the store, register an eventfd (`IORING_REGISTER_EVENTFD`), and add it with `create_file_event(efd, EVENT_READABLE, cb)` on the worker thread.
   - **The callback.** The epoll driver is edge-triggered (`EventEpoll.cc:66`), so the callback reads the eventfd, then repeats `io_uring_get_events()` and reaps until the CQ is empty.
   - **Teardown** in `Worker::destroy()` (`Stack.h:323`, `Stack.cc:59`): close every attached stream, wait for completions (bounded), exit the ring.
   - **Submission** happens only on the worker thread. A full SQ goes to a backlog. CQEs dispatch through tagged `user_data` to refcounted completion targets.
   - **CQ headroom.** Keep in-flight requests at or below the CQ size minus two per attached stream, so a collector's pressure CQE always fits. v9 ends collection with `-ENOSPC` otherwise (`opaque.c:1417-1426`).
4. **FREE batching.**
   - **Queueing.** `opaque_release()` may run on any thread. It pushes the handle onto a worker ring's multi-producer list, round robin.
   - **Waking.** When the list was empty, it wakes the worker through one **persistent** per-worker EventCallback with an "armed" flag. `dispatch_event_external` does not own callbacks and dedups only consecutive duplicates (`Event.cc:509-524`).
   - **Submitting.** The worker turns the list into FREE SQEs. FREE does not count against `max_requests`.
   - **After shutdown,** releases are dropped: store teardown frees every object.
5. **Admission** (§5.3). An atomic counter against `ms_opaque_inflight_max`. `try_admit` never blocks.
6. **`lowmem()`.**
   - It reflects the last STAT `memory_mode`. Worker 0 issues STAT at most once a second, and right after a pressure CQE.
   - A FORWARD_ONLY flag seen since that STAT also counts as LOWMEM.
7. **Pressure listeners** run on the worker that received the pressure CQE, at most once every 10 ms per store.
8. **`materialize()`.**
   - A dedicated materializer thread with its own ring (which imports the store) serves callers on any thread.
   - Opaque ptrs are read with READ_OBJECT in pieces of at most 64 KiB; ordinary ptrs are shared.
   - The caller blocks. Assert that the caller is not a messenger worker.

**Tests** (UML on the v9 or E1 kernel), `unittest_opaque_store`:
- create, export and import on N threads;
- FREE batches from foreign threads leave STAT at 0 objects;
- `materialize()` is byte-exact. Create objects with a test-only helper that attaches a loopback TCP pair and runs RECV_OBJECT;
- an `RLIMIT_MEMLOCK` failure turns the feature off;
- the probes on kernels with and without K2 and K4.

### C3. Messenger receive engine (Ceph, msg/async)

**Branch.** `wip-fuse-zc-c3-rx` on C2. **Depends on:** C2. K2 is optional because of the fallback in step 5i. **Needed by:** C6, T1. This is the largest package.

**Specification.**
1. **frames_v2.** Make `disassemble_first_segment` and `disassemble_remaining_segments` public (`frames_v2.h:385-388`); the comment at 357-384 describes exactly this split read. Keep the `ms_crc_data` behaviour (`m_with_data_crc`).
2. **Front-only peek.** Add `MOSDOpReply::peek_op_payload_lens(const bufferlist& front, uint16_t header_version, std::vector<std::pair<uint16_t /*op*/, uint32_t /*payload_len*/>>* out)`.
   - It decodes only the front: the v8 layout at `MOSDOpReply.h:239-264`, or v2–7 at 281-327, where the data split exists only from version 4.
   - `decode_payload` cannot be reused, because it splits DATA inline.
3. **Attach state.** Add a new state `OPAQUE_ATTACHING`, placed **outside** `[START_CONNECT..SESSION_RECONNECTING]` (`ProtocolV2.h:18-45`). Otherwise a lossy `_fault()` reconnects instead of stopping (`ProtocolV2.cc:343`, 389).
   - In `handle_server_ident`, replace `return ready();` (2206) with `return opaque_try_attach();`.
   - **Preconditions**, all evaluated after line 2192:
     - the store and this worker's ring exist;
     - `connection->peer_is_osd()` (`Connection.h:188`) and `connection->policy.lossy`;
     - `auth_meta->con_mode == CEPH_CON_MODE_CRC` and `!session_stream_handlers.rx`;
     - `rx_frame_asm.get_is_rev1()` and `!session_compression_handlers.rx`;
     - `ms_crc_data`;
     - `connection->recv_start == connection->recv_end && !connection->pendingReadLen`. `read_until` prefetches up to 64 KiB (`AsyncConnection.cc:232-284`), and those bytes would be lost.
   - If any precondition fails, `return ready();` unchanged.
   - **Otherwise:**
     1. `delete_file_event(cs.fd(), EVENT_READABLE)`;
     2. create a refcounted `OpaqueStream` that does not own the connection (CQE handlers check the connection under a lock);
     3. submit COLLECT;
     4. set the state to `OPAQUE_ATTACHING` and return nullptr.
   - **Token CQE**, under `connection->lock`:
     - if the state is no longer `OPAQUE_ATTACHING` (the connection faulted meanwhile), close the stream;
     - otherwise run `ready()` without its final `CONTINUE(read_frame)` (split `ready()` into a common part and the continuation), mark the connection attached, and start the engine at stream offset 0.
   - **Attach failure** (`-EBUSY`, `-ENOTCONN`, `-ENOSPC`, `-EOPNOTSUPP`, `-EINVAL`): re-add `EVENT_READABLE`, run `ready()`, count it.
4. **Reroute every userspace read once attached.**
   - `read_event()` (493-518) for READY and THROTTLE_*, and every `CONTINUE(read_frame)` site, call `opaque_next_frame()` instead. The sites are `ready()` 1369, `handle_message` 1471 and 1596, `handle_keepalive2` 1722, `handle_keepalive2_ack` 1739, `handle_message_ack` 1754, and the abort path 1404.
   - A userspace read on an attached socket returns `-EBUSY` (`tcp.c:2682`) and would fault the connection. Removing `EVENT_READABLE` alone does not stop time-event and external-event wakeups from reaching `read_event()`.
5. **The engine.** It is driven by CQEs, and every handler takes `connection->lock` as `process()` does (`AsyncConnection.cc:389`).
   a. **Keep the connection alive.** Every CQE updates `connection->last_active` (390). Otherwise the idle tick faults busy connections after 900 s (870-880).
   b. **Frame start.**
      - INSPECT at `frame_off` with a capacity of `CEPH_OPAQUE_FIRST_INSPECT` (512 B).
      - While more bytes are needed, INSPECT at the **end offset**; never re-inspect the same offset.
      - Parse the 32-byte preamble with `rx_frame_asm.disassemble_preamble()`.
      - Lay out the segments with `get_segment_onwire_len` (segment 0 is `logical_len + 4` in rev1) and `get_epilogue_onwire_len` (13, or 0 for a single-segment frame).
   c. **Control frames** (KEEPALIVE2, KEEPALIVE2_ACK, ACK; any other tag faults as today): INSPECT the whole frame, DISCARD it, then run the existing handlers (1692-1755).
   d. **MESSAGE.**
      - The state must be READY (1175-1179).
      - Run `throttle_message`, `throttle_bytes` and `throttle_dispatch_queue` (1600-1690) before receiving the segments. Waiting is safe: held bytes backpressure TCP.
      - The throttle timers resume the engine.
   e. **Headers.**
      - INSPECT segment 0 (41 + 4 bytes), the front and the middle, in pieces of at most 64 KiB at end offsets, copying them into bufferlists.
      - **DISCARD each copied range before inspecting further**, so the bytes held undecided stay small.
      - Check the segment 0 crc with `disassemble_first_segment`.
   f. **Decide DATA.**
      - **Zero-copy case:** `header2.type == CEPH_MSG_OSD_OPREPLY`, `connection->opaque_hint_get(tid)` returns a hint, and DATA ≥ `ms_opaque_keep_min_bytes`.
        - Peek the front for the payload lengths.
        - For each READ op in the hint whose outdata is at least `keep_min`, split that outdata into chunks of at most `CEPH_OPAQUE_OBJECT_MAX_BYTES`, aligned to multiples of the chunk size in **object offset** (the hint's offset plus the position).
        - Submit one RECV_OBJECT per chunk, with `IORING_OPAQUE_RECV_F_CRC32C` when `crc_supported()`.
        - INSPECT and DISCARD every other DATA byte; this covers other ops' outdata such as SplitOp's GET_INTERNAL_VERSIONS.
        - Submit the epilogue INSPECT at the DATA end offset immediately. A following header can be read independently of a preceding RECV_OBJECT.
      - **Otherwise** INSPECT and DISCARD DATA in pieces of at most 64 KiB (the copy path).
      - A RECV_OBJECT or DISCARD must never overlap the first byte of a pending INSPECT.
   g. **Epilogue:** INSPECT 13 bytes, then DISCARD them.
   h. **Assemble.**
      - `rx_segments_data` gets header, front and middle (copied).
      - DATA is built in order: copied bytes, then one opaque ptr per object covering exactly that object, then copied bytes, and so on. Its total must be exactly `logical_len`, which `frames_v2.cc:386` asserts.
      - Each opaque ptr comes from `create_opaque()` with the crc and flags of its CQE.
      - `disassemble_remaining_segments()` then checks the front, middle and DATA crcs and late_status. `bufferlist::crc32c` re-seeds the cached object crcs without touching bytes (`buffer.cc:2117-2125`).
   i. **Without K2.** Submit RECV_OBJECT without the flag, and compute each object's crc by materializing it with READ_OBJECT on this ring. Count `opaque_crc_fallback_bytes`. This is a correctness fallback only.
   j. **Abort and crc errors.**
      - late_status ABORTED: drop the frame (the raw destructors free the objects), reset the throttle, return to READY and read the next frame, exactly as today (1401-1405). **It is not a fault.**
      - A crc mismatch or malformed late_status raises FrameError, which faults the connection.
   k. **Dispatch.** Then `handle_message()` as today: `decode_message` with crcflags 0; fast dispatch drops `connection->lock` and rechecks the state (1571-1583). If the state is no longer READY afterwards, stop the engine.
6. **Errors.**
   - **RECV_OBJECT `-ENOBUFS`, `-EMSGSIZE` or `-ENOSPC`** (the prefix is restored): fall back for that range, INSPECT and DISCARD in pieces of at most 64 KiB, and count `opaque_recv_failures`.
   - **Fault the connection on:**
     - RECV_OBJECT `-ENODATA`, or a network error;
     - INSPECT returning 0 (EOF) or an error;
     - INSPECT `-ENODATA`, which means a consumed offset and is a bug.
   - **Collector `-ENOBUFS` with `F_MORE`** (extra1 = token, extra2 = first unread TCP offset): notify the pressure listeners; nothing else.
   - **Terminal collector CQE** (EOF, error, `-ECANCELED`, `-ENOSPC`): fault the connection.
7. **Teardown.**
   - `AsyncConnection::shutdown_socket()` (656-668) is the single funnel. When a stream is attached, submit STREAM_CLOSE before `cs.shutdown()`/`cs.close()`. Closing the fd does not release the claim: the stream holds a reference to the socket file.
   - `reset_recv_state()` resets the engine.
   - Completions that arrive after the connection is gone release what they carry and do nothing else.
8. **Perf counters:** kept bytes, copied bytes, RECV fallbacks, crc errors, attached streams, pressure events.

**Tests** (UML on the v9 or E1 kernel; loopback takes the kernel copy fallback, which is fine for function).
- **Fake OSD.** A server AsyncMessenger posing as an OSD sends scripted MOSDOpReply messages to an attached client connection. The client registers hints through a test hook and compares the materialized bufferlists byte for byte. Cases:
  - single READ;
  - several ops, including GET_INTERNAL_VERSIONS;
  - DATA below `keep_min`;
  - DATA spanning several chunks;
  - empty DATA;
  - a front larger than 64 KiB;
  - interleaved keepalives;
  - arrivals split by delays;
  - an injected late_status abort;
  - injected crc corruption (fault);
  - peer close (fault plus STREAM_CLOSE);
  - a tiny store (the pressure listener fires and the fallback loses no bytes).
- **Idle timeout:** a busy attached connection outlives a small `ms_connection_idle_timeout`.
- **Leaks:** after `mark_down_all`, STAT reports 0 streams and 0 objects.

### C4. Objecter opt-in, admission and SplitOp (Ceph, osdc)

**Branch.** `wip-fuse-zc-c4-objecter` on C2. **Needed by:** C5, C6.

**Specification.**
1. **`local_flags`** (§5.3) on `Objecter::read_trunc` (3468-3488), `sg_read_trunc` (4041-4060), `Filer::read_trunc` (`Filer.cc:64-81`), `WritebackHandler::read` (`WritebackHandler.h:17-21`) and `ObjecterWriteback::read` (`ObjecterWriteback.h:18-28`, which today drops `op_flags` entirely). `OBJECTER_LOCAL_OPAQUE_OK` sets `Op::opaque_ok`. Wire flags do not change.
2. **Admission** in `_op_submit_with_budget` (`Objecter.cc:2474-2515`), not in `_op_submit`, which is re-entered on redirect and EAGAIN.
   - If the op is opted in and has READ ops, call `try_admit()` with the sum of their extent lengths.
   - On failure, clear `opaque_ok` and count a denial.
   - Release the reservation in `_finish_op` (3561-3587) and on cancel.
3. **Hints.**
   - **Add** in `_send_op` (3648-3719), after `ConnectionRef con = op->session->con` (3692): for opted-in ops, `con->opaque_hint_add(op->tid, {each READ op's index and extent offset})`.
   - **Remove** in `_session_op_remove` (3432-3447) with `from->con`. That one hook covers `_finish_op`, `op_cancel`, `close_session`, redirect, EAGAIN, retry-writes, split EAGAIN and osdmap re-targeting.
   - Hints are best effort; a stale attempt is ignored anyway (3907-3917).
4. **Storage on `Connection`:** a small map from tid to hint under a new leaf mutex, in place of the dead `rx_buffers` (`Connection.h:55-56`, 217-230). It is called with Objecter's `rwlock` and the session lock held.
5. **SplitOp.**
   - Copy `opaque_ok` from the parent to the sub-ops in `SplitOp::create` (1088-1106). Sub-ops go straight to `_op_submit`; the parent's reservation covers them.
   - An EC direct read (`prepare_single_op`, 899-928) is an ordinary single op.
6. **Pre-sized copies.** `Objecter.cc:3991-4003` and `SplitOp.cc:588-608` copy into a caller's pre-sized buffer. Assert `!bl.has_opaque()` there; Client passes empty buffers, so those branches are not reached.

**Tests.**
- Admission accounting under concurrency, with no leak after cancel, redirect or EAGAIN resend.
- The hint lifecycle with a fake Connection.
- SplitOp propagation.

### C5. ObjectCacher and Client (Ceph, osdc and client)

**Branch.** `wip-fuse-zc-c5-client` on C1 + C4. It can start early against a fake opaque owner. **Needed by:** C6.

**Specification.**
1. **Opt-in.**
   - `ObjectSet::opaque_ok` (`ObjectCacher.h:389-406`) is set by Client while the inode has an eligible zero-copy open.
   - `bh_read` (`ObjectCacher.cc:810-838`) passes `OBJECTER_LOCAL_OPAQUE_OK` through `WritebackHandler::read` when `bh->ob->oset->opaque_ok` is set. Readahead-only reads also require `!lowmem()`.
2. **Zero padding.** Short-read padding in `bh_read_finish` (855-860) and `Client::_read_sync` (12155-12157) uses `raw_zeros` (`append_zero2`) when the list holds opaque data. Those become ZERO pieces (`is_zero_fast()` identifies `raw_zeros`, `buffer.cc:572-575`).
3. **Byte safety.** `maybe_rebuild_buffer` (257-267, 8 callers) skips lists with opaque data. `operator<<(BufferHead)` (`ObjectCacher.h:766`) prints `firstbyte` only without opaque data. It is printed at about 40 sites from debug level 7.
4. **`file_read_handoff()`** (§5.3). It needs a new name because `file_read_ex` exists (`ObjectCacher.h:717-725`).
   - Map, wait and retry exactly like `file_read_ex`/`_readx` (1422-1734).
   - On completion, produce pieces in buffer order:
     - clean opaque data: OBJECT pieces;
     - `raw_zeros` and ZERO BHs: ZERO pieces. ZERO holes are object offsets (1643-1647), so build pieces during assembly, where the buffer offsets are known;
     - everything else (dirty, TX, ordinary memory): MEM pieces.
   - Then remove the delivered part of clean opaque BHs from the cache: split at the range boundaries and `bh_remove` the delivered part, under `client_lock`. Dirty and TX BHs are never removed.
5. **`trim_opaque(target)`** is public and runs under `client_lock`. It evicts clean opaque BHs, FORWARD_ONLY first and then LRU from `bh_lru_rest`, until staged opaque bytes are at most `target`.
6. **FORWARD_ONLY is never kept as readahead.** If `bh_read_finish` receives FORWARD_ONLY data for a BH that no reader waits on, it discards it. If a reader waits, it marks the BH dontneed (`bh_add` puts it at the LRU bottom, 2804-2805).
7. **Pressure.** Client registers a pressure listener at init. The listener only posts a finisher job, which takes `client_lock` and calls `trim_opaque(staged / 2)`. It never takes `client_lock` on a messenger worker.
8. **`ll_zero_copy_eligible(in, flags)`** requires all of:
   - an O_RDONLY open;
   - `!in->is_fscrypt_enabled()` (`Inode.h:195-197`);
   - no inline data (`in->inline_version >= CEPH_INLINE_NONE`, `Client.cc:11742`);
   - the store exists.
9. **`ll_read_zc(fh, off, len, plan)`.**
   - **Locking:** a scoped `client_lock`, as in `ll_read` (17275-17311). It returns with the lock released.
   - **Base:** a variant of `_read` (11679-11892), with the same caps.
   - **Cached path** (case 1): `file_read_handoff()`, with the same waits as `_read_async` (11956-12088), which drop `client_lock` while waiting.
   - **Sync path** (case 3): `filer->read_trunc` with `OBJECTER_LOCAL_OPAQUE_OK`, then turn the result into pieces.
   - **Readahead** (`do_readahead`, 11914-11934) is unchanged except for the LOWMEM rule.
   - **Ineligible inodes** take the normal path and get MEM pieces.
   - **`last`.** Once the cache has dropped its references, `piece.last = (bp.raw_nref() == number of plan pieces that reference that raw)`.
10. **Materialize for every other consumer.** Any consumer that is not zero-copy and can now see opaque data materializes it with `OpaqueStore::materialize()` after dropping `client_lock`:
    - `Client::read` (11505-11507);
    - `_preadv_pwritev_locked` (12324-12326);
    - `ll_read` (17307), which `fuse_ll_read` uses for non-zero-copy opens of an inode whose cache holds opaque data;
    - every other caller of `_read`.

    fscrypt paths are excluded by eligibility.

**Tests.** ObjectCacher tests with a fake WritebackHandler that returns opaque raws, and a fake owner that records releases:
- hand-off removes exactly the delivered ranges;
- a partially delivered object stays cached with `last = false`; a fully delivered one gets `last = true`;
- `trim_opaque` evicts in the right order, FORWARD_ONLY first;
- printing at debug 20 does not abort;
- non-zero-copy reads materialize, through a fake materializer;
- `ll_read_zc` pieces for holes, EOF and dirty data (MEM).

### C6. ceph-fuse integration (Ceph)

**Branch.** `wip-fuse-zc-c6-fuse` on C2–C5, built against libfuse L1–L3 installed in `/mnt/workspace/fuse-zc/prefix`. **Needed by:** T1.

**Specification.**
1. **Build.**
   - A CMake check for `fuse_reply_uring_ops` in the installed libfuse defines `HAVE_FUSE_URING_OPS`; the zero-copy code builds only then.
   - 64-bit capabilities go through `fuse_set_feature_flag`, because `do_init` uses the legacy 32-bit `want` (`fuse_ll.cc:1477-1490`).
2. **main** (`src/ceph_fuse.cc`). With `fuse_zero_copy`, call `OpaqueStore::create(cct)` **before** the messenger is created (276). The NetworkStack and its workers start inside the AsyncMessenger constructor (`AsyncMessenger.cc:385`), and `PosixWorker::initialize()` needs the store. On failure, log it and continue without zero copy.
3. **Mount options** (the argv built in `init()`, `fuse_ll.cc:1609-1741`): `-o io_uring`, `-o io_uring_bufpool`, `-o io_uring_zero_copy`, `-o io_uring_defer_taskrun`, `-o io_uring_workers=<fuse_io_uring_workers>`. The splice options do nothing in this mode.
4. **Hooks**, set with `fuse_session_set_uring_hooks` before `fuse_session_mount` (`start()`, 1743-1796).
   - `queue_init`:
     - imports the store into the queue ring (`IORING_REGISTER_OPAQUE_STORE` with `IORING_OPAQUE_REG_IMPORT` and `store_fd()`) and records `store_id[qid]`;
     - probes FIXED_COPY once;
     - without FIXED_COPY, creates a per-queue memfd for MEM pieces.
   - `/dev/zero` is opened once per process as a plain fd. libfuse's fixed-file table has only `/dev/fuse`, and `READ_FIXED` accepts a plain fd.
5. **Opens.** In `fuse_ll_open` (842-876) and `fuse_ll_create` (1292-1302), if `ll_zero_copy_eligible(in, fi->flags)`:
   - set `fi->io_uring_zero_copy = 1`;
   - mark the Fh;
   - set `in->oset.opaque_ok`, cleared in release when the last zero-copy Fh closes.

   The `keep_cache` and `direct_io` logic is unchanged.
6. **`fuse_ll_read`** (878-911).
   - If `fuse_req_zero_copy(req, &zc) == 1`, call `ll_read_zc`. An error goes to `fuse_reply_err`.
   - Otherwise build the ops:
     - **OBJECT:** `OPAQUE_OBJ_FILL` with `store_id[zc.qid]`, the handle, ptr offset, length, `buf_index = zc.buf_index` and `addr3` = the slot offset, plus `ioprio = LAST` when `piece.last`;
     - **ZERO:** `READ_FIXED` from the `/dev/zero` fd at the slot offset plus `done`, continued while `done < len`;
     - **MEM:** `FIXED_COPY` from the data plus `done`; without K4, `pwrite` into the queue memfd, then `READ_FIXED` from it, with continuation.

     Then call `fuse_reply_uring_ops()`.
   - Without a slot, take the existing path. Any opaque data in it was already materialized (C5).
7. **`done` callback.**
   - Call `bp.mark_opaque_consumed()` for each OBJECT op whose CQE has `IORING_CQE_F_OPAQUE_CONSUMED`.
   - Drop the plan; unconsumed raws free through their owner.
   - Return `plan.length` if every op completed in full, else `-EIO`. Never exceed `zc.length`.
8. **Perf counters:** fills, consumed, MEM bytes, ZERO bytes, fallback reads.
9. **Docs:** `doc/man/8/ceph-fuse.rst` and the CephFS docs. Requirements:
   - a kernel with the series plus K1 and K2;
   - libfuse with L1–L3;
   - `CAP_SYS_ADMIN`;
   - `RLIMIT_MEMLOCK` of at least the store limit, or `CAP_IPC_LOCK`;
   - `fuse.enable_uring=1`.

**Tests.** Covered by T1.

### E1. Integration environment (scripts only)

**Location.** `/mnt/workspace/fuse-zc/env`, a small git repo of scripts. **Depends on:** nothing; it starts with v9 and picks up K1, K2 and K4 as they land. **Needed by:** C2, C3 and libfuse testing (kernel), T1.

**Deliverables.**
1. **`kernel-int.sh`.**
   - Merges `fuse-zc/kernel-int` (v9 plus K1, K2 and K4 once they exist).
   - Builds it as UML with a config fragment: `IO_URING`, `IO_URING_OPAQUE_OBJ` and its KUnit option, `SOCK_RX_OWNER`, `FUSE_FS`, `FUSE_IO_URING`, `HOSTFS`, `UML_NET_VECTOR`, `INET`, `MEMCG`, KASAN, `PROVE_LOCKING`, `DEBUG_ATOMIC_SLEEP`.
   - Installs the UAPI headers to `/mnt/workspace/fuse-zc/kheaders`.
2. **`net.sh up|down`:** a user-owned tap `fzc0`, host `10.77.0.1/24`.
3. **`cluster.sh start|stop`:**
   - a vstart cluster from the Ceph worktree build: MON=1, OSD=3, MDS=1, MGR=1;
   - memstore OSDs, msgr2, bound to `10.77.0.1`, with a CephFS created;
   - separate from `/mnt/workspace/devcluster-data`.
4. **`guest.sh <script>`.**
   - Boots UML: `mem=4G`, hostfs root, `fuse.enable_uring=1`, `vec0:transport=tap,ifname=fzc0`.
   - The guest init sets `10.77.0.2/24` and runs the script as root, then powers off.
   - `guest.sh` returns the script's exit status and the console log.
5. **Smoke tests.**
   - Stock ceph-fuse from the Ceph build mounts in the guest and reads a 100 MiB file with the right checksum.
   - Later, libfuse zero-copy through `passthrough_hp` (after L6).

**Acceptance.** One command runs a guest test end to end, and a README documents it.

### T1. Integration tests

**Runs on** E1 with the integration kernel, patched libfuse and the C6 build. **Depends on:** C6, E1, K1, K2.

**Cases.**
- **Data integrity:**
  - `fsx`, read-only and alongside a writer on a non-zero-copy open;
  - `fio --verify`, sequential and random, at several block sizes;
  - reads that cross chunk and object boundaries; holes; EOF;
  - files written through another mount.
- **Read after write** through ObjectCacher: dirty data comes back as MEM pieces.
- **OSD restarts and connection resets** during reads (`ms_inject_socket_failures` on the OSDs).
- **A small store**, forcing admission denials, RECV fallbacks, pressure CQEs, LOWMEM and FORWARD_ONLY.
- **Interrupts:** a `flock` interrupt (L5), and SIGKILL of readers mid-read.
- **Teardown:**
  - unmount with reads in flight;
  - after unmount, STAT shows 0 objects and 0 streams, through a new admin socket command `opaque stat`.
- **Kernel-buffer paths:** FILL and FIXED_COPY into real FUSE slots succeed and are counted. That covers the kernel-buffer DEST path of K1 and K4.
- **Logging:** runs with `debug_objectcacher=20` and `debug_ms=20` (no aborts from logging).
- **Mixed opens:** a non-zero-copy open of the same inode at the same time (materialize path).

**Acceptance.** Everything passes on single-CPU and `ncpus=2` UML, with no kernel diagnostics, and an ASan build of ceph-fuse is clean.

### B1. Benchmarks and tuning

**Prerequisite** (decision O8): a machine with a real NIC that can boot the integration kernel natively. UML over loopback or tap cannot show zero-copy capture or real costs. Repeat the series' KUnit and selftests on that kernel first; the series has only been validated under UML with loopback.

**Measure** against today's ceph-fuse, with matched readahead and thread settings:
- throughput and CPU per GiB for sequential and random reads, 4 KiB to 4 MiB;
- copied bytes (perf counters) and CQEs per READ;
- store charge and extents per payload byte (STAT), which gives the charge factor for the sizing rule;
- fallback rates;
- contention on the store's `tables` mutex and `wait_lock` (input to K3);
- the effect of `ms_opaque_keep_min_bytes` and `CEPH_OPAQUE_OBJECT_MAX_BYTES`.

**Deliverable.** A report with numbers and recommended defaults.

---

## 7. Milestones

| Milestone | Done when |
|---|---|
| **M1 Kernel** | K1 and K2 pass the full UML suite (KUnit and selftests, single CPU and `ncpus=2`, debug options on); E1 builds `fuse-zc/kernel-int`. K4 has its liburing test passing. |
| **M2 libfuse** | L1–L3 merged on one branch; `passthrough_hp` zero-copy reads are byte-exact in UML; existing tests unchanged. |
| **M3 Receive parity** | C1–C4: the Ceph client in UML receives OSD read replies as objects, materialized reads are byte-exact, and fallbacks, pressure and teardown leave STAT at zero. |
| **M4 End to end** | C5, C6 and T1 pass: ceph-fuse serves zero-copy reads through FUSE slots, with no copy through userspace (perf counters). |
| **M5 Numbers and upstream** | B1 report; defaults set; K3 decided. Postings: the opaque series' next version with K1 and K2; io_uring FIXED_COPY; libfuse PRs; Ceph PRs. |

---

## 8. Decisions for the owner

Workers proceed with the default in each row. Change a default before dispatch if you disagree.

| # | Decision | Default in this plan |
|---|---|---|
| O1 | Deliver K1 and K2 as add-on patches on v9, or fold them into the series' five patches for the next posting | Add-on patches, each mirroring the series layout (code, KUnit, selftest, docs) |
| O2 | FILL as its own opcode, or READ_OBJECT extended with `buf_index` and a lifted cap | Own opcode: it needs `buf_index`, its own prep and a `LAST` flag, as SEND does |
| O3 | FILL counted against `max_requests` or exempt | Exempt. Counted, every ring that issues FILL would need `IORING_SETUP_SUBMIT_ALL`, because a prep `-EAGAIN` stops the submit batch |
| O4 | Store lock scalability (`tables`, `wait_lock`) | Decide after B1 (K3) |
| O5 | Add a raw FUSE-over-io_uring server to the series selftests, to test kernel buffers and DEST-direction rejection in the series itself | No; T1 covers kernel buffers |
| O6 | Post FIXED_COPY upstream, or rely only on the memfd bounce | Post it; keep the bounce as the fallback |
| O7 | libfuse names (`io_uring_defer_taskrun`, `io_uring_zero_copy`, `io_uring_workers`, `fuse_reply_uring_ops`, the hooks) | As in §5.2; maintainers may rename them |
| O8 | Benchmark hardware: a machine with a real NIC that boots the integration kernel | Needed for B1; this machine cannot boot a test kernel |
| O9 | Store sizing defaults (§5.3) | Placeholders until B1 |
| O10 | Ceph Objecter opt-in as a `local_flags` parameter, or an options struct | `local_flags` |
| O11 | libfuse default ring mode | Unchanged; only ceph-fuse turns on `io_uring_defer_taskrun`. Master measured `SINGLE_ISSUER`/`DEFER_TASKRUN` slower for its own use. |
