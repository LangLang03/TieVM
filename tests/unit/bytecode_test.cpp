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

TEST(BytecodeTest, VerifierRejectsInvalidLoadKConstantIndex) {
    Module module("invalid.loadk");
    auto& fn = module.AddFunction("entry", 2, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).LoadK(0, 7).Ret(0);
    module.set_entry_function(0);
    const auto result = Verifier::Verify(module);
    EXPECT_FALSE(result.status.ok());
}

TEST(BytecodeTest, VerifierRejectsInvalidCallWindowAndArgCount) {
    Module module("invalid.call");
    auto& callee = module.AddFunction("callee", 2, 1);
    auto& callee_bb = callee.AddBlock("entry");
    InstructionBuilder(callee_bb).Ret(0);

    auto& entry = module.AddFunction("entry", 2, 0);
    auto& entry_bb = entry.AddBlock("entry");
    InstructionBuilder(entry_bb).Call(1, 0, 2).Ret(1);
    module.set_entry_function(1);

    const auto result = Verifier::Verify(module);
    EXPECT_FALSE(result.status.ok());
}

TEST(BytecodeTest, DeserializeRejectsTrailingGarbageBytes) {
    Module module = test::BuildAddModule(1, 2);
    auto bytes_or = Serializer::Serialize(module, false);
    ASSERT_TRUE(bytes_or.ok()) << bytes_or.status().message();
    auto bytes = std::move(bytes_or.value());
    bytes.push_back(0xFF);

    auto parsed_or = Serializer::Deserialize(bytes);
    EXPECT_FALSE(parsed_or.ok());
}

TEST(BytecodeTest, DeserializeCanSkipVerifierForTrustedInput) {
    Module module = test::BuildAddModule(1, 2);
    auto bytes_or = Serializer::Serialize(module, false);
    ASSERT_TRUE(bytes_or.ok()) << bytes_or.status().message();
    auto bytes = std::move(bytes_or.value());

    size_t offset = 0;
    auto read_u32 = [&](size_t pos) -> uint32_t {
        return static_cast<uint32_t>(bytes[pos]) |
               (static_cast<uint32_t>(bytes[pos + 1]) << 8u) |
               (static_cast<uint32_t>(bytes[pos + 2]) << 16u) |
               (static_cast<uint32_t>(bytes[pos + 3]) << 24u);
    };

    offset += 4;   // magic
    offset += 2;   // major
    offset += 2;   // minor
    offset += 4;   // flags

    const uint32_t module_name_len = read_u32(offset);
    offset += 4 + module_name_len;
    offset += 4;  // version major
    offset += 4;  // version minor
    offset += 4;  // version patch
    offset += 4;  // entry function

    const uint32_t const_count = read_u32(offset);
    offset += 4;
    const uint32_t func_count = read_u32(offset);
    offset += 4;
    ASSERT_EQ(func_count, 1u);

    for (uint32_t i = 0; i < const_count; ++i) {
        ASSERT_LT(offset, bytes.size());
        const uint8_t type = bytes[offset++];
        if (type == static_cast<uint8_t>(ConstantType::kInt64) ||
            type == static_cast<uint8_t>(ConstantType::kFloat64)) {
            offset += 8;
            continue;
        }
        if (type == static_cast<uint8_t>(ConstantType::kUtf8)) {
            const uint32_t len = read_u32(offset);
            offset += 4 + len;
            continue;
        }
        FAIL() << "unexpected constant type while mutating serialized bytes";
    }

    const uint32_t fn_name_len = read_u32(offset);
    offset += 4 + fn_name_len;
    ASSERT_LE(offset + 2, bytes.size());
    // Corrupt reg_count to zero so strict verification rejects it.
    bytes[offset] = 0;
    bytes[offset + 1] = 0;

    DeserializeOptions trusted_options;
    trusted_options.verify = false;
    auto trusted_or = Serializer::Deserialize(bytes, trusted_options);
    EXPECT_TRUE(trusted_or.ok()) << trusted_or.status().message();

    auto strict_or = Serializer::Deserialize(bytes);
    EXPECT_FALSE(strict_or.ok());
}

TEST(BytecodeTest, FastLoopDecJnzExecutesCorrectly) {
    Module module("fast.decjnz");
    const auto c_zero = module.AddConstant(Constant::Int64(0));
    const auto c_n = module.AddConstant(Constant::Int64(100));
    auto& fn = module.AddFunction("entry", 4, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(0, c_zero)
        .LoadK(1, c_n)
        .Add(0, 0, 1)
        .DecJnz(1, -1)
        .Ret(0);
    module.set_entry_function(0);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 5050);
}

TEST(BytecodeTest, FastLoopAddDecJnzExecutesCorrectly) {
    Module module("fast.add_decjnz");
    const auto c_zero = module.AddConstant(Constant::Int64(0));
    const auto c_n = module.AddConstant(Constant::Int64(100));
    auto& fn = module.AddFunction("entry", 4, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(0, c_zero)
        .LoadK(1, c_n)
        .AddDecJnz(0, 1, 0)
        .Ret(0);
    module.set_entry_function(0);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 5050);
}

TEST(BytecodeTest, ImmArithmeticAndZeroBranchesWork) {
    Module module("fast.imm");
    const auto c_ten = module.AddConstant(Constant::Int64(10));
    const auto c_bad = module.AddConstant(Constant::Int64(999));
    auto& fn = module.AddFunction("entry", 4, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(0, c_ten)
        .AddImm(0, 0, 5)
        .SubImm(0, 0, 2)
        .JmpIfZero(0, 2)
        .JmpIfNotZero(0, 2)
        .LoadK(0, c_bad)
        .Ret(0);
    module.set_entry_function(0);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 13);
}

TEST(BytecodeTest, IncDecAndSubImmJnzWork) {
    Module module("fast.inc_dec_sub_imm_jnz");
    const auto c_zero = module.AddConstant(Constant::Int64(0));
    const auto c_five = module.AddConstant(Constant::Int64(5));
    auto& fn = module.AddFunction("entry", 4, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(0, c_five)
        .LoadK(1, c_zero)
        .Inc(1)
        .SubImmJnz(0, 1, -1)
        .Dec(1)
        .Inc(1)
        .Ret(1);
    module.set_entry_function(0);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 5);
}

TEST(BytecodeTest, AddImmJnzWorks) {
    Module module("fast.add_imm_jnz");
    const auto c_zero = module.AddConstant(Constant::Int64(0));
    const auto c_neg_five = module.AddConstant(Constant::Int64(-5));
    auto& fn = module.AddFunction("entry", 4, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(0, c_neg_five)
        .LoadK(1, c_zero)
        .Inc(1)
        .AddImmJnz(0, 1, -1)
        .Ret(1);
    module.set_entry_function(0);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 5);
}

TEST(BytecodeTest, ClosureUpvalueAndCallClosureWork) {
    Module module("closure.upvalue");
    const auto c_40 = module.AddConstant(Constant::Int64(40));
    const auto c_2 = module.AddConstant(Constant::Int64(2));

    auto& adder = module.AddFunction("adder", 4, 1, 1, false);
    auto& adder_bb = adder.AddBlock("entry");
    InstructionBuilder(adder_bb).GetUpval(1, 0).Add(0, 0, 1).Ret(0);

    auto& entry = module.AddFunction("entry", 8, 0);
    auto& entry_bb = entry.AddBlock("entry");
    InstructionBuilder(entry_bb)
        .LoadK(1, c_40)
        .Closure(0, 0, 1, 1)
        .LoadK(2, c_2)
        .CallClosure(1, 0, 1)
        .Ret(1);
    module.set_entry_function(1);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 42);
}

TEST(BytecodeTest, VarArgAndTailCallWork) {
    Module module("call.protocol");
    const auto c_1 = module.AddConstant(Constant::Int64(1));
    const auto c_20 = module.AddConstant(Constant::Int64(20));
    const auto c_22 = module.AddConstant(Constant::Int64(22));
    const auto c_41 = module.AddConstant(Constant::Int64(41));

    auto& plus_one = module.AddFunction("plus_one", 4, 1);
    auto& plus_one_bb = plus_one.AddBlock("entry");
    InstructionBuilder(plus_one_bb).LoadK(1, c_1).Add(0, 0, 1).Ret(0);

    auto& wrapper = module.AddFunction("wrapper", 4, 1);
    auto& wrapper_bb = wrapper.AddBlock("entry");
    InstructionBuilder(wrapper_bb).Mov(1, 0).TailCall(0, 0, 1);

    auto& var_sum = module.AddFunction("var_sum", 8, 0, 0, true);
    auto& var_sum_bb = var_sum.AddBlock("entry");
    InstructionBuilder(var_sum_bb).VarArg(0, 0, 1).VarArg(1, 1, 1).Add(0, 0, 1).Ret(0);

    auto& entry = module.AddFunction("entry", 10, 0);
    auto& entry_bb = entry.AddBlock("entry");
    InstructionBuilder(entry_bb)
        .LoadK(1, c_41)
        .Call(0, 1, 1)
        .LoadK(4, c_20)
        .LoadK(5, c_22)
        .Call(3, 2, 2)
        .Add(0, 0, 3)
        .Ret(0);
    module.set_entry_function(3);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 84);
}

TEST(BytecodeTest, StringAndBitFastInstructionsWork) {
    Module module("fast.string.bit");
    const auto c_hello = module.AddConstant(Constant::Utf8("hello "));
    const auto c_world = module.AddConstant(Constant::Utf8("world"));
    const auto c_6 = module.AddConstant(Constant::Int64(6));
    const auto c_3 = module.AddConstant(Constant::Int64(3));
    const auto c_8 = module.AddConstant(Constant::Int64(8));
    const auto c_1 = module.AddConstant(Constant::Int64(1));

    auto& fn = module.AddFunction("entry", 16, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(1, c_hello)
        .LoadK(2, c_world)
        .StrConcat(0, 1, 2)
        .StrLen(3, 0)
        .LoadK(4, c_6)
        .LoadK(5, c_3)
        .BitAnd(6, 4, 5)
        .LoadK(7, c_8)
        .LoadK(8, c_1)
        .BitXor(9, 7, 8)
        .BitOr(10, 6, 9)
        .BitNot(10, 10)
        .BitShl(10, 10, 8)
        .BitShr(10, 10, 8)
        .Add(0, 3, 10)
        .Ret(0);
    module.set_entry_function(0);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), -1);
}

TEST(BytecodeTest, SerializeDeserializeClosureVarArgHeadersRoundTrip) {
    Module module("closure.vararg.header");
    auto& captured = module.AddFunction("captured", 4, 1, 2, false);
    auto& captured_bb = captured.AddBlock("entry");
    InstructionBuilder(captured_bb).Ret(0);
    auto& variadic = module.AddFunction("variadic", 6, 1, 0, true);
    auto& variadic_bb = variadic.AddBlock("entry");
    InstructionBuilder(variadic_bb).Ret(0);
    module.set_entry_function(0);

    auto bytes_or = Serializer::Serialize(module, false);
    ASSERT_TRUE(bytes_or.ok()) << bytes_or.status().message();
    auto parsed_or = Serializer::Deserialize(bytes_or.value());
    ASSERT_TRUE(parsed_or.ok()) << parsed_or.status().message();

    ASSERT_EQ(parsed_or.value().functions().size(), 2u);
    EXPECT_EQ(parsed_or.value().functions()[0].upvalue_count(), 2u);
    EXPECT_FALSE(parsed_or.value().functions()[0].is_vararg());
    EXPECT_EQ(parsed_or.value().functions()[1].upvalue_count(), 0u);
    EXPECT_TRUE(parsed_or.value().functions()[1].is_vararg());
}

TEST(BytecodeTest, SerializeDeserializeClassMetadataRoundTrip) {
    Module module("class.meta");
    auto& plus1_fn = module.AddFunction("plus1_impl", 4, 2);
    auto& plus1_bb = plus1_fn.AddBlock("entry");
    InstructionBuilder(plus1_bb).AddImm(0, 1, 1).Ret(0);

    BytecodeClassDecl demo_class;
    demo_class.name = "DemoMath";
    demo_class.methods.push_back(BytecodeMethodDecl{
        "plus1",
        0,
        BytecodeAccessModifier::kPublic,
        true,
    });
    module.AddClass(std::move(demo_class));
    module.set_entry_function(0);

    auto bytes_or = Serializer::Serialize(module, false);
    ASSERT_TRUE(bytes_or.ok()) << bytes_or.status().message();
    auto parsed_or = Serializer::Deserialize(bytes_or.value());
    ASSERT_TRUE(parsed_or.ok()) << parsed_or.status().message();
    ASSERT_EQ(parsed_or.value().classes().size(), 1u);
    EXPECT_EQ(parsed_or.value().classes()[0].name, "DemoMath");
    ASSERT_EQ(parsed_or.value().classes()[0].methods.size(), 1u);
    EXPECT_EQ(parsed_or.value().classes()[0].methods[0].name, "plus1");
    EXPECT_EQ(parsed_or.value().classes()[0].methods[0].function_index, 0u);
}

TEST(BytecodeTest, VerifierRejectsInvalidClassAccessModifier) {
    Module module("class.meta.invalid.access");
    auto& fn = module.AddFunction("entry", 2, 1);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).Ret(0);
    module.set_entry_function(0);

    BytecodeClassDecl klass;
    klass.name = "BrokenClass";
    klass.methods.push_back(BytecodeMethodDecl{
        "run",
        0,
        static_cast<BytecodeAccessModifier>(255),
        true,
    });
    module.AddClass(std::move(klass));

    auto bytes_or = Serializer::Serialize(module, false);
    EXPECT_FALSE(bytes_or.ok());
}

TEST(BytecodeTest, RuntimeValidationRejectsInvalidRegisterAccess) {
    Module module("runtime.invalid.reg");
    auto& fn = module.AddFunction("entry", 1, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).Add(0, 0, 2).Ret(0);
    module.set_entry_function(0);

    VmInstance vm;
    vm.SetRuntimeValidationEnabled(true);
    auto result_or = vm.ExecuteModule(module);
    EXPECT_FALSE(result_or.ok());
}

TEST(BytecodeTest, TryCatchFinallyHandlesThrowAndExecutesFinally) {
    Module module("try.catch.finally");
    const auto c_zero = module.AddConstant(Constant::Int64(0));
    const auto c_one = module.AddConstant(Constant::Int64(1));
    auto& fn = module.AddFunction("entry", 8, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(1, c_one)
        .LoadK(2, c_zero)
        .TryBegin(6, 8, 11)
        .Div(3, 1, 2)
        .LoadK(0, c_zero)
        .TryEnd()
        .LoadK(0, c_one)
        .EndCatch()
        .LoadK(4, c_one)
        .Add(0, 0, 4)
        .EndFinally()
        .Ret(0);
    module.set_entry_function(0);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 2);
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
