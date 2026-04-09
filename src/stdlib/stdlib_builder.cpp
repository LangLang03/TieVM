#include "tie/vm/stdlib/stdlib_builder.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "tie/vm/bytecode/builder.hpp"
#include "tie/vm/bytecode/serializer.hpp"

namespace tie::vm {

namespace {

struct ExportSpec {
    std::string_view vm_symbol;
    std::string_view native_symbol;
    std::vector<AbiType> params;
    AbiType return_type;
    CallingConvention convention = CallingConvention::kSystem;
};

std::string FunctionNameFromSymbol(std::string_view symbol) {
    const auto pos = symbol.find_last_of('.');
    if (pos == std::string_view::npos || pos + 1 >= symbol.size()) {
        return std::string(symbol);
    }
    return std::string(symbol.substr(pos + 1));
}

std::string NativeStdlibFilename() {
#if defined(_WIN32)
    return "tievm_std_native.dll";
#elif defined(__APPLE__)
    return "libtievm_std_native.dylib";
#else
    return "libtievm_std_native.so";
#endif
}

StatusOr<std::vector<uint8_t>> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return Status::NotFound("file not found: " + path.string());
    }
    return std::vector<uint8_t>(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

StatusOr<std::filesystem::path> ResolveNativeStdlibPath() {
#if defined(_WIN32)
    char* env_path = nullptr;
    size_t env_len = 0;
    if (_dupenv_s(&env_path, &env_len, "TIEVM_STDLIB_NATIVE_PATH") == 0 &&
        env_path != nullptr) {
        const std::filesystem::path path(env_path);
        free(env_path);
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
#else
    if (const char* env_path = std::getenv("TIEVM_STDLIB_NATIVE_PATH"); env_path != nullptr) {
        const std::filesystem::path path(env_path);
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
#endif

    const std::array<std::filesystem::path, 3> candidates = {
        std::filesystem::current_path() / NativeStdlibFilename(),
        std::filesystem::current_path() / "build" / NativeStdlibFilename(),
        std::filesystem::current_path() / ".." / NativeStdlibFilename(),
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate.lexically_normal();
        }
    }
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(
             std::filesystem::current_path(),
             std::filesystem::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().filename() == NativeStdlibFilename()) {
            return entry.path().lexically_normal();
        }
    }
    return Status::NotFound(
        "native stdlib library not found, set TIEVM_STDLIB_NATIVE_PATH");
}

StatusOr<TlbModuleEntry> BuildStdModule(
    const std::string& module_name, const std::vector<ExportSpec>& exports,
    const std::string& library_relative_path) {
    Module module(module_name);
    module.version() = SemanticVersion{0, 2, 0};
    const uint32_t library_index = module.AddFfiLibraryPath(library_relative_path);

    auto& entry_function = module.AddFunction("entry", 2, 0);
    auto& entry_block = entry_function.AddBlock("entry");
    InstructionBuilder(entry_block).Ret(0);

    for (const auto& spec : exports) {
        const auto symbol_idx = module.AddConstant(Constant::Utf8(std::string(spec.vm_symbol)));
        FunctionSignature signature;
        signature.name = std::string(spec.vm_symbol);
        signature.convention = spec.convention;
        signature.return_type = spec.return_type;
        signature.params = spec.params;
        const uint32_t signature_index = module.AddFfiSignature(std::move(signature));

        FfiSymbolBinding binding;
        binding.vm_symbol = std::string(spec.vm_symbol);
        binding.native_symbol = std::string(spec.native_symbol);
        binding.library_index = library_index;
        binding.signature_index = signature_index;
        const uint32_t binding_index = module.AddFfiBinding(std::move(binding));

        auto& fn = module.AddFunction(
            FunctionNameFromSymbol(spec.vm_symbol),
            static_cast<uint16_t>(spec.params.size() + 4),
            static_cast<uint16_t>(spec.params.size()));
        fn.ffi_binding().enabled = true;
        fn.ffi_binding().convention = spec.convention;
        fn.ffi_binding().signature_index = signature_index;
        fn.ffi_binding().binding_index = binding_index;

        auto& bb = fn.AddBlock("entry");
        InstructionBuilder builder(bb);
        for (uint16_t i = 0; i < spec.params.size(); ++i) {
            builder.Mov(static_cast<uint32_t>(i + 1), static_cast<uint32_t>(i));
        }
        builder.FfiCall(0, symbol_idx, static_cast<uint32_t>(spec.params.size())).Ret(0);
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
    tlb_entry.native_plugins = {library_relative_path};
    return tlb_entry;
}

std::vector<ExportSpec> IoExports() {
    return {
        {"tie.std.io.print",
         "tie_std_io_print",
         {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
         {AbiValueKind::kVoid, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.io.read_text",
         "tie_std_io_read_text",
         {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
         {AbiValueKind::kUtf8, OwnershipQualifier::kOwned, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.io.write_text",
         "tie_std_io_write_text",
         {
             {AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
             {AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
         },
         {AbiValueKind::kBool, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
    };
}

std::vector<ExportSpec> CollectionsExports() {
    return {
        {"tie.std.collections.array_new", "tie_std_collections_array_new", {},
         {AbiValueKind::kPointer, OwnershipQualifier::kOwned, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.collections.array_push", "tie_std_collections_array_push",
         {
             {AbiValueKind::kPointer, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
             {AbiValueKind::kI64, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
         },
         {AbiValueKind::kI64, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.collections.array_get", "tie_std_collections_array_get",
         {
             {AbiValueKind::kPointer, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
             {AbiValueKind::kI64, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
         },
         {AbiValueKind::kI64, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.collections.array_size", "tie_std_collections_array_size",
         {{AbiValueKind::kPointer, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
         {AbiValueKind::kI64, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.collections.array_free", "tie_std_collections_array_free",
         {{AbiValueKind::kPointer, OwnershipQualifier::kOwned, FfiPassingMode::kValue, 0, 0}},
         {AbiValueKind::kBool, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.collections.map_new", "tie_std_collections_map_new", {},
         {AbiValueKind::kPointer, OwnershipQualifier::kOwned, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.collections.map_set", "tie_std_collections_map_set",
         {
             {AbiValueKind::kPointer, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
             {AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
             {AbiValueKind::kI64, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
         },
         {AbiValueKind::kBool, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.collections.map_get", "tie_std_collections_map_get",
         {
             {AbiValueKind::kPointer, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
             {AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
         },
         {AbiValueKind::kI64, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.collections.map_has", "tie_std_collections_map_has",
         {
             {AbiValueKind::kPointer, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
             {AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
         },
         {AbiValueKind::kBool, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.collections.map_free", "tie_std_collections_map_free",
         {{AbiValueKind::kPointer, OwnershipQualifier::kOwned, FfiPassingMode::kValue, 0, 0}},
         {AbiValueKind::kBool, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
    };
}

std::vector<ExportSpec> StringExports() {
    return {
        {"tie.std.string.concat", "tie_std_string_concat",
         {
             {AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
             {AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
         },
         {AbiValueKind::kUtf8, OwnershipQualifier::kOwned, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.string.length", "tie_std_string_length",
         {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
         {AbiValueKind::kI64, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.string.utf8_validate", "tie_std_string_utf8_validate",
         {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
         {AbiValueKind::kBool, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.string.codepoints", "tie_std_string_codepoints",
         {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
         {AbiValueKind::kI64, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
        {"tie.std.string.slice", "tie_std_string_slice",
         {
             {AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
             {AbiValueKind::kI64, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
             {AbiValueKind::kI64, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0},
         },
         {AbiValueKind::kUtf8, OwnershipQualifier::kOwned, FfiPassingMode::kValue, 0, 0}},
    };
}

std::vector<ExportSpec> ConcurrentExports() {
    return {
        {"tie.std.concurrent.sleep_ms", "tie_std_concurrent_sleep_ms",
         {{AbiValueKind::kI64, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
         {AbiValueKind::kVoid, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
    };
}

std::vector<ExportSpec> NetExports() {
    return {
        {"tie.std.net.is_ipv4", "tie_std_net_is_ipv4",
         {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
         {AbiValueKind::kBool, OwnershipQualifier::kBorrowed, FfiPassingMode::kValue, 0, 0}},
    };
}

}  // namespace

StatusOr<TlbContainer> BuildStdlibContainer() {
    TlbContainer container;
    const std::string library_relative_path =
        "libs/" + CurrentPlatformName() + "/" + CurrentArchName() + "/" +
        NativeStdlibFilename();
    const std::array<std::pair<std::string, std::vector<ExportSpec>>, 5> modules = {{
        {"tie.std.io", IoExports()},
        {"tie.std.collections", CollectionsExports()},
        {"tie.std.string", StringExports()},
        {"tie.std.concurrent", ConcurrentExports()},
        {"tie.std.net", NetExports()},
    }};

    for (const auto& [name, exports] : modules) {
        auto entry_or = BuildStdModule(name, exports, library_relative_path);
        if (!entry_or.ok()) {
            return entry_or.status();
        }
        container.AddModule(std::move(entry_or.value()));
    }
    return container;
}

StatusOr<TlbsBundle> BuildStdlibBundle() {
    auto container_or = BuildStdlibContainer();
    if (!container_or.ok()) {
        return container_or.status();
    }

    auto native_path_or = ResolveNativeStdlibPath();
    if (!native_path_or.ok()) {
        return native_path_or.status();
    }
    auto native_bytes_or = ReadFileBytes(native_path_or.value());
    if (!native_bytes_or.ok()) {
        return native_bytes_or.status();
    }

    TlbsBundle bundle;
    bundle.manifest().name = "tie.stdlib";
    bundle.manifest().version = SemanticVersion{0, 2, 0};
    bundle.manifest().metadata["format"] = "tlbs";
    bundle.manifest().entry_module = "modules/tie.std.io.tbc";

    const auto lib_relative =
        "libs/" + CurrentPlatformName() + "/" + CurrentArchName() + "/" +
        NativeStdlibFilename();
    bundle.SetLibrary(lib_relative, std::move(native_bytes_or.value()));

    for (const auto& entry : container_or.value().modules()) {
        const std::string module_rel = "modules/" + entry.module_name + ".tbc";
        bundle.manifest().modules.push_back(module_rel);
        bundle.SetModule(module_rel, entry.bytecode);
    }
    return bundle;
}

Status BuildStdlibTlb(const std::filesystem::path& path) {
    auto container_or = BuildStdlibContainer();
    if (!container_or.ok()) {
        return container_or.status();
    }
    return container_or.value().SerializeToFile(path);
}

Status BuildStdlibTlbs(const std::filesystem::path& path) {
    auto bundle_or = BuildStdlibBundle();
    if (!bundle_or.ok()) {
        return bundle_or.status();
    }
    if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
        return bundle_or.value().SerializeToDirectory(path);
    }
    return bundle_or.value().SerializeToZip(path);
}

}  // namespace tie::vm
