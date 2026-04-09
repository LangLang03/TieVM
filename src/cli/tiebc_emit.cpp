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
