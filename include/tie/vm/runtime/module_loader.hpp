#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tie/vm/bytecode/module.hpp"
#include "tie/vm/bytecode/serializer.hpp"
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
    struct TlbLoadOptions {
        DeserializeOptions deserialize_options{};
    };

    struct TlbsLoadOptions {
        DeserializeOptions deserialize_options{};
        bool enable_cache = true;
        std::optional<std::filesystem::path> cache_dir;
    };

    ModuleLoader() = default;
    ~ModuleLoader();

    Status LoadBytecodeModule(Module module);
    Status LoadTlbFile(const std::filesystem::path& path);
    Status LoadTlbFile(const std::filesystem::path& path, TlbLoadOptions options);
    Status LoadTlbsFile(const std::filesystem::path& path);
    Status LoadTlbsFile(const std::filesystem::path& path, TlbsLoadOptions options);

    [[nodiscard]] StatusOr<Module> GetModule(const std::string& name) const;
    [[nodiscard]] StatusOr<const Module*> GetModulePtr(const std::string& name) const;
    [[nodiscard]] std::optional<Module> FindModuleByFfiSymbol(std::string_view symbol) const;
    [[nodiscard]] const Module* FindModuleByFfiSymbolPtr(std::string_view symbol) const;
    [[nodiscard]] std::vector<std::string> ActiveModuleNames() const;

    [[nodiscard]] HotReloadSession BeginHotReload();

    Status CommitHotReload(std::vector<LoadedModule> replacements);

  private:
    friend class HotReloadSession;

    mutable std::mutex mu_;
    std::unordered_map<std::string, LoadedModule> modules_;
    std::vector<std::filesystem::path> temp_materialized_dirs_;
};

}  // namespace tie::vm
