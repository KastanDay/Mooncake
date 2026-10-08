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
#include <chrono>
#include <future>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "master_service.h"
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
        std::shared_lock<std::shared_mutex> lock(service.client_mutex_);
        return service.ok_client_.contains(client_id);
    }

    static void ReclaimNow(MasterService& service) {
        service.ClearInvalidHandles();
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
    auto pass = std::async(std::launch::async, [&] {
        pass_started = true;
        ReclaimNow(*service);
        pass_done = true;
    });
    while (!pass_started) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(milliseconds(20));  // Into its batches.
    const bool started_mid_pass = !pass_done;
    // The remount thread itself records whether the pass was still running
    // when it finished: the order, not a later observation.
    auto finished_mid_pass = std::async(std::launch::async, [&] {
        service->ReMountSegment({b.segment}, b.id);
        return !pass_done.load();
    });
    ASSERT_EQ(finished_mid_pass.wait_for(std::chrono::seconds(60)),
              std::future_status::ready);
    const bool remount_first = finished_mid_pass.get();
    ASSERT_EQ(pass.wait_for(std::chrono::seconds(120)),
              std::future_status::ready);
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
    auto service = MakeService(/*ttl_sec=*/2);
    auto victim = MountClient(*service, "expiry_victim", 0x100000000);
    auto survivor = MountClient(*service, "expiry_survivor", 0x200000000);
    KeepAlive keepalive(*service, {victim.id, survivor.id});
    OffloadDiskOnlyKeys(*service, victim, "expiry_key_", 20000);
    ASSERT_TRUE(IsOk(*service, victim.id));

    keepalive.Drop(victim.id);
    EXPECT_TRUE(WaitFor([&] { return !IsOk(*service, victim.id); },
                        milliseconds(10000)));
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
    config.client_live_ttl_sec = 2;
    config.enable_snapshot = true;
    config.snapshot_backup_dir = dir;
    config.snapshot_object_store_type = "local";
    auto service = std::make_unique<MasterService>(config);
    ASSERT_FALSE(BackgroundCleanup(*service));

    auto victim = MountClient(*service, "sync_victim", 0x100000000);
    auto survivor = MountClient(*service, "sync_survivor", 0x200000000);
    KeepAlive keepalive(*service, {victim.id, survivor.id});
    OffloadDiskOnlyKeys(*service, victim, "sync_key_", 2000);
    keepalive.Drop(victim.id);
    EXPECT_TRUE(WaitFor([&] { return !IsOk(*service, victim.id); },
                        milliseconds(10000)));
    // No background worker in this mode: the expiry itself reclaimed them.
    EXPECT_TRUE(
        WaitFor([&] { return DiskReplicasOwnedBy(*service, victim.id) == 0; },
                milliseconds(2000)));
    EXPECT_EQ(keepalive.not_ok(), 0);
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

// The scale shape of the incident, in process: a client with many LOCAL_DISK
// replicas leaves while other clients keep pinging and remounting. Ping and
// remount latency must not grow with the index. MOONCAKE_LIVENESS_SCALE_KEYS
// sets the key count (default 200000; the incident had about 13.8M).
TEST_F(MasterLivenessIsolationTest, LatencyStaysFlatWhileManyKeysAreReclaimed) {
    size_t keys = 200000;
    if (const char* env = std::getenv("MOONCAKE_LIVENESS_SCALE_KEYS")) {
        keys = std::stoull(env);
    }
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
