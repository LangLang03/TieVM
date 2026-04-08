#include <atomic>

#include <gtest/gtest.h>

#include "tie/vm/gc/gc_controller.hpp"

namespace tie::vm {

TEST(GcTest, MinorCollectionKeepsReachable) {
    GcController gc;
    auto root_or = gc.Allocate();
    auto child_or = gc.Allocate();
    ASSERT_TRUE(root_or.ok());
    ASSERT_TRUE(child_or.ok());
    ASSERT_TRUE(gc.AddRoot(root_or.value()).ok());
    ASSERT_TRUE(gc.AddReference(root_or.value(), child_or.value()).ok());
    ASSERT_TRUE(gc.CollectMinor().ok());
    EXPECT_GE(gc.LiveObjects(), 2u);
}

TEST(GcTest, WeakPhantomAndFinalizerWork) {
    GcController gc;
    std::atomic<int> finalized{0};

    auto obj_or = gc.Allocate();
    ASSERT_TRUE(obj_or.ok());
    const ObjectId id = obj_or.value();
    auto weak_or = gc.CreateWeakRef(id);
    auto phantom_or = gc.CreatePhantomRef(id);
    ASSERT_TRUE(weak_or.ok());
    ASSERT_TRUE(phantom_or.ok());
    ASSERT_TRUE(gc.RegisterFinalizer(id, [&](ObjectId) { finalized.fetch_add(1); }).ok());

    ASSERT_TRUE(gc.CollectMinor().ok());
    ASSERT_TRUE(gc.CollectMajorAsync().ok());
    ASSERT_TRUE(gc.WaitForMajor().ok());

    EXPECT_FALSE(gc.ResolveWeakRef(weak_or.value()).has_value());
    auto phantom = gc.DrainPhantomQueue();
    EXPECT_FALSE(phantom.empty());
    EXPECT_GT(finalized.load(), 0);
}

}  // namespace tie::vm

