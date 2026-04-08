#include <filesystem>
#include <iostream>
#include <string>

#include "tie/vm/api.hpp"

namespace {

int PrintUsage() {
    std::cerr << "Usage:\n";
    std::cerr << "  tievm run <file.tbc>\n";
    std::cerr << "  tievm run <file.tlb> [module_name]\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || std::string(argv[1]) != "run") {
        return PrintUsage();
    }

    const std::filesystem::path input = argv[2];
    tie::vm::VmInstance vm;

    if (input.extension() == ".tbc") {
        auto module_or = tie::vm::Serializer::DeserializeFromFile(input);
        if (!module_or.ok()) {
            std::cerr << "deserialize failed: " << module_or.status().message() << "\n";
            return 2;
        }
        auto value_or = vm.ExecuteModule(module_or.value());
        if (!value_or.ok()) {
            std::cerr << "runtime failed: " << value_or.status().message() << "\n";
            return 3;
        }
        std::cout << value_or.value().ToString() << "\n";
        return 0;
    }

    if (input.extension() == ".tlb") {
        auto status = vm.loader().LoadTlbFile(input);
        if (!status.ok()) {
            std::cerr << "tlb load failed: " << status.message() << "\n";
            return 4;
        }
        std::string module_name;
        if (argc >= 4) {
            module_name = argv[3];
        } else {
            auto names = vm.loader().ActiveModuleNames();
            if (names.empty()) {
                std::cerr << "tlb has no module\n";
                return 5;
            }
            module_name = names.front();
        }
        auto value_or = vm.ExecuteLoadedModule(module_name);
        if (!value_or.ok()) {
            std::cerr << "runtime failed: " << value_or.status().message() << "\n";
            return 6;
        }
        std::cout << value_or.value().ToString() << "\n";
        return 0;
    }

    std::cerr << "unsupported file extension: " << input.extension().string() << "\n";
    return 7;
}

