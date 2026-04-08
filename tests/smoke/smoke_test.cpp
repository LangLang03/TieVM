#include <atomic>
#include <filesystem>

#include <gtest/gtest.h>

#include "test_helpers.hpp"

namespace tie::vm {

TEST(Smoke, BytecodeLoadExecute) {
    Module module = test::BuildAddModule(40, 2);
    const auto path = test::TempPath("smoke.tbc");
    ASSERT_TRUE(Serializer::SerializeToFile(module, path).ok());
    auto parsed_or = Serializer::DeserializeFromFile(path);
    ASSERT_TRUE(parsed_or.ok()) << parsed_or.status().message();
    VmInstance vm;
    auto result_or = vm.ExecuteModule(parsed_or.value());
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 42);
}

TEST(Smoke, OopMultiInheritanceVirtualInvoke) {
    VmInstance vm;
    ClassDescriptor a;
    a.name = "A";
    ASSERT_TRUE(vm.reflection().RegisterClass(std::move(a)).ok());

    ClassDescriptor b;
    b.name = "B";
    b.base_classes = {"A"};
    b.methods["who"] = MethodDescriptor{
        "who",
        AccessModifier::kPublic,
        true,
        [](ObjectId, const std::vector<Value>&) { return StatusOr<Value>(Value::Int64(9)); }};
    ASSERT_TRUE(vm.reflection().RegisterClass(std::move(b)).ok());

    ClassDescriptor c;
    c.name = "C";
    c.base_classes = {"A"};
    ASSERT_TRUE(vm.reflection().RegisterClass(std::move(c)).ok());

    ClassDescriptor d;
    d.name = "D";
    d.base_classes = {"B", "C"};
    ASSERT_TRUE(vm.reflection().RegisterClass(std::move(d)).ok());

    Module module("smoke.oop");
    const auto class_name = module.AddConstant(Constant::Utf8("D"));
    const auto method_name = module.AddConstant(Constant::Utf8("who"));
    auto& fn = module.AddFunction("entry", 4, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).NewObject(0, class_name).Invoke(0, method_name, 0).Ret(0);
    module.set_entry_function(0);

    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 9);
}

TEST(Smoke, ReflectionDynamicCall) {
    VmInstance vm;
    ClassDescriptor klass;
    klass.name = "Echo";
    klass.methods["plus1"] = MethodDescriptor{
        "plus1",
        AccessModifier::kPublic,
        true,
        [](ObjectId, const std::vector<Value>& args) {
            return StatusOr<Value>(Value::Int64(args[0].AsInt64() + 1));
        }};
    ASSERT_TRUE(vm.reflection().RegisterClass(std::move(klass)).ok());
    auto obj_or = vm.reflection().NewObject("Echo");
    ASSERT_TRUE(obj_or.ok());
    auto value_or = vm.reflection().Invoke(obj_or.value(), "plus1", {Value::Int64(41)});
    ASSERT_TRUE(value_or.ok()) << value_or.status().message();
    EXPECT_EQ(value_or.value().AsInt64(), 42);
}

TEST(Smoke, GcStressWithWeakPhantomFinalizer) {
    GcController gc;
    std::atomic<int> finalized{0};
    std::vector<uint64_t> weak_refs;
    for (int i = 0; i < 200; ++i) {
        auto id_or = gc.Allocate();
        ASSERT_TRUE(id_or.ok());
        auto weak_or = gc.CreateWeakRef(id_or.value());
        ASSERT_TRUE(weak_or.ok());
        weak_refs.push_back(weak_or.value());
        ASSERT_TRUE(gc.RegisterFinalizer(id_or.value(), [&](ObjectId) { finalized++; }).ok());
    }
    ASSERT_TRUE(gc.CollectMajorAsync().ok());
    ASSERT_TRUE(gc.WaitForMajor().ok());
    size_t dead = 0;
    for (auto id : weak_refs) {
        if (!gc.ResolveWeakRef(id).has_value()) {
            ++dead;
        }
    }
    EXPECT_EQ(dead, weak_refs.size());
    EXPECT_GT(finalized.load(), 0);
}

TEST(Smoke, VmToCAndCToVmCallback) {
    VmInstance vm;
    ASSERT_TRUE(vm.ffi()
                    .RegisterVmCallback(
                        "cb_plus_10",
                        [](const std::vector<Value>& args) {
                            return StatusOr<Value>(Value::Int64(args[0].AsInt64() + 10));
                        })
                    .ok());

    FunctionSignature sig;
    sig.name = "native_bridge";
    sig.convention = CallingConvention::kFastcall;
    sig.return_type = {AbiValueKind::kI64, OwnershipQualifier::kBorrowed};
    sig.params = {{AbiValueKind::kI64, OwnershipQualifier::kBorrowed}};
    ASSERT_TRUE(vm.ffi()
                    .RegisterFunction(
                        std::move(sig),
                        [&](VmThread&, const std::vector<Value>& args) {
                            vm.ffi().AttachCurrentThread();
                            auto value_or =
                                vm.ffi().InvokeVmCallback("cb_plus_10", {args[0]});
                            vm.ffi().DetachCurrentThread();
                            if (!value_or.ok()) {
                                return StatusOr<Value>(value_or.status());
                            }
                            return value_or;
                        })
                    .ok());

    Module module("smoke.ffi");
    const auto v = module.AddConstant(Constant::Int64(32));
    const auto sym = module.AddConstant(Constant::Utf8("native_bridge"));
    auto& fn = module.AddFunction("entry", 4, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).LoadK(1, v).FfiCall(0, sym, 1).Ret(0);
    module.set_entry_function(0);

    auto result_or = vm.ExecuteModule(module);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_EQ(result_or.value().AsInt64(), 42);
}

TEST(Smoke, Utf8StrictValidation) {
    const std::string valid =
        "UTF-8 "
        "\xE4\xB8\xA5\xE6\xA0\xBC\xE6\xA0\xA1\xE9\xAA\x8C";
    EXPECT_TRUE(utf8::Validate(valid).ok());
    const std::string bad = std::string("x") + static_cast<char>(0x80);
    EXPECT_FALSE(utf8::Validate(bad).ok());
}

TEST(Smoke, TlbHotReloadAtomicAndConflictReject) {
    ModuleLoader loader;
    auto s1 = loader.BeginHotReload();
    LoadedModule m1{"mod", SemanticVersion{0, 1, 0}, test::BuildAddModule(1, 1)};
    m1.module.version() = m1.version;
    s1.Stage(std::move(m1));
    ASSERT_TRUE(s1.Commit().ok());

    auto module_or = loader.GetModule("mod");
    ASSERT_TRUE(module_or.ok());
    EXPECT_EQ(module_or.value().version().minor, 1u);

    auto s2 = loader.BeginHotReload();
    LoadedModule c1{"mod", SemanticVersion{0, 2, 0}, test::BuildAddModule(2, 2)};
    c1.module.version() = c1.version;
    LoadedModule c2{"mod", SemanticVersion{0, 3, 0}, test::BuildAddModule(3, 3)};
    c2.module.version() = c2.version;
    s2.Stage(std::move(c1));
    s2.Stage(std::move(c2));
    EXPECT_FALSE(s2.Commit().ok());
}

TEST(Smoke, AotPlaceholderPipeline) {
    AotPipeline pipeline;
    AotUnit unit;
    unit.module_name = "smoke.aot";
    unit.metadata = {{"backend", "none"}, {"stage", "placeholder"}};
    ASSERT_TRUE(pipeline.AddUnit(std::move(unit)).ok());

    const auto out_dir = test::TempPath("smoke_aot");
    ASSERT_TRUE(pipeline.EmitMetadataDirectory(out_dir).ok());
    EXPECT_TRUE(std::filesystem::exists(out_dir / "smoke.aot.aotmeta"));
}

}  // namespace tie::vm
