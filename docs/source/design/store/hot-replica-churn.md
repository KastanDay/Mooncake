---
orphan: true
---

# Keeping KV Hit Rates Through Store Churn (Design Proposal)

Status: 2026-10-08. Base: `v0.3.13.post1` plus
`kastan/master-liveness-isolation`. P0 (measure) and A (co-locate batch puts)
are implemented as Cloudflare patches, behind `--enable_kv_churn_metrics` and
`--colocate_batch_puts`, both off by default (section 4.1). B and C are
proposals.

## Summary

The goal is to keep the SGLang KV hit rate high while Stores shrink, restart, or
disappear. The idea under review is to replicate hot objects to more than one
Store. Upstream already ships the mechanism for that, dynamic hot replica fanout
(kvcache-ai/Mooncake#3389, in our base, off by default). As shipped, though, it
would do almost nothing for us: its admission threshold, heat tracking, quota
charging and target selection each block it on eu-west1 (section 2).

The research also turned up a second effect, which is probably the larger one.
SGLang uses a cached prefix only up to its first missing page, and Mooncake
places every page independently. A long prefix therefore depends on dozens of
segments, so losing a small, random slice of memory breaks most long prefixes
(section 1.3). Replicating hot objects protects the shared start of popular
prefixes. It cannot protect the long tail of conversation-specific prefixes,
which is where scattering does the most damage.

Recommendation, in order:

| # | Change | Protects against | Memory cost | Size |
|---|--------|------------------|-------------|------|
| P0 | **Measure.** Count misses on keys that churn destroyed, the heat distribution, and how many segments each prefix spans. No behaviour change. | (decides the rest) | about 0 | S |
| A | **Co-locate a batch put's objects** in its first object's chunk, so a prefix depends on a few chunks instead of dozens. | all churn, every prefix | 0 | S |
| B | **Hot standing replicas** (the idea under review): adapt #3389 so that hot objects hold a second memory replica on another host's most stable segment, within a fixed budget, and are repaired after a loss. | crash, eviction, node loss, shrink, for the hot shared part of prefixes | budget (for example 5%) | M–L |
| C | **Drain before free**: before freeing a chunk, the Store asks the Master to move that chunk's hot objects to other Stores and to offload the rest to the Store's own SSD, within a short deadline. | planned shrink and planned exit | 0 | M |

A and C are not replication, but they serve the same goal. A is the cheapest and
plausibly the largest win; P0 tells us whether that holds. B is the feature
under review and is designed in full below. It works best combined with A,
because A makes every prefix's risk about the fraction of memory lost, and B
reduces that risk to nearly zero for the hot part of each prefix.

## 1. What churn actually destroys

### 1.1 Only memory-only copies are lost

eu-west1 runs `--offload_on_evict`, so an evicted object is written to its
Store's SSD before its memory is freed (unless the offload queue is full). An
eviction keeps the object servable as an SSD hit, and promotion-on-hit brings it
back to memory. Churn is different. These events drop memory replicas without
any offload:

| Event | What is lost | SSD copy survives? | Today's recovery |
|-------|--------------|--------------------|------------------|
| Planned shrink (`unmount_and_free_segment(ids, 0)`) | one chunk's objects | yes, but hot objects have no SSD copy (only evictions offload) | none |
| Planned Store exit (withdrawn permit, overdue shrink) | all of the Store's memory | yes (the disk survives the restart) | none for memory-only objects |
| Container crash or OOM | all of the Store's memory | yes, rescanned at setup | none for memory-only objects |
| Pod eviction or node loss | memory and possibly disk | maybe | none |
| Whole-cluster flush (the 2026-10-08 Master cascade) | everything | partly | fixed separately by the liveness work; replication cannot help |

### 1.2 eu-west1, 2026-10-08 (Prometheus, about 15 h of history)

- Memory tier: 13.7 TiB capacity, 85% allocated, about 7M memory-resident
  objects out of 21.9M total. SSD tier: 86 TiB, 36 TiB used.
- Objects average 1.8 MB (median 1.4 MB). The cluster reads about 2.4k objects/s
  and writes about 2.2k objects/s (3.9 GiB/s), so an object is read about 1.2
  times per hour on average. Memory serves 96 to 98% of hits.
- **Elastic shrink dropped 3.9 TiB of memory in 10 h**, in 18 capacity drops of
  64 to 503 GiB each. That is 28% of the memory tier, all of it without an
  offload. The Master saw 75 segment unmounts in 24 h.
- The restarts in that window were all the 07:44 cascade: 45 Store restarts and
  47 remounts. Outside the incident, Store crashes are rare. Planned shrink is
  the dominant churn event.
- Tenants are at or near quota: gemma-4-26b 100%, kimi-k27 90%, kimi-k26 88%.
  `reserved-headroom` holds 0.83 TiB uncharged.
- A rough event study of the SGLang token hit rate (cached/prompt tokens, mean
  0.65): in the 10 minutes after a minute with an unmount, the hit rate is 2.2
  points lower than in the 15 to 5 minutes before (n=31; the same comparison at
  random times has a standard error of 1.1 points). That is about 2σ and
  confounded: unmounts happen because model pods are starting. P0 replaces it
  with a direct measure.

### 1.3 Prefix fragility: why small losses cost a lot

`MooncakeStore.batch_exists` in SGLang returns the index of the first missing
key. The usable prefix ends there, and every later page is recomputed even if it
is still in the cache. `free_ratio_first` allocates each object independently:
it samples six segments from a random start and takes the emptiest. So one
prefix's pages are spread across most segments in the cluster.

Suppose a fraction `f` of memory is lost uniformly at random, and a prefix
consists of `K` keys (pages times keys per page). The prefix survives intact
with probability `(1-f)^K`, and the expected fraction of its tokens still usable
is about `(1-(1-f)^K)/(Kf)`:

| Loss `f` | Tokens lost, K=50 | K=200 | K=800 | Prefixes broken, K=200 |
|----------|-------------------|-------|-------|------------------------|
| 0.45% (one 64 GiB chunk) | 11% | 34% | 73% | 59% |
| 2.4% (an average Store, 340 GiB) | 43% | 80% | 95% | 99% |
| 3.6% (a 500 GiB shrink) | 55% | 87% | 97% | 100% |

These figures apply to prefixes that are read again before something rewrites
them. The first reader after a loss recomputes the tail and writes it back, so
each break costs one tail recompute per reused prefix. That is still the
mechanism that turns a 0.45% capacity drop into a visible dip in hit rate.

If each prefix lived on a few segments, a loss of `f` would cost about `f` of
the tokens (the whole prefix with probability about `f`) instead of tens of
percent. Replication cannot do this for prefixes in general, because the long
tails are cold. Placement can, which is the case for change A.

Heat has a useful property here. Reading page `i` of a prefix implies having
read pages `0..i-1`, so read counts never increase along a prefix chain. A heat
threshold therefore selects a *prefix-closed* set: the top of the prefix tree
(system prompts, popular documents, shared multi-turn trunks). Protecting the
hot set protects whole prefix starts, not scattered pages, which is the reason
B is worth having.

## 2. What upstream provides

### 2.1 Dynamic hot replica fanout (#3389, RFC #3388)

Upstream goals are hot-node bandwidth fanout and, later, cross-domain
placement. The RFC lists "a separate shrink controller or active reclaim
policy" as a non-goal. Churn resilience would be a new stage built on its
machinery. The pieces worth keeping:

- Master-side admission with distributed execution. Reads record heat. A
  background worker selects a source and a target and submits a `REPLICA_COPY`
  task to the source Store's client, which runs `Copy()`.
- Per-object `dynamic_replicas` records, a 30-s lease, version-epoch fencing, an
  action cooldown, and a recreate cooldown after an eviction removes a dynamic
  replica.
- `--dynamic_replication_mode=off|observe|enforce` and
  `--dynamic_replication_max_memory_replicas` (default 2).

Why it would do almost nothing on eu-west1, even in enforce mode:

| # | Gap | Effect here |
|---|-----|-------------|
| G1 | Admission requires `ceil(0.8 QPS × 10 s) = 8` reads of one key within 10 s. | About 2,400× the mean per-key read rate. L3 reads of a shared prefix come from different SGLang instances, minutes apart (each instance then holds the prefix in its host cache), so almost nothing qualifies. |
| G2 | Heat lives in a map capped at 50k entries, under one global mutex taken for every key of every get. Cleanup frees at most 256 entries per second. | With about 2.4k distinct keys read per second, the map fills within seconds and most new keys are never observed. The mutex is a new lock on the RPC IO threads. |
| G3 | `CopyStart` charges the extra replica to the tenant's quota and refuses with `TENANT_QUOTA_EXCEEDED` (no eviction). | Our tenants sit at quota, so most copies are refused. |
| G4 | A copy target must stay below the hard-coded watermark 0.85 after the copy. | Segments run up to the 0.95 eviction watermark, so few targets qualify. Kastan's unmerged commit 88758ba8 makes this configurable. |
| G5 | Placement prefers another host and lower utilisation. It does not exclude segments that are draining or unmounting gracefully, and it knows nothing about stability. | A replica can land on the very chunk the agent frees next. |
| G6 | Nothing re-replicates after a replica is lost, and no metrics are exported (observe mode only writes `VLOG(1)`). | No repair; observe mode cannot be measured. |
| G7 | Copy throughput: the client polls every 1 s for at most 16 tasks and runs them on 4 threads. | About 16 copies/s per source Store (roughly 29 MB/s). Fine for steady state, too slow for draining a chunk. |

### 2.2 Other relevant pieces

- **Graceful unmount.** `GracefulUnmountSegment` marks a segment
  `GRACEFULLY_UNMOUNTING` (readable, no new allocations) and unmounts it at a
  deadline. The Python binding already accepts it:
  `unmount_and_free_segment(ids, grace_period_seconds)`. With a nonzero grace,
  the call returns at once and frees later from a timer, which does not fit the
  agent's synchronous free and pinned-memory checks (C uses a different shape).
- **DrainJob** (#1815; native RPCs in open #4436; snapshot support in open
  #4481). It sets segments to `DRAINING` and moves objects off them with
  `REPLICA_MOVE` tasks. Its scheduler scans the whole index on every tick while
  holding `snapshot_mutex_` shared, which is the O(total keys) hold the liveness
  work removed elsewhere. Unusable at our scale as it stands, but its job and
  status API is the right shape for C.
- **#3662** (on upstream main, not in our base): readers prefer a local memory
  replica regardless of replica order. Needed if extra replicas should also
  spread read load.
- **#3400** (closed, work in progress): domain-aware placement for dynamic
  replicas. Not needed for churn.

## 3. Design

Principle: **a valuable object needs a copy that survives the likely event, on
the cheapest tier that still makes it a hit.** Every mechanism below follows
the liveness rules: no O(total keys) work and no unbounded lock hold on an RPC
IO thread, snapshot locks taken per shard section, and long passes that step
aside for waiting writers.

### 3.1 Heat: a decayed counter on each object (replaces the windows map)

Add a 4-byte `mutable` field to `ObjectMetadata`: a 16-bit count and a 16-bit
epoch. The epoch counts `--hot_heat_half_life_seconds` periods (default 600).
On each read in `GetReplicaList` and `BatchGetReplicaList`, under the
object's existing `SpinLock`, taken in the same place and the same way
`GrantReadLease` already updates the mutable lease:

```
count = (count >> min(16, now_epoch - epoch)) + 1   // saturating
epoch = now_epoch
```

- Exact per key, with no collisions, no global map, no new lock, and O(1) work.
  It costs 88 MB at 22M keys. A count-min sketch would save that memory but
  adds error and a shared structure on the hot path.
- Heat is read lazily (decayed to now) wherever a decision needs it: admission,
  repair, drain ordering and trimming.
- The threshold `--hot_admission_heat` (for example 3, meaning about three
  reads per half-life) is set from the P0 histogram, not guessed.

### 3.2 Standing hot replicas (change B)

Keep #3389's data path: proposals, leases, version fences, `dynamic_replicas`
records, `REPLICA_COPY`, and the cooldowns. Replace its policy:

1. **Admission.** When a read lifts a key's heat across the threshold, and the
   key has fewer than `k` (2) memory replicas on segments in `OK` status, the
   key is pushed onto the existing deduplicated admission queue. That happens
   on a threshold crossing only, so the push is rare. The worker runs off the
   IO threads.
2. **Placement** (in `SelectDynamicReplicaPlan`):
   - Exclude segments that are `DRAINING`, `GRACEFULLY_UNMOUNTING` or
     `UNMOUNTING`, and hosts that already hold a replica of the key. A
     different host is a hard requirement whenever one exists.
   - Rank by stability, then free ratio. Stability is the segment's mount age,
     which the Master records at mount (new, in memory only). The agent frees
     the newest chunk first and the baseline last, so older segments outlive
     newer ones on the same Store.
   - Make the target watermark a flag (from 88758ba8) and default it to the
     eviction watermark.
3. **Budget and charging.** Extra replicas are charged to a system quota
   tenant, `--hot_replica_quota_tenant` (for example `hot-replicas`), not to the
   object's tenant. Operators size it in `tenant_quotas.yaml` the same way as
   `reserved-headroom`, so tenants' effective quotas shrink by the budget, and
   it is exported by the existing `mooncake_tenant_quota_*` metrics.
   - A tenant at quota still gets protection (fixes G3).
   - Total memory accounting stays exact.
   - Admission stops while the budget is exhausted.
   - Accounting rule: a key's first live memory replica is always charged to
     its tenant, and replicas beyond it to the system tenant.
     `CompletedMemoryQuotaCharge` splits accordingly.
4. **Survivor promotion.** If the original replica is lost and only the
   dynamic one survives, the survivor becomes the original: its dynamic record
   is forgotten and its charge moves to the tenant. That can push the tenant
   over quota, which the background trim from the liveness work handles. If
   the key is still hot, repair (3.3) creates a new extra. So eviction and
   quota never treat the key's only copy as a disposable extra.
5. **Trimming without a registry.** No hot-set map exists to walk. Extras are
   reclaimed lazily by the passes that already visit keys:
   - Quota eviction and `BatchEvict` drop an extra replica (not the key) when
     the system tenant is over budget, or when the key's heat has decayed below
     half the threshold.
   - Whole-key eviction of a cold key frees all of its replicas, as today, and
     records the recreate cooldown.

   Admission is the only thing that adds extras, so the budget is enforced
   where it matters.

### 3.3 Repair after an unplanned loss

When a client expires or a segment is unmounted, the liveness cleanup worker
already visits every key in bounded batches to drop replicas on the retired
segment. Add one check per affected key: if it is still hot and now has fewer
than `k` replicas, enqueue a proposal. That is O(1) per affected key, with no
new walk and no registry. Repair inherits the cleanup worker's step-aside
behaviour.

### 3.4 Co-locate batch puts (change A): pin to the first object's chunk

SGLang writes a request's consecutive pages, and every key of each page, in one
`BatchPutStart`. Under `--colocate_batch_puts` (default off, Master only), the
batch's first successful memory replica fixes an **anchor chunk**: the
allocator, that is the one mounted segment, it landed in. Every later object of
the batch is placed:

1. in the anchor chunk, if it has room;
2. else in another chunk of the same Store, the first with room in mount order
   after the anchor, so a spill also stays together;
3. else by the normal ranked placement (`free_ratio_first`).

If the first object fails to allocate, the next one that succeeds becomes the
anchor. A client's own placement preference (`preferred_segment(s)`, or
`local_first`) wins over the anchor.

**Why the chunk and not the segment name.** On our elastic Stores every chunk
of a Store mounts under the same segment name: `Client::MountSegmentAndGetId`
sets `segment.name = local_hostname_`, so the Master's `AllocatorManager` holds
about five 64 GiB allocators under one name per Store. Within a name,
`allocateSingle` starts at a random allocator and takes the first that fits.
Preferring the first object's segment *name* would keep a batch on one Store
but still spread its ~63 objects over all of that Store's chunks. Elastic shrink
frees one chunk at a time, and it is the dominant churn (3.9 TiB in 10 h), so
under name pinning one chunk shrink still breaks nearly every batch on that
Store. For a ~200-key prefix written in ~4 batches, the share of prefixes one
64 GiB chunk shrink (0.45% of memory) breaks:

| Placement | Prefixes broken |
|-----------|-----------------|
| Today (per object) | about 59% |
| Pinned to the first object's segment name | about 9% (`1-(40/41)^4`) |
| Pinned to the first object's chunk | about 1.8% (`1-(1-0.0045)^4`) |

For a whole-Store exit or crash the two pinning choices are equivalent.

**The existing `prefer_alloc_in_same_node` path does not pin here.** It sets
`preferred_segment` to the first replica's `transport_endpoint_`. With
`metadata_server=P2PHANDSHAKE` (our deployment), the Store's segment name is
`POD_IP:<port>` from `RealClient`'s auto-bound port, while its transport
endpoint is `POD_IP:<handshake port>` that the transfer engine binds separately
(`findAvailableTcpPort`). They never match, so the preference names no segment
and placement silently falls back to ranked. It also pins only to a name.

Mechanics:

- The anchor is a `weak_ptr` to the allocator plus its segment name, carried in
  a per-batch `BatchPlacement` through `PutStart` and
  `AllocateAndInsertMetadata` to `AllocateNearAnchor` (in
  `allocation_strategy.h`), which runs under the same `ScopedAllocatorAccess`
  lock as the allocation. An allocator is used only while it is still a member
  of `getAllocators(name)`: unmount, graceful unmount and drain all remove it,
  and a graceful unmount keeps it alive, so a live `weak_ptr` alone is not
  enough. The anchor is never held as a `shared_ptr`: that would keep an
  unmounted chunk's replicas readable for the rest of the batch.
- Capacity balance is unchanged at batch granularity: a batch of up to a few
  hundred MB is small next to a 64 GiB chunk, and its first object is still
  placed by `free_ratio_first`.
- Cost: a prefix is read from fewer Stores, so each read has less parallelism
  and is more exposed to one hot Store. The trunk is the most-read part of a
  prefix, and B's extra replicas (plus #3662's local preference) spread it.
  P0's segments-spanned histograms, which count chunks as well as Stores, show
  whether this matters.
- A later refinement is parent-affine placement (place a batch next to its
  predecessor page). That needs a client hint and is out of scope.

### 3.5 Drain before free (change C)

Today the agent frees a chunk with grace 0, which drops every memory-only
object on it. Instead:

1. The agent calls a new binding, `drain_segments(ids, deadline_s)`. The Master
   marks the segments `DRAINING` (readable, no new allocations) and returns at
   once. The deadline is adaptive: short when a model pod is waiting (for
   example 10 s), longer for a pre-emptive shrink (for example 60 s), and zero
   when the deadline to shrink is near or memory pressure demands it.
2. A Master drain worker (one, coalescing every draining segment) walks the
   index in the cleanup worker's bounded, step-aside batches. For each object
   with a replica on a draining segment:
   - hot, and without another healthy replica: re-home it (a copy to another
     host, as in 3.2, charged to the system tenant);
   - otherwise, with no servable copy: queue an offload to the owning Store's
     own SSD through the existing offload queue. This is a local write, the SSD
     has spare capacity, and promotion-on-hit restores the object if it is
     read.

   Candidates are kept in a bounded heap ordered by heat, then recency (lease
   time), and sized to what the deadline allows, so the most valuable objects
   go first.
3. The agent polls `drain_status(ids)` (or simply sleeps) until the work is
   done or the deadline passes. It then frees with grace 0 exactly as today:
   the free path, its pinned-memory accounting and its blocked-file markers do
   not change.
4. **Fencing (required).** Before a segment's memory is freed, the Store must
   finish or cancel every in-flight local read of it, both offload writes and
   copies sourced from it. Otherwise it frees memory that its own threads are
   still reading. The drain worker stops issuing tasks for a segment when its
   deadline passes; the Store-side free waits for that segment's in-flight tasks
   (bounded) or cancels them.

The walk is O(total keys) per drain pass, about 15 s at 22M keys, taken in
bounded batches off the IO threads. That is acceptable for planned events,
which arrive minutes apart. The segment-to-replica index deferred in the
liveness notes would make it O(objects on the segment), and would speed up the
cleanup worker too. C could later be expressed as a DrainJob mode once #4436
lands, replacing that job's planner with this walk.

### 3.6 Copy engine

- A global in-flight cap for dynamic copies (`--hot_copy_max_inflight`, for
  example 256) and a per-source-Store cap, so repair after a large loss cannot
  flood a single Store.
- Client change (small, upstreamable): when a task fetch returns a full batch
  of 16, poll again immediately instead of sleeping 1 s.
- Repair estimate: a 400k-object hot set (5% budget at 1.8 MB), losing an
  average Store, needs about 10k copies spread across about 40 source Stores,
  which takes roughly 16 s at today's rates.

### 3.7 Metrics (all phases)

- `hot_heat_bucket` histogram, sampled on reads. "Would admit" counts at
  several thresholds.
- Churn misses (P0, 4.1), labelled by cause, by age since the drop, and by heat
  at drop time.
- Admission: proposals, copies started, completed, failed and refused (by
  reason: budget, no target, cooldown, quota). Extra-replica bytes. Survivor
  promotions. Repairs queued and completed.
- Drain: objects and bytes re-homed, offloaded, and abandoned at the deadline;
  drain latency.
- Placement: segments spanned per `BatchGetReplicaList` and per
  `BatchPutStart` (histograms), counted both as distinct chunks (allocators)
  and as distinct Stores (segment names). Only the chunk count sees what A
  changes.

## 4. Phases

### P0: measure (no behaviour change)

- The heat field (3.1) and the metrics (3.7). The heat-threshold crossing
  counters replace #3389's observe-mode `VLOG`: they give the admission rate
  any threshold would see, which is what sizing B needs.
- **Churn-miss filter.** Six time slices (10 minutes each by default) of 32-bit
  entries: a 24-bit fingerprint of (tenant, key) plus a 4-bit cause and a 4-bit
  heat bucket, in an open-addressed table at most half full, about 48 MB at the
  default 1M drops per slice. A Bloom filter cannot carry the labels. When the
  cleanup worker (or an eviction) drops an object's last servable copy, it
  inserts the object, labelled by cause (planned unmount, client expiry, disk
  unmount, eviction without a copy) and heat at the drop. The cause comes from
  a small registry of the allocators and disk registrations retired in the
  window, matched by `weak_ptr` owner, so it is exact per chunk. A miss in
  `ExistKey`, `BatchExistKey`, `GetReplicaList` or `BatchGetReplicaList`
  checks the filter; a miss on an object still awaiting cleanup is attributed
  directly from its lost replica (age `pending`). Cost lands on misses only,
  and lookups are lock-free.

  The result is the rate of lookups for objects that churn destroyed less than
  an hour earlier: an upper bound on what A, B and C together can recover, with
  attribution:
  - hot at drop time: B would have saved it;
  - dropped by a planned unmount: C would have;
  - and so on.
- **Exit criteria.** Churn misses are at least about 1% of exist lookups, or
  the SGLang hit-rate dip around unmounts is confirmed. In either case,
  proceed. Size B's budget from the hot set's size at the chosen threshold.

### 4.1 What P0 and A shipped

| Flag | Default | Effect |
|------|---------|--------|
| `--enable_kv_churn_metrics` | false | Heat on every served read, the churn-miss filter and registry, and the `master_kv_*` metrics |
| `--hot_heat_half_life_seconds` | 600 | Heat half-life |
| `--churn_miss_window_seconds` | 3600 | How long a drop is remembered |
| `--churn_miss_slice_capacity` | 1048576 | Drops per slice (8 bytes each) |
| `--colocate_batch_puts` | false | Change A (3.4) |

| Metric | Labels | Reads as |
|--------|--------|----------|
| `master_kv_read_heat` | histogram | Heat of each read object, after the read |
| `master_kv_heat_crossings_total`, `_bytes_total` | `threshold` (2, 3, 4, 8, 16) | B's admission rate at that threshold |
| `master_kv_churn_drops_total`, `master_kv_churn_drop_bytes_total` | `cause`, `heat` | Objects churn dropped, and how hot they were |
| `master_kv_churn_degraded_total` | `cause` | Objects that lost a replica but kept a servable one |
| `master_kv_lookup_misses_total` | `api` (exist, get) | All misses: the denominator |
| `master_kv_churn_misses_total` | `api`, `cause`, `heat` | Misses on objects churn dropped within the window |
| `master_kv_churn_miss_age_total` | `api`, `age_slice` (pending, 0 to 5) | The same, by time since the drop |
| `master_kv_batch_exist_usable_keys_total`, `_stranded_keys_total` | | Keys before the first miss of a batch exist, and keys present after it (cached but unusable by SGLang) |
| `master_kv_batch_exist_first_miss_total` | `cause` (or `none`) | Prefix breaks, by what broke them |
| `master_kv_churn_filter_overflow_total` | | Drops not remembered (slice full) |
| `master_kv_batch_segments_spanned` | `op` (put, get), `level` (chunk, store) | Placement spread per batch |

Exit criteria read straight off these: churn misses over lookup misses, the
`first_miss` share by cause, and stranded over usable keys.

### P1: A plus B

- Co-locating batch puts first, because it is the smallest change. Canary it,
  then compare churn misses and SGLang hit rate around unmounts.
- Then the standing replicas, in observe mode, then enforce mode, with the
  budget starting small (2%).

### P2: C

- Agent and binding changes, the drain worker, and the fencing.
- If P0 shows planned-unmount misses dominate and A falls short, C can move
  ahead of B.

## 5. Test plan

- **Unit:**
  - heat decay and saturation;
  - admission on a threshold crossing;
  - placement excludes draining segments and same-host targets, and prefers
    older segments;
  - charging splits between tenant and system;
  - survivor promotion moves the charge;
  - lazy trimming of extras when over budget;
  - repair enqueued by the cleanup worker;
  - drain ordering, its deadline, and fencing;
  - batch co-location in one chunk, its fallback to a sibling chunk and then
    ranked placement, and skipping an unmounted or unmounting anchor;
  - churn-filter attribution.

  Each test must fail without the change it covers, as in the liveness suite.
- **Liveness:** rerun `master_liveness_isolation_test`. Add a benchmark for
  `BatchGetReplicaList` with the heat update, which must not regress p99.
- **Churn harness (eu-west1, held nodes, as in the liveness A/B):**
  - extend `swarm.py` with a prefix workload: chains of K keys, written in one
    batch and read from the start with first-miss semantics, under a skewed
    (Zipf) trunk distribution;
  - drive shrinks, Store exits, crashes and evictions;
  - compare the usable prefix length and the churn-miss rate across baseline,
    A, A+B and A+B+C;
  - confirm that no client expires and that Ping latency is unchanged.
- **Canary:** SGLang `cache_hit_rate` and the cached-token rate around unmounts,
  and Master p99 RPC latency.

## 6. Alternatives considered

- **`replica_num=2` for every object.** Doubles memory for an even smaller
  hit-rate gain per byte. Rejected.
- **Write-through of hot objects to their own Store's SSD** instead of a second
  memory replica. Cheap (the SSD has spare capacity) and survives shrink,
  restart and exit, but not node loss, and it serves SSD hits. A fallback for
  B if the memory budget is contested. C's offload path already covers planned
  events this way.
- **Free the oldest elastic chunk instead of the newest.** `free_ratio_first`
  sends almost every write to a freshly mounted chunk, but at 3.9 GiB/s a 64
  GiB chunk reaches parity in about 20 s, so chunk age barely predicts heat.
  Little to gain.
- **A hot-set registry or count-min sketch.** More state to keep consistent.
  Per-object heat plus the existing walks covers admission, repair, trimming
  and drain without one.
- **Upstream DrainJob as it stands.** Its full scans under the snapshot lock
  are unsafe at our scale (2.2).

## 7. Open questions

1. **Budget for B.** What fraction of memory may extra replicas take, and
   should they be charged to a system tenant (proposed) rather than to the
   object's tenant (upstream)?
2. **Drain deadline for C.** How long can a shrink wait before its memory goes
   back to a starting model pod?
3. **Change A.** Co-location is not replication. Should it be part of this
   project or a separate track?
4. **Upstreaming.** Should A, B and C be designed as upstreamable follow-ups to
   #3389 and DrainJob, at some cost in generality, or kept as Cloudflare
   patches?
