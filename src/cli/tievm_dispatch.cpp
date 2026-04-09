#include "tievm_commands.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

namespace {

int PrintUsage() {
    std::cerr << "Usage:\n";
    std::cerr << "  tievm run <file.tbc>\n";
    std::cerr << "  tievm run <file.tlb> [module_name]\n";
    std::cerr << "  tievm run <file.tlbs> [module_name]\n";
    return 1;
}

}  // namespace

int RunTievm(int argc, char** argv) {
    if (argc < 3 || std::string(argv[1]) != "run") {
        return PrintUsage();
    }

    const std::filesystem::path input = argv[2];
    VmInstance vm;

    if (input.extension() == ".tbc") {
        return RunTbcFile(vm, input);
    }
    if (input.extension() == ".tlb" || input.extension() == ".tlbs") {
        std::optional<std::string> module_name_override;
        if (argc >= 4) {
            module_name_override = argv[3];
        }
        return RunTlbFile(vm, input, module_name_override);
    }

    std::cerr << "unsupported file extension: " << input.extension().string() << "\n";
    return 7;
}

}  // namespace tie::vm::cli
