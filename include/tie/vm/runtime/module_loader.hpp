#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "tie/vm/bytecode/module.hpp"
#include "tie/vm/common/status.hpp"

namespace tie::vm {

class HotReloadSession;

struct LoadedModule {
    std::string name;
    SemanticVersion version;
    Module module;

    LoadedModule(std::string n, SemanticVersion v, Module m)
        : name(std::move(n)), version(v), module(std::move(m)) {}
};

class ModuleLoader {
  public:
    ModuleLoader() = default;

    Status LoadBytecodeModule(Module module);
    Status LoadTlbFile(const std::filesystem::path& path);
    Status LoadTlbsFile(const std::filesystem::path& path);

    [[nodiscard]] StatusOr<Module> GetModule(const std::string& name) const;
    [[nodiscard]] std::vector<std::string> ActiveModuleNames() const;

    [[nodiscard]] HotReloadSession BeginHotReload();

    Status CommitHotReload(std::vector<LoadedModule> replacements);

  private:
    friend class HotReloadSession;

    mutable std::mutex mu_;
    std::unordered_map<std::string, LoadedModule> modules_;
    std::vector<std::filesystem::path> materialized_bundle_dirs_;
};

}  // namespace tie::vm
