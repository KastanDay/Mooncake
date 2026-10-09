#pragma once

// Measures what Store churn costs the KV cache: per-object read heat, the
// objects churn drops, the lookups that later miss them, and how many
// segments a batch spans. Phase P0 of
// docs/source/design/store/hot-replica-churn.md. Observation only.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/functional/hash.hpp>

#include "types.h"
#include "ylt/metric/counter.hpp"
#include "ylt/metric/histogram.hpp"

namespace mooncake {

class BufferAllocatorBase;

// Why an object lost its last servable copy.
enum class ChurnCause : uint8_t {
    kUnknown = 0,       // a lost replica no recorded event explains
    kUnmount = 1,       // its memory segment was unmounted (planned)
    kClientExpiry = 2,  // its Store stopped pinging and expired
    kDiskUnmount = 3,   // its Store unmounted the local disk holding it
    kEvictMemory = 4,   // memory eviction left no servable copy
    kEvictDisk = 5,     // disk eviction left no servable copy
};
inline constexpr size_t kChurnCauseCount = 6;
const char* ChurnCauseName(ChurnCause cause);

// Read heat: reads per half-life, halved at every half-life boundary. One
// 32-bit word per object: the count in the high 16 bits (saturating), the
// half-life epoch of the last read in the low 16.
namespace kv_heat {

inline uint32_t Decayed(uint32_t packed, uint16_t now_epoch) {
    const uint32_t count = packed >> 16;
    const uint16_t age = static_cast<uint16_t>(now_epoch - (packed & 0xFFFF));
    return age >= 16 ? 0 : count >> age;
}

// Counts one read. Returns the heat before and after it.
inline std::pair<uint32_t, uint32_t> RecordRead(std::atomic<uint32_t>& cell,
                                                uint16_t now_epoch) {
    uint32_t packed = cell.load(std::memory_order_relaxed);
    for (;;) {
        const uint32_t before = Decayed(packed, now_epoch);
        const uint32_t after = before < 0xFFFF ? before + 1 : before;
        if (cell.compare_exchange_weak(packed, (after << 16) | now_epoch,
                                       std::memory_order_relaxed)) {
            return {before, after};
        }
    }
}

}  // namespace kv_heat

// Heat buckets used as metric labels: 0-1, 2, 3, 4-7, 8-15, 16+.
inline constexpr size_t kHeatBucketCount = 6;
size_t HeatBucket(uint32_t heat);
const char* HeatBucketName(size_t bucket);

// Hash identifying (tenant, key) in the churn-miss filter.
uint64_t ChurnKeyHash(const std::string& tenant, const std::string& key);

// Remembers which objects churn dropped in the last window: time slices of
// 32-bit entries, each a 24-bit fingerprint plus the drop's cause and heat
// bucket. Insert and Find are lock-free apart from a slice's reuse. False
// positives are about 1 in 10^6 lookups.
class ChurnMissFilter {
   public:
    static constexpr size_t kSlices = 6;
    static constexpr size_t kMaxProbe = 16;

    struct Hit {
        ChurnCause cause;
        size_t heat_bucket;
        size_t age_slice;  // 0 = the newest slice
    };

    ChurnMissFilter(std::chrono::seconds window, size_t slice_capacity);

    // Returns false when the current slice is full.
    bool Insert(uint64_t key_hash, ChurnCause cause, size_t heat_bucket,
                std::chrono::steady_clock::time_point now);
    std::optional<Hit> Find(uint64_t key_hash,
                            std::chrono::steady_clock::time_point now) const;

    std::chrono::seconds slice_length() const { return slice_length_; }

   private:
    struct Slice {
        std::atomic<int64_t> id{-1};
        std::atomic<size_t> size{0};
        std::unique_ptr<std::atomic<uint32_t>[]> slots;
    };

    int64_t SliceId(std::chrono::steady_clock::time_point now) const;
    Slice& CurrentSlice(int64_t id);

    const std::chrono::seconds slice_length_;
    const size_t slice_capacity_;
    const size_t mask_;
    std::vector<Slice> slices_;
    std::mutex reuse_mutex_;
};

// Remembers the event that retired each memory segment (by its allocator)
// and each local-disk registration (by its owner) over the last window, so
// a replica found lost later can be attributed to it.
class ChurnLossRegistry {
   public:
    explicit ChurnLossRegistry(std::chrono::seconds retention)
        : retention_(retention) {}

    void RecordAllocator(const std::weak_ptr<BufferAllocatorBase>& allocator,
                         ChurnCause cause,
                         std::chrono::steady_clock::time_point now);
    void RecordDiskOwner(const UUID& client_id, ChurnCause cause,
                         std::chrono::steady_clock::time_point now);

    ChurnCause AllocatorCause(
        const std::weak_ptr<BufferAllocatorBase>& allocator) const;
    ChurnCause DiskOwnerCause(const UUID& client_id) const;

   private:
    struct Entry {
        ChurnCause cause;
        std::chrono::steady_clock::time_point at;
    };
    void PruneLocked(std::chrono::steady_clock::time_point now);

    const std::chrono::seconds retention_;
    mutable std::mutex mutex_;
    std::map<std::weak_ptr<BufferAllocatorBase>, Entry, std::owner_less<>>
        allocators_;
    std::unordered_map<UUID, Entry, boost::hash<UUID>> disk_owners_;
};

// One lookup that missed: whether churn explains it, and how.
struct ChurnMiss {
    ChurnCause cause{ChurnCause::kUnknown};
    size_t heat_bucket{0};
    // 0..kSlices-1 for a dropped object found in the filter, kPendingAge for
    // one whose metadata is still awaiting cleanup.
    size_t age_slice{0};
    static constexpr size_t kPendingAge = ChurnMissFilter::kSlices;
};

class KvChurnMetrics {
   public:
    static KvChurnMetrics& instance();

    void ObserveRead(uint32_t heat_before, uint32_t heat_after, uint64_t size);
    void ObserveDrop(ChurnCause cause, size_t heat_bucket, uint64_t size);
    void ObserveDegraded(ChurnCause cause);
    void ObserveFilterOverflow();
    // api: "exist" or "get". churn is empty for a miss churn does not
    // explain.
    void ObserveMiss(const char* api, const std::optional<ChurnMiss>& churn);
    // A prefix-ordered batch exist: keys usable before the first miss, keys
    // present after it, and what explains the first miss.
    void ObserveBatchExist(size_t usable, size_t stranded,
                           const std::optional<ChurnMiss>* first_miss);
    // Distinct memory segments (chunks) and Stores a batch's objects are on.
    void ObserveBatchSpan(const char* op, size_t chunks, size_t stores);
    // Bytes of served reads whose memory replica is on `store` (a segment
    // name): the read load co-location concentrates.
    void ObserveStoreRead(const std::string& store, uint64_t bytes);
    // Drops the Store's label when its segment is unmounted, as the other
    // per-segment metrics do.
    void RemoveStore(const std::string& store);

    std::string Serialize();

   private:
    KvChurnMetrics();

    ylt::metric::histogram_t read_heat_;
    ylt::metric::dynamic_counter_1t heat_crossings_;
    ylt::metric::dynamic_counter_1t heat_crossing_bytes_;
    ylt::metric::dynamic_counter_2t drops_;
    ylt::metric::dynamic_counter_2t drop_bytes_;
    ylt::metric::dynamic_counter_1t degraded_;
    ylt::metric::counter_t filter_overflow_;
    ylt::metric::dynamic_counter_1t lookup_misses_;
    ylt::metric::dynamic_counter_3t churn_misses_;
    ylt::metric::dynamic_counter_2t churn_miss_age_;
    ylt::metric::counter_t batch_exist_usable_keys_;
    ylt::metric::counter_t batch_exist_stranded_keys_;
    ylt::metric::dynamic_counter_1t batch_exist_first_miss_;
    ylt::metric::dynamic_histogram_2t batch_span_;
    ylt::metric::dynamic_counter_1t store_read_bytes_;
};

}  // namespace mooncake
