# Independent Analysis: CephFS Nearfull Write Stall (DFBUGS-5893)

See [jira.md](jira.md) for the original bug report and [opus_analysis.md](opus_analysis.md)
for the prior code review. This document adds independent verification, additional
angles, and a refined diagnosis.

All code references are against `/mnt/attached/linux` (kernel 5.15-based, ODF 4.21 era).

---

## Agreement with prior analysis

The prior analysis in `opus_analysis.md` is fundamentally sound. I agree that:

1. **Nearfull is almost certainly NOT the root cause.** The check at `file.c:2388-2497`
   is stateless and per-syscall. There is no "nearfull mode" that latches. The flags
   are read from the live osdmap on each `write()` call and cannot persist after the
   pool drops below the nearfull threshold.

2. **Theory A (lost Fb cap) is the best fit** for the observed symptoms.

3. **Theory C (CEPH_I_ERROR_WRITE) is self-healing** and cannot sustain a multi-hour stall.

4. **Theory E (FULL flag) would produce zero throughput**, not 200 KiB/s.

However, there are several additional mechanisms and one underexplored compounding
scenario worth documenting.

---

## Additional Analysis

### F. Cap staleness via `s_cap_gen` — a stronger variant of Theory A

The prior analysis focuses on the MDS choosing not to re-grant Fb. But there is a
more mechanical path that guarantees cap loss without any MDS-side decision:

**The `s_cap_gen` bump during reconnect.**

At `mds_client.c:5047`, when `send_mds_reconnect` begins:

```c
atomic_inc(&session->s_cap_gen);
```

This immediately invalidates ALL existing caps for that session. From this point,
`__cap_is_valid()` (`caps.c:788-806`) returns 0 for any cap whose `cap_gen` is
less than the new `s_cap_gen`:

```c
if (cap->cap_gen < gen || time_after_eq(jiffies, ttl)) {
    // STALE
    return 0;
}
```

Since `__ceph_caps_issued()` (`caps.c:813`) skips stale caps, the client
effectively holds **zero valid caps** until the MDS responds to the reconnect
and `handle_cap_grant()` updates `cap->cap_gen` at `caps.c:3570`:

```c
cap->cap_gen = atomic_read(&session->s_cap_gen);
```

The critical question is: **what does the MDS grant in the reconnect response?**

The reconnect protocol re-establishes caps, but the MDS may not immediately
re-grant Fb if:
- The MDS's internal locker hasn't fully evaluated the client's cap wants
- There are pending cap revocations from the pre-disruption state
- The inode is in a transitional state during recovery

The same `s_cap_gen` bump happens on STALE session at `mds_client.c:4456`:

```c
case CEPH_SESSION_STALE:
    atomic_inc(&session->s_cap_gen);
    session->s_cap_ttl = jiffies - 1;
    send_renew_caps(mdsc, session);
```

If the session went STALE during the disruption (which is likely given mon-b
and OSD-1 restarted), caps would be invalidated. The renewal path
(`send_renew_caps`) requests fresh caps, but the MDS's response determines
what gets granted. There is a window where caps are stale and any write
falls through to `ceph_sync_write`.

The important detail is that in `wake_up_session_cb` with `RENEWCAPS`
(`mds_client.c:1993-2001`):

```c
} else if (ev == RENEWCAPS) {
    cap = __get_cap_for_mds(ci, mds);
    /* mds did not re-issue stale cap */
    if (cap && cap->cap_gen < atomic_read(&cap->session->s_cap_gen))
        cap->issued = cap->implemented = CEPH_CAP_PIN;
}
```

If the MDS does not re-issue the cap after renewal, the cap is **explicitly
demoted to PIN only**. PIN allows metadata operations but NOT data operations.
The client would then need to re-acquire caps through `ceph_renew_caps()`
(`file.c:300`), which issues a LOOKUP request to the MDS. This round-trip
adds latency but should eventually restore caps.

**The failure scenario**: if the MDS's renewal response arrives but doesn't
include Fb (only Fw), `handle_cap_grant` updates `cap_gen` (making the cap
"valid" again) but with only Fw granted. The client silently falls through
to `ceph_sync_write` on every subsequent `write()`. There is no timeout,
no retry, and no log message for this condition. The client will request
Fb as a "want" on each write, but `try_get_cap_refs` at `caps.c:2913-2916`
just gives what's available:

```c
if ((have & want) == want)
    *got = need | (want & ~exclude);
else
    *got = need;  // Fb not granted, proceed with only Fw
```

This is the same conclusion as Theory A, but the path through `s_cap_gen`
bump + STALE session provides a concrete, mechanical explanation for HOW
the Fb cap is lost — it's not just the MDS being "conservative," it's the
explicit invalidation + renewal protocol.

---

### G. Writeback congestion flag as compounding factor

There is a subtle issue in the congestion tracking that could compound with
the primary cap-loss theory.

At `addr.c:1632`:
```c
if (wbc->sync_mode == WB_SYNC_NONE && fsc->write_congested)
    return 0;
```

When `write_congested` is true, **all background (WB_SYNC_NONE) writeback
is suppressed**. Only explicit `fsync`/`WB_SYNC_ALL` writeback proceeds.

The congestion flag is set when `writeback_count` exceeds a threshold
(`addr.c:777-779`) and cleared when it drops below the off-threshold
(`addr.c:860-862`, `addr.c:935-938`).

During the disruption, if OSD writes were slow or failing, `writeback_count`
would have climbed (pages in writeback waiting for OSD acks) and set
`write_congested = true`. The flag only clears when writeback completions
bring the count back down. If the cluster was slow to process the backlog,
the congestion flag could persist for some time.

However, this is primarily a compounding factor, not a root cause:
- If the client has Fb and is doing buffered writes, congestion only
  affects background flush timing, not the write() syscall itself
- If the client lacks Fb and is doing sync writes, congestion is irrelevant
  because sync writes bypass the writeback machinery entirely

The scenario where this matters: if Fb was partially restored (e.g., for
some inodes but not others), congestion could throttle the buffered
writeback path for those inodes that do have Fb.

---

### H. The `filp_gen` invalidation — unlikely but worth ruling out

At `super.c:1013`, during forced umount:
```c
fsc->filp_gen++; // invalidate open files
```

And in the cap acquisition path at `caps.c:3115-3119`:
```c
if (fi && (fi->fmode & CEPH_FILE_MODE_WR) &&
    fi->filp_gen != READ_ONCE(fsc->filp_gen)) {
    if (ret >= 0 && _got)
        ceph_put_cap_refs(ci, _got);
    return -EBADF;
}
```

If `filp_gen` was somehow bumped (only happens on forced umount via
`__ceph_umount_begin`), all open file handles would get EBADF on every
cap acquisition. FIO would report errors, not slow writes.

**Verdict: not applicable here** — forced umount didn't happen, but
documenting it to distinguish from the cap-loss scenarios.

---

### I. Osdmap subscription gap — a concrete path for Theory B

The prior analysis correctly identifies the stale-osdmap theory (B) but
dismisses it somewhat quickly. Here's a more concrete scenario:

The osdmap subscription is a one-shot subscription for most conditions.
At `osd_client.c:2364-2376`, continuous subscription is only triggered
by FULL, PAUSERD, or PAUSEWR — **not by NEARFULL**. After the mon-b
restart:

1. Client's osdmap has pool-level NEARFULL flag set (from the earlier
   RBD fill test)
2. Mon-b restarts, possibly causing a brief subscription gap
3. The pool's NEARFULL flag is cleared in the cluster
4. Client doesn't receive the updated map because its subscription
   expired and it has no reason to request a new one (NEARFULL doesn't
   trigger continuous subscription)
5. Every `write()` sees the stale NEARFULL flag and adds IOCB_DSYNC

The missing piece: **when does the client request a new osdmap?**

The client requests new maps when:
- An OSD request is sent and the OSD responds with a newer map epoch
- The monitor pushes a map update (requires active subscription)
- The client explicitly requests via `maybe_request_map`

In scenario (5), the sync writes DO go to OSDs and complete successfully.
The OSD response includes the OSD's current map epoch. If the OSD's epoch
is newer than the client's, `ceph_osdc_handle_map` is triggered by the
OSD's response, which should deliver the updated map.

**This is the key rebuttal**: every successful OSD write response carries
epoch information. If the client is doing 200+ OSD writes per second,
it would receive updated map information very quickly. The stale-osdmap
theory requires the OSD responses to somehow NOT trigger a map update,
which is hard to sustain for hours.

**Verdict: Theory B is unlikely for a multi-hour stall**, but could
explain the initial minutes of the disruption. The map should self-correct
within seconds once OSD communication resumes.

---

## Refined Diagnosis

### Most likely sequence of events

1. **RBD fill test** causes pool to approach nearfull. OSD-1 and mon-b
   restart as part of or consequence of this test.

2. **MDS session disruption** — the OSD/mon restarts trigger MDS
   instability. The client's MDS session either:
   - Goes STALE (MDS sends CEPH_SESSION_STALE) — bumps `s_cap_gen`
   - Goes through full reconnect — bumps `s_cap_gen` and resets cap
     sequences

3. **Cap invalidation** — all caps become stale via `s_cap_gen` bump.
   The client re-acquires caps through reconnect or renewal.

4. **MDS re-grants Fw without Fb** — the MDS issues `pAsLsXsFw` but
   withholds `Fb`. This is a valid MDS decision, especially if:
   - The MDS locker sees recent nearfull state in the pool
   - The MDS is being conservative during recovery
   - There's internal cap contention state from the disruption

5. **Every write falls to `ceph_sync_write`** — the check at
   `file.c:2423` sees no Fb in `got`, routes through sync path.
   At 4 KiB per write with ~5-10ms OSD latency: 100-200 writes/s
   = 400 KiB/s to 800 KiB/s. With Azure network overhead and
   post-disruption OSD load, 200-300 KiB/s is expected.

6. **Stall persists** — the client keeps requesting Fb as a "want"
   on each write, but `try_get_cap_refs` silently returns only Fw.
   No log message, no error, no timeout. This can persist indefinitely.

7. **Mgr restart breaks the deadlock** — mgr restart causes MDS map
   epoch bump. `check_new_map` triggers cap re-evaluation. The MDS
   re-grants Fb, restoring buffered writes and ~300 MiB/s throughput.

### What to verify

If the issue recurs, capture these diagnostics **during the stall**:

1. **Caps held on the file being written:**
   ```
   cat /sys/kernel/debug/ceph/<fsid>/caps
   ```
   Look for the target inode. If it shows `Fw` but not `Fb` — confirmed.

2. **MDS session state:**
   ```
   cat /sys/kernel/debug/ceph/<fsid>/mdsc
   ```
   Check for STALE or RECONNECTING sessions.

3. **Osdmap pool flags:**
   ```
   cat /sys/kernel/debug/ceph/<fsid>/osdmap
   ```
   Verify pool NEARFULL flag is cleared.

4. **Kernel dmesg with dynamic debug:**
   ```
   echo 'file fs/ceph/file.c +p' > /sys/kernel/debug/dynamic_debug/control
   echo 'file fs/ceph/caps.c +p' > /sys/kernel/debug/dynamic_debug/control
   ```
   The `doutc` at `file.c:2420-2421` will print exactly which caps were
   obtained: `"got cap refs on %s"`. If it says `Fw` without `Fb`, that's
   the smoking gun.

5. **MDS admin socket (server-side):**
   ```
   ceph daemon mds.<id> session ls
   ```
   Check the `granted` caps for the client's session — compare what the
   MDS thinks it granted vs. what the client reports holding.

### Suggested fix / mitigation

**Short-term**: if the stall occurs again, instead of restarting the mgr,
try forcing cap re-evaluation from the client side:

```
# Force a cap check on the affected session
echo 1 > /sys/kernel/debug/ceph/<fsid>/reset_session_<mds_id>
```

Or from the MDS side:
```
ceph tell mds.<id> client reconnect <client_id>
```

**Long-term**: the kernel client should log a warning when it repeatedly
fails to obtain Fb for a file that is being actively written. Something
like:

```c
// In ceph_write_iter, after ceph_get_caps:
if (!(got & CEPH_CAP_FILE_BUFFER) && !(fi->flags & CEPH_F_SYNC) &&
    !(iocb->ki_flags & IOCB_DIRECT)) {
    pr_warn_ratelimited("ceph: %llx.%llx write falling back to sync "
                        "(no Fb cap, got %s)\n",
                        ceph_vinop(inode), ceph_cap_string(got));
}
```

This would make the silent fallback visible in dmesg, dramatically
reducing future debugging time for this class of issue.
