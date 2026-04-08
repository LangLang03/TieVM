#pragma once

#include <string>
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
    VmInstance();

    [[nodiscard]] StatusOr<Value> ExecuteModule(
        const Module& module, const std::vector<Value>& args = {});
    [[nodiscard]] StatusOr<Value> ExecuteLoadedModule(
        const std::string& module_name, const std::vector<Value>& args = {});

    [[nodiscard]] VmThread CreateThread();

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
    ObjectModel object_model_;
    ReflectionRegistry reflection_;
    GcController gc_;
    FfiBridge ffi_;
    ModuleLoader loader_;
    ExceptionBridge exception_bridge_;
};

}  // namespace tie::vm

