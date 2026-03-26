CephFS writes got stuck at ~200–300 KiB/s for hours after a disruption, even though the cluster was HEALTH_OK and later proved capable of ~300 MiB/s; the suspicion is a CephFS client-side writeback stall triggered when the pool got nearfull and not returning to normal behavior until the mgr restarted. [redhat.atlassian](https://redhat.atlassian.net/browse/DFBUGS-5893?visitedUserSeg=true)

### TL;DR of the issue

- Environment: ODF 4.21 on Azure IPI, 3× workers (D16s_v3), 3× 512 GiB OSDs (1.5 TiB raw), CephFS RWO PVC. [redhat.atlassian](https://redhat.atlassian.net/browse/DFBUGS-5893?visitedUserSeg=true)
- Workload: FIO, sequential 4 KiB buffered writes, 407 GiB target on CephFS. [redhat.atlassian](https://redhat.atlassian.net/browse/DFBUGS-5893?visitedUserSeg=true)
- Symptom: For ~3h52m, Ceph cluster shows only ~14 GiB data, with write rate ~200–300 KiB/s, while HEALTH_OK and ~97% free capacity. [redhat.atlassian](https://redhat.atlassian.net/browse/DFBUGS-5893?visitedUserSeg=true)
- After FIO timeout, the same data flushed at ~300+ MiB/s and cluster data jumped from 14 GiB to ~370 GiB after a mgr restart, showing hardware and backend can do normal throughput. [redhat.atlassian](https://redhat.atlassian.net/browse/DFBUGS-5893?visitedUserSeg=true)
- There had been earlier disruption (RBD fill test causing OSD-1, mon-b, rook-ceph-operator restarts) shortly before this CephFS test, so the theory is: some CephFS client/MDS writeback path got stuck in a degraded mode after the nearfull/OSD events. [redhat.atlassian](https://redhat.atlassian.net/browse/DFBUGS-5893?visitedUserSeg=true)

### Venky’s comment explained

Venky is pointing at a known CephFS kernel client behavior:  
- When the backing pool hits the nearfull ratio, the CephFS kernel client switches from buffered writes to synchronous I/O to avoid overfilling and to respect backpressure from the cluster. [redhat.atlassian](https://redhat.atlassian.net/browse/DFBUGS-5893?visitedUserSeg=true)
- Once space is **again** “aplenty” (i.e., no longer nearfull and plenty of free capacity), the client is supposed to switch back to buffered I/O, restoring normal high throughput. [redhat.atlassian](https://redhat.atlassian.net/browse/DFBUGS-5893?visitedUserSeg=true)

In this bug, the pool is no longer close to full (only ~3% used), yet the observed behavior (200 KiB/s, effectively sync-like throughput) looks like the client never switched back to buffered mode. Venky is essentially saying:  
- “What you’re seeing matches the ‘sync IO when nearfull’ mechanism, but the client should have reverted to buffered once capacity returned. That apparently did not happen, which is why this is being handed to you to debug as a CephFS/kclient issue.” [redhat.atlassian](https://redhat.atlassian.net/browse/DFBUGS-5893?visitedUserSeg=true)

Do you want help drafting a comment back on the bug (e.g., hypotheses, next debug steps, or repro tweaks)?
