#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "test_helpers.hpp"
#include "tie/vm/api.hpp"
#include "tiebc_commands.hpp"

namespace tie::vm {

namespace {

Module BuildDebugModule(std::string name, int64_t value) {
    Module module(std::move(name));
    const auto c = module.AddConstant(Constant::Int64(value));
    auto& fn = module.AddFunction("entry", 4, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).LoadK(0, c).Mov(0, 0).Ret(0);
    module.AddDebugLine(DebugLineEntry{0, 0, 1, 1});
    module.set_entry_function(0);
    return module;
}

Module BuildConstFibModule(int64_t n) {
    Module module("opt.const_fib");
    const auto c_zero = module.AddConstant(Constant::Int64(0));
    const auto c_one = module.AddConstant(Constant::Int64(1));
    const auto c_n = module.AddConstant(Constant::Int64(n));
    auto& fn = module.AddFunction("entry", 8, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(1, c_zero)
        .LoadK(2, c_one)
        .LoadK(3, c_n)
        .JmpIfZero(3, 10)
        .LoadK(4, c_one)
        .CmpEq(5, 3, 4)
        .JmpIf(5, 6)
        .SubImm(3, 3, 1)
        .Add(4, 1, 2)
        .Mov(1, 2)
        .Mov(2, 4)
        .DecJnz(3, -3)
        .Ret(2)
        .Ret(1);
    module.set_entry_function(0);
    return module;
}

bool HasOpcode(const Function& function, OpCode opcode) {
    const auto code = function.FlattenedInstructions();
    for (const auto& inst : code) {
        if (inst.opcode == opcode) {
            return true;
        }
    }
    return false;
}

size_t CountOpcode(const Function& function, OpCode opcode) {
    size_t count = 0;
    const auto code = function.FlattenedInstructions();
    for (const auto& inst : code) {
        if (inst.opcode == opcode) {
            ++count;
        }
    }
    return count;
}

int RunOptCmdWithArgs(const std::vector<std::string>& args) {
    std::vector<std::string> owned = args;
    std::vector<char*> argv;
    argv.reserve(owned.size());
    for (auto& arg : owned) {
        argv.push_back(arg.data());
    }
    return cli::OptCmd(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST(OptimizerTest, O1LoopFusionKeepsBehavior) {
    Module module("opt.loop");
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

    BytecodeOptOptions options;
    options.level = BytecodeOptLevel::kO1;
    auto optimized_or = OptimizeBytecodeModule(module, options);
    ASSERT_TRUE(optimized_or.ok()) << optimized_or.status().message();

    const auto& optimized = optimized_or.value().module;
    EXPECT_TRUE(HasOpcode(optimized.functions()[0], OpCode::kAddDecJnz));
    EXPECT_FALSE(HasOpcode(optimized.functions()[0], OpCode::kDecJnz));

    VmInstance vm;
    auto result_or = vm.ExecuteModule(optimized);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 5050);
}

TEST(OptimizerTest, O1TailcallRewritesCallRetPair) {
    Module module("opt.tailcall");
    const auto c_one = module.AddConstant(Constant::Int64(1));
    const auto c_41 = module.AddConstant(Constant::Int64(41));

    auto& plus1 = module.AddFunction("plus1", 2, 1);
    auto& plus1_bb = plus1.AddBlock("entry");
    InstructionBuilder(plus1_bb).LoadK(1, c_one).Add(0, 0, 1).Ret(0);

    auto& entry = module.AddFunction("entry", 4, 0);
    auto& entry_bb = entry.AddBlock("entry");
    InstructionBuilder(entry_bb).LoadK(1, c_41).Call(0, 0, 1).Ret(0);
    module.set_entry_function(1);

    BytecodeOptOptions options;
    options.level = BytecodeOptLevel::kO1;
    auto optimized_or = OptimizeBytecodeModule(module, options);
    ASSERT_TRUE(optimized_or.ok()) << optimized_or.status().message();

    const auto& code = optimized_or.value().module.functions()[1].FlattenedInstructions();
    ASSERT_FALSE(code.empty());
    EXPECT_EQ(code.back().opcode, OpCode::kTailCall);
}

TEST(OptimizerTest, O2ConstFoldAndDceEliminateDeadPath) {
    Module module("opt.const_dce");
    const auto c2 = module.AddConstant(Constant::Int64(2));
    const auto c3 = module.AddConstant(Constant::Int64(3));
    const auto c999 = module.AddConstant(Constant::Int64(999));
    auto& fn = module.AddFunction("entry", 4, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(0, c2)
        .LoadK(1, c3)
        .Add(0, 0, 1)
        .JmpIfNotZero(0, 2)
        .LoadK(1, c999)
        .Ret(0);
    module.set_entry_function(0);

    BytecodeOptOptions options;
    options.level = BytecodeOptLevel::kO2;
    auto optimized_or = OptimizeBytecodeModule(module, options);
    ASSERT_TRUE(optimized_or.ok()) << optimized_or.status().message();

    const auto& code = optimized_or.value().module.functions()[0].FlattenedInstructions();
    EXPECT_FALSE(HasOpcode(optimized_or.value().module.functions()[0], OpCode::kAdd));

    VmInstance vm;
    auto result_or = vm.ExecuteModule(optimized_or.value().module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 5);

    for (const auto& inst : code) {
        EXPECT_FALSE(inst.opcode == OpCode::kLoadK && inst.b == c999);
    }
}

TEST(OptimizerTest, O3ConstExecFoldsPureLoopToConstantReturn) {
    Module module = BuildConstFibModule(20);

    BytecodeOptOptions o2_options;
    o2_options.level = BytecodeOptLevel::kO2;
    auto o2_or = OptimizeBytecodeModule(module, o2_options);
    ASSERT_TRUE(o2_or.ok()) << o2_or.status().message();
    EXPECT_TRUE(HasOpcode(o2_or.value().module.functions()[0], OpCode::kDecJnz));

    BytecodeOptOptions o3_options;
    o3_options.level = BytecodeOptLevel::kO3;
    auto o3_or = OptimizeBytecodeModule(module, o3_options);
    ASSERT_TRUE(o3_or.ok()) << o3_or.status().message();

    const auto& code = o3_or.value().module.functions()[0].FlattenedInstructions();
    ASSERT_EQ(code.size(), 2u);
    EXPECT_EQ(code[0].opcode, OpCode::kLoadK);
    EXPECT_EQ(code[1].opcode, OpCode::kRet);

    const auto const_idx = code[0].b;
    ASSERT_LT(const_idx, o3_or.value().module.constants().size());
    EXPECT_EQ(o3_or.value().module.constants()[const_idx].type, ConstantType::kInt64);
    EXPECT_EQ(o3_or.value().module.constants()[const_idx].int64_value, 6765);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(o3_or.value().module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 6765);
}

TEST(OptimizerTest, O3BypassesFfiForwarderCalls) {
    Module module("opt.ffi.forwarders");
    const auto sym_read = module.AddConstant(Constant::Utf8("tie.std.io.read_i64"));
    const auto sym_abs = module.AddConstant(Constant::Utf8("tie.std.math.abs"));
    const auto c_10 = module.AddConstant(Constant::Int64(10));

    auto& read_fn = module.AddFunction("read_i64", 4, 0);
    auto& read_bb = read_fn.AddBlock("entry");
    InstructionBuilder(read_bb).FfiCall(0, sym_read, 0).Ret(0);

    auto& abs_fn = module.AddFunction("abs_i64", 4, 1);
    auto& abs_bb = abs_fn.AddBlock("entry");
    InstructionBuilder(abs_bb).Mov(1, 0).FfiCall(0, sym_abs, 1).Ret(0);

    auto& entry = module.AddFunction("entry", 8, 0);
    auto& entry_bb = entry.AddBlock("entry");
    InstructionBuilder(entry_bb)
        .LoadK(1, c_10)
        .Call(0, 1, 1)
        .Call(3, 0, 0)
        .Add(0, 0, 3)
        .Ret(0);
    module.set_entry_function(2);

    BytecodeOptOptions o2_options;
    o2_options.level = BytecodeOptLevel::kO2;
    auto o2_or = OptimizeBytecodeModule(module, o2_options);
    ASSERT_TRUE(o2_or.ok()) << o2_or.status().message();
    EXPECT_EQ(CountOpcode(o2_or.value().module.functions()[2], OpCode::kCall), 2u);

    BytecodeOptOptions o3_options;
    o3_options.level = BytecodeOptLevel::kO3;
    auto o3_or = OptimizeBytecodeModule(module, o3_options);
    ASSERT_TRUE(o3_or.ok()) << o3_or.status().message();

    const auto& entry_fn = o3_or.value().module.functions()[2];
    EXPECT_EQ(CountOpcode(entry_fn, OpCode::kCall), 0u);
    EXPECT_EQ(CountOpcode(entry_fn, OpCode::kFfiCall), 2u);
}

TEST(OptimizerTest, O3InlineSmallAllowsExportedCaller) {
    Module module("opt.inline.exported_caller");
    const auto c_40 = module.AddConstant(Constant::Int64(40));

    auto& callee = module.AddFunction("inc", 1, 1);
    auto& callee_bb = callee.AddBlock("entry");
    InstructionBuilder(callee_bb).AddImm(0, 0, 1).Ret(0);

    auto& exported = module.AddFunction("exported_wrap", 4, 1, 0, false, true);
    auto& exported_bb = exported.AddBlock("entry");
    InstructionBuilder(exported_bb).Mov(1, 0).Call(0, 0, 1).AddImm(0, 0, 1).Ret(0);

    auto& entry = module.AddFunction("entry", 4, 0);
    auto& entry_bb = entry.AddBlock("entry");
    InstructionBuilder(entry_bb).LoadK(1, c_40).Call(0, 1, 1).Ret(0);
    module.set_entry_function(2);

    BytecodeOptOptions options;
    options.level = BytecodeOptLevel::kO3;
    auto optimized_or = OptimizeBytecodeModule(module, options);
    ASSERT_TRUE(optimized_or.ok()) << optimized_or.status().message();

    const auto& exported_code = optimized_or.value().module.functions()[1].FlattenedInstructions();
    bool has_call = false;
    for (const auto& inst : exported_code) {
        if (inst.opcode == OpCode::kCall) {
            has_call = true;
        }
    }
    EXPECT_FALSE(has_call);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(optimized_or.value().module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 42);
}

TEST(OptimizerTest, O3DoesNotInlineExportedCallee) {
    Module module("opt.inline.exported_callee");
    const auto c_40 = module.AddConstant(Constant::Int64(40));

    auto& exported_callee = module.AddFunction("inc_exported", 1, 1, 0, false, true);
    auto& callee_bb = exported_callee.AddBlock("entry");
    InstructionBuilder(callee_bb).AddImm(0, 0, 1).Ret(0);

    auto& caller = module.AddFunction("caller", 4, 1);
    auto& caller_bb = caller.AddBlock("entry");
    InstructionBuilder(caller_bb).Mov(1, 0).Call(0, 0, 1).AddImm(0, 0, 1).Ret(0);

    auto& entry = module.AddFunction("entry", 4, 0);
    auto& entry_bb = entry.AddBlock("entry");
    InstructionBuilder(entry_bb).LoadK(1, c_40).Call(0, 1, 1).Ret(0);
    module.set_entry_function(2);

    BytecodeOptOptions options;
    options.level = BytecodeOptLevel::kO3;
    auto optimized_or = OptimizeBytecodeModule(module, options);
    ASSERT_TRUE(optimized_or.ok()) << optimized_or.status().message();

    EXPECT_TRUE(HasOpcode(optimized_or.value().module.functions()[1], OpCode::kCall));
}

TEST(OptimizerTest, PassEnableDisableAffectsPipelineAndClearsDebugLines) {
    Module module = BuildDebugModule("opt.pipeline", 7);

    BytecodeOptOptions options;
    options.level = BytecodeOptLevel::kO1;
    options.disable_passes = {BytecodeOptPass::kLoopFusion};
    options.enable_passes = {BytecodeOptPass::kConstFold};

    auto optimized_or = OptimizeBytecodeModule(module, options);
    ASSERT_TRUE(optimized_or.ok()) << optimized_or.status().message();

    const auto& stats = optimized_or.value().stats;
    EXPECT_TRUE(
        std::find(stats.executed_passes.begin(), stats.executed_passes.end(), BytecodeOptPass::kConstFold) !=
        stats.executed_passes.end());
    EXPECT_TRUE(
        std::find(stats.executed_passes.begin(), stats.executed_passes.end(), BytecodeOptPass::kLoopFusion) ==
        stats.executed_passes.end());
    EXPECT_TRUE(optimized_or.value().module.debug_lines().empty());
}

TEST(OptimizerCliTest, TlbDefaultOptimizesAllModules) {
    Module first = BuildDebugModule("pkg.first", 1);
    Module second = BuildDebugModule("pkg.second", 2);

    auto first_bytes_or = Serializer::Serialize(first, true);
    ASSERT_TRUE(first_bytes_or.ok()) << first_bytes_or.status().message();
    auto second_bytes_or = Serializer::Serialize(second, true);
    ASSERT_TRUE(second_bytes_or.ok()) << second_bytes_or.status().message();

    TlbContainer container;
    container.AddModule(TlbModuleEntry{"pkg.first", SemanticVersion{0, 1, 0}, first_bytes_or.value(), {}});
    container.AddModule(TlbModuleEntry{"pkg.second", SemanticVersion{0, 1, 0}, second_bytes_or.value(), {}});

    const auto input = test::TempPath("opt_all.tlb");
    const auto output = test::TempPath("opt_all_out.tlb");
    ASSERT_TRUE(container.SerializeToFile(input).ok());

    const int rc = RunOptCmdWithArgs({
        "tiebc",
        "opt",
        input.string(),
        "-o",
        output.string(),
    });
    ASSERT_EQ(rc, 0);

    auto out_or = TlbContainer::DeserializeFromFile(output);
    ASSERT_TRUE(out_or.ok()) << out_or.status().message();
    ASSERT_EQ(out_or.value().modules().size(), 2u);

    for (const auto& entry : out_or.value().modules()) {
        auto module_or = Serializer::Deserialize(entry.bytecode);
        ASSERT_TRUE(module_or.ok()) << module_or.status().message();
        EXPECT_TRUE(module_or.value().debug_lines().empty());
    }
}

TEST(OptimizerCliTest, TlbsModuleFilterOptimizesOnlyTargetModule) {
    Module first = BuildDebugModule("pkg.first", 1);
    Module second = BuildDebugModule("pkg.second", 2);

    auto first_bytes_or = Serializer::Serialize(first, true);
    ASSERT_TRUE(first_bytes_or.ok()) << first_bytes_or.status().message();
    auto second_bytes_or = Serializer::Serialize(second, true);
    ASSERT_TRUE(second_bytes_or.ok()) << second_bytes_or.status().message();

    TlbsBundle bundle;
    bundle.manifest().name = "opt.bundle";
    bundle.manifest().modules = {"modules/first.tbc", "modules/second.tbc"};
    bundle.SetModule("modules/first.tbc", first_bytes_or.value());
    bundle.SetModule("modules/second.tbc", second_bytes_or.value());

    const auto input = test::TempPath("opt_input.tlbs");
    const auto output = test::TempPath("opt_output.tlbs");
    ASSERT_TRUE(bundle.SerializeToDirectory(input).ok());

    const int rc = RunOptCmdWithArgs({
        "tiebc",
        "opt",
        input.string(),
        "-o",
        output.string(),
        "--module",
        "modules/second.tbc",
    });
    ASSERT_EQ(rc, 0);

    auto out_or = TlbsBundle::Deserialize(output);
    ASSERT_TRUE(out_or.ok()) << out_or.status().message();

    auto first_module_or = Serializer::Deserialize(out_or.value().modules().at("modules/first.tbc"));
    ASSERT_TRUE(first_module_or.ok()) << first_module_or.status().message();
    EXPECT_FALSE(first_module_or.value().debug_lines().empty());

    auto second_module_or = Serializer::Deserialize(out_or.value().modules().at("modules/second.tbc"));
    ASSERT_TRUE(second_module_or.ok()) << second_module_or.status().message();
    EXPECT_TRUE(second_module_or.value().debug_lines().empty());
}

}  // namespace tie::vm
