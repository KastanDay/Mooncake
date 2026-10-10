#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace mooncake {

/**
 * Object-group routing: the group, and so the metadata shard, of every
 * grouped object (keyed by its tenant-scoped key), and the groups whose
 * members' read leases need a refresh (keyed by tenant-scoped group id).
 *
 * Every metadata lookup reads a route, so a route lookup must never wait out
 * work proportional to the number of routes. One global map under one
 * exclusive lock did: on eu-west1 (2026-10-10, SGLang with
 * enable_group_semantics, one route per KV object) the insert that took the
 * map past 24,607,243 entries rehashed it in place for 10.3 s, and every
 * metadata lookup on every RPC thread waited behind it. No Ping was answered
 * for that long, and the client monitor expired all 47 clients.
 *
 * Stripes, each with its own map and lock, bound a rehash to one stripe's
 * routes (about 1/kStripes of them), and leave lookups in the other stripes
 * untouched. Each stripe's lock is a leaf: held only for one map operation,
 * never while waiting for another lock.
 */
class GroupRoutingIndex {
   public:
    static constexpr size_t kStripes = 1024;  // A power of two.

    GroupRoutingIndex() : stripes_(std::make_unique<Stripe[]>(kStripes)) {}
    GroupRoutingIndex(const GroupRoutingIndex&) = delete;
    GroupRoutingIndex& operator=(const GroupRoutingIndex&) = delete;

    // The group of the object, or nullopt if it is not grouped.
    std::optional<std::string> FindRoute(const std::string& scoped_key) const {
        const Stripe& stripe = StripeFor(scoped_key);
        std::shared_lock<std::shared_mutex> lock(stripe.mutex);
        auto it = stripe.routes.find(scoped_key);
        if (it == stripe.routes.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void SetRoute(const std::string& scoped_key, const std::string& group_id) {
        Stripe& stripe = StripeFor(scoped_key);
        std::unique_lock<std::shared_mutex> lock(stripe.mutex);
        stripe.routes[scoped_key] = group_id;
    }

    // Erases the route only if it still names `group_id`.
    void EraseRoute(const std::string& scoped_key,
                    const std::string& group_id) {
        Stripe& stripe = StripeFor(scoped_key);
        std::unique_lock<std::shared_mutex> lock(stripe.mutex);
        auto it = stripe.routes.find(scoped_key);
        if (it != stripe.routes.end() && it->second == group_id) {
            stripe.routes.erase(it);
        }
    }

    void MarkNeedsRefresh(const std::string& scoped_group) {
        Stripe& stripe = StripeFor(scoped_group);
        std::unique_lock<std::shared_mutex> lock(stripe.mutex);
        stripe.needs_refresh.insert(scoped_group);
    }

    bool NeedsRefresh(const std::string& scoped_group) const {
        const Stripe& stripe = StripeFor(scoped_group);
        std::shared_lock<std::shared_mutex> lock(stripe.mutex);
        return stripe.needs_refresh.contains(scoped_group);
    }

    void ClearNeedsRefresh(const std::string& scoped_group) {
        Stripe& stripe = StripeFor(scoped_group);
        std::unique_lock<std::shared_mutex> lock(stripe.mutex);
        stripe.needs_refresh.erase(scoped_group);
    }

    // Replaces the whole index (a rebuild after a restore). Each stripe is
    // swapped under its own lock: a lookup racing the rebuild sees a stripe
    // either before or after, as it saw the one map before.
    void Reset(const std::unordered_map<std::string, std::string>& routes,
               const std::unordered_set<std::string>& needs_refresh) {
        auto next = std::make_unique<Stripe[]>(kStripes);
        for (const auto& [scoped_key, group_id] : routes) {
            next[StripeIndex(scoped_key)].routes.emplace(scoped_key, group_id);
        }
        for (const auto& scoped_group : needs_refresh) {
            next[StripeIndex(scoped_group)].needs_refresh.insert(scoped_group);
        }
        for (size_t i = 0; i < kStripes; ++i) {
            std::unique_lock<std::shared_mutex> lock(stripes_[i].mutex);
            stripes_[i].routes.swap(next[i].routes);
            stripes_[i].needs_refresh.swap(next[i].needs_refresh);
        }
        // The old contents are freed here, outside every stripe lock.
    }

    void Clear() { Reset({}, {}); }

    size_t RouteCount() const {
        size_t count = 0;
        for (size_t i = 0; i < kStripes; ++i) {
            std::shared_lock<std::shared_mutex> lock(stripes_[i].mutex);
            count += stripes_[i].routes.size();
        }
        return count;
    }

    // The most routes any one stripe holds: what one rehash can cost.
    size_t LargestStripeRouteCount() const {
        size_t largest = 0;
        for (size_t i = 0; i < kStripes; ++i) {
            std::shared_lock<std::shared_mutex> lock(stripes_[i].mutex);
            largest = std::max(largest, stripes_[i].routes.size());
        }
        return largest;
    }

    // Holds the stripe of `key` exclusively, as a rehash of it does.
    std::unique_lock<std::shared_mutex> LockStripeForTesting(
        const std::string& key) {
        return std::unique_lock<std::shared_mutex>(StripeFor(key).mutex);
    }
    static size_t StripeIndexForTesting(const std::string& key) {
        return StripeIndex(key);
    }

   private:
    struct Stripe {
        mutable std::shared_mutex mutex;
        std::unordered_map<std::string, std::string> routes;
        std::unordered_set<std::string> needs_refresh;
    };

    // The top bits of a multiplicative mix: the maps bucket by the hash
    // modulo a prime, so the stripe must not reuse its low bits.
    static size_t StripeIndex(const std::string& key) {
        const uint64_t h = std::hash<std::string>{}(key);
        return static_cast<size_t>((h * 0x9E3779B97F4A7C15ULL) >> 54) &
               (kStripes - 1);
    }
    Stripe& StripeFor(const std::string& key) {
        return stripes_[StripeIndex(key)];
    }
    const Stripe& StripeFor(const std::string& key) const {
        return stripes_[StripeIndex(key)];
    }

    std::unique_ptr<Stripe[]> stripes_;
};

}  // namespace mooncake
