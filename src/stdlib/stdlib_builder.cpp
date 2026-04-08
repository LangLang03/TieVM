#include "tie/vm/stdlib/stdlib_builder.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "tie/vm/bytecode/builder.hpp"
#include "tie/vm/bytecode/serializer.hpp"

namespace tie::vm {

namespace {

struct ExportSpec {
    std::string_view symbol;
    uint16_t arg_count;
};

std::string FunctionNameFromSymbol(std::string_view symbol) {
    const auto pos = symbol.find_last_of('.');
    if (pos == std::string_view::npos || pos + 1 >= symbol.size()) {
        return std::string(symbol);
    }
    return std::string(symbol.substr(pos + 1));
}

StatusOr<TlbModuleEntry> BuildStdModule(
    const std::string& module_name, const std::vector<ExportSpec>& exports) {
    Module module(module_name);
    module.version() = SemanticVersion{0, 1, 0};
    auto& entry_function = module.AddFunction("entry", /*reg_count=*/2, /*param_count=*/0);
    auto& entry_block = entry_function.AddBlock("entry");
    InstructionBuilder(entry_block).Ret(0);

    for (const auto& spec : exports) {
        const auto symbol_idx = module.AddConstant(Constant::Utf8(std::string(spec.symbol)));
        auto& fn = module.AddFunction(
            FunctionNameFromSymbol(spec.symbol),
            static_cast<uint16_t>(spec.arg_count + 4),
            spec.arg_count);
        auto& bb = fn.AddBlock("entry");
        InstructionBuilder builder(bb);
        for (uint16_t i = 0; i < spec.arg_count; ++i) {
            builder.Mov(static_cast<uint32_t>(i + 1), static_cast<uint32_t>(i));
        }
        builder.FfiCall(0, symbol_idx, spec.arg_count).Ret(0);
    }

    module.set_entry_function(0);
    auto bytes_or = Serializer::Serialize(module, false);
    if (!bytes_or.ok()) {
        return bytes_or.status();
    }
    TlbModuleEntry tlb_entry;
    tlb_entry.module_name = module_name;
    tlb_entry.version = module.version();
    tlb_entry.bytecode = std::move(bytes_or.value());
    return tlb_entry;
}

}  // namespace

StatusOr<TlbContainer> BuildStdlibContainer() {
    TlbContainer container;
    const std::array<std::pair<std::string, std::vector<ExportSpec>>, 5> modules = {{
        {"tie.std.io",
         {
             {"tie.std.io.print", 1},
             {"tie.std.io.read_text", 1},
             {"tie.std.io.write_text", 2},
         }},
        {"tie.std.collections",
         {
             {"tie.std.collections.array_new", 0},
             {"tie.std.collections.array_push", 2},
             {"tie.std.collections.array_get", 2},
             {"tie.std.collections.array_size", 1},
             {"tie.std.collections.map_new", 0},
             {"tie.std.collections.map_set", 3},
             {"tie.std.collections.map_get", 2},
             {"tie.std.collections.map_has", 2},
         }},
        {"tie.std.string",
         {
             {"tie.std.string.concat", 2},
             {"tie.std.string.length", 1},
             {"tie.std.string.utf8_validate", 1},
             {"tie.std.string.codepoints", 1},
             {"tie.std.string.slice", 3},
         }},
        {"tie.std.concurrent",
         {
             {"tie.std.concurrent.sleep_ms", 1},
         }},
        {"tie.std.net",
         {
             {"tie.std.net.is_ipv4", 1},
         }},
    }};

    for (const auto& [name, exports] : modules) {
        auto entry_or = BuildStdModule(name, exports);
        if (!entry_or.ok()) {
            return entry_or.status();
        }
        container.AddModule(std::move(entry_or.value()));
    }
    return container;
}

Status BuildStdlibTlb(const std::filesystem::path& path) {
    auto container_or = BuildStdlibContainer();
    if (!container_or.ok()) {
        return container_or.status();
    }
    return container_or.value().SerializeToFile(path);
}

}  // namespace tie::vm
