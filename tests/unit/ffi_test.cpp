#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

#include "tie/vm/api.hpp"

namespace tie::vm {

namespace {

std::filesystem::path FindNativeLib() {
#if defined(_WIN32)
    const char* file = "tievm_std_native.dll";
#elif defined(__APPLE__)
    const char* file = "libtievm_std_native.dylib";
#else
    const char* file = "libtievm_std_native.so";
#endif
    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::current_path() / file,
        std::filesystem::current_path() / "build" / file,
        std::filesystem::current_path() / ".." / file,
    };
    for (const auto& path : candidates) {
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(
             std::filesystem::current_path(),
             std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file() && entry.path().filename() == file) {
            return entry.path();
        }
    }
    return {};
}

Module BuildDynamicFfiModule(
    const std::string& vm_symbol, const std::string& native_symbol, const AbiType& return_type,
    std::vector<AbiType> params) {
    Module module("ffi.dynamic");
    auto lib = FindNativeLib();
    EXPECT_FALSE(lib.empty());
    const uint32_t lib_idx = module.AddFfiLibraryPath(lib.string());

    FunctionSignature sig;
    sig.name = vm_symbol;
    sig.convention = CallingConvention::kSystem;
    sig.return_type = return_type;
    sig.params = params;
    const uint32_t sig_idx = module.AddFfiSignature(std::move(sig));

    FfiSymbolBinding binding;
    binding.vm_symbol = vm_symbol;
    binding.native_symbol = native_symbol;
    binding.library_index = lib_idx;
    binding.signature_index = sig_idx;
    const uint32_t binding_idx = module.AddFfiBinding(std::move(binding));

    const auto sym = module.AddConstant(Constant::Utf8(vm_symbol));
    auto& fn = module.AddFunction("entry", 8, static_cast<uint16_t>(params.size()));
    fn.ffi_binding().enabled = true;
    fn.ffi_binding().convention = CallingConvention::kSystem;
    fn.ffi_binding().signature_index = sig_idx;
    fn.ffi_binding().binding_index = binding_idx;
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder builder(bb);
    for (uint32_t i = 0; i < params.size(); ++i) {
        builder.Mov(2 + i, i);
    }
    builder.FfiCall(1, sym, static_cast<uint32_t>(params.size())).Ret(1);
    module.set_entry_function(0);
    return module;
}

}  // namespace

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

TEST(FfiTest, DynamicLibraryDirectAddCall) {
    VmInstance vm;
    Module module = BuildDynamicFfiModule(
        "demo.add",
        "tie_demo_add",
        {AbiValueKind::kI32, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
        {
            {AbiValueKind::kI32, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
            {AbiValueKind::kI32, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
        });
    auto result_or = vm.ExecuteModule(module, {Value::Int64(20), Value::Int64(22)});
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 42);
}

TEST(FfiTest, DynamicLibraryPointerInOutCall) {
    VmInstance vm;
    Module module = BuildDynamicFfiModule(
        "demo.inout",
        "tie_demo_inout_i64",
        {AbiValueKind::kVoid, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
        {
            {AbiValueKind::kPointer, OwnershipQualifier::kBorrowed, FfiPassingMode::kPointerInOut, 0, 8},
            {AbiValueKind::kI64, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
        });
    int64_t value = 41;
    auto result_or = vm.ExecuteModule(
        module,
        {Value::Pointer(reinterpret_cast<uint64_t>(&value)), Value::Int64(1)});
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(value, 42);
}

TEST(FfiTest, DynamicLibraryStructByValueCall) {
    struct Pair32 {
        int32_t a;
        int32_t b;
    };
    VmInstance vm;
    Module module = BuildDynamicFfiModule(
        "demo.struct.sum",
        "tie_demo_pair_sum",
        {AbiValueKind::kI32, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
        {{
            AbiValueKind::kStruct,
            OwnershipQualifier::kBorrowed,
            FfiPassingMode::kValue,
            0,
            8,
        }});

    Pair32 pair{40, 2};
    auto result_or = vm.ExecuteModule(module, {Value::Pointer(reinterpret_cast<uint64_t>(&pair))});
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 42);
}

TEST(FfiTest, ErrorStackContainsVmAndFfiFrames) {
    VmInstance vm;
    Module module = BuildDynamicFfiModule(
        "demo.bad",
        "tie_demo_symbol_missing",
        {AbiValueKind::kI32, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
        {
            {AbiValueKind::kI32, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
            {AbiValueKind::kI32, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
        });
    auto result_or = vm.ExecuteModule(module, {Value::Int64(1), Value::Int64(2)});
    ASSERT_FALSE(result_or.ok());
    ASSERT_TRUE(result_or.status().vm_error().has_value());
    const auto& frames = result_or.status().vm_error()->frames;
    ASSERT_FALSE(frames.empty());
    bool has_vm = false;
    bool has_ffi = false;
    for (const auto& frame : frames) {
        if (frame.kind == StackFrameKind::kVm) {
            has_vm = true;
        }
        if (frame.kind == StackFrameKind::kFfi) {
            has_ffi = true;
        }
    }
    EXPECT_TRUE(has_vm);
    EXPECT_TRUE(has_ffi);
}

}  // namespace tie::vm
