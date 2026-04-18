#include "tiebc_commands.hpp"

#include <iostream>
#include <string>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

namespace {

int PrintAotUsage() {
#if defined(TIEVM_ENABLE_HELP)
    std::cerr << "Usage:\n";
    std::cerr
        << "  tiebc aot <input.{tbc|tlb|tlbs}> [module] -o <output> "
           "[--shared] "
           "[--target <triple>] [--cc <clang>] [--sysroot <path>] "
           "[--opt <O0|O1|O2|O3>] [--ldflag <flag>]... [--cflag <flag>]... "
           "[--emit-ir <file.ll>] [--emit-obj <file.o>] [--emit-header <file.h>]\n";
#endif
    return 1;
}

}  // namespace

int AotCompileCmd(int argc, char** argv) {
    if (argc < 5) {
        return PrintAotUsage();
    }

    AotCompileOptions options;
    options.input_path = std::filesystem::path(argv[2]);

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (!arg.empty() && arg[0] != '-') {
            if (!options.module_name_override.has_value()) {
                options.module_name_override = arg;
                continue;
            }
            std::cerr << "aot: duplicate module argument: " << arg << "\n";
            return PrintAotUsage();
        }

        if (arg == "-o") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            options.output_executable = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--shared") {
            options.output_kind = AotOutputKind::kSharedLibrary;
            continue;
        }
        if (arg == "--target") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            options.target_triple = std::string(argv[++i]);
            continue;
        }
        if (arg == "--cc") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            options.clang_path = std::string(argv[++i]);
            continue;
        }
        if (arg == "--sysroot") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            options.sysroot = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--opt") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            options.opt_level = std::string(argv[++i]);
            continue;
        }
        if (arg == "--emit-ir") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            options.emit_ir = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--emit-obj") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            options.emit_obj = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--emit-header") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            options.emit_header = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--ldflag") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            options.ldflags.push_back(std::string(argv[++i]));
            continue;
        }
        if (arg == "--cflag") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            options.cflags.push_back(std::string(argv[++i]));
            continue;
        }

        std::cerr << "aot: unknown argument: " << arg << "\n";
        return PrintAotUsage();
    }

    if (options.output_executable.empty()) {
        std::cerr << "aot: missing required -o <output>\n";
        return PrintAotUsage();
    }

    AotCompiler compiler;
    auto result_or = compiler.Compile(options);
    if (!result_or.ok()) {
        std::cerr << "aot compile failed: " << result_or.status().message() << "\n";
        return 2;
    }

    const auto& result = result_or.value();
    std::cout << "aot wrote " << result.output_executable.string()
              << " module=" << result.compiled_module
              << " target=" << result.target_triple
              << " kind="
              << (result.output_kind == AotOutputKind::kSharedLibrary ? "shared" : "exe")
              << "\n";
    if (result.emitted_ir.has_value()) {
        std::cout << "aot ir=" << result.emitted_ir->string() << "\n";
    }
    if (result.emitted_obj.has_value()) {
        std::cout << "aot obj=" << result.emitted_obj->string() << "\n";
    }
    if (result.emitted_header.has_value()) {
        std::cout << "aot header=" << result.emitted_header->string() << "\n";
    }
    if (!result.exported_functions.empty()) {
        std::cout << "aot exports=";
        for (size_t i = 0; i < result.exported_functions.size(); ++i) {
            if (i > 0) {
                std::cout << ",";
            }
            std::cout << result.exported_functions[i];
        }
        std::cout << "\n";
    }
    return 0;
}

}  // namespace tie::vm::cli
