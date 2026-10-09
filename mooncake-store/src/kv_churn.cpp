#include "kv_churn.h"

#include <algorithm>
#include <array>
#include <iterator>

#include "allocator.h"

namespace mooncake {

namespace {

constexpr std::array<uint32_t, 5> kHeatThresholds = {2, 3, 4, 8, 16};

uint64_t Mix64(uint64_t x) {
    // splitmix64 finalizer
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

size_t NextPowerOfTwo(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

uint32_t PackEntry(uint64_t key_hash, ChurnCause cause, size_t heat_bucket) {
    uint32_t fingerprint = static_cast<uint32_t>(key_hash >> 40);  // 24 bits
    if (fingerprint == 0) fingerprint = 1;  // 0 marks an empty slot
    const uint32_t label = (static_cast<uint32_t>(cause) << 4) |
                           static_cast<uint32_t>(heat_bucket & 0xF);
    return (fingerprint << 8) | label;
}

uint32_t EntryFingerprint(uint32_t entry) { return entry >> 8; }

const char* AgeSliceName(size_t age_slice) {
    static constexpr std::array<const char*, ChurnMissFilter::kSlices + 1>
        kNames = {"0", "1", "2", "3", "4", "5", "pending"};
    return kNames[std::min(age_slice, kNames.size() - 1)];
}

}  // namespace

const char* ChurnCauseName(ChurnCause cause) {
    switch (cause) {
        case ChurnCause::kUnmount:
            return "unmount";
        case ChurnCause::kClientExpiry:
            return "client_expiry";
        case ChurnCause::kDiskUnmount:
            return "disk_unmount";
        case ChurnCause::kEvictMemory:
            return "evict_memory";
        case ChurnCause::kEvictDisk:
            return "evict_disk";
        case ChurnCause::kUnknown:
        default:
            return "unknown";
    }
}

size_t HeatBucket(uint32_t heat) {
    if (heat < 2) return 0;
    if (heat < 3) return 1;
    if (heat < 4) return 2;
    if (heat < 8) return 3;
    if (heat < 16) return 4;
    return 5;
}

const char* HeatBucketName(size_t bucket) {
    static constexpr std::array<const char*, kHeatBucketCount> kNames = {
        "0-1", "2", "3", "4-7", "8-15", "16+"};
    return kNames[std::min(bucket, kNames.size() - 1)];
}

uint64_t ChurnKeyHash(const std::string& tenant, const std::string& key) {
    const uint64_t h = std::hash<std::string>{}(key);
    return Mix64(h ^ Mix64(std::hash<std::string>{}(tenant) + 1));
}

// ---------------------------------------------------------------------------
// ChurnMissFilter

ChurnMissFilter::ChurnMissFilter(std::chrono::seconds window,
                                 size_t slice_capacity)
    : slice_length_(std::max<std::chrono::seconds::rep>(
          1, window.count() / static_cast<int64_t>(kSlices))),
      // At most 2^26 drops (512 MiB) per slice: a larger value is a typo.
      slice_capacity_(std::clamp<size_t>(slice_capacity, 1, size_t{1} << 26)),
      // At most half full, so probe sequences stay short.
      mask_(NextPowerOfTwo(2 * slice_capacity_) - 1),
      slices_(kSlices) {
    for (auto& slice : slices_) {
        slice.slots = std::make_unique<std::atomic<uint32_t>[]>(mask_ + 1);
        for (size_t i = 0; i <= mask_; ++i) {
            slice.slots[i].store(0, std::memory_order_relaxed);
        }
    }
}

int64_t ChurnMissFilter::SliceId(
    std::chrono::steady_clock::time_point now) const {
    return std::chrono::duration_cast<std::chrono::seconds>(
               now.time_since_epoch())
               .count() /
           slice_length_.count();
}

ChurnMissFilter::Slice& ChurnMissFilter::CurrentSlice(int64_t id) {
    Slice& slice = slices_[static_cast<size_t>(id) % kSlices];
    if (slice.id.load(std::memory_order_acquire) == id) {
        return slice;
    }
    // Reusing the slot of the slice that just aged out: once per slice
    // length. Readers that race with it may miss or see stale entries;
    // the measurement tolerates both.
    std::lock_guard<std::mutex> lock(reuse_mutex_);
    if (slice.id.load(std::memory_order_acquire) != id) {
        slice.id.store(-1, std::memory_order_release);
        for (size_t i = 0; i <= mask_; ++i) {
            slice.slots[i].store(0, std::memory_order_relaxed);
        }
        slice.size.store(0, std::memory_order_relaxed);
        slice.id.store(id, std::memory_order_release);
    }
    return slice;
}

bool ChurnMissFilter::Insert(uint64_t key_hash, ChurnCause cause,
                             size_t heat_bucket,
                             std::chrono::steady_clock::time_point now) {
    Slice& slice = CurrentSlice(SliceId(now));
    const uint32_t entry = PackEntry(key_hash, cause, heat_bucket);
    const uint32_t fingerprint = EntryFingerprint(entry);
    const size_t base = static_cast<size_t>(key_hash) & mask_;
    for (size_t i = 0; i < kMaxProbe; ++i) {
        auto& cell = slice.slots[(base + i) & mask_];
        uint32_t current = cell.load(std::memory_order_relaxed);
        if (current != 0 && EntryFingerprint(current) == fingerprint) {
            cell.store(entry, std::memory_order_relaxed);  // newest drop wins
            return true;
        }
        if (current == 0) {
            if (slice.size.load(std::memory_order_relaxed) >= slice_capacity_) {
                return false;
            }
            if (cell.compare_exchange_strong(current, entry,
                                             std::memory_order_relaxed)) {
                slice.size.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            if (EntryFingerprint(current) == fingerprint) {
                cell.store(entry, std::memory_order_relaxed);
                return true;
            }
        }
    }
    return false;
}

std::optional<ChurnMissFilter::Hit> ChurnMissFilter::Find(
    uint64_t key_hash, std::chrono::steady_clock::time_point now) const {
    const int64_t newest = SliceId(now);
    const uint32_t fingerprint =
        EntryFingerprint(PackEntry(key_hash, ChurnCause::kUnknown, 0));
    const size_t base = static_cast<size_t>(key_hash) & mask_;
    for (size_t age = 0; age < kSlices; ++age) {
        const int64_t id = newest - static_cast<int64_t>(age);
        if (id < 0) break;
        const Slice& slice = slices_[static_cast<size_t>(id) % kSlices];
        if (slice.id.load(std::memory_order_acquire) != id) {
            continue;
        }
        for (size_t i = 0; i < kMaxProbe; ++i) {
            const uint32_t entry =
                slice.slots[(base + i) & mask_].load(std::memory_order_relaxed);
            if (entry == 0) break;  // no deletions, so the key is absent
            if (EntryFingerprint(entry) == fingerprint) {
                return Hit{static_cast<ChurnCause>((entry >> 4) & 0xF),
                           static_cast<size_t>(entry & 0xF), age};
            }
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// ChurnLossRegistry

void ChurnLossRegistry::PruneLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = allocators_.begin(); it != allocators_.end();) {
        it = now - it->second.at > retention_ ? allocators_.erase(it)
                                              : std::next(it);
    }
    for (auto it = disk_owners_.begin(); it != disk_owners_.end();) {
        it = now - it->second.at > retention_ ? disk_owners_.erase(it)
                                              : std::next(it);
    }
}

void ChurnLossRegistry::RecordAllocator(
    const std::weak_ptr<BufferAllocatorBase>& allocator, ChurnCause cause,
    std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    PruneLocked(now);
    allocators_[allocator] = Entry{cause, now};
}

void ChurnLossRegistry::RecordDiskOwner(
    const UUID& client_id, ChurnCause cause,
    std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    PruneLocked(now);
    disk_owners_[client_id] = Entry{cause, now};
}

ChurnCause ChurnLossRegistry::AllocatorCause(
    const std::weak_ptr<BufferAllocatorBase>& allocator) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = allocators_.find(allocator);
    return it == allocators_.end() ? ChurnCause::kUnknown : it->second.cause;
}

ChurnCause ChurnLossRegistry::DiskOwnerCause(const UUID& client_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = disk_owners_.find(client_id);
    return it == disk_owners_.end() ? ChurnCause::kUnknown : it->second.cause;
}

// ---------------------------------------------------------------------------
// KvChurnMetrics

KvChurnMetrics& KvChurnMetrics::instance() {
    static KvChurnMetrics metrics;
    return metrics;
}

KvChurnMetrics::KvChurnMetrics()
    : read_heat_("master_kv_read_heat",
                 "Decayed read heat of an object (reads per heat half-life) "
                 "after each read",
                 {1, 2, 3, 4, 8, 16, 32, 64, 128, 256}),
      heat_crossings_("master_kv_heat_crossings_total",
                      "Reads that lifted an object's heat to the threshold: "
                      "the admission rate a hot-replica threshold would see",
                      {"threshold"}),
      heat_crossing_bytes_("master_kv_heat_crossing_bytes_total",
                           "Object bytes of master_kv_heat_crossings_total",
                           {"threshold"}),
      drops_("master_kv_churn_drops_total",
             "Objects that lost their last servable copy, by cause and by "
             "heat when dropped",
             {"cause", "heat"}),
      drop_bytes_("master_kv_churn_drop_bytes_total",
                  "Object bytes of master_kv_churn_drops_total",
                  {"cause", "heat"}),
      degraded_("master_kv_churn_degraded_total",
                "Objects that lost a replica to churn but kept a servable "
                "one",
                {"cause"}),
      filter_overflow_("master_kv_churn_filter_overflow_total",
                       "Drops not remembered because the churn-miss filter's "
                       "current slice was full"),
      lookup_misses_("master_kv_lookup_misses_total",
                     "Keys looked up and not servable", {"api"}),
      churn_misses_("master_kv_churn_misses_total",
                    "Missed keys that churn dropped within the window, by "
                    "cause and heat when dropped",
                    {"api", "cause", "heat"}),
      churn_miss_age_("master_kv_churn_miss_age_total",
                      "master_kv_churn_misses_total by time since the drop, "
                      "in churn-miss filter slices (pending: metadata not yet "
                      "cleaned up)",
                      {"api", "age_slice"}),
      batch_exist_usable_keys_(
          "master_kv_batch_exist_usable_keys_total",
          "BatchExistKey keys before the first missing one (usable by a "
          "first-miss prefix reader such as SGLang)"),
      batch_exist_stranded_keys_(
          "master_kv_batch_exist_stranded_keys_total",
          "BatchExistKey keys present after the first missing one (cached "
          "but unusable by a first-miss prefix reader)"),
      batch_exist_first_miss_(
          "master_kv_batch_exist_first_miss_total",
          "BatchExistKey batches with a missing key, by the churn cause of "
          "the first one (none: churn does not explain it)",
          {"cause"}),
      batch_span_("master_kv_batch_segments_spanned",
                  "Distinct memory segments (chunk) or Stores (store) holding "
                  "one batch's objects",
                  {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64}, {"op", "level"}),
      store_read_bytes_("master_kv_store_read_bytes_total",
                        "Object bytes of served reads whose memory replica is "
                        "on this Store (segment name); its label is dropped "
                        "when the Store unmounts a segment",
                        {"segment"}) {}

void KvChurnMetrics::ObserveRead(uint32_t heat_before, uint32_t heat_after,
                                 uint64_t size) {
    read_heat_.observe(static_cast<int64_t>(heat_after));
    for (const uint32_t threshold : kHeatThresholds) {
        if (heat_before < threshold && threshold <= heat_after) {
            const std::array<std::string, 1> label{std::to_string(threshold)};
            heat_crossings_.inc(label);
            heat_crossing_bytes_.inc(label, static_cast<int64_t>(size));
        }
    }
}

void KvChurnMetrics::ObserveDrop(ChurnCause cause, size_t heat_bucket,
                                 uint64_t size) {
    const std::array<std::string, 2> labels{ChurnCauseName(cause),
                                            HeatBucketName(heat_bucket)};
    drops_.inc(labels);
    drop_bytes_.inc(labels, static_cast<int64_t>(size));
}

void KvChurnMetrics::ObserveDegraded(ChurnCause cause) {
    degraded_.inc({ChurnCauseName(cause)});
}

void KvChurnMetrics::ObserveFilterOverflow() { filter_overflow_.inc(); }

void KvChurnMetrics::ObserveMiss(const char* api,
                                 const std::optional<ChurnMiss>& churn) {
    lookup_misses_.inc({api});
    if (!churn) return;
    churn_misses_.inc({api, ChurnCauseName(churn->cause),
                       HeatBucketName(churn->heat_bucket)});
    churn_miss_age_.inc({api, AgeSliceName(churn->age_slice)});
}

void KvChurnMetrics::ObserveBatchExist(
    size_t usable, size_t stranded,
    const std::optional<ChurnMiss>* first_miss) {
    batch_exist_usable_keys_.inc(static_cast<int64_t>(usable));
    if (stranded > 0) {
        batch_exist_stranded_keys_.inc(static_cast<int64_t>(stranded));
    }
    if (first_miss) {
        batch_exist_first_miss_.inc(
            {*first_miss ? ChurnCauseName((*first_miss)->cause) : "none"});
    }
}

void KvChurnMetrics::ObserveBatchSpan(const char* op, size_t chunks,
                                      size_t stores) {
    batch_span_.observe({op, "chunk"}, static_cast<int64_t>(chunks));
    batch_span_.observe({op, "store"}, static_cast<int64_t>(stores));
}

void KvChurnMetrics::ObserveStoreRead(const std::string& store,
                                      uint64_t bytes) {
    store_read_bytes_.inc({store}, static_cast<int64_t>(bytes));
}

void KvChurnMetrics::RemoveStore(const std::string& store) {
    store_read_bytes_.remove_label_value({{"segment", store}});
}

std::string KvChurnMetrics::Serialize() {
    std::string out;
    read_heat_.serialize(out);
    heat_crossings_.serialize(out);
    heat_crossing_bytes_.serialize(out);
    drops_.serialize(out);
    drop_bytes_.serialize(out);
    degraded_.serialize(out);
    filter_overflow_.serialize(out);
    lookup_misses_.serialize(out);
    churn_misses_.serialize(out);
    churn_miss_age_.serialize(out);
    batch_exist_usable_keys_.serialize(out);
    batch_exist_stranded_keys_.serialize(out);
    batch_exist_first_miss_.serialize(out);
    batch_span_.serialize(out);
    store_read_bytes_.serialize(out);
    return out;
}

}  // namespace mooncake
