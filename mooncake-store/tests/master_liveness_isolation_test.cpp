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
#include <chrono>
#include <future>
#include <shared_mutex>
#include <string>
#include <thread>
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

    static std::unique_ptr<MasterService> MakeService(int64_t ttl_sec = 10) {
        MasterServiceConfig config;
        config.enable_offload = true;
        config.default_kv_lease_ttl = 0;
        config.client_live_ttl_sec = ttl_sec;
        return std::make_unique<MasterService>(config);
    }

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
    std::this_thread::sleep_for(milliseconds(200));  // Remount is now waiting.
    EXPECT_FALSE(remount_done);

    // On a regression the Ping would wait for the held pass: answer it from
    // a future, so the test fails instead of deadlocking.
    auto ping =
        std::async(std::launch::async, [&] { return service->Ping(a.id); });
    EXPECT_EQ(ping.wait_for(milliseconds(100)), std::future_status::ready)
        << "Ping waited behind the remount";

    // Nor does anything else that needs client_mutex_ wait on it.
    std::atomic<bool> client_lock_taken{false};
    std::thread client_user([&] {
        std::unique_lock<std::shared_mutex> lock(ClientMutex(*service));
        client_lock_taken = true;
    });
    EXPECT_TRUE(
        WaitFor([&] { return client_lock_taken.load(); }, milliseconds(500)))
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
        work.wait_for(milliseconds(100)) == std::future_status::ready;
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
    constexpr int kOld = 5000;
    for (int i = 0; i < kOld; ++i) {
        PutAndOffload(*service, a, "rehash_old_" + std::to_string(i));
    }
    RetireDiskWithoutCleanup(*service, a.id);

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int i = 0; !stop && i < 20000; ++i) {
            Put(*service, b, "rehash_new_" + std::to_string(i), 64);
        }
    });
    ReclaimNow(*service);
    stop = true;
    writer.join();
    ReclaimNow(*service);  // Anything the first pass met mid-rehash.
    EXPECT_EQ(DiskReplicasOwnedBy(*service, a.id), 0u);
}

// The pass holds snapshot_mutex_ only per batch and steps aside for a
// waiting exclusive writer (a remount or a disk unmount).
TEST_F(MasterLivenessIsolationTest, ReclaimPassYieldsToExclusiveWriters) {
    auto service = MakeService();
    auto a = MountClient(*service, "yield_a", 0x100000000);
    auto b = MountClient(*service, "yield_b", 0x200000000);
    constexpr int kKeys = 20000;
    for (int i = 0; i < kKeys; ++i) {
        PutAndOffload(*service, a, "yield_key_" + std::to_string(i), 64);
    }
    RetireDiskWithoutCleanup(*service, a.id);

    std::atomic<bool> pass_done{false};
    std::thread pass([&] {
        ReclaimNow(*service);
        pass_done = true;
    });
    std::this_thread::sleep_for(milliseconds(5));
    auto elapsed = Time([&] {
        ASSERT_TRUE(service->ReMountSegment({b.segment}, b.id).has_value());
    });
    pass.join();
    EXPECT_LT(elapsed.count(), 500) << "remount waited for the whole pass";
    EXPECT_EQ(DiskReplicasOwnedBy(*service, a.id), 0u);
}

// End to end through the monitor thread: one client stops pinging; it alone
// expires, its replicas stop being served at once, and they are reclaimed in
// the background while the other client keeps pinging OK throughout.
TEST_F(MasterLivenessIsolationTest, OneExpiryLeavesOtherClientsAlive) {
    auto service = MakeService(/*ttl_sec=*/2);
    auto victim = MountClient(*service, "expiry_victim", 0x100000000);
    auto survivor = MountClient(*service, "expiry_survivor", 0x200000000);
    constexpr int kKeys = 5000;
    for (int i = 0; i < kKeys; ++i) {
        PutAndOffload(*service, victim, "expiry_key_" + std::to_string(i), 64);
    }

    std::atomic<bool> stop{false};
    std::atomic<int> survivor_not_ok{0};
    std::thread pinger([&] {
        while (!stop) {
            auto ping = service->Ping(survivor.id);
            if (!ping || ping->client_status != ClientStatus::OK) {
                ++survivor_not_ok;
            }
            std::this_thread::sleep_for(milliseconds(200));
        }
    });

    EXPECT_TRUE(WaitFor([&] { return !IsOk(*service, victim.id); },
                        milliseconds(8000)));
    EXPECT_FALSE(HasReadableDiskReplicaOf(*service, "expiry_key_0", victim.id));
    auto ping = service->Ping(victim.id);
    ASSERT_TRUE(ping.has_value());
    EXPECT_EQ(ping->client_status, ClientStatus::NEED_REMOUNT);
    EXPECT_TRUE(
        WaitFor([&] { return DiskReplicasOwnedBy(*service, victim.id) == 0; },
                milliseconds(10000)));
    stop = true;
    pinger.join();
    EXPECT_EQ(survivor_not_ok.load(), 0);
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
    {
        // Disk replicas directly, without memory replicas: the index the
        // reclaim pass walks, at a fraction of the setup time.
        std::vector<OffloadTaskItem> tasks;
        std::vector<StorageObjectMetadata> metas;
        for (size_t i = 0; i < keys; ++i) {
            tasks.push_back({.tenant_id = TenantId::Default().value(),
                             .key = "scale_key_" + std::to_string(i),
                             .size = 64});
            StorageObjectMetadata meta;
            meta.data_size = 64;
            meta.transport_endpoint = victim.segment.name;
            metas.push_back(meta);
            if (tasks.size() == 4096 || i + 1 == keys) {
                ASSERT_TRUE(
                    service->NotifyOffloadSuccess(victim.id, tasks, metas)
                        .has_value());
                tasks.clear();
                metas.clear();
            }
        }
    }
    ASSERT_EQ(DiskReplicasOwnedBy(*service, victim.id), keys);

    milliseconds unmount = Time([&] {
        ASSERT_TRUE(service->UnmountLocalDiskSegment(victim.id).has_value());
    });
    milliseconds worst_ping{0};
    milliseconds worst_remount{0};
    const auto reclaim_start = Clock::now();
    while (DiskReplicasOwnedBy(*service, victim.id) != 0 &&
           Clock::now() - reclaim_start < std::chrono::minutes(10)) {
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
    }
    const auto reclaim =
        std::chrono::duration_cast<milliseconds>(Clock::now() - reclaim_start);
    LOG(INFO) << "keys=" << keys << " unmount_ms=" << unmount.count()
              << " reclaim_ms=" << reclaim.count()
              << " worst_ping_ms=" << worst_ping.count()
              << " worst_remount_ms=" << worst_remount.count();
    EXPECT_EQ(DiskReplicasOwnedBy(*service, victim.id), 0u);
    EXPECT_LT(unmount.count(), 100);
    EXPECT_LT(worst_ping.count(), 50);
    EXPECT_LT(worst_remount.count(), 500);
}

}  // namespace mooncake::test

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
