#include "tiebc_commands.hpp"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

namespace {

struct OptCliOptions {
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    std::optional<std::string> module_filter;
    BytecodeOptOptions opt_options;
    bool trusted = false;
};

struct OptRunSummary {
    BytecodeOptStats stats;
    size_t optimized_modules = 0;
};

int PrintTiebcOptUsage() {
#if defined(TIEVM_ENABLE_HELP)
    std::cerr << "Usage:\n";
    std::cerr
        << "  tiebc opt <input.{tbc|tlb|tlbs}> -o <output> "
           "[--module <name>] [--opt <O1|O2|O3>] "
           "[--enable-pass <pass>]... [--disable-pass <pass>]... "
           "[--inline-max-inst <n>] [--trusted]\n";
#endif
    return 1;
}

int PrintTievmOptUsage() {
#if defined(TIEVM_ENABLE_HELP)
    std::cerr << "Usage:\n";
    std::cerr
        << "  tievm_opt <input.{tbc|tlb|tlbs}> -o <output> "
           "[--module <name>] [--opt <O1|O2|O3>] "
           "[--enable-pass <pass>]... [--disable-pass <pass>]... "
           "[--inline-max-inst <n>] [--trusted]\n";
#endif
    return 1;
}

bool ParseUInt32(std::string_view text, uint32_t* out) {
    if (text.empty()) {
        return false;
    }
    uint32_t parsed = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        return false;
    }
    *out = parsed;
    return true;
}

bool MatchModuleFilter(
    std::string_view filter, std::string_view module_name,
    std::string_view package_or_path) {
    if (filter == module_name || filter == package_or_path) {
        return true;
    }
    if (filter == std::filesystem::path(std::string(module_name)).stem().string()) {
        return true;
    }
    if (filter == std::filesystem::path(std::string(package_or_path)).stem().string()) {
        return true;
    }
    return false;
}

void MergeStats(BytecodeOptStats* into, const BytecodeOptStats& from) {
    into->module_function_count += from.module_function_count;
    into->optimized_function_count += from.optimized_function_count;
    into->rewritten_instruction_count += from.rewritten_instruction_count;
    into->removed_instruction_count += from.removed_instruction_count;
    into->inlined_callsite_count += from.inlined_callsite_count;
    if (into->executed_passes.empty()) {
        into->executed_passes = from.executed_passes;
    }
}

std::string FormatPassList(const std::vector<BytecodeOptPass>& passes) {
    std::string out;
    for (size_t i = 0; i < passes.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += std::string(BytecodeOptPassName(passes[i]));
    }
    return out;
}

StatusOr<OptRunSummary> OptimizeTbc(const OptCliOptions& options) {
    DeserializeOptions deserialize_options;
    deserialize_options.verify = !options.trusted;
    auto module_or = Serializer::DeserializeFromFile(options.input_path, deserialize_options);
    if (!module_or.ok()) {
        return module_or.status();
    }

    if (options.module_filter.has_value() &&
        !MatchModuleFilter(
            *options.module_filter,
            module_or.value().name(),
            options.input_path.stem().string())) {
        return Status::NotFound("module filter not matched: " + *options.module_filter);
    }

    auto opt_options = options.opt_options;
    opt_options.verify_input = !options.trusted;
    auto optimized_or = OptimizeBytecodeModule(module_or.value(), opt_options);
    if (!optimized_or.ok()) {
        return optimized_or.status();
    }

    auto write_status =
        Serializer::SerializeToFile(optimized_or.value().module, options.output_path, false);
    if (!write_status.ok()) {
        return write_status;
    }

    OptRunSummary summary;
    summary.stats = optimized_or.value().stats;
    summary.optimized_modules = 1;
    return summary;
}

StatusOr<OptRunSummary> OptimizeTlb(const OptCliOptions& options) {
    auto container_or = TlbContainer::DeserializeFromFile(options.input_path);
    if (!container_or.ok()) {
        return container_or.status();
    }

    TlbContainer output_container;
    OptRunSummary summary;

    bool matched = false;
    for (const auto& entry : container_or.value().modules()) {
        TlbModuleEntry out_entry = entry;

        DeserializeOptions deserialize_options;
        deserialize_options.verify = !options.trusted;
        auto module_or = Serializer::Deserialize(entry.bytecode, deserialize_options);
        if (!module_or.ok()) {
            return module_or.status();
        }

        const bool should_optimize =
            !options.module_filter.has_value() ||
            MatchModuleFilter(*options.module_filter, module_or.value().name(), entry.module_name);
        if (should_optimize) {
            matched = true;
            auto opt_options = options.opt_options;
            opt_options.verify_input = !options.trusted;
            auto optimized_or = OptimizeBytecodeModule(module_or.value(), opt_options);
            if (!optimized_or.ok()) {
                return optimized_or.status();
            }

            auto bytes_or = Serializer::Serialize(optimized_or.value().module, false);
            if (!bytes_or.ok()) {
                return bytes_or.status();
            }

            out_entry.bytecode = std::move(bytes_or.value());
            MergeStats(&summary.stats, optimized_or.value().stats);
            ++summary.optimized_modules;
        }

        output_container.AddModule(std::move(out_entry));
    }

    if (options.module_filter.has_value() && !matched) {
        return Status::NotFound("module filter not matched: " + *options.module_filter);
    }

    auto write_status = output_container.SerializeToFile(options.output_path);
    if (!write_status.ok()) {
        return write_status;
    }

    return summary;
}

StatusOr<OptRunSummary> OptimizeTlbs(const OptCliOptions& options) {
    auto bundle_or = TlbsBundle::Deserialize(options.input_path);
    if (!bundle_or.ok()) {
        return bundle_or.status();
    }

    auto bundle = std::move(bundle_or.value());
    OptRunSummary summary;

    bool matched = false;
    for (const auto& module_path : bundle.manifest().modules) {
        auto it = bundle.modules().find(module_path);
        if (it == bundle.modules().end()) {
            return Status::NotFound("module bytes missing in tlbs: " + module_path);
        }

        DeserializeOptions deserialize_options;
        deserialize_options.verify = !options.trusted;
        auto module_or = Serializer::Deserialize(it->second, deserialize_options);
        if (!module_or.ok()) {
            return module_or.status();
        }

        const bool should_optimize =
            !options.module_filter.has_value() ||
            MatchModuleFilter(*options.module_filter, module_or.value().name(), module_path);
        if (!should_optimize) {
            continue;
        }

        matched = true;
        auto opt_options = options.opt_options;
        opt_options.verify_input = !options.trusted;
        auto optimized_or = OptimizeBytecodeModule(module_or.value(), opt_options);
        if (!optimized_or.ok()) {
            return optimized_or.status();
        }

        auto bytes_or = Serializer::Serialize(optimized_or.value().module, false);
        if (!bytes_or.ok()) {
            return bytes_or.status();
        }

        bundle.SetModule(module_path, std::move(bytes_or.value()));
        MergeStats(&summary.stats, optimized_or.value().stats);
        ++summary.optimized_modules;
    }

    if (options.module_filter.has_value() && !matched) {
        return Status::NotFound("module filter not matched: " + *options.module_filter);
    }

    Status write_status;
    if (std::filesystem::exists(options.output_path) &&
        std::filesystem::is_directory(options.output_path)) {
        write_status = bundle.SerializeToDirectory(options.output_path);
    } else {
        write_status = bundle.SerializeToZip(options.output_path);
    }
    if (!write_status.ok()) {
        return write_status;
    }

    return summary;
}

StatusOr<OptRunSummary> OptimizeInputByExtension(const OptCliOptions& options) {
    const auto ext = options.input_path.extension().string();
    if (ext == ".tbc") {
        return OptimizeTbc(options);
    }
    if (ext == ".tlb") {
        return OptimizeTlb(options);
    }
    if (ext == ".tlbs") {
        return OptimizeTlbs(options);
    }
    return Status::InvalidArgument("unsupported input extension: " + ext);
}

int RunOptFromArgv(int argc, char** argv, int input_index, bool from_tiebc) {
    if (argc <= input_index) {
        return from_tiebc ? PrintTiebcOptUsage() : PrintTievmOptUsage();
    }

    OptCliOptions options;
    options.input_path = std::filesystem::path(argv[input_index]);
    options.opt_options.level = BytecodeOptLevel::kO2;

    for (int i = input_index + 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-o") {
            if (i + 1 >= argc) {
                return from_tiebc ? PrintTiebcOptUsage() : PrintTievmOptUsage();
            }
            options.output_path = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg == "--module") {
            if (i + 1 >= argc) {
                return from_tiebc ? PrintTiebcOptUsage() : PrintTievmOptUsage();
            }
            options.module_filter = std::string(argv[++i]);
            continue;
        }
        if (arg == "--opt") {
            if (i + 1 >= argc) {
                return from_tiebc ? PrintTiebcOptUsage() : PrintTievmOptUsage();
            }
            auto level_or = ParseBytecodeOptLevel(argv[++i]);
            if (!level_or.ok()) {
                std::cerr << "opt: " << level_or.status().message() << "\n";
                return 1;
            }
            if (level_or.value() == BytecodeOptLevel::kO0) {
                std::cerr << "opt: --opt only supports O1/O2/O3\n";
                return 1;
            }
            options.opt_options.level = level_or.value();
            continue;
        }
        if (arg == "--enable-pass") {
            if (i + 1 >= argc) {
                return from_tiebc ? PrintTiebcOptUsage() : PrintTievmOptUsage();
            }
            auto pass_or = ParseBytecodeOptPass(argv[++i]);
            if (!pass_or.ok()) {
                std::cerr << "opt: " << pass_or.status().message() << "\n";
                return 1;
            }
            options.opt_options.enable_passes.push_back(pass_or.value());
            continue;
        }
        if (arg == "--disable-pass") {
            if (i + 1 >= argc) {
                return from_tiebc ? PrintTiebcOptUsage() : PrintTievmOptUsage();
            }
            auto pass_or = ParseBytecodeOptPass(argv[++i]);
            if (!pass_or.ok()) {
                std::cerr << "opt: " << pass_or.status().message() << "\n";
                return 1;
            }
            options.opt_options.disable_passes.push_back(pass_or.value());
            continue;
        }
        if (arg == "--inline-max-inst") {
            if (i + 1 >= argc) {
                return from_tiebc ? PrintTiebcOptUsage() : PrintTievmOptUsage();
            }
            uint32_t parsed = 0;
            if (!ParseUInt32(argv[++i], &parsed) || parsed == 0) {
                std::cerr << "opt: --inline-max-inst must be a positive integer\n";
                return 1;
            }
            options.opt_options.inline_max_inst = parsed;
            continue;
        }
        if (arg == "--trusted") {
            options.trusted = true;
            continue;
        }

        std::cerr << "opt: unknown argument: " << arg << "\n";
        return from_tiebc ? PrintTiebcOptUsage() : PrintTievmOptUsage();
    }

    if (options.output_path.empty()) {
        std::cerr << "opt: missing required -o <output>\n";
        return from_tiebc ? PrintTiebcOptUsage() : PrintTievmOptUsage();
    }

    auto summary_or = OptimizeInputByExtension(options);
    if (!summary_or.ok()) {
        std::cerr << "optimize failed: " << summary_or.status().message() << "\n";
        return 2;
    }

    const auto& summary = summary_or.value();
    std::cout << "opt wrote " << options.output_path.string() << " modules="
              << summary.optimized_modules << " functions="
              << summary.stats.module_function_count << " optimized_functions="
              << summary.stats.optimized_function_count << " rewritten="
              << summary.stats.rewritten_instruction_count << " removed="
              << summary.stats.removed_instruction_count << " inlined="
              << summary.stats.inlined_callsite_count << "\n";
    if (!summary.stats.executed_passes.empty()) {
        std::cout << "opt passes=" << FormatPassList(summary.stats.executed_passes) << "\n";
    }

    return 0;
}

}  // namespace

int OptCmd(int argc, char** argv) { return RunOptFromArgv(argc, argv, 2, true); }

int RunTievmOpt(int argc, char** argv) { return RunOptFromArgv(argc, argv, 1, false); }

}  // namespace tie::vm::cli

