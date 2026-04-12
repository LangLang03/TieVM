#include "tie/vm/runtime/module_loader.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>

#include "tie/vm/runtime/hot_reload_session.hpp"
#include "tie/vm/tlb/tlb_container.hpp"
#include "tie/vm/tlb/tlbs_bundle.hpp"

namespace tie::vm {

namespace {

constexpr uint64_t kFNV1aOffset = 14695981039346656037ULL;
constexpr uint64_t kFNV1aPrime = 1099511628211ULL;

std::string HexU64(uint64_t value) {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << value;
    return oss.str();
}

StatusOr<std::string> HashFileFNV1a(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return Status::NotFound("failed opening tlbs for hashing: " + path.string());
    }
    uint64_t hash = kFNV1aOffset;
    char buffer[64 * 1024];
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
        const auto n = static_cast<size_t>(in.gcount());
        for (size_t i = 0; i < n; ++i) {
            hash ^= static_cast<uint8_t>(buffer[i]);
            hash *= kFNV1aPrime;
        }
    }
    if (!in.eof()) {
        return Status::SerializationError("failed reading tlbs for hashing");
    }
    return HexU64(hash);
}

std::filesystem::path DefaultTlbsCacheRoot() {
#if defined(_WIN32)
    if (const char* localappdata = std::getenv("LOCALAPPDATA"); localappdata != nullptr) {
        return std::filesystem::path(localappdata) / "tievm" / "tlbs";
    }
#endif
    if (const char* xdg_cache = std::getenv("XDG_CACHE_HOME"); xdg_cache != nullptr) {
        return std::filesystem::path(xdg_cache) / "tievm" / "tlbs";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        return std::filesystem::path(home) / ".cache" / "tievm" / "tlbs";
    }
    return std::filesystem::temp_directory_path() / "tievm_cache" / "tlbs";
}

StatusOr<std::filesystem::path> MaterializeBundleToTemp(const TlbsBundle& bundle) {
    const auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto root =
        std::filesystem::temp_directory_path() /
        ("tievm_tlbs_" + std::to_string(static_cast<long long>(timestamp)));
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        return Status::SerializationError(
            "failed creating temporary tlbs directory: " + ec.message());
    }
    auto status = bundle.SerializeToDirectory(root);
    if (!status.ok()) {
        return status;
    }
    return root;
}

bool HasInvalidRelativeSegments(const std::filesystem::path& path) {
    for (const auto& part : path) {
        const auto token = part.string();
        if (token.empty() || token == "." || token == "..") {
            return true;
        }
    }
    return false;
}

StatusOr<std::filesystem::path> CanonicalForCompare(const std::filesystem::path& path) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        return Status::SerializationError(
            "failed canonicalizing path: " + path.string());
    }
    return canonical.lexically_normal();
}

bool IsWithinRoot(
    const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto root_str = root.generic_string();
    const auto candidate_str = candidate.generic_string();
    if (candidate_str == root_str) {
        return true;
    }
    const std::string root_prefix = root_str + "/";
    return candidate_str.rfind(root_prefix, 0) == 0;
}

Status EnsureMaterializedBundleFiles(
    const TlbsBundle& bundle, const std::filesystem::path& root_dir) {
    bool missing = !std::filesystem::exists(root_dir / "manifest.toml");
    if (!missing) {
        for (const auto& module_path : bundle.manifest().modules) {
            if (!std::filesystem::exists(root_dir / module_path)) {
                missing = true;
                break;
            }
        }
    }
    if (!missing) {
        for (const auto& [lib_path, _] : bundle.libraries()) {
            if (!std::filesystem::exists(root_dir / lib_path)) {
                missing = true;
                break;
            }
        }
    }
    if (!missing) {
        return Status::Ok();
    }
    return bundle.SerializeToDirectory(root_dir);
}

StatusOr<std::filesystem::path> MaterializeBundleToCache(
    const TlbsBundle& bundle, const std::filesystem::path& source_zip,
    const std::filesystem::path& cache_root) {
    auto hash_or = HashFileFNV1a(source_zip);
    if (!hash_or.ok()) {
        return hash_or.status();
    }
    const auto cache_base =
        cache_root / CurrentPlatformName() / CurrentArchName() / hash_or.value();
    const auto manifest = cache_base / "manifest.toml";
    if (std::filesystem::exists(manifest)) {
        auto status = EnsureMaterializedBundleFiles(bundle, cache_base);
        if (status.ok()) {
            return cache_base;
        }
        std::error_code clear_ec;
        std::filesystem::remove_all(cache_base, clear_ec);
    }
    if (!std::filesystem::exists(manifest) && std::filesystem::exists(cache_base)) {
        std::error_code clear_ec;
        std::filesystem::remove_all(cache_base, clear_ec);
    }

    std::error_code ec;
    std::filesystem::create_directories(cache_base.parent_path(), ec);
    if (ec) {
        return Status::SerializationError(
            "failed creating tlbs cache directory: " + ec.message());
    }

    const auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto staging =
        cache_base.parent_path() /
        (cache_base.filename().string() + ".tmp." +
         std::to_string(static_cast<long long>(timestamp)));
    std::filesystem::remove_all(staging, ec);
    ec.clear();

    auto status = bundle.SerializeToDirectory(staging);
    if (!status.ok()) {
        std::filesystem::remove_all(staging, ec);
        return status;
    }

    std::filesystem::rename(staging, cache_base, ec);
    if (ec) {
        if (std::filesystem::exists(manifest)) {
            std::filesystem::remove_all(staging, ec);
            return cache_base;
        }
        std::filesystem::remove_all(staging, ec);
        return Status::SerializationError(
            "failed finalizing tlbs cache directory: " + ec.message());
    }
    return cache_base;
}

}  // namespace

ModuleLoader::~ModuleLoader() {
    std::vector<std::filesystem::path> dirs;
    {
        std::lock_guard<std::mutex> lock(mu_);
        dirs = std::move(temp_materialized_dirs_);
    }
    std::error_code ec;
    for (const auto& dir : dirs) {
        std::filesystem::remove_all(dir, ec);
        ec.clear();
    }
}

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
    return LoadTlbFile(path, TlbLoadOptions{});
}

Status ModuleLoader::LoadTlbFile(
    const std::filesystem::path& path, TlbLoadOptions options) {
    auto container_or = TlbContainer::DeserializeFromFile(path);
    if (!container_or.ok()) {
        return container_or.status();
    }
    for (const auto& item : container_or.value().modules()) {
        auto module_or = Serializer::Deserialize(item.bytecode, options.deserialize_options);
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
    return LoadTlbsFile(path, TlbsLoadOptions{});
}

Status ModuleLoader::LoadTlbsFile(
    const std::filesystem::path& path, TlbsLoadOptions options) {
    auto bundle_or = TlbsBundle::Deserialize(path);
    if (!bundle_or.ok()) {
        return bundle_or.status();
    }
    const auto& bundle = bundle_or.value();
    const auto& manifest = bundle.manifest();

    std::filesystem::path root_dir = path;
    bool root_dir_is_temporary = false;
    if (!std::filesystem::is_directory(path)) {
        if (options.enable_cache) {
            const auto cache_root = options.cache_dir.value_or(DefaultTlbsCacheRoot());
            auto cache_or = MaterializeBundleToCache(bundle, path, cache_root);
            if (cache_or.ok()) {
                root_dir = std::move(cache_or.value());
            } else {
                auto temp_or = MaterializeBundleToTemp(bundle);
                if (!temp_or.ok()) {
                    return temp_or.status();
                }
                root_dir = std::move(temp_or.value());
                root_dir_is_temporary = true;
            }
        } else {
            auto temp_or = MaterializeBundleToTemp(bundle);
            if (!temp_or.ok()) {
                return temp_or.status();
            }
            root_dir = std::move(temp_or.value());
            root_dir_is_temporary = true;
        }
        if (root_dir_is_temporary) {
            std::lock_guard<std::mutex> lock(mu_);
            temp_materialized_dirs_.push_back(root_dir);
        }
    }

    auto root_canonical_or = CanonicalForCompare(root_dir);
    if (!root_canonical_or.ok()) {
        return root_canonical_or.status();
    }
    const auto root_canonical = std::move(root_canonical_or.value());

    std::unordered_map<std::string, std::string> symbol_origin;
    bool entry_module_loaded = !manifest.entry_module.has_value();
    for (const auto& module_path : manifest.modules) {
        auto it = bundle.modules().find(module_path);
        if (it == bundle.modules().end()) {
            return Status::NotFound("module bytes missing in bundle: " + module_path);
        }
        auto module_or = Serializer::Deserialize(it->second, options.deserialize_options);
        if (!module_or.ok()) {
            return module_or.status();
        }
        auto module = std::move(module_or.value());
        for (auto& library_path : module.ffi_library_paths()) {
            std::filesystem::path lib(library_path);
            if (lib.has_root_name() || lib.has_root_directory() || lib.is_absolute()) {
                return Status::InvalidArgument(
                    "ffi library path in tlbs must be relative: " + library_path);
            }
            lib = lib.lexically_normal();
            if (lib.empty() || HasInvalidRelativeSegments(lib)) {
                return Status::InvalidArgument(
                    "ffi library path contains invalid segment: " + library_path);
            }
            const auto lib_rel = lib.generic_string();
            if (lib_rel.rfind("libs/", 0) != 0) {
                return Status::InvalidArgument(
                    "ffi library path must stay under libs/: " + library_path);
            }
            const auto resolved = (root_dir / lib).lexically_normal();
            auto resolved_canonical_or = CanonicalForCompare(resolved);
            if (!resolved_canonical_or.ok()) {
                return resolved_canonical_or.status();
            }
            if (!IsWithinRoot(root_canonical, resolved_canonical_or.value())) {
                return Status::InvalidArgument(
                    "ffi library path escapes tlbs root: " + library_path);
            }
            if (!std::filesystem::exists(resolved_canonical_or.value())) {
                return Status::NotFound(
                    "ffi library not found: " +
                    resolved_canonical_or.value().string());
            }
            library_path = resolved_canonical_or.value().string();
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
        if (manifest.entry_module.has_value() && module_path == *manifest.entry_module) {
            entry_module_loaded = true;
        }
    }
    if (!entry_module_loaded) {
        return Status::NotFound("entry module missing from tlbs manifest");
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

StatusOr<const Module*> ModuleLoader::GetModulePtr(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = modules_.find(name);
    if (it == modules_.end()) {
        return Status::NotFound("module not loaded: " + name);
    }
    return &it->second.module;
}

std::optional<Module> ModuleLoader::FindModuleByFfiSymbol(std::string_view symbol) const {
    const Module* ptr = FindModuleByFfiSymbolPtr(symbol);
    if (ptr == nullptr) {
        return std::nullopt;
    }
    return *ptr;
}

const Module* ModuleLoader::FindModuleByFfiSymbolPtr(std::string_view symbol) const {
    std::lock_guard<std::mutex> lock(mu_);
    const LoadedModule* best = nullptr;
    for (const auto& [name, loaded] : modules_) {
        bool matched = false;
        for (const auto& binding : loaded.module.ffi_bindings()) {
            if (binding.vm_symbol == symbol) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        if (best == nullptr || name < best->name) {
            best = &loaded;
        }
    }
    if (best == nullptr) {
        return nullptr;
    }
    return &best->module;
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
