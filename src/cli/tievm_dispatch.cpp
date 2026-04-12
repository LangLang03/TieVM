#include "tievm_commands.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

namespace {

int PrintUsage() {
#if defined(TIEVM_ENABLE_HELP) && !defined(TIEVM_MINIMAL_STRINGS)
    std::cerr << "Usage:\n";
    std::cerr << "  tievm run <file.tbc> [--validate] [--trusted] [--cache-dir <dir>]\n";
    std::cerr
        << "  tievm run <file.tlb> [module_name] [--validate] [--trusted] [--cache-dir <dir>]\n";
    std::cerr
        << "  tievm run <file.tlbs> [module_name] [--validate] [--trusted] [--cache-dir <dir>]\n";
#endif
    return 1;
}

}  // namespace

int RunTievm(int argc, char** argv) {
    if (argc < 3 || std::string(argv[1]) != "run") {
        return PrintUsage();
    }

    const std::filesystem::path input = argv[2];
    const bool is_tlb = input.extension() == ".tlb" || input.extension() == ".tlbs";
    std::optional<std::string> module_name_override;
    bool enable_runtime_validate = false;
    RunConfig run_config;

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--validate") {
            enable_runtime_validate = true;
            continue;
        }
        if (arg == "--trusted") {
            run_config.trusted = true;
            continue;
        }
        if (arg == "--cache-dir") {
            if (i + 1 >= argc) {
                return PrintUsage();
            }
            run_config.cache_dir = std::filesystem::path(argv[++i]);
            continue;
        }
        if (is_tlb && !module_name_override.has_value()) {
            module_name_override = arg;
            continue;
        }
#if !defined(TIEVM_MINIMAL_STRINGS)
        std::cerr << "unknown argument: " << arg << "\n";
#endif
        return PrintUsage();
    }

    VmInstance vm;
    vm.SetRuntimeValidationEnabled(enable_runtime_validate);

    if (input.extension() == ".tbc") {
        return RunTbcFile(vm, input, run_config);
    }
    if (is_tlb) {
        return RunTlbFile(vm, input, module_name_override, run_config);
    }

#if !defined(TIEVM_MINIMAL_STRINGS)
    std::cerr << "unsupported file extension: " << input.extension().string() << "\n";
#endif
    return 7;
}

}  // namespace tie::vm::cli
