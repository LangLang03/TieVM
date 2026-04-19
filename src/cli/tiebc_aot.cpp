#include "tiebc_commands.hpp"

#include <charconv>
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
           "[--opt <O0|O1|O2|O3>] "
           "[--bc-opt <O1|O2|O3>] [--bc-enable-pass <pass>]... "
           "[--bc-disable-pass <pass>]... [--bc-inline-max-inst <n>] "
           "[--ldflag <flag>]... [--cflag <flag>]... "
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
        if (arg == "--bc-opt") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            auto level_or = ParseBytecodeOptLevel(argv[++i]);
            if (!level_or.ok()) {
                std::cerr << "aot: " << level_or.status().message() << "\n";
                return PrintAotUsage();
            }
            if (level_or.value() == BytecodeOptLevel::kO0) {
                std::cerr << "aot: --bc-opt only supports O1/O2/O3\n";
                return PrintAotUsage();
            }
            options.bytecode_opt_level = level_or.value();
            continue;
        }
        if (arg == "--bc-enable-pass") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            auto pass_or = ParseBytecodeOptPass(argv[++i]);
            if (!pass_or.ok()) {
                std::cerr << "aot: " << pass_or.status().message() << "\n";
                return PrintAotUsage();
            }
            options.bytecode_enable_passes.push_back(pass_or.value());
            continue;
        }
        if (arg == "--bc-disable-pass") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            auto pass_or = ParseBytecodeOptPass(argv[++i]);
            if (!pass_or.ok()) {
                std::cerr << "aot: " << pass_or.status().message() << "\n";
                return PrintAotUsage();
            }
            options.bytecode_disable_passes.push_back(pass_or.value());
            continue;
        }
        if (arg == "--bc-inline-max-inst") {
            if (i + 1 >= argc) {
                return PrintAotUsage();
            }
            uint32_t parsed = 0;
            const std::string value = argv[++i];
            const auto [ptr, ec] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (ec != std::errc() || ptr != value.data() + value.size() || parsed == 0) {
                std::cerr << "aot: --bc-inline-max-inst must be a positive integer\n";
                return PrintAotUsage();
            }
            options.bytecode_inline_max_inst = parsed;
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
    if (result.bytecode_opt_stats.has_value()) {
        const auto& stats = *result.bytecode_opt_stats;
        std::cout << "aot bc-opt level=" << BytecodeOptLevelName(*options.bytecode_opt_level)
                  << " functions=" << stats.module_function_count
                  << " optimized_functions=" << stats.optimized_function_count
                  << " rewritten=" << stats.rewritten_instruction_count
                  << " removed=" << stats.removed_instruction_count
                  << " inlined=" << stats.inlined_callsite_count << "\n";
        if (!stats.executed_passes.empty()) {
            std::cout << "aot bc-opt passes=";
            for (size_t i = 0; i < stats.executed_passes.size(); ++i) {
                if (i > 0) {
                    std::cout << ",";
                }
                std::cout << BytecodeOptPassName(stats.executed_passes[i]);
            }
            std::cout << "\n";
        }
    }
    return 0;
}

}  // namespace tie::vm::cli
