#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

#include "tie/vm/bytecode/module.hpp"
#include "tie/vm/common/status.hpp"
#include "tie/vm/ffi/ffi_bridge.hpp"
#include "tie/vm/gc/gc_controller.hpp"
#include "tie/vm/runtime/exception_bridge.hpp"
#include "tie/vm/runtime/hot_reload_session.hpp"
#include "tie/vm/runtime/module_loader.hpp"
#include "tie/vm/runtime/object_model.hpp"
#include "tie/vm/runtime/reflection_registry.hpp"
#include "tie/vm/runtime/value.hpp"

namespace tie::vm {

class VmThread;

class VmInstance {
  public:
    using OutputSink = std::function<void(const std::string&)>;

    VmInstance();

    [[nodiscard]] StatusOr<Value> ExecuteModule(
        const Module& module, const std::vector<Value>& args = {});
    [[nodiscard]] StatusOr<Value> ExecuteLoadedModule(
        const std::string& module_name, const std::vector<Value>& args = {});

    [[nodiscard]] VmThread CreateThread();

    [[nodiscard]] StatusOr<Value> InternString(const std::string& value);
    [[nodiscard]] StatusOr<std::string> ResolveString(const Value& value) const;
    [[nodiscard]] StatusOr<const std::string*> ResolveStringPtr(const Value& value) const;

    [[nodiscard]] StatusOr<Value> CreateArray();
    [[nodiscard]] StatusOr<int64_t> ArrayPush(Value array_handle, Value element);
    [[nodiscard]] StatusOr<Value> ArrayGet(Value array_handle, int64_t index) const;
    [[nodiscard]] StatusOr<int64_t> ArraySize(Value array_handle) const;

    [[nodiscard]] StatusOr<Value> CreateMap();
    Status MapSet(Value map_handle, const std::string& key, Value element);
    [[nodiscard]] StatusOr<Value> MapGet(Value map_handle, const std::string& key) const;
    [[nodiscard]] StatusOr<bool> MapHas(Value map_handle, const std::string& key) const;

    void SetOutputSink(OutputSink sink);
    void EmitOutputLine(const std::string& text) const;
    void SetRuntimeValidationEnabled(bool enabled);
    [[nodiscard]] bool runtime_validation_enabled() const {
        return runtime_validation_enabled_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] FfiBridge& ffi() { return ffi_; }
    [[nodiscard]] const FfiBridge& ffi() const { return ffi_; }
    [[nodiscard]] GcController& gc() { return gc_; }
    [[nodiscard]] const GcController& gc() const { return gc_; }
    [[nodiscard]] ObjectModel& object_model() { return object_model_; }
    [[nodiscard]] const ObjectModel& object_model() const { return object_model_; }
    [[nodiscard]] ReflectionRegistry& reflection() { return reflection_; }
    [[nodiscard]] const ReflectionRegistry& reflection() const { return reflection_; }
    [[nodiscard]] ModuleLoader& loader() { return loader_; }
    [[nodiscard]] const ModuleLoader& loader() const { return loader_; }
    [[nodiscard]] const ExceptionBridge& exception_bridge() const { return exception_bridge_; }

  private:
    mutable std::mutex runtime_mu_;
    uint64_t next_string_id_ = 1;
    uint64_t next_array_id_ = 1;
    uint64_t next_map_id_ = 1;
    std::unordered_map<uint64_t, std::string> strings_;
    std::unordered_map<std::string, uint64_t> string_ids_;
    std::unordered_map<uint64_t, std::vector<Value>> arrays_;
    std::unordered_map<uint64_t, std::unordered_map<std::string, Value>> maps_;
    OutputSink output_sink_;
    std::atomic<bool> runtime_validation_enabled_{false};

    ObjectModel object_model_;
    ReflectionRegistry reflection_;
    GcController gc_;
    FfiBridge ffi_;
    ModuleLoader loader_;
    ExceptionBridge exception_bridge_;
};

}  // namespace tie::vm
