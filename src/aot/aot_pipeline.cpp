#include "tie/vm/aot/aot_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "tie/vm/bytecode/opcode.hpp"
#include "tie/vm/bytecode/serializer.hpp"
#include "tie/vm/bytecode/verifier.hpp"
#include "tie/vm/tlb/tlb_container.hpp"
#include "tie/vm/tlb/tlbs_bundle.hpp"

namespace tie::vm {

namespace {

struct LoadedAotModule {
    Module module{"aot.invalid"};
    std::string module_name;
    std::filesystem::path input_path;
    std::filesystem::path base_dir;
    std::vector<std::filesystem::path> linked_libraries;
    std::optional<std::filesystem::path> temp_materialized_root;
};

struct ExportedFunctionInfo {
    uint32_t function_index = 0;
    std::string name;
    uint16_t param_count = 0;
    bool is_vararg = false;
    std::vector<BytecodeValueType> param_types;
    BytecodeValueType return_type = BytecodeValueType::kAny;
};

std::vector<ExportedFunctionInfo> CollectExportedFunctions(const Module& module) {
    std::vector<ExportedFunctionInfo> out;
    out.reserve(module.functions().size());
    for (size_t i = 0; i < module.functions().size(); ++i) {
        const auto& function = module.functions()[i];
        if (!function.is_exported()) {
            continue;
        }
        out.push_back(ExportedFunctionInfo{
            static_cast<uint32_t>(i),
            function.name(),
            function.param_count(),
            function.is_vararg(),
            function.param_types(),
            function.return_type()});
    }
    return out;
}

bool IsWindowsTarget(const AotCompileOptions& options) {
    if (options.target_triple.has_value()) {
        auto triple = *options.target_triple;
        std::transform(
            triple.begin(), triple.end(), triple.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (triple.find("windows") != std::string::npos ||
            triple.find("mingw") != std::string::npos ||
            triple.find("msvc") != std::string::npos) {
            return true;
        }
        return false;
    }
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

int32_t SignedU32(uint32_t value) { return static_cast<int32_t>(value); }
constexpr uint32_t kInvalidTryTarget = 0xFFFFFFFFu;

std::string BuildAotError(
    std::string_view code, std::string_view message, std::string_view module = {},
    std::string_view function = {}, std::optional<size_t> pc = std::nullopt,
    std::optional<OpCode> opcode = std::nullopt) {
    std::ostringstream oss;
    oss << "[AOT] " << code;
    if (!module.empty()) {
        oss << " module=" << module;
    }
    if (!function.empty()) {
        oss << " function=" << function;
    }
    if (pc.has_value()) {
        oss << " pc=" << *pc;
    }
    if (opcode.has_value()) {
        oss << " opcode=" << OpCodeName(*opcode);
    }
    oss << " msg=" << message;
    return oss.str();
}

Status AotInvalid(
    std::string_view code, std::string_view message, std::string_view module = {},
    std::string_view function = {}, std::optional<size_t> pc = std::nullopt,
    std::optional<OpCode> opcode = std::nullopt) {
    return Status::InvalidState(BuildAotError(code, message, module, function, pc, opcode));
}

Status AotUnsupported(
    std::string_view message, std::string_view module, std::string_view function, size_t pc,
    OpCode opcode) {
    return Status::Unsupported(
        BuildAotError("LOWER_UNSUPPORTED", message, module, function, pc, opcode));
}

std::string QuoteShellArg(const std::string& raw) {
#if defined(_WIN32)
    std::string escaped = "\"";
    for (char c : raw) {
        if (c == '"') {
            escaped += "\\\"";
        } else {
            escaped.push_back(c);
        }
    }
    escaped += '"';
    return escaped;
#else
    std::string escaped = "'";
    for (char c : raw) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(c);
        }
    }
    escaped += '\'';
    return escaped;
#endif
}

Status RunCommand(const std::vector<std::string>& args) {
    if (args.empty()) {
        return Status::InvalidArgument("[AOT] TOOLCHAIN msg=empty command");
    }
    std::string cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            cmd.push_back(' ');
        }
        cmd += QuoteShellArg(args[i]);
    }
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        return Status::RuntimeError(
            "[AOT] TOOLCHAIN msg=command failed rc=" + std::to_string(rc) + " cmd=" + cmd);
    }
    return Status::Ok();
}

std::string EscapeLlvmQuotedIdent(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string EscapeLlvmCString(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2 + 8);
    for (unsigned char c : bytes) {
        const bool printable = c >= 32 && c <= 126 && c != '\\' && c != '"';
        if (printable) {
            out.push_back(static_cast<char>(c));
            continue;
        }
        out.push_back('\\');
        out.push_back(kHex[(c >> 4u) & 0x0Fu]);
        out.push_back(kHex[c & 0x0Fu]);
    }
    out += "\\00";
    return out;
}

uint64_t DoubleBits(double value) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::filesystem::path MakeTempDirPath(std::string_view suffix) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("tievm_aot_" + std::to_string(static_cast<long long>(now)) + "_" +
            std::string(suffix));
}

StatusOr<std::filesystem::path> MaterializeTlbsZip(const TlbsBundle& bundle) {
    auto root = MakeTempDirPath("tlbs");
    auto status = bundle.SerializeToDirectory(root);
    if (!status.ok()) {
        return status;
    }
    return root;
}

Status ResolveFfiLibraries(LoadedAotModule* loaded) {
    std::set<std::filesystem::path> dedup;
    for (auto& lib : loaded->module.ffi_library_paths()) {
        std::filesystem::path path(lib);
        if (!path.is_absolute() && !path.has_root_directory() && !path.has_root_name()) {
            path = loaded->base_dir / path;
        }
        std::error_code ec;
        auto normalized = std::filesystem::weakly_canonical(path, ec);
        if (ec) {
            normalized = path.lexically_normal();
        }
        if (!std::filesystem::exists(normalized)) {
            return AotInvalid(
                "FFI_LIBRARY_NOT_FOUND",
                "ffi library not found: " + normalized.string(),
                loaded->module_name);
        }
        lib = normalized.string();
        dedup.insert(normalized);
    }
    loaded->linked_libraries.assign(dedup.begin(), dedup.end());
    return Status::Ok();
}

bool MatchModuleOverride(
    const std::string& override_name, const std::string& module_name,
    const std::string& package_name) {
    if (override_name == module_name || override_name == package_name) {
        return true;
    }
    const auto stem = std::filesystem::path(package_name).stem().string();
    return override_name == stem;
}

StatusOr<LoadedAotModule> LoadFromTbc(const AotCompileOptions& options) {
    DeserializeOptions deserialize_options;
    deserialize_options.verify = options.verify;
    auto module_or = Serializer::DeserializeFromFile(options.input_path, deserialize_options);
    if (!module_or.ok()) {
        return module_or.status();
    }

    LoadedAotModule loaded;
    loaded.module = std::move(module_or.value());
    loaded.module_name = loaded.module.name();
    loaded.input_path = options.input_path;
    loaded.base_dir = options.input_path.parent_path();
    if (options.module_name_override.has_value()) {
        // .tbc always compiles its own module; override is accepted but ignored.
    }
    return loaded;
}

StatusOr<LoadedAotModule> LoadFromTlb(const AotCompileOptions& options) {
    auto container_or = TlbContainer::DeserializeFromFile(options.input_path);
    if (!container_or.ok()) {
        return container_or.status();
    }

    const auto& modules = container_or.value().modules();
    if (modules.empty()) {
        return AotInvalid("INPUT_EMPTY", "tlb has no modules");
    }

    DeserializeOptions deserialize_options;
    deserialize_options.verify = options.verify;

    std::optional<TlbModuleEntry> selected_entry;
    Module selected_module("invalid");

    for (const auto& entry : modules) {
        auto module_or = Serializer::Deserialize(entry.bytecode, deserialize_options);
        if (!module_or.ok()) {
            return module_or.status();
        }
        auto module = std::move(module_or.value());
        module.version() = entry.version;

        const bool selected =
            !options.module_name_override.has_value() ? (&entry == &modules.front())
                                                      : MatchModuleOverride(
                                                            *options.module_name_override,
                                                            module.name(),
                                                            entry.module_name);
        if (selected) {
            selected_entry = entry;
            selected_module = std::move(module);
            break;
        }
    }

    if (!selected_entry.has_value()) {
        return AotInvalid(
            "MODULE_NOT_FOUND",
            "module override not found in tlb: " + *options.module_name_override);
    }

    LoadedAotModule loaded;
    loaded.module = std::move(selected_module);
    loaded.module_name = loaded.module.name();
    loaded.input_path = options.input_path;
    loaded.base_dir = options.input_path.parent_path();
    return loaded;
}

StatusOr<LoadedAotModule> LoadFromTlbs(const AotCompileOptions& options) {
    auto bundle_or = TlbsBundle::Deserialize(options.input_path);
    if (!bundle_or.ok()) {
        return bundle_or.status();
    }
    const auto& bundle = bundle_or.value();
    const auto& manifest = bundle.manifest();

    if (manifest.modules.empty()) {
        return AotInvalid("INPUT_EMPTY", "tlbs manifest has no modules");
    }

    std::filesystem::path root_dir = options.input_path;
    std::optional<std::filesystem::path> temp_root;
    if (!std::filesystem::is_directory(options.input_path)) {
        auto root_or = MaterializeTlbsZip(bundle);
        if (!root_or.ok()) {
            return root_or.status();
        }
        root_dir = root_or.value();
        temp_root = root_dir;
    }

    std::string selected_module_path;
    if (options.module_name_override.has_value()) {
        const auto& override_name = *options.module_name_override;
        for (const auto& path : manifest.modules) {
            if (override_name == path || override_name == std::filesystem::path(path).stem().string()) {
                selected_module_path = path;
                break;
            }
        }
        if (selected_module_path.empty()) {
            DeserializeOptions deserialize_options;
            deserialize_options.verify = options.verify;
            for (const auto& path : manifest.modules) {
                auto module_it = bundle.modules().find(path);
                if (module_it == bundle.modules().end()) {
                    continue;
                }
                auto module_or = Serializer::Deserialize(module_it->second, deserialize_options);
                if (!module_or.ok()) {
                    return module_or.status();
                }
                if (module_or.value().name() == override_name) {
                    selected_module_path = path;
                    break;
                }
            }
        }
        if (selected_module_path.empty()) {
            return AotInvalid(
                "MODULE_NOT_FOUND",
                "module override not found in tlbs: " + override_name);
        }
    } else if (manifest.entry_module.has_value()) {
        selected_module_path = *manifest.entry_module;
    } else {
        selected_module_path = manifest.modules.front();
    }

    auto bytes_it = bundle.modules().find(selected_module_path);
    if (bytes_it == bundle.modules().end()) {
        return AotInvalid(
            "MODULE_NOT_FOUND",
            "selected module bytes missing from tlbs: " + selected_module_path);
    }

    DeserializeOptions deserialize_options;
    deserialize_options.verify = options.verify;
    auto module_or = Serializer::Deserialize(bytes_it->second, deserialize_options);
    if (!module_or.ok()) {
        return module_or.status();
    }

    LoadedAotModule loaded;
    loaded.module = std::move(module_or.value());
    loaded.module_name = loaded.module.name();
    loaded.input_path = options.input_path;
    loaded.base_dir = root_dir;
    loaded.temp_materialized_root = temp_root;
    return loaded;
}

StatusOr<LoadedAotModule> LoadInputModule(const AotCompileOptions& options) {
    const auto ext = options.input_path.extension().string();
    if (ext == ".tbc") {
        return LoadFromTbc(options);
    }
    if (ext == ".tlb") {
        return LoadFromTlb(options);
    }
    if (ext == ".tlbs") {
        return LoadFromTlbs(options);
    }
    return AotInvalid(
        "UNSUPPORTED_INPUT",
        "unsupported input extension: " + ext);
}

struct FfiLoweredInfo {
    const FfiSymbolBinding* binding = nullptr;
    const FunctionSignature* signature = nullptr;
    std::string native_ir_name;
    std::vector<std::string> llvm_param_types;
    std::string llvm_return_type;
};

struct AotClassMethodInfo {
    uint32_t function_index = 0;
    BytecodeAccessModifier access = BytecodeAccessModifier::kPublic;
};

struct AotClassInfo {
    std::string name;
    std::vector<uint32_t> base_indices;
    std::vector<uint32_t> mro;
    std::unordered_map<std::string, AotClassMethodInfo> methods;
};

enum class InvokeResolveKind : uint8_t {
    kFound = 0,
    kPrivate = 1,
    kMissing = 2,
};

struct InvokeResolveResult {
    InvokeResolveKind kind = InvokeResolveKind::kMissing;
    uint32_t function_index = 0;
};

StatusOr<std::string> AbiTypeToLlvm(const AbiType& type, bool is_return) {
    switch (type.kind) {
        case AbiValueKind::kVoid:
            if (!is_return) {
                return AotInvalid("FFI_SIGNATURE", "void parameter type is invalid");
            }
            return std::string("void");
        case AbiValueKind::kF32:
            return std::string("float");
        case AbiValueKind::kF64:
            return std::string("double");
        case AbiValueKind::kStruct:
            if (type.passing == FfiPassingMode::kValue) {
                if (type.size == 1) {
                    return std::string("i8");
                }
                if (type.size == 2) {
                    return std::string("i16");
                }
                if (type.size == 4) {
                    return std::string("i32");
                }
                if (type.size == 8) {
                    return std::string("i64");
                }
                return AotInvalid(
                    "FFI_SIGNATURE",
                    "struct by-value size unsupported (>8 bytes)");
            }
            return std::string("i64");
        case AbiValueKind::kI8:
        case AbiValueKind::kU8:
        case AbiValueKind::kI16:
        case AbiValueKind::kU16:
        case AbiValueKind::kI32:
        case AbiValueKind::kU32:
        case AbiValueKind::kI64:
        case AbiValueKind::kU64:
        case AbiValueKind::kBool:
        case AbiValueKind::kPointer:
        case AbiValueKind::kUtf8:
        case AbiValueKind::kObject:
            return std::string("i64");
    }
    return AotInvalid("FFI_SIGNATURE", "unknown abi value kind");
}

StatusOr<std::optional<uint8_t>> BytecodeTypeToAotTag(BytecodeValueType type) {
    switch (type) {
        case BytecodeValueType::kAny:
            return std::optional<uint8_t>{};
        case BytecodeValueType::kNull:
            return std::optional<uint8_t>{static_cast<uint8_t>(0)};
        case BytecodeValueType::kInt64:
            return std::optional<uint8_t>{static_cast<uint8_t>(1)};
        case BytecodeValueType::kFloat64:
            return std::optional<uint8_t>{static_cast<uint8_t>(2)};
        case BytecodeValueType::kBool:
            return std::optional<uint8_t>{static_cast<uint8_t>(3)};
        case BytecodeValueType::kObject:
            return std::optional<uint8_t>{static_cast<uint8_t>(4)};
        case BytecodeValueType::kPointer:
        case BytecodeValueType::kString:
            return std::optional<uint8_t>{static_cast<uint8_t>(5)};
        case BytecodeValueType::kClosure:
            return std::optional<uint8_t>{static_cast<uint8_t>(7)};
    }
    return AotInvalid("TYPE_SIGNATURE", "unknown bytecode function param type");
}

StatusOr<std::string> BytecodeTypeToExportParamLlvmType(BytecodeValueType type) {
    switch (type) {
        case BytecodeValueType::kAny:
        case BytecodeValueType::kNull:
            return std::string("%TieValue");
        case BytecodeValueType::kInt64:
        case BytecodeValueType::kObject:
        case BytecodeValueType::kClosure:
            return std::string("i64");
        case BytecodeValueType::kFloat64:
            return std::string("double");
        case BytecodeValueType::kBool:
            return std::string("i1");
        case BytecodeValueType::kPointer:
        case BytecodeValueType::kString:
            return std::string("ptr");
    }
    return AotInvalid("TYPE_SIGNATURE", "unknown bytecode function param type");
}

StatusOr<std::string> BytecodeTypeToExportParamCType(BytecodeValueType type) {
    switch (type) {
        case BytecodeValueType::kAny:
        case BytecodeValueType::kNull:
            return std::string("tievm_aot_value");
        case BytecodeValueType::kInt64:
            return std::string("int64_t");
        case BytecodeValueType::kFloat64:
            return std::string("double");
        case BytecodeValueType::kBool:
            return std::string("bool");
        case BytecodeValueType::kObject:
        case BytecodeValueType::kClosure:
            return std::string("uint64_t");
        case BytecodeValueType::kPointer:
            return std::string("void*");
        case BytecodeValueType::kString:
            return std::string("const char*");
    }
    return AotInvalid("TYPE_SIGNATURE", "unknown bytecode function param type");
}

StatusOr<std::string> BytecodeTypeToExportReturnLlvmType(BytecodeValueType type) {
    switch (type) {
        case BytecodeValueType::kAny:
            return std::string("%TieValue");
        case BytecodeValueType::kNull:
            return std::string("void");
        case BytecodeValueType::kInt64:
        case BytecodeValueType::kObject:
        case BytecodeValueType::kClosure:
            return std::string("i64");
        case BytecodeValueType::kFloat64:
            return std::string("double");
        case BytecodeValueType::kBool:
            return std::string("i1");
        case BytecodeValueType::kPointer:
        case BytecodeValueType::kString:
            return std::string("ptr");
    }
    return AotInvalid("TYPE_SIGNATURE", "unknown bytecode function return type");
}

StatusOr<std::string> BytecodeTypeToExportReturnCType(BytecodeValueType type) {
    switch (type) {
        case BytecodeValueType::kAny:
            return std::string("tievm_aot_value");
        case BytecodeValueType::kNull:
            return std::string("void");
        case BytecodeValueType::kInt64:
            return std::string("int64_t");
        case BytecodeValueType::kFloat64:
            return std::string("double");
        case BytecodeValueType::kBool:
            return std::string("bool");
        case BytecodeValueType::kObject:
        case BytecodeValueType::kClosure:
            return std::string("uint64_t");
        case BytecodeValueType::kPointer:
            return std::string("void*");
        case BytecodeValueType::kString:
            return std::string("const char*");
    }
    return AotInvalid("TYPE_SIGNATURE", "unknown bytecode function return type");
}

class LlvmIrBuilder {
  public:
    LlvmIrBuilder(
        const LoadedAotModule& loaded, AotOutputKind output_kind,
        const std::vector<ExportedFunctionInfo>& exported_functions)
        : loaded_(loaded),
          module_(loaded.module),
          output_kind_(output_kind),
          exported_functions_(exported_functions) {}

    StatusOr<std::string> Build() {
        auto ffi_status = BuildFfiDecls();
        if (!ffi_status.ok()) {
            return ffi_status;
        }
        auto class_status = BuildClassMetadata();
        if (!class_status.ok()) {
            return class_status;
        }

        out_ << "; TieVM AOT v1\n";
        out_ << "%TieValue = type { i8, i64 }\n\n";
        out_ << "%TieClosureHeader = type { i32, i32 }\n\n";
        out_ << "%TieAotObject = type { i32 }\n\n";
        if (output_kind_ == AotOutputKind::kExecutable) {
            out_ << "declare i32 @printf(ptr, ...)\n";
        }
        out_ << "declare i64 @strlen(ptr)\n";
        out_ << "declare ptr @malloc(i64)\n";
        out_ << "declare ptr @memcpy(ptr, ptr, i64)\n";
        out_ << "declare void @abort()\n";
        for (const auto& decl : ffi_decls_) {
            out_ << decl << "\n";
        }
        out_ << "\n";

        if (output_kind_ == AotOutputKind::kExecutable) {
            out_ << "@.tievm_fmt_i64 = private unnamed_addr constant [6 x i8] c\"%lld\\0A\\00\", align 1\n\n";
        }

        out_ << "define i1 @tievm_aot_is_truthy(i8 %tag, i64 %bits) {\n"
             << "entry:\n"
             << "  switch i8 %tag, label %default [\n"
             << "    i8 0, label %isfalse\n"
             << "    i8 1, label %isint\n"
             << "    i8 2, label %isfloat\n"
             << "    i8 3, label %isbool\n"
             << "  ]\n"
             << "isfalse:\n"
             << "  ret i1 false\n"
             << "isint:\n"
             << "  %int_non_zero = icmp ne i64 %bits, 0\n"
             << "  ret i1 %int_non_zero\n"
             << "isfloat:\n"
             << "  %float_bits = bitcast i64 %bits to double\n"
             << "  %float_non_zero = fcmp une double %float_bits, 0.000000e+00\n"
             << "  ret i1 %float_non_zero\n"
             << "isbool:\n"
             << "  %bool_non_zero = icmp ne i64 %bits, 0\n"
             << "  ret i1 %bool_non_zero\n"
             << "default:\n"
             << "  ret i1 true\n"
             << "}\n\n";

        for (size_t i = 0; i < module_.functions().size(); ++i) {
            auto status = EmitFunction(i);
            if (!status.ok()) {
                return status;
            }
        }

        if (!globals_.empty()) {
            for (const auto& global : globals_) {
                out_ << global << "\n";
            }
            out_ << "\n";
        }

        auto export_status = EmitExportWrappers();
        if (!export_status.ok()) {
            return export_status;
        }

        if (output_kind_ == AotOutputKind::kExecutable) {
            const auto entry_index = module_.entry_function();
            out_ << "define i32 @main(i32 %argc, ptr %argv) {\n"
                 << "entry:\n"
                 << "  %ret = call %TieValue @tievm_fn_" << entry_index
                 << "(ptr null, i32 0, ptr null)\n"
                 << "  %ret_bits = extractvalue %TieValue %ret, 1\n"
                 << "  %fmt = getelementptr inbounds [6 x i8], ptr @.tievm_fmt_i64, i64 0, i64 0\n"
                 << "  %printed = call i32 (ptr, ...) @printf(ptr %fmt, i64 %ret_bits)\n"
                 << "  ret i32 0\n"
                 << "}\n";
        }

        return out_.str();
    }

  private:
    struct LoadedReg {
        std::string value;
        std::string tag;
        std::string bits;
    };

    std::string Temp() { return "%t" + std::to_string(temp_id_++); }

    std::string Label(std::string_view suffix) {
        return "l" + std::to_string(temp_id_++) + "_" + std::string(suffix);
    }

    std::string RegPtr(uint16_t reg_count, uint32_t reg_idx) {
        std::string ptr = Temp();
        func_ << "  " << ptr << " = getelementptr inbounds [" << reg_count
              << " x %TieValue], ptr %regs, i64 0, i64 " << reg_idx << "\n";
        return ptr;
    }

    LoadedReg LoadReg(uint16_t reg_count, uint32_t reg_idx) {
        LoadedReg out;
        const auto ptr = RegPtr(reg_count, reg_idx);
        out.value = Temp();
        func_ << "  " << out.value << " = load %TieValue, ptr " << ptr << ", align 8\n";
        out.tag = Temp();
        func_ << "  " << out.tag << " = extractvalue %TieValue " << out.value << ", 0\n";
        out.bits = Temp();
        func_ << "  " << out.bits << " = extractvalue %TieValue " << out.value << ", 1\n";
        return out;
    }

    std::string MakeValue(std::string_view tag_expr, std::string_view bits_expr) {
        const auto step1 = Temp();
        func_ << "  " << step1 << " = insertvalue %TieValue poison, " << tag_expr << ", 0\n";
        const auto step2 = Temp();
        func_ << "  " << step2 << " = insertvalue %TieValue " << step1 << ", " << bits_expr
              << ", 1\n";
        return step2;
    }

    void StoreReg(uint16_t reg_count, uint32_t reg_idx, std::string_view value_var) {
        const auto ptr = RegPtr(reg_count, reg_idx);
        func_ << "  store %TieValue " << value_var << ", ptr " << ptr << ", align 8\n";
    }

    std::string NullValue() { return "{ i8 0, i64 0 }"; }

    std::string FunctionName(size_t idx) { return "@tievm_fn_" + std::to_string(idx); }

    void EmitPcSwitch(std::string_view target_var) {
        func_ << "  switch i32 " << target_var << ", label %panic [\n";
        for (size_t i = 0; i < current_code_size_; ++i) {
            func_ << "    i32 " << i << ", label %bb" << i << "\n";
        }
        func_ << "  ]\n";
    }

    Status EmitExportWrappers() {
        for (const auto& exported : exported_functions_) {
            if (exported.function_index >= module_.functions().size()) {
                return AotInvalid(
                    "EXPORT_INDEX",
                    "exported function index out of range",
                    module_.name(),
                    exported.name);
            }
            const auto& function = module_.functions()[exported.function_index];
            if (exported.param_types.size() != exported.param_count) {
                return AotInvalid(
                    "EXPORT_SIGNATURE",
                    "exported function param type count mismatch",
                    module_.name(),
                    exported.name);
            }
            if (function.param_types().size() != function.param_count()) {
                return AotInvalid(
                    "EXPORT_SIGNATURE",
                    "function param type metadata mismatch",
                    module_.name(),
                    exported.name);
            }

            std::vector<std::string> llvm_param_types;
            llvm_param_types.reserve(exported.param_types.size());
            for (const auto param_type : exported.param_types) {
                auto llvm_type_or = BytecodeTypeToExportParamLlvmType(param_type);
                if (!llvm_type_or.ok()) {
                    return llvm_type_or.status();
                }
                llvm_param_types.push_back(llvm_type_or.value());
            }
            auto llvm_return_type_or = BytecodeTypeToExportReturnLlvmType(exported.return_type);
            if (!llvm_return_type_or.ok()) {
                return llvm_return_type_or.status();
            }
            const auto& llvm_return_type = llvm_return_type_or.value();

            out_ << "define " << llvm_return_type << " @\"" << EscapeLlvmQuotedIdent(exported.name)
                 << "\"(";
            for (size_t i = 0; i < llvm_param_types.size(); ++i) {
                if (i > 0) {
                    out_ << ", ";
                }
                out_ << llvm_param_types[i] << " %arg" << i;
            }
            out_ << ") {\n";
            out_ << "entry:\n";

            std::string args_ptr = "null";
            if (exported.param_count > 0) {
                out_ << "  %args_buf = alloca [" << exported.param_count << " x %TieValue], align 8\n";
                uint32_t wrapper_tmp_id = 0;
                auto wrapper_tmp = [&]() { return "%ew" + std::to_string(wrapper_tmp_id++); };
                auto emit_make_value = [&](std::string_view tag_expr, std::string_view bits_expr) {
                    const auto step1 = wrapper_tmp();
                    out_ << "  " << step1 << " = insertvalue %TieValue poison, " << tag_expr
                         << ", 0\n";
                    const auto step2 = wrapper_tmp();
                    out_ << "  " << step2 << " = insertvalue %TieValue " << step1 << ", "
                         << bits_expr << ", 1\n";
                    return step2;
                };

                for (uint16_t i = 0; i < exported.param_count; ++i) {
                    const auto param_type = exported.param_types[i];
                    const std::string arg_name = "%arg" + std::to_string(i);
                    std::string tie_value;
                    switch (param_type) {
                        case BytecodeValueType::kAny:
                        case BytecodeValueType::kNull:
                            tie_value = arg_name;
                            break;
                        case BytecodeValueType::kInt64:
                            tie_value = emit_make_value("i8 1", "i64 " + arg_name);
                            break;
                        case BytecodeValueType::kFloat64: {
                            const auto bits = wrapper_tmp();
                            out_ << "  " << bits << " = bitcast double " << arg_name << " to i64\n";
                            tie_value = emit_make_value("i8 2", "i64 " + bits);
                            break;
                        }
                        case BytecodeValueType::kBool: {
                            const auto bits = wrapper_tmp();
                            out_ << "  " << bits << " = zext i1 " << arg_name << " to i64\n";
                            tie_value = emit_make_value("i8 3", "i64 " + bits);
                            break;
                        }
                        case BytecodeValueType::kObject:
                            tie_value = emit_make_value("i8 4", "i64 " + arg_name);
                            break;
                        case BytecodeValueType::kPointer:
                        case BytecodeValueType::kString: {
                            const auto bits = wrapper_tmp();
                            out_ << "  " << bits << " = ptrtoint ptr " << arg_name << " to i64\n";
                            tie_value = emit_make_value("i8 5", "i64 " + bits);
                            break;
                        }
                        case BytecodeValueType::kClosure:
                            tie_value = emit_make_value("i8 7", "i64 " + arg_name);
                            break;
                    }

                    const auto slot = wrapper_tmp();
                    out_ << "  " << slot
                         << " = getelementptr inbounds [" << exported.param_count
                         << " x %TieValue], ptr %args_buf, i64 0, i64 " << i << "\n";
                    out_ << "  store %TieValue " << tie_value << ", ptr " << slot << ", align 8\n";
                }

                args_ptr = "%args_ptr";
                out_ << "  %args_ptr = getelementptr inbounds [" << exported.param_count
                     << " x %TieValue], ptr %args_buf, i64 0, i64 0\n";
            }

            out_ << "  %ret = call %TieValue " << FunctionName(exported.function_index)
                 << "(ptr " << args_ptr << ", i32 "
                 << static_cast<uint32_t>(exported.param_count) << ", ptr null)\n";

            auto expected_tag_or = BytecodeTypeToAotTag(exported.return_type);
            if (!expected_tag_or.ok()) {
                return expected_tag_or.status();
            }
            if (expected_tag_or.value().has_value()) {
                const auto ret_tag = Temp();
                const auto ret_tag_ok = Temp();
                const auto ret_type_ok_label = Label("export_ret_type_ok");
                const auto ret_type_bad_label = Label("export_ret_type_bad");
                out_ << "  " << ret_tag << " = extractvalue %TieValue %ret, 0\n";
                out_ << "  " << ret_tag_ok << " = icmp eq i8 " << ret_tag << ", "
                     << static_cast<uint32_t>(*expected_tag_or.value()) << "\n";
                out_ << "  br i1 " << ret_tag_ok << ", label %" << ret_type_ok_label
                     << ", label %" << ret_type_bad_label << "\n";
                out_ << ret_type_bad_label << ":\n";
                out_ << "  call void @abort()\n";
                out_ << "  unreachable\n";
                out_ << ret_type_ok_label << ":\n";
            }

            switch (exported.return_type) {
                case BytecodeValueType::kAny:
                    out_ << "  ret %TieValue %ret\n";
                    break;
                case BytecodeValueType::kNull:
                    out_ << "  ret void\n";
                    break;
                case BytecodeValueType::kInt64:
                case BytecodeValueType::kObject:
                case BytecodeValueType::kClosure: {
                    const auto ret_bits = Temp();
                    out_ << "  " << ret_bits << " = extractvalue %TieValue %ret, 1\n";
                    out_ << "  ret i64 " << ret_bits << "\n";
                    break;
                }
                case BytecodeValueType::kFloat64: {
                    const auto ret_bits = Temp();
                    const auto ret_f64 = Temp();
                    out_ << "  " << ret_bits << " = extractvalue %TieValue %ret, 1\n";
                    out_ << "  " << ret_f64 << " = bitcast i64 " << ret_bits << " to double\n";
                    out_ << "  ret double " << ret_f64 << "\n";
                    break;
                }
                case BytecodeValueType::kBool: {
                    const auto ret_bits = Temp();
                    const auto ret_bool = Temp();
                    out_ << "  " << ret_bits << " = extractvalue %TieValue %ret, 1\n";
                    out_ << "  " << ret_bool << " = icmp ne i64 " << ret_bits << ", 0\n";
                    out_ << "  ret i1 " << ret_bool << "\n";
                    break;
                }
                case BytecodeValueType::kPointer:
                case BytecodeValueType::kString: {
                    const auto ret_bits = Temp();
                    const auto ret_ptr = Temp();
                    out_ << "  " << ret_bits << " = extractvalue %TieValue %ret, 1\n";
                    out_ << "  " << ret_ptr << " = inttoptr i64 " << ret_bits << " to ptr\n";
                    out_ << "  ret ptr " << ret_ptr << "\n";
                    break;
                }
            }
            out_ << "}\n\n";
        }
        return Status::Ok();
    }

    Status BuildClassMetadata() {
        class_name_to_index_.clear();
        class_infos_.clear();
        class_infos_.reserve(module_.classes().size());

        for (const auto& class_decl : module_.classes()) {
            const uint32_t class_index = static_cast<uint32_t>(class_infos_.size());
            auto inserted = class_name_to_index_.emplace(class_decl.name, class_index);
            if (!inserted.second) {
                return AotInvalid(
                    "CLASS_DUPLICATE",
                    "duplicate class metadata name: " + class_decl.name,
                    module_.name());
            }

            AotClassInfo info;
            info.name = class_decl.name;
            info.base_indices.reserve(class_decl.base_classes.size());
            info.methods.reserve(class_decl.methods.size());
            for (const auto& method_decl : class_decl.methods) {
                if (method_decl.function_index >= module_.functions().size()) {
                    return AotInvalid(
                        "CLASS_METHOD_TARGET",
                        "class method target function index out of range: " +
                            std::to_string(method_decl.function_index),
                        module_.name(),
                        class_decl.name);
                }
                AotClassMethodInfo method;
                method.function_index = method_decl.function_index;
                method.access = method_decl.access;
                auto method_inserted = info.methods.emplace(method_decl.name, method);
                if (!method_inserted.second) {
                    return AotInvalid(
                        "CLASS_METHOD_DUPLICATE",
                        "duplicate method name in class metadata: " + method_decl.name,
                        module_.name(),
                        class_decl.name);
                }
            }
            class_infos_.push_back(std::move(info));
        }

        for (size_t class_index = 0; class_index < module_.classes().size(); ++class_index) {
            const auto& class_decl = module_.classes()[class_index];
            auto& info = class_infos_[class_index];
            for (const auto& base_name : class_decl.base_classes) {
                auto base_it = class_name_to_index_.find(base_name);
                if (base_it == class_name_to_index_.end()) {
                    return AotInvalid(
                        "CLASS_BASE_NOT_FOUND",
                        "class base not found in metadata: " + base_name,
                        module_.name(),
                        class_decl.name);
                }
                info.base_indices.push_back(base_it->second);
            }
        }

        if (class_infos_.empty()) {
            return Status::Ok();
        }

        std::vector<std::optional<std::vector<uint32_t>>> mro_cache(class_infos_.size());
        std::vector<uint8_t> visiting(class_infos_.size(), 0);
        for (uint32_t class_index = 0; class_index < class_infos_.size(); ++class_index) {
            auto mro_or = LinearizeClass(class_index, &mro_cache, &visiting);
            if (!mro_or.ok()) {
                return mro_or.status();
            }
            class_infos_[class_index].mro = std::move(mro_or.value());
        }
        return Status::Ok();
    }

    StatusOr<std::vector<uint32_t>> MergeC3Indices(std::vector<std::vector<uint32_t>> seqs) const {
        std::vector<uint32_t> result;
        while (true) {
            seqs.erase(
                std::remove_if(
                    seqs.begin(), seqs.end(),
                    [](const auto& seq) { return seq.empty(); }),
                seqs.end());
            if (seqs.empty()) {
                return result;
            }

            std::optional<uint32_t> candidate;
            for (const auto& seq : seqs) {
                const auto head = seq.front();
                bool in_tail = false;
                for (const auto& other : seqs) {
                    if (&other == &seq || other.size() < 2) {
                        continue;
                    }
                    if (std::find(other.begin() + 1, other.end(), head) != other.end()) {
                        in_tail = true;
                        break;
                    }
                }
                if (!in_tail) {
                    candidate = head;
                    break;
                }
            }

            if (!candidate.has_value()) {
                return AotInvalid(
                    "CLASS_MRO",
                    "C3 linearization failed due to inconsistent hierarchy",
                    module_.name());
            }

            result.push_back(candidate.value());
            for (auto& seq : seqs) {
                if (!seq.empty() && seq.front() == candidate.value()) {
                    seq.erase(seq.begin());
                }
            }
        }
    }

    StatusOr<std::vector<uint32_t>> LinearizeClass(
        uint32_t class_index, std::vector<std::optional<std::vector<uint32_t>>>* mro_cache,
        std::vector<uint8_t>* visiting) const {
        if (class_index >= class_infos_.size()) {
            return AotInvalid("CLASS_MRO", "class index out of range during linearization");
        }
        if ((*mro_cache)[class_index].has_value()) {
            return (*mro_cache)[class_index].value();
        }
        if ((*visiting)[class_index] == 1) {
            return AotInvalid(
                "CLASS_MRO",
                "class inheritance cycle detected at: " + class_infos_[class_index].name,
                module_.name());
        }
        (*visiting)[class_index] = 1;

        std::vector<std::vector<uint32_t>> seqs;
        for (uint32_t base_index : class_infos_[class_index].base_indices) {
            auto base_mro_or = LinearizeClass(base_index, mro_cache, visiting);
            if (!base_mro_or.ok()) {
                return base_mro_or.status();
            }
            seqs.push_back(base_mro_or.value());
        }
        seqs.push_back(class_infos_[class_index].base_indices);

        auto merged_or = MergeC3Indices(std::move(seqs));
        if (!merged_or.ok()) {
            return merged_or.status();
        }

        std::vector<uint32_t> out;
        out.push_back(class_index);
        out.insert(out.end(), merged_or.value().begin(), merged_or.value().end());
        (*mro_cache)[class_index] = out;
        (*visiting)[class_index] = 2;
        return out;
    }

    InvokeResolveResult ResolveInvokeMethod(
        uint32_t class_index, std::string_view method_name) const {
        if (class_index >= class_infos_.size()) {
            return InvokeResolveResult{};
        }
        const std::string key(method_name);
        for (uint32_t mro_class : class_infos_[class_index].mro) {
            const auto& methods = class_infos_[mro_class].methods;
            auto it = methods.find(key);
            if (it == methods.end()) {
                continue;
            }
            if (it->second.access == BytecodeAccessModifier::kPrivate) {
                return InvokeResolveResult{
                    InvokeResolveKind::kPrivate,
                    it->second.function_index,
                };
            }
            return InvokeResolveResult{
                InvokeResolveKind::kFound,
                it->second.function_index,
            };
        }
        return InvokeResolveResult{};
    }

    Status BuildFfiDecls() {
        for (const auto& binding : module_.ffi_bindings()) {
            if (binding.signature_index >= module_.ffi_signatures().size()) {
                return AotInvalid("FFI_SIGNATURE", "binding signature index out of range");
            }
            const auto& signature = module_.ffi_signatures()[binding.signature_index];
            if (signature.convention != CallingConvention::kSystem &&
                signature.convention != CallingConvention::kCdecl) {
                return AotInvalid(
                    "FFI_CONVENTION",
                    "only system/cdecl are supported in AOT static call lowering");
            }

            FfiLoweredInfo lowered;
            lowered.binding = &binding;
            lowered.signature = &signature;
            lowered.native_ir_name = "@\"" + EscapeLlvmQuotedIdent(binding.native_symbol) + "\"";

            auto ret_or = AbiTypeToLlvm(signature.return_type, true);
            if (!ret_or.ok()) {
                return ret_or.status();
            }
            lowered.llvm_return_type = ret_or.value();

            for (const auto& param : signature.params) {
                auto param_or = AbiTypeToLlvm(param, false);
                if (!param_or.ok()) {
                    return param_or.status();
                }
                lowered.llvm_param_types.push_back(param_or.value());
            }

            std::ostringstream decl;
            decl << "declare " << lowered.llvm_return_type << " " << lowered.native_ir_name << "(";
            for (size_t i = 0; i < lowered.llvm_param_types.size(); ++i) {
                if (i > 0) {
                    decl << ", ";
                }
                decl << lowered.llvm_param_types[i];
            }
            decl << ")";
            if (ffi_decl_dedup_.insert(decl.str()).second) {
                ffi_decls_.push_back(decl.str());
            }
            ffi_map_.insert_or_assign(binding.vm_symbol, lowered);
        }
        return Status::Ok();
    }

    StatusOr<std::string> Utf8GlobalPtrBits(uint32_t constant_idx, const Constant& constant) {
        auto it = utf8_const_to_global_.find(constant_idx);
        if (it != utf8_const_to_global_.end()) {
            const auto ptr = Temp();
            func_ << "  " << ptr << " = getelementptr inbounds ["
                  << (constant.utf8_value.size() + 1) << " x i8], ptr " << it->second
                  << ", i64 0, i64 0\n";
            const auto ptrbits = Temp();
            func_ << "  " << ptrbits << " = ptrtoint ptr " << ptr << " to i64\n";
            return ptrbits;
        }
        if (constant.utf8_value.find('\0') != std::string::npos) {
            return AotInvalid(
                "STRING_CONST",
                "embedded NUL in utf8 constants is unsupported in AOT");
        }

        const auto global_name = "@.tievm_str_" + std::to_string(utf8_const_to_global_.size());
        const auto size = constant.utf8_value.size() + 1;
        std::ostringstream global;
        global << global_name << " = private unnamed_addr constant [" << size
               << " x i8] c\"" << EscapeLlvmCString(constant.utf8_value) << "\", align 1";
        globals_.push_back(global.str());

        const auto ptr = Temp();
        func_ << "  " << ptr << " = getelementptr inbounds [" << size << " x i8], ptr "
              << global_name << ", i64 0, i64 0\n";
        utf8_const_to_global_[constant_idx] = global_name;

        const auto ptrbits = Temp();
        func_ << "  " << ptrbits << " = ptrtoint ptr " << ptr << " to i64\n";
        return ptrbits;
    }

    Status EmitFunction(size_t fn_idx) {
        const auto& function = module_.functions()[fn_idx];
        const auto code = function.FlattenedInstructions();
        if (code.empty()) {
            return AotInvalid(
                "LOWER_EMPTY_FUNCTION",
                "function has no instruction",
                module_.name(),
                function.name());
        }

        func_.str(std::string());
        func_.clear();
        temp_id_ = 0;
        current_code_size_ = code.size();

        uint32_t try_slot_count = 0;
        for (const auto& inst : code) {
            if (inst.opcode == OpCode::kTryBegin) {
                ++try_slot_count;
            }
        }

        func_ << "define internal %TieValue " << FunctionName(fn_idx)
              << "(ptr %args, i32 %argc, ptr %closure) {\n";
        func_ << "entry:\n";
        func_ << "  %regs = alloca [" << function.reg_count() << " x %TieValue], align 8\n";
        const std::string throw_value_ptr = "%throw_value";
        func_ << "  " << throw_value_ptr << " = alloca %TieValue, align 8\n";
        func_ << "  store %TieValue " << NullValue() << ", ptr " << throw_value_ptr << ", align 8\n";

        const std::string try_sp_ptr = "%try_sp";
        const std::string try_catch_ptr = "%try_catch";
        const std::string try_finally_ptr = "%try_finally";
        const std::string try_end_ptr = "%try_end";
        const std::string try_rethrow_ptr = "%try_rethrow";
        const std::string try_pending_ptr = "%try_pending";
        if (try_slot_count > 0) {
            func_ << "  " << try_sp_ptr << " = alloca i32, align 4\n";
            func_ << "  store i32 0, ptr " << try_sp_ptr << ", align 4\n";
            func_ << "  " << try_catch_ptr << " = alloca [" << try_slot_count
                  << " x i32], align 4\n";
            func_ << "  " << try_finally_ptr << " = alloca [" << try_slot_count
                  << " x i32], align 4\n";
            func_ << "  " << try_end_ptr << " = alloca [" << try_slot_count
                  << " x i32], align 4\n";
            func_ << "  " << try_rethrow_ptr << " = alloca [" << try_slot_count
                  << " x i1], align 1\n";
            func_ << "  " << try_pending_ptr << " = alloca [" << try_slot_count
                  << " x %TieValue], align 8\n";
        }

        const auto throw_dispatch_label = Label("throw_dispatch");
        const auto throw_scan_label = Label("throw_scan");
        const auto throw_check_label = Label("throw_check");
        const auto throw_check_finally_label = Label("throw_check_finally");
        const auto throw_to_catch_label = Label("throw_to_catch");
        const auto throw_to_finally_label = Label("throw_to_finally");
        const auto throw_continue_label = Label("throw_continue");
        const auto throw_unhandled_label = Label("throw_unhandled");

        for (uint32_t i = 0; i < function.reg_count(); ++i) {
            StoreReg(function.reg_count(), i, NullValue());
        }

        if (function.is_vararg()) {
            func_ << "  %argc_check = icmp uge i32 %argc, " << function.param_count() << "\n";
        } else {
            func_ << "  %argc_check = icmp eq i32 %argc, " << function.param_count() << "\n";
        }
        const auto args_ok = Label("args_ok");
        func_ << "  br i1 %argc_check, label %" << args_ok << ", label %panic\n";
        func_ << args_ok << ":\n";

        for (uint32_t i = 0; i < function.param_count(); ++i) {
            const auto arg_ptr = Temp();
            func_ << "  " << arg_ptr << " = getelementptr inbounds %TieValue, ptr %args, i64 " << i
                  << "\n";
            const auto arg_val = Temp();
            func_ << "  " << arg_val << " = load %TieValue, ptr " << arg_ptr << ", align 8\n";
            StoreReg(function.reg_count(), i, arg_val);
        }

        for (uint32_t i = 0; i < function.param_count(); ++i) {
            auto expected_tag_or = BytecodeTypeToAotTag(function.param_types()[i]);
            if (!expected_tag_or.ok()) {
                return expected_tag_or.status();
            }
            if (!expected_tag_or.value().has_value()) {
                continue;
            }
            const auto arg = LoadReg(function.reg_count(), i);
            const auto type_ok = Temp();
            const auto type_ok_label = Label("arg_type_ok");
            func_ << "  " << type_ok << " = icmp eq i8 " << arg.tag << ", "
                  << static_cast<uint32_t>(*expected_tag_or.value()) << "\n";
            func_ << "  br i1 " << type_ok << ", label %" << type_ok_label
                  << ", label %panic\n";
            func_ << type_ok_label << ":\n";
        }

        func_ << "  br label %bb0\n";

        for (size_t pc = 0; pc < code.size(); ++pc) {
            const auto& inst = code[pc];
            func_ << "bb" << pc << ":\n";
            const auto next_label =
                (pc + 1 < code.size()) ? ("bb" + std::to_string(pc + 1)) : std::string("panic");

            switch (inst.opcode) {
                case OpCode::kNop:
                    func_ << "  br label %" << next_label << "\n";
                    break;
                case OpCode::kMov: {
                    const auto src = LoadReg(function.reg_count(), inst.b);
                    StoreReg(function.reg_count(), inst.a, src.value);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kLoadK: {
                    if (inst.b >= module_.constants().size()) {
                        return AotInvalid(
                            "LOWER_CONST_BOUNDS",
                            "constant index out of range",
                            module_.name(),
                            function.name(),
                            pc,
                            inst.opcode);
                    }
                    const auto& constant = module_.constants()[inst.b];
                    if (constant.type == ConstantType::kInt64) {
                        const auto value = MakeValue(
                            "i8 1",
                            "i64 " + std::to_string(constant.int64_value));
                        StoreReg(function.reg_count(), inst.a, value);
                    } else if (constant.type == ConstantType::kFloat64) {
                        const auto value = MakeValue(
                            "i8 2",
                            "i64 " + std::to_string(DoubleBits(constant.float64_value)));
                        StoreReg(function.reg_count(), inst.a, value);
                    } else {
                        auto ptr_bits_or = Utf8GlobalPtrBits(inst.b, constant);
                        if (!ptr_bits_or.ok()) {
                            return ptr_bits_or.status();
                        }
                        const auto value = MakeValue("i8 5", "i64 " + ptr_bits_or.value());
                        StoreReg(function.reg_count(), inst.a, value);
                    }
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kAdd:
                case OpCode::kSub:
                case OpCode::kMul:
                case OpCode::kBitAnd:
                case OpCode::kBitOr:
                case OpCode::kBitXor:
                case OpCode::kBitShl:
                case OpCode::kBitShr: {
                    const auto lhs = LoadReg(function.reg_count(), inst.b);
                    const auto rhs = LoadReg(function.reg_count(), inst.c);
                    const auto result_bits = Temp();
                    const char* op = "add";
                    switch (inst.opcode) {
                        case OpCode::kAdd:
                            op = "add";
                            break;
                        case OpCode::kSub:
                            op = "sub";
                            break;
                        case OpCode::kMul:
                            op = "mul";
                            break;
                        case OpCode::kBitAnd:
                            op = "and";
                            break;
                        case OpCode::kBitOr:
                            op = "or";
                            break;
                        case OpCode::kBitXor:
                            op = "xor";
                            break;
                        case OpCode::kBitShl:
                            op = "shl";
                            break;
                        case OpCode::kBitShr:
                            op = "ashr";
                            break;
                        default:
                            break;
                    }
                    if (inst.opcode == OpCode::kBitShl || inst.opcode == OpCode::kBitShr) {
                        const auto shift_masked = Temp();
                        func_ << "  " << shift_masked << " = and i64 " << rhs.bits << ", 63\n";
                        func_ << "  " << result_bits << " = " << op << " i64 " << lhs.bits << ", "
                              << shift_masked << "\n";
                    } else {
                        func_ << "  " << result_bits << " = " << op << " i64 " << lhs.bits << ", "
                              << rhs.bits << "\n";
                    }
                    const auto value = MakeValue("i8 1", "i64 " + result_bits);
                    StoreReg(function.reg_count(), inst.a, value);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kDiv: {
                    const auto lhs = LoadReg(function.reg_count(), inst.b);
                    const auto rhs = LoadReg(function.reg_count(), inst.c);
                    const auto divisor_zero = Temp();
                    const auto div_ok_label = Label("div_ok");
                    const auto div_throw_label = Label("div_throw");
                    func_ << "  " << divisor_zero << " = icmp eq i64 " << rhs.bits << ", 0\n";
                    func_ << "  br i1 " << divisor_zero << ", label %" << div_throw_label
                          << ", label %" << div_ok_label << "\n";
                    func_ << div_throw_label << ":\n";
                    func_ << "  store %TieValue " << NullValue() << ", ptr " << throw_value_ptr
                          << ", align 8\n";
                    func_ << "  br label %" << throw_dispatch_label << "\n";
                    func_ << div_ok_label << ":\n";
                    const auto result_bits = Temp();
                    func_ << "  " << result_bits << " = sdiv i64 " << lhs.bits << ", " << rhs.bits
                          << "\n";
                    const auto value = MakeValue("i8 1", "i64 " + result_bits);
                    StoreReg(function.reg_count(), inst.a, value);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kAddImm:
                case OpCode::kSubImm: {
                    const auto src = LoadReg(function.reg_count(), inst.b);
                    const int32_t imm = SignedU32(inst.c);
                    const auto result_bits = Temp();
                    if (inst.opcode == OpCode::kSubImm) {
                        func_ << "  " << result_bits << " = sub i64 " << src.bits << ", " << imm
                              << "\n";
                    } else {
                        func_ << "  " << result_bits << " = add i64 " << src.bits << ", " << imm
                              << "\n";
                    }
                    const auto value = MakeValue("i8 1", "i64 " + result_bits);
                    StoreReg(function.reg_count(), inst.a, value);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kInc:
                case OpCode::kDec: {
                    const auto src = LoadReg(function.reg_count(), inst.a);
                    const auto result_bits = Temp();
                    if (inst.opcode == OpCode::kDec) {
                        func_ << "  " << result_bits << " = sub i64 " << src.bits << ", 1\n";
                    } else {
                        func_ << "  " << result_bits << " = add i64 " << src.bits << ", 1\n";
                    }
                    const auto value = MakeValue("i8 1", "i64 " + result_bits);
                    StoreReg(function.reg_count(), inst.a, value);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kSubImmJnz:
                case OpCode::kAddImmJnz: {
                    const auto src = LoadReg(function.reg_count(), inst.a);
                    const int32_t imm = SignedU32(inst.b);
                    const auto result_bits = Temp();
                    if (inst.opcode == OpCode::kSubImmJnz) {
                        func_ << "  " << result_bits << " = sub i64 " << src.bits << ", " << imm
                              << "\n";
                    } else {
                        func_ << "  " << result_bits << " = add i64 " << src.bits << ", " << imm
                              << "\n";
                    }
                    const auto value = MakeValue("i8 1", "i64 " + result_bits);
                    StoreReg(function.reg_count(), inst.a, value);
                    const auto cond = Temp();
                    const int64_t target =
                        static_cast<int64_t>(pc) + static_cast<int64_t>(SignedU32(inst.c));
                    func_ << "  " << cond << " = icmp ne i64 " << result_bits << ", 0\n";
                    func_ << "  br i1 " << cond << ", label %bb" << target << ", label %"
                          << next_label << "\n";
                    break;
                }
                case OpCode::kCmpEq: {
                    const auto lhs = LoadReg(function.reg_count(), inst.b);
                    const auto rhs = LoadReg(function.reg_count(), inst.c);
                    const auto tag_eq = Temp();
                    const auto bits_eq = Temp();
                    const auto both_eq = Temp();
                    const auto bits = Temp();
                    func_ << "  " << tag_eq << " = icmp eq i8 " << lhs.tag << ", " << rhs.tag << "\n";
                    func_ << "  " << bits_eq << " = icmp eq i64 " << lhs.bits << ", " << rhs.bits
                          << "\n";
                    func_ << "  " << both_eq << " = and i1 " << tag_eq << ", " << bits_eq << "\n";
                    func_ << "  " << bits << " = zext i1 " << both_eq << " to i64\n";
                    const auto value = MakeValue("i8 3", "i64 " + bits);
                    StoreReg(function.reg_count(), inst.a, value);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kBitNot: {
                    const auto src = LoadReg(function.reg_count(), inst.b);
                    const auto bits = Temp();
                    func_ << "  " << bits << " = xor i64 " << src.bits << ", -1\n";
                    const auto value = MakeValue("i8 1", "i64 " + bits);
                    StoreReg(function.reg_count(), inst.a, value);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kStrLen: {
                    const auto src = LoadReg(function.reg_count(), inst.b);
                    const auto ptr = Temp();
                    const auto len = Temp();
                    func_ << "  " << ptr << " = inttoptr i64 " << src.bits << " to ptr\n";
                    func_ << "  " << len << " = call i64 @strlen(ptr " << ptr << ")\n";
                    const auto value = MakeValue("i8 1", "i64 " + len);
                    StoreReg(function.reg_count(), inst.a, value);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kStrConcat: {
                    const auto lhs = LoadReg(function.reg_count(), inst.b);
                    const auto rhs = LoadReg(function.reg_count(), inst.c);
                    const auto lhs_ptr = Temp();
                    const auto rhs_ptr = Temp();
                    const auto lhs_len = Temp();
                    const auto rhs_len = Temp();
                    const auto total_len = Temp();
                    const auto alloc_len = Temp();
                    const auto out_ptr = Temp();
                    const auto rhs_base = Temp();
                    const auto tail_ptr = Temp();
                    const auto out_bits = Temp();

                    func_ << "  " << lhs_ptr << " = inttoptr i64 " << lhs.bits << " to ptr\n";
                    func_ << "  " << rhs_ptr << " = inttoptr i64 " << rhs.bits << " to ptr\n";
                    func_ << "  " << lhs_len << " = call i64 @strlen(ptr " << lhs_ptr << ")\n";
                    func_ << "  " << rhs_len << " = call i64 @strlen(ptr " << rhs_ptr << ")\n";
                    func_ << "  " << total_len << " = add i64 " << lhs_len << ", " << rhs_len << "\n";
                    func_ << "  " << alloc_len << " = add i64 " << total_len << ", 1\n";
                    func_ << "  " << out_ptr << " = call ptr @malloc(i64 " << alloc_len << ")\n";
                    func_ << "  call ptr @memcpy(ptr " << out_ptr << ", ptr " << lhs_ptr << ", i64 "
                          << lhs_len << ")\n";
                    func_ << "  " << rhs_base << " = getelementptr inbounds i8, ptr " << out_ptr
                          << ", i64 " << lhs_len << "\n";
                    func_ << "  call ptr @memcpy(ptr " << rhs_base << ", ptr " << rhs_ptr
                          << ", i64 " << rhs_len << ")\n";
                    func_ << "  " << tail_ptr << " = getelementptr inbounds i8, ptr " << out_ptr
                          << ", i64 " << total_len << "\n";
                    func_ << "  store i8 0, ptr " << tail_ptr << ", align 1\n";
                    func_ << "  " << out_bits << " = ptrtoint ptr " << out_ptr << " to i64\n";
                    const auto value = MakeValue("i8 5", "i64 " + out_bits);
                    StoreReg(function.reg_count(), inst.a, value);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kJmp: {
                    const auto target = static_cast<int64_t>(pc) + SignedU32(inst.a);
                    func_ << "  br label %bb" << target << "\n";
                    break;
                }
                case OpCode::kJmpIf: {
                    const auto cond = LoadReg(function.reg_count(), inst.a);
                    const auto is_truthy = Temp();
                    const auto target = static_cast<int64_t>(pc) + SignedU32(inst.b);
                    func_ << "  " << is_truthy << " = call i1 @tievm_aot_is_truthy(i8 " << cond.tag
                          << ", i64 " << cond.bits << ")\n";
                    func_ << "  br i1 " << is_truthy << ", label %bb" << target << ", label %"
                          << next_label << "\n";
                    break;
                }
                case OpCode::kJmpIfZero:
                case OpCode::kJmpIfNotZero: {
                    const auto value = LoadReg(function.reg_count(), inst.a);
                    const auto cond = Temp();
                    const auto target = static_cast<int64_t>(pc) + SignedU32(inst.b);
                    const char* cmp = (inst.opcode == OpCode::kJmpIfZero) ? "eq" : "ne";
                    func_ << "  " << cond << " = icmp " << cmp << " i64 " << value.bits << ", 0\n";
                    func_ << "  br i1 " << cond << ", label %bb" << target << ", label %"
                          << next_label << "\n";
                    break;
                }
                case OpCode::kDecJnz: {
                    const auto value = LoadReg(function.reg_count(), inst.a);
                    const auto next_counter = Temp();
                    const auto cond = Temp();
                    const auto target = static_cast<int64_t>(pc) + SignedU32(inst.b);
                    func_ << "  " << next_counter << " = sub i64 " << value.bits << ", 1\n";
                    const auto stored = MakeValue("i8 1", "i64 " + next_counter);
                    StoreReg(function.reg_count(), inst.a, stored);
                    func_ << "  " << cond << " = icmp ne i64 " << next_counter << ", 0\n";
                    func_ << "  br i1 " << cond << ", label %bb" << target << ", label %"
                          << next_label << "\n";
                    break;
                }
                case OpCode::kAddDecJnz: {
                    const auto acc = LoadReg(function.reg_count(), inst.a);
                    const auto counter = LoadReg(function.reg_count(), inst.b);
                    const auto next_acc = Temp();
                    const auto next_counter = Temp();
                    const auto cond = Temp();
                    const auto target = static_cast<int64_t>(pc) + SignedU32(inst.c);
                    func_ << "  " << next_acc << " = add i64 " << acc.bits << ", " << counter.bits << "\n";
                    func_ << "  " << next_counter << " = sub i64 " << counter.bits << ", 1\n";
                    const auto acc_value = MakeValue("i8 1", "i64 " + next_acc);
                    const auto ctr_value = MakeValue("i8 1", "i64 " + next_counter);
                    StoreReg(function.reg_count(), inst.a, acc_value);
                    StoreReg(function.reg_count(), inst.b, ctr_value);
                    func_ << "  " << cond << " = icmp ne i64 " << next_counter << ", 0\n";
                    func_ << "  br i1 " << cond << ", label %bb" << target << ", label %"
                          << next_label << "\n";
                    break;
                }
                case OpCode::kCall:
                case OpCode::kTailCall: {
                    if (inst.b >= module_.functions().size()) {
                        return AotInvalid(
                            "LOWER_CALL_TARGET",
                            "call target out of range",
                            module_.name(),
                            function.name(),
                            pc,
                            inst.opcode);
                    }
                    std::string args_ptr = "null";
                    if (inst.c > 0) {
                        const auto argbuf = Temp();
                        func_ << "  " << argbuf << " = alloca [" << inst.c << " x %TieValue], align 8\n";
                        for (uint32_t i = 0; i < inst.c; ++i) {
                            const auto src = LoadReg(function.reg_count(), inst.a + 1 + i);
                            const auto dst_ptr = Temp();
                            func_ << "  " << dst_ptr << " = getelementptr inbounds [" << inst.c
                                  << " x %TieValue], ptr " << argbuf << ", i64 0, i64 " << i << "\n";
                            func_ << "  store %TieValue " << src.value << ", ptr " << dst_ptr
                                  << ", align 8\n";
                        }
                        const auto first = Temp();
                        func_ << "  " << first << " = getelementptr inbounds [" << inst.c
                              << " x %TieValue], ptr " << argbuf << ", i64 0, i64 0\n";
                        args_ptr = first;
                    }
                    const auto ret = Temp();
                    func_ << "  " << ret << " = call %TieValue " << FunctionName(inst.b) << "(ptr "
                          << args_ptr << ", i32 " << inst.c << ", ptr null)\n";
                    if (inst.opcode == OpCode::kTailCall) {
                        func_ << "  ret %TieValue " << ret << "\n";
                    } else {
                        StoreReg(function.reg_count(), inst.a, ret);
                        func_ << "  br label %" << next_label << "\n";
                    }
                    break;
                }
                case OpCode::kVarArg: {
                    for (uint32_t i = 0; i < inst.c; ++i) {
                        const uint32_t source_idx = function.param_count() + inst.b + i;
                        const auto has_arg = Temp();
                        const auto has_arg_label = Label("vararg_has");
                        const auto no_arg_label = Label("vararg_none");
                        const auto merge_label = Label("vararg_merge");
                        func_ << "  " << has_arg << " = icmp ult i32 " << source_idx << ", %argc\n";
                        func_ << "  br i1 " << has_arg << ", label %" << has_arg_label
                              << ", label %" << no_arg_label << "\n";
                        func_ << has_arg_label << ":\n";
                        const auto src_ptr = Temp();
                        const auto src_val = Temp();
                        func_ << "  " << src_ptr
                              << " = getelementptr inbounds %TieValue, ptr %args, i64 "
                              << source_idx << "\n";
                        func_ << "  " << src_val << " = load %TieValue, ptr " << src_ptr
                              << ", align 8\n";
                        func_ << "  br label %" << merge_label << "\n";
                        func_ << no_arg_label << ":\n";
                        func_ << "  br label %" << merge_label << "\n";
                        func_ << merge_label << ":\n";
                        const auto merged = Temp();
                        func_ << "  " << merged << " = phi %TieValue [ " << src_val << ", %"
                              << has_arg_label << " ], [ zeroinitializer, %" << no_arg_label
                              << " ]\n";
                        StoreReg(function.reg_count(), inst.a + i, merged);
                    }
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kClosure: {
                    if (inst.b >= module_.functions().size()) {
                        return AotInvalid(
                            "LOWER_CLOSURE_TARGET",
                            "closure function index out of range",
                            module_.name(),
                            function.name(),
                            pc,
                            inst.opcode);
                    }
                    const uint32_t upvalue_count = inst.flags;
                    const auto upvalue_bytes_ptr = Temp();
                    const auto upvalue_bytes = Temp();
                    const auto total_bytes = Temp();
                    const auto closure_mem = Temp();
                    func_ << "  " << upvalue_bytes_ptr
                          << " = getelementptr inbounds %TieValue, ptr null, i64 "
                          << upvalue_count << "\n";
                    func_ << "  " << upvalue_bytes << " = ptrtoint ptr " << upvalue_bytes_ptr
                          << " to i64\n";
                    func_ << "  " << total_bytes << " = add i64 " << upvalue_bytes << ", 8\n";
                    func_ << "  " << closure_mem << " = call ptr @malloc(i64 " << total_bytes
                          << ")\n";

                    const auto fn_slot = Temp();
                    func_ << "  " << fn_slot
                          << " = getelementptr inbounds %TieClosureHeader, ptr " << closure_mem
                          << ", i32 0, i32 0\n";
                    func_ << "  store i32 " << inst.b << ", ptr " << fn_slot << ", align 4\n";
                    const auto count_slot = Temp();
                    func_ << "  " << count_slot
                          << " = getelementptr inbounds %TieClosureHeader, ptr " << closure_mem
                          << ", i32 0, i32 1\n";
                    func_ << "  store i32 " << upvalue_count << ", ptr " << count_slot
                          << ", align 4\n";

                    const auto upvalues_base_i8 = Temp();
                    const auto upvalues_base = Temp();
                    func_ << "  " << upvalues_base_i8 << " = getelementptr inbounds i8, ptr "
                          << closure_mem << ", i64 8\n";
                    func_ << "  " << upvalues_base << " = bitcast ptr " << upvalues_base_i8
                          << " to ptr\n";
                    for (uint32_t i = 0; i < upvalue_count; ++i) {
                        const auto src = LoadReg(function.reg_count(), inst.c + i);
                        const auto up_slot = Temp();
                        func_ << "  " << up_slot << " = getelementptr inbounds %TieValue, ptr "
                              << upvalues_base << ", i64 " << i << "\n";
                        func_ << "  store %TieValue " << src.value << ", ptr " << up_slot
                              << ", align 8\n";
                    }

                    const auto closure_bits = Temp();
                    func_ << "  " << closure_bits << " = ptrtoint ptr " << closure_mem
                          << " to i64\n";
                    const auto value = MakeValue("i8 7", "i64 " + closure_bits);
                    StoreReg(function.reg_count(), inst.a, value);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kGetUpval: {
                    const auto closure_is_null = Temp();
                    const auto get_ok = Label("get_upval_ok");
                    func_ << "  " << closure_is_null << " = icmp eq ptr %closure, null\n";
                    func_ << "  br i1 " << closure_is_null << ", label %panic, label %" << get_ok
                          << "\n";
                    func_ << get_ok << ":\n";
                    const auto count_slot = Temp();
                    func_ << "  " << count_slot
                          << " = getelementptr inbounds %TieClosureHeader, ptr %closure, i32 0, i32 1\n";
                    const auto count = Temp();
                    func_ << "  " << count << " = load i32, ptr " << count_slot << ", align 4\n";
                    const auto idx_ok = Temp();
                    const auto idx_ok_label = Label("get_upval_idx_ok");
                    func_ << "  " << idx_ok << " = icmp ult i32 " << inst.b << ", " << count
                          << "\n";
                    func_ << "  br i1 " << idx_ok << ", label %" << idx_ok_label
                          << ", label %panic\n";
                    func_ << idx_ok_label << ":\n";
                    const auto upvalues_i8 = Temp();
                    const auto upvalues = Temp();
                    func_ << "  " << upvalues_i8
                          << " = getelementptr inbounds i8, ptr %closure, i64 8\n";
                    func_ << "  " << upvalues << " = bitcast ptr " << upvalues_i8 << " to ptr\n";
                    const auto slot = Temp();
                    func_ << "  " << slot << " = getelementptr inbounds %TieValue, ptr "
                          << upvalues << ", i64 " << inst.b << "\n";
                    const auto value = Temp();
                    func_ << "  " << value << " = load %TieValue, ptr " << slot << ", align 8\n";
                    StoreReg(function.reg_count(), inst.a, value);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kSetUpval: {
                    const auto closure_is_null = Temp();
                    const auto set_ok = Label("set_upval_ok");
                    func_ << "  " << closure_is_null << " = icmp eq ptr %closure, null\n";
                    func_ << "  br i1 " << closure_is_null << ", label %panic, label %" << set_ok
                          << "\n";
                    func_ << set_ok << ":\n";
                    const auto count_slot = Temp();
                    func_ << "  " << count_slot
                          << " = getelementptr inbounds %TieClosureHeader, ptr %closure, i32 0, i32 1\n";
                    const auto count = Temp();
                    func_ << "  " << count << " = load i32, ptr " << count_slot << ", align 4\n";
                    const auto idx_ok = Temp();
                    const auto idx_ok_label = Label("set_upval_idx_ok");
                    func_ << "  " << idx_ok << " = icmp ult i32 " << inst.b << ", " << count
                          << "\n";
                    func_ << "  br i1 " << idx_ok << ", label %" << idx_ok_label
                          << ", label %panic\n";
                    func_ << idx_ok_label << ":\n";
                    const auto upvalues_i8 = Temp();
                    const auto upvalues = Temp();
                    func_ << "  " << upvalues_i8
                          << " = getelementptr inbounds i8, ptr %closure, i64 8\n";
                    func_ << "  " << upvalues << " = bitcast ptr " << upvalues_i8 << " to ptr\n";
                    const auto slot = Temp();
                    func_ << "  " << slot << " = getelementptr inbounds %TieValue, ptr "
                          << upvalues << ", i64 " << inst.b << "\n";
                    const auto src = LoadReg(function.reg_count(), inst.a);
                    func_ << "  store %TieValue " << src.value << ", ptr " << slot << ", align 8\n";
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kCallClosure:
                case OpCode::kTailCallClosure: {
                    const auto closure = LoadReg(function.reg_count(), inst.b);
                    const auto is_closure = Temp();
                    const auto closure_ok = Label("call_closure_ok");
                    func_ << "  " << is_closure << " = icmp eq i8 " << closure.tag << ", 7\n";
                    func_ << "  br i1 " << is_closure << ", label %" << closure_ok
                          << ", label %panic\n";
                    func_ << closure_ok << ":\n";
                    const auto closure_ptr = Temp();
                    func_ << "  " << closure_ptr << " = inttoptr i64 " << closure.bits
                          << " to ptr\n";
                    const auto fn_slot = Temp();
                    func_ << "  " << fn_slot
                          << " = getelementptr inbounds %TieClosureHeader, ptr " << closure_ptr
                          << ", i32 0, i32 0\n";
                    const auto fn_index = Temp();
                    func_ << "  " << fn_index << " = load i32, ptr " << fn_slot << ", align 4\n";

                    std::string args_ptr = "null";
                    if (inst.c > 0) {
                        const auto argbuf = Temp();
                        func_ << "  " << argbuf << " = alloca [" << inst.c
                              << " x %TieValue], align 8\n";
                        for (uint32_t i = 0; i < inst.c; ++i) {
                            const auto src = LoadReg(function.reg_count(), inst.a + 1 + i);
                            const auto dst_ptr = Temp();
                            func_ << "  " << dst_ptr << " = getelementptr inbounds [" << inst.c
                                  << " x %TieValue], ptr " << argbuf << ", i64 0, i64 " << i
                                  << "\n";
                            func_ << "  store %TieValue " << src.value << ", ptr " << dst_ptr
                                  << ", align 8\n";
                        }
                        const auto first = Temp();
                        func_ << "  " << first << " = getelementptr inbounds [" << inst.c
                              << " x %TieValue], ptr " << argbuf << ", i64 0, i64 0\n";
                        args_ptr = first;
                    }

                    const auto dispatch_merge = Label("call_closure_merge");
                    std::vector<std::string> case_labels(module_.functions().size());
                    for (size_t i = 0; i < module_.functions().size(); ++i) {
                        case_labels[i] = Label("call_closure_case_" + std::to_string(i));
                    }
                    func_ << "  switch i32 " << fn_index << ", label %panic [\n";
                    for (size_t i = 0; i < module_.functions().size(); ++i) {
                        func_ << "    i32 " << i << ", label %" << case_labels[i] << "\n";
                    }
                    func_ << "  ]\n";

                    std::vector<std::string> call_values(module_.functions().size());
                    for (size_t i = 0; i < module_.functions().size(); ++i) {
                        func_ << case_labels[i] << ":\n";
                        call_values[i] = Temp();
                        func_ << "  " << call_values[i] << " = call %TieValue " << FunctionName(i)
                              << "(ptr " << args_ptr << ", i32 " << inst.c << ", ptr "
                              << closure_ptr << ")\n";
                        if (inst.opcode == OpCode::kTailCallClosure) {
                            func_ << "  ret %TieValue " << call_values[i] << "\n";
                        } else {
                            func_ << "  br label %" << dispatch_merge << "\n";
                        }
                    }

                    if (inst.opcode == OpCode::kCallClosure) {
                        func_ << dispatch_merge << ":\n";
                        const auto merged = Temp();
                        func_ << "  " << merged << " = phi %TieValue ";
                        for (size_t i = 0; i < module_.functions().size(); ++i) {
                            if (i > 0) {
                                func_ << ", ";
                            }
                            func_ << "[ " << call_values[i] << ", %" << case_labels[i] << " ]";
                        }
                        func_ << "\n";
                        StoreReg(function.reg_count(), inst.a, merged);
                        func_ << "  br label %" << next_label << "\n";
                    }
                    break;
                }
                case OpCode::kFfiCall: {
                    if (inst.b >= module_.constants().size() ||
                        module_.constants()[inst.b].type != ConstantType::kUtf8) {
                        return AotInvalid(
                            "LOWER_FFI_SYMBOL",
                            "ffi_call requires utf8 symbol constant",
                            module_.name(),
                            function.name(),
                            pc,
                            inst.opcode);
                    }
                    const auto& symbol = module_.constants()[inst.b].utf8_value;
                    auto binding_it = ffi_map_.find(symbol);
                    if (binding_it == ffi_map_.end()) {
                        return AotInvalid(
                            "LOWER_FFI_BINDING",
                            "ffi symbol binding not found: " + symbol,
                            module_.name(),
                            function.name(),
                            pc,
                            inst.opcode);
                    }
                    const auto& ffi = binding_it->second;
                    if (ffi.signature->params.size() != inst.c) {
                        return AotInvalid(
                            "LOWER_FFI_ARITY",
                            "ffi argument count mismatch for symbol: " + symbol,
                            module_.name(),
                            function.name(),
                            pc,
                            inst.opcode);
                    }

                    std::vector<std::string> arg_values;
                    arg_values.reserve(inst.c);
                    for (uint32_t i = 0; i < inst.c; ++i) {
                        const auto src = LoadReg(function.reg_count(), inst.a + 1 + i);
                        const auto& type = ffi.signature->params[i];
                        const auto& llvm_type = ffi.llvm_param_types[i];
                        if (type.kind == AbiValueKind::kF32 || type.kind == AbiValueKind::kF64) {
                            const auto as_double_bits = Temp();
                            func_ << "  " << as_double_bits << " = bitcast i64 " << src.bits
                                  << " to double\n";
                            if (type.kind == AbiValueKind::kF32) {
                                const auto as_float = Temp();
                                func_ << "  " << as_float << " = fptrunc double " << as_double_bits
                                      << " to float\n";
                                arg_values.push_back(llvm_type + " " + as_float);
                            } else {
                                arg_values.push_back(llvm_type + " " + as_double_bits);
                            }
                        } else {
                            arg_values.push_back(llvm_type + " " + src.bits);
                        }
                    }

                    const auto call_result = Temp();
                    func_ << "  ";
                    if (ffi.llvm_return_type == "void") {
                        func_ << "call " << ffi.llvm_return_type << " " << ffi.native_ir_name << "(";
                    } else {
                        func_ << call_result << " = call " << ffi.llvm_return_type << " "
                              << ffi.native_ir_name << "(";
                    }
                    for (size_t i = 0; i < arg_values.size(); ++i) {
                        if (i > 0) {
                            func_ << ", ";
                        }
                        func_ << arg_values[i];
                    }
                    func_ << ")\n";

                    std::string value;
                    if (ffi.signature->return_type.kind == AbiValueKind::kVoid) {
                        value = MakeValue("i8 0", "i64 0");
                    } else if (ffi.signature->return_type.kind == AbiValueKind::kF32 ||
                               ffi.signature->return_type.kind == AbiValueKind::kF64) {
                        std::string as_double = call_result;
                        if (ffi.signature->return_type.kind == AbiValueKind::kF32) {
                            as_double = Temp();
                            func_ << "  " << as_double << " = fpext float " << call_result
                                  << " to double\n";
                        }
                        const auto bits = Temp();
                        func_ << "  " << bits << " = bitcast double " << as_double << " to i64\n";
                        value = MakeValue("i8 2", "i64 " + bits);
                    } else if (ffi.signature->return_type.kind == AbiValueKind::kBool) {
                        const auto bool_bits = Temp();
                        func_ << "  " << bool_bits << " = icmp ne i64 " << call_result << ", 0\n";
                        const auto z = Temp();
                        func_ << "  " << z << " = zext i1 " << bool_bits << " to i64\n";
                        value = MakeValue("i8 3", "i64 " + z);
                    } else if (ffi.signature->return_type.kind == AbiValueKind::kObject) {
                        value = MakeValue("i8 4", "i64 " + call_result);
                    } else if (ffi.signature->return_type.kind == AbiValueKind::kPointer ||
                               ffi.signature->return_type.kind == AbiValueKind::kUtf8 ||
                               ffi.signature->return_type.kind == AbiValueKind::kStruct) {
                        value = MakeValue("i8 5", "i64 " + call_result);
                    } else {
                        value = MakeValue("i8 1", "i64 " + call_result);
                    }

                    StoreReg(function.reg_count(), inst.a, value);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kTryBegin: {
                    if (try_slot_count == 0) {
                        return AotInvalid(
                            "LOWER_TRY_STACK",
                            "try stack not initialized",
                            module_.name(),
                            function.name(),
                            pc,
                            inst.opcode);
                    }
                    const auto sp = Temp();
                    const auto sp_ok = Temp();
                    const auto begin_ok = Label("try_begin_ok");
                    func_ << "  " << sp << " = load i32, ptr " << try_sp_ptr << ", align 4\n";
                    func_ << "  " << sp_ok << " = icmp ult i32 " << sp << ", " << try_slot_count
                          << "\n";
                    func_ << "  br i1 " << sp_ok << ", label %" << begin_ok << ", label %panic\n";
                    func_ << begin_ok << ":\n";
                    const auto idx64 = Temp();
                    func_ << "  " << idx64 << " = zext i32 " << sp << " to i64\n";
                    const auto catch_slot = Temp();
                    func_ << "  " << catch_slot << " = getelementptr inbounds [" << try_slot_count
                          << " x i32], ptr " << try_catch_ptr << ", i64 0, i64 " << idx64 << "\n";
                    func_ << "  store i32 " << inst.a << ", ptr " << catch_slot << ", align 4\n";
                    const auto finally_slot = Temp();
                    func_ << "  " << finally_slot << " = getelementptr inbounds [" << try_slot_count
                          << " x i32], ptr " << try_finally_ptr << ", i64 0, i64 " << idx64
                          << "\n";
                    func_ << "  store i32 " << inst.b << ", ptr " << finally_slot << ", align 4\n";
                    const auto end_slot = Temp();
                    func_ << "  " << end_slot << " = getelementptr inbounds [" << try_slot_count
                          << " x i32], ptr " << try_end_ptr << ", i64 0, i64 " << idx64 << "\n";
                    func_ << "  store i32 " << inst.c << ", ptr " << end_slot << ", align 4\n";
                    const auto rethrow_slot = Temp();
                    func_ << "  " << rethrow_slot << " = getelementptr inbounds ["
                          << try_slot_count << " x i1], ptr " << try_rethrow_ptr << ", i64 0, i64 "
                          << idx64 << "\n";
                    func_ << "  store i1 false, ptr " << rethrow_slot << ", align 1\n";
                    const auto pending_slot = Temp();
                    func_ << "  " << pending_slot << " = getelementptr inbounds [" << try_slot_count
                          << " x %TieValue], ptr " << try_pending_ptr << ", i64 0, i64 " << idx64
                          << "\n";
                    func_ << "  store %TieValue " << NullValue() << ", ptr " << pending_slot
                          << ", align 8\n";
                    const auto sp_next = Temp();
                    func_ << "  " << sp_next << " = add i32 " << sp << ", 1\n";
                    func_ << "  store i32 " << sp_next << ", ptr " << try_sp_ptr << ", align 4\n";
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kTryEnd:
                case OpCode::kEndCatch: {
                    if (try_slot_count == 0) {
                        return AotInvalid(
                            "LOWER_TRY_STACK",
                            "try stack not initialized",
                            module_.name(),
                            function.name(),
                            pc,
                            inst.opcode);
                    }
                    const auto frame_ok = Label("try_frame_ok");
                    const auto with_finally = Label("try_with_finally");
                    const auto without_finally = Label("try_without_finally");
                    const auto sp = Temp();
                    const auto has_frame = Temp();
                    func_ << "  " << sp << " = load i32, ptr " << try_sp_ptr << ", align 4\n";
                    func_ << "  " << has_frame << " = icmp sgt i32 " << sp << ", 0\n";
                    func_ << "  br i1 " << has_frame << ", label %" << frame_ok
                          << ", label %panic\n";
                    func_ << frame_ok << ":\n";
                    const auto idx = Temp();
                    const auto idx64 = Temp();
                    func_ << "  " << idx << " = sub i32 " << sp << ", 1\n";
                    func_ << "  " << idx64 << " = zext i32 " << idx << " to i64\n";
                    const auto finally_slot = Temp();
                    func_ << "  " << finally_slot << " = getelementptr inbounds ["
                          << try_slot_count << " x i32], ptr " << try_finally_ptr
                          << ", i64 0, i64 " << idx64 << "\n";
                    const auto finally_target = Temp();
                    func_ << "  " << finally_target << " = load i32, ptr " << finally_slot
                          << ", align 4\n";
                    const auto has_finally = Temp();
                    func_ << "  " << has_finally << " = icmp ne i32 " << finally_target << ", "
                          << kInvalidTryTarget << "\n";
                    func_ << "  br i1 " << has_finally << ", label %" << with_finally
                          << ", label %" << without_finally << "\n";
                    func_ << with_finally << ":\n";
                    const auto rethrow_slot = Temp();
                    func_ << "  " << rethrow_slot << " = getelementptr inbounds ["
                          << try_slot_count << " x i1], ptr " << try_rethrow_ptr << ", i64 0, i64 "
                          << idx64 << "\n";
                    func_ << "  store i1 false, ptr " << rethrow_slot << ", align 1\n";
                    const auto pending_slot = Temp();
                    func_ << "  " << pending_slot << " = getelementptr inbounds ["
                          << try_slot_count << " x %TieValue], ptr " << try_pending_ptr
                          << ", i64 0, i64 " << idx64 << "\n";
                    func_ << "  store %TieValue " << NullValue() << ", ptr " << pending_slot
                          << ", align 8\n";
                    EmitPcSwitch(finally_target);
                    func_ << without_finally << ":\n";
                    const auto end_slot = Temp();
                    func_ << "  " << end_slot << " = getelementptr inbounds [" << try_slot_count
                          << " x i32], ptr " << try_end_ptr << ", i64 0, i64 " << idx64 << "\n";
                    const auto end_target = Temp();
                    func_ << "  " << end_target << " = load i32, ptr " << end_slot << ", align 4\n";
                    func_ << "  store i32 " << idx << ", ptr " << try_sp_ptr << ", align 4\n";
                    EmitPcSwitch(end_target);
                    break;
                }
                case OpCode::kEndFinally: {
                    if (try_slot_count == 0) {
                        return AotInvalid(
                            "LOWER_TRY_STACK",
                            "try stack not initialized",
                            module_.name(),
                            function.name(),
                            pc,
                            inst.opcode);
                    }
                    const auto frame_ok = Label("end_finally_frame_ok");
                    const auto rethrow_label = Label("end_finally_rethrow");
                    const auto normal_label = Label("end_finally_normal");
                    const auto sp = Temp();
                    const auto has_frame = Temp();
                    func_ << "  " << sp << " = load i32, ptr " << try_sp_ptr << ", align 4\n";
                    func_ << "  " << has_frame << " = icmp sgt i32 " << sp << ", 0\n";
                    func_ << "  br i1 " << has_frame << ", label %" << frame_ok
                          << ", label %panic\n";
                    func_ << frame_ok << ":\n";
                    const auto idx = Temp();
                    const auto idx64 = Temp();
                    func_ << "  " << idx << " = sub i32 " << sp << ", 1\n";
                    func_ << "  " << idx64 << " = zext i32 " << idx << " to i64\n";
                    const auto end_slot = Temp();
                    func_ << "  " << end_slot << " = getelementptr inbounds [" << try_slot_count
                          << " x i32], ptr " << try_end_ptr << ", i64 0, i64 " << idx64 << "\n";
                    const auto end_target = Temp();
                    func_ << "  " << end_target << " = load i32, ptr " << end_slot << ", align 4\n";
                    const auto rethrow_slot = Temp();
                    func_ << "  " << rethrow_slot << " = getelementptr inbounds [" << try_slot_count
                          << " x i1], ptr " << try_rethrow_ptr << ", i64 0, i64 " << idx64 << "\n";
                    const auto rethrow = Temp();
                    func_ << "  " << rethrow << " = load i1, ptr " << rethrow_slot
                          << ", align 1\n";
                    const auto pending_slot = Temp();
                    func_ << "  " << pending_slot << " = getelementptr inbounds ["
                          << try_slot_count << " x %TieValue], ptr " << try_pending_ptr
                          << ", i64 0, i64 " << idx64 << "\n";
                    const auto pending = Temp();
                    func_ << "  " << pending << " = load %TieValue, ptr " << pending_slot
                          << ", align 8\n";
                    func_ << "  store i32 " << idx << ", ptr " << try_sp_ptr << ", align 4\n";
                    func_ << "  br i1 " << rethrow << ", label %" << rethrow_label
                          << ", label %" << normal_label << "\n";
                    func_ << rethrow_label << ":\n";
                    func_ << "  store %TieValue " << pending << ", ptr " << throw_value_ptr
                          << ", align 8\n";
                    func_ << "  br label %" << throw_dispatch_label << "\n";
                    func_ << normal_label << ":\n";
                    EmitPcSwitch(end_target);
                    break;
                }
                case OpCode::kThrow: {
                    const auto thrown = LoadReg(function.reg_count(), inst.a);
                    func_ << "  store %TieValue " << thrown.value << ", ptr " << throw_value_ptr
                          << ", align 8\n";
                    func_ << "  br label %" << throw_dispatch_label << "\n";
                    break;
                }
                case OpCode::kRet: {
                    const auto src = LoadReg(function.reg_count(), inst.a);
                    func_ << "  ret %TieValue " << src.value << "\n";
                    break;
                }
                case OpCode::kHalt:
                    func_ << "  ret %TieValue { i8 0, i64 0 }\n";
                    break;
                case OpCode::kNewObject: {
                    if (inst.b >= module_.constants().size() ||
                        module_.constants()[inst.b].type != ConstantType::kUtf8) {
                        return AotInvalid(
                            "LOWER_NEW_OBJECT_CLASS",
                            "new_object requires utf8 class constant",
                            module_.name(),
                            function.name(),
                            pc,
                            inst.opcode);
                    }
                    const auto& class_name = module_.constants()[inst.b].utf8_value;
                    auto class_it = class_name_to_index_.find(class_name);
                    if (class_it == class_name_to_index_.end()) {
                        return AotInvalid(
                            "LOWER_NEW_OBJECT_CLASS",
                            "class not found in metadata: " + class_name,
                            module_.name(),
                            function.name(),
                            pc,
                            inst.opcode);
                    }
                    const auto obj_size_ptr = Temp();
                    const auto obj_size = Temp();
                    const auto obj_mem = Temp();
                    const auto class_slot = Temp();
                    const auto obj_bits = Temp();
                    func_ << "  " << obj_size_ptr
                          << " = getelementptr inbounds %TieAotObject, ptr null, i64 1\n";
                    func_ << "  " << obj_size << " = ptrtoint ptr " << obj_size_ptr << " to i64\n";
                    func_ << "  " << obj_mem << " = call ptr @malloc(i64 " << obj_size << ")\n";
                    func_ << "  " << class_slot
                          << " = getelementptr inbounds %TieAotObject, ptr " << obj_mem
                          << ", i32 0, i32 0\n";
                    func_ << "  store i32 " << class_it->second << ", ptr " << class_slot
                          << ", align 4\n";
                    func_ << "  " << obj_bits << " = ptrtoint ptr " << obj_mem << " to i64\n";
                    const auto value = MakeValue("i8 4", "i64 " + obj_bits);
                    StoreReg(function.reg_count(), inst.a, value);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                case OpCode::kInvoke: {
                    if (inst.b >= module_.constants().size() ||
                        module_.constants()[inst.b].type != ConstantType::kUtf8) {
                        return AotInvalid(
                            "LOWER_INVOKE_METHOD",
                            "invoke requires utf8 method constant",
                            module_.name(),
                            function.name(),
                            pc,
                            inst.opcode);
                    }
                    if (class_infos_.empty()) {
                        return AotInvalid(
                            "LOWER_INVOKE_CLASS_TABLE",
                            "invoke requires class metadata in module",
                            module_.name(),
                            function.name(),
                            pc,
                            inst.opcode);
                    }

                    const auto& method_name = module_.constants()[inst.b].utf8_value;
                    const auto object = LoadReg(function.reg_count(), inst.a);
                    const auto is_object = Temp();
                    const auto object_ok = Label("invoke_object_ok");
                    func_ << "  " << is_object << " = icmp eq i8 " << object.tag << ", 4\n";
                    func_ << "  br i1 " << is_object << ", label %" << object_ok
                          << ", label %panic\n";
                    func_ << object_ok << ":\n";
                    const auto object_ptr = Temp();
                    func_ << "  " << object_ptr << " = inttoptr i64 " << object.bits << " to ptr\n";
                    const auto class_slot = Temp();
                    func_ << "  " << class_slot
                          << " = getelementptr inbounds %TieAotObject, ptr " << object_ptr
                          << ", i32 0, i32 0\n";
                    const auto class_id = Temp();
                    func_ << "  " << class_id << " = load i32, ptr " << class_slot << ", align 4\n";

                    const uint32_t invoke_argc = inst.c + 1;
                    const auto argbuf = Temp();
                    func_ << "  " << argbuf << " = alloca [" << invoke_argc
                          << " x %TieValue], align 8\n";
                    const auto self_dst = Temp();
                    func_ << "  " << self_dst << " = getelementptr inbounds [" << invoke_argc
                          << " x %TieValue], ptr " << argbuf << ", i64 0, i64 0\n";
                    func_ << "  store %TieValue " << object.value << ", ptr " << self_dst
                          << ", align 8\n";
                    for (uint32_t i = 0; i < inst.c; ++i) {
                        const auto src = LoadReg(function.reg_count(), inst.a + 1 + i);
                        const auto dst = Temp();
                        func_ << "  " << dst << " = getelementptr inbounds [" << invoke_argc
                              << " x %TieValue], ptr " << argbuf << ", i64 0, i64 " << (i + 1)
                              << "\n";
                        func_ << "  store %TieValue " << src.value << ", ptr " << dst
                              << ", align 8\n";
                    }
                    const auto args_ptr = Temp();
                    func_ << "  " << args_ptr << " = getelementptr inbounds [" << invoke_argc
                          << " x %TieValue], ptr " << argbuf << ", i64 0, i64 0\n";

                    const auto merge_label = Label("invoke_merge");
                    std::vector<std::string> case_labels(class_infos_.size());
                    for (size_t i = 0; i < class_infos_.size(); ++i) {
                        case_labels[i] = Label("invoke_class_" + std::to_string(i));
                    }

                    func_ << "  switch i32 " << class_id << ", label %panic [\n";
                    for (size_t i = 0; i < class_infos_.size(); ++i) {
                        func_ << "    i32 " << i << ", label %" << case_labels[i] << "\n";
                    }
                    func_ << "  ]\n";

                    std::vector<std::string> merge_values;
                    std::vector<std::string> merge_preds;
                    for (size_t i = 0; i < class_infos_.size(); ++i) {
                        func_ << case_labels[i] << ":\n";
                        const auto resolved =
                            ResolveInvokeMethod(static_cast<uint32_t>(i), method_name);
                        if (resolved.kind != InvokeResolveKind::kFound) {
                            func_ << "  br label %panic\n";
                            continue;
                        }
                        const auto ret = Temp();
                        func_ << "  " << ret << " = call %TieValue "
                              << FunctionName(resolved.function_index) << "(ptr " << args_ptr
                              << ", i32 " << invoke_argc << ", ptr null)\n";
                        func_ << "  br label %" << merge_label << "\n";
                        merge_values.push_back(ret);
                        merge_preds.push_back(case_labels[i]);
                    }

                    if (merge_values.empty()) {
                        break;
                    }

                    func_ << merge_label << ":\n";
                    const auto merged = Temp();
                    func_ << "  " << merged << " = phi %TieValue ";
                    for (size_t i = 0; i < merge_values.size(); ++i) {
                        if (i > 0) {
                            func_ << ", ";
                        }
                        func_ << "[ " << merge_values[i] << ", %" << merge_preds[i] << " ]";
                    }
                    func_ << "\n";
                    StoreReg(function.reg_count(), inst.a, merged);
                    func_ << "  br label %" << next_label << "\n";
                    break;
                }
                default:
                    return AotUnsupported(
                        "unknown opcode",
                        module_.name(),
                        function.name(),
                        pc,
                        inst.opcode);
            }
        }

        func_ << throw_dispatch_label << ":\n";
        if (try_slot_count == 0) {
            func_ << "  br label %" << throw_unhandled_label << "\n";
        } else {
            const auto scan_init = Temp();
            func_ << "  " << scan_init << " = load i32, ptr " << try_sp_ptr << ", align 4\n";
            func_ << "  br label %" << throw_scan_label << "\n";

            func_ << throw_scan_label << ":\n";
            const auto scan_cur = Temp();
            const auto scan_next = Temp();
            const auto scan_has = Temp();
            func_ << "  " << scan_cur << " = phi i32 [ " << scan_init << ", %"
                  << throw_dispatch_label << " ], [ " << scan_next << ", %"
                  << throw_continue_label << " ]\n";
            func_ << "  " << scan_has << " = icmp sgt i32 " << scan_cur << ", 0\n";
            func_ << "  br i1 " << scan_has << ", label %" << throw_check_label
                  << ", label %" << throw_unhandled_label << "\n";

            func_ << throw_check_label << ":\n";
            const auto scan_idx = Temp();
            const auto scan_idx64 = Temp();
            func_ << "  " << scan_idx << " = sub i32 " << scan_cur << ", 1\n";
            func_ << "  " << scan_idx64 << " = zext i32 " << scan_idx << " to i64\n";
            const auto catch_slot = Temp();
            func_ << "  " << catch_slot << " = getelementptr inbounds [" << try_slot_count
                  << " x i32], ptr " << try_catch_ptr << ", i64 0, i64 " << scan_idx64 << "\n";
            const auto catch_target = Temp();
            func_ << "  " << catch_target << " = load i32, ptr " << catch_slot << ", align 4\n";
            const auto has_catch = Temp();
            func_ << "  " << has_catch << " = icmp ne i32 " << catch_target << ", "
                  << kInvalidTryTarget << "\n";
            func_ << "  br i1 " << has_catch << ", label %" << throw_to_catch_label
                  << ", label %" << throw_check_finally_label << "\n";

            func_ << throw_check_finally_label << ":\n";
            const auto finally_slot = Temp();
            func_ << "  " << finally_slot << " = getelementptr inbounds [" << try_slot_count
                  << " x i32], ptr " << try_finally_ptr << ", i64 0, i64 " << scan_idx64 << "\n";
            const auto finally_target = Temp();
            func_ << "  " << finally_target << " = load i32, ptr " << finally_slot
                  << ", align 4\n";
            const auto has_finally = Temp();
            func_ << "  " << has_finally << " = icmp ne i32 " << finally_target << ", "
                  << kInvalidTryTarget << "\n";
            func_ << "  br i1 " << has_finally << ", label %" << throw_to_finally_label
                  << ", label %" << throw_continue_label << "\n";

            func_ << throw_to_catch_label << ":\n";
            const auto pending_slot_catch = Temp();
            func_ << "  " << pending_slot_catch << " = getelementptr inbounds ["
                  << try_slot_count << " x %TieValue], ptr " << try_pending_ptr
                  << ", i64 0, i64 " << scan_idx64 << "\n";
            const auto throw_value_catch = Temp();
            func_ << "  " << throw_value_catch << " = load %TieValue, ptr " << throw_value_ptr
                  << ", align 8\n";
            func_ << "  store %TieValue " << throw_value_catch << ", ptr " << pending_slot_catch
                  << ", align 8\n";
            const auto rethrow_slot_catch = Temp();
            func_ << "  " << rethrow_slot_catch << " = getelementptr inbounds [" << try_slot_count
                  << " x i1], ptr " << try_rethrow_ptr << ", i64 0, i64 " << scan_idx64 << "\n";
            func_ << "  store i1 false, ptr " << rethrow_slot_catch << ", align 1\n";
            const auto new_sp_catch = Temp();
            func_ << "  " << new_sp_catch << " = add i32 " << scan_idx << ", 1\n";
            func_ << "  store i32 " << new_sp_catch << ", ptr " << try_sp_ptr << ", align 4\n";
            EmitPcSwitch(catch_target);

            func_ << throw_to_finally_label << ":\n";
            const auto pending_slot_finally = Temp();
            func_ << "  " << pending_slot_finally << " = getelementptr inbounds ["
                  << try_slot_count << " x %TieValue], ptr " << try_pending_ptr
                  << ", i64 0, i64 " << scan_idx64 << "\n";
            const auto throw_value_finally = Temp();
            func_ << "  " << throw_value_finally << " = load %TieValue, ptr " << throw_value_ptr
                  << ", align 8\n";
            func_ << "  store %TieValue " << throw_value_finally << ", ptr "
                  << pending_slot_finally << ", align 8\n";
            const auto rethrow_slot_finally = Temp();
            func_ << "  " << rethrow_slot_finally << " = getelementptr inbounds ["
                  << try_slot_count << " x i1], ptr " << try_rethrow_ptr << ", i64 0, i64 "
                  << scan_idx64 << "\n";
            func_ << "  store i1 true, ptr " << rethrow_slot_finally << ", align 1\n";
            const auto new_sp_finally = Temp();
            func_ << "  " << new_sp_finally << " = add i32 " << scan_idx << ", 1\n";
            func_ << "  store i32 " << new_sp_finally << ", ptr " << try_sp_ptr << ", align 4\n";
            EmitPcSwitch(finally_target);

            func_ << throw_continue_label << ":\n";
            func_ << "  " << scan_next << " = sub i32 " << scan_cur << ", 1\n";
            func_ << "  br label %" << throw_scan_label << "\n";
        }
        func_ << throw_unhandled_label << ":\n";
        func_ << "  br label %panic\n";

        func_ << "panic:\n";
        func_ << "  call void @abort()\n";
        func_ << "  unreachable\n";
        func_ << "}\n\n";

        out_ << func_.str();
        return Status::Ok();
    }

    const LoadedAotModule& loaded_;
    const Module& module_;
    AotOutputKind output_kind_;
    const std::vector<ExportedFunctionInfo>& exported_functions_;
    std::ostringstream out_;
    std::ostringstream func_;
    int temp_id_ = 0;
    size_t current_code_size_ = 0;
    std::unordered_map<uint32_t, std::string> utf8_const_to_global_;
    std::vector<std::string> globals_;
    std::vector<std::string> ffi_decls_;
    std::set<std::string> ffi_decl_dedup_;
    std::unordered_map<std::string, FfiLoweredInfo> ffi_map_;
    std::unordered_map<std::string, uint32_t> class_name_to_index_;
    std::vector<AotClassInfo> class_infos_;
};

Status WriteTextFile(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return Status::SerializationError(
            "[AOT] FILE_IO msg=failed creating output directory: " + ec.message());
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return Status::SerializationError(
            "[AOT] FILE_IO msg=failed opening output file: " + path.string());
    }
    out << text;
    if (!out.good()) {
        return Status::SerializationError(
            "[AOT] FILE_IO msg=failed writing output file: " + path.string());
    }
    return Status::Ok();
}

StatusOr<std::filesystem::path> ResolveHeaderPath(const AotCompileOptions& options) {
    if (options.emit_header.has_value()) {
        return options.emit_header.value();
    }
    if (options.output_executable.empty()) {
        return Status::InvalidArgument("[AOT] INPUT msg=output path is empty");
    }
    auto path = options.output_executable;
    path.replace_extension(".h");
    return path;
}

StatusOr<std::string> BuildExportHeader(
    const std::string& module_name,
    const std::vector<ExportedFunctionInfo>& exported_functions) {
    std::ostringstream out;
    out << "#pragma once\n\n";
    out << "#include <stdint.h>\n\n";
    out << "#include <stdbool.h>\n\n";
    out << "#if defined(_WIN32) || defined(__CYGWIN__)\n";
    out << "#  ifdef TIEVM_AOT_IMPORT\n";
    out << "#    define TIEVM_AOT_API __declspec(dllimport)\n";
    out << "#  else\n";
    out << "#    define TIEVM_AOT_API\n";
    out << "#  endif\n";
    out << "#else\n";
    out << "#  define TIEVM_AOT_API __attribute__((visibility(\"default\")))\n";
    out << "#endif\n\n";
    out << "#ifdef __cplusplus\n";
    out << "extern \"C\" {\n";
    out << "#endif\n\n";
    out << "typedef struct tievm_aot_value {\n";
    out << "    uint8_t tag;\n";
    out << "    int64_t bits;\n";
    out << "} tievm_aot_value;\n\n";
    out << "/* module: " << module_name << " */\n";
    for (const auto& exported : exported_functions) {
        if (exported.param_types.size() != exported.param_count) {
            return AotInvalid(
                "EXPORT_SIGNATURE",
                "exported function param type count mismatch while generating header",
                module_name,
                exported.name);
        }
        auto ret_type_or = BytecodeTypeToExportReturnCType(exported.return_type);
        if (!ret_type_or.ok()) {
            return ret_type_or.status();
        }
        out << "TIEVM_AOT_API " << ret_type_or.value() << " " << exported.name << "(";
        if (exported.param_count == 0) {
            out << "void";
        } else {
            for (uint16_t i = 0; i < exported.param_count; ++i) {
                auto c_type_or = BytecodeTypeToExportParamCType(exported.param_types[i]);
                if (!c_type_or.ok()) {
                    return c_type_or.status();
                }
                if (i > 0) {
                    out << ", ";
                }
                out << c_type_or.value() << " arg" << i;
            }
        }
        out << ");\n";
    }
    out << "\n";
    out << "#ifdef __cplusplus\n";
    out << "}  // extern \"C\"\n";
    out << "#endif\n";
    return out.str();
}

StatusOr<std::filesystem::path> ResolveIrPath(const AotCompileOptions& options) {
    if (options.emit_ir.has_value()) {
        return options.emit_ir.value();
    }
    return MakeTempDirPath("aot.ll");
}

StatusOr<std::filesystem::path> ResolveObjPath(
    const AotCompileOptions& options, const std::filesystem::path& ir_path) {
    if (options.emit_obj.has_value()) {
        return options.emit_obj.value();
    }
#if defined(_WIN32)
    return ir_path.parent_path() / (ir_path.stem().string() + ".obj");
#else
    return ir_path.parent_path() / (ir_path.stem().string() + ".o");
#endif
}

Status BuildObjectFile(
    const AotCompileOptions& options, const std::filesystem::path& ir_path,
    const std::filesystem::path& obj_path) {
    std::vector<std::string> cmd;
    cmd.push_back(options.clang_path);
    cmd.push_back("-c");
    cmd.push_back("-x");
    cmd.push_back("ir");
    cmd.push_back(ir_path.string());
    cmd.push_back("-Wno-override-module");
    cmd.push_back("-o");
    cmd.push_back(obj_path.string());
    cmd.push_back("-" + options.opt_level);
    if (options.output_kind == AotOutputKind::kSharedLibrary && !IsWindowsTarget(options)) {
        cmd.push_back("-fPIC");
    }
    if (options.target_triple.has_value()) {
        cmd.push_back("--target=" + *options.target_triple);
    }
    if (options.sysroot.has_value()) {
        cmd.push_back("--sysroot=" + options.sysroot->string());
    }
    cmd.insert(cmd.end(), options.cflags.begin(), options.cflags.end());
    return RunCommand(cmd);
}

Status LinkArtifact(
    const AotCompileOptions& options, const std::filesystem::path& obj_path,
    const std::filesystem::path& output_path,
    const std::vector<std::filesystem::path>& linked_libraries) {
    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);
    if (ec) {
        return Status::SerializationError(
            "[AOT] FILE_IO msg=failed creating output directory: " + ec.message());
    }

    std::vector<std::string> cmd;
    cmd.push_back(options.clang_path);
    cmd.push_back(obj_path.string());
    if (options.output_kind == AotOutputKind::kSharedLibrary) {
        cmd.push_back("-shared");
    }
    cmd.push_back("-o");
    cmd.push_back(output_path.string());
    if (options.target_triple.has_value()) {
        cmd.push_back("--target=" + *options.target_triple);
    }
    if (options.sysroot.has_value()) {
        cmd.push_back("--sysroot=" + options.sysroot->string());
    }
    cmd.insert(cmd.end(), options.ldflags.begin(), options.ldflags.end());
    for (const auto& lib : linked_libraries) {
        cmd.push_back(lib.string());
    }
    return RunCommand(cmd);
}

}  // namespace

StatusOr<AotCompileResult> AotCompiler::Compile(const AotCompileOptions& options) const {
    if (options.input_path.empty()) {
        return Status::InvalidArgument("[AOT] INPUT msg=input path is empty");
    }
    if (options.output_executable.empty()) {
        return Status::InvalidArgument("[AOT] INPUT msg=output path is empty");
    }
    if (options.opt_level != "O0" && options.opt_level != "O1" && options.opt_level != "O2" &&
        options.opt_level != "O3") {
        return Status::InvalidArgument("[AOT] INPUT msg=opt level must be one of O0/O1/O2/O3");
    }
    if (options.output_kind != AotOutputKind::kExecutable &&
        options.output_kind != AotOutputKind::kSharedLibrary) {
        return Status::InvalidArgument("[AOT] INPUT msg=unsupported output kind");
    }

    auto loaded_or = LoadInputModule(options);
    if (!loaded_or.ok()) {
        return loaded_or.status();
    }
    auto loaded = std::move(loaded_or.value());

    struct TempCleanup {
        std::optional<std::filesystem::path> path;
        ~TempCleanup() {
            if (path.has_value()) {
                std::error_code ec;
                std::filesystem::remove_all(*path, ec);
            }
        }
    } temp_cleanup{loaded.temp_materialized_root};

    auto verify = Verifier::Verify(loaded.module);
    if (!verify.status.ok()) {
        return AotInvalid("VERIFY", verify.status.message(), loaded.module_name);
    }

    const auto exported_functions = CollectExportedFunctions(loaded.module);
    static const std::unordered_set<std::string> kReservedExportNames = {
        "printf",
        "strlen",
        "malloc",
        "memcpy",
        "abort",
        "tievm_aot_is_truthy",
    };
    for (const auto& exported : exported_functions) {
        const auto& exported_fn = loaded.module.functions()[exported.function_index];
        if (exported_fn.upvalue_count() != 0) {
            return AotInvalid(
                "EXPORT_CLOSURE_UNSUPPORTED",
                "exported function cannot capture upvalues",
                loaded.module_name,
                exported.name);
        }
        if (kReservedExportNames.contains(exported.name) ||
            exported.name.rfind("tievm_fn_", 0) == 0) {
            return AotInvalid(
                "EXPORT_NAME_CONFLICT",
                "exported function name conflicts with reserved AOT symbol",
                loaded.module_name,
                exported.name);
        }
    }

    if (options.output_kind == AotOutputKind::kExecutable) {
        const auto entry_idx = loaded.module.entry_function();
        if (entry_idx >= loaded.module.functions().size()) {
            return AotInvalid("ENTRY", "entry function index out of range", loaded.module_name);
        }
        const auto& entry_fn = loaded.module.functions()[entry_idx];
        if (entry_fn.param_count() != 0 || entry_fn.is_vararg()) {
            return AotInvalid(
                "ENTRY_SIGNATURE",
                "entry function must be non-vararg with zero parameters for executable AOT",
                loaded.module_name,
                entry_fn.name());
        }
        for (const auto& exported : exported_functions) {
            if (exported.name == "main") {
                return AotInvalid(
                    "EXPORT_NAME_CONFLICT",
                    "exported function name 'main' conflicts with executable entry symbol",
                    loaded.module_name,
                    exported.name);
            }
        }
    } else if (exported_functions.empty()) {
        return AotInvalid(
            "EXPORT_REQUIRED",
            "shared library output requires at least one exported function",
            loaded.module_name);
    }

    auto ffi_status = ResolveFfiLibraries(&loaded);
    if (!ffi_status.ok()) {
        return ffi_status;
    }

    LlvmIrBuilder builder(loaded, options.output_kind, exported_functions);
    auto ir_or = builder.Build();
    if (!ir_or.ok()) {
        return ir_or.status();
    }

    auto ir_path_or = ResolveIrPath(options);
    if (!ir_path_or.ok()) {
        return ir_path_or.status();
    }
    const auto ir_path = ir_path_or.value();
    auto write_status = WriteTextFile(ir_path, ir_or.value());
    if (!write_status.ok()) {
        return write_status;
    }

    auto obj_path_or = ResolveObjPath(options, ir_path);
    if (!obj_path_or.ok()) {
        return obj_path_or.status();
    }
    const auto obj_path = obj_path_or.value();

    auto obj_status = BuildObjectFile(options, ir_path, obj_path);
    if (!obj_status.ok()) {
        return obj_status;
    }

    auto link_status = LinkArtifact(
        options, obj_path, options.output_executable, loaded.linked_libraries);
    if (!link_status.ok()) {
        return link_status;
    }

    std::optional<std::filesystem::path> header_path;
    if (options.emit_header.has_value() || !exported_functions.empty()) {
        auto header_path_or = ResolveHeaderPath(options);
        if (!header_path_or.ok()) {
            return header_path_or.status();
        }
        header_path = header_path_or.value();
        auto header_text_or = BuildExportHeader(loaded.module_name, exported_functions);
        if (!header_text_or.ok()) {
            return header_text_or.status();
        }
        auto header_status = WriteTextFile(
            *header_path, header_text_or.value());
        if (!header_status.ok()) {
            return header_status;
        }
    }

    if (!options.emit_ir.has_value()) {
        std::error_code ec;
        std::filesystem::remove(ir_path, ec);
    }
    if (!options.emit_obj.has_value()) {
        std::error_code ec;
        std::filesystem::remove(obj_path, ec);
    }

    AotCompileResult result;
    result.output_executable = options.output_executable;
    result.output_kind = options.output_kind;
    if (options.emit_ir.has_value()) {
        result.emitted_ir = ir_path;
    }
    if (options.emit_obj.has_value()) {
        result.emitted_obj = obj_path;
    }
    if (header_path.has_value()) {
        result.emitted_header = *header_path;
    }
    result.compiled_module = loaded.module_name;
    result.linked_libraries = loaded.linked_libraries;
    result.target_triple = options.target_triple.value_or("host");
    result.exported_functions.reserve(exported_functions.size());
    for (const auto& exported : exported_functions) {
        result.exported_functions.push_back(exported.name);
    }
    return result;
}

Status AotMetadataEmitter::Emit(const AotUnit&, const std::filesystem::path&) const {
    return Status::Unsupported(
        "[AOT] DEPRECATED msg=AotMetadataEmitter is deprecated, use AotCompiler::Compile");
}

Status AotPipeline::AddUnit(AotUnit) {
    return Status::Unsupported(
        "[AOT] DEPRECATED msg=AotPipeline is deprecated, use AotCompiler::Compile");
}

StatusOr<std::vector<AotUnit>> AotPipeline::SnapshotUnits() const {
    return Status::Unsupported(
        "[AOT] DEPRECATED msg=AotPipeline is deprecated, use AotCompiler::Compile");
}

Status AotPipeline::EmitMetadataDirectory(const std::filesystem::path&) const {
    return Status::Unsupported(
        "[AOT] DEPRECATED msg=AotPipeline is deprecated, use AotCompiler::Compile");
}

}  // namespace tie::vm
