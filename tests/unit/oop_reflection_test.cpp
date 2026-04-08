#include <gtest/gtest.h>

#include "tie/vm/runtime/object_model.hpp"
#include "tie/vm/runtime/reflection_registry.hpp"

namespace tie::vm {

TEST(OopReflectionTest, MultiInheritanceUsesC3) {
    ObjectModel model;
    ReflectionRegistry refl(&model);

    ClassDescriptor a;
    a.name = "A";
    a.methods["who"] = MethodDescriptor{
        "who",
        AccessModifier::kPublic,
        true,
        [](ObjectId, const std::vector<Value>&) { return StatusOr<Value>(Value::Int64(1)); }};
    ASSERT_TRUE(refl.RegisterClass(std::move(a)).ok());

    ClassDescriptor b;
    b.name = "B";
    b.base_classes = {"A"};
    b.methods["who"] = MethodDescriptor{
        "who",
        AccessModifier::kPublic,
        true,
        [](ObjectId, const std::vector<Value>&) { return StatusOr<Value>(Value::Int64(2)); }};
    ASSERT_TRUE(refl.RegisterClass(std::move(b)).ok());

    ClassDescriptor c;
    c.name = "C";
    c.base_classes = {"A"};
    c.methods["who"] = MethodDescriptor{
        "who",
        AccessModifier::kPublic,
        true,
        [](ObjectId, const std::vector<Value>&) { return StatusOr<Value>(Value::Int64(3)); }};
    ASSERT_TRUE(refl.RegisterClass(std::move(c)).ok());

    ClassDescriptor d;
    d.name = "D";
    d.base_classes = {"B", "C"};
    ASSERT_TRUE(refl.RegisterClass(std::move(d)).ok());

    auto mro_or = model.ComputeMro("D");
    ASSERT_TRUE(mro_or.ok()) << mro_or.status().message();
    ASSERT_GE(mro_or.value().size(), 4u);
    EXPECT_EQ(mro_or.value()[0], "D");
    EXPECT_EQ(mro_or.value()[1], "B");
    EXPECT_EQ(mro_or.value()[2], "C");

    auto obj_or = refl.NewObject("D");
    ASSERT_TRUE(obj_or.ok()) << obj_or.status().message();
    auto result_or = refl.Invoke(obj_or.value(), "who", {});
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 2);
}

TEST(OopReflectionTest, PrivateMethodIsBlockedByDefault) {
    ObjectModel model;
    ReflectionRegistry refl(&model);

    ClassDescriptor secret;
    secret.name = "Secret";
    secret.methods["hidden"] = MethodDescriptor{
        "hidden",
        AccessModifier::kPrivate,
        true,
        [](ObjectId, const std::vector<Value>&) { return StatusOr<Value>(Value::Int64(42)); }};
    ASSERT_TRUE(refl.RegisterClass(std::move(secret)).ok());
    auto obj_or = refl.NewObject("Secret");
    ASSERT_TRUE(obj_or.ok()) << obj_or.status().message();
    auto result_or = refl.Invoke(obj_or.value(), "hidden", {});
    EXPECT_FALSE(result_or.ok());
}

}  // namespace tie::vm

