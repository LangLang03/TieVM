#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tie/vm/common/status.hpp"
#include "tie/vm/ffi/ffi_types.hpp"
#include "tie/vm/runtime/value.hpp"

namespace tie::vm {

class Module;
class VmThread;

class FfiBridge {
  public:
    using NativeFunction = std::function<StatusOr<Value>(VmThread&, const std::vector<Value>&)>;
    using VmCallback = std::function<StatusOr<Value>(const std::vector<Value>&)>;

    FfiBridge() = default;
    ~FfiBridge();
    FfiBridge(const FfiBridge&) = delete;
    FfiBridge& operator=(const FfiBridge&) = delete;

    Status RegisterFunction(FunctionSignature signature, NativeFunction fn);
    [[nodiscard]] StatusOr<Value> CallFunction(
        std::string_view name, VmThread& vm_thread, const std::vector<Value>& args) const;
    [[nodiscard]] StatusOr<Value> CallBoundFunction(
        const Module& module, uint32_t function_index, std::string_view vm_symbol,
        VmThread& vm_thread, const std::vector<Value>& args) const;
    [[nodiscard]] Status ValidateModuleBindings(const Module& module) const;

    Status RegisterVmCallback(std::string name, VmCallback callback);
    [[nodiscard]] StatusOr<Value> InvokeVmCallback(
        std::string_view name, const std::vector<Value>& args) const;

    Status AttachCurrentThread();
    Status DetachCurrentThread();
    [[nodiscard]] bool IsCurrentThreadAttached() const;

  private:
    struct Entry {
        FunctionSignature signature;
        NativeFunction function;
    };

    struct DynamicPlanFastKey {
        const Module* module = nullptr;
        uint32_t function_index = 0;

        [[nodiscard]] bool operator==(const DynamicPlanFastKey& rhs) const {
            return module == rhs.module && function_index == rhs.function_index;
        }
    };

    struct DynamicPlanFastKeyHash {
        [[nodiscard]] size_t operator()(const DynamicPlanFastKey& key) const {
            const auto a = std::hash<const Module*>{}(key.module);
            const auto b = std::hash<uint32_t>{}(key.function_index);
            return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
        }
    };

    struct DynamicCallPlan {
        FunctionSignature signature;
        std::string library_path;
        std::string symbol_name;
        void* symbol = nullptr;
    };

    [[nodiscard]] StatusOr<DynamicCallPlan> ResolveDynamicPlan(
        const Module& module, uint32_t function_index, std::string_view vm_symbol) const;
    [[nodiscard]] StatusOr<Value> InvokeDynamicPlan(
        const DynamicCallPlan& plan, VmThread& vm_thread,
        const std::vector<Value>& args) const;
    [[nodiscard]] StatusOr<void*> LoadDynamicSymbol(
        const std::string& library_path, const std::string& symbol_name) const;
    void CloseAllLibraries();

    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> native_functions_;
    std::unordered_map<std::string, VmCallback> callbacks_;
    std::unordered_set<std::thread::id> attached_threads_;
    mutable std::unordered_map<std::string, void*> dynamic_lib_handles_;
    mutable std::unordered_map<std::string, DynamicCallPlan> dynamic_plan_cache_;
    mutable std::unordered_map<DynamicPlanFastKey, DynamicCallPlan, DynamicPlanFastKeyHash>
        dynamic_plan_fast_cache_;
};

}  // namespace tie::vm
