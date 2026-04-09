#include "tie/vm/runtime/module_loader.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <unordered_map>

#include "tie/vm/bytecode/serializer.hpp"
#include "tie/vm/runtime/hot_reload_session.hpp"
#include "tie/vm/tlb/tlb_container.hpp"
#include "tie/vm/tlb/tlbs_bundle.hpp"

namespace tie::vm {

Status ModuleLoader::LoadBytecodeModule(Module module) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string name = module.name();
    const auto version = module.version();
    auto it = modules_.find(name);
    if (it != modules_.end()) {
        it->second.version = version;
        it->second.module = std::move(module);
        return Status::Ok();
    }
    modules_.emplace(name, LoadedModule{name, version, std::move(module)});
    return Status::Ok();
}

Status ModuleLoader::LoadTlbFile(const std::filesystem::path& path) {
    auto container_or = TlbContainer::DeserializeFromFile(path);
    if (!container_or.ok()) {
        return container_or.status();
    }
    for (const auto& item : container_or.value().modules()) {
        auto module_or = Serializer::Deserialize(item.bytecode);
        if (!module_or.ok()) {
            return module_or.status();
        }
        auto module = std::move(module_or.value());
        module.version() = item.version;
        auto status = LoadBytecodeModule(std::move(module));
        if (!status.ok()) {
            return status;
        }
    }
    return Status::Ok();
}

Status ModuleLoader::LoadTlbsFile(const std::filesystem::path& path) {
    auto bundle_or = TlbsBundle::Deserialize(path);
    if (!bundle_or.ok()) {
        return bundle_or.status();
    }
    const auto& bundle = bundle_or.value();

    std::filesystem::path root_dir = path;
    if (!std::filesystem::is_directory(path)) {
        const auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_dir = std::filesystem::temp_directory_path() /
                   ("tievm_tlbs_" + std::to_string(static_cast<long long>(timestamp)));
        std::filesystem::create_directories(root_dir);
        auto write_status = bundle.SerializeToDirectory(root_dir);
        if (!write_status.ok()) {
            return write_status;
        }
        std::lock_guard<std::mutex> lock(mu_);
        materialized_bundle_dirs_.push_back(root_dir);
    }

    std::unordered_map<std::string, std::string> symbol_origin;
    for (const auto& module_path : bundle.manifest().modules) {
        auto it = bundle.modules().find(module_path);
        if (it == bundle.modules().end()) {
            return Status::NotFound("module bytes missing in bundle: " + module_path);
        }
        auto module_or = Serializer::Deserialize(it->second);
        if (!module_or.ok()) {
            return module_or.status();
        }
        auto module = std::move(module_or.value());
        for (auto& library_path : module.ffi_library_paths()) {
            std::filesystem::path lib(library_path);
            if (!lib.is_absolute()) {
                lib = root_dir / lib;
            }
            library_path = lib.lexically_normal().string();
            if (!std::filesystem::exists(library_path)) {
                return Status::NotFound("ffi library not found: " + library_path);
            }
        }
        for (const auto& binding : module.ffi_bindings()) {
            if (binding.library_index >= module.ffi_library_paths().size()) {
                return Status::InvalidArgument("ffi binding library index out of range");
            }
            const std::string origin = module.ffi_library_paths()[binding.library_index] + "::" +
                                       binding.native_symbol;
            auto it_symbol = symbol_origin.find(binding.vm_symbol);
            if (it_symbol != symbol_origin.end() && it_symbol->second != origin) {
                return Status::InvalidArgument(
                    "ffi symbol conflict in tlbs: " + binding.vm_symbol);
            }
            symbol_origin.insert_or_assign(binding.vm_symbol, origin);
        }
        auto status = LoadBytecodeModule(std::move(module));
        if (!status.ok()) {
            return status;
        }
    }
    return Status::Ok();
}

StatusOr<Module> ModuleLoader::GetModule(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = modules_.find(name);
    if (it == modules_.end()) {
        return Status::NotFound("module not loaded: " + name);
    }
    return it->second.module;
}

std::vector<std::string> ModuleLoader::ActiveModuleNames() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    out.reserve(modules_.size());
    for (const auto& [name, _] : modules_) {
        out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

HotReloadSession ModuleLoader::BeginHotReload() { return HotReloadSession(this); }

Status ModuleLoader::CommitHotReload(std::vector<LoadedModule> replacements) {
    std::lock_guard<std::mutex> lock(mu_);
    std::unordered_map<std::string, SemanticVersion> staged;
    for (const auto& module : replacements) {
        auto it = staged.find(module.name);
        if (it != staged.end()) {
            if (it->second.major != module.version.major ||
                it->second.minor != module.version.minor ||
                it->second.patch != module.version.patch) {
                return Status::InvalidArgument(
                    "hot reload conflict: same module name with different versions");
            }
            continue;
        }
        staged.emplace(module.name, module.version);
    }
    for (auto& module : replacements) {
        modules_.insert_or_assign(module.name, std::move(module));
    }
    return Status::Ok();
}

}  // namespace tie::vm
