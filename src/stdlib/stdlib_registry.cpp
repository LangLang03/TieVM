#include "tie/vm/stdlib/stdlib_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

#include "tie/vm/ffi/ffi_bridge.hpp"
#include "tie/vm/runtime/vm_instance.hpp"
#include "tie/vm/runtime/vm_thread.hpp"
#include "tie/vm/utf8/utf8.hpp"

namespace tie::vm {

namespace {

StatusOr<std::string> RequireString(VmThread& thread, const Value& value) {
    if (value.type() != Value::Type::kString) {
        return Status::InvalidArgument("expected utf8 string argument");
    }
    return thread.owner().ResolveString(value);
}

Status RegisterFunction(
    VmInstance* vm, const std::string& name, std::vector<AbiType> params,
    AbiType return_type, FfiBridge::NativeFunction fn) {
    FunctionSignature signature;
    signature.name = name;
    signature.convention = CallingConvention::kSystem;
    signature.params = std::move(params);
    signature.return_type = return_type;
    return vm->ffi().RegisterFunction(std::move(signature), std::move(fn));
}

std::string EncodeCodePoint(uint32_t cp) {
    std::string out;
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
    return out;
}

bool IsIpv4(const std::string& text) {
    std::istringstream stream(text);
    std::string part;
    int count = 0;
    while (std::getline(stream, part, '.')) {
        if (part.empty() || part.size() > 3) {
            return false;
        }
        for (const char ch : part) {
            if (ch < '0' || ch > '9') {
                return false;
            }
        }
        const int value = std::stoi(part);
        if (value < 0 || value > 255) {
            return false;
        }
        ++count;
    }
    return count == 4;
}

Status RegisterString(VmInstance* vm) {
    {
        auto status = RegisterFunction(
            vm, "tie.std.string.concat",
            {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed},
             {AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kUtf8, OwnershipQualifier::kOwned},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                auto lhs_or = RequireString(thread, args[0]);
                if (!lhs_or.ok()) {
                    return lhs_or.status();
                }
                auto rhs_or = RequireString(thread, args[1]);
                if (!rhs_or.ok()) {
                    return rhs_or.status();
                }
                return thread.owner().InternString(lhs_or.value() + rhs_or.value());
            });
        if (!status.ok()) {
            return status;
        }
    }
    {
        auto status = RegisterFunction(
            vm, "tie.std.string.length",
            {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kI64, OwnershipQualifier::kBorrowed},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                auto text_or = RequireString(thread, args[0]);
                if (!text_or.ok()) {
                    return text_or.status();
                }
                auto count_or = utf8::CountCodePoints(text_or.value());
                if (!count_or.ok()) {
                    return count_or.status();
                }
                return Value::Int64(static_cast<int64_t>(count_or.value()));
            });
        if (!status.ok()) {
            return status;
        }
    }
    {
        auto status = RegisterFunction(
            vm, "tie.std.string.utf8_validate",
            {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kBool, OwnershipQualifier::kBorrowed},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                auto text_or = RequireString(thread, args[0]);
                if (!text_or.ok()) {
                    return text_or.status();
                }
                return Value::Bool(utf8::Validate(text_or.value()).ok());
            });
        if (!status.ok()) {
            return status;
        }
    }
    {
        auto status = RegisterFunction(
            vm, "tie.std.string.codepoints",
            {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kI64, OwnershipQualifier::kBorrowed},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                auto text_or = RequireString(thread, args[0]);
                if (!text_or.ok()) {
                    return text_or.status();
                }
                auto cps_or = utf8::DecodeCodePoints(text_or.value());
                if (!cps_or.ok()) {
                    return cps_or.status();
                }
                return Value::Int64(static_cast<int64_t>(cps_or.value().size()));
            });
        if (!status.ok()) {
            return status;
        }
    }
    {
        auto status = RegisterFunction(
            vm, "tie.std.string.slice",
            {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed},
             {AbiValueKind::kI64, OwnershipQualifier::kBorrowed},
             {AbiValueKind::kI64, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kUtf8, OwnershipQualifier::kOwned},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                auto text_or = RequireString(thread, args[0]);
                if (!text_or.ok()) {
                    return text_or.status();
                }
                const int64_t start = args[1].AsInt64();
                const int64_t len = args[2].AsInt64();
                if (start < 0 || len < 0) {
                    return Status::InvalidArgument("slice start/len cannot be negative");
                }

                auto cps_or = utf8::DecodeCodePoints(text_or.value());
                if (!cps_or.ok()) {
                    return cps_or.status();
                }
                const auto& cps = cps_or.value();
                const size_t begin = static_cast<size_t>(start);
                if (begin > cps.size()) {
                    return Status::InvalidArgument("slice start out of range");
                }
                const size_t end = std::min(cps.size(), begin + static_cast<size_t>(len));
                std::string out;
                for (size_t i = begin; i < end; ++i) {
                    out += EncodeCodePoint(cps[i]);
                }
                return thread.owner().InternString(out);
            });
        if (!status.ok()) {
            return status;
        }
    }
    return Status::Ok();
}

Status RegisterCollections(VmInstance* vm) {
    {
        auto status = RegisterFunction(
            vm, "tie.std.collections.array_new", {}, {AbiValueKind::kPointer, OwnershipQualifier::kOwned},
            [](VmThread& thread, const std::vector<Value>&) -> StatusOr<Value> {
                return thread.owner().CreateArray();
            });
        if (!status.ok()) {
            return status;
        }
    }
    {
        auto status = RegisterFunction(
            vm, "tie.std.collections.array_push",
            {{AbiValueKind::kPointer, OwnershipQualifier::kBorrowed},
             {AbiValueKind::kObject, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kI64, OwnershipQualifier::kBorrowed},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                auto size_or = thread.owner().ArrayPush(args[0], args[1]);
                if (!size_or.ok()) {
                    return size_or.status();
                }
                return Value::Int64(size_or.value());
            });
        if (!status.ok()) {
            return status;
        }
    }
    {
        auto status = RegisterFunction(
            vm, "tie.std.collections.array_get",
            {{AbiValueKind::kPointer, OwnershipQualifier::kBorrowed},
             {AbiValueKind::kI64, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kObject, OwnershipQualifier::kBorrowed},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                return thread.owner().ArrayGet(args[0], args[1].AsInt64());
            });
        if (!status.ok()) {
            return status;
        }
    }
    {
        auto status = RegisterFunction(
            vm, "tie.std.collections.array_size",
            {{AbiValueKind::kPointer, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kI64, OwnershipQualifier::kBorrowed},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                auto size_or = thread.owner().ArraySize(args[0]);
                if (!size_or.ok()) {
                    return size_or.status();
                }
                return Value::Int64(size_or.value());
            });
        if (!status.ok()) {
            return status;
        }
    }
    {
        auto status = RegisterFunction(
            vm, "tie.std.collections.map_new", {}, {AbiValueKind::kPointer, OwnershipQualifier::kOwned},
            [](VmThread& thread, const std::vector<Value>&) -> StatusOr<Value> {
                return thread.owner().CreateMap();
            });
        if (!status.ok()) {
            return status;
        }
    }
    {
        auto status = RegisterFunction(
            vm, "tie.std.collections.map_set",
            {{AbiValueKind::kPointer, OwnershipQualifier::kBorrowed},
             {AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed},
             {AbiValueKind::kObject, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kBool, OwnershipQualifier::kBorrowed},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                auto key_or = RequireString(thread, args[1]);
                if (!key_or.ok()) {
                    return key_or.status();
                }
                const auto status = thread.owner().MapSet(args[0], key_or.value(), args[2]);
                if (!status.ok()) {
                    return status;
                }
                return Value::Bool(true);
            });
        if (!status.ok()) {
            return status;
        }
    }
    {
        auto status = RegisterFunction(
            vm, "tie.std.collections.map_get",
            {{AbiValueKind::kPointer, OwnershipQualifier::kBorrowed},
             {AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kObject, OwnershipQualifier::kBorrowed},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                auto key_or = RequireString(thread, args[1]);
                if (!key_or.ok()) {
                    return key_or.status();
                }
                return thread.owner().MapGet(args[0], key_or.value());
            });
        if (!status.ok()) {
            return status;
        }
    }
    {
        auto status = RegisterFunction(
            vm, "tie.std.collections.map_has",
            {{AbiValueKind::kPointer, OwnershipQualifier::kBorrowed},
             {AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kBool, OwnershipQualifier::kBorrowed},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                auto key_or = RequireString(thread, args[1]);
                if (!key_or.ok()) {
                    return key_or.status();
                }
                auto has_or = thread.owner().MapHas(args[0], key_or.value());
                if (!has_or.ok()) {
                    return has_or.status();
                }
                return Value::Bool(has_or.value());
            });
        if (!status.ok()) {
            return status;
        }
    }
    return Status::Ok();
}

Status RegisterIo(VmInstance* vm) {
    {
        auto status = RegisterFunction(
            vm, "tie.std.io.print",
            {{AbiValueKind::kObject, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kVoid, OwnershipQualifier::kBorrowed},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                std::string text;
                if (args[0].type() == Value::Type::kString) {
                    auto text_or = thread.owner().ResolveString(args[0]);
                    if (!text_or.ok()) {
                        return text_or.status();
                    }
                    text = text_or.value();
                } else {
                    text = args[0].ToString();
                }
                thread.owner().EmitOutputLine(text);
                return Value::Null();
            });
        if (!status.ok()) {
            return status;
        }
    }
    {
        auto status = RegisterFunction(
            vm, "tie.std.io.read_text",
            {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kUtf8, OwnershipQualifier::kOwned},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                auto path_or = RequireString(thread, args[0]);
                if (!path_or.ok()) {
                    return path_or.status();
                }
                std::ifstream in(path_or.value(), std::ios::binary);
                if (!in.is_open()) {
                    return Status::NotFound("read_text path not found");
                }
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                auto validate = utf8::Validate(text);
                if (!validate.ok()) {
                    return validate;
                }
                return thread.owner().InternString(text);
            });
        if (!status.ok()) {
            return status;
        }
    }
    {
        auto status = RegisterFunction(
            vm, "tie.std.io.write_text",
            {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed},
             {AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed}},
            {AbiValueKind::kBool, OwnershipQualifier::kBorrowed},
            [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
                auto path_or = RequireString(thread, args[0]);
                if (!path_or.ok()) {
                    return path_or.status();
                }
                auto text_or = RequireString(thread, args[1]);
                if (!text_or.ok()) {
                    return text_or.status();
                }
                std::ofstream out(path_or.value(), std::ios::binary | std::ios::trunc);
                if (!out.is_open()) {
                    return Status::RuntimeError("write_text failed to open path");
                }
                out.write(text_or.value().data(), static_cast<std::streamsize>(text_or.value().size()));
                if (!out.good()) {
                    return Status::RuntimeError("write_text failed to write data");
                }
                return Value::Bool(true);
            });
        if (!status.ok()) {
            return status;
        }
    }
    return Status::Ok();
}

Status RegisterConcurrent(VmInstance* vm) {
    return RegisterFunction(
        vm, "tie.std.concurrent.sleep_ms",
        {{AbiValueKind::kI64, OwnershipQualifier::kBorrowed}},
        {AbiValueKind::kVoid, OwnershipQualifier::kBorrowed},
        [](VmThread&, const std::vector<Value>& args) -> StatusOr<Value> {
            const int64_t millis = args[0].AsInt64();
            if (millis < 0) {
                return Status::InvalidArgument("sleep_ms cannot be negative");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(millis));
            return Value::Null();
        });
}

Status RegisterNet(VmInstance* vm) {
    return RegisterFunction(
        vm, "tie.std.net.is_ipv4",
        {{AbiValueKind::kUtf8, OwnershipQualifier::kBorrowed}},
        {AbiValueKind::kBool, OwnershipQualifier::kBorrowed},
        [](VmThread& thread, const std::vector<Value>& args) -> StatusOr<Value> {
            auto text_or = RequireString(thread, args[0]);
            if (!text_or.ok()) {
                return text_or.status();
            }
            return Value::Bool(IsIpv4(text_or.value()));
        });
}

}  // namespace

Status StdlibRegistry::RegisterCore(VmInstance* vm) {
    if (vm == nullptr) {
        return Status::InvalidArgument("vm cannot be null");
    }
    auto status = RegisterString(vm);
    if (!status.ok()) {
        return status;
    }
    status = RegisterCollections(vm);
    if (!status.ok()) {
        return status;
    }
    status = RegisterIo(vm);
    if (!status.ok()) {
        return status;
    }
    status = RegisterConcurrent(vm);
    if (!status.ok()) {
        return status;
    }
    return RegisterNet(vm);
}

}  // namespace tie::vm
