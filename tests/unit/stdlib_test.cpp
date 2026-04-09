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
    EXPECT_GE(modules.size(), 5u);

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
}

}  // namespace tie::vm
