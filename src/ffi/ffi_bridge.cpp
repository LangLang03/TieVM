#include "tie/vm/ffi/ffi_bridge.hpp"

#include <cstring>
#include <filesystem>
#include <type_traits>
#include <utility>

#include "tie/vm/bytecode/module.hpp"
#include "tie/vm/runtime/vm_instance.hpp"
#include "tie/vm/runtime/vm_thread.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace tie::vm {

namespace {

struct AbiScalar {
    uint64_t u64 = 0;
    double f64 = 0.0;
};

bool IsFloatClass(const AbiType& type) {
    return type.kind == AbiValueKind::kF32 || type.kind == AbiValueKind::kF64;
}

template <typename T>
T ReadAs(const AbiScalar& value) {
    if constexpr (std::is_same_v<T, double>) {
        return value.f64;
    } else {
        return static_cast<T>(value.u64);
    }
}

template <size_t Index, uint64_t Mask>
using ArgByMask = std::conditional_t<((Mask >> Index) & 1ULL) != 0ULL, double, uint64_t>;

template <typename Ret, uint64_t Mask, size_t... I>
Ret CallMaskedImpl(void* symbol, const AbiScalar* args, std::index_sequence<I...>) {
    using Fn = Ret (*)(ArgByMask<I, Mask>...);
    auto* fn = reinterpret_cast<Fn>(symbol);
    return fn(ReadAs<ArgByMask<I, Mask>>(args[I])...);
}

template <typename Ret, size_t N, uint64_t Mask = 0>
Ret CallByMask(void* symbol, uint64_t runtime_mask, const AbiScalar* args) {
    if constexpr (Mask < (1ULL << N)) {
        if (runtime_mask == Mask) {
            return CallMaskedImpl<Ret, Mask>(symbol, args, std::make_index_sequence<N>{});
        }
        return CallByMask<Ret, N, Mask + 1>(symbol, runtime_mask, args);
    } else {
        return Ret{};
    }
}

template <size_t N, uint64_t Mask = 0>
void CallVoidByMask(void* symbol, uint64_t runtime_mask, const AbiScalar* args) {
    if constexpr (Mask < (1ULL << N)) {
        if (runtime_mask == Mask) {
            if constexpr (N == 0) {
                using F0 = void (*)();
                auto* fn = reinterpret_cast<F0>(symbol);
                fn();
            } else {
                if constexpr (N == 1) {
                    using F1 = void (*)(ArgByMask<0, Mask>);
                    auto* fn = reinterpret_cast<F1>(symbol);
                    fn(ReadAs<ArgByMask<0, Mask>>(args[0]));
                } else if constexpr (N == 2) {
                    using F2 = void (*)(ArgByMask<0, Mask>, ArgByMask<1, Mask>);
                    auto* fn = reinterpret_cast<F2>(symbol);
                    fn(ReadAs<ArgByMask<0, Mask>>(args[0]), ReadAs<ArgByMask<1, Mask>>(args[1]));
                } else if constexpr (N == 3) {
                    using F3 = void (*)(ArgByMask<0, Mask>, ArgByMask<1, Mask>, ArgByMask<2, Mask>);
                    auto* fn = reinterpret_cast<F3>(symbol);
                    fn(ReadAs<ArgByMask<0, Mask>>(args[0]), ReadAs<ArgByMask<1, Mask>>(args[1]),
                       ReadAs<ArgByMask<2, Mask>>(args[2]));
                } else if constexpr (N == 4) {
                    using F4 =
                        void (*)(ArgByMask<0, Mask>, ArgByMask<1, Mask>, ArgByMask<2, Mask>,
                                 ArgByMask<3, Mask>);
                    auto* fn = reinterpret_cast<F4>(symbol);
                    fn(ReadAs<ArgByMask<0, Mask>>(args[0]), ReadAs<ArgByMask<1, Mask>>(args[1]),
                       ReadAs<ArgByMask<2, Mask>>(args[2]), ReadAs<ArgByMask<3, Mask>>(args[3]));
                } else if constexpr (N == 5) {
                    using F5 =
                        void (*)(ArgByMask<0, Mask>, ArgByMask<1, Mask>, ArgByMask<2, Mask>,
                                 ArgByMask<3, Mask>, ArgByMask<4, Mask>);
                    auto* fn = reinterpret_cast<F5>(symbol);
                    fn(ReadAs<ArgByMask<0, Mask>>(args[0]), ReadAs<ArgByMask<1, Mask>>(args[1]),
                       ReadAs<ArgByMask<2, Mask>>(args[2]), ReadAs<ArgByMask<3, Mask>>(args[3]),
                       ReadAs<ArgByMask<4, Mask>>(args[4]));
                } else if constexpr (N == 6) {
                    using F6 =
                        void (*)(ArgByMask<0, Mask>, ArgByMask<1, Mask>, ArgByMask<2, Mask>,
                                 ArgByMask<3, Mask>, ArgByMask<4, Mask>, ArgByMask<5, Mask>);
                    auto* fn = reinterpret_cast<F6>(symbol);
                    fn(ReadAs<ArgByMask<0, Mask>>(args[0]), ReadAs<ArgByMask<1, Mask>>(args[1]),
                       ReadAs<ArgByMask<2, Mask>>(args[2]), ReadAs<ArgByMask<3, Mask>>(args[3]),
                       ReadAs<ArgByMask<4, Mask>>(args[4]), ReadAs<ArgByMask<5, Mask>>(args[5]));
                } else if constexpr (N == 7) {
                    using F7 =
                        void (*)(ArgByMask<0, Mask>, ArgByMask<1, Mask>, ArgByMask<2, Mask>,
                                 ArgByMask<3, Mask>, ArgByMask<4, Mask>, ArgByMask<5, Mask>,
                                 ArgByMask<6, Mask>);
                    auto* fn = reinterpret_cast<F7>(symbol);
                    fn(ReadAs<ArgByMask<0, Mask>>(args[0]), ReadAs<ArgByMask<1, Mask>>(args[1]),
                       ReadAs<ArgByMask<2, Mask>>(args[2]), ReadAs<ArgByMask<3, Mask>>(args[3]),
                       ReadAs<ArgByMask<4, Mask>>(args[4]), ReadAs<ArgByMask<5, Mask>>(args[5]),
                       ReadAs<ArgByMask<6, Mask>>(args[6]));
                } else {
                    using F8 =
                        void (*)(ArgByMask<0, Mask>, ArgByMask<1, Mask>, ArgByMask<2, Mask>,
                                 ArgByMask<3, Mask>, ArgByMask<4, Mask>, ArgByMask<5, Mask>,
                                 ArgByMask<6, Mask>, ArgByMask<7, Mask>);
                    auto* fn = reinterpret_cast<F8>(symbol);
                    fn(ReadAs<ArgByMask<0, Mask>>(args[0]), ReadAs<ArgByMask<1, Mask>>(args[1]),
                       ReadAs<ArgByMask<2, Mask>>(args[2]), ReadAs<ArgByMask<3, Mask>>(args[3]),
                       ReadAs<ArgByMask<4, Mask>>(args[4]), ReadAs<ArgByMask<5, Mask>>(args[5]),
                       ReadAs<ArgByMask<6, Mask>>(args[6]), ReadAs<ArgByMask<7, Mask>>(args[7]));
                }
            }
            return;
        }
        return CallVoidByMask<N, Mask + 1>(symbol, runtime_mask, args);
    }
}

template <typename Ret>
StatusOr<Ret> DispatchTypedCall(void* symbol, uint64_t mask, const std::vector<AbiScalar>& args) {
    switch (args.size()) {
        case 0:
            return CallByMask<Ret, 0>(symbol, mask, args.data());
        case 1:
            return CallByMask<Ret, 1>(symbol, mask, args.data());
        case 2:
            return CallByMask<Ret, 2>(symbol, mask, args.data());
        case 3:
            return CallByMask<Ret, 3>(symbol, mask, args.data());
        case 4:
            return CallByMask<Ret, 4>(symbol, mask, args.data());
        case 5:
            return CallByMask<Ret, 5>(symbol, mask, args.data());
        case 6:
            return CallByMask<Ret, 6>(symbol, mask, args.data());
        case 7:
            return CallByMask<Ret, 7>(symbol, mask, args.data());
        case 8:
            return CallByMask<Ret, 8>(symbol, mask, args.data());
        default:
            return Status::Unsupported("ffi max arity is 8 in current backend");
    }
}

Status DispatchVoidCall(void* symbol, uint64_t mask, const std::vector<AbiScalar>& args) {
    switch (args.size()) {
        case 0:
            CallVoidByMask<0>(symbol, mask, args.data());
            return Status::Ok();
        case 1:
            CallVoidByMask<1>(symbol, mask, args.data());
            return Status::Ok();
        case 2:
            CallVoidByMask<2>(symbol, mask, args.data());
            return Status::Ok();
        case 3:
            CallVoidByMask<3>(symbol, mask, args.data());
            return Status::Ok();
        case 4:
            CallVoidByMask<4>(symbol, mask, args.data());
            return Status::Ok();
        case 5:
            CallVoidByMask<5>(symbol, mask, args.data());
            return Status::Ok();
        case 6:
            CallVoidByMask<6>(symbol, mask, args.data());
            return Status::Ok();
        case 7:
            CallVoidByMask<7>(symbol, mask, args.data());
            return Status::Ok();
        case 8:
            CallVoidByMask<8>(symbol, mask, args.data());
            return Status::Ok();
        default:
            return Status::Unsupported("ffi max arity is 8 in current backend");
    }
}

void CloseLibraryHandle(void* handle) {
    if (handle == nullptr) {
        return;
    }
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

StatusOr<void*> OpenDynamicLibrary(const std::string& path) {
#ifdef _WIN32
    auto* handle = LoadLibraryA(path.c_str());
    if (handle == nullptr) {
        return Status::FfiError("failed loading dynamic library: " + path);
    }
    return reinterpret_cast<void*>(handle);
#else
    auto* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        return Status::FfiError("failed loading dynamic library: " + path);
    }
    return handle;
#endif
}

void* ResolveDynamicSymbol(void* library_handle, const std::string& symbol_name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(
        GetProcAddress(reinterpret_cast<HMODULE>(library_handle), symbol_name.c_str()));
#else
    return dlsym(library_handle, symbol_name.c_str());
#endif
}

StatusOr<uint64_t> ConvertValueToRawInt(
    VmThread& vm_thread, const AbiType& type, const Value& value, std::vector<std::string>& tmp_strings) {
    switch (type.kind) {
        case AbiValueKind::kBool:
            return value.AsBool() ? 1ULL : 0ULL;
        case AbiValueKind::kI8:
        case AbiValueKind::kU8:
        case AbiValueKind::kI16:
        case AbiValueKind::kU16:
        case AbiValueKind::kI32:
        case AbiValueKind::kU32:
        case AbiValueKind::kI64:
        case AbiValueKind::kU64:
            return static_cast<uint64_t>(value.AsInt64());
        case AbiValueKind::kPointer:
            return value.AsPointer();
        case AbiValueKind::kObject:
            return value.AsObject();
        case AbiValueKind::kUtf8: {
            if (value.type() == Value::Type::kPointer) {
                return value.AsPointer();
            }
            if (value.type() != Value::Type::kString) {
                return Status::InvalidArgument("ffi utf8 parameter requires string handle");
            }
            auto text_or = vm_thread.owner().ResolveString(value);
            if (!text_or.ok()) {
                return text_or.status();
            }
            tmp_strings.push_back(text_or.value());
            return reinterpret_cast<uint64_t>(tmp_strings.back().c_str());
        }
        case AbiValueKind::kStruct: {
            if (type.passing != FfiPassingMode::kValue) {
                return value.AsPointer();
            }
            if (type.size > 8) {
                return Status::Unsupported("ffi struct by value supports max size=8 for now");
            }
            const auto ptr = value.AsPointer();
            uint64_t raw = 0;
            std::memcpy(&raw, reinterpret_cast<const void*>(ptr), type.size);
            return raw;
        }
        case AbiValueKind::kF32:
        case AbiValueKind::kF64:
        case AbiValueKind::kVoid:
            return Status::InvalidArgument("ffi type cannot be converted to integer class");
    }
    return Status::InvalidArgument("unsupported ffi integer conversion");
}

StatusOr<double> ConvertValueToRawFloat(const Value& value) {
    if (value.type() == Value::Type::kFloat64) {
        return value.AsFloat64();
    }
    if (value.type() == Value::Type::kInt64) {
        return static_cast<double>(value.AsInt64());
    }
    return Status::InvalidArgument("ffi float parameter requires float64/int64 value");
}

Value ConvertRawResultToValue(VmThread& vm_thread, const AbiType& type, uint64_t raw_int, double raw_f64) {
    switch (type.kind) {
        case AbiValueKind::kVoid:
            return Value::Null();
        case AbiValueKind::kBool:
            return Value::Bool(raw_int != 0);
        case AbiValueKind::kI8:
        case AbiValueKind::kU8:
        case AbiValueKind::kI16:
        case AbiValueKind::kU16:
        case AbiValueKind::kI32:
        case AbiValueKind::kU32:
        case AbiValueKind::kI64:
        case AbiValueKind::kU64:
            return Value::Int64(static_cast<int64_t>(raw_int));
        case AbiValueKind::kF32:
            return Value::Float64(static_cast<double>(static_cast<float>(raw_f64)));
        case AbiValueKind::kF64:
            return Value::Float64(raw_f64);
        case AbiValueKind::kPointer:
        case AbiValueKind::kStruct:
            return Value::Pointer(raw_int);
        case AbiValueKind::kObject:
            return Value::Object(raw_int);
        case AbiValueKind::kUtf8: {
            const auto* text = reinterpret_cast<const char*>(raw_int);
            if (text == nullptr) {
                return Value::Null();
            }
            auto str_or = vm_thread.owner().InternString(text);
            if (str_or.ok()) {
                return str_or.value();
            }
            return Value::Null();
        }
    }
    return Value::Null();
}

std::string MakePlanCacheKey(
    const Module& module, uint32_t function_index, std::string_view symbol_name) {
    return module.name() + "#" + std::to_string(function_index) + ":" + std::string(symbol_name);
}

}  // namespace

FfiBridge::~FfiBridge() { CloseAllLibraries(); }

void FfiBridge::CloseAllLibraries() {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [_, handle] : dynamic_lib_handles_) {
        CloseLibraryHandle(handle);
    }
    dynamic_lib_handles_.clear();
    dynamic_plan_cache_.clear();
}

Status FfiBridge::RegisterFunction(FunctionSignature signature, NativeFunction fn) {
    if (signature.name.empty()) {
        return Status::InvalidArgument("ffi function name is empty");
    }
    if (!fn) {
        return Status::InvalidArgument("ffi function body is empty");
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (native_functions_.contains(signature.name)) {
        return Status::AlreadyExists("ffi function already registered");
    }
    std::string key = signature.name;
    native_functions_.insert({key, Entry{std::move(signature), std::move(fn)}});
    return Status::Ok();
}

StatusOr<void*> FfiBridge::LoadDynamicSymbol(
    const std::string& library_path, const std::string& symbol_name) const {
    void* handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = dynamic_lib_handles_.find(library_path);
        if (it != dynamic_lib_handles_.end()) {
            handle = it->second;
        }
    }

    if (handle == nullptr) {
        auto handle_or = OpenDynamicLibrary(library_path);
        if (!handle_or.ok()) {
            return handle_or.status();
        }
        handle = handle_or.value();
        std::lock_guard<std::mutex> lock(mu_);
        dynamic_lib_handles_.insert({library_path, handle});
    }

    auto* symbol = ResolveDynamicSymbol(handle, symbol_name);
    if (symbol == nullptr) {
        return Status::FfiError(
            "symbol not found in dynamic library: " + symbol_name + " from " + library_path);
    }
    return symbol;
}

StatusOr<FfiBridge::DynamicCallPlan> FfiBridge::ResolveDynamicPlan(
    const Module& module, uint32_t function_index, std::string_view vm_symbol) const {
    const std::string cache_key = MakePlanCacheKey(module, function_index, vm_symbol);
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = dynamic_plan_cache_.find(cache_key);
        if (it != dynamic_plan_cache_.end()) {
            return it->second;
        }
    }

    std::optional<uint32_t> binding_index;
    if (function_index < module.functions().size() &&
        module.functions()[function_index].ffi_binding().enabled) {
        const auto& header = module.functions()[function_index].ffi_binding();
        binding_index = header.binding_index;
    } else {
        for (uint32_t i = 0; i < module.ffi_bindings().size(); ++i) {
            if (module.ffi_bindings()[i].vm_symbol == vm_symbol) {
                binding_index = i;
                break;
            }
        }
    }
    if (!binding_index.has_value()) {
        return Status::NotFound("ffi symbol binding not found: " + std::string(vm_symbol));
    }
    if (*binding_index >= module.ffi_bindings().size()) {
        return Status::InvalidState("ffi binding index out of range");
    }

    const auto& binding = module.ffi_bindings()[*binding_index];
    if (binding.library_index >= module.ffi_library_paths().size()) {
        return Status::InvalidState("ffi library index out of range");
    }
    if (binding.signature_index >= module.ffi_signatures().size()) {
        return Status::InvalidState("ffi signature index out of range");
    }

    DynamicCallPlan plan;
    plan.signature = module.ffi_signatures()[binding.signature_index];
    plan.library_path = module.ffi_library_paths()[binding.library_index];
    plan.symbol_name = binding.native_symbol;
    auto symbol_or = LoadDynamicSymbol(plan.library_path, plan.symbol_name);
    if (!symbol_or.ok()) {
        return symbol_or.status();
    }
    plan.symbol = symbol_or.value();

    std::lock_guard<std::mutex> lock(mu_);
    dynamic_plan_cache_.insert({cache_key, plan});
    return plan;
}

Status FfiBridge::ValidateModuleBindings(const Module& module) const {
    std::unordered_set<std::string> dedup;
    for (const auto& binding : module.ffi_bindings()) {
        if (!dedup.insert(binding.vm_symbol).second) {
            return Status::InvalidArgument("duplicate ffi vm symbol: " + binding.vm_symbol);
        }
        if (binding.library_index >= module.ffi_library_paths().size()) {
            return Status::InvalidArgument("ffi library index out of range");
        }
        if (binding.signature_index >= module.ffi_signatures().size()) {
            return Status::InvalidArgument("ffi signature index out of range");
        }
        if (!std::filesystem::exists(module.ffi_library_paths()[binding.library_index])) {
            return Status::NotFound(
                "ffi library not found: " + module.ffi_library_paths()[binding.library_index]);
        }
        auto symbol_or = LoadDynamicSymbol(
            module.ffi_library_paths()[binding.library_index], binding.native_symbol);
        if (!symbol_or.ok()) {
            return symbol_or.status();
        }
    }
    return Status::Ok();
}

StatusOr<Value> FfiBridge::InvokeDynamicPlan(
    const DynamicCallPlan& plan, VmThread& vm_thread, const std::vector<Value>& args) const {
    if (args.size() != plan.signature.params.size()) {
        return Status::InvalidArgument("ffi argument count mismatch");
    }

    if (plan.symbol_name == "tie_std_io_print" && args.size() == 1) {
        if (args[0].type() == Value::Type::kString) {
            auto text_or = vm_thread.owner().ResolveString(args[0]);
            if (text_or.ok()) {
                vm_thread.owner().EmitOutputLine(text_or.value());
                return Value::Null();
            }
        }
        vm_thread.owner().EmitOutputLine(args[0].ToString());
        return Value::Null();
    }

    std::vector<std::string> temp_strings;
    temp_strings.reserve(args.size());
    std::vector<AbiScalar> raw_args;
    raw_args.resize(args.size());
    uint64_t float_mask = 0;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& param_type = plan.signature.params[i];
        if (IsFloatClass(param_type)) {
            auto f_or = ConvertValueToRawFloat(args[i]);
            if (!f_or.ok()) {
                return f_or.status();
            }
            raw_args[i].f64 = f_or.value();
            float_mask |= (1ULL << i);
        } else {
            auto v_or = ConvertValueToRawInt(vm_thread, param_type, args[i], temp_strings);
            if (!v_or.ok()) {
                return v_or.status();
            }
            raw_args[i].u64 = v_or.value();
        }
    }

    if (plan.signature.return_type.kind == AbiValueKind::kVoid) {
        auto status = DispatchVoidCall(plan.symbol, float_mask, raw_args);
        if (!status.ok()) {
            return status;
        }
        return Value::Null();
    }
    if (plan.signature.return_type.kind == AbiValueKind::kF32) {
        auto ret_or = DispatchTypedCall<float>(plan.symbol, float_mask, raw_args);
        if (!ret_or.ok()) {
            return ret_or.status();
        }
        return ConvertRawResultToValue(vm_thread, plan.signature.return_type, 0, ret_or.value());
    }
    if (plan.signature.return_type.kind == AbiValueKind::kF64) {
        auto ret_or = DispatchTypedCall<double>(plan.symbol, float_mask, raw_args);
        if (!ret_or.ok()) {
            return ret_or.status();
        }
        return ConvertRawResultToValue(vm_thread, plan.signature.return_type, 0, ret_or.value());
    }

    auto ret_or = DispatchTypedCall<uint64_t>(plan.symbol, float_mask, raw_args);
    if (!ret_or.ok()) {
        return ret_or.status();
    }
    return ConvertRawResultToValue(vm_thread, plan.signature.return_type, ret_or.value(), 0.0);
}

StatusOr<Value> FfiBridge::CallFunction(
    std::string_view name, VmThread& vm_thread, const std::vector<Value>& args) const {
    Entry entry;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = native_functions_.find(std::string(name));
        if (it == native_functions_.end()) {
            return Status::NotFound("ffi symbol not found");
        }
        entry = it->second;
    }
    if (args.size() != entry.signature.params.size()) {
        return Status::InvalidArgument("ffi argument count mismatch");
    }
    return entry.function(vm_thread, args);
}

StatusOr<Value> FfiBridge::CallBoundFunction(
    const Module& module, uint32_t function_index, std::string_view vm_symbol,
    VmThread& vm_thread, const std::vector<Value>& args) const {
    if (module.ffi_bindings().empty()) {
        return CallFunction(vm_symbol, vm_thread, args);
    }
    auto plan_or = ResolveDynamicPlan(module, function_index, vm_symbol);
    if (!plan_or.ok()) {
        VmError error{plan_or.status().message(), {}};
        error.PushFrame(StackFrame{
            StackFrameKind::kFfi,
            module.name(),
            "",
            0,
            0,
            0,
            "",
            std::string(vm_symbol)});
        return Status::FfiError(std::move(error));
    }
    auto value_or = InvokeDynamicPlan(plan_or.value(), vm_thread, args);
    if (!value_or.ok()) {
        VmError error{value_or.status().message(), {}};
        error.PushFrame(StackFrame{
            StackFrameKind::kFfi,
            module.name(),
            "",
            0,
            0,
            0,
            plan_or.value().library_path,
            plan_or.value().symbol_name});
        return Status::FfiError(std::move(error));
    }
    return value_or;
}

Status FfiBridge::RegisterVmCallback(std::string name, VmCallback callback) {
    if (name.empty()) {
        return Status::InvalidArgument("callback name is empty");
    }
    if (!callback) {
        return Status::InvalidArgument("callback body is empty");
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (callbacks_.contains(name)) {
        return Status::AlreadyExists("callback already registered");
    }
    callbacks_.insert({std::move(name), std::move(callback)});
    return Status::Ok();
}

StatusOr<Value> FfiBridge::InvokeVmCallback(
    std::string_view name, const std::vector<Value>& args) const {
    if (!IsCurrentThreadAttached()) {
        return Status::FfiError("external thread is not attached to VM");
    }
    VmCallback callback;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = callbacks_.find(std::string(name));
        if (it == callbacks_.end()) {
            return Status::NotFound("vm callback not found");
        }
        callback = it->second;
    }
    return callback(args);
}

Status FfiBridge::AttachCurrentThread() {
    std::lock_guard<std::mutex> lock(mu_);
    attached_threads_.insert(std::this_thread::get_id());
    return Status::Ok();
}

Status FfiBridge::DetachCurrentThread() {
    std::lock_guard<std::mutex> lock(mu_);
    attached_threads_.erase(std::this_thread::get_id());
    return Status::Ok();
}

bool FfiBridge::IsCurrentThreadAttached() const {
    std::lock_guard<std::mutex> lock(mu_);
    return attached_threads_.contains(std::this_thread::get_id());
}

}  // namespace tie::vm
