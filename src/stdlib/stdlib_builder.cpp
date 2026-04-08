#include "tie/vm/stdlib/stdlib_builder.hpp"

#include "tie/vm/bytecode/builder.hpp"
#include "tie/vm/bytecode/serializer.hpp"

namespace tie::vm {

namespace {

StatusOr<TlbModuleEntry> BuildStubStdModule(const std::string& module_name, int64_t marker) {
    Module module(module_name);
    module.version() = SemanticVersion{0, 1, 0};
    auto c_idx = module.AddConstant(Constant::Int64(marker));
    auto& fn = module.AddFunction("entry", /*reg_count=*/4, /*param_count=*/0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).LoadK(0, c_idx).Ret(0);
    module.set_entry_function(0);
    auto bytes_or = Serializer::Serialize(module, false);
    if (!bytes_or.ok()) {
        return bytes_or.status();
    }
    TlbModuleEntry entry;
    entry.module_name = module_name;
    entry.version = module.version();
    entry.bytecode = std::move(bytes_or.value());
    return entry;
}

}  // namespace

StatusOr<TlbContainer> BuildStdlibContainer() {
    TlbContainer container;
    constexpr struct {
        const char* name;
        int64_t marker;
    } kModules[] = {
        {"tie.std.io", 1},
        {"tie.std.collections", 2},
        {"tie.std.string", 3},
        {"tie.std.concurrent", 4},
        {"tie.std.net", 5},
    };

    for (const auto& spec : kModules) {
        auto entry_or = BuildStubStdModule(spec.name, spec.marker);
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

