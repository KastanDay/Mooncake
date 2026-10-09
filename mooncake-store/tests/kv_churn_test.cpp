// Churn measurement (--enable_kv_churn_metrics) and batch co-location
// (--colocate_batch_puts): phases P0 and A of
// docs/source/design/store/hot-replica-churn.md.
//
// An elastic Store mounts every 64 GiB chunk of its memory under one segment
// name, and elastic shrink frees one chunk at a time. Placing a batch on one
// Store is therefore not enough: it must land in one chunk, or a single
// shrink breaks nearly every prefix written to that Store.

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "kv_churn.h"
#include "master_service.h"
#include "rpc_service.h"
#include "types.h"

namespace mooncake::test {

using std::chrono::seconds;
using Clock = std::chrono::steady_clock;

class KvChurnTest : public ::testing::Test {
   protected:
    static constexpr size_t kMiB = 1024 * 1024;

    void SetUp() override {
        google::InitGoogleLogging("KvChurnTest");
        FLAGS_logtostderr = true;
    }
    void TearDown() override { google::ShutdownGoogleLogging(); }

    static MasterServiceConfig Config(bool colocate, bool metrics) {
        MasterServiceConfig config;
        config.default_kv_lease_ttl = 0;
        config.client_live_ttl_sec = 3600;
        config.allocation_strategy_type =
            AllocationStrategyType::FREE_RATIO_FIRST;
        config.kv_churn.enable_metrics = metrics;
        config.kv_churn.colocate_batch_puts = colocate;
        return config;
    }

    static Segment MakeChunk(const std::string& name, uintptr_t base,
                             size_t size) {
        Segment segment;
        segment.id = generate_uuid();
        segment.name = name;
        segment.base = base;
        segment.size = size;
        segment.te_endpoint = name;
        return segment;
    }

    // Mounts `count` chunks of `size` bytes under one segment name, the way
    // an elastic Store mounts its memory.
    template <typename Service>
    static std::vector<Segment> MountStore(Service& service, const UUID& client,
                                           const std::string& name,
                                           uintptr_t base, size_t count,
                                           size_t size) {
        std::vector<Segment> chunks;
        for (size_t i = 0; i < count; ++i) {
            chunks.push_back(MakeChunk(name, base + i * size, size));
            EXPECT_TRUE(
                service.MountSegment(chunks.back(), client).has_value());
        }
        return chunks;
    }

    // Index into `chunks` of the chunk holding a replica, or -1.
    static int ChunkOf(const std::vector<Segment>& chunks,
                       const std::vector<Replica::Descriptor>& replicas) {
        for (const auto& replica : replicas) {
            if (!replica.is_memory_replica()) continue;
            const auto address = replica.get_memory_descriptor()
                                     .buffer_descriptor.buffer_address_;
            for (size_t i = 0; i < chunks.size(); ++i) {
                if (address >= chunks[i].base &&
                    address < chunks[i].base + chunks[i].size) {
                    return static_cast<int>(i);
                }
            }
        }
        return -1;
    }

    static tl::expected<std::vector<Replica::Descriptor>, ErrorCode> Put(
        MasterService& service, const UUID& client, const std::string& key,
        size_t size, MasterService::BatchPlacement* placement,
        const std::string& preferred_segment = "") {
        ReplicateConfig config;
        config.replica_num = 1;
        config.preferred_segment = preferred_segment;
        return service.PutStart(client, key, TenantId::Default(), size, config,
                                placement);
    }

    static void PutAndEnd(MasterService& service, const UUID& client,
                          const std::string& key, size_t size,
                          MasterService::BatchPlacement* placement = nullptr,
                          const std::string& preferred_segment = "") {
        ASSERT_TRUE(
            Put(service, client, key, size, placement, preferred_segment)
                .has_value());
        ASSERT_TRUE(
            service
                .PutEnd(client, key, TenantId::Default(), ReplicaType::MEMORY)
                .has_value());
    }

    // The value of the first metric line named `name` whose labels include
    // every one of `labels` (each `k="v"`), and no `le` label. 0 if absent.
    static double Metric(const std::string& name,
                         const std::vector<std::string>& labels = {}) {
        std::istringstream text(KvChurnMetrics::instance().Serialize());
        std::string line;
        while (std::getline(text, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (line.rfind(name + "{", 0) != 0 &&
                line.rfind(name + " ", 0) != 0)
                continue;
            if (line.find("le=") != std::string::npos) continue;
            bool all = true;
            for (const auto& label : labels) {
                all = all && line.find(label) != std::string::npos;
            }
            if (all) {
                return std::stod(line.substr(line.rfind(' ') + 1));
            }
        }
        return 0;
    }

    static void StopCleanupWorker(MasterService& service) {
        service.replica_cleanup_worker_.Stop();
    }
    static void RunCleanup(MasterService& service) {
        service.ClearInvalidHandles(/*lock_snapshot_per_batch=*/false);
    }
    static void Expire(MasterService& service, const UUID& client) {
        service.ExpireClients({client}, Clock::now() + std::chrono::hours(2));
    }
    static void EvictWithoutCopy(MasterService& service, const std::string& key,
                                 uint64_t size) {
        MasterService::ObjectMetadata metadata(
            generate_uuid(), std::chrono::system_clock::now(), size, {});
        service.PublishKvRemovedAfterEvict(key, size, "cpu", metadata,
                                           TenantId::Default());
    }
};

// ---------------------------------------------------------------------------
// Change A: co-locate a batch put's objects in one chunk.

// Two Stores of five chunks each. A BatchPutStart of 40 objects lands in a
// single chunk; without co-location it spreads over most of a Store's chunks.
TEST_F(KvChurnTest, BatchPutLandsInOneChunk) {
    WrappedMasterServiceConfig config;
    config.default_kv_lease_ttl = 0;
    config.allocation_strategy_type = AllocationStrategyType::FREE_RATIO_FIRST;
    config.kv_churn.colocate_batch_puts = true;
    config.kv_churn.enable_metrics = true;
    WrappedMasterService service(config);
    const UUID client_a = generate_uuid();
    const UUID client_b = generate_uuid();
    auto chunks =
        MountStore(service, client_a, "store-a", 0x100000000, 5, 64 * kMiB);
    auto store_b =
        MountStore(service, client_b, "store-b", 0x200000000, 5, 64 * kMiB);
    chunks.insert(chunks.end(), store_b.begin(), store_b.end());

    std::vector<std::string> keys;
    for (int i = 0; i < 40; ++i) keys.push_back("batch-" + std::to_string(i));
    const std::vector<uint64_t> sizes(keys.size(), 256 * 1024);
    const double spans_before = Metric("master_kv_batch_segments_spanned_sum",
                                       {"op=\"put\"", "level=\"chunk\""});

    auto results = service.BatchPutStart(client_a, keys, sizes, {});
    std::set<int> used;
    for (const auto& result : results) {
        ASSERT_TRUE(result.has_value());
        used.insert(ChunkOf(chunks, result.value()));
    }
    EXPECT_EQ(used.size(), 1u);
    EXPECT_NE(*used.begin(), -1);
    EXPECT_EQ(Metric("master_kv_batch_segments_spanned_sum",
                     {"op=\"put\"", "level=\"chunk\""}) -
                  spans_before,
              1);
}

// With the flag off, a batch is placed object by object as before.
TEST_F(KvChurnTest, FlagOffKeepsPerObjectPlacement) {
    WrappedMasterServiceConfig config;
    config.default_kv_lease_ttl = 0;
    config.allocation_strategy_type = AllocationStrategyType::FREE_RATIO_FIRST;
    config.kv_churn.enable_metrics = true;
    WrappedMasterService service(config);
    const UUID client_a = generate_uuid();
    const UUID client_b = generate_uuid();
    auto chunks =
        MountStore(service, client_a, "store-a", 0x100000000, 5, 64 * kMiB);
    auto store_b =
        MountStore(service, client_b, "store-b", 0x200000000, 5, 64 * kMiB);
    chunks.insert(chunks.end(), store_b.begin(), store_b.end());

    std::vector<std::string> keys;
    for (int i = 0; i < 40; ++i) keys.push_back("spread-" + std::to_string(i));
    const std::vector<uint64_t> sizes(keys.size(), 256 * 1024);
    const double spans_before = Metric("master_kv_batch_segments_spanned_sum",
                                       {"op=\"put\"", "level=\"chunk\""});

    auto results = service.BatchPutStart(client_a, keys, sizes, {});
    std::set<int> used;
    for (const auto& result : results) {
        ASSERT_TRUE(result.has_value());
        used.insert(ChunkOf(chunks, result.value()));
    }
    // 40 objects drawn over ten chunks: one chunk by chance is ~10^-27.
    EXPECT_GT(used.size(), 1u);
    EXPECT_EQ(Metric("master_kv_batch_segments_spanned_sum",
                     {"op=\"put\"", "level=\"chunk\""}) -
                  spans_before,
              static_cast<double>(used.size()));
}

// The anchor chunk fills mid-batch: the batch moves to one sibling chunk of
// the same Store, and only when the whole Store is full to the ranked
// placement (the other Store).
TEST_F(KvChurnTest, FullAnchorSpillsToSiblingThenRanked) {
    MasterService service(Config(/*colocate=*/true, /*metrics=*/false));
    const UUID client_a = generate_uuid();
    const UUID client_b = generate_uuid();
    // Store B exists first and is half full, so the batch's first object
    // goes to the emptier Store A.
    auto chunks =
        MountStore(service, client_b, "store-b", 0x200000000, 1, 16 * kMiB);
    PutAndEnd(service, client_b, "filler", 8 * kMiB);
    auto store_a =
        MountStore(service, client_a, "store-a", 0x100000000, 2, 1 * kMiB);
    chunks.insert(chunks.end(), store_a.begin(), store_a.end());
    constexpr int kStoreB = 0;

    auto placement = service.BeginBatchPlacement();
    ASSERT_TRUE(placement.has_value());
    std::vector<int> sequence;
    for (int i = 0; i < 30; ++i) {
        auto result = Put(service, client_a, "spill-" + std::to_string(i),
                          128 * 1024, &*placement);
        ASSERT_TRUE(result.has_value()) << i;
        sequence.push_back(ChunkOf(chunks, result.value()));
    }

    // Runs: anchor chunk, then the sibling, then Store B; no chunk twice.
    std::vector<int> runs;
    for (int chunk : sequence) {
        if (runs.empty() || runs.back() != chunk) runs.push_back(chunk);
    }
    ASSERT_EQ(runs.size(), 3u) << ::testing::PrintToString(sequence);
    EXPECT_NE(runs[0], kStoreB);
    EXPECT_NE(runs[1], kStoreB);
    EXPECT_NE(runs[0], runs[1]);
    EXPECT_EQ(runs[2], kStoreB);
}

// The anchor chunk is unmounted between two keys of a batch: later objects
// never go to it, and stay together in one sibling chunk.
TEST_F(KvChurnTest, UnmountedAnchorIsSkipped) {
    MasterService service(Config(/*colocate=*/true, /*metrics=*/false));
    const UUID client_a = generate_uuid();
    const UUID client_b = generate_uuid();
    auto chunks =
        MountStore(service, client_b, "store-b", 0x200000000, 1, 64 * kMiB);
    PutAndEnd(service, client_b, "filler", 32 * kMiB);
    auto store_a =
        MountStore(service, client_a, "store-a", 0x100000000, 3, 64 * kMiB);
    chunks.insert(chunks.end(), store_a.begin(), store_a.end());

    auto placement = service.BeginBatchPlacement();
    auto first = Put(service, client_a, "unmount-0", 256 * 1024, &*placement);
    ASSERT_TRUE(first.has_value());
    const int anchor = ChunkOf(chunks, first.value());
    ASSERT_GE(anchor, 1);  // in Store A
    ASSERT_TRUE(
        service.UnmountSegment(chunks[anchor].id, client_a).has_value());

    std::set<int> used;
    for (int i = 1; i <= 10; ++i) {
        auto result = Put(service, client_a, "unmount-" + std::to_string(i),
                          256 * 1024, &*placement);
        ASSERT_TRUE(result.has_value());
        used.insert(ChunkOf(chunks, result.value()));
    }
    EXPECT_FALSE(used.contains(anchor));
    ASSERT_EQ(used.size(), 1u);
    EXPECT_GE(*used.begin(), 1);  // a sibling in Store A
}

// The same with a graceful unmount, which keeps the chunk's allocator alive
// (a dead weak_ptr is not the signal) but no longer allocatable.
TEST_F(KvChurnTest, GracefullyUnmountingAnchorIsSkipped) {
    MasterService service(Config(/*colocate=*/true, /*metrics=*/false));
    const UUID client_a = generate_uuid();
    const UUID client_b = generate_uuid();
    auto chunks =
        MountStore(service, client_b, "store-b", 0x200000000, 1, 64 * kMiB);
    PutAndEnd(service, client_b, "filler", 32 * kMiB);
    auto store_a =
        MountStore(service, client_a, "store-a", 0x100000000, 3, 64 * kMiB);
    chunks.insert(chunks.end(), store_a.begin(), store_a.end());

    auto placement = service.BeginBatchPlacement();
    auto first = Put(service, client_a, "graceful-0", 256 * 1024, &*placement);
    ASSERT_TRUE(first.has_value());
    const int anchor = ChunkOf(chunks, first.value());
    ASSERT_GE(anchor, 1);
    ASSERT_TRUE(service
                    .GracefulUnmountSegment(chunks[anchor].id, client_a,
                                            /*grace_period_ms=*/3600 * 1000)
                    .has_value());
    ASSERT_FALSE(placement->anchor.expired());

    std::set<int> used;
    for (int i = 1; i <= 10; ++i) {
        auto result = Put(service, client_a, "graceful-" + std::to_string(i),
                          256 * 1024, &*placement);
        ASSERT_TRUE(result.has_value());
        used.insert(ChunkOf(chunks, result.value()));
    }
    EXPECT_FALSE(used.contains(anchor));
    ASSERT_EQ(used.size(), 1u);
    EXPECT_GE(*used.begin(), 1);
}

// A client's explicit preferred segment wins over the anchor.
TEST_F(KvChurnTest, ClientPreferenceOverridesAnchor) {
    MasterService service(Config(/*colocate=*/true, /*metrics=*/false));
    const UUID client_a = generate_uuid();
    const UUID client_b = generate_uuid();
    auto chunks =
        MountStore(service, client_a, "store-a", 0x100000000, 1, 64 * kMiB);
    auto store_b =
        MountStore(service, client_b, "store-b", 0x200000000, 1, 64 * kMiB);
    chunks.insert(chunks.end(), store_b.begin(), store_b.end());

    auto placement = service.BeginBatchPlacement();
    auto first = Put(service, client_a, "pref-0", 1024, &*placement, "store-a");
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(ChunkOf(chunks, first.value()), 0);
    auto second =
        Put(service, client_a, "pref-1", 1024, &*placement, "store-b");
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(ChunkOf(chunks, second.value()), 1);
}

// ---------------------------------------------------------------------------
// P0: measurement.

TEST_F(KvChurnTest, HeatDecaysByHalfLifeAndSaturates) {
    std::atomic<uint32_t> cell{0};
    EXPECT_EQ(kv_heat::RecordRead(cell, 10), std::make_pair(0u, 1u));
    EXPECT_EQ(kv_heat::RecordRead(cell, 10), std::make_pair(1u, 2u));
    EXPECT_EQ(kv_heat::RecordRead(cell, 10), std::make_pair(2u, 3u));
    EXPECT_EQ(kv_heat::Decayed(cell.load(), 11), 1u);  // 3 >> 1
    EXPECT_EQ(kv_heat::Decayed(cell.load(), 40), 0u);
    EXPECT_EQ(kv_heat::RecordRead(cell, 11), std::make_pair(1u, 2u));
    // The epoch wraps at 2^16 half-lives.
    std::atomic<uint32_t> wrapped{(4u << 16) | 0xFFFF};
    EXPECT_EQ(kv_heat::Decayed(wrapped.load(), 0), 2u);
    std::atomic<uint32_t> full{0xFFFFu << 16 | 7};
    EXPECT_EQ(kv_heat::RecordRead(full, 7).second, 0xFFFFu);
    EXPECT_EQ(HeatBucket(1), 0u);
    EXPECT_EQ(HeatBucket(3), 2u);
    EXPECT_EQ(HeatBucket(15), 4u);
    EXPECT_EQ(HeatBucket(300), 5u);
}

TEST_F(KvChurnTest, MissFilterRemembersForOneWindow) {
    ChurnMissFilter filter(seconds(60), /*slice_capacity=*/4);
    const auto t0 = Clock::time_point(seconds(6000));
    const uint64_t key = ChurnKeyHash("default", "k");
    EXPECT_FALSE(filter.Find(key, t0).has_value());
    ASSERT_TRUE(filter.Insert(key, ChurnCause::kUnmount, 3, t0));

    auto hit = filter.Find(key, t0 + seconds(5));
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->cause, ChurnCause::kUnmount);
    EXPECT_EQ(hit->heat_bucket, 3u);
    EXPECT_EQ(hit->age_slice, 0u);
    hit = filter.Find(key, t0 + seconds(25));
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->age_slice, 2u);
    EXPECT_FALSE(filter.Find(key, t0 + seconds(70)).has_value());
    EXPECT_FALSE(
        filter.Find(ChurnKeyHash("other-tenant", "k"), t0).has_value());

    // A slice holds slice_capacity drops; a newer drop of the same key
    // replaces the old one.
    for (int i = 1; i < 4; ++i) {
        ASSERT_TRUE(filter.Insert(ChurnKeyHash("default", std::to_string(i)),
                                  ChurnCause::kClientExpiry, 0, t0));
    }
    EXPECT_FALSE(filter.Insert(ChurnKeyHash("default", "5"),
                               ChurnCause::kClientExpiry, 0, t0));
    ASSERT_TRUE(filter.Insert(key, ChurnCause::kEvictMemory, 1, t0));
    EXPECT_EQ(filter.Find(key, t0)->cause, ChurnCause::kEvictMemory);
}

// An object on an unmounted chunk: a miss before cleanup is a pending churn
// miss, the cleanup pass records the drop with the heat it had, and a miss
// after it is found in the filter.
TEST_F(KvChurnTest, UnmountedObjectMissesAreChurnMisses) {
    MasterService service(Config(/*colocate=*/false, /*metrics=*/true));
    StopCleanupWorker(service);
    const UUID client = generate_uuid();
    auto chunks =
        MountStore(service, client, "store-u", 0x100000000, 2, 64 * kMiB);
    PutAndEnd(service, client, "u-key", 4096);
    const double crossings3 =
        Metric("master_kv_heat_crossings_total", {"threshold=\"3\""});
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(
            service.GetReplicaList("u-key", TenantId::Default()).has_value());
    }
    EXPECT_EQ(Metric("master_kv_heat_crossings_total", {"threshold=\"3\""}) -
                  crossings3,
              1);

    const auto miss = std::vector<std::string>{
        "api=\"exist\"", "cause=\"unmount\"", "heat=\"3\""};
    const double misses = Metric("master_kv_churn_misses_total", miss);
    const double pending = Metric("master_kv_churn_miss_age_total",
                                  {"api=\"exist\"", "age_slice=\"pending\""});
    const double fresh = Metric("master_kv_churn_miss_age_total",
                                {"api=\"exist\"", "age_slice=\"0\""});
    const double drops = Metric("master_kv_churn_drops_total",
                                {"cause=\"unmount\"", "heat=\"3\""});

    for (const auto& chunk : chunks) {
        ASSERT_TRUE(service.UnmountSegment(chunk.id, client).has_value());
    }
    EXPECT_FALSE(service.ExistKey("u-key", TenantId::Default()).value());
    EXPECT_EQ(Metric("master_kv_churn_misses_total", miss) - misses, 1);
    EXPECT_EQ(Metric("master_kv_churn_miss_age_total",
                     {"api=\"exist\"", "age_slice=\"pending\""}) -
                  pending,
              1);
    // The batch paths see the same pending loss.
    const double first_miss =
        Metric("master_kv_batch_exist_first_miss_total", {"cause=\"unmount\""});
    const auto get_miss = std::vector<std::string>{
        "api=\"get\"", "cause=\"unmount\"", "heat=\"3\""};
    const double get_misses = Metric("master_kv_churn_misses_total", get_miss);
    service.BatchExistKey({"u-key"}, TenantId::Default());
    EXPECT_FALSE(service.BatchGetReplicaList({"u-key"}, TenantId::Default())[0]
                     .has_value());
    EXPECT_EQ(Metric("master_kv_batch_exist_first_miss_total",
                     {"cause=\"unmount\""}) -
                  first_miss,
              1);
    EXPECT_EQ(Metric("master_kv_churn_misses_total", get_miss) - get_misses, 1);

    RunCleanup(service);
    EXPECT_EQ(Metric("master_kv_churn_drops_total",
                     {"cause=\"unmount\"", "heat=\"3\""}) -
                  drops,
              1);
    EXPECT_FALSE(service.ExistKey("u-key", TenantId::Default()).value());
    EXPECT_EQ(Metric("master_kv_churn_misses_total", miss) - misses, 3);
    EXPECT_EQ(Metric("master_kv_churn_miss_age_total",
                     {"api=\"exist\"", "age_slice=\"0\""}) -
                  fresh,
              1);
}

// A Store expires: its objects' loss is attributed to client expiry, and a
// prefix-ordered batch exist reports the first miss's cause and the keys
// stranded behind it.
TEST_F(KvChurnTest, BatchExistAttributesFirstMissAndStrandedKeys) {
    MasterService service(Config(/*colocate=*/false, /*metrics=*/true));
    StopCleanupWorker(service);
    const UUID survivor = generate_uuid();
    const UUID doomed = generate_uuid();
    MountStore(service, survivor, "store-s", 0x100000000, 1, 64 * kMiB);
    for (const char* key : {"p0", "p2", "p3"}) {
        PutAndEnd(service, survivor, key, 4096);
    }
    MountStore(service, doomed, "store-d", 0x200000000, 1, 64 * kMiB);
    PutAndEnd(service, doomed, "p1", 4096, nullptr, "store-d");
    ASSERT_TRUE(service.Ping(doomed).has_value());
    Expire(service, doomed);
    RunCleanup(service);

    const double usable = Metric("master_kv_batch_exist_usable_keys_total");
    const double stranded = Metric("master_kv_batch_exist_stranded_keys_total");
    const double first_miss = Metric("master_kv_batch_exist_first_miss_total",
                                     {"cause=\"client_expiry\""});
    const double untracked =
        Metric("master_kv_batch_exist_first_miss_total", {"cause=\"none\""});
    const double lookups =
        Metric("master_kv_lookup_misses_total", {"api=\"exist\""});

    auto results =
        service.BatchExistKey({"p0", "p1", "p2", "p3"}, TenantId::Default());
    ASSERT_EQ(results.size(), 4u);
    EXPECT_TRUE(results[0].value());
    EXPECT_FALSE(results[1].value());
    EXPECT_TRUE(results[2].value());
    EXPECT_TRUE(results[3].value());
    EXPECT_EQ(Metric("master_kv_batch_exist_usable_keys_total") - usable, 1);
    EXPECT_EQ(Metric("master_kv_batch_exist_stranded_keys_total") - stranded,
              2);
    EXPECT_EQ(Metric("master_kv_batch_exist_first_miss_total",
                     {"cause=\"client_expiry\""}) -
                  first_miss,
              1);

    // A key never written is a miss churn does not explain.
    service.BatchExistKey({"never-written", "p0"}, TenantId::Default());
    EXPECT_EQ(
        Metric("master_kv_batch_exist_first_miss_total", {"cause=\"none\""}) -
            untracked,
        1);
    EXPECT_EQ(
        Metric("master_kv_lookup_misses_total", {"api=\"exist\""}) - lookups,
        2);
}

// Eviction that leaves no servable copy is remembered as evict_memory.
TEST_F(KvChurnTest, EvictionWithoutCopyIsRemembered) {
    MasterService service(Config(/*colocate=*/false, /*metrics=*/true));
    const double misses = Metric("master_kv_churn_misses_total",
                                 {"api=\"get\"", "cause=\"evict_memory\""});
    EvictWithoutCopy(service, "evicted", 4096);
    EXPECT_FALSE(
        service.GetReplicaList("evicted", TenantId::Default()).has_value());
    EXPECT_EQ(Metric("master_kv_churn_misses_total",
                     {"api=\"get\"", "cause=\"evict_memory\""}) -
                  misses,
              1);
}

// The read-side span counts chunks, not segment names: a co-located batch
// is read from one chunk, a scattered one from several chunks of one Store.
TEST_F(KvChurnTest, BatchGetSpanCountsChunks) {
    for (const bool colocate : {true, false}) {
        MasterService service(Config(colocate, /*metrics=*/true));
        const UUID client = generate_uuid();
        MountStore(service, client, "store-g", 0x100000000, 5, 64 * kMiB);
        auto placement = service.BeginBatchPlacement();
        std::vector<std::string> keys;
        for (int i = 0; i < 30; ++i) {
            keys.push_back("g-" + std::to_string(i));
            PutAndEnd(service, client, keys.back(), 256 * 1024, &*placement);
        }
        const std::vector<std::string> chunk{"op=\"get\"", "level=\"chunk\""};
        const std::vector<std::string> store{"op=\"get\"", "level=\"store\""};
        const double chunks_before =
            Metric("master_kv_batch_segments_spanned_sum", chunk);
        const double stores_before =
            Metric("master_kv_batch_segments_spanned_sum", store);
        auto results = service.BatchGetReplicaList(keys, TenantId::Default());
        for (const auto& result : results) ASSERT_TRUE(result.has_value());
        const double chunks =
            Metric("master_kv_batch_segments_spanned_sum", chunk) -
            chunks_before;
        EXPECT_EQ(Metric("master_kv_batch_segments_spanned_sum", store) -
                      stores_before,
                  1);
        if (colocate) {
            EXPECT_EQ(chunks, 1) << "colocate";
        } else {
            EXPECT_GT(chunks, 1) << "scattered";
        }
    }
}

}  // namespace mooncake::test
