// Client liveness must not depend on metadata cleanup, and cleanup must not
// hold any lock that liveness, mounts or the data path wait on for longer than
// one bounded batch.
//
// On eu-west1 (2026-10-08, about 13.8M keys) one Store's disk unmount walked
// the whole index inside its RPC handler for 58 s, a client whose Pings shared
// that RPC thread expired, its expiry swept the whole index again on the
// monitor thread while holding snapshot_mutex_, and a remount from that
// client waited for snapshot_mutex_ while holding client_mutex_, which every
// Ping and Put needed: every client expired at once and the cache was
// flushed. These tests pin each link of that chain open.

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <future>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "master_service.h"
#include "tenant_quota_policy_store.h"
#include "types.h"

namespace mooncake::test {

using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

class MasterLivenessIsolationTest : public ::testing::Test {
   protected:
    void SetUp() override {
        google::InitGoogleLogging("MasterLivenessIsolationTest");
        FLAGS_logtostderr = true;
    }

    void TearDown() override { google::ShutdownGoogleLogging(); }

    // A TTL long enough that no fixture client expires during setup; the
    // expiry tests pass a short one and keep their clients pinging.
    static std::unique_ptr<MasterService> MakeService(int64_t ttl_sec = 3600) {
        MasterServiceConfig config;
        config.enable_offload = true;
        config.default_kv_lease_ttl = 0;
        config.client_live_ttl_sec = ttl_sec;
        return std::make_unique<MasterService>(config);
    }

    // Tenant quotas on: `tenant` may hold `quota_bytes`. With lease_ms 0,
    // objects are evictable as soon as written.
    static std::unique_ptr<MasterService> MakeQuotaService(
        const std::string& tenant, uint64_t quota_bytes,
        uint64_t lease_ms = 0) {
        TenantQuotaPolicySnapshot policy;
        policy.tenant_quotas.emplace(tenant, quota_bytes);
        const std::string path = std::string("/tmp/liveness_quota_") +
                                 std::to_string(::getpid()) + "_" +
                                 std::to_string(next_policy_++) + ".yaml";
        std::ofstream(path) << FormatTenantQuotaPolicyYaml(policy);
        MasterServiceConfig config;
        config.default_kv_lease_ttl = lease_ms;
        config.client_live_ttl_sec = 3600;
        config.enable_multi_tenants = true;
        config.tenant_quota_connector_type = "file";
        config.tenant_quota_connector_uri = path;
        return std::make_unique<MasterService>(config);
    }
    static inline std::atomic<int> next_policy_{0};

    // Pings the given clients every 200 ms until destroyed; Drop() stops
    // pinging one of them.
    class KeepAlive {
       public:
        KeepAlive(MasterService& service, std::vector<UUID> clients)
            : clients_(std::move(clients)), thread_([this, &service] {
                  while (!stop_) {
                      std::vector<UUID> clients;
                      {
                          std::lock_guard<std::mutex> lock(mutex_);
                          clients = clients_;
                      }
                      for (const auto& id : clients) {
                          auto ping = service.Ping(id);
                          if (!ping ||
                              ping->client_status != ClientStatus::OK) {
                              ++not_ok_;
                          }
                      }
                      std::this_thread::sleep_for(milliseconds(200));
                  }
              }) {}
        ~KeepAlive() {
            stop_ = true;
            thread_.join();
        }
        void Drop(const UUID& id) {
            std::lock_guard<std::mutex> lock(mutex_);
            std::erase(clients_, id);
        }
        int not_ok() const { return not_ok_.load(); }

       private:
        std::mutex mutex_;
        std::vector<UUID> clients_;
        std::atomic<bool> stop_{false};
        std::atomic<int> not_ok_{0};
        std::thread thread_;
    };

    struct Client {
        UUID id;
        Segment segment;
    };

    // A memory-only client with one large segment.
    static Client MountMemoryClient(MasterService& service,
                                    const std::string& name, size_t base,
                                    size_t size) {
        Client client{generate_uuid(), {}};
        client.segment.id = generate_uuid();
        client.segment.name = name;
        client.segment.base = base;
        client.segment.size = size;
        client.segment.te_endpoint = name;
        EXPECT_TRUE(
            service.MountSegment(client.segment, client.id).has_value());
        EXPECT_TRUE(
            service.ReMountSegment({client.segment}, client.id).has_value());
        return client;
    }

    static tl::expected<void, ErrorCode> PutIn(MasterService& service,
                                               const Client& client,
                                               const std::string& tenant,
                                               const std::string& key,
                                               uint64_t size) {
        ReplicateConfig config;
        config.replica_num = 1;
        auto start =
            service.PutStart(client.id, key, TenantId(tenant), size, config);
        if (!start) {
            return tl::make_unexpected(start.error());
        }
        auto end = service.PutEnd(client.id, key, TenantId(tenant),
                                  ReplicaType::MEMORY);
        if (!end) {
            return tl::make_unexpected(end.error());
        }
        return {};
    }

    static uint64_t Charged(MasterService& service, const std::string& t) {
        return service.GetTenantQuotaSnapshot(TenantId(t))->charged_bytes;
    }
    static uint64_t Effective(MasterService& service, const std::string& t) {
        return service.GetTenantQuotaSnapshot(TenantId(t))
            ->effective_quota_bytes;
    }
    static void StopQuotaTrimWorker(MasterService& service) {
        service.quota_trim_worker_.Stop();
    }
    static void TrimNow(MasterService& service) {
        service.TrimTenantsOverQuota();
    }
    static constexpr size_t kInlineBudget =
        MasterService::kInlineEvictionKeyBudget;

    // Mounts a memory segment and a LOCAL_DISK segment, then remounts once
    // the way a client answering its first NEED_REMOUNT Ping does, so the
    // client is OK.
    static Client MountClient(MasterService& service, const std::string& name,
                              size_t base) {
        Client client{generate_uuid(), {}};
        client.segment.id = generate_uuid();
        client.segment.name = name;
        client.segment.base = base;
        client.segment.size = 256 * 1024 * 1024;
        client.segment.te_endpoint = name;
        EXPECT_TRUE(
            service.MountSegment(client.segment, client.id).has_value());
        EXPECT_TRUE(service.MountLocalDiskSegment(client.id, true).has_value());
        EXPECT_TRUE(service.ReportSsdCapacity(client.id, 1 << 30).has_value());
        EXPECT_TRUE(
            service.ReMountSegment({client.segment}, client.id).has_value());
        return client;
    }

    static void Put(MasterService& service, const Client& client,
                    const std::string& key, int64_t size = 1024) {
        ReplicateConfig config;
        config.replica_num = 1;
        ASSERT_TRUE(
            service.PutStart(client.id, key, TenantId::Default(), size, config)
                .has_value());
        ASSERT_TRUE(service
                        .PutEnd(client.id, key, TenantId::Default(),
                                ReplicaType::MEMORY)
                        .has_value());
    }

    static tl::expected<void, ErrorCode> Offload(MasterService& service,
                                                 const Client& client,
                                                 const std::string& key,
                                                 int64_t size = 1024) {
        StorageObjectMetadata metadata;
        metadata.data_size = size;
        metadata.transport_endpoint = client.segment.name;
        OffloadTaskItem task{
            .tenant_id = TenantId::Default().value(), .key = key, .size = size};
        return service.NotifyOffloadSuccess(client.id, {task}, {metadata});
    }

    // Disk-only keys, in batches: the index a cleanup pass walks, at a
    // fraction of the setup time of Put + offload.
    static void OffloadDiskOnlyKeys(MasterService& service,
                                    const Client& client,
                                    const std::string& prefix, size_t count) {
        std::vector<OffloadTaskItem> tasks;
        std::vector<StorageObjectMetadata> metas;
        for (size_t i = 0; i < count; ++i) {
            tasks.push_back({.tenant_id = TenantId::Default().value(),
                             .key = prefix + std::to_string(i),
                             .size = 64});
            StorageObjectMetadata meta;
            meta.data_size = 64;
            meta.transport_endpoint = client.segment.name;
            metas.push_back(meta);
            if (tasks.size() == 4096 || i + 1 == count) {
                ASSERT_TRUE(
                    service.NotifyOffloadSuccess(client.id, tasks, metas)
                        .has_value());
                tasks.clear();
                metas.clear();
            }
        }
    }

    static void PutAndOffload(MasterService& service, const Client& client,
                              const std::string& key, int64_t size = 1024) {
        Put(service, client, key, size);
        ASSERT_TRUE(Offload(service, client, key, size).has_value());
    }

    static std::shared_mutex& SnapshotMutex(MasterService& service) {
        return service.snapshot_mutex_;
    }
    static std::shared_mutex& ClientMutex(MasterService& service) {
        return service.client_mutex_;
    }
    static uint64_t Generation(MasterService& service, const UUID& id) {
        return service.local_ssd_manager_.Generation(id).value_or(0);
    }
    static ErrorCode EnqueuePromotion(MasterService& service, const UUID& id,
                                      PromotionTaskItem task,
                                      uint64_t generation) {
        return service.local_ssd_manager_.EnqueuePromotion(id, std::move(task),
                                                           generation);
    }
    static bool BackgroundCleanup(MasterService& service) {
        return service.enable_async_segment_cleanup_;
    }
    static size_t ShardOf(MasterService& service, const std::string& key) {
        return service.getMetadataShardIndex(TenantId::Default(), key);
    }
    static int SnapshotWritersWaiting(MasterService& service) {
        return service.snapshot_writers_waiting_.load();
    }
    static void ExpireNow(MasterService& service, const UUID& client_id,
                          Clock::time_point selected_at) {
        service.ExpireClients({client_id}, selected_at);
    }

    // Ends a LOCAL_DISK registration without scheduling cleanup, so its
    // replicas are left as garbage for the test to observe.
    static void RetireDiskWithoutCleanup(MasterService& service,
                                         const UUID& client_id) {
        auto lock = service.LockSnapshotExclusive();
        service.local_ssd_manager_.UnregisterClient(client_id);
    }

    // Read without pinging: a Ping would keep the client alive.
    static bool IsOk(MasterService& service, const UUID& client_id) {
        std::lock_guard<std::mutex> lock(service.liveness_mutex_);
        auto it = service.client_liveness_.find(client_id);
        return it != service.client_liveness_.end() && it->second.ok;
    }

    // Ends a LOCAL_DISK registration the way an expiry does: no exclusive
    // snapshot lock, so an admission already past its own check can be in
    // flight, holding the lock shared.
    static void RetireDiskAsExpiryDoes(MasterService& service,
                                       const UUID& client_id) {
        service.local_ssd_manager_.UnregisterClient(client_id);
    }

    // Holds the key's shard lock until the returned accessor is destroyed.
    static auto LockShard(MasterService& service, const std::string& key) {
        return std::make_unique<MasterService::MetadataShardAccessorRW>(
            &service, ShardOf(service, key));
    }

    static void ReclaimNow(MasterService& service) {
        service.ClearInvalidHandles();
    }

    // The key's LOCAL_DISK replica owned by `owner`, under its shard lock.
    template <typename Fn>
    static bool WithDiskReplica(MasterService& service, const std::string& key,
                                const UUID& owner, Fn&& fn) {
        MasterService::MetadataShardAccessorRW shard(&service,
                                                     ShardOf(service, key));
        auto tenant = shard->tenants.find(TenantId::Default());
        if (tenant == shard->tenants.end()) {
            return false;
        }
        auto it = tenant->second.metadata.find(key);
        if (it == tenant->second.metadata.end()) {
            return false;
        }
        bool found = false;
        it->second.VisitReplicas(
            [&owner](const Replica& r) {
                return r.get_local_disk_client_id() == owner;
            },
            [&](Replica& r) {
                found = true;
                fn(r, it->second);
            });
        return found;
    }

    static uint64_t DiskGeneration(MasterService& service,
                                   const std::string& key, const UUID& owner) {
        uint64_t generation = UINT64_MAX;
        WithDiskReplica(service, key, owner, [&](Replica& r, auto&) {
            generation = r.get_local_disk_generation().value();
        });
        return generation;
    }

    // What a snapshot restore leaves before binding: the registry decoded
    // into fresh registrations (new generations, no used bytes), and every
    // LOCAL_DISK replica decoded bound to none.
    static void DecodeAsRestored(MasterService& service,
                                 const std::vector<std::string>& keys,
                                 const UUID& owner) {
        service.local_ssd_manager_.RestorePersistedState(
            service.local_ssd_manager_.ExportPersistedState());
        for (const auto& key : keys) {
            WithDiskReplica(service, key, owner, [](Replica& r, auto&) {
                r.set_local_disk_generation(0);
            });
        }
    }

    static void BindRestored(MasterService& service) {
        service.BindRestoredLocalDiskReplicas();
    }

    static bool Rebind(MasterService& service, const std::string& key,
                       const Replica& incoming) {
        bool rebound = false;
        WithDiskReplica(service, key, *incoming.get_local_disk_client_id(),
                        [&](Replica&, auto& metadata) {
                            rebound = service.RebindLocalDiskReplica(metadata,
                                                                     incoming);
                        });
        return rebound;
    }

    static tl::expected<bool, ErrorCode> AddReplica(MasterService& service,
                                                    const UUID& client_id,
                                                    const std::string& key,
                                                    Replica& replica) {
        return service.AddReplica(client_id, key, TenantId::Default(), replica);
    }

    static tl::expected<void, ErrorCode> PushPromotion(MasterService& service,
                                                       const std::string& key,
                                                       const UUID& owner) {
        tl::expected<void, ErrorCode> result =
            tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        Replica source(owner, 0, "", ReplicaStatus::COMPLETE);
        WithDiskReplica(service, key, owner, [&](Replica& r, auto&) {
            source.rebind_local_disk(r);
        });
        return service.PushPromotionQueue(
            MasterService::ObjectIdentity{TenantId::Default(), key}, source);
    }

    static int64_t UsedBytes(MasterService& service, const UUID& client_id) {
        auto usage = service.local_ssd_manager_.GetUsage(client_id);
        return usage ? usage->used_bytes : -1;
    }

    // LOCAL_DISK replicas in the index owned by `owner`, readable or not.
    static size_t DiskReplicasOwnedBy(MasterService& service,
                                      const UUID& owner) {
        size_t count = 0;
        for (size_t i = 0; i < MasterService::kNumShards; ++i) {
            MasterService::MetadataShardAccessorRW shard(&service, i);
            for (auto& [tenant, state] : shard->tenants) {
                for (auto& [key, metadata] : state.metadata) {
                    for (const auto& replica : metadata.GetAllReplicas()) {
                        if (replica.get_local_disk_client_id() == owner) {
                            ++count;
                        }
                    }
                }
            }
        }
        return count;
    }

    static bool HasReadableDiskReplicaOf(MasterService& service,
                                         const std::string& key,
                                         const UUID& owner) {
        auto replicas = service.GetReplicaList(key, TenantId::Default());
        if (!replicas) {
            return false;
        }
        for (const auto& descriptor : replicas->replicas) {
            if (descriptor.is_local_disk_replica() &&
                descriptor.get_local_disk_descriptor().client_id == owner) {
                return true;
            }
        }
        return false;
    }

    template <typename Fn>
    static milliseconds Time(Fn&& fn) {
        const auto start = Clock::now();
        fn();
        return std::chrono::duration_cast<milliseconds>(Clock::now() - start);
    }

    template <typename Pred>
    static bool WaitFor(Pred&& pred, milliseconds timeout) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(milliseconds(20));
        }
        return pred();
    }
};

// The convoy itself: a metadata pass holds snapshot_mutex_ shared, a remount
// waits for it exclusively. Pings must still be answered at once, and the
// waiting remount must not hold client_mutex_ (Puts and other mounts).
TEST_F(MasterLivenessIsolationTest, PingIsAnsweredWhileARemountWaitsOnAPass) {
    auto service = MakeService();
    auto a = MountClient(*service, "liveness_a", 0x100000000);
    auto b = MountClient(*service, "liveness_b", 0x200000000);

    std::promise<void> pass_holding;
    std::atomic<bool> release_pass{false};
    std::thread pass([&] {
        std::shared_lock<std::shared_mutex> lock(SnapshotMutex(*service));
        pass_holding.set_value();
        while (!release_pass) {
            std::this_thread::sleep_for(milliseconds(5));
        }
    });
    pass_holding.get_future().wait();

    std::atomic<bool> remount_done{false};
    std::thread remount([&] {
        service->ReMountSegment({b.segment}, b.id);
        remount_done = true;
    });
    // No fatal assertion while the threads run: a failure must still release
    // the pass and join them.
    const bool remount_waiting =
        WaitFor([&] { return SnapshotWritersWaiting(*service) > 0; },
                milliseconds(5000));
    EXPECT_TRUE(remount_waiting)
        << "the remount never waited for the held pass";
    EXPECT_FALSE(remount_done);

    // On a regression the Ping would wait for the held pass: answer it from
    // a future, so the test fails instead of deadlocking.
    auto ping =
        std::async(std::launch::async, [&] { return service->Ping(a.id); });
    // Generous: on a regression the Ping waits until the pass is released.
    EXPECT_EQ(ping.wait_for(milliseconds(2000)), std::future_status::ready)
        << "Ping waited behind the remount";

    // Nor does anything else that needs client_mutex_ wait on it.
    std::atomic<bool> client_lock_taken{false};
    std::thread client_user([&] {
        std::unique_lock<std::shared_mutex> lock(ClientMutex(*service));
        client_lock_taken = true;
    });
    EXPECT_TRUE(
        WaitFor([&] { return client_lock_taken.load(); }, milliseconds(2000)))
        << "the waiting remount holds client_mutex_";

    release_pass = true;
    pass.join();
    remount.join();
    client_user.join();
    EXPECT_TRUE(remount_done);
    auto answer = ping.get();
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->client_status, ClientStatus::OK);
}

// The data path never takes client_mutex_: a long hold of it (a remount, an
// expiry) cannot occupy the RPC threads serving Puts.
TEST_F(MasterLivenessIsolationTest, PutsDoNotWaitForClientMutex) {
    auto service = MakeService();
    auto a = MountClient(*service, "liveness_put", 0x100000000);

    std::promise<void> holding;
    std::atomic<bool> release{false};
    std::thread holder([&] {
        std::unique_lock<std::shared_mutex> lock(ClientMutex(*service));
        holding.set_value();
        while (!release) {
            std::this_thread::sleep_for(milliseconds(5));
        }
    });
    holding.get_future().wait();
    auto work = std::async(std::launch::async, [&] {
        Put(*service, a, "put_while_client_mutex_held");
        return service->Ping(a.id).has_value();
    });
    const bool prompt =
        work.wait_for(milliseconds(2000)) == std::future_status::ready;
    release = true;
    holder.join();
    EXPECT_TRUE(prompt) << "Put or Ping waited for client_mutex_";
    EXPECT_TRUE(work.get());
}

// A disk unmount answers at once, without walking the index; from its return
// its replicas are never handed out, and the cleanup worker reclaims them.
TEST_F(MasterLivenessIsolationTest, DiskUnmountRetiresAtOnceAndReclaimsLater) {
    auto service = MakeService();
    auto a = MountClient(*service, "unmount_a", 0x100000000);
    constexpr int kKeys = 2000;
    for (int i = 0; i < kKeys; ++i) {
        PutAndOffload(*service, a, "unmount_key_" + std::to_string(i));
    }
    ASSERT_EQ(DiskReplicasOwnedBy(*service, a.id), size_t(kKeys));
    ASSERT_TRUE(HasReadableDiskReplicaOf(*service, "unmount_key_0", a.id));

    ASSERT_TRUE(service->UnmountLocalDiskSegment(a.id).has_value());
    for (int i = 0; i < kKeys; i += 97) {
        EXPECT_FALSE(HasReadableDiskReplicaOf(
            *service, "unmount_key_" + std::to_string(i), a.id))
            << "a retired registration's replica was handed out";
    }
    EXPECT_TRUE(
        WaitFor([&] { return DiskReplicasOwnedBy(*service, a.id) == 0; },
                milliseconds(10000)));
    // The memory replicas stay: only the disk went away.
    auto replicas =
        service->GetReplicaList("unmount_key_1", TenantId::Default());
    ASSERT_TRUE(replicas.has_value());
    EXPECT_FALSE(replicas->replicas.empty());
}

// Same client id, new registration (an expired client that comes back): the
// old registration's replicas stay unreadable, are reclaimed, and their bytes
// are not debited from the new registration.
TEST_F(MasterLivenessIsolationTest, ReregistrationDoesNotResurrectOldReplicas) {
    auto service = MakeService();
    auto a = MountClient(*service, "rereg_a", 0x100000000);
    PutAndOffload(*service, a, "rereg_key", 4096);
    ASSERT_EQ(UsedBytes(*service, a.id), 4096);

    RetireDiskWithoutCleanup(*service, a.id);
    ASSERT_TRUE(service->MountLocalDiskSegment(a.id, true).has_value());
    EXPECT_EQ(UsedBytes(*service, a.id), 0);
    EXPECT_FALSE(HasReadableDiskReplicaOf(*service, "rereg_key", a.id))
        << "the old registration's replica came back";

    ReclaimNow(*service);
    EXPECT_EQ(DiskReplicasOwnedBy(*service, a.id), 0u);
    EXPECT_EQ(UsedBytes(*service, a.id), 0) << "old bytes debited from new";

    // The new registration offloads the key again, as a rescan would.
    ASSERT_TRUE(Offload(*service, a, "rereg_key", 4096).has_value());
    EXPECT_TRUE(HasReadableDiskReplicaOf(*service, "rereg_key", a.id));
    EXPECT_EQ(UsedBytes(*service, a.id), 4096);
}

// A replica of an ended registration, not yet reclaimed, must not keep
// another owner's offload of the same key out.
TEST_F(MasterLivenessIsolationTest, EndedRegistrationDoesNotShadowNewOwner) {
    auto service = MakeService();
    auto a = MountClient(*service, "shadow_a", 0x100000000);
    auto b = MountClient(*service, "shadow_b", 0x200000000);
    PutAndOffload(*service, a, "shadow_key");
    RetireDiskWithoutCleanup(*service, a.id);

    ASSERT_TRUE(Offload(*service, b, "shadow_key").has_value());
    EXPECT_TRUE(HasReadableDiskReplicaOf(*service, "shadow_key", b.id));
    EXPECT_EQ(DiskReplicasOwnedBy(*service, a.id), 0u);
}

// A cleanup pass completes over keys added (and maps rehashed) while it runs.
TEST_F(MasterLivenessIsolationTest, ReclaimPassCoversKeysDespiteRehash) {
    auto service = MakeService();
    auto a = MountClient(*service, "rehash_a", 0x100000000);
    auto b = MountClient(*service, "rehash_b", 0x200000000);
    // Every key in one shard, so the pass takes many batches there and the
    // concurrent inserts rehash the very map it is walking.
    auto keys_in_shard = [&](const std::string& prefix, size_t count) {
        std::vector<std::string> keys;
        for (size_t i = 0; keys.size() < count; ++i) {
            std::string key = prefix + std::to_string(i);
            if (ShardOf(*service, key) == 0) {
                keys.push_back(std::move(key));
            }
        }
        return keys;
    };
    const auto old_keys = keys_in_shard("rehash_old_", 3000);
    const auto new_keys = keys_in_shard("rehash_new_", 20000);
    for (const auto& key : old_keys) {
        PutAndOffload(*service, a, key);
    }
    RetireDiskWithoutCleanup(*service, a.id);

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (size_t i = 0; !stop && i < new_keys.size(); ++i) {
            Put(*service, b, new_keys[i], 64);
        }
    });
    ReclaimNow(*service);  // One pass, asserted on its own.
    stop = true;
    writer.join();
    EXPECT_EQ(DiskReplicasOwnedBy(*service, a.id), 0u);
}

// The pass holds snapshot_mutex_ only per batch and steps aside for a
// waiting exclusive writer (a remount or a disk unmount).
TEST_F(MasterLivenessIsolationTest, ReclaimPassYieldsToExclusiveWriters) {
    auto service = MakeService();
    auto a = MountClient(*service, "yield_a", 0x100000000);
    auto b = MountClient(*service, "yield_b", 0x200000000);
    // Enough keys for a pass of about a second, far longer than a remount.
    OffloadDiskOnlyKeys(*service, a, "yield_key_", 1000000);
    RetireDiskWithoutCleanup(*service, a.id);

    std::atomic<bool> pass_started{false};
    std::atomic<bool> pass_done{false};
    Clock::time_point pass_end;
    auto pass = std::async(std::launch::async, [&] {
        pass_started = true;
        ReclaimNow(*service);
        pass_end = Clock::now();
        pass_done = true;
    });
    while (!pass_started) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(milliseconds(20));  // Into its batches.
    const bool started_mid_pass = !pass_done;
    // Each side stamps its own completion: the order, not a later sample.
    auto remount_end = std::async(std::launch::async, [&] {
        service->ReMountSegment({b.segment}, b.id);
        return Clock::now();
    });
    ASSERT_EQ(remount_end.wait_for(std::chrono::seconds(60)),
              std::future_status::ready);
    const auto remounted_at = remount_end.get();
    ASSERT_EQ(pass.wait_for(std::chrono::seconds(120)),
              std::future_status::ready);
    const bool remount_first = remounted_at < pass_end;
    if (!started_mid_pass) {
        GTEST_SKIP() << "the pass finished before the remount started";
    }
    EXPECT_TRUE(remount_first) << "the remount waited for the whole pass";
    EXPECT_EQ(DiskReplicasOwnedBy(*service, a.id), 0u);
}

// A promotion task for a replica of an ended registration never reaches the
// mailbox of a later registration under the same id.
TEST_F(MasterLivenessIsolationTest, PromotionEnqueueIsBoundToRegistration) {
    auto service = MakeService();
    auto a = MountClient(*service, "promo_a", 0x100000000);
    const uint64_t old_generation = Generation(*service, a.id);
    RetireDiskWithoutCleanup(*service, a.id);
    ASSERT_TRUE(service->MountLocalDiskSegment(a.id, true).has_value());
    const uint64_t new_generation = Generation(*service, a.id);
    ASSERT_NE(old_generation, new_generation);

    PromotionTaskItem task{.tenant_id = TenantId::Default().value(),
                           .key = "promo_key",
                           .size = 64};
    EXPECT_EQ(EnqueuePromotion(*service, a.id, task, old_generation),
              ErrorCode::SEGMENT_NOT_FOUND);
    EXPECT_EQ(EnqueuePromotion(*service, a.id, task, new_generation),
              ErrorCode::OK);
}

// End to end through the monitor thread: one client stops pinging; it alone
// expires, its replicas stop being served at once, and they are reclaimed in
// the background while the other client keeps pinging OK throughout.
TEST_F(MasterLivenessIsolationTest, OneExpiryLeavesOtherClientsAlive) {
    // 25 pings per TTL: a stalled test process does not expire the survivor.
    auto service = MakeService(/*ttl_sec=*/5);
    auto victim = MountClient(*service, "expiry_victim", 0x100000000);
    auto survivor = MountClient(*service, "expiry_survivor", 0x200000000);
    KeepAlive keepalive(*service, {victim.id, survivor.id});
    OffloadDiskOnlyKeys(*service, victim, "expiry_key_", 20000);
    ASSERT_TRUE(IsOk(*service, victim.id));

    keepalive.Drop(victim.id);
    EXPECT_TRUE(WaitFor([&] { return !IsOk(*service, victim.id); },
                        milliseconds(15000)));
    EXPECT_FALSE(HasReadableDiskReplicaOf(*service, "expiry_key_0", victim.id));
    EXPECT_TRUE(
        WaitFor([&] { return DiskReplicasOwnedBy(*service, victim.id) == 0; },
                milliseconds(10000)));
    EXPECT_TRUE(IsOk(*service, survivor.id));
    EXPECT_EQ(keepalive.not_ok(), 0);
}

// The synchronous mode (HA, snapshot or CXL) sweeps inside the expiry: the
// expired owner's disk registration must already be retired by then, or its
// replicas would stay behind, unreadable, until some later sweep.
TEST_F(MasterLivenessIsolationTest, SynchronousExpiryReclaimsDiskReplicas) {
    const std::string dir =
        std::string("/tmp/liveness_snapshot_") + std::to_string(::getpid());
    std::filesystem::create_directories(dir);
    ::setenv("MOONCAKE_SNAPSHOT_LOCAL_PATH", dir.c_str(), 1);
    MasterServiceConfig config;
    config.enable_offload = true;
    config.default_kv_lease_ttl = 0;
    config.client_live_ttl_sec = 1;
    config.enable_snapshot = true;
    config.snapshot_backup_dir = dir;
    config.snapshot_object_store_type = "local";
    auto service = std::make_unique<MasterService>(config);
    ASSERT_FALSE(BackgroundCleanup(*service));

    auto victim = MountClient(*service, "sync_victim", 0x100000000);
    OffloadDiskOnlyKeys(*service, victim, "sync_key_", 2000);
    std::this_thread::sleep_for(milliseconds(1100));  // Past its TTL.
    // The expiry the monitor would carry out, without racing it: no
    // background worker in this mode, so the expiry itself reclaims them.
    ExpireNow(*service, victim.id, Clock::now());
    EXPECT_FALSE(IsOk(*service, victim.id));
    EXPECT_EQ(DiskReplicasOwnedBy(*service, victim.id), 0u);
}

// A client selected for expiry that pings before the expiry is carried out
// stays, and keeps its pending graceful unmount.
TEST_F(MasterLivenessIsolationTest, FreshPingKeepsGracefulUnmountDeadline) {
    auto service = MakeService();
    auto a = MountClient(*service, "graceful_a", 0x100000000);
    ASSERT_TRUE(service
                    ->GracefulUnmountSegment(a.segment.id, a.id,
                                             /*grace_period_ms=*/500)
                    .has_value());
    const auto selected_at = Clock::now() - std::chrono::hours(1);
    ASSERT_TRUE(service->Ping(a.id).has_value());  // Pinged since selection.
    ExpireNow(*service, a.id, selected_at);
    EXPECT_TRUE(IsOk(*service, a.id));
    // The graceful unmount still completes at its deadline.
    EXPECT_TRUE(WaitFor(
        [&] {
            return !service->QuerySegmentStatus(a.segment.name).has_value();
        },
        milliseconds(5000)));
}

// An ended registration's disk replica is not a backup: eviction must not
// drop the last servable (memory) copy because of it.
TEST_F(MasterLivenessIsolationTest, EvictionIgnoresEndedRegistrationBackup) {
    MasterServiceConfig config;
    config.enable_offload = true;
    config.default_kv_lease_ttl = 0;
    config.client_live_ttl_sec = 3600;
    config.offload_on_evict = true;
    config.offload_force_evict = false;
    auto service = std::make_unique<MasterService>(config);
    auto a = MountClient(*service, "evict_a", 0x100000000);
    auto b = MountClient(*service, "evict_b", 0x200000000);
    PutAndOffload(*service, a, "evict_key");
    RetireDiskWithoutCleanup(*service, a.id);

    service->RunBatchEvictForTesting(1.0, 1.0);
    auto replicas = service->GetReplicaList("evict_key", TenantId::Default());
    ASSERT_TRUE(replicas.has_value())
        << "the last servable copy was evicted against a dead backup";
    bool has_memory = false;
    for (const auto& descriptor : replicas->replicas) {
        has_memory = has_memory || descriptor.is_memory_replica();
    }
    EXPECT_TRUE(has_memory);
}

// An admission racing a retirement: the replica was bound to the old
// registration before the retirement and reaches its shard after. It is
// refused there, under the shard lock, with no cleanup pass needed (one may
// already have passed this shard). Both admission paths: a disk-only key
// (AddReplica) and an offload of a memory replica (the existing-object path).
TEST_F(MasterLivenessIsolationTest, AdmissionRacingRetirementIsRefused) {
    auto service = MakeService();
    auto a = MountClient(*service, "race_a", 0x100000000);
    Put(*service, a, "race_existing");
    for (const std::string key : {"race_disk_only", "race_existing"}) {
        std::promise<void> holding;
        std::promise<void> release;
        std::thread holder([&] {
            auto shard = LockShard(*service, key);
            holding.set_value();
            release.get_future().wait();
        });
        holding.get_future().wait();
        auto admission = std::async(std::launch::async,
                                    [&] { return Offload(*service, a, key); });
        // Into its wait for the shard lock, past the generation it binds
        // (taken before the shard lock): still waiting when the registration
        // ends, so the refusal is the shard-held re-check's.
        std::this_thread::sleep_for(milliseconds(500));
        ASSERT_EQ(admission.wait_for(milliseconds(0)),
                  std::future_status::timeout)
            << key;
        RetireDiskAsExpiryDoes(*service, a.id);
        release.set_value();
        holder.join();
        auto result = admission.get();
        ASSERT_FALSE(result.has_value()) << key;
        EXPECT_EQ(result.error(), ErrorCode::SEGMENT_NOT_FOUND) << key;
        EXPECT_EQ(DiskReplicasOwnedBy(*service, a.id), 0u) << key;
        ASSERT_TRUE(service->MountLocalDiskSegment(a.id, true).has_value());
    }
}

// A replica bound to an earlier registration than the owner's current one
// is refused, not rebound to the new one and credited there.
TEST_F(MasterLivenessIsolationTest, AddReplicaRefusesAnEndedBinding) {
    auto service = MakeService();
    auto a = MountClient(*service, "bound_a", 0x100000000);
    const uint64_t old_generation = Generation(*service, a.id);
    RetireDiskWithoutCleanup(*service, a.id);
    ASSERT_TRUE(service->MountLocalDiskSegment(a.id, true).has_value());
    Replica replica(a.id, 64, a.segment.name, ReplicaStatus::COMPLETE);
    replica.set_local_disk_generation(old_generation);
    auto result = AddReplica(*service, a.id, "bound_key", replica);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::SEGMENT_NOT_FOUND);
    EXPECT_EQ(UsedBytes(*service, a.id), 0);
}

// A snapshot restore binds every restored LOCAL_DISK replica to its owner's
// restored registration and credits its bytes there; until then (and for an
// owner without one) a replica is bound to none and is never served. Then a
// retirement and a same-id re-registration: the old replica is garbage, its
// re-adoption is bound to the new registration, and usage balances.
TEST_F(MasterLivenessIsolationTest, RestoreBindsDiskReplicasToRegistrations) {
    auto service = MakeService();
    auto a = MountClient(*service, "restore_a", 0x100000000);
    auto b = MountClient(*service, "restore_b", 0x200000000);
    OffloadDiskOnlyKeys(*service, a, "restore_a_", 10);  // 64 bytes each.
    OffloadDiskOnlyKeys(*service, b, "restore_b_", 10);
    std::vector<std::string> a_keys, b_keys;
    for (int i = 0; i < 10; ++i) {
        a_keys.push_back("restore_a_" + std::to_string(i));
        b_keys.push_back("restore_b_" + std::to_string(i));
    }
    DecodeAsRestored(*service, a_keys, a.id);
    DecodeAsRestored(*service, b_keys, b.id);
    RetireDiskWithoutCleanup(*service, b.id);  // b's registration not restored.
    EXPECT_EQ(UsedBytes(*service, a.id), 0);
    EXPECT_FALSE(HasReadableDiskReplicaOf(*service, "restore_a_0", a.id))
        << "a replica bound to no registration was served";

    BindRestored(*service);
    EXPECT_EQ(DiskGeneration(*service, "restore_a_0", a.id),
              Generation(*service, a.id));
    EXPECT_TRUE(HasReadableDiskReplicaOf(*service, "restore_a_0", a.id));
    EXPECT_EQ(UsedBytes(*service, a.id), 10 * 64);
    EXPECT_EQ(DiskGeneration(*service, "restore_b_0", b.id), 0u);
    EXPECT_FALSE(HasReadableDiskReplicaOf(*service, "restore_b_0", b.id));

    // Retire a and register it again under the same id.
    RetireDiskWithoutCleanup(*service, a.id);
    ASSERT_TRUE(service->MountLocalDiskSegment(a.id, true).has_value());
    EXPECT_FALSE(HasReadableDiskReplicaOf(*service, "restore_a_0", a.id))
        << "a retired registration's replica came back after re-registration";
    ASSERT_TRUE(Offload(*service, a, "restore_a_0", 64).has_value());
    EXPECT_TRUE(HasReadableDiskReplicaOf(*service, "restore_a_0", a.id));
    ReclaimNow(*service);
    EXPECT_EQ(DiskReplicasOwnedBy(*service, a.id), 1u);  // The re-adopted one.
    EXPECT_EQ(DiskReplicasOwnedBy(*service, b.id), 0u);
    EXPECT_EQ(UsedBytes(*service, a.id), 64);
    EXPECT_TRUE(HasReadableDiskReplicaOf(*service, "restore_a_0", a.id));
}

// Re-adoption in place (HA keeps an ended registration's replica until its
// removal is durable, so the admission finds it): rebinding moves it to the
// new registration once, and a duplicate report is not credited again.
TEST_F(MasterLivenessIsolationTest, ReadoptionRebindsInPlace) {
    auto service = MakeService();
    auto a = MountClient(*service, "rebind_a", 0x100000000);
    PutAndOffload(*service, a, "rebind_key", 64);
    RetireDiskWithoutCleanup(*service, a.id);
    ASSERT_TRUE(service->MountLocalDiskSegment(a.id, true).has_value());
    ASSERT_FALSE(HasReadableDiskReplicaOf(*service, "rebind_key", a.id));

    Replica incoming(a.id, 64, a.segment.name, ReplicaStatus::COMPLETE);
    incoming.set_local_disk_generation(Generation(*service, a.id));
    EXPECT_TRUE(Rebind(*service, "rebind_key", incoming));
    EXPECT_TRUE(HasReadableDiskReplicaOf(*service, "rebind_key", a.id));
    EXPECT_FALSE(Rebind(*service, "rebind_key", incoming)) << "duplicate";
    ReclaimNow(*service);
    EXPECT_EQ(DiskReplicasOwnedBy(*service, a.id), 1u);
}

// A replica already marked REMOVED (HA: its removal awaits durability) is
// not rebound: the re-adoption is added beside it instead.
TEST_F(MasterLivenessIsolationTest, ReadoptionDoesNotReviveARemovedReplica) {
    auto service = MakeService();
    auto a = MountClient(*service, "removed_a", 0x100000000);
    PutAndOffload(*service, a, "removed_key", 64);
    RetireDiskWithoutCleanup(*service, a.id);
    ASSERT_TRUE(service->MountLocalDiskSegment(a.id, true).has_value());
    WithDiskReplica(*service, "removed_key", a.id,
                    [](Replica& r, auto&) { r.mark_removed(); });
    Replica incoming(a.id, 64, a.segment.name, ReplicaStatus::COMPLETE);
    incoming.set_local_disk_generation(Generation(*service, a.id));
    EXPECT_FALSE(Rebind(*service, "removed_key", incoming));
    size_t completed = 0;
    WithDiskReplica(*service, "removed_key", a.id,
                    [&](Replica& r, auto&) { completed += r.is_completed(); });
    EXPECT_EQ(completed, 0u) << "a REMOVED replica was rebound";
}

// A promotion pushed for a source of an ended registration finds no
// mailbox: the holder's later registration never receives it.
TEST_F(MasterLivenessIsolationTest, PromotionPushAfterRetirementIsRefused) {
    auto service = MakeService();
    auto a = MountClient(*service, "push_a", 0x100000000);
    PutAndOffload(*service, a, "push_key", 64);
    EXPECT_TRUE(PushPromotion(*service, "push_key", a.id).has_value());
    RetireDiskWithoutCleanup(*service, a.id);
    ASSERT_TRUE(service->MountLocalDiskSegment(a.id, true).has_value());
    auto result = PushPromotion(*service, "push_key", a.id);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::SEGMENT_NOT_FOUND);
}

// An expired client's memory replicas are unreadable at once (its segment's
// allocator is released) and their keys are reclaimed by the pass.
TEST_F(MasterLivenessIsolationTest, ExpiredMemoryReplicasAreReclaimed) {
    auto service = MakeService(/*ttl_sec=*/1);
    auto victim = MountClient(*service, "mem_victim", 0x100000000);
    for (int i = 0; i < 100; ++i) {
        Put(*service, victim, "mem_key_" + std::to_string(i));
    }
    ASSERT_EQ(service->GetKeyCount(), 100u);
    std::this_thread::sleep_for(milliseconds(1100));
    ExpireNow(*service, victim.id, Clock::now());
    ASSERT_FALSE(IsOk(*service, victim.id));
    EXPECT_FALSE(
        service->GetReplicaList("mem_key_0", TenantId::Default()).has_value());
    EXPECT_TRUE(WaitFor([&] { return service->GetKeyCount() == 0; },
                        milliseconds(10000)));
}

// With offload disabled, LOCAL_DISK replicas are admitted without a
// registration. Their owner expiring with neither a memory segment nor a
// registration still schedules the pass that reclaims them.
TEST_F(MasterLivenessIsolationTest, ExpiryWithoutSegmentsStillReclaims) {
    MasterServiceConfig config;
    config.enable_offload = false;
    config.default_kv_lease_ttl = 0;
    config.client_live_ttl_sec = 1;
    auto service = std::make_unique<MasterService>(config);
    Client x{generate_uuid(), {}};
    x.segment.name = "bare_x";
    ASSERT_TRUE(service->ReMountSegment({}, x.id).has_value());  // OK, bare.
    OffloadDiskOnlyKeys(*service, x, "bare_key_", 10);
    ASSERT_EQ(DiskReplicasOwnedBy(*service, x.id), 10u);
    std::this_thread::sleep_for(milliseconds(1100));
    ExpireNow(*service, x.id, Clock::now());
    EXPECT_FALSE(IsOk(*service, x.id));
    EXPECT_TRUE(
        WaitFor([&] { return DiskReplicasOwnedBy(*service, x.id) == 0; },
                milliseconds(10000)));
    auto ping = service->Ping(x.id);
    ASSERT_TRUE(ping.has_value());
    EXPECT_EQ(ping->client_status, ClientStatus::NEED_REMOUNT);
}

// A tenant far over its quota (after a capacity drop or a lowered quota) is
// not trimmed inside a write's RPC: on eu-west1 one Put evicted a 519k-object
// shortfall inline, holding its RPC IO thread for 33 s, and the clients on
// that thread expired. The write examines at most kInlineEvictionKeyBudget
// keys and is refused; the background trim evicts the shortfall; the retry
// succeeds.
TEST_F(MasterLivenessIsolationTest, LargeQuotaOverageIsTrimmedNotInline) {
    constexpr uint64_t kKiB = 1 << 10;
    auto service = MakeQuotaService("trim", 4ULL << 30);
    auto a = MountMemoryClient(*service, "trim_a", 0x100000000, 4ULL << 30);
    for (int i = 0; i < 20000; ++i) {
        ASSERT_TRUE(
            PutIn(*service, a, "trim", "trim_" + std::to_string(i), 64 * kKiB));
    }
    StopQuotaTrimWorker(*service);  // Only the write path, deterministically.
    ASSERT_TRUE(
        service->UpsertTenantQuotaPolicy(TenantId("trim"), 64ULL << 20));
    const uint64_t before = Charged(*service, "trim");
    tl::expected<void, ErrorCode> refused;
    const auto took = Time(
        [&] { refused = PutIn(*service, a, "trim", "trim_new", 64 * kKiB); });
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), ErrorCode::TENANT_QUOTA_EXCEEDED);
    // At most the budget's worth of objects, on each of the write's attempts.
    EXPECT_GE(Charged(*service, "trim"),
              before - 3 * kInlineBudget * 64 * kKiB);
    EXPECT_LT(took.count(), 1000);

    TrimNow(*service);
    EXPECT_LE(Charged(*service, "trim"), Effective(*service, "trim"));
    EXPECT_TRUE(PutIn(*service, a, "trim", "trim_new", 64 * kKiB).has_value());
}

// The same bound when nothing is evictable: a tenant at its quota whose keys
// are all under a read lease no longer has one write scan all of them.
TEST_F(MasterLivenessIsolationTest, WriteToLeasedTenantIsRefusedQuickly) {
    constexpr uint64_t kKiB = 1 << 10;
    auto service = MakeQuotaService("leased", 400000 * 4 * kKiB,
                                    /*lease_ms=*/600000);
    auto a = MountMemoryClient(*service, "leased_a", 0x100000000, 4ULL << 30);
    for (int i = 0; i < 400000; ++i) {
        ASSERT_TRUE(PutIn(*service, a, "leased", "leased_" + std::to_string(i),
                          4 * kKiB));
        // Read: a lease, so it is not evictable.
        ASSERT_TRUE(service->ExistKey("leased_" + std::to_string(i),
                                      TenantId("leased")));
    }
    StopQuotaTrimWorker(*service);
    tl::expected<void, ErrorCode> refused;
    const auto took = Time([&] {
        refused = PutIn(*service, a, "leased", "leased_new", 4 * kKiB);
    });
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), ErrorCode::TENANT_QUOTA_EXCEEDED);
    LOG(INFO) << "refused after " << took.count() << " ms";
    EXPECT_LT(took.count(), 200);
}

// The steady state is unchanged: a tenant at its quota makes room for one
// more write inline.
TEST_F(MasterLivenessIsolationTest, WriteAtQuotaStillEvictsInline) {
    constexpr uint64_t kMiB = 1 << 20;
    auto service = MakeQuotaService("steady", 64 * kMiB);
    auto a = MountMemoryClient(*service, "steady_a", 0x100000000, 1024 * kMiB);
    for (int i = 0; i < 64; ++i) {
        ASSERT_TRUE(
            PutIn(*service, a, "steady", "steady_" + std::to_string(i), kMiB));
    }
    StopQuotaTrimWorker(*service);
    EXPECT_TRUE(PutIn(*service, a, "steady", "steady_more", kMiB).has_value());
    EXPECT_LE(Charged(*service, "steady"), 64 * kMiB);
}

// A remount (an exclusive snapshot writer, on its RPC IO thread) waits for at
// most one shard section of a long eviction, never the whole of it: the quota
// trim and BatchEvict take snapshot_mutex_ per shard and step aside.
TEST_F(MasterLivenessIsolationTest, RemountDoesNotWaitOutAnEviction) {
    constexpr uint64_t kKiB = 1 << 10;
    for (const bool batch_evict : {false, true}) {
        auto service = MakeQuotaService("big", 1ULL << 40);
        auto a =
            MountMemoryClient(*service, "evict_a", 0x100000000, 8ULL << 30);
        auto b = MountMemoryClient(*service, "evict_b", 0x400000000, 1 << 26);
        for (int i = 0; i < 400000; ++i) {
            ASSERT_TRUE(PutIn(*service, a, "big", "big_" + std::to_string(i),
                              4 * kKiB));
        }
        StopQuotaTrimWorker(*service);
        ASSERT_TRUE(service->UpsertTenantQuotaPolicy(TenantId("big"), kKiB));

        std::atomic<bool> started{false};
        Clock::time_point pass_end;
        auto pass = std::async(std::launch::async, [&] {
            started = true;
            if (batch_evict) {
                service->RunBatchEvictForTesting(1.0, 1.0);
            } else {
                TrimNow(*service);
            }
            pass_end = Clock::now();
        });
        while (!started) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(milliseconds(30));  // Into its shards.
        const auto remount_started = Clock::now();
        const auto remount = Time(
            [&] { ASSERT_TRUE(service->ReMountSegment({b.segment}, b.id)); });
        ASSERT_EQ(pass.wait_for(std::chrono::seconds(120)),
                  std::future_status::ready);
        const auto left = std::chrono::duration_cast<milliseconds>(
            pass_end - remount_started);
        LOG(INFO) << (batch_evict ? "BatchEvict" : "quota trim")
                  << ": remount_ms=" << remount.count()
                  << " eviction_left_when_remount_started_ms=" << left.count();
        if (left.count() < 100) {
            continue;  // Too little of the eviction left to tell.
        }
        EXPECT_LT(remount.count(), left.count() / 2)
            << "the remount waited out the eviction";
    }
}

// The scale shape of the incident, in process: a client with many LOCAL_DISK
// replicas leaves while other clients keep pinging and remounting. Ping and
// remount latency must not grow with the index. MOONCAKE_LIVENESS_SCALE_KEYS
// sets the key count (200000 is a quick run; the incident had about 13.8M).
TEST_F(MasterLivenessIsolationTest, LatencyStaysFlatWhileManyKeysAreReclaimed) {
    // A benchmark (wall-clock bounds), run on request.
    const char* env = std::getenv("MOONCAKE_LIVENESS_SCALE_KEYS");
    if (!env) {
        GTEST_SKIP() << "set MOONCAKE_LIVENESS_SCALE_KEYS to run";
    }
    const size_t keys = std::stoull(env);
    auto service = MakeService();
    auto victim = MountClient(*service, "scale_victim", 0x100000000);
    auto other = MountClient(*service, "scale_other", 0x200000000);
    OffloadDiskOnlyKeys(*service, victim, "scale_key_", keys);
    ASSERT_EQ(DiskReplicasOwnedBy(*service, victim.id), keys);

    milliseconds unmount = Time([&] {
        ASSERT_TRUE(service->UnmountLocalDiskSegment(victim.id).has_value());
    });
    milliseconds worst_ping{0};
    milliseconds worst_remount{0};
    const auto reclaim_start = Clock::now();
    do {
        for (int i = 0; i < 20; ++i) {
            worst_ping =
                std::max(worst_ping, Time([&] {
                             ASSERT_TRUE(service->Ping(other.id).has_value());
                         }));
            std::this_thread::sleep_for(milliseconds(10));
        }
        worst_remount = std::max(
            worst_remount, Time([&] {
                ASSERT_TRUE(service->ReMountSegment({other.segment}, other.id)
                                .has_value());
            }));
    } while (DiskReplicasOwnedBy(*service, victim.id) != 0 &&
             Clock::now() - reclaim_start < std::chrono::minutes(10));
    const auto reclaim =
        std::chrono::duration_cast<milliseconds>(Clock::now() - reclaim_start);
    LOG(INFO) << "keys=" << keys << " unmount_ms=" << unmount.count()
              << " reclaim_ms=" << reclaim.count()
              << " worst_ping_ms=" << worst_ping.count()
              << " worst_remount_ms=" << worst_remount.count();
    EXPECT_EQ(DiskReplicasOwnedBy(*service, victim.id), 0u);
    // Generous bounds (instrumented builds, loaded runners): the unpatched
    // unmount alone grows by about 0.7 ms per thousand keys.
    EXPECT_LT(unmount.count(), 1000);
    EXPECT_LT(worst_ping.count(), 500);
    EXPECT_LT(worst_remount.count(), 2000);
}

}  // namespace mooncake::test

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
