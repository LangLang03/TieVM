#include <filesystem>
#include <iostream>
#include <string>

#include "tie/vm/api.hpp"

namespace {

int Usage() {
    std::cerr << "Usage:\n";
    std::cerr << "  tiebc check <file.tbc>\n";
    std::cerr << "  tiebc disasm <file.tbc>\n";
    std::cerr << "  tiebc build-stdlib <output.tlb>\n";
    std::cerr << "  tiebc emit-hello <output.tbc>\n";
    return 1;
}

int Check(const std::filesystem::path& path) {
    auto module_or = tie::vm::Serializer::DeserializeFromFile(path);
    if (!module_or.ok()) {
        std::cerr << "deserialize failed: " << module_or.status().message() << "\n";
        return 2;
    }
    auto verify = tie::vm::Verifier::Verify(module_or.value());
    if (!verify.status.ok()) {
        std::cerr << "verify failed: " << verify.status.message() << "\n";
        return 3;
    }
    std::cout << "OK\n";
    for (const auto& warning : verify.warnings) {
        std::cout << "warning: " << warning << "\n";
    }
    return 0;
}

int Disasm(const std::filesystem::path& path) {
    auto module_or = tie::vm::Serializer::DeserializeFromFile(path);
    if (!module_or.ok()) {
        std::cerr << "deserialize failed: " << module_or.status().message() << "\n";
        return 2;
    }
    const auto& module = module_or.value();
    std::cout << "module " << module.name() << " v" << module.version().ToString() << "\n";
    for (size_t fn_i = 0; fn_i < module.functions().size(); ++fn_i) {
        const auto& fn = module.functions()[fn_i];
        std::cout << "func[" << fn_i << "] " << fn.name() << " regs=" << fn.reg_count()
                  << " params=" << fn.param_count() << "\n";
        const auto code = fn.FlattenedInstructions();
        for (size_t i = 0; i < code.size(); ++i) {
            const auto& inst = code[i];
            std::cout << "  " << i << ": " << tie::vm::OpCodeName(inst.opcode) << " "
                      << inst.a << ", " << inst.b << ", " << inst.c << "\n";
        }
    }
    return 0;
}

int EmitHello(const std::filesystem::path& path) {
    tie::vm::Module module("demo.hello");
    module.version() = tie::vm::SemanticVersion{0, 1, 0};
    const auto print_symbol = module.AddConstant(tie::vm::Constant::Utf8("tie.std.io.print"));
    const auto hello = module.AddConstant(tie::vm::Constant::Utf8("Hello, World!"));
    const auto chinese = module.AddConstant(
        tie::vm::Constant::Utf8(
            "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C"
            "\xE4\xB8\x96\xE7\x95\x8C\xEF\xBC\x81"));

    auto& fn = module.AddFunction("entry", 6, 0);
    auto& bb = fn.AddBlock("entry");
    tie::vm::InstructionBuilder(bb)
        .LoadK(1, hello)
        .FfiCall(0, print_symbol, 1)
        .LoadK(1, chinese)
        .FfiCall(0, print_symbol, 1)
        .Ret(0);
    module.set_entry_function(0);

    auto status = tie::vm::Serializer::SerializeToFile(module, path, true);
    if (!status.ok()) {
        std::cerr << "emit hello failed: " << status.message() << "\n";
        return 2;
    }
    std::cout << "wrote " << path.string() << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return Usage();
    }
    const std::string cmd = argv[1];
    const std::filesystem::path path = argv[2];
    if (cmd == "check") {
        return Check(path);
    }
    if (cmd == "disasm") {
        return Disasm(path);
    }
    if (cmd == "build-stdlib") {
        auto status = tie::vm::BuildStdlibTlb(path);
        if (!status.ok()) {
            std::cerr << "build stdlib failed: " << status.message() << "\n";
            return 2;
        }
        std::cout << "wrote " << path.string() << "\n";
        return 0;
    }
    if (cmd == "emit-hello") {
        return EmitHello(path);
    }
    return Usage();
}
