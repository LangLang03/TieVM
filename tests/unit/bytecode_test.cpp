#include <gtest/gtest.h>

#include "test_helpers.hpp"

namespace tie::vm {

TEST(BytecodeTest, InstructionIsFixedLength) { EXPECT_EQ(kInstructionSize, 16u); }

TEST(BytecodeTest, SerializeDeserializeRoundTrip) {
    Module module = test::BuildAddModule(2, 3);
    auto bytes_or = Serializer::Serialize(module, true);
    ASSERT_TRUE(bytes_or.ok()) << bytes_or.status().message();
    auto parsed_or = Serializer::Deserialize(bytes_or.value());
    ASSERT_TRUE(parsed_or.ok()) << parsed_or.status().message();
    EXPECT_EQ(parsed_or.value().functions().size(), 1u);
    EXPECT_EQ(parsed_or.value().constants().size(), 2u);
    EXPECT_EQ(parsed_or.value().name(), "test.math");
}

TEST(BytecodeTest, VerifierRejectsInvalidRegister) {
    Module module("invalid");
    module.AddConstant(Constant::Int64(1));
    auto& fn = module.AddFunction("entry", 1, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).LoadK(0, 0).Add(0, 0, 2).Ret(0);
    module.set_entry_function(0);
    auto result = Verifier::Verify(module);
    EXPECT_FALSE(result.status.ok());
}

}  // namespace tie::vm
