#include "tie/vm/runtime/module_loader.hpp"

#include <algorithm>

#include "tie/vm/bytecode/serializer.hpp"
#include "tie/vm/runtime/hot_reload_session.hpp"
#include "tie/vm/tlb/tlb_container.hpp"

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
