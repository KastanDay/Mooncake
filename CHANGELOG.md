# Changelog

Cloudflare's Mooncake wheels, built from this branch (`kastan/wheel`: an upstream
release plus Cloudflare's patches) and published to the internal Python registry,
python.cfdata.org. Newest first. Each entry is a GitLab release tagged
`wheel-<version>` on the wheels' source commit; the version is the build's UTC
start time, `YY.MDD.HMMSS`.

## wheel-26.1008.211229 (2026-10-08)

Master: keep client liveness independent of metadata cleanup.

On eu-west1 (2026-10-08, about 13.8M keys), one elastic Store's disk unmount
flushed the whole cache:

1. `UnmountLocalDiskSegment` walked every metadata shard inside its RPC handler
   for 58 s. Synchronous coro_rpc handlers run on their connection's IO thread,
   so that walk held the thread and starved every connection sharing it.
2. A client on that thread missed its Pings and expired.
3. Its expiry swept the whole index again on the client-monitor thread, holding
   `snapshot_mutex_` shared throughout.
4. The expired, still-alive client remounted. `ReMountSegment` took
   `client_mutex_` exclusively and then waited for `snapshot_mutex_`.
5. Every Ping, and every Put/Upsert/batch call, needed `client_mutex_` shared,
   so they all waited behind that remount and all 75 clients expired at once.
6. The mass expiry then erased all 13.8M keys while holding shard locks, for
   about 2 minutes.

The defect was that liveness depended on O(total keys) metadata cleanup, run
inline on threads and under locks that liveness also needed. At 1 PiB of SSD
(0.5 to 1 billion keys), one Store crash would have frozen the Master for tens
of minutes.

The change is Master only: no client, protocol or flag change, and the 10-s
client TTL stays. Three mechanisms:

- **Pings never wait.**
  - `Ping` records receipt in a liveness table under a leaf mutex. Nothing
    holds that mutex while waiting for another lock.
  - The table is the only record of a client's status; `ok_client_` is gone.
  - The monitor reads the table and revalidates each expiry under the locks:
    a client that pinged since it was selected stays.
  - Lock order is `snapshot_mutex_`, then `client_mutex_`, then the leaf.
    Remount takes the snapshot lock first, so nothing holds `client_mutex_`
    while it waits. The data paths no longer take `client_mutex_` at all.
- **A replica's validity is checked in O(1) when it is read, and is
  monotone.**
  - Each LOCAL_DISK replica is bound to the disk-registration generation it
    was admitted under. It is servable only while that generation is current.
  - Generations are never reused, and 0 (bound to none) is never current. So
    once a replica is invalid it stays invalid: a later registration under
    the same client id cannot resurrect it.
  - A snapshot restore binds each restored replica to its owner's restored
    registration and credits its bytes there, as a live admission does.
  - An owner that reports a key again under a new registration rebinds its
    completed replica in place (HA keeps the old replica until its removal is
    durable; one already marked for removal is left to go).
  - A promoted HA standby has no SSD registry to bind against, so its disk
    replicas serve nothing until each owner re-registers and rescans. That is
    the price of monotonicity, in a mode eu-west1 does not run.
  - Used bytes are credited and debited per registration.
- **Cleanup is background garbage collection.**
  - Disk unmount and client expiry retire the registration or segment in
    O(that client's segments) and schedule the coalescing cleanup worker.
    Every expiry schedules one.
  - The worker visits at most about 256 keys per lock hold, taking
    `snapshot_mutex_` shared and one shard lock per batch.
  - It resumes from a bucket cursor and restarts a tenant if its map was
    rehashed, which is rare.
  - It steps aside whenever an exclusive writer, such as a remount or a disk
    unmount, is waiting.
  - HA, snapshot and CXL modes keep their synchronous sweeps, with the same
    predicate.

- **No RPC handler does unbounded eviction, and no long pass holds the
  snapshot lock.** A live A/B on eu-west1 (23M keys, 76 clients) found the
  same failure through a second path:
  - After a capacity drop, a tenant's quota collapses below what it holds.
    Its next Put evicted the whole shortfall inline, on its RPC IO thread:
    519k objects over 33 s.
  - `BatchEvict` held `snapshot_mutex_` shared for its whole run, so remounts
    and disk mounts waited out the run on their IO threads.
  - Crashing all three test Stores at once expired all 76 clients.

  Now:
  - A write's inline eviction examines at most 4,096 keys. If that is not
    enough, the write is refused (`quota_trimming`) and its demand goes to a
    background quota-trim worker. The worker also trims every tenant down to
    its quota whenever capacity or a policy changes.
  - `BatchEvict`, the quota eviction and the cleanup pass take the snapshot
    lock per shard section, and step aside while a writer waits. HA, snapshot
    and CXL modes keep their whole-run holds, so a snapshot never lands
    mid-eviction there.
  - Tenants without an explicit policy (orphan state) are never trimmed, as
    before.
  - The quota eviction walks each shard from a random bucket. Evicted keys
    that keep a disk replica stay in place, so a walk from the start
    re-examined a growing run of keys that free nothing on every write. In
    the A/B that refused 29k writes during a disk-heavy spill.
  - A slow inline eviction logs `inline_quota_eviction_slow`.

Also:
- Only a servable disk replica counts as an eviction backup or a promotion
  source, and a promotion task reaches only its registration's mailbox.
- Admission re-checks the generation under the shard lock, so a replica racing
  a retirement is refused rather than published behind the cleanup pass.
- The cleanup plan builds surviving descriptors only when the HA oplog needs
  them, and the `get_descriptor` error is rate-limited (27,432 lines in one
  minute during the incident, under shard locks).

Reviews:
- a critical design review and two implementation reviews by Codex
  (GPT-6-astra);
- a design review and implementation audit by Claude (Fable 5.1). Its
  simplifications went in: monotone generations replace a special sweep for
  restored replicas, and there is one record of client status.

Tests: new `master_liveness_isolation_test` (24 tests and a benchmark). It
covers each link of the incident chain, and these regressions:
- re-registration and restore;
- admission racing retirement;
- eviction and promotion against ended registrations;
- the offload-disabled path;
- rehash and yield in the cleanup pass.
Each test fails without the change it covers.

The existing Master suites pass. Three tests encoded the old semantics and
were adapted:
- one expected a never-remounted owner's disk replica to be stale;
- one called the removed owner-targeted sweep;
- the manager tests now name the registration they credit.

What this does not fix: handlers still run on the RPC IO threads that also
carry Pings. Every known long hold is now bounded, but a new unbounded handler
would starve its thread's clients again. `GetReplicaListByRegex`,
`RemoveByRegex` and `RemoveAll` still scan the whole index inline; do not call
them on a large index.

Deferred, as gates before the index grows well past today's:
- moving blocking handlers off the IO threads (defence in depth);
- an owner-to-replica index, so reclamation is targeted (today a pass costs
  about 10 s at 14M keys and grows linearly);
- a lock-free published generation map for the read path;
- reclaiming a hot key's dead disk replica when it is accessed;
- parallel quota trimming across shards, if a trim after a large capacity drop takes minutes;
- recoverable client suspicion.

### Artifacts

Version `26.1008.211229` (built 2026-10-08 21:12:29 UTC) from `317da53980cfe7b4b81f76c9fce3e92e422cf5d2` on `kastan/wheel`.
Builder images: non-cuda: `pytorch/manylinux2_28-builder:cuda12.8`. CPython 3.12, x86_64.

| Variant | File | sha256 | Registry |
|---|---|---|---|
| non-cuda | `mooncake_transfer_engine_non_cuda-26.1008.211229-cp312-cp312-manylinux_2_28_x86_64.whl` | `1c8e7682fba49f29659e71d98f0804373b6ef82cbe20ade9ebe0a2ca28fd80d2` | [https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/…](https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1008.211229-cp312-cp312-manylinux_2_28_x86_64.whl) |

Pin (mooncake-helm `mooncake-shared-cache` values; pods get `PYTHON_REGISTRY` from
the `cf-python-registry` Secret):

```yaml
  master.package: "${PYTHON_REGISTRY}/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1008.211229-cp312-cp312-manylinux_2_28_x86_64.whl#sha256=1c8e7682fba49f29659e71d98f0804373b6ef82cbe20ade9ebe0a2ca28fd80d2"
```

### Commits since v0.3.13.post1

- `431e70ef` [Store] Unregister a segment's memory when its mount fails (Kastan Day)
- `57d1b314` Add additive successful-read source receipts to Store clients (Kastan Day)
- `598eeac7` Distinguish observed replica eviction from surviving servable metadata (Kastan Day)
- `1f4ce545` Record wheel-26.1006.215531 in the changelog (Kastan Day)
- `5c120b67` [Store] Keep Master client liveness independent of metadata cleanup (Kastan Day)
- `28edeb72` [Store] Close the gaps an implementation review found (Kastan Day)
- `7d7cd076` [Store] Fence promotion enqueue and expiry's deadline cleanup (Kastan Day)
- `ad44146a` Record wheel-26.1008.43949 in the changelog (Kastan Day)
- `294ed5a4` [Store] Make disk-replica validity monotone; one record of client status (Kastan Day)
- `8f86e3ea` [Store] Rebind only a completed disk replica on re-adoption (Kastan Day)
- `5644d1f9` Record wheel-26.1008.65205 in the changelog (Kastan Day)
- `7a5f072e` [Store] Bound inline quota eviction; no eviction pass holds the snapshot lock (Kastan Day)
- `d208dfb3` Record wheel-26.1008.202747 in the changelog (Kastan Day)
- `1e3e5fe1` [Store] Walk quota eviction from a random bucket; quiet routine trims (Kastan Day)
- `317da539` [Store] Format the quota eviction walk (Kastan Day)

## wheel-26.1008.202747 (2026-10-08)

Master: keep client liveness independent of metadata cleanup.

On eu-west1 (2026-10-08, about 13.8M keys), one elastic Store's disk unmount
flushed the whole cache:

1. `UnmountLocalDiskSegment` walked every metadata shard inside its RPC handler
   for 58 s. Synchronous coro_rpc handlers run on their connection's IO thread,
   so that walk held the thread and starved every connection sharing it.
2. A client on that thread missed its Pings and expired.
3. Its expiry swept the whole index again on the client-monitor thread, holding
   `snapshot_mutex_` shared throughout.
4. The expired, still-alive client remounted. `ReMountSegment` took
   `client_mutex_` exclusively and then waited for `snapshot_mutex_`.
5. Every Ping, and every Put/Upsert/batch call, needed `client_mutex_` shared,
   so they all waited behind that remount and all 75 clients expired at once.
6. The mass expiry then erased all 13.8M keys while holding shard locks, for
   about 2 minutes.

The defect was that liveness depended on O(total keys) metadata cleanup, run
inline on threads and under locks that liveness also needed. At 1 PiB of SSD
(0.5 to 1 billion keys), one Store crash would have frozen the Master for tens
of minutes.

The change is Master only: no client, protocol or flag change, and the 10-s
client TTL stays. Three mechanisms:

- **Pings never wait.**
  - `Ping` records receipt in a liveness table under a leaf mutex. Nothing
    holds that mutex while waiting for another lock.
  - The table is the only record of a client's status; `ok_client_` is gone.
  - The monitor reads the table and revalidates each expiry under the locks:
    a client that pinged since it was selected stays.
  - Lock order is `snapshot_mutex_`, then `client_mutex_`, then the leaf.
    Remount takes the snapshot lock first, so nothing holds `client_mutex_`
    while it waits. The data paths no longer take `client_mutex_` at all.
- **A replica's validity is checked in O(1) when it is read, and is
  monotone.**
  - Each LOCAL_DISK replica is bound to the disk-registration generation it
    was admitted under. It is servable only while that generation is current.
  - Generations are never reused, and 0 (bound to none) is never current. So
    once a replica is invalid it stays invalid: a later registration under
    the same client id cannot resurrect it.
  - A snapshot restore binds each restored replica to its owner's restored
    registration and credits its bytes there, as a live admission does.
  - An owner that reports a key again under a new registration rebinds its
    completed replica in place (HA keeps the old replica until its removal is
    durable; one already marked for removal is left to go).
  - A promoted HA standby has no SSD registry to bind against, so its disk
    replicas serve nothing until each owner re-registers and rescans. That is
    the price of monotonicity, in a mode eu-west1 does not run.
  - Used bytes are credited and debited per registration.
- **Cleanup is background garbage collection.**
  - Disk unmount and client expiry retire the registration or segment in
    O(that client's segments) and schedule the coalescing cleanup worker.
    Every expiry schedules one.
  - The worker visits at most about 256 keys per lock hold, taking
    `snapshot_mutex_` shared and one shard lock per batch.
  - It resumes from a bucket cursor and restarts a tenant if its map was
    rehashed, which is rare.
  - It steps aside whenever an exclusive writer, such as a remount or a disk
    unmount, is waiting.
  - HA, snapshot and CXL modes keep their synchronous sweeps, with the same
    predicate.

- **No RPC handler does unbounded eviction, and no long pass holds the
  snapshot lock.** A live A/B on eu-west1 (23M keys, 76 clients) found the
  same failure through a second path:
  - After a capacity drop, a tenant's quota collapses below what it holds.
    Its next Put evicted the whole shortfall inline, on its RPC IO thread:
    519k objects over 33 s.
  - `BatchEvict` held `snapshot_mutex_` shared for its whole run, so remounts
    and disk mounts waited out the run on their IO threads.
  - Crashing all three test Stores at once expired all 76 clients.

  Now:
  - A write's inline eviction examines at most 4,096 keys. If that is not
    enough, the write is refused (`quota_trimming`) and its demand goes to a
    background quota-trim worker. The worker also trims every tenant down to
    its quota whenever capacity or a policy changes.
  - `BatchEvict`, the quota eviction and the cleanup pass take the snapshot
    lock per shard section, and step aside while a writer waits. HA, snapshot
    and CXL modes keep their whole-run holds, so a snapshot never lands
    mid-eviction there.
  - Tenants without an explicit policy (orphan state) are never trimmed, as
    before.
  - A slow inline eviction logs `inline_quota_eviction_slow`.

Also:
- Only a servable disk replica counts as an eviction backup or a promotion
  source, and a promotion task reaches only its registration's mailbox.
- Admission re-checks the generation under the shard lock, so a replica racing
  a retirement is refused rather than published behind the cleanup pass.
- The cleanup plan builds surviving descriptors only when the HA oplog needs
  them, and the `get_descriptor` error is rate-limited (27,432 lines in one
  minute during the incident, under shard locks).

Reviews:
- a critical design review and two implementation reviews by Codex
  (GPT-6-astra);
- a design review and implementation audit by Claude (Fable 5.1). Its
  simplifications went in: monotone generations replace a special sweep for
  restored replicas, and there is one record of client status.

Tests: new `master_liveness_isolation_test` (24 tests and a benchmark). It
covers each link of the incident chain, and these regressions:
- re-registration and restore;
- admission racing retirement;
- eviction and promotion against ended registrations;
- the offload-disabled path;
- rehash and yield in the cleanup pass.
Each test fails without the change it covers.

The existing Master suites pass. Three tests encoded the old semantics and
were adapted:
- one expected a never-remounted owner's disk replica to be stale;
- one called the removed owner-targeted sweep;
- the manager tests now name the registration they credit.

What this does not fix: handlers still run on the RPC IO threads that also
carry Pings. Every known long hold is now bounded, but a new unbounded handler
would starve its thread's clients again. `GetReplicaListByRegex`,
`RemoveByRegex` and `RemoveAll` still scan the whole index inline; do not call
them on a large index.

Deferred, as gates before the index grows well past today's:
- moving blocking handlers off the IO threads (defence in depth);
- an owner-to-replica index, so reclamation is targeted (today a pass costs
  about 10 s at 14M keys and grows linearly);
- a lock-free published generation map for the read path;
- reclaiming a hot key's dead disk replica when it is accessed;
- parallel quota trimming across shards, if a trim after a large capacity drop takes minutes;
- recoverable client suspicion.

### Artifacts

Version `26.1008.202747` (built 2026-10-08 20:27:47 UTC) from `7a5f072e6913c7643c8723e41aa0a8fcb1fdd0bb` on `kastan/wheel`.
Builder images: non-cuda: `pytorch/manylinux2_28-builder:cuda12.8`. CPython 3.12, x86_64.

| Variant | File | sha256 | Registry |
|---|---|---|---|
| non-cuda | `mooncake_transfer_engine_non_cuda-26.1008.202747-cp312-cp312-manylinux_2_28_x86_64.whl` | `fd596b3db3d3953f816887eee71abe1d55c5b8008623f3a221b0a2ae90cc15cf` | [https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/…](https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1008.202747-cp312-cp312-manylinux_2_28_x86_64.whl) |

Pin (mooncake-helm `mooncake-shared-cache` values; pods get `PYTHON_REGISTRY` from
the `cf-python-registry` Secret):

```yaml
  master.package: "${PYTHON_REGISTRY}/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1008.202747-cp312-cp312-manylinux_2_28_x86_64.whl#sha256=fd596b3db3d3953f816887eee71abe1d55c5b8008623f3a221b0a2ae90cc15cf"
```

### Commits since v0.3.13.post1

- `431e70ef` [Store] Unregister a segment's memory when its mount fails (Kastan Day)
- `57d1b314` Add additive successful-read source receipts to Store clients (Kastan Day)
- `598eeac7` Distinguish observed replica eviction from surviving servable metadata (Kastan Day)
- `1f4ce545` Record wheel-26.1006.215531 in the changelog (Kastan Day)
- `5c120b67` [Store] Keep Master client liveness independent of metadata cleanup (Kastan Day)
- `28edeb72` [Store] Close the gaps an implementation review found (Kastan Day)
- `7d7cd076` [Store] Fence promotion enqueue and expiry's deadline cleanup (Kastan Day)
- `ad44146a` Record wheel-26.1008.43949 in the changelog (Kastan Day)
- `294ed5a4` [Store] Make disk-replica validity monotone; one record of client status (Kastan Day)
- `8f86e3ea` [Store] Rebind only a completed disk replica on re-adoption (Kastan Day)
- `5644d1f9` Record wheel-26.1008.65205 in the changelog (Kastan Day)
- `7a5f072e` [Store] Bound inline quota eviction; no eviction pass holds the snapshot lock (Kastan Day)

## wheel-26.1008.65205 (2026-10-08)

Master: keep client liveness independent of metadata cleanup.

On eu-west1 (2026-10-08, about 13.8M keys), one elastic Store's disk unmount
flushed the whole cache:

1. `UnmountLocalDiskSegment` walked every metadata shard inside its RPC handler
   for 58 s. Synchronous coro_rpc handlers run on their connection's IO thread,
   so that walk held the thread and starved every connection sharing it.
2. A client on that thread missed its Pings and expired.
3. Its expiry swept the whole index again on the client-monitor thread, holding
   `snapshot_mutex_` shared throughout.
4. The expired, still-alive client remounted. `ReMountSegment` took
   `client_mutex_` exclusively and then waited for `snapshot_mutex_`.
5. Every Ping, and every Put/Upsert/batch call, needed `client_mutex_` shared,
   so they all waited behind that remount and all 75 clients expired at once.
6. The mass expiry then erased all 13.8M keys while holding shard locks, for
   about 2 minutes.

The defect was that liveness depended on O(total keys) metadata cleanup, run
inline on threads and under locks that liveness also needed. At 1 PiB of SSD
(0.5 to 1 billion keys), one Store crash would have frozen the Master for tens
of minutes.

The change is Master only: no client, protocol or flag change, and the 10-s
client TTL stays. Three mechanisms:

- **Pings never wait.**
  - `Ping` records receipt in a liveness table under a leaf mutex. Nothing
    holds that mutex while waiting for another lock.
  - The table is the only record of a client's status; `ok_client_` is gone.
  - The monitor reads the table and revalidates each expiry under the locks:
    a client that pinged since it was selected stays.
  - Lock order is `snapshot_mutex_`, then `client_mutex_`, then the leaf.
    Remount takes the snapshot lock first, so nothing holds `client_mutex_`
    while it waits. The data paths no longer take `client_mutex_` at all.
- **A replica's validity is checked in O(1) when it is read, and is
  monotone.**
  - Each LOCAL_DISK replica is bound to the disk-registration generation it
    was admitted under. It is servable only while that generation is current.
  - Generations are never reused, and 0 (bound to none) is never current. So
    once a replica is invalid it stays invalid: a later registration under
    the same client id cannot resurrect it.
  - A snapshot restore binds each restored replica to its owner's restored
    registration and credits its bytes there, as a live admission does.
  - An owner that reports a key again under a new registration rebinds its
    completed replica in place (HA keeps the old replica until its removal is
    durable; one already marked for removal is left to go).
  - A promoted HA standby has no SSD registry to bind against, so its disk
    replicas serve nothing until each owner re-registers and rescans. That is
    the price of monotonicity, in a mode eu-west1 does not run.
  - Used bytes are credited and debited per registration.
- **Cleanup is background garbage collection.**
  - Disk unmount and client expiry retire the registration or segment in
    O(that client's segments) and schedule the coalescing cleanup worker.
    Every expiry schedules one.
  - The worker visits at most about 256 keys per lock hold, taking
    `snapshot_mutex_` shared and one shard lock per batch.
  - It resumes from a bucket cursor and restarts a tenant if its map was
    rehashed, which is rare.
  - It steps aside whenever an exclusive writer, such as a remount or a disk
    unmount, is waiting.
  - HA, snapshot and CXL modes keep their synchronous sweeps, with the same
    predicate.

Also:
- Only a servable disk replica counts as an eviction backup or a promotion
  source, and a promotion task reaches only its registration's mailbox.
- Admission re-checks the generation under the shard lock, so a replica racing
  a retirement is refused rather than published behind the cleanup pass.
- The cleanup plan builds surviving descriptors only when the HA oplog needs
  them, and the `get_descriptor` error is rate-limited (27,432 lines in one
  minute during the incident, under shard locks).

Reviews:
- a critical design review and two implementation reviews by Codex
  (GPT-6-astra);
- a design review and implementation audit by Claude (Fable 5.1). Its
  simplifications went in: monotone generations replace a special sweep for
  restored replicas, and there is one record of client status.

Tests: new `master_liveness_isolation_test` (20 tests and a benchmark). It
covers each link of the incident chain, and these regressions:
- re-registration and restore;
- admission racing retirement;
- eviction and promotion against ended registrations;
- the offload-disabled path;
- rehash and yield in the cleanup pass.
Each test fails without the change it covers.

The existing Master suites pass. Three tests encoded the old semantics and
were adapted:
- one expected a never-remounted owner's disk replica to be stale;
- one called the removed owner-targeted sweep;
- the manager tests now name the registration they credit.

What this does not fix: the Master no longer stalls Pings with its own
cleanup, but a synchronous handler can still hold an IO thread. Remount, disk
unmount and graceful unmount take `snapshot_mutex_` exclusively on their IO
thread, so they wait for every shared holder, including `BatchEvict`'s
whole-index pass when DRAM is above the eviction watermark. At about 14M keys
that wait is short. It grows with the index.

Deferred, as gates before the index grows well past today's:
- moving blocking handlers off the IO threads, or scoping `BatchEvict`'s
  snapshot hold per batch;
- an owner-to-replica index, so reclamation is targeted (today a pass costs
  about 10 s at 14M keys and grows linearly);
- a lock-free published generation map for the read path;
- reclaiming a hot key's dead disk replica when it is accessed;
- bounded eviction and quota reclamation;
- recoverable client suspicion.

### Artifacts

Version `26.1008.65205` (built 2026-10-08 06:52:05 UTC) from `8f86e3ead09575a87c614034261e4e2c613dd640` on `kastan/wheel`.
Builder images: non-cuda: `pytorch/manylinux2_28-builder:cuda12.8`. CPython 3.12, x86_64.

| Variant | File | sha256 | Registry |
|---|---|---|---|
| non-cuda | `mooncake_transfer_engine_non_cuda-26.1008.65205-cp312-cp312-manylinux_2_28_x86_64.whl` | `5b18bb07c1cb8594e85986bfddae8518fc2658c7ac85403784b20413a8a8d0d8` | [https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/…](https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1008.65205-cp312-cp312-manylinux_2_28_x86_64.whl) |

Pin (mooncake-helm `mooncake-shared-cache` values; pods get `PYTHON_REGISTRY` from
the `cf-python-registry` Secret):

```yaml
  master.package: "${PYTHON_REGISTRY}/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1008.65205-cp312-cp312-manylinux_2_28_x86_64.whl#sha256=5b18bb07c1cb8594e85986bfddae8518fc2658c7ac85403784b20413a8a8d0d8"
```

### Commits since v0.3.13.post1

- `431e70ef` [Store] Unregister a segment's memory when its mount fails (Kastan Day)
- `57d1b314` Add additive successful-read source receipts to Store clients (Kastan Day)
- `598eeac7` Distinguish observed replica eviction from surviving servable metadata (Kastan Day)
- `1f4ce545` Record wheel-26.1006.215531 in the changelog (Kastan Day)
- `5c120b67` [Store] Keep Master client liveness independent of metadata cleanup (Kastan Day)
- `28edeb72` [Store] Close the gaps an implementation review found (Kastan Day)
- `7d7cd076` [Store] Fence promotion enqueue and expiry's deadline cleanup (Kastan Day)
- `ad44146a` Record wheel-26.1008.43949 in the changelog (Kastan Day)
- `294ed5a4` [Store] Make disk-replica validity monotone; one record of client status (Kastan Day)
- `8f86e3ea` [Store] Rebind only a completed disk replica on re-adoption (Kastan Day)

## wheel-26.1008.43949 (2026-10-08)

Master: keep client liveness independent of metadata cleanup.

On eu-west1 (2026-10-08, about 13.8M keys), one elastic Store's disk unmount
flushed the whole cache:

1. `UnmountLocalDiskSegment` walked every metadata shard inside its RPC handler
   for 58 s. Synchronous coro_rpc handlers run on their connection's IO thread,
   so that walk held the thread and starved every connection sharing it.
2. A client on that thread missed its Pings and expired.
3. Its expiry swept the whole index again on the client-monitor thread, holding
   `snapshot_mutex_` shared throughout.
4. The expired, still-alive client remounted. `ReMountSegment` took
   `client_mutex_` exclusively and then waited for `snapshot_mutex_`.
5. Every Ping, and every Put/Upsert/batch call, needed `client_mutex_` shared,
   so they all waited behind that remount and all 75 clients expired at once.
6. The mass expiry then erased all 13.8M keys while holding shard locks, for
   about 2 minutes.

The defect was that liveness depended on O(total keys) metadata cleanup, run
inline on threads and under locks that liveness also needed. At 1 PiB of SSD
(0.5 to 1 billion keys), one Store crash would have frozen the Master for tens
of minutes.

Changes (Master only: no client, protocol or flag change; the 10-s client TTL
stays):

- **Pings never wait.** `Ping` records receipt in a liveness table under a leaf
  mutex, which it never holds while waiting for another lock. It no longer
  touches `client_mutex_` or the ping queue. The monitor reads that table,
  revalidates each expiry under the locks (so a client that pinged since it
  was selected stays), and does only O(that client's segments) work.
- **One lock order: `snapshot_mutex_`, then `client_mutex_`, then the leaf.**
  Remount takes the snapshot lock first, so it never holds `client_mutex_` while
  it waits. The Put, Upsert, batch-replica and unmount paths no longer take
  `client_mutex_` at all; they used it only to copy the alive-client set.
- **A replica's validity is checked in O(1) when it is read.**
  - Each LOCAL_DISK replica is bound to the disk-registration generation it was
    admitted under. It is readable only while that generation is current, so
    from the moment its owner unmounts the disk or expires, before cleanup
    reaches it.
  - A later registration under the same client id does not resurrect old
    replicas.
  - Used bytes are credited and debited per registration.
  - A stale disk replica no longer keeps another owner's offload of the same
    key out.
- **Cleanup is background garbage collection.** Disk unmount and client expiry
  retire the registration or segment and schedule the existing coalescing
  cleanup worker; neither walks the index any more. The worker's pass:
  - visits at most 256 keys per lock hold, taking `snapshot_mutex_` shared and
    one shard lock per batch;
  - resumes from a bucket cursor (whole buckets per batch; on a rehash, which is
    rare, the tenant starts over);
  - steps aside whenever an exclusive writer such as a remount or disk unmount
    is waiting.

  HA, snapshot and CXL modes keep their synchronous sweeps, with the new
  predicate.
- **Only a servable disk replica counts.** Eviction treats a LOCAL_DISK
  replica as a backup, and promotion takes it as a source, only while its
  registration is current, and a promotion task is enqueued only into the
  mailbox of that registration. Admission re-checks the generation under the shard
  lock, so a replica racing a retirement is refused rather than published
  behind the cleanup pass. An expiry retires disk registrations before any
  synchronous sweep. A client that pings between selection and expiry keeps its
  graceful-unmount deadline, and an expiry never removes a deadline set after it.
- **The amplifier is gone.** The cleanup plan builds surviving descriptors only
  when the HA oplog needs them; building them asked every surviving memory
  replica's expired allocator for an endpoint, and each one logged an error.
  The `get_descriptor` error is now rate-limited: there were 27,432 of them in
  one minute, under shard locks.

Reviews: a critical design review and two implementation reviews (Codex,
GPT-6-astra), each finding fixed before this build.

Tests: new `master_liveness_isolation_test`. It covers:
- a Ping answered while a remount waits behind a held pass;
- Puts not waiting for `client_mutex_`;
- a disk unmount that returns at once and reclaims later;
- a same-id re-registration that does not resurrect old replicas;
- an ended registration that does not shadow a new owner;
- a pass that covers keys across concurrent rehashes;
- a pass that yields to exclusive writers;
- a monitor-driven expiry that leaves other clients alive;
- latency staying flat while many keys are reclaimed.

The existing Master suites pass. Two tests encoded the old semantics and were
adapted: one expected a never-remounted owner's disk replica to be treated as
stale, the other called the removed owner-targeted sweep.

Deferred to before 1 PiB:
- offloading blocking handlers from IO threads;
- an owner-to-replica index for targeted reclamation;
- bounded eviction and quota reclamation;
- recoverable client suspicion.

### Artifacts

Version `26.1008.43949` (built 2026-10-08 04:39:49 UTC) from `7d7cd07608d4f646181473cecc0a3315c5696c80` on `kastan/wheel`.
Builder images: non-cuda: `pytorch/manylinux2_28-builder:cuda12.8`. CPython 3.12, x86_64.

| Variant | File | sha256 | Registry |
|---|---|---|---|
| non-cuda | `mooncake_transfer_engine_non_cuda-26.1008.43949-cp312-cp312-manylinux_2_28_x86_64.whl` | `3c717747f8dd8cce193f8857797746505e76a2030654146416753976605f47f0` | [https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/…](https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1008.43949-cp312-cp312-manylinux_2_28_x86_64.whl) |

Pin (mooncake-helm `mooncake-shared-cache` values; pods get `PYTHON_REGISTRY` from
the `cf-python-registry` Secret):

```yaml
  master.package: "${PYTHON_REGISTRY}/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1008.43949-cp312-cp312-manylinux_2_28_x86_64.whl#sha256=3c717747f8dd8cce193f8857797746505e76a2030654146416753976605f47f0"
```

### Commits since v0.3.13.post1

- `431e70ef` [Store] Unregister a segment's memory when its mount fails (Kastan Day)
- `57d1b314` Add additive successful-read source receipts to Store clients (Kastan Day)
- `598eeac7` Distinguish observed replica eviction from surviving servable metadata (Kastan Day)
- `1f4ce545` Record wheel-26.1006.215531 in the changelog (Kastan Day)
- `5c120b67` [Store] Keep Master client liveness independent of metadata cleanup (Kastan Day)
- `28edeb72` [Store] Close the gaps an implementation review found (Kastan Day)
- `7d7cd076` [Store] Fence promotion enqueue and expiry's deadline cleanup (Kastan Day)

## wheel-26.1006.215531 (2026-10-06)

Upstream Mooncake **v0.3.13.post1** plus Cloudflare's three patches:

- the mount-failure fix the elastic Stores need (mooncake-helm !111);
- the KV-cache observability additions (mooncake-helm !107).

This is the first release from `kastan/wheel`, and the first that ships both variants together: **cuda13** for the Stores and SGLang clients, and **non-cuda** for the Master. The package names are upstream's (`mooncake-transfer-engine-cuda13`, `mooncake-transfer-engine-non-cuda`), and `import mooncake` is unchanged. Only the version differs: it is the build's UTC start time.

It replaces two separate wheels, which can now be retired:
- `26.1006.205117` / `0.3.13.post1+unregfix`: the fix only, used by !111;
- `0.3.13.post1+cf.20261001.kvobs1`: observability only, on the cache PVC for !107.

### Changes

#### 1. Unregister a segment's memory when its mount fails

Backport of kvcache-ai/Mooncake#4482. Client side: `mooncake-store/src/client_service.cpp`.

- **Problem:** `allocate_and_mount_segment` registers a buffer with the transfer engine, then asks the Master to mount it. When the Master rejects the mount (for example during a Master restart, or while it is unreachable), stock 0.3.13.post1 frees the buffer but leaves it registered.
  - The memory stays pinned (`VmPin`).
  - Every later registration at that address fails.
  - A Store can then never grow again until it restarts.
- **Seen on eu-west1:** the elastic Store churn test hit it after a Master restart (`VmPin +512 GiB`). A Store on the stock wheel exited 70 through the agent's stranded-chunk check; Stores on the fix logged the same mount failure and kept running.
- **Fix:** unregister the buffer before freeing it when the mount fails.
- **Test:** `NonHAReconnectTest.FailedMountLeavesBufferUnregistered` passes with the fix and fails with it reverted, in upstream CI's environment.
- **In production:** the elastic Store agent's `VmPin` check stays as a backstop.

#### 2. Successful-read source receipts in Store clients

Python client: `real_client`, `store_py`.

- **New APIs:** `batch_get_into_with_sources(keys, buffer_ptrs, sizes)` and `batch_get_into_multi_buffers_with_sources(...)` perform the same reads as `batch_get_into` / `batch_get_into_multi_buffers`. They also return, per key, the tier of the replica that served it: `"memory"`, `"local_disk"`, or `"unknown"` (miss or failure).
- **Additive:** the existing APIs are unchanged (internally they pass no receipt vector) and record the same transfer metrics.
- **Purpose:** this lets SGLang's cache-observability patch (`sglang-v0.5.14-cache-observability.patch`, !107) attribute KV hits to DRAM or SSD, once the SGLang clients run this wheel.
- **Test:** `RealClientTest.BatchReadReceiptsFollowSuccessfulSelectedReplica`.

#### 3. Distinguish observed replica eviction from surviving servable metadata

Master: `master_metric_manager`, `master_service`.

- **New metrics:**
  - `master_replica_eviction_logical_bytes_total{medium="memory"|"disk"|"unknown", disposition="surviving_servable_metadata"|"no_servable_metadata"}`: logical bytes removed by eviction, split by whether another completed, valid replica still serves the object.
  - `master_eviction_no_servable_metadata_objects_total` and `master_eviction_no_servable_metadata_logical_bytes_total`: evictions that left the object with no servable replica, which is real cache loss rather than tier movement.
- **Created on first eviction:** an idle Master does not export them yet.
- **Behaviour change:** disk (DFS) eviction now runs the eviction observer for every eviction that removed replicas, not only when the object's metadata became invalid.
  - The metric is always recorded.
  - The "KV removed" event is published only when the Master's KV event publisher is enabled, which eu-west1's Master does not do.
- **Test:** `MasterMetricsTest.ReplicaEvictionsKeepSurvivingMetadataSeparateAndLabelsBounded`.

### Compatibility

- **No RPC or wire-format change:** none of the patches touches RPC types or the client–Master protocol. Patched Stores and clients interoperate with stock 0.3.13.post1 Masters and clients, and the reverse. mooncake-helm !111 runs patched Stores beside a stock Master and stock SGLang clients.
- **Mount-failure fix:** client-side only.
- **Receipts:** opt-in APIs.
- **Eviction metrics:** need the patched Master (non-cuda wheel).

### Verified on eu-west1

`mooncake-shared-cache/wheel/build.py verify` installs from python.cfdata.org with the `production/cf-python-registry` Secret, the way the pods do.

- **cuda13**, in a production pod made from the elastic Store container:
  - installed `mooncake-transfer-engine-cuda13 26.1006.215531`;
  - RDMA setup, a 1 GiB `allocate_and_mount_segment(…, "rdma", "*")`, a 4 MiB put/get round trip, `unmount_and_free_segment`;
  - `batch_get_into_with_sources` returned `memory` for a stored key and `unknown` for a missing one;
  - after a fill past a 50% watermark, the Master exported all three eviction families (79 objects, 331 MB, `disposition="no_servable_metadata"`).
- **non-cuda**, with `verify --component master`, in the Master Deployment's own image (`python:3.12-slim`) and install line:
  - installed `mooncake-transfer-engine-non-cuda 26.1006.215531`;
  - `/health` OK;
  - a TCP client put/get round trip;
  - after a fill past the watermark, all three eviction families exported.
- **non-cuda build:** passed the release's smoke check that no file in a non-CUDA wheel links against CUDA.

### Not included

- **CI-only commits** from `kastan/kv-observability-v1` (`842dc053`, `aa94290e`, `a075b833`): GitHub Actions workflow changes that do not affect the wheel.
- **`mooncake-v0.3.13.post1-wheel-prerequisites.patch`:** build-recipe only. This release builds with mooncake-helm's `wheel/build-wheel.sh`, which follows the upstream release profile per variant.

### Used by

- **mooncake-helm !111:** `store.package` (cuda13) for every Store.
- **mooncake-helm !107:** the opt-in KV-observability overlays.
  - `master.package`: non-cuda.
  - `store.package` and `inferenceServer.mooncakeWheel`: cuda13.

### Build

From mooncake-helm:
- `uv run --no-project python mooncake-shared-cache/wheel/build.py build --ref kastan/wheel` builds one 16-CPU staging pod (cuda13), then one 8-CPU pod (non-cuda), with no GPU and no PVC. The source is streamed from the Mac as a git bundle. Both wheels are published from the Mac with the production service token.
- The procedure is in the skill `building-mooncake-store-wheel`.

### Artifacts

Version `26.1006.215531` (built 2026-10-06 21:55:31 UTC) from `598eeac7e83682c01cd3f33e2c3d28065d584669` on `kastan/wheel`.
Builder images: cuda13: `pytorch/manylinux2_28-builder:cuda13.0`, non-cuda: `pytorch/manylinux2_28-builder:cuda12.8`. CPython 3.12, x86_64.

| Variant | File | sha256 | Registry |
|---|---|---|---|
| cuda13 | `mooncake_transfer_engine_cuda13-26.1006.215531-cp312-cp312-manylinux_2_28_x86_64.whl` | `09886902d39f389a7227afa49e9e13d08f1c6944399b3dfb2a69007a3c9fb817` | [https://python.cfdata.org/project/mooncake-transfer-engine-cuda13/files/…](https://python.cfdata.org/project/mooncake-transfer-engine-cuda13/files/mooncake_transfer_engine_cuda13-26.1006.215531-cp312-cp312-manylinux_2_28_x86_64.whl) |
| non-cuda | `mooncake_transfer_engine_non_cuda-26.1006.215531-cp312-cp312-manylinux_2_28_x86_64.whl` | `c3c77d2340c11b5ce1db38c968a30e1abec661204409a1a9d855d5ac67f74f74` | [https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/…](https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1006.215531-cp312-cp312-manylinux_2_28_x86_64.whl) |

Pin (mooncake-helm `mooncake-shared-cache` values; pods get `PYTHON_REGISTRY` from
the `cf-python-registry` Secret):

```yaml
  store.package: "${PYTHON_REGISTRY}/project/mooncake-transfer-engine-cuda13/files/mooncake_transfer_engine_cuda13-26.1006.215531-cp312-cp312-manylinux_2_28_x86_64.whl#sha256=09886902d39f389a7227afa49e9e13d08f1c6944399b3dfb2a69007a3c9fb817"
  master.package: "${PYTHON_REGISTRY}/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1006.215531-cp312-cp312-manylinux_2_28_x86_64.whl#sha256=c3c77d2340c11b5ce1db38c968a30e1abec661204409a1a9d855d5ac67f74f74"
```

### Commits since v0.3.13.post1

- `431e70ef` [Store] Unregister a segment's memory when its mount fails (Kastan Day)
- `57d1b314` Add additive successful-read source receipts to Store clients (Kastan Day)
- `598eeac7` Distinguish observed replica eviction from surviving servable metadata (Kastan Day)
