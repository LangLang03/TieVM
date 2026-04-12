#include <gtest/gtest.h>

#include <algorithm>

#include "tie/vm/api.hpp"
#include "test_helpers.hpp"

namespace tie::vm {

namespace {

void LoadStdlibOrFail(VmInstance* vm) {
    auto bundle_or = BuildStdlibBundle();
    ASSERT_TRUE(bundle_or.ok()) << bundle_or.status().message();
    const auto dir = test::TempPath("stdlib_bundle.tlbs");
    ASSERT_TRUE(bundle_or.value().SerializeToDirectory(dir).ok());
    ASSERT_TRUE(vm->loader().LoadTlbsFile(dir).ok());
}

}  // namespace

TEST(StdlibTest, StringConcatAndLength) {
    VmInstance vm;
    LoadStdlibOrFail(&vm);
    Module module("stdlib.string.test");
    const auto hello = module.AddConstant(Constant::Utf8("hello "));
    const auto world = module.AddConstant(Constant::Utf8("world"));
    const auto concat = module.AddConstant(Constant::Utf8("tie.std.string.concat"));
    const auto length = module.AddConstant(Constant::Utf8("tie.std.string.length"));

    auto& fn = module.AddFunction("entry", 8, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(1, hello)
        .LoadK(2, world)
        .FfiCall(0, concat, 2)
        .Mov(1, 0)
        .FfiCall(0, length, 1)
        .Ret(0);
    module.set_entry_function(0);

    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 11);
}

TEST(StdlibTest, CollectionsArrayAndMap) {
    VmInstance vm;
    LoadStdlibOrFail(&vm);

    Module array_module("stdlib.array.test");
    const auto sym_array_new =
        array_module.AddConstant(Constant::Utf8("tie.std.collections.array_new"));
    const auto sym_array_push =
        array_module.AddConstant(Constant::Utf8("tie.std.collections.array_push"));
    const auto sym_array_size =
        array_module.AddConstant(Constant::Utf8("tie.std.collections.array_size"));
    const auto sym_array_free =
        array_module.AddConstant(Constant::Utf8("tie.std.collections.array_free"));
    const auto v42 = array_module.AddConstant(Constant::Int64(42));
    auto& array_fn = array_module.AddFunction("entry", 10, 0);
    auto& array_bb = array_fn.AddBlock("entry");
    InstructionBuilder(array_bb)
        .FfiCall(0, sym_array_new, 0)
        .Mov(4, 0)
        .Mov(1, 4)
        .LoadK(2, v42)
        .FfiCall(0, sym_array_push, 2)
        .Mov(1, 4)
        .FfiCall(0, sym_array_size, 1)
        .Mov(5, 0)
        .Mov(1, 4)
        .FfiCall(0, sym_array_free, 1)
        .Mov(0, 5)
        .Ret(0);
    array_module.set_entry_function(0);
    auto array_result_or = vm.ExecuteModule(array_module);
    ASSERT_TRUE(array_result_or.ok()) << array_result_or.status().message();
    EXPECT_EQ(array_result_or.value().AsInt64(), 1);

    Module map_module("stdlib.map.test");
    const auto sym_map_new =
        map_module.AddConstant(Constant::Utf8("tie.std.collections.map_new"));
    const auto sym_map_set =
        map_module.AddConstant(Constant::Utf8("tie.std.collections.map_set"));
    const auto sym_map_has =
        map_module.AddConstant(Constant::Utf8("tie.std.collections.map_has"));
    const auto sym_map_free =
        map_module.AddConstant(Constant::Utf8("tie.std.collections.map_free"));
    const auto key = map_module.AddConstant(Constant::Utf8("answer"));
    const auto value = map_module.AddConstant(Constant::Int64(42));
    auto& map_fn = map_module.AddFunction("entry", 12, 0);
    auto& map_bb = map_fn.AddBlock("entry");
    InstructionBuilder(map_bb)
        .FfiCall(0, sym_map_new, 0)
        .Mov(4, 0)
        .Mov(1, 4)
        .LoadK(2, key)
        .LoadK(3, value)
        .FfiCall(0, sym_map_set, 3)
        .Mov(1, 4)
        .LoadK(2, key)
        .FfiCall(0, sym_map_has, 2)
        .Mov(5, 0)
        .Mov(1, 4)
        .FfiCall(0, sym_map_free, 1)
        .Mov(0, 5)
        .Ret(0);
    map_module.set_entry_function(0);
    auto map_result_or = vm.ExecuteModule(map_module);
    ASSERT_TRUE(map_result_or.ok()) << map_result_or.status().message();
    EXPECT_TRUE(map_result_or.value().AsBool());
}

TEST(StdlibTest, BuildStdlibContainerContainsRealApis) {
    auto container_or = BuildStdlibContainer();
    ASSERT_TRUE(container_or.ok()) << container_or.status().message();
    const auto& modules = container_or.value().modules();
    EXPECT_GE(modules.size(), 7u);

    auto it = std::find_if(
        modules.begin(), modules.end(),
        [](const TlbModuleEntry& entry) { return entry.module_name == "tie.std.string"; });
    ASSERT_NE(it, modules.end());
    auto module_or = Serializer::Deserialize(it->bytecode);
    ASSERT_TRUE(module_or.ok()) << module_or.status().message();
    const auto& functions = module_or.value().functions();
    auto has_concat = std::any_of(
        functions.begin(), functions.end(),
        [](const Function& fn) { return fn.name() == "concat"; });
    EXPECT_TRUE(has_concat);

    auto math_it = std::find_if(
        modules.begin(), modules.end(),
        [](const TlbModuleEntry& entry) { return entry.module_name == "tie.std.math"; });
    ASSERT_NE(math_it, modules.end());
    auto math_module_or = Serializer::Deserialize(math_it->bytecode);
    ASSERT_TRUE(math_module_or.ok()) << math_module_or.status().message();
    const auto& math_functions = math_module_or.value().functions();
    auto has_gcd = std::any_of(
        math_functions.begin(), math_functions.end(),
        [](const Function& fn) { return fn.name() == "gcd"; });
    EXPECT_TRUE(has_gcd);
}

TEST(StdlibTest, StringUtilitiesAndMathAndTime) {
    VmInstance vm;
    LoadStdlibOrFail(&vm);

    Module string_module("stdlib.string.extra");
    const auto sym_replace = string_module.AddConstant(Constant::Utf8("tie.std.string.replace"));
    const auto sym_upper = string_module.AddConstant(Constant::Utf8("tie.std.string.upper_ascii"));
    const auto sym_find = string_module.AddConstant(Constant::Utf8("tie.std.string.find"));
    const auto text = string_module.AddConstant(Constant::Utf8("hello world"));
    const auto from = string_module.AddConstant(Constant::Utf8("world"));
    const auto to = string_module.AddConstant(Constant::Utf8("tie"));
    const auto needle = string_module.AddConstant(Constant::Utf8("TIE"));
    auto& sfn = string_module.AddFunction("entry", 12, 0);
    auto& sbb = sfn.AddBlock("entry");
    InstructionBuilder(sbb)
        .LoadK(1, text)
        .LoadK(2, from)
        .LoadK(3, to)
        .FfiCall(0, sym_replace, 3)
        .Mov(1, 0)
        .FfiCall(0, sym_upper, 1)
        .Mov(1, 0)
        .LoadK(2, needle)
        .FfiCall(0, sym_find, 2)
        .Ret(0);
    string_module.set_entry_function(0);
    auto string_result_or = vm.ExecuteModule(string_module);
    ASSERT_TRUE(string_result_or.ok()) << string_result_or.status().message();
    EXPECT_EQ(string_result_or.value().AsInt64(), 6);

    Module math_module("stdlib.math.extra");
    const auto sym_gcd = math_module.AddConstant(Constant::Utf8("tie.std.math.gcd"));
    const auto sym_clamp = math_module.AddConstant(Constant::Utf8("tie.std.math.clamp"));
    const auto sym_pow = math_module.AddConstant(Constant::Utf8("tie.std.math.pow_i"));
    const auto sym_abs = math_module.AddConstant(Constant::Utf8("tie.std.math.abs"));
    const auto c84 = math_module.AddConstant(Constant::Int64(84));
    const auto c30 = math_module.AddConstant(Constant::Int64(30));
    const auto c5 = math_module.AddConstant(Constant::Int64(5));
    const auto c10 = math_module.AddConstant(Constant::Int64(10));
    const auto c7 = math_module.AddConstant(Constant::Int64(7));
    const auto c2 = math_module.AddConstant(Constant::Int64(2));
    const auto c3 = math_module.AddConstant(Constant::Int64(3));
    const auto cneg3 = math_module.AddConstant(Constant::Int64(-3));
    auto& mfn = math_module.AddFunction("entry", 14, 0);
    auto& mbb = mfn.AddBlock("entry");
    InstructionBuilder(mbb)
        .LoadK(1, c84)
        .LoadK(2, c30)
        .FfiCall(0, sym_gcd, 2)
        .Mov(6, 0)
        .LoadK(1, c5)
        .LoadK(2, c10)
        .LoadK(3, c7)
        .FfiCall(0, sym_clamp, 3)
        .Add(6, 6, 0)
        .LoadK(1, c2)
        .LoadK(2, c3)
        .FfiCall(0, sym_pow, 2)
        .Add(6, 6, 0)
        .LoadK(1, cneg3)
        .FfiCall(0, sym_abs, 1)
        .Add(6, 6, 0)
        .Ret(6);
    math_module.set_entry_function(0);
    auto math_result_or = vm.ExecuteModule(math_module);
    ASSERT_TRUE(math_result_or.ok()) << math_result_or.status().message();
    EXPECT_EQ(math_result_or.value().AsInt64(), 24);

    Module time_module("stdlib.time.extra");
    const auto sym_now = time_module.AddConstant(Constant::Utf8("tie.std.time.now_ms"));
    auto& tfn = time_module.AddFunction("entry", 4, 0);
    auto& tbb = tfn.AddBlock("entry");
    InstructionBuilder(tbb).FfiCall(0, sym_now, 0).Ret(0);
    time_module.set_entry_function(0);
    auto time_result_or = vm.ExecuteModule(time_module);
    ASSERT_TRUE(time_result_or.ok()) << time_result_or.status().message();
    EXPECT_GT(time_result_or.value().AsInt64(), 0);
}

TEST(StdlibTest, CollectionsExtendedApis) {
    VmInstance vm;
    LoadStdlibOrFail(&vm);

    Module array_module("stdlib.array.extra");
    const auto sym_array_new =
        array_module.AddConstant(Constant::Utf8("tie.std.collections.array_new"));
    const auto sym_array_push =
        array_module.AddConstant(Constant::Utf8("tie.std.collections.array_push"));
    const auto sym_array_pop =
        array_module.AddConstant(Constant::Utf8("tie.std.collections.array_pop"));
    const auto sym_array_size =
        array_module.AddConstant(Constant::Utf8("tie.std.collections.array_size"));
    const auto sym_array_clear =
        array_module.AddConstant(Constant::Utf8("tie.std.collections.array_clear"));
    const auto sym_array_free =
        array_module.AddConstant(Constant::Utf8("tie.std.collections.array_free"));
    const auto c42 = array_module.AddConstant(Constant::Int64(42));
    const auto c7 = array_module.AddConstant(Constant::Int64(7));
    auto& afn = array_module.AddFunction("entry", 12, 0);
    auto& abb = afn.AddBlock("entry");
    InstructionBuilder(abb)
        .FfiCall(0, sym_array_new, 0)
        .Mov(4, 0)
        .Mov(1, 4)
        .LoadK(2, c42)
        .FfiCall(0, sym_array_push, 2)
        .Mov(1, 4)
        .LoadK(2, c7)
        .FfiCall(0, sym_array_push, 2)
        .Mov(1, 4)
        .FfiCall(0, sym_array_pop, 1)
        .Mov(5, 0)
        .Mov(1, 4)
        .FfiCall(0, sym_array_size, 1)
        .Add(5, 5, 0)
        .Mov(1, 4)
        .FfiCall(0, sym_array_clear, 1)
        .Mov(1, 4)
        .FfiCall(0, sym_array_size, 1)
        .Add(0, 5, 0)
        .Mov(6, 0)
        .Mov(1, 4)
        .FfiCall(0, sym_array_free, 1)
        .Mov(0, 6)
        .Ret(0);
    array_module.set_entry_function(0);
    auto array_result_or = vm.ExecuteModule(array_module);
    ASSERT_TRUE(array_result_or.ok()) << array_result_or.status().message();
    EXPECT_EQ(array_result_or.value().AsInt64(), 8);

    Module map_module("stdlib.map.extra");
    const auto sym_map_new =
        map_module.AddConstant(Constant::Utf8("tie.std.collections.map_new"));
    const auto sym_map_set =
        map_module.AddConstant(Constant::Utf8("tie.std.collections.map_set"));
    const auto sym_map_size =
        map_module.AddConstant(Constant::Utf8("tie.std.collections.map_size"));
    const auto sym_map_remove =
        map_module.AddConstant(Constant::Utf8("tie.std.collections.map_remove"));
    const auto sym_map_free =
        map_module.AddConstant(Constant::Utf8("tie.std.collections.map_free"));
    const auto key = map_module.AddConstant(Constant::Utf8("answer"));
    const auto value = map_module.AddConstant(Constant::Int64(42));
    auto& mfn = map_module.AddFunction("entry", 12, 0);
    auto& mbb = mfn.AddBlock("entry");
    InstructionBuilder(mbb)
        .FfiCall(0, sym_map_new, 0)
        .Mov(4, 0)
        .Mov(1, 4)
        .LoadK(2, key)
        .LoadK(3, value)
        .FfiCall(0, sym_map_set, 3)
        .Mov(1, 4)
        .FfiCall(0, sym_map_size, 1)
        .Mov(5, 0)
        .Mov(1, 4)
        .LoadK(2, key)
        .FfiCall(0, sym_map_remove, 2)
        .Mov(1, 4)
        .FfiCall(0, sym_map_size, 1)
        .Add(0, 5, 0)
        .Mov(6, 0)
        .Mov(1, 4)
        .FfiCall(0, sym_map_free, 1)
        .Mov(0, 6)
        .Ret(0);
    map_module.set_entry_function(0);
    auto map_result_or = vm.ExecuteModule(map_module);
    ASSERT_TRUE(map_result_or.ok()) << map_result_or.status().message();
    EXPECT_EQ(map_result_or.value().AsInt64(), 1);
}

}  // namespace tie::vm
