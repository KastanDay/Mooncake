#include "tenant_quota_packing_scale.h"

#include <gtest/gtest.h>

namespace mooncake::test {

namespace {

TenantQuotaPackingScale MakeScale(double floor = 0.8) {
    return TenantQuotaPackingScale(
        TenantQuotaPackingScale::Options{.floor = floor});
}

}  // namespace

TEST(TenantQuotaPackingScaleTest, IgnoresEmptyAndNearlyEmptyPools) {
    auto scale = MakeScale();
    EXPECT_FALSE(scale.Observe(0, 0, 0).has_value());
    EXPECT_FALSE(scale.Observe(10, 0, 100).has_value());
    // Reserved bytes under half of capacity: not representative yet.
    EXPECT_FALSE(scale.Observe(40, 49, 100).has_value());
    EXPECT_DOUBLE_EQ(scale.applied(), 1.0);
}

TEST(TenantQuotaPackingScaleTest, ConvergesGraduallyToPackingEfficiency) {
    auto scale = MakeScale();
    // First sample moves the average 10% of the way from 1.0 to 0.9.
    auto first = scale.Observe(90, 100, 100);
    ASSERT_TRUE(first.has_value());
    EXPECT_NEAR(*first, 0.99, 1e-12);

    double previous = *first;
    for (int i = 0; i < 200; ++i) {
        if (auto next = scale.Observe(90, 100, 100)) {
            EXPECT_LT(*next, previous);
            EXPECT_GE(*next, 0.9);
            previous = *next;
        }
    }
    EXPECT_NEAR(scale.applied(), 0.9, 0.005);
}

TEST(TenantQuotaPackingScaleTest, ClampsToFloorAndOne) {
    auto scale = MakeScale(/*floor=*/0.8);
    for (int i = 0; i < 200; ++i) {
        scale.Observe(50, 100, 100);  // 0.5, below the floor
    }
    EXPECT_GE(scale.applied(), 0.8);
    EXPECT_NEAR(scale.applied(), 0.8, 0.005);

    // Requested above reserved (torn reads) never scales capacity up.
    for (int i = 0; i < 200; ++i) {
        scale.Observe(120, 100, 100);
    }
    EXPECT_LE(scale.applied(), 1.0);
    EXPECT_NEAR(scale.applied(), 1.0, 0.005);
}

TEST(TenantQuotaPackingScaleTest, SmallMovesAreNotReported) {
    auto scale = MakeScale();
    for (int i = 0; i < 200; ++i) {
        EXPECT_FALSE(scale.Observe(998, 1000, 1000).has_value());
    }
    EXPECT_DOUBLE_EQ(scale.applied(), 1.0);
}

}  // namespace mooncake::test
