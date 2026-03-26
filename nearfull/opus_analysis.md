# CephFS Write Stall Analysis — Kernel Client Code Review

See [jira.md](jira.md) for the original issue description.

All code references are against the tree at `/mnt/attached/linux` (kernel 5.15-based,
ODF 4.21 era). Line numbers may shift across rebases.

---

## TL;DR

| # | Theory | Root cause | Matches 200 KiB/s? | Persistent for hours? | Explains mgr-restart fix? | Likelihood |
|---|--------|-----------|--------------------|-----------------------|--------------------------|------------|
| **A** | **Lost Fb (FILE\_BUFFER) cap after MDS reconnect** | MDS doesn't re-grant Fb after session reconnect; every write falls to `ceph_sync_write` — one OSD round-trip per 4 KiB | Yes — 4 KiB / ~5 ms ≈ 200–800 KiB/s | Yes — persists until MDS reassesses caps | Yes — mgr restart → MDS map update → cap re-evaluation → Fb restored | **Highest** |
| **B** | **Stale osdmap retaining POOL\_FLAG\_NEARFULL** | Client's local osdmap copy still has the nearfull flag; every write triggers IOCB\_DSYNC → `ceph_fsync` → per-write OSD flush | Yes — same one-RT-per-write math | Only if mon subscription is broken | Partially — mgr restart may trigger osdmap push | Medium |
| **C** | **`CEPH_I_ERROR_WRITE` sticky flag from writeback failures** | OSD errors during disruption set per-inode flag forcing sync writes | Yes | No — first successful sync write clears it | No | Low (compounding only) |
| **D** | **`i_max_size` starvation after MDS reconnect** | Reconnect resets `i_wanted_max_size`/`i_requested_max_size` to 0; MDS slow to grant new allocation | Partial — periodic stalls more than steady slow | Possible but atypical | Yes | Medium-low |
| **E** | **Pool FULL flag stuck in osdmap (not nearfull)** | Client holds stale map with CEPH\_POOL\_FLAG\_FULL; all OSD writes paused | No — would be **zero** throughput, not 200 KiB/s | Yes | Yes | Low |

**Bottom line:** Theory A (lost Fb cap) is the best fit. The 200 KiB/s throughput
is the signature of per-object synchronous OSD writes for a 4 KiB sequential workload,
and cap loss after MDS reconnect is the only mechanism that is both persistent and
resolved by an indirect MDS map update (triggered by mgr restart).

---

## Why nearfull is (almost certainly) not the root cause

### 1. The nearfull check is stateless — no "mode" to get stuck in

The entire nearfull mechanism in the kernel client consists of **six lines** inside
`ceph_write_iter` in `fs/ceph/file.c`. There is no per-inode flag, no mount-level
state, and no "nearfull mode" that persists between `write()` syscalls.

At the top of every `write()` call, the current osdmap flags are read fresh:

```
fs/ceph/file.c:2388-2391

    down_read(&osdc->lock);
    map_flags = osdc->osdmap->flags;
    pool_flags = ceph_pg_pool_flags(osdc->osdmap, ci->i_layout.pool_id);
    up_read(&osdc->lock);
```

Then, only at the very end, after the write has already succeeded through the
normal buffered path (`generic_perform_write`), the nearfull flags are checked:

```
fs/ceph/file.c:2492-2497

    if (written >= 0) {
        if ((map_flags & CEPH_OSDMAP_NEARFULL) ||
            (pool_flags & CEPH_POOL_FLAG_NEARFULL))
            iocb->ki_flags |= IOCB_DSYNC;
        written = generic_write_sync(iocb, written);
    }
```

The variables `map_flags` and `pool_flags` are **local to this function call**.
The next `write()` reads them again from the live osdmap. If the pool's nearfull
flag has been cleared (which it must be at 97% free / HEALTH\_OK), the next write
will not set `IOCB_DSYNC`. There is nothing to "switch back" because there is
nothing that was "switched" — it is a per-syscall, stateless check.

### 2. The cluster-wide CEPH\_OSDMAP\_NEARFULL flag is legacy

The header comment in `include/linux/ceph/rados.h` says it explicitly:

```
include/linux/ceph/rados.h:143-149

    #define CEPH_OSDMAP_NEARFULL (1<<0)  /* sync writes (near ENOSPC),
                                            not set since ~luminous */
    #define CEPH_OSDMAP_FULL     (1<<1)  /* no data writes (ENOSPC),
                                            not set since ~luminous */
```

Since Luminous (~2017), Ceph uses **per-pool** flags (`CEPH_POOL_FLAG_NEARFULL`,
`CEPH_POOL_FLAG_FULL`) instead of the global osdmap bits. The code checks both
for backward compatibility, but on any modern cluster (including ODF 4.21), only
the pool-level flags matter.

### 3. Nearfull does NOT switch from buffered to sync writes

This is the most common misconception. The nearfull path does **not** reroute
data through `ceph_sync_write`. The data path decision is made **before** the
nearfull check, at line 2423:

```
fs/ceph/file.c:2423-2465

    if ((got & (CEPH_CAP_FILE_BUFFER|CEPH_CAP_FILE_LAZYIO)) == 0 ||
        (iocb->ki_flags & IOCB_DIRECT) || (fi->flags & CEPH_F_SYNC) ||
        (ci->i_ceph_flags & CEPH_I_ERROR_WRITE)) {
        /* ... sync write path ... */
        written = ceph_sync_write(iocb, &data, pos, snapc);
    } else {
        /* buffered: data goes into page cache */
        written = generic_perform_write(iocb, from);
    }
```

If the client holds the Fb (FILE\_BUFFER) cap and none of the other sync-forcing
conditions are true, data goes into the page cache via `generic_perform_write`
**regardless of nearfull**. The nearfull code then adds a post-write datasync
flush via `generic_write_sync`, which calls `vfs_fsync_range` → `ceph_fsync`
→ `file_write_and_wait_range`. This forces the dirty pages to be flushed
synchronously before the syscall returns, but the data did go through the page
cache and the writepages batching path — not through `ceph_sync_write`.

### 4. But for this workload, DSYNC converges to the same throughput as sync writes

For single-threaded FIO doing sequential 4 KiB writes:

- **DSYNC path:** write 4 KiB to page cache → `generic_write_sync` →
  `ceph_fsync(datasync=1)` → `file_write_and_wait_range` → triggers writeback
  of that single dirty page → one OSD WRITE request for 4 KiB → wait for OSD
  ack → return to userspace. Next write repeats. No page-cache batching occurs
  because the writer blocks on each flush.

- **Sync write path:** copy 4 KiB to temp pages → create OSD WRITE request →
  `ceph_osdc_start_request` + `ceph_osdc_wait_request` → wait for OSD ack →
  invalidate page cache → return. Next write repeats.

Both produce **one OSD round-trip per 4 KiB write**. At ~2–5 ms OSD latency,
that gives ~200–500 writes/s → **800 KiB/s to 2 MiB/s**, which brackets the
observed 200–300 KiB/s (the lower end accounting for Azure network jitter and
OSD load from the earlier disruption).

So even if nearfull **were** still set, it would produce the right symptom for
this workload. But it can't be the cause of a multi-hour stall because the flag
is transient. The real question is: what persistent mechanism forces per-write
OSD round-trips?

---

## Theory A: Lost Fb (CEPH\_CAP\_FILE\_BUFFER) capability — most likely

### The cap-based write path decision

The only thing that determines buffered vs sync write at line 2423 of `file.c`
is **what capabilities the client holds**. `CEPH_CAP_FILE_BUFFER` (Fb) is what
allows the client to buffer dirty data in its page cache. Without it, every
write goes through `ceph_sync_write`.

The client requests Fb as a "want" (not a hard "need"):

```
fs/ceph/file.c:2405-2410

    if (!(fi->flags & CEPH_F_SYNC) && !direct_lock)
        want |= CEPH_CAP_FILE_BUFFER;
    /* ... */
    err = ceph_get_caps(file, CEPH_CAP_FILE_WR, want, pos + count, &got);
```

Inside `try_get_cap_refs` (`caps.c:2876`), if the MDS hasn't granted Fb, the
client gets only what it needs (Fw) and proceeds silently:

```
fs/ceph/caps.c:2913-2916

    if ((have & want) == want)
        *got = need | (want & ~exclude);
    else
        *got = need;
```

There is no error, no warning, no log message. The fallback to sync writes is
a normal, silent code path. The client will keep requesting Fb on subsequent
writes, but if the MDS doesn't grant it, every write falls through to
`ceph_sync_write` indefinitely.

### Why MDS might withhold Fb after reconnect

When an MDS restarts or a client session reconnects, the cap state machine
goes through a specific recovery sequence. In `send_mds_reconnect`
(`mds_client.c:4980`), all caps are re-encoded and sent to the MDS. The cap
sequence numbers are reset:

```
fs/ceph/mds_client.c:4704-4750 (reconnect_caps_cb)

    cap->seq = 0;        /* reset cap seq */
    cap->issue_seq = 0;  /* and issue_seq */
    cap->mseq = 0;       /* and migrate_seq */
    cap->cap_gen = atomic_read(&cap->session->s_cap_gen);
```

The MDS then decides what to re-grant. If:

- The MDS is conservative after seeing a pool that was recently nearfull
- There is another client or MDS replica involved in cap contention
- The MDS locker state didn't fully recover after the disruption
- The MDS sees pending cap revocations from the pre-restart state

...it may issue only Fw (FILE\_WR) without Fb (FILE\_BUFFER). This is a valid
MDS decision — there's nothing "broken" about it from the MDS's perspective.
The client will write correctly, just slowly.

### Why mgr restart fixes it

A Ceph mgr restart causes an MDS map epoch bump. The CephFS client processes
new MDS maps in `ceph_mdsc_handle_mdsmap` (`mds_client.c:6746`), which calls
`check_new_map`. If the MDS transitions through states, the client may:

1. Trigger cap re-evaluation via `wake_up_session_caps`
2. Kick flushing caps via `ceph_kick_flushing_caps`
3. Cause the MDS to reassess cap distribution

Any of these can result in the MDS re-granting Fb, immediately restoring
buffered write throughput.

### What to look for diagnostically

During the stall, examining `/sys/kernel/debug/ceph/<fsid>/caps` would show
the held capabilities per inode. If the file being written shows `pAsLsXs`
(auth caps) with `Fw` but **not** `Fb`, that confirms this theory.

MDS debug logs (level 20) would show `handle_client_caps` messages with the
granted cap set. A grant of `pAsLsXsFw` without the `b` (buffer) bit is the
smoking gun.

---

## Theory B: Stale osdmap retaining POOL\_FLAG\_NEARFULL

### Mechanism

If the client's osdmap subscription was disrupted by the mon-b restart, the
client could be holding a stale osdmap where the pool still has
`CEPH_POOL_FLAG_NEARFULL` set. Every `write()` would then take the DSYNC path
at `file.c:2493`, producing the same per-write OSD flush behavior.

The pool flags are looked up via `ceph_pg_pool_flags`:

```
net/ceph/osdmap.c:775-781

    u64 ceph_pg_pool_flags(struct ceph_osdmap *map, u64 id)
    {
        struct ceph_pg_pool_info *pi;
        pi = lookup_pg_pool(&map->pg_pools, id);
        return pi ? pi->flags : 0;
    }
```

The osdmap is updated when the client receives new maps from the monitor.
If the monitor subscription is broken, the client keeps the last map it
received, which could still have the nearfull flag.

### Map subscription renewal

The OSD client tries to get new maps via `maybe_request_map`
(`osd_client.c:2357`):

```
net/ceph/osd_client.c:2364-2376

    if (ceph_osdmap_flag(osdc, CEPH_OSDMAP_FULL) ||
        ceph_osdmap_flag(osdc, CEPH_OSDMAP_PAUSERD) ||
        ceph_osdmap_flag(osdc, CEPH_OSDMAP_PAUSEWR)) {
        continuous = true;
    } else {
        /* one-shot subscription */
    }
```

Note: `CEPH_OSDMAP_NEARFULL` does **not** trigger continuous subscription.
Only cluster-level FULL, PAUSERD, and PAUSEWR do. So if the issue is a
per-pool nearfull flag on a stale map, the client won't aggressively request
new maps — it would rely on the normal periodic map check, which could be slow
to converge.

### Why this is less likely than Theory A

- A stale osdmap would also affect OSD request routing, which would likely
  cause other visible symptoms (misrouted ops, OSD log errors).
- The cluster was HEALTH\_OK, meaning monitors were functional and should
  have been pushing map updates.
- Pool nearfull flags are typically cleared quickly once usage drops, and the
  monitor would push that update to subscribed clients.

---

## Theory C: `CEPH_I_ERROR_WRITE` from writeback failures

### Mechanism

If any async writeback OSD request fails (e.g., during the OSD-1 down period),
`writepages_finish` sets a per-inode error flag:

```
fs/ceph/addr.c:891-898

    if (rc < 0) {
        mapping_set_error(mapping, rc);
        ceph_set_error_write(ci);
        if (rc == -EBLOCKLISTED)
            fsc->blocklisted = true;
    } else {
        ceph_clear_error_write(ci);
    }
```

This flag is checked in `ceph_write_iter` at line 2425 and forces the sync
write path. The flag is defined in `super.h:687`:

```
fs/ceph/super.h:710-726

    static inline void ceph_set_error_write(struct ceph_inode_info *ci)
    {
        if (!test_bit(CEPH_I_ERROR_WRITE_BIT, &ci->i_ceph_flags)) {
            spin_lock(&ci->i_ceph_lock);
            set_bit(CEPH_I_ERROR_WRITE_BIT, &ci->i_ceph_flags);
            spin_unlock(&ci->i_ceph_lock);
        }
    }
```

### Why this is unlikely as root cause

The flag is cleared on the **next successful write**, whether sync or async:

- `ceph_sync_write` (`file.c:2060`): `ceph_clear_error_write(ci)` after a
  successful OSD write.
- `writepages_finish` (`addr.c:897`): `ceph_clear_error_write(ci)` on `rc >= 0`.

So the sequence would be: writeback error → flag set → next write goes sync →
sync write succeeds → flag cleared → next write tries buffered again. The flag
is self-healing within one or two write calls.

The only way this becomes persistent is if **sync writes also keep failing**,
which would require an ongoing OSD problem — contradicting HEALTH\_OK.

This could be a **compounding factor** during the initial moments of the
disruption (when OSD-1 was actually down), but it cannot sustain a multi-hour
stall on a healthy cluster.

---

## Theory D: `i_max_size` starvation after MDS reconnect

### Mechanism

The MDS controls how far a client can write into a file through the
`i_max_size` field. In `try_get_cap_refs` (`caps.c:2857`):

```
fs/ceph/caps.c:2857-2863

    if (have & need & CEPH_CAP_FILE_WR) {
        if (endoff >= 0 && endoff > (loff_t)ci->i_max_size) {
            if (endoff > ci->i_requested_max_size)
                ret = ci->i_auth_cap ? -EFBIG : -EUCLEAN;
            goto out_unlock;
        }
    }
```

When `endoff > i_max_size`, the write blocks. The caller loop in
`__ceph_get_caps` (`caps.c:3128`) handles this:

```
fs/ceph/caps.c:3128-3131

    if (ret == -EFBIG) {
        check_max_size(inode, endoff);
        continue;
    }
```

`check_max_size` requests a larger allocation from the MDS via
`ceph_check_caps`, and the MDS responds with a new `max_size` in
`handle_cap_grant` (`caps.c:3667`):

```
fs/ceph/caps.c:3667-3677

    if (ci->i_auth_cap == cap && (newcaps & CEPH_CAP_ANY_FILE_WR)) {
        if (max_size != ci->i_max_size) {
            ci->i_max_size = max_size;
            if (max_size >= ci->i_wanted_max_size) {
                ci->i_wanted_max_size = 0;
                ci->i_requested_max_size = 0;
            }
            wake = true;
        }
    }
```

### The reconnect reset

During MDS reconnect, `wake_up_session_cb` resets the size request state:

```
fs/ceph/mds_client.c:1988-1992

    if (ev == RECONNECT) {
        spin_lock(&ci->i_ceph_lock);
        ci->i_wanted_max_size = 0;
        ci->i_requested_max_size = 0;
        spin_unlock(&ci->i_ceph_lock);
    }
```

If the MDS then grants a small initial `max_size`, the client would repeatedly
block on the `endoff > i_max_size` check, round-trip to the MDS for a larger
allocation, write a bit, hit the limit again, and repeat. This creates a
throttling pattern.

### Why this is less likely

This mechanism typically manifests as **periodic stalls** (write → block → MDS
grant → burst → block again) rather than a steady 200 KiB/s. The MDS normally
grants `max_size` in large increments (often 4 MiB or more), so the writer
would see bursts followed by pauses, not a uniform low throughput. However, if
the MDS is granting very small increments due to its own post-restart
conservatism, this could compound with Theory A.

---

## Theory E: Pool FULL flag stuck in osdmap

### Mechanism

If the pool's `CEPH_POOL_FLAG_FULL` bit remained set in the client's osdmap,
writes would be rejected with `-ENOSPC` at `file.c:2392`:

```
fs/ceph/file.c:2392-2396

    if ((map_flags & CEPH_OSDMAP_FULL) ||
        (pool_flags & CEPH_POOL_FLAG_FULL)) {
        err = -ENOSPC;
        goto out;
    }
```

At the libceph layer, OSD write requests would also be paused:

```
net/ceph/osd_client.c:1524-1528

    bool pausewr = ceph_osdmap_flag(osdc, CEPH_OSDMAP_PAUSEWR) ||
                   ceph_osdmap_flag(osdc, CEPH_OSDMAP_FULL) ||
                   __pool_full(pi);
    return ((t->flags & CEPH_OSD_FLAG_WRITE) && pausewr) || ...
```

### Why this is unlikely

A FULL flag would cause **zero writes** reaching the OSD, not 200 KiB/s. The
CephFS client would fail writes with ENOSPC (or the libceph layer would pause
them in the queue). FIO would report errors or stall completely, not show steady
slow progress.

---

## Appendix: Key code paths reference

| Component | File | Key lines | Purpose |
|-----------|------|-----------|---------|
| Nearfull flag check | `fs/ceph/file.c` | 2388–2396, 2492–2497 | Read osdmap; FULL → ENOSPC, NEARFULL → IOCB\_DSYNC |
| Buffered vs sync decision | `fs/ceph/file.c` | 2423–2465 | Fb cap present → `generic_perform_write`; absent → `ceph_sync_write` |
| Cap acquisition | `fs/ceph/caps.c` | 2819–2972 | `try_get_cap_refs`: grants `need` + optionally `want` based on held caps |
| Cap grant from MDS | `fs/ceph/caps.c` | 3630–3745 | `handle_cap_grant`: updates `i_max_size`, issued caps, queues writeback on revoke |
| Reconnect cap re-encode | `fs/ceph/mds_client.c` | 4704–4750 | `reconnect_caps_cb`: resets seq/issue\_seq/mseq to 0 |
| Reconnect max\_size reset | `fs/ceph/mds_client.c` | 1988–1992 | `wake_up_session_cb(RECONNECT)`: zeros `i_wanted_max_size` / `i_requested_max_size` |
| MDS map handling | `fs/ceph/mds_client.c` | 5568–5673, 6746–6799 | `check_new_map` / `ceph_mdsc_handle_mdsmap`: triggers reconnect, kicks caps |
| Error write flag | `fs/ceph/super.h` | 710–726 | `ceph_set_error_write` / `ceph_clear_error_write` |
| Writeback error handling | `fs/ceph/addr.c` | 891–898 | `writepages_finish`: sets/clears `CEPH_I_ERROR_WRITE` |
| OSD full/pause logic | `net/ceph/osd_client.c` | 1517–1534, 2404–2443 | `target_should_be_paused`, `__submit_request` |
| Pool flag definitions | `include/linux/ceph/osdmap.h` | 37–42 | `CEPH_POOL_FLAG_FULL`, `CEPH_POOL_FLAG_NEARFULL` |
| Osdmap flag definitions | `include/linux/ceph/rados.h` | 143–152 | Legacy `CEPH_OSDMAP_NEARFULL/FULL` ("not set since ~luminous") |
| Pool flag lookup | `net/ceph/osdmap.c` | 775–781 | `ceph_pg_pool_flags` |
| Congestion control | `fs/ceph/addr.c` | 62–65, 777–862 | `CONGESTION_ON_THRESH` / `writeback_count` / `write_congested` |
