# Changelog

Cloudflare's Mooncake wheels, built from this branch (`kastan/wheel`: an upstream
release plus Cloudflare's patches) and published to the internal Python registry,
python.cfdata.org. Newest first. Each entry is a GitLab release tagged
`wheel-<version>` on the wheels' source commit; the version is the build's UTC
start time, `YY.MDD.HMMSS`.

## wheel-26.1010.50306 (2026-10-10)

Master: wheel-26.1010.35017 plus kastan/Mooncake!6, the review follow-up to the routing-stall fix (!5).

- **Mass-expiry hold from two clients due at once.** A partial stall, one RPC IO thread blocked past the TTL, leaves only the about 3 of 47 clients on that thread unanswered. The others keep pinging, so no observation stall is recorded. 26.1010.35017's threshold, max(3, clients/10), was 4 at 47 clients, so those 3 would expire. It now holds from 2 due at once, from 3 tracked clients. Two or more clients that really die together are expired one TTL later.
- **The stall guard is reset on a failed restore** (HA/restore only).

Same base (Mooncake 0.3.13.post1) and flags as 26.1010.35017 and 26.1009.173841.

**Tests:**
- `PartialStallOfAFewClientsIsHeldOneTtl` fails with the old threshold and passes now. `master_liveness_isolation_test` passes 37.
- master_service (153), kv_churn (17), promotion_on_hit (56), master_admin_server (53), tenant_quota (32), master_service_tenant_quota (33), master_service_ssd (25), snapshot_child_process (25), segment (20), master_metrics (17), client_metrics (16), offload_on_evict (15), evict_scenario (13), service_scenario (12), tenant_quota_ledger (11), local_ssd (10), ssd_metrics (6), non_ha_reconnect (4), packing_scale (4), local_ssd_codec (3) and double_erase (1) pass.

### Artifacts

Version `26.1010.50306` (built 2026-10-10 05:03:06 UTC) from `ab3a2b951fbf4cf5ce938c6c078022b51a44b686` on `kastan/wheel`.
Builder images: non-cuda: `pytorch/manylinux2_28-builder:cuda12.8`. CPython 3.12, x86_64.

| Variant | File | sha256 | Registry |
|---|---|---|---|
| non-cuda | `mooncake_transfer_engine_non_cuda-26.1010.50306-cp312-cp312-manylinux_2_28_x86_64.whl` | `29c26cd0641e063856150c9b6b7891056db5396e1aa853d81ef07b308c6ffd41` | [https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/…](https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1010.50306-cp312-cp312-manylinux_2_28_x86_64.whl) |

Pin (mooncake-helm `mooncake-shared-cache` values; pods get `PYTHON_REGISTRY` from
the `cf-python-registry` Secret):

```yaml
  master.package: "${PYTHON_REGISTRY}/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1010.50306-cp312-cp312-manylinux_2_28_x86_64.whl#sha256=29c26cd0641e063856150c9b6b7891056db5396e1aa853d81ef07b308c6ffd41"
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
- `1fc4dd95` Record wheel-26.1008.211229 in the changelog (Kastan Day)
- `fe3310b8` [Store] Refuse writes to far-over tenants without scanning; quieter evictions (Kastan Day)
- `b29e8e9c` [Store] Trim only what refused writes add; bound batches; no empty-census loop (Kastan Day)
- `947d58f7` [Store] Bound inline quota eviction by time as well as keys (Kastan Day)
- `90a89aca` [Store] Sum refused writes' demand for the quota trim (Kastan Day)
- `fdbac9a7` [Store] Log slow RPCs and slow exclusive snapshot waits and holds (Kastan Day)
- `d97c307e` [Store] Keep busy tenants' writes inline; refuse only collapsed quotas (Kastan Day)
- `858be0a8` [Store] Cap the quota eviction's bucket visits separately from its keys (Kastan Day)
- `647c8193` [Store] Export the allocators' footprint and largest free region (Kastan Day)
- `2b3744bc` [Store] Count queued offloads as quota-eviction progress (Kastan Day)
- `0cc0eae9` [Store] Scale tenant quota capacity by allocator packing, behind a flag (Kastan Day)
- `d2730429` [Store] Keep the inline quota eviction to its deadline between shards (Kastan Day)
- `205af36e` [Store] Format the last two quota-eviction changes (Kastan Day)
- `f1c14eb5` [Store] Serve the Stores' batch RPCs off the RPC IO threads (Kastan Day)
- `41d40227` [Store] Order the Stores' batch RPCs per connection, not per client (Kastan Day)
- `a544e76e` Record wheel-v0.3.13.post1+26.1009.65649 in the changelog (Kastan Day)
- `1c3f9734` [Store] Measure what Store churn costs the KV cache (Kastan Day)
- `dbd268dd` [Store] Co-locate a batch put in its first object's chunk (Kastan Day)
- `e9bbe868` [Doc] Design for keeping KV hit rates through Store churn (Kastan Day)
- `10b303ef` Merge kastan/hot-replica-churn (kastan/Mooncake!3) into the wheel branch (Kastan Day)
- `6ec05f69` Merge kastan/quota-packing-scale (kastan/Mooncake!1, !2) into the wheel branch (Kastan Day)
- `9dadbe4b` Record wheel-v0.3.13.post1+26.1009.155842 in the changelog (Kastan Day)
- `a45c87f6` [Store] Count rewrite-time churn drops; measure read load per Store (Kastan Day)
- `99703cf7` [Doc] Caveats for reading the churn metrics (Kastan Day)
- `fa83f4bf` Merge kastan/hot-replica-churn (kastan/Mooncake!4) into the wheel branch (Kastan Day)
- `e5ffcfae` Record wheel-v0.3.13.post1+26.1009.173841 in the changelog (Kastan Day)
- `8be977ef` [Store] Stripe group routing; don't count a Master stall against clients (Kastan Day)
- `2b848529` Merge kastan/master-stall-mass-expiry into the wheel branch (Kastan Day)
- `a7674ad0` Record wheel-26.1010.35017 in the changelog (Kastan Day)
- `25a52b62` [Store] Hold two clients due at once; reset the stall guard on restore (Kastan Day)
- `ab3a2b95` Merge kastan/master-stall-mass-expiry review fixes into the wheel branch (Kastan Day)

## wheel-26.1010.35017 (2026-10-10)

Master: wheel-v0.3.13.post1+26.1009.173841 plus a fix for the 10.3-s routing stall that flushed eu-west1's DRAM tier at 01:34 UTC on 2026-10-10 (kastan/Mooncake!5). Same base (Mooncake 0.3.13.post1); named 26.1010.35017 because the `<base>+` naming isn't on mooncake-helm `main` yet.

**Cause.** With SGLang's `enable_group_semantics`, every KV object has a route in one global `std::unordered_map`. Every metadata lookup took its lock shared. The insert that took the map past 24,607,243 routes rehashed it under the exclusive lock for 10.3 s, so no RPC was served, Pings included. The client monitor then expired all 47 clients, and the DRAM tier with them. The next rehash, at about 50M routes, would have taken about 20 s.

**Changes:**
- **Striped group routing:** routes and lease-refresh marks live in 1,024 stripes, each with its own leaf lock. A rehash moves about 1/1,024 of the routes, and lookups in other stripes never wait for it. Routing behaviour is unchanged.
- **Stall credit:** with 3 or more clients, a gap of min(3 s, TTL/3) in which no client is observed is treated as the Master not answering, and doesn't count against client TTLs (capped at 3 TTLs).
- **Mass-expiry hold:** when max(3, clients/10) or more clients fall due at once, none is expired until they stay due for one more TTL.
- One silent client still expires at its TTL. New log lines: `action=master_observation_stall`, `action=client_expiry_deferred`.

No protocol or flag change: a drop-in for 26.1009.173841, with the same Master flags.

**Tests:**
- 8 new tests in `master_liveness_isolation_test` (36 pass). With the old code restored, the insert test's worst insert takes 364 ms, and every pinging Store expires 3 s into a held IO thread.
- master_service (153), kv_churn (17), promotion_on_hit (56), ssd (25), tenant_quota (33), evict_scenario (13), metrics (17), segment (20), test_for_snapshot (69), snapshot_child_process (25), service_scenario (12), master_scenario (27), offload_on_evict (15), double_erase (1), admin_server (53), non_ha_reconnect (4) and kv_event_publisher (2) pass.
- `master_service_ha_test` passes 84 of 85: `PromotionCatchesUpToDurablePrefix` needs etcd and fails the same way without this change.

### Artifacts

Version `26.1010.35017` (built 2026-10-10 03:50:17 UTC) from `2b8485292b22691966e0a9594f243ba3d7098a0b` on `kastan/wheel`.
Builder images: non-cuda: `pytorch/manylinux2_28-builder:cuda12.8`. CPython 3.12, x86_64.

| Variant | File | sha256 | Registry |
|---|---|---|---|
| non-cuda | `mooncake_transfer_engine_non_cuda-26.1010.35017-cp312-cp312-manylinux_2_28_x86_64.whl` | `6850f0a4d8d9325ba987c77075808bfed91ccedb594a5ec39fd37419ab603ef4` | [https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/…](https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1010.35017-cp312-cp312-manylinux_2_28_x86_64.whl) |

Pin (mooncake-helm `mooncake-shared-cache` values; pods get `PYTHON_REGISTRY` from
the `cf-python-registry` Secret):

```yaml
  master.package: "${PYTHON_REGISTRY}/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-26.1010.35017-cp312-cp312-manylinux_2_28_x86_64.whl#sha256=6850f0a4d8d9325ba987c77075808bfed91ccedb594a5ec39fd37419ab603ef4"
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
- `1fc4dd95` Record wheel-26.1008.211229 in the changelog (Kastan Day)
- `fe3310b8` [Store] Refuse writes to far-over tenants without scanning; quieter evictions (Kastan Day)
- `b29e8e9c` [Store] Trim only what refused writes add; bound batches; no empty-census loop (Kastan Day)
- `947d58f7` [Store] Bound inline quota eviction by time as well as keys (Kastan Day)
- `90a89aca` [Store] Sum refused writes' demand for the quota trim (Kastan Day)
- `fdbac9a7` [Store] Log slow RPCs and slow exclusive snapshot waits and holds (Kastan Day)
- `d97c307e` [Store] Keep busy tenants' writes inline; refuse only collapsed quotas (Kastan Day)
- `858be0a8` [Store] Cap the quota eviction's bucket visits separately from its keys (Kastan Day)
- `647c8193` [Store] Export the allocators' footprint and largest free region (Kastan Day)
- `2b3744bc` [Store] Count queued offloads as quota-eviction progress (Kastan Day)
- `0cc0eae9` [Store] Scale tenant quota capacity by allocator packing, behind a flag (Kastan Day)
- `d2730429` [Store] Keep the inline quota eviction to its deadline between shards (Kastan Day)
- `205af36e` [Store] Format the last two quota-eviction changes (Kastan Day)
- `f1c14eb5` [Store] Serve the Stores' batch RPCs off the RPC IO threads (Kastan Day)
- `41d40227` [Store] Order the Stores' batch RPCs per connection, not per client (Kastan Day)
- `a544e76e` Record wheel-v0.3.13.post1+26.1009.65649 in the changelog (Kastan Day)
- `1c3f9734` [Store] Measure what Store churn costs the KV cache (Kastan Day)
- `dbd268dd` [Store] Co-locate a batch put in its first object's chunk (Kastan Day)
- `e9bbe868` [Doc] Design for keeping KV hit rates through Store churn (Kastan Day)
- `10b303ef` Merge kastan/hot-replica-churn (kastan/Mooncake!3) into the wheel branch (Kastan Day)
- `6ec05f69` Merge kastan/quota-packing-scale (kastan/Mooncake!1, !2) into the wheel branch (Kastan Day)
- `9dadbe4b` Record wheel-v0.3.13.post1+26.1009.155842 in the changelog (Kastan Day)
- `a45c87f6` [Store] Count rewrite-time churn drops; measure read load per Store (Kastan Day)
- `99703cf7` [Doc] Caveats for reading the churn metrics (Kastan Day)
- `fa83f4bf` Merge kastan/hot-replica-churn (kastan/Mooncake!4) into the wheel branch (Kastan Day)
- `e5ffcfae` Record wheel-v0.3.13.post1+26.1009.173841 in the changelog (Kastan Day)
- `8be977ef` [Store] Stripe group routing; don't count a Master stall against clients (Kastan Day)
- `2b848529` Merge kastan/master-stall-mass-expiry into the wheel branch (Kastan Day)

## wheel-v0.3.13.post1+26.1009.173841 (2026-10-09)

Master: the liveness fix plus churn metrics, batch-put co-location and allocator footprint metrics.

This is wheel-v0.3.13.post1+26.1009.155842 plus kastan/Mooncake!4 (below). That wheel was wheel-v0.3.13.post1+26.1009.65649 (the Master liveness fix, A/B-tested on eu-west1 with 0 collateral expiries in 35 scenarios) with three more Master-only changes merged in:

- **kastan/Mooncake!3**, Store churn's cost to the KV cache, and batch-put co-location (phases P0 and A of `docs/source/design/store/hot-replica-churn.md`):
  - `--enable_kv_churn_metrics` (off by default) adds per-object read heat, which objects churn drops and why (unmount, client expiry, disk unmount, evictions), misses on them later, and prefix breaks in `BatchExistKey`. About 48 MB of Master memory.
  - `--colocate_batch_puts` (off by default) places a batch put in its first object's 64-GiB chunk, then a sibling chunk, then `free_ratio_first`. One elastic shrink then breaks about 2% of prefixes instead of about 59%.
- **kastan/Mooncake!1**, the allocators' footprint: `master_allocated_footprint_bytes`, and per segment the footprint and largest free region. Together they show the padding and fragmentation that `master_allocated_bytes` misses: eu-west1 tops out near 87% allocated, and pool-wide 5% evictions follow.
- **kastan/Mooncake!2**, `--tenant_quota_packing_scale` (off by default): scales the capacity tenant quotas divide by the allocators' packing efficiency, smoothed and bounded below by `--tenant_quota_packing_scale_floor` (0.8).

- **kastan/Mooncake!4**, follow-ups to !3's review, behind `--enable_kv_churn_metrics`:
  - a rewrite (`PutStart`/`UpsertStart`) that reclaims a lost object before the cleanup pass now counts as a churn drop, so `master_kv_churn_drops_total`'s heat is no longer biased cold;
  - `master_kv_store_read_bytes_total{segment}` counts served read bytes per Store, the read load co-location concentrates;
  - `--churn_miss_slice_capacity` is clamped to 2^26 (a larger value hung startup);
  - the design doc's caveats for reading the metrics.

All new behaviour is behind flags that are off by default. There is no client or protocol change.

Tests, on the merge:
- The Master suites pass, including HA and snapshot, the client and server suites, `kv_churn_test` (17), `tenant_quota_packing_scale_test` (4), `buffer_allocator_test` (20) and `master_liveness_isolation_test` (28).
- Two tests fail the same way without these changes:
  - one HA test needs etcd;
  - `MasterServiceSSDSnapshotTest.EvictObject` is flaky upstream. It failed 3 of 10 runs alone on the pre-liveness base, and 2 of 10 on 65649.
- The built `mooncake_master` starts with `--enable_kv_churn_metrics=true --colocate_batch_puts=true`, the flags mooncake-helm !111 passes.

### Artifacts

Version `0.3.13.post1+26.1009.173841` (built 2026-10-09 17:38:41 UTC) from `fa83f4bff27a5c5ed5fe8097b959ecad8211f1e4` on `kastan/wheel`.
Builder images: non-cuda: `pytorch/manylinux2_28-builder:cuda12.8`. CPython 3.12, x86_64.

| Variant | File | sha256 | Registry |
|---|---|---|---|
| non-cuda | `mooncake_transfer_engine_non_cuda-0.3.13.post1+26.1009.173841-cp312-cp312-manylinux_2_28_x86_64.whl` | `6f8f9773b1e8a1749d95357ba47a9aacf9f70399bd8d2adf1e9446c7182cd6b1` | [https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/…](https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-0.3.13.post1+26.1009.173841-cp312-cp312-manylinux_2_28_x86_64.whl) |

Pin (mooncake-helm `mooncake-shared-cache` values; pods get `PYTHON_REGISTRY` from
the `cf-python-registry` Secret):

```yaml
  master.package: "${PYTHON_REGISTRY}/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-0.3.13.post1+26.1009.173841-cp312-cp312-manylinux_2_28_x86_64.whl#sha256=6f8f9773b1e8a1749d95357ba47a9aacf9f70399bd8d2adf1e9446c7182cd6b1"
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
- `1fc4dd95` Record wheel-26.1008.211229 in the changelog (Kastan Day)
- `fe3310b8` [Store] Refuse writes to far-over tenants without scanning; quieter evictions (Kastan Day)
- `b29e8e9c` [Store] Trim only what refused writes add; bound batches; no empty-census loop (Kastan Day)
- `947d58f7` [Store] Bound inline quota eviction by time as well as keys (Kastan Day)
- `90a89aca` [Store] Sum refused writes' demand for the quota trim (Kastan Day)
- `fdbac9a7` [Store] Log slow RPCs and slow exclusive snapshot waits and holds (Kastan Day)
- `d97c307e` [Store] Keep busy tenants' writes inline; refuse only collapsed quotas (Kastan Day)
- `858be0a8` [Store] Cap the quota eviction's bucket visits separately from its keys (Kastan Day)
- `647c8193` [Store] Export the allocators' footprint and largest free region (Kastan Day)
- `2b3744bc` [Store] Count queued offloads as quota-eviction progress (Kastan Day)
- `0cc0eae9` [Store] Scale tenant quota capacity by allocator packing, behind a flag (Kastan Day)
- `d2730429` [Store] Keep the inline quota eviction to its deadline between shards (Kastan Day)
- `205af36e` [Store] Format the last two quota-eviction changes (Kastan Day)
- `f1c14eb5` [Store] Serve the Stores' batch RPCs off the RPC IO threads (Kastan Day)
- `41d40227` [Store] Order the Stores' batch RPCs per connection, not per client (Kastan Day)
- `a544e76e` Record wheel-v0.3.13.post1+26.1009.65649 in the changelog (Kastan Day)
- `1c3f9734` [Store] Measure what Store churn costs the KV cache (Kastan Day)
- `dbd268dd` [Store] Co-locate a batch put in its first object's chunk (Kastan Day)
- `e9bbe868` [Doc] Design for keeping KV hit rates through Store churn (Kastan Day)
- `10b303ef` Merge kastan/hot-replica-churn (kastan/Mooncake!3) into the wheel branch (Kastan Day)
- `6ec05f69` Merge kastan/quota-packing-scale (kastan/Mooncake!1, !2) into the wheel branch (Kastan Day)
- `9dadbe4b` Record wheel-v0.3.13.post1+26.1009.155842 in the changelog (Kastan Day)
- `a45c87f6` [Store] Count rewrite-time churn drops; measure read load per Store (Kastan Day)
- `99703cf7` [Doc] Caveats for reading the churn metrics (Kastan Day)
- `fa83f4bf` Merge kastan/hot-replica-churn (kastan/Mooncake!4) into the wheel branch (Kastan Day)

## wheel-v0.3.13.post1+26.1009.155842 (2026-10-09)

Master: the liveness fix plus churn metrics, batch-put co-location and allocator footprint metrics.

This is wheel-v0.3.13.post1+26.1009.65649 (the Master liveness fix, A/B-tested on eu-west1 with 0 collateral expiries in 35 scenarios) with three more Master-only changes merged in:

- **kastan/Mooncake!3**, Store churn's cost to the KV cache, and batch-put co-location (phases P0 and A of `docs/source/design/store/hot-replica-churn.md`):
  - `--enable_kv_churn_metrics` (off by default) adds per-object read heat, which objects churn drops and why (unmount, client expiry, disk unmount, evictions), misses on them later, and prefix breaks in `BatchExistKey`. About 48 MB of Master memory.
  - `--colocate_batch_puts` (off by default) places a batch put in its first object's 64-GiB chunk, then a sibling chunk, then `free_ratio_first`. One elastic shrink then breaks about 2% of prefixes instead of about 59%.
- **kastan/Mooncake!1**, the allocators' footprint: `master_allocated_footprint_bytes`, and per segment the footprint and largest free region. Together they show the padding and fragmentation that `master_allocated_bytes` misses: eu-west1 tops out near 87% allocated, and pool-wide 5% evictions follow.
- **kastan/Mooncake!2**, `--tenant_quota_packing_scale` (off by default): scales the capacity tenant quotas divide by the allocators' packing efficiency, smoothed and bounded below by `--tenant_quota_packing_scale_floor` (0.8).

All new behaviour is behind flags that are off by default. There is no client or protocol change.

Tests, on the merge:
- The Master suites pass, including HA and snapshot, the client and server suites, `kv_churn_test` (12), `tenant_quota_packing_scale_test` (4), `buffer_allocator_test` (20) and `master_liveness_isolation_test` (28).
- Two tests fail the same way without these changes:
  - one HA test needs etcd;
  - `MasterServiceSSDSnapshotTest.EvictObject` is flaky upstream. It failed 3 of 10 runs alone on the pre-liveness base, and 2 of 10 on 65649.
- The built `mooncake_master` starts with `--enable_kv_churn_metrics=true --colocate_batch_puts=true`, the flags mooncake-helm !111 passes.

### Artifacts

Version `0.3.13.post1+26.1009.155842` (built 2026-10-09 15:58:42 UTC) from `6ec05f69a0f3ccd427227b274dd0f4253ec5a000` on `kastan/wheel`.
Builder images: non-cuda: `pytorch/manylinux2_28-builder:cuda12.8`. CPython 3.12, x86_64.

| Variant | File | sha256 | Registry |
|---|---|---|---|
| non-cuda | `mooncake_transfer_engine_non_cuda-0.3.13.post1+26.1009.155842-cp312-cp312-manylinux_2_28_x86_64.whl` | `a77cfe05618bebc730f6b2a6d62a53f294e7b6bb0cbb6663e317781432a66c49` | [https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/…](https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-0.3.13.post1+26.1009.155842-cp312-cp312-manylinux_2_28_x86_64.whl) |

Pin (mooncake-helm `mooncake-shared-cache` values; pods get `PYTHON_REGISTRY` from
the `cf-python-registry` Secret):

```yaml
  master.package: "${PYTHON_REGISTRY}/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-0.3.13.post1+26.1009.155842-cp312-cp312-manylinux_2_28_x86_64.whl#sha256=a77cfe05618bebc730f6b2a6d62a53f294e7b6bb0cbb6663e317781432a66c49"
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
- `1fc4dd95` Record wheel-26.1008.211229 in the changelog (Kastan Day)
- `fe3310b8` [Store] Refuse writes to far-over tenants without scanning; quieter evictions (Kastan Day)
- `b29e8e9c` [Store] Trim only what refused writes add; bound batches; no empty-census loop (Kastan Day)
- `947d58f7` [Store] Bound inline quota eviction by time as well as keys (Kastan Day)
- `90a89aca` [Store] Sum refused writes' demand for the quota trim (Kastan Day)
- `fdbac9a7` [Store] Log slow RPCs and slow exclusive snapshot waits and holds (Kastan Day)
- `d97c307e` [Store] Keep busy tenants' writes inline; refuse only collapsed quotas (Kastan Day)
- `858be0a8` [Store] Cap the quota eviction's bucket visits separately from its keys (Kastan Day)
- `647c8193` [Store] Export the allocators' footprint and largest free region (Kastan Day)
- `2b3744bc` [Store] Count queued offloads as quota-eviction progress (Kastan Day)
- `0cc0eae9` [Store] Scale tenant quota capacity by allocator packing, behind a flag (Kastan Day)
- `d2730429` [Store] Keep the inline quota eviction to its deadline between shards (Kastan Day)
- `205af36e` [Store] Format the last two quota-eviction changes (Kastan Day)
- `f1c14eb5` [Store] Serve the Stores' batch RPCs off the RPC IO threads (Kastan Day)
- `41d40227` [Store] Order the Stores' batch RPCs per connection, not per client (Kastan Day)
- `a544e76e` Record wheel-v0.3.13.post1+26.1009.65649 in the changelog (Kastan Day)
- `1c3f9734` [Store] Measure what Store churn costs the KV cache (Kastan Day)
- `dbd268dd` [Store] Co-locate a batch put in its first object's chunk (Kastan Day)
- `e9bbe868` [Doc] Design for keeping KV hit rates through Store churn (Kastan Day)
- `10b303ef` Merge kastan/hot-replica-churn (kastan/Mooncake!3) into the wheel branch (Kastan Day)
- `6ec05f69` Merge kastan/quota-packing-scale (kastan/Mooncake!1, !2) into the wheel branch (Kastan Day)

## wheel-v0.3.13.post1+26.1009.65649 (2026-10-09)

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
  - A write's inline eviction examines at most 4,096 keys (and 65,536
    buckets), for at most 20 ms. A pass that freed memory or queued offloads
    made progress, and the write retries.
  - The pass checks its deadline per shard as well as per 32 keys. Behind
    the trim's shard walk, a pass for a tenant with few keys per shard
    examined almost nothing and so never checked it. After three Stores
    crashed in the A/B, all sixteen RPC threads trailed the trim for up to
    12 s and 48 clients expired.
  - A pass that ran out of budget with neither refuses the write
    (`quota_trimming`). Its own size goes to a background quota-trim worker,
    and its RPC thread refuses that tenant's writes for the next 100 ms
    without scanning.
  - A tenant over by more than its whole quota (and at least 256 MiB) is
    refused at once.
  - The worker trims every tenant with a policy down to its quota, plus what
    refused writes asked for, whenever capacity or a policy changes.
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
  - With offload-on-evict, a pass over memory-only objects queues their
    offloads and frees nothing until they land. Reading that as "nothing
    evictable" stopped the inline passes that fill the offload queue, and
    refused 1.7M of a spill's writes.
  - A slow inline eviction logs `inline_quota_eviction_slow`. The
    per-eviction `[TENANT-EVICT]` warnings and per-key batch refusals log
    every 1000th: they ran at about 1,800 lines/s from the RPC threads.
  - After a census that found nothing evictable, `BatchEvict` waits a second
    before the next. At zero capacity every refused write used to trigger a
    full census every 10 ms.

- **Stalls can be attributed.** A synchronous RPC over 500 ms logs
  `action=rpc_slow` with its name. An exclusive `snapshot_mutex_` wait or
  hold over 100 ms logs `action=snapshot_exclusive_wait_slow` or
  `..._hold_slow`, with the caller.

- **The Stores' batch RPCs leave the IO threads.** `BatchEvictDiskReplica`
  and `NotifyOffloadSuccess` run on serial workers (sixteen, as the RPC
  threads) chosen by connection, and answer through `coro_rpc::context`.
  - A disk watermark eviction is a single call: in the A/B, 497,445 keys
    held an IO thread for 10.4 s, and two clients whose Pings queued behind
    it expired.
  - Each connection's calls keep their order and connections run in
    parallel, as on the IO threads. Ordering all of a Store's calls on one
    worker slowed its offload notices: a spill was refused 1.24M times.
  - They are registered under the synchronous methods' route keys, so the
    wire is unchanged.

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

Tests: new `master_liveness_isolation_test` (28 tests and a benchmark). It
covers each link of the incident chain, and these regressions:
- re-registration and restore;
- admission racing retirement;
- eviction and promotion against ended registrations;
- the offload-disabled path;
- rehash and yield in the cleanup pass.
Each test fails without the change it covers.

eu-west1 A/B (an isolated test Master in staging, 3 test Stores, a 72-client
swarm, production's key and client counts): on production's wheel every
scenario failed, with Pings at 0/s and Master-only calls stalled 19 to 30 s.
On this wheel, 35 scenarios had 0 collateral expiries and 0 corrupt reads:
- the incident replay, disk unmounts, client and Store freezes, single and
  triple Store crashes, and their repeats;
- a 12-minute soak with all Stores resizing every 20 s;
- 2x the keys (52M);
- a Master restart.
Worst Master-only call 4.3 s, against the 10-s client TTL.

The existing Master suites pass. Three tests encoded the old semantics and
were adapted:
- one expected a never-remounted owner's disk replica to be stale;
- one called the removed owner-targeted sweep;
- the manager tests now name the registration they credit.

What this does not fix: other handlers still run on the RPC IO threads that
carry Pings. Every known long hold is now bounded, but a new unbounded handler
would starve its thread's clients again. `GetReplicaListByRegex`,
`RemoveByRegex` and `RemoveAll` still scan the whole index inline; do not call
them on a large index.

Deferred, as gates before the index grows well past today's:
- moving the remaining handlers off the IO threads (defence in depth);
- an owner-to-replica index, so reclamation is targeted (today a pass costs
  about 10 s at 14M keys and grows linearly);
- a lock-free published generation map for the read path;
- reclaiming a hot key's dead disk replica when it is accessed;
- parallel quota trimming across shards, if a trim after a large capacity drop takes minutes;
- recoverable client suspicion.

### Artifacts

Version `0.3.13.post1+26.1009.65649` (built 2026-10-09 06:56:49 UTC) from `41d402276e4c6678295440aa98e15acb03b009b5` on `kastan/wheel`.
Builder images: non-cuda: `pytorch/manylinux2_28-builder:cuda12.8`. CPython 3.12, x86_64.

| Variant | File | sha256 | Registry |
|---|---|---|---|
| non-cuda | `mooncake_transfer_engine_non_cuda-0.3.13.post1+26.1009.65649-cp312-cp312-manylinux_2_28_x86_64.whl` | `6dc7ae963cf14b98c873c0c518ea2b5fc9edc2e0960d9fc51dfde2a708621cc3` | [https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/…](https://python.cfdata.org/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-0.3.13.post1+26.1009.65649-cp312-cp312-manylinux_2_28_x86_64.whl) |

Pin (mooncake-helm `mooncake-shared-cache` values; pods get `PYTHON_REGISTRY` from
the `cf-python-registry` Secret):

```yaml
  master.package: "${PYTHON_REGISTRY}/project/mooncake-transfer-engine-non-cuda/files/mooncake_transfer_engine_non_cuda-0.3.13.post1+26.1009.65649-cp312-cp312-manylinux_2_28_x86_64.whl#sha256=6dc7ae963cf14b98c873c0c518ea2b5fc9edc2e0960d9fc51dfde2a708621cc3"
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
- `1fc4dd95` Record wheel-26.1008.211229 in the changelog (Kastan Day)
- `fe3310b8` [Store] Refuse writes to far-over tenants without scanning; quieter evictions (Kastan Day)
- `b29e8e9c` [Store] Trim only what refused writes add; bound batches; no empty-census loop (Kastan Day)
- `947d58f7` [Store] Bound inline quota eviction by time as well as keys (Kastan Day)
- `90a89aca` [Store] Sum refused writes' demand for the quota trim (Kastan Day)
- `fdbac9a7` [Store] Log slow RPCs and slow exclusive snapshot waits and holds (Kastan Day)
- `d97c307e` [Store] Keep busy tenants' writes inline; refuse only collapsed quotas (Kastan Day)
- `858be0a8` [Store] Cap the quota eviction's bucket visits separately from its keys (Kastan Day)
- `2b3744bc` [Store] Count queued offloads as quota-eviction progress (Kastan Day)
- `d2730429` [Store] Keep the inline quota eviction to its deadline between shards (Kastan Day)
- `205af36e` [Store] Format the last two quota-eviction changes (Kastan Day)
- `f1c14eb5` [Store] Serve the Stores' batch RPCs off the RPC IO threads (Kastan Day)
- `41d40227` [Store] Order the Stores' batch RPCs per connection, not per client (Kastan Day)

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
