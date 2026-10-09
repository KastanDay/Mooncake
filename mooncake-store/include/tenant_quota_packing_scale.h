#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace mooncake {

/**
 * Scales tenant quota capacity to what the memory allocators can hold.
 *
 * Tenant quotas, like master_allocated_bytes, count the bytes objects request.
 * The offset allocator pads each allocation up to its size class, so quotas
 * that add up to the pool's capacity cannot all be met: the allocators fill
 * first, a Put fails, and the Master evicts a share of every tenant's objects
 * regardless of quota. Scaling capacity by the packing efficiency (requested /
 * reserved bytes) makes the quotas add up to what fits, so a full tenant evicts
 * its own objects instead.
 *
 * The scale moves slowly and only when the pool is full enough for the
 * efficiency to be representative: it starts at 1.0, follows an exponential
 * moving average of the samples, is clamped to [floor, 1.0], and is reported
 * only when it moves by at least min_change, so that quota recomputes (and the
 * trims they cause) stay rare. Not thread-safe; one caller owns it.
 */
class TenantQuotaPackingScale {
   public:
    struct Options {
        double floor = 0.8;
        // Weight of each new sample in the moving average.
        double sample_weight = 0.1;
        // Reserved bytes as a share of capacity below which samples are
        // ignored: a nearly empty pool's few objects say little about its
        // packing once full, and quotas do not bind there anyway.
        double min_reserved_ratio = 0.5;
        double min_change = 0.005;
    };

    explicit TenantQuotaPackingScale(Options options) : options_(options) {}

    // Records one sample of the memory tier. Returns the new scale when it
    // moved by at least min_change, and nullopt otherwise.
    std::optional<double> Observe(uint64_t requested_bytes,
                                  uint64_t reserved_bytes,
                                  uint64_t capacity_bytes) {
        if (capacity_bytes == 0 || reserved_bytes == 0 ||
            static_cast<double>(reserved_bytes) <
                options_.min_reserved_ratio *
                    static_cast<double>(capacity_bytes)) {
            return std::nullopt;
        }
        const double sample =
            std::clamp(static_cast<double>(requested_bytes) /
                           static_cast<double>(reserved_bytes),
                       options_.floor, 1.0);
        smoothed_ += options_.sample_weight * (sample - smoothed_);
        if (std::abs(smoothed_ - applied_) < options_.min_change) {
            return std::nullopt;
        }
        applied_ = smoothed_;
        return applied_;
    }

    double applied() const { return applied_; }

   private:
    Options options_;
    double smoothed_ = 1.0;
    double applied_ = 1.0;
};

}  // namespace mooncake
