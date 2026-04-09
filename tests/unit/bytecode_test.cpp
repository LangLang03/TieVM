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

TEST(BytecodeTest, SerializeDeserializeFfiMetadataRoundTrip) {
    Module module("ffi.meta");
    const auto symbol = module.AddConstant(Constant::Utf8("demo.add"));
    const auto lib_idx = module.AddFfiLibraryPath("libs/windows/x64/tievm_std_native.dll");
    FunctionSignature signature;
    signature.name = "demo.add";
    signature.convention = CallingConvention::kSystem;
    signature.return_type = {AbiValueKind::kI32, OwnershipQualifier::kBorrowed};
    signature.params = {
        {AbiValueKind::kI32, OwnershipQualifier::kBorrowed},
        {AbiValueKind::kI32, OwnershipQualifier::kBorrowed},
    };
    const auto sig_idx = module.AddFfiSignature(std::move(signature));
    const auto bind_idx = module.AddFfiBinding(
        FfiSymbolBinding{"demo.add", "tie_demo_add", lib_idx, sig_idx});
    auto& fn = module.AddFunction("entry", 8, 0);
    fn.ffi_binding().enabled = true;
    fn.ffi_binding().convention = CallingConvention::kSystem;
    fn.ffi_binding().signature_index = sig_idx;
    fn.ffi_binding().binding_index = bind_idx;
    auto k20 = module.AddConstant(Constant::Int64(20));
    auto k22 = module.AddConstant(Constant::Int64(22));
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).LoadK(2, k20).LoadK(3, k22).FfiCall(1, symbol, 2).Ret(1);
    module.set_entry_function(0);

    auto bytes_or = Serializer::Serialize(module, true);
    ASSERT_TRUE(bytes_or.ok()) << bytes_or.status().message();
    auto parsed_or = Serializer::Deserialize(bytes_or.value());
    ASSERT_TRUE(parsed_or.ok()) << parsed_or.status().message();
    const auto& parsed = parsed_or.value();
    ASSERT_EQ(parsed.ffi_library_paths().size(), 1u);
    ASSERT_EQ(parsed.ffi_signatures().size(), 1u);
    ASSERT_EQ(parsed.ffi_bindings().size(), 1u);
    EXPECT_TRUE(parsed.functions()[0].ffi_binding().enabled);
    EXPECT_EQ(parsed.ffi_bindings()[0].native_symbol, "tie_demo_add");
}

}  // namespace tie::vm
