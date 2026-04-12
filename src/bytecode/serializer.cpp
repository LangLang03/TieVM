#include "tie/vm/bytecode/serializer.hpp"

#include <cstring>
#include <fstream>

#include "tie/vm/bytecode/verifier.hpp"

namespace tie::vm {

namespace {

constexpr uint8_t kMagic[4] = {'T', 'B', 'C', '0'};
constexpr uint16_t kFormatMajor = 0;
constexpr uint16_t kFormatMinor = 4;
constexpr uint16_t kLegacyFormatMinorV1 = 1;
constexpr uint16_t kLegacyFormatMinorV2 = 2;
constexpr uint16_t kLegacyFormatMinorV3 = 3;
constexpr uint32_t kFlagDebug = 1u << 0;
constexpr uint32_t kFlagFfiMetadata = 1u << 1;
constexpr uint32_t kFlagClassMetadata = 1u << 2;

void WriteU8(std::vector<uint8_t>& out, uint8_t value) { out.push_back(value); }

void WriteU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void WriteU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFu));
    }
}

void WriteU64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFu));
    }
}

void WriteString(std::vector<uint8_t>& out, const std::string& value) {
    WriteU32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void WriteAbiType(std::vector<uint8_t>& out, const AbiType& type) {
    WriteU8(out, static_cast<uint8_t>(type.kind));
    WriteU8(out, static_cast<uint8_t>(type.ownership));
    WriteU8(out, static_cast<uint8_t>(type.passing));
    WriteU8(out, 0);
    WriteU32(out, type.struct_index);
    WriteU32(out, type.size);
}

template <typename T>
StatusOr<T> ReadIntegral(const std::vector<uint8_t>& bytes, size_t* offset);

template <>
StatusOr<uint8_t> ReadIntegral<uint8_t>(const std::vector<uint8_t>& bytes, size_t* offset) {
    if (*offset + 1 > bytes.size()) {
        return Status::SerializationError("unexpected eof while reading u8");
    }
    return bytes[(*offset)++];
}

template <>
StatusOr<uint16_t> ReadIntegral<uint16_t>(
    const std::vector<uint8_t>& bytes, size_t* offset) {
    if (*offset + 2 > bytes.size()) {
        return Status::SerializationError("unexpected eof while reading u16");
    }
    const uint16_t v = static_cast<uint16_t>(bytes[*offset]) |
                       static_cast<uint16_t>(bytes[*offset + 1] << 8);
    *offset += 2;
    return v;
}

template <>
StatusOr<uint32_t> ReadIntegral<uint32_t>(
    const std::vector<uint8_t>& bytes, size_t* offset) {
    if (*offset + 4 > bytes.size()) {
        return Status::SerializationError("unexpected eof while reading u32");
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(bytes[*offset + i]) << (i * 8);
    }
    *offset += 4;
    return v;
}

template <>
StatusOr<uint64_t> ReadIntegral<uint64_t>(
    const std::vector<uint8_t>& bytes, size_t* offset) {
    if (*offset + 8 > bytes.size()) {
        return Status::SerializationError("unexpected eof while reading u64");
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(bytes[*offset + i]) << (i * 8);
    }
    *offset += 8;
    return v;
}

StatusOr<std::string> ReadString(const std::vector<uint8_t>& bytes, size_t* offset) {
    auto len_or = ReadIntegral<uint32_t>(bytes, offset);
    if (!len_or.ok()) {
        return len_or.status();
    }
    const uint32_t len = len_or.value();
    if (*offset + len > bytes.size()) {
        return Status::SerializationError("unexpected eof while reading string");
    }
    std::string value(
        reinterpret_cast<const char*>(bytes.data() + static_cast<ptrdiff_t>(*offset)), len);
    *offset += len;
    return value;
}

StatusOr<AbiType> ReadAbiType(const std::vector<uint8_t>& bytes, size_t* offset) {
    auto kind_or = ReadIntegral<uint8_t>(bytes, offset);
    auto ownership_or = ReadIntegral<uint8_t>(bytes, offset);
    auto passing_or = ReadIntegral<uint8_t>(bytes, offset);
    auto reserved_or = ReadIntegral<uint8_t>(bytes, offset);
    auto struct_or = ReadIntegral<uint32_t>(bytes, offset);
    auto size_or = ReadIntegral<uint32_t>(bytes, offset);
    if (!kind_or.ok() || !ownership_or.ok() || !passing_or.ok() || !reserved_or.ok() ||
        !struct_or.ok() || !size_or.ok()) {
        return Status::SerializationError("failed reading abi type");
    }
    AbiType type;
    type.kind = static_cast<AbiValueKind>(kind_or.value());
    type.ownership = static_cast<OwnershipQualifier>(ownership_or.value());
    type.passing = static_cast<FfiPassingMode>(passing_or.value());
    type.struct_index = struct_or.value();
    type.size = size_or.value();
    return type;
}

bool HasAnyFfiMetadata(const Module& module) {
    if (!module.ffi_library_paths().empty() || !module.ffi_structs().empty() ||
        !module.ffi_signatures().empty() || !module.ffi_bindings().empty()) {
        return true;
    }
    for (const auto& function : module.functions()) {
        if (function.ffi_binding().enabled) {
            return true;
        }
    }
    return false;
}

bool HasAnyClassMetadata(const Module& module) { return !module.classes().empty(); }

}  // namespace

StatusOr<std::vector<uint8_t>> Serializer::Serialize(
    const Module& module, bool include_debug_section) {
    auto verify = Verifier::Verify(module);
    if (!verify.status.ok()) {
        return verify.status;
    }

    std::vector<uint8_t> out;
    out.reserve(2048);
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    WriteU16(out, kFormatMajor);
    WriteU16(out, kFormatMinor);

    uint32_t flags = 0;
    if (include_debug_section) {
        flags |= kFlagDebug;
    }
    if (HasAnyFfiMetadata(module)) {
        flags |= kFlagFfiMetadata;
    }
    if (HasAnyClassMetadata(module)) {
        flags |= kFlagClassMetadata;
    }
    WriteU32(out, flags);
    WriteString(out, module.name());
    WriteU32(out, module.version().major);
    WriteU32(out, module.version().minor);
    WriteU32(out, module.version().patch);
    WriteU32(out, module.entry_function());
    WriteU32(out, static_cast<uint32_t>(module.constants().size()));
    WriteU32(out, static_cast<uint32_t>(module.functions().size()));

    for (const auto& constant : module.constants()) {
        WriteU8(out, static_cast<uint8_t>(constant.type));
        switch (constant.type) {
            case ConstantType::kInt64:
                WriteU64(out, static_cast<uint64_t>(constant.int64_value));
                break;
            case ConstantType::kFloat64: {
                uint64_t bits = 0;
                std::memcpy(&bits, &constant.float64_value, sizeof(bits));
                WriteU64(out, bits);
                break;
            }
            case ConstantType::kUtf8:
                WriteString(out, constant.utf8_value);
                break;
        }
    }

    for (const auto& function : module.functions()) {
        WriteString(out, function.name());
        WriteU16(out, function.reg_count());
        WriteU16(out, function.param_count());
        WriteU8(out, function.ffi_binding().enabled ? 1 : 0);
        WriteU8(out, static_cast<uint8_t>(function.ffi_binding().convention));
        WriteU16(out, 0);
        WriteU32(out, function.ffi_binding().signature_index);
        WriteU32(out, function.ffi_binding().binding_index);
        WriteU16(out, function.upvalue_count());
        WriteU8(out, function.is_vararg() ? 1 : 0);
        WriteU8(out, 0);

        const auto instructions = function.FlattenedInstructions();
        WriteU32(out, static_cast<uint32_t>(instructions.size()));
        for (const auto& inst : instructions) {
            WriteU8(out, static_cast<uint8_t>(inst.opcode));
            WriteU8(out, inst.flags);
            WriteU16(out, inst.reserved);
            WriteU32(out, inst.a);
            WriteU32(out, inst.b);
            WriteU32(out, inst.c);
        }
    }

    if ((flags & kFlagDebug) != 0u) {
        WriteU32(out, static_cast<uint32_t>(module.debug_lines().size()));
        for (const auto& line : module.debug_lines()) {
            WriteU32(out, line.function_index);
            WriteU32(out, line.instruction_index);
            WriteU32(out, line.line);
            WriteU32(out, line.column);
        }
    }

    if ((flags & kFlagFfiMetadata) != 0u) {
        WriteU32(out, static_cast<uint32_t>(module.ffi_library_paths().size()));
        for (const auto& path : module.ffi_library_paths()) {
            WriteString(out, path);
        }

        WriteU32(out, static_cast<uint32_t>(module.ffi_structs().size()));
        for (const auto& layout : module.ffi_structs()) {
            WriteString(out, layout.name);
            WriteU32(out, layout.size);
            WriteU32(out, layout.alignment);
            WriteU32(out, static_cast<uint32_t>(layout.fields.size()));
            for (const auto& field : layout.fields) {
                WriteU32(out, field.offset);
                WriteAbiType(out, field.type);
            }
        }

        WriteU32(out, static_cast<uint32_t>(module.ffi_signatures().size()));
        for (const auto& signature : module.ffi_signatures()) {
            WriteString(out, signature.name);
            WriteU8(out, static_cast<uint8_t>(signature.convention));
            WriteU8(out, 0);
            WriteU16(out, 0);
            WriteAbiType(out, signature.return_type);
            WriteU32(out, static_cast<uint32_t>(signature.params.size()));
            for (const auto& param : signature.params) {
                WriteAbiType(out, param);
            }
        }

        WriteU32(out, static_cast<uint32_t>(module.ffi_bindings().size()));
        for (const auto& binding : module.ffi_bindings()) {
            WriteString(out, binding.vm_symbol);
            WriteString(out, binding.native_symbol);
            WriteU32(out, binding.library_index);
            WriteU32(out, binding.signature_index);
        }
    }

    if ((flags & kFlagClassMetadata) != 0u) {
        WriteU32(out, static_cast<uint32_t>(module.classes().size()));
        for (const auto& class_decl : module.classes()) {
            WriteString(out, class_decl.name);
            WriteU32(out, static_cast<uint32_t>(class_decl.base_classes.size()));
            for (const auto& base : class_decl.base_classes) {
                WriteString(out, base);
            }
            WriteU32(out, static_cast<uint32_t>(class_decl.methods.size()));
            for (const auto& method : class_decl.methods) {
                WriteString(out, method.name);
                WriteU32(out, method.function_index);
                WriteU8(out, static_cast<uint8_t>(method.access));
                WriteU8(out, method.is_virtual ? 1 : 0);
                WriteU16(out, 0);
            }
        }
    }

    return out;
}

StatusOr<Module> Serializer::Deserialize(
    const std::vector<uint8_t>& bytes, const DeserializeOptions& options) {
    size_t offset = 0;
    if (bytes.size() < 4 || std::memcmp(bytes.data(), kMagic, 4) != 0) {
        return Status::SerializationError("invalid tbc magic");
    }
    offset += 4;

    auto format_major_or = ReadIntegral<uint16_t>(bytes, &offset);
    if (!format_major_or.ok()) {
        return format_major_or.status();
    }
    auto format_minor_or = ReadIntegral<uint16_t>(bytes, &offset);
    if (!format_minor_or.ok()) {
        return format_minor_or.status();
    }
    const uint16_t format_major = format_major_or.value();
    const uint16_t format_minor = format_minor_or.value();
    if (format_major != kFormatMajor) {
        return Status::SerializationError("unsupported tbc major version");
    }
    if (format_minor != kLegacyFormatMinorV1 && format_minor != kLegacyFormatMinorV2 &&
        format_minor != kLegacyFormatMinorV3 &&
        format_minor != kFormatMinor) {
        return Status::SerializationError("unsupported tbc minor version");
    }

    auto flags_or = ReadIntegral<uint32_t>(bytes, &offset);
    if (!flags_or.ok()) {
        return flags_or.status();
    }
    const uint32_t flags = flags_or.value();

    auto name_or = ReadString(bytes, &offset);
    if (!name_or.ok()) {
        return name_or.status();
    }
    Module module(name_or.value());

    auto vmaj_or = ReadIntegral<uint32_t>(bytes, &offset);
    auto vmin_or = ReadIntegral<uint32_t>(bytes, &offset);
    auto vpat_or = ReadIntegral<uint32_t>(bytes, &offset);
    if (!vmaj_or.ok() || !vmin_or.ok() || !vpat_or.ok()) {
        return Status::SerializationError("failed reading module version");
    }
    module.version() = SemanticVersion{vmaj_or.value(), vmin_or.value(), vpat_or.value()};

    auto entry_or = ReadIntegral<uint32_t>(bytes, &offset);
    auto const_count_or = ReadIntegral<uint32_t>(bytes, &offset);
    auto func_count_or = ReadIntegral<uint32_t>(bytes, &offset);
    if (!entry_or.ok() || !const_count_or.ok() || !func_count_or.ok()) {
        return Status::SerializationError("failed reading module header");
    }
    module.set_entry_function(entry_or.value());

    for (uint32_t i = 0; i < const_count_or.value(); ++i) {
        auto type_or = ReadIntegral<uint8_t>(bytes, &offset);
        if (!type_or.ok()) {
            return type_or.status();
        }
        const auto type = static_cast<ConstantType>(type_or.value());
        switch (type) {
            case ConstantType::kInt64: {
                auto v_or = ReadIntegral<uint64_t>(bytes, &offset);
                if (!v_or.ok()) {
                    return v_or.status();
                }
                module.AddConstant(Constant::Int64(static_cast<int64_t>(v_or.value())));
                break;
            }
            case ConstantType::kFloat64: {
                auto v_or = ReadIntegral<uint64_t>(bytes, &offset);
                if (!v_or.ok()) {
                    return v_or.status();
                }
                double f = 0.0;
                const uint64_t bits = v_or.value();
                std::memcpy(&f, &bits, sizeof(f));
                module.AddConstant(Constant::Float64(f));
                break;
            }
            case ConstantType::kUtf8: {
                auto s_or = ReadString(bytes, &offset);
                if (!s_or.ok()) {
                    return s_or.status();
                }
                module.AddConstant(Constant::Utf8(std::move(s_or.value())));
                break;
            }
            default:
                return Status::SerializationError("unknown constant type");
        }
    }

    for (uint32_t i = 0; i < func_count_or.value(); ++i) {
        auto fn_name_or = ReadString(bytes, &offset);
        auto reg_or = ReadIntegral<uint16_t>(bytes, &offset);
        auto param_or = ReadIntegral<uint16_t>(bytes, &offset);
        if (!fn_name_or.ok() || !reg_or.ok() || !param_or.ok()) {
            return Status::SerializationError("failed reading function header");
        }

        auto& fn = module.AddFunction(fn_name_or.value(), reg_or.value(), param_or.value());
        if (format_minor >= kLegacyFormatMinorV2) {
            auto ffi_enabled_or = ReadIntegral<uint8_t>(bytes, &offset);
            auto ffi_convention_or = ReadIntegral<uint8_t>(bytes, &offset);
            auto ffi_reserved_or = ReadIntegral<uint16_t>(bytes, &offset);
            auto ffi_signature_or = ReadIntegral<uint32_t>(bytes, &offset);
            auto ffi_binding_or = ReadIntegral<uint32_t>(bytes, &offset);
            if (!ffi_enabled_or.ok() || !ffi_convention_or.ok() || !ffi_reserved_or.ok() ||
                !ffi_signature_or.ok() || !ffi_binding_or.ok()) {
                return Status::SerializationError("failed reading ffi function header");
            }
            fn.ffi_binding().enabled = ffi_enabled_or.value() != 0;
            fn.ffi_binding().convention =
                static_cast<CallingConvention>(ffi_convention_or.value());
            fn.ffi_binding().signature_index = ffi_signature_or.value();
            fn.ffi_binding().binding_index = ffi_binding_or.value();
        }
        if (format_minor >= kFormatMinor) {
            auto upvalue_count_or = ReadIntegral<uint16_t>(bytes, &offset);
            auto vararg_enabled_or = ReadIntegral<uint8_t>(bytes, &offset);
            auto vararg_reserved_or = ReadIntegral<uint8_t>(bytes, &offset);
            if (!upvalue_count_or.ok() || !vararg_enabled_or.ok() || !vararg_reserved_or.ok()) {
                return Status::SerializationError("failed reading closure/vararg function header");
            }
            fn.set_upvalue_count(upvalue_count_or.value());
            fn.set_is_vararg(vararg_enabled_or.value() != 0);
        }

        auto inst_count_or = ReadIntegral<uint32_t>(bytes, &offset);
        if (!inst_count_or.ok()) {
            return Status::SerializationError("failed reading function instruction count");
        }

        auto& block = fn.AddBlock("entry");
        for (uint32_t j = 0; j < inst_count_or.value(); ++j) {
            auto opcode_or = ReadIntegral<uint8_t>(bytes, &offset);
            auto flags_or2 = ReadIntegral<uint8_t>(bytes, &offset);
            auto reserved_or = ReadIntegral<uint16_t>(bytes, &offset);
            auto a_or = ReadIntegral<uint32_t>(bytes, &offset);
            auto b_or = ReadIntegral<uint32_t>(bytes, &offset);
            auto c_or = ReadIntegral<uint32_t>(bytes, &offset);
            if (!opcode_or.ok() || !flags_or2.ok() || !reserved_or.ok() || !a_or.ok() ||
                !b_or.ok() || !c_or.ok()) {
                return Status::SerializationError("failed reading instruction");
            }
            block.Append(Instruction{
                static_cast<OpCode>(opcode_or.value()),
                flags_or2.value(),
                reserved_or.value(),
                a_or.value(),
                b_or.value(),
                c_or.value(),
            });
        }
    }

    if ((flags & kFlagDebug) != 0u) {
        auto debug_count_or = ReadIntegral<uint32_t>(bytes, &offset);
        if (!debug_count_or.ok()) {
            return debug_count_or.status();
        }
        for (uint32_t i = 0; i < debug_count_or.value(); ++i) {
            auto fn_or = ReadIntegral<uint32_t>(bytes, &offset);
            auto ip_or = ReadIntegral<uint32_t>(bytes, &offset);
            auto line_or = ReadIntegral<uint32_t>(bytes, &offset);
            auto col_or = ReadIntegral<uint32_t>(bytes, &offset);
            if (!fn_or.ok() || !ip_or.ok() || !line_or.ok() || !col_or.ok()) {
                return Status::SerializationError("failed reading debug line");
            }
            module.AddDebugLine(
                DebugLineEntry{fn_or.value(), ip_or.value(), line_or.value(), col_or.value()});
        }
    }

    if ((flags & kFlagFfiMetadata) != 0u) {
        if (format_minor < kLegacyFormatMinorV2) {
            return Status::SerializationError("ffi metadata requires tbc format v0.2+");
        }

        auto lib_count_or = ReadIntegral<uint32_t>(bytes, &offset);
        if (!lib_count_or.ok()) {
            return lib_count_or.status();
        }
        for (uint32_t i = 0; i < lib_count_or.value(); ++i) {
            auto path_or = ReadString(bytes, &offset);
            if (!path_or.ok()) {
                return path_or.status();
            }
            module.AddFfiLibraryPath(std::move(path_or.value()));
        }

        auto struct_count_or = ReadIntegral<uint32_t>(bytes, &offset);
        if (!struct_count_or.ok()) {
            return struct_count_or.status();
        }
        for (uint32_t i = 0; i < struct_count_or.value(); ++i) {
            auto name_or = ReadString(bytes, &offset);
            auto size_or = ReadIntegral<uint32_t>(bytes, &offset);
            auto align_or = ReadIntegral<uint32_t>(bytes, &offset);
            auto field_count_or = ReadIntegral<uint32_t>(bytes, &offset);
            if (!name_or.ok() || !size_or.ok() || !align_or.ok() || !field_count_or.ok()) {
                return Status::SerializationError("failed reading ffi struct header");
            }
            FfiStructLayout layout;
            layout.name = std::move(name_or.value());
            layout.size = size_or.value();
            layout.alignment = align_or.value();
            for (uint32_t f = 0; f < field_count_or.value(); ++f) {
                auto offset_or = ReadIntegral<uint32_t>(bytes, &offset);
                auto type_or = ReadAbiType(bytes, &offset);
                if (!offset_or.ok() || !type_or.ok()) {
                    return Status::SerializationError("failed reading ffi struct field");
                }
                layout.fields.push_back(FfiStructField{offset_or.value(), type_or.value()});
            }
            module.AddFfiStruct(std::move(layout));
        }

        auto sig_count_or = ReadIntegral<uint32_t>(bytes, &offset);
        if (!sig_count_or.ok()) {
            return sig_count_or.status();
        }
        for (uint32_t i = 0; i < sig_count_or.value(); ++i) {
            auto name_or = ReadString(bytes, &offset);
            auto convention_or = ReadIntegral<uint8_t>(bytes, &offset);
            auto reserved8_or = ReadIntegral<uint8_t>(bytes, &offset);
            auto reserved16_or = ReadIntegral<uint16_t>(bytes, &offset);
            auto ret_or = ReadAbiType(bytes, &offset);
            auto param_count_or = ReadIntegral<uint32_t>(bytes, &offset);
            if (!name_or.ok() || !convention_or.ok() || !reserved8_or.ok() ||
                !reserved16_or.ok() || !ret_or.ok() || !param_count_or.ok()) {
                return Status::SerializationError("failed reading ffi signature header");
            }
            FunctionSignature signature;
            signature.name = std::move(name_or.value());
            signature.convention = static_cast<CallingConvention>(convention_or.value());
            signature.return_type = ret_or.value();
            for (uint32_t p = 0; p < param_count_or.value(); ++p) {
                auto param_or = ReadAbiType(bytes, &offset);
                if (!param_or.ok()) {
                    return Status::SerializationError("failed reading ffi signature param");
                }
                signature.params.push_back(param_or.value());
            }
            module.AddFfiSignature(std::move(signature));
        }

        auto binding_count_or = ReadIntegral<uint32_t>(bytes, &offset);
        if (!binding_count_or.ok()) {
            return binding_count_or.status();
        }
        for (uint32_t i = 0; i < binding_count_or.value(); ++i) {
            auto vm_symbol_or = ReadString(bytes, &offset);
            auto native_symbol_or = ReadString(bytes, &offset);
            auto lib_index_or = ReadIntegral<uint32_t>(bytes, &offset);
            auto sig_index_or = ReadIntegral<uint32_t>(bytes, &offset);
            if (!vm_symbol_or.ok() || !native_symbol_or.ok() || !lib_index_or.ok() ||
                !sig_index_or.ok()) {
                return Status::SerializationError("failed reading ffi binding");
            }
            module.AddFfiBinding(FfiSymbolBinding{
                std::move(vm_symbol_or.value()),
                std::move(native_symbol_or.value()),
                lib_index_or.value(),
                sig_index_or.value()});
        }
    }

    if ((flags & kFlagClassMetadata) != 0u) {
        if (format_minor < kFormatMinor) {
            return Status::SerializationError("class metadata requires tbc format v0.4+");
        }
        auto class_count_or = ReadIntegral<uint32_t>(bytes, &offset);
        if (!class_count_or.ok()) {
            return class_count_or.status();
        }
        for (uint32_t i = 0; i < class_count_or.value(); ++i) {
            auto class_name_or = ReadString(bytes, &offset);
            auto base_count_or = ReadIntegral<uint32_t>(bytes, &offset);
            if (!class_name_or.ok() || !base_count_or.ok()) {
                return Status::SerializationError("failed reading class metadata header");
            }
            BytecodeClassDecl class_decl;
            class_decl.name = std::move(class_name_or.value());
            for (uint32_t b = 0; b < base_count_or.value(); ++b) {
                auto base_or = ReadString(bytes, &offset);
                if (!base_or.ok()) {
                    return Status::SerializationError("failed reading class base metadata");
                }
                class_decl.base_classes.push_back(std::move(base_or.value()));
            }
            auto method_count_or = ReadIntegral<uint32_t>(bytes, &offset);
            if (!method_count_or.ok()) {
                return Status::SerializationError("failed reading class method count");
            }
            for (uint32_t m = 0; m < method_count_or.value(); ++m) {
                auto method_name_or = ReadString(bytes, &offset);
                auto function_index_or = ReadIntegral<uint32_t>(bytes, &offset);
                auto access_or = ReadIntegral<uint8_t>(bytes, &offset);
                auto is_virtual_or = ReadIntegral<uint8_t>(bytes, &offset);
                auto reserved_or = ReadIntegral<uint16_t>(bytes, &offset);
                if (!method_name_or.ok() || !function_index_or.ok() || !access_or.ok() ||
                    !is_virtual_or.ok() || !reserved_or.ok()) {
                    return Status::SerializationError("failed reading class method metadata");
                }
                if (access_or.value() >
                    static_cast<uint8_t>(BytecodeAccessModifier::kPrivate)) {
                    return Status::SerializationError(
                        "class method access modifier out of range");
                }
                class_decl.methods.push_back(BytecodeMethodDecl{
                    std::move(method_name_or.value()),
                    function_index_or.value(),
                    static_cast<BytecodeAccessModifier>(access_or.value()),
                    is_virtual_or.value() != 0});
            }
            module.AddClass(std::move(class_decl));
        }
    }

    if (offset != bytes.size()) {
        return Status::SerializationError("trailing bytes after tbc payload");
    }

    if (options.verify) {
        auto verify = Verifier::Verify(module);
        if (!verify.status.ok()) {
            return verify.status;
        }
    }
    return module;
}

Status Serializer::SerializeToFile(
    const Module& module, const std::filesystem::path& path, bool include_debug_section) {
    auto bytes_or = Serialize(module, include_debug_section);
    if (!bytes_or.ok()) {
        return bytes_or.status();
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return Status::SerializationError("failed opening output file");
    }
    const auto& bytes = bytes_or.value();
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out.good()) {
        return Status::SerializationError("failed writing output file");
    }
    return Status::Ok();
}

StatusOr<Module> Serializer::DeserializeFromFile(
    const std::filesystem::path& path, const DeserializeOptions& options) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return Status::NotFound("input file not found");
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Deserialize(bytes, options);
}

}  // namespace tie::vm
