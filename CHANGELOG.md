# Changelog

Cloudflare's Mooncake wheels, built from this branch (`kastan/wheel`: an upstream
release plus Cloudflare's patches) and published to the internal Python registry,
python.cfdata.org. Newest first. Each entry is a GitLab release tagged
`wheel-<version>` on the wheels' source commit; the version is the build's UTC
start time, `YY.MDD.HMMSS`.

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
