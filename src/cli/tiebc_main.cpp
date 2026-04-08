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
    return Usage();
}

