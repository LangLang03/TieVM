#include <gtest/gtest.h>

#include "tie/vm/api.hpp"

namespace tie::vm {

TEST(FfiTest, VmToCFunctionCall) {
    VmInstance vm;
    FunctionSignature sig;
    sig.name = "native_add";
    sig.convention = CallingConvention::kCdecl;
    sig.return_type = {AbiValueKind::kI64, OwnershipQualifier::kBorrowed};
    sig.params = {
        {AbiValueKind::kI64, OwnershipQualifier::kBorrowed},
        {AbiValueKind::kI64, OwnershipQualifier::kBorrowed},
    };
    ASSERT_TRUE(vm.ffi().RegisterFunction(
                          std::move(sig),
                          [](VmThread&, const std::vector<Value>& args) {
                              return StatusOr<Value>(
                                  Value::Int64(args[0].AsInt64() + args[1].AsInt64()));
                          })
                    .ok());

    Module module("ffi.test");
    const auto k5 = module.AddConstant(Constant::Int64(5));
    const auto k7 = module.AddConstant(Constant::Int64(7));
    const auto sym = module.AddConstant(Constant::Utf8("native_add"));
    auto& fn = module.AddFunction("entry", 8, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(1, k5)
        .LoadK(2, k7)
        .FfiCall(0, sym, 2)
        .Ret(0);
    module.set_entry_function(0);

    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 12);
}

TEST(FfiTest, ExternalThreadMustAttachForVmCallback) {
    VmInstance vm;
    ASSERT_TRUE(vm.ffi().RegisterVmCallback(
                          "plus_one",
                          [](const std::vector<Value>& args) {
                              return StatusOr<Value>(Value::Int64(args[0].AsInt64() + 1));
                          })
                    .ok());

    auto fail_or = vm.ffi().InvokeVmCallback("plus_one", {Value::Int64(1)});
    EXPECT_FALSE(fail_or.ok());

    ASSERT_TRUE(vm.ffi().AttachCurrentThread().ok());
    auto ok_or = vm.ffi().InvokeVmCallback("plus_one", {Value::Int64(1)});
    ASSERT_TRUE(ok_or.ok()) << ok_or.status().message();
    EXPECT_EQ(ok_or.value().AsInt64(), 2);
    ASSERT_TRUE(vm.ffi().DetachCurrentThread().ok());
}

}  // namespace tie::vm

