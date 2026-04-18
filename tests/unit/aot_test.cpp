#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "test_helpers.hpp"

namespace tie::vm {

namespace {

bool HasClang() {
#if defined(_WIN32)
    return std::system("clang --version >NUL 2>&1") == 0;
#else
    return std::system("clang --version >/dev/null 2>&1") == 0;
#endif
}

std::string QuoteShellArg(const std::string& raw) {
#if defined(_WIN32)
    std::string escaped = "\"";
    for (char c : raw) {
        if (c == '"') {
            escaped += "\\\"";
        } else {
            escaped.push_back(c);
        }
    }
    escaped += '"';
    return escaped;
#else
    std::string escaped = "'";
    for (char c : raw) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(c);
        }
    }
    escaped += '\'';
    return escaped;
#endif
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(AotTest, DeprecatedPlaceholderApiReturnsUnsupported) {
    AotPipeline pipeline;
    AotUnit unit;
    unit.module_name = "deprecated";
    unit.metadata = {{"k", "v"}};

    auto add = pipeline.AddUnit(std::move(unit));
    EXPECT_FALSE(add.ok());
    EXPECT_EQ(add.code(), ErrorCode::kUnsupported);

    auto snapshot = pipeline.SnapshotUnits();
    EXPECT_FALSE(snapshot.ok());
    EXPECT_EQ(snapshot.status().code(), ErrorCode::kUnsupported);

    auto emit = pipeline.EmitMetadataDirectory(test::TempPath("aot_deprecated"));
    EXPECT_FALSE(emit.ok());
    EXPECT_EQ(emit.code(), ErrorCode::kUnsupported);
}

TEST(AotTest, CompileTbcToExecutableAndRun) {
    if (!HasClang()) {
        GTEST_SKIP() << "clang not found";
    }

    Module module("aot.sum");
    const auto c0 = module.AddConstant(Constant::Int64(0));
    const auto c5 = module.AddConstant(Constant::Int64(5));
    auto& fn = module.AddFunction("entry", 8, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).LoadK(0, c0).LoadK(1, c5).AddDecJnz(0, 1, 0).Ret(0);
    module.set_entry_function(0);

    const auto input = test::TempPath("aot_sum.tbc");
    ASSERT_TRUE(Serializer::SerializeToFile(module, input).ok());

#if defined(_WIN32)
    const auto exe = test::TempPath("aot_sum.exe");
#else
    const auto exe = test::TempPath("aot_sum.out");
#endif

    AotCompileOptions options;
    options.input_path = input;
    options.output_executable = exe;
    options.opt_level = "O3";

    AotCompiler compiler;
    auto result_or = compiler.Compile(options);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_TRUE(std::filesystem::exists(exe));

    const auto out_file = test::TempPath("aot_sum_stdout.txt");
    const std::string run_cmd =
        QuoteShellArg(exe.string()) + " > " + QuoteShellArg(out_file.string()) + " 2>&1";
    const int rc = std::system(run_cmd.c_str());
    ASSERT_EQ(rc, 0);

    const auto text = ReadTextFile(out_file);
    EXPECT_NE(text.find("15"), std::string::npos);
}

TEST(AotTest, CompileTlbsDirectoryToExecutableAndRun) {
    if (!HasClang()) {
        GTEST_SKIP() << "clang not found";
    }

    Module module("aot.entry");
    const auto c42 = module.AddConstant(Constant::Int64(42));
    auto& fn = module.AddFunction("entry", 4, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).LoadK(0, c42).Ret(0);
    module.set_entry_function(0);

    auto bytes_or = Serializer::Serialize(module, false);
    ASSERT_TRUE(bytes_or.ok()) << bytes_or.status().message();

    TlbsBundle bundle;
    bundle.manifest().name = "aot.bundle";
    bundle.manifest().modules = {"modules/aot_entry.tbc"};
    bundle.manifest().entry_module = "modules/aot_entry.tbc";
    bundle.SetModule("modules/aot_entry.tbc", std::move(bytes_or.value()));

    const auto tlbs_dir = test::TempPath("aot_bundle.tlbs");
    ASSERT_TRUE(bundle.SerializeToDirectory(tlbs_dir).ok());

#if defined(_WIN32)
    const auto exe = test::TempPath("aot_bundle.exe");
#else
    const auto exe = test::TempPath("aot_bundle.out");
#endif

    AotCompileOptions options;
    options.input_path = tlbs_dir;
    options.output_executable = exe;

    AotCompiler compiler;
    auto result_or = compiler.Compile(options);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_TRUE(std::filesystem::exists(exe));

    const auto out_file = test::TempPath("aot_bundle_stdout.txt");
    const std::string run_cmd =
        QuoteShellArg(exe.string()) + " > " + QuoteShellArg(out_file.string()) + " 2>&1";
    const int rc = std::system(run_cmd.c_str());
    ASSERT_EQ(rc, 0);

    const auto text = ReadTextFile(out_file);
    EXPECT_NE(text.find("42"), std::string::npos);
}

TEST(AotTest, CompileTlbUsesModuleOverride) {
    if (!HasClang()) {
        GTEST_SKIP() << "clang not found";
    }

    Module first("aot.first");
    const auto c1 = first.AddConstant(Constant::Int64(1));
    auto& f1 = first.AddFunction("entry", 2, 0);
    auto& b1 = f1.AddBlock("entry");
    InstructionBuilder(b1).LoadK(0, c1).Ret(0);
    first.set_entry_function(0);

    Module second("aot.second");
    const auto c2 = second.AddConstant(Constant::Int64(2));
    auto& f2 = second.AddFunction("entry", 2, 0);
    auto& b2 = f2.AddBlock("entry");
    InstructionBuilder(b2).LoadK(0, c2).Ret(0);
    second.set_entry_function(0);

    auto first_bytes_or = Serializer::Serialize(first, false);
    ASSERT_TRUE(first_bytes_or.ok()) << first_bytes_or.status().message();
    auto second_bytes_or = Serializer::Serialize(second, false);
    ASSERT_TRUE(second_bytes_or.ok()) << second_bytes_or.status().message();

    TlbContainer tlb;
    tlb.AddModule(TlbModuleEntry{"pkg.first", SemanticVersion{0, 1, 0}, first_bytes_or.value(), {}});
    tlb.AddModule(TlbModuleEntry{"pkg.second", SemanticVersion{0, 1, 0}, second_bytes_or.value(), {}});
    const auto input = test::TempPath("aot_multi.tlb");
    ASSERT_TRUE(tlb.SerializeToFile(input).ok());

#if defined(_WIN32)
    const auto exe = test::TempPath("aot_multi.exe");
#else
    const auto exe = test::TempPath("aot_multi.out");
#endif

    AotCompileOptions options;
    options.input_path = input;
    options.module_name_override = "pkg.second";
    options.output_executable = exe;

    AotCompiler compiler;
    auto result_or = compiler.Compile(options);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_TRUE(std::filesystem::exists(exe));

    const auto out_file = test::TempPath("aot_multi_stdout.txt");
    const std::string run_cmd =
        QuoteShellArg(exe.string()) + " > " + QuoteShellArg(out_file.string()) + " 2>&1";
    const int rc = std::system(run_cmd.c_str());
    ASSERT_EQ(rc, 0);

    const auto text = ReadTextFile(out_file);
    EXPECT_NE(text.find("2"), std::string::npos);
}

TEST(AotTest, CompileVarArgOpcodeAndRun) {
    if (!HasClang()) {
        GTEST_SKIP() << "clang not found";
    }

    Module module("aot.vararg");
    const auto c10 = module.AddConstant(Constant::Int64(10));
    const auto c20 = module.AddConstant(Constant::Int64(20));

    auto& vararg_fn = module.AddFunction("sum2", 8, 0, 0, true);
    auto& vararg_bb = vararg_fn.AddBlock("entry");
    InstructionBuilder(vararg_bb).VarArg(0, 0, 1).VarArg(1, 1, 1).Add(0, 0, 1).Ret(0);

    auto& entry = module.AddFunction("entry", 8, 0);
    auto& entry_bb = entry.AddBlock("entry");
    InstructionBuilder(entry_bb)
        .LoadK(1, c10)
        .LoadK(2, c20)
        .Call(0, 0, 2)
        .Ret(0);
    module.set_entry_function(1);

    const auto input = test::TempPath("aot_vararg.tbc");
    ASSERT_TRUE(Serializer::SerializeToFile(module, input).ok());

#if defined(_WIN32)
    const auto exe = test::TempPath("aot_vararg.exe");
#else
    const auto exe = test::TempPath("aot_vararg.out");
#endif

    AotCompileOptions options;
    options.input_path = input;
    options.output_executable = exe;

    AotCompiler compiler;
    auto result_or = compiler.Compile(options);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_TRUE(std::filesystem::exists(exe));

    const auto out_file = test::TempPath("aot_vararg_stdout.txt");
    const std::string run_cmd =
        QuoteShellArg(exe.string()) + " > " + QuoteShellArg(out_file.string()) + " 2>&1";
    const int rc = std::system(run_cmd.c_str());
    ASSERT_EQ(rc, 0);

    const auto text = ReadTextFile(out_file);
    EXPECT_NE(text.find("30"), std::string::npos);
}

TEST(AotTest, CompileTryCatchFinallyAndThrowOpcode) {
    if (!HasClang()) {
        GTEST_SKIP() << "clang not found";
    }

    Module module("aot.try.throw");
    const auto c_zero = module.AddConstant(Constant::Int64(0));
    const auto c_one = module.AddConstant(Constant::Int64(1));
    auto& fn = module.AddFunction("entry", 8, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(0, c_zero)
        .TryBegin(5, 7, 10)
        .LoadK(3, c_one)
        .Throw(3)
        .TryEnd()
        .LoadK(0, c_one)
        .EndCatch()
        .LoadK(4, c_one)
        .Add(0, 0, 4)
        .EndFinally()
        .Ret(0);
    module.set_entry_function(0);

    const auto input = test::TempPath("aot_try_throw.tbc");
    ASSERT_TRUE(Serializer::SerializeToFile(module, input).ok());

#if defined(_WIN32)
    const auto exe = test::TempPath("aot_try_throw.exe");
#else
    const auto exe = test::TempPath("aot_try_throw.out");
#endif

    AotCompileOptions options;
    options.input_path = input;
    options.output_executable = exe;

    AotCompiler compiler;
    auto result_or = compiler.Compile(options);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_TRUE(std::filesystem::exists(exe));

    const auto out_file = test::TempPath("aot_try_throw_stdout.txt");
    const std::string run_cmd =
        QuoteShellArg(exe.string()) + " > " + QuoteShellArg(out_file.string()) + " 2>&1";
    const int rc = std::system(run_cmd.c_str());
    ASSERT_EQ(rc, 0);

    const auto text = ReadTextFile(out_file);
    EXPECT_NE(text.find("2"), std::string::npos);
}

TEST(AotTest, CompileSharedLibraryExportsFunctionsAndGeneratesHeader) {
    if (!HasClang()) {
        GTEST_SKIP() << "clang not found";
    }
#if defined(_WIN32)
    GTEST_SKIP() << "shared export integration test not enabled on windows yet";
#else
    Module module("aot.export.sum");
    auto& sum = module.AddFunction(
        "sum_export",
        4,
        2,
        0,
        false,
        true,
        {BytecodeValueType::kInt64, BytecodeValueType::kBool},
        BytecodeValueType::kInt64);
    auto& sum_bb = sum.AddBlock("entry");
    InstructionBuilder(sum_bb).Add(0, 0, 1).Ret(0);
    module.set_entry_function(0);

    const auto input = test::TempPath("aot_export_sum.tbc");
    ASSERT_TRUE(Serializer::SerializeToFile(module, input).ok());

#if defined(__APPLE__)
    const auto shared = test::TempPath("libaot_export_sum.dylib");
#else
    const auto shared = test::TempPath("libaot_export_sum.so");
#endif
    const auto header = test::TempPath("aot_export_sum.h");
    const auto client_src = test::TempPath("aot_export_client.c");
    const auto client_exe = test::TempPath("aot_export_client.out");

    AotCompileOptions options;
    options.input_path = input;
    options.output_executable = shared;
    options.output_kind = AotOutputKind::kSharedLibrary;
    options.emit_header = header;

    AotCompiler compiler;
    auto result_or = compiler.Compile(options);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    ASSERT_TRUE(std::filesystem::exists(shared));
    ASSERT_TRUE(std::filesystem::exists(header));

    const auto& result = result_or.value();
    EXPECT_EQ(result.output_kind, AotOutputKind::kSharedLibrary);
    ASSERT_EQ(result.exported_functions.size(), 1u);
    EXPECT_EQ(result.exported_functions[0], "sum_export");
    ASSERT_TRUE(result.emitted_header.has_value());
    EXPECT_EQ(result.emitted_header.value(), header);

    const auto header_text = ReadTextFile(header);
    EXPECT_NE(header_text.find("sum_export"), std::string::npos);
    EXPECT_NE(
        header_text.find("int64_t sum_export(int64_t arg0, bool arg1)"),
        std::string::npos);

    std::ofstream out(client_src, std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out << "#include <stdint.h>\n";
    out << "#include \"" << header.string() << "\"\n";
    out << "int main(void) {\n";
    out << "  int64_t result = sum_export(41, true);\n";
    out << "  return (result == 42) ? 0 : 3;\n";
    out << "}\n";
    out.close();

    const std::string compile_cmd =
        std::string("clang ") + QuoteShellArg(client_src.string()) + " " +
        QuoteShellArg(shared.string()) + " " +
        QuoteShellArg("-Wl,-rpath," + shared.parent_path().string()) + " -o " +
        QuoteShellArg(client_exe.string());
    const int compile_rc = std::system(compile_cmd.c_str());
    ASSERT_EQ(compile_rc, 0);

    const std::string run_cmd = QuoteShellArg(client_exe.string()) + " >/dev/null 2>&1";
    const int run_rc = std::system(run_cmd.c_str());
    ASSERT_EQ(run_rc, 0);
#endif
}

TEST(AotTest, CompileSharedLibraryRequiresExportedFunction) {
    if (!HasClang()) {
        GTEST_SKIP() << "clang not found";
    }

    Module module("aot.shared.noexport");
    auto& fn = module.AddFunction("hidden_impl", 2, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).Ret(0);
    module.set_entry_function(0);

    const auto input = test::TempPath("aot_shared_noexport.tbc");
    ASSERT_TRUE(Serializer::SerializeToFile(module, input).ok());

#if defined(_WIN32)
    const auto shared = test::TempPath("aot_shared_noexport.dll");
#elif defined(__APPLE__)
    const auto shared = test::TempPath("aot_shared_noexport.dylib");
#else
    const auto shared = test::TempPath("aot_shared_noexport.so");
#endif

    AotCompileOptions options;
    options.input_path = input;
    options.output_executable = shared;
    options.output_kind = AotOutputKind::kSharedLibrary;

    AotCompiler compiler;
    auto result_or = compiler.Compile(options);
    EXPECT_FALSE(result_or.ok());
}

TEST(AotTest, CompileOopInvokeWithC3DispatchAndRun) {
    if (!HasClang()) {
        GTEST_SKIP() << "clang not found";
    }

    Module module("aot.oop.c3");
    const auto c_one = module.AddConstant(Constant::Int64(1));
    const auto c_two = module.AddConstant(Constant::Int64(2));
    const auto c_three = module.AddConstant(Constant::Int64(3));
    const auto c_class_d = module.AddConstant(Constant::Utf8("D"));
    const auto c_method_who = module.AddConstant(Constant::Utf8("who"));

    auto& fn_a = module.AddFunction("a_who", 4, 1);
    auto& bb_a = fn_a.AddBlock("entry");
    InstructionBuilder(bb_a).LoadK(0, c_one).Ret(0);

    auto& fn_b = module.AddFunction("b_who", 4, 1);
    auto& bb_b = fn_b.AddBlock("entry");
    InstructionBuilder(bb_b).LoadK(0, c_two).Ret(0);

    auto& fn_c = module.AddFunction("c_who", 4, 1);
    auto& bb_c = fn_c.AddBlock("entry");
    InstructionBuilder(bb_c).LoadK(0, c_three).Ret(0);

    BytecodeClassDecl class_a;
    class_a.name = "A";
    class_a.methods.push_back(
        BytecodeMethodDecl{"who", 0, BytecodeAccessModifier::kPublic, true});
    module.AddClass(std::move(class_a));

    BytecodeClassDecl class_b;
    class_b.name = "B";
    class_b.base_classes = {"A"};
    class_b.methods.push_back(
        BytecodeMethodDecl{"who", 1, BytecodeAccessModifier::kPublic, true});
    module.AddClass(std::move(class_b));

    BytecodeClassDecl class_c;
    class_c.name = "C";
    class_c.base_classes = {"A"};
    class_c.methods.push_back(
        BytecodeMethodDecl{"who", 2, BytecodeAccessModifier::kPublic, true});
    module.AddClass(std::move(class_c));

    BytecodeClassDecl class_d;
    class_d.name = "D";
    class_d.base_classes = {"B", "C"};
    module.AddClass(std::move(class_d));

    auto& entry = module.AddFunction("entry", 8, 0);
    auto& entry_bb = entry.AddBlock("entry");
    InstructionBuilder(entry_bb).NewObject(0, c_class_d).Invoke(0, c_method_who, 0).Ret(0);
    module.set_entry_function(3);

    const auto input = test::TempPath("aot_oop_c3.tbc");
    ASSERT_TRUE(Serializer::SerializeToFile(module, input).ok());

#if defined(_WIN32)
    const auto exe = test::TempPath("aot_oop_c3.exe");
#else
    const auto exe = test::TempPath("aot_oop_c3.out");
#endif

    AotCompileOptions options;
    options.input_path = input;
    options.output_executable = exe;

    AotCompiler compiler;
    auto result_or = compiler.Compile(options);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_TRUE(std::filesystem::exists(exe));

    const auto out_file = test::TempPath("aot_oop_c3_stdout.txt");
    const std::string run_cmd =
        QuoteShellArg(exe.string()) + " > " + QuoteShellArg(out_file.string()) + " 2>&1";
    const int rc = std::system(run_cmd.c_str());
    ASSERT_EQ(rc, 0);

    const auto text = ReadTextFile(out_file);
    EXPECT_NE(text.find("2"), std::string::npos);
}

TEST(AotTest, CompileOopInvokePrivateMethodAbortsAtRuntime) {
    if (!HasClang()) {
        GTEST_SKIP() << "clang not found";
    }

    Module module("aot.oop.private");
    const auto c_secret = module.AddConstant(Constant::Utf8("Secret"));
    const auto c_hidden = module.AddConstant(Constant::Utf8("hidden"));
    const auto c_42 = module.AddConstant(Constant::Int64(42));

    auto& hidden_fn = module.AddFunction("hidden_impl", 4, 1);
    auto& hidden_bb = hidden_fn.AddBlock("entry");
    InstructionBuilder(hidden_bb).LoadK(0, c_42).Ret(0);

    BytecodeClassDecl secret;
    secret.name = "Secret";
    secret.methods.push_back(
        BytecodeMethodDecl{"hidden", 0, BytecodeAccessModifier::kPrivate, true});
    module.AddClass(std::move(secret));

    auto& entry = module.AddFunction("entry", 6, 0);
    auto& entry_bb = entry.AddBlock("entry");
    InstructionBuilder(entry_bb).NewObject(0, c_secret).Invoke(0, c_hidden, 0).Ret(0);
    module.set_entry_function(1);

    const auto input = test::TempPath("aot_oop_private.tbc");
    ASSERT_TRUE(Serializer::SerializeToFile(module, input).ok());

#if defined(_WIN32)
    const auto exe = test::TempPath("aot_oop_private.exe");
#else
    const auto exe = test::TempPath("aot_oop_private.out");
#endif

    AotCompileOptions options;
    options.input_path = input;
    options.output_executable = exe;

    AotCompiler compiler;
    auto result_or = compiler.Compile(options);
    ASSERT_TRUE(result_or.ok()) << result_or.status().message();
    EXPECT_TRUE(std::filesystem::exists(exe));

    const auto out_file = test::TempPath("aot_oop_private_stdout.txt");
    const std::string run_cmd =
        QuoteShellArg(exe.string()) + " > " + QuoteShellArg(out_file.string()) + " 2>&1";
    const int rc = std::system(run_cmd.c_str());
    EXPECT_NE(rc, 0);
}

}  // namespace tie::vm
