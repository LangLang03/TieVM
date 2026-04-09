#include "tiebc_commands.hpp"

#include <iostream>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

namespace {

std::string NativeStdlibFilename() {
#if defined(_WIN32)
    return "tievm_std_native.dll";
#elif defined(__APPLE__)
    return "libtievm_std_native.dylib";
#else
    return "libtievm_std_native.so";
#endif
}

}  // namespace

int EmitHello(const std::filesystem::path& path) {
    Module module("demo.hello");
    module.version() = SemanticVersion{0, 1, 0};
    const auto print_symbol = module.AddConstant(Constant::Utf8("tie.std.io.print"));
    const auto hello = module.AddConstant(Constant::Utf8("Hello, World!"));
    const auto chinese = module.AddConstant(
        Constant::Utf8(
            "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C"
            "\xE4\xB8\x96\xE7\x95\x8C\xEF\xBC\x81"));
    const auto lib_index = module.AddFfiLibraryPath(
        (std::filesystem::current_path() / NativeStdlibFilename()).string());

    FunctionSignature print_sig;
    print_sig.name = "tie.std.io.print";
    print_sig.convention = CallingConvention::kSystem;
    print_sig.return_type = {AbiValueKind::kVoid, OwnershipQualifier::kBorrowed};
    print_sig.params = {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed}};
    const auto sig_index = module.AddFfiSignature(std::move(print_sig));
    const auto bind_index = module.AddFfiBinding(FfiSymbolBinding{
        "tie.std.io.print",
        "tie_std_io_print",
        lib_index,
        sig_index,
    });

    auto& fn = module.AddFunction("entry", 6, 0);
    fn.ffi_binding().enabled = true;
    fn.ffi_binding().convention = CallingConvention::kSystem;
    fn.ffi_binding().signature_index = sig_index;
    fn.ffi_binding().binding_index = bind_index;
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(1, hello)
        .FfiCall(0, print_symbol, 1)
        .LoadK(1, chinese)
        .FfiCall(0, print_symbol, 1)
        .Ret(0);
    module.set_entry_function(0);

    auto status = Serializer::SerializeToFile(module, path, true);
    if (!status.ok()) {
        std::cerr << "emit hello failed: " << status.message() << "\n";
        return 2;
    }
    std::cout << "wrote " << path.string() << "\n";
    return 0;
}

int EmitOpset(const std::filesystem::path& path) {
    Module module("demo.opset");
    module.version() = SemanticVersion{0, 1, 0};
    const auto c_one = module.AddConstant(Constant::Int64(1));
    const auto c_three = module.AddConstant(Constant::Int64(3));
    const auto c_six = module.AddConstant(Constant::Int64(6));
    const auto c_ten = module.AddConstant(Constant::Int64(10));
    const auto c_hundred = module.AddConstant(Constant::Int64(100));
    const auto c_hello = module.AddConstant(Constant::Utf8("hello "));
    const auto c_world = module.AddConstant(Constant::Utf8("world"));

    auto& closure_add = module.AddFunction("closure_add", 4, 1, 1, false);
    auto& closure_add_bb = closure_add.AddBlock("entry");
    InstructionBuilder(closure_add_bb).GetUpval(1, 0).Add(0, 0, 1).Ret(0);

    auto& var_sum = module.AddFunction("var_sum", 6, 0, 0, true);
    auto& var_sum_bb = var_sum.AddBlock("entry");
    InstructionBuilder(var_sum_bb).VarArg(0, 0, 1).VarArg(1, 1, 1).Add(0, 0, 1).Ret(0);

    auto& id = module.AddFunction("id", 2, 1);
    auto& id_bb = id.AddBlock("entry");
    InstructionBuilder(id_bb).Ret(0);

    auto& tail_to_id = module.AddFunction("tail_to_id", 4, 1);
    auto& tail_to_id_bb = tail_to_id.AddBlock("entry");
    InstructionBuilder(tail_to_id_bb).Mov(1, 0).TailCall(0, 2, 1);

    auto& tail_to_closure = module.AddFunction("tail_to_closure", 6, 2);
    auto& tail_to_closure_bb = tail_to_closure.AddBlock("entry");
    InstructionBuilder(tail_to_closure_bb).Mov(3, 1).TailCallClosure(2, 0, 1);

    auto& entry = module.AddFunction("entry", 24, 0);
    auto& bb = entry.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(1, c_ten)
        .Closure(0, 0, 1, 1)
        .LoadK(4, c_one)
        .CallClosure(3, 0, 1)
        .LoadK(7, c_ten)
        .LoadK(8, c_hundred)
        .Call(6, 1, 2)
        .LoadK(10, c_ten)
        .Call(9, 3, 1)
        .Mov(12, 0)
        .LoadK(13, c_one)
        .Call(11, 4, 2)
        .LoadK(14, c_hello)
        .LoadK(15, c_world)
        .StrConcat(16, 14, 15)
        .StrLen(17, 16)
        .LoadK(18, c_six)
        .LoadK(19, c_three)
        .BitAnd(18, 18, 19)
        .BitOr(18, 18, 17)
        .Add(0, 3, 6)
        .Add(0, 0, 9)
        .Add(0, 0, 11)
        .Add(0, 0, 18)
        .Ret(0);

    module.set_entry_function(5);
    auto status = Serializer::SerializeToFile(module, path, true);
    if (!status.ok()) {
        std::cerr << "emit opset failed: " << status.message() << "\n";
        return 2;
    }
    std::cout << "wrote " << path.string() << "\n";
    return 0;
}

int EmitOopOk(const std::filesystem::path& path) {
    Module module("demo.oop_ok");
    module.version() = SemanticVersion{0, 1, 0};

    const auto c_20 = module.AddConstant(Constant::Int64(20));
    const auto c_22 = module.AddConstant(Constant::Int64(22));
    const auto c_demo_class = module.AddConstant(Constant::Utf8("DemoMath"));
    const auto c_plus1_method = module.AddConstant(Constant::Utf8("plus1"));

    auto& add_fn = module.AddFunction("add2", 4, 2);
    auto& add_bb = add_fn.AddBlock("entry");
    InstructionBuilder(add_bb).Add(0, 0, 1).Ret(0);

    auto& plus1_fn = module.AddFunction("plus1_impl", 4, 2);
    auto& plus1_bb = plus1_fn.AddBlock("entry");
    InstructionBuilder(plus1_bb).AddImm(0, 1, 1).Ret(0);

    BytecodeClassDecl demo_class;
    demo_class.name = "DemoMath";
    demo_class.methods.push_back(BytecodeMethodDecl{
        "plus1",
        1,
        BytecodeAccessModifier::kPublic,
        true,
    });
    module.AddClass(std::move(demo_class));

    auto& entry = module.AddFunction("entry", 10, 0);
    auto& bb = entry.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(1, c_20)
        .LoadK(2, c_22)
        .Call(0, 0, 2)
        .NewObject(3, c_demo_class)
        .Mov(4, 0)
        .Invoke(3, c_plus1_method, 1)
        .Add(0, 0, 3)
        .Ret(0);
    module.set_entry_function(2);

    auto status = Serializer::SerializeToFile(module, path, true);
    if (!status.ok()) {
        std::cerr << "emit oop_ok failed: " << status.message() << "\n";
        return 2;
    }
    std::cout << "wrote " << path.string() << "\n";
    return 0;
}

int EmitOopError(const std::filesystem::path& path) {
    Module module("demo.oop_error");
    module.version() = SemanticVersion{0, 1, 0};

    const auto c_20 = module.AddConstant(Constant::Int64(20));
    const auto c_22 = module.AddConstant(Constant::Int64(22));
    const auto c_demo_class = module.AddConstant(Constant::Utf8("DemoMath"));
    const auto c_missing_method = module.AddConstant(Constant::Utf8("missing_method"));

    auto& add_fn = module.AddFunction("add2", 4, 2);
    auto& add_bb = add_fn.AddBlock("entry");
    InstructionBuilder(add_bb).Add(0, 0, 1).Ret(0);

    auto& plus1_fn = module.AddFunction("plus1_impl", 4, 2);
    auto& plus1_bb = plus1_fn.AddBlock("entry");
    InstructionBuilder(plus1_bb).AddImm(0, 1, 1).Ret(0);

    BytecodeClassDecl demo_class;
    demo_class.name = "DemoMath";
    demo_class.methods.push_back(BytecodeMethodDecl{
        "plus1",
        1,
        BytecodeAccessModifier::kPublic,
        true,
    });
    module.AddClass(std::move(demo_class));

    auto& entry = module.AddFunction("entry", 10, 0);
    auto& bb = entry.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(1, c_20)
        .LoadK(2, c_22)
        .Call(0, 0, 2)
        .NewObject(3, c_demo_class)
        .Invoke(3, c_missing_method, 0)
        .Ret(0);
    module.set_entry_function(2);

    auto status = Serializer::SerializeToFile(module, path, true);
    if (!status.ok()) {
        std::cerr << "emit oop_error failed: " << status.message() << "\n";
        return 2;
    }
    std::cout << "wrote " << path.string() << "\n";
    return 0;
}

int BuildStdlibTlbsCmd(const std::filesystem::path& path) {
    auto status = BuildStdlibTlbs(path);
    if (!status.ok()) {
        std::cerr << "build stdlib.tlbs failed: " << status.message() << "\n";
        return 2;
    }
    std::cout << "wrote " << path.string() << "\n";
    return 0;
}

}  // namespace tie::vm::cli
