#include <filesystem>
#include <system_error>

#include <gtest/gtest.h>

#include "test_helpers.hpp"
#include "tie/vm/api.hpp"
#include "tiebc_commands.hpp"

namespace tie::vm {

TEST(TiebcEmitTest, EmitClosureUpvalueBytecodeAndExecute) {
    const auto path = test::TempPath("emit_closure_upvalue.tbc");
    ASSERT_EQ(cli::EmitClosureUpvalue(path), 0);

    auto module_or = Serializer::DeserializeFromFile(path);
    ASSERT_TRUE(module_or.ok()) << module_or.status().message();
    const auto& module = module_or.value();
    ASSERT_EQ(module.functions().size(), 2u);
    ASSERT_EQ(module.entry_function(), 1u);
    EXPECT_EQ(module.functions()[0].upvalue_count(), 1u);

    const auto closure_code = module.functions()[0].FlattenedInstructions();
    bool has_get_upval = false;
    bool has_set_upval = false;
    for (const auto& inst : closure_code) {
        if (inst.opcode == OpCode::kGetUpval) {
            has_get_upval = true;
        } else if (inst.opcode == OpCode::kSetUpval) {
            has_set_upval = true;
        }
    }
    EXPECT_TRUE(has_get_upval);
    EXPECT_TRUE(has_set_upval);

    const auto entry_code = module.functions()[1].FlattenedInstructions();
    bool has_closure = false;
    bool has_call_closure = false;
    for (const auto& inst : entry_code) {
        if (inst.opcode == OpCode::kClosure) {
            has_closure = true;
        } else if (inst.opcode == OpCode::kCallClosure) {
            has_call_closure = true;
        }
    }
    EXPECT_TRUE(has_closure);
    EXPECT_TRUE(has_call_closure);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 42);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(TiebcEmitTest, EmitErrorHandlingBytecodeAndExecute) {
    const auto path = test::TempPath("emit_error_handling.tbc");
    ASSERT_EQ(cli::EmitErrorHandling(path), 0);

    auto module_or = Serializer::DeserializeFromFile(path);
    ASSERT_TRUE(module_or.ok()) << module_or.status().message();
    const auto& module = module_or.value();
    ASSERT_EQ(module.functions().size(), 1u);
    const auto code = module.functions()[0].FlattenedInstructions();

    bool has_try_begin = false;
    bool has_try_end = false;
    bool has_end_catch = false;
    bool has_end_finally = false;
    for (const auto& inst : code) {
        if (inst.opcode == OpCode::kTryBegin) {
            has_try_begin = true;
        } else if (inst.opcode == OpCode::kTryEnd) {
            has_try_end = true;
        } else if (inst.opcode == OpCode::kEndCatch) {
            has_end_catch = true;
        } else if (inst.opcode == OpCode::kEndFinally) {
            has_end_finally = true;
        }
    }
    EXPECT_TRUE(has_try_begin);
    EXPECT_TRUE(has_try_end);
    EXPECT_TRUE(has_end_catch);
    EXPECT_TRUE(has_end_finally);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 2);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(TiebcEmitTest, EmitFibBytecodeAndExecute) {
    const auto path = test::TempPath("emit_fib.tbc");
    ASSERT_EQ(cli::EmitFib(path, 20), 0);

    auto module_or = Serializer::DeserializeFromFile(path);
    ASSERT_TRUE(module_or.ok()) << module_or.status().message();
    const auto& module = module_or.value();
    ASSERT_EQ(module.functions().size(), 1u);
    const auto code = module.functions()[0].FlattenedInstructions();

    bool has_dec_jnz = false;
    bool has_add = false;
    bool has_mov = false;
    for (const auto& inst : code) {
        if (inst.opcode == OpCode::kDecJnz) {
            has_dec_jnz = true;
        } else if (inst.opcode == OpCode::kAdd) {
            has_add = true;
        } else if (inst.opcode == OpCode::kMov) {
            has_mov = true;
        }
    }
    EXPECT_TRUE(has_dec_jnz);
    EXPECT_TRUE(has_add);
    EXPECT_TRUE(has_mov);

    VmInstance vm;
    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 6765);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace tie::vm
