#include "tie/vm/runtime/vm_instance.hpp"

#include "tie/vm/runtime/vm_thread.hpp"

namespace tie::vm {

VmInstance::VmInstance() : reflection_(&object_model_) {}

StatusOr<Value> VmInstance::ExecuteModule(const Module& module, const std::vector<Value>& args) {
    return exception_bridge_.Run([&]() {
        auto thread = CreateThread();
        return thread.Execute(module, module.entry_function(), args);
    });
}

StatusOr<Value> VmInstance::ExecuteLoadedModule(
    const std::string& module_name, const std::vector<Value>& args) {
    auto module_or = loader_.GetModule(module_name);
    if (!module_or.ok()) {
        return module_or.status();
    }
    return ExecuteModule(module_or.value(), args);
}

VmThread VmInstance::CreateThread() { return VmThread(this); }

}  // namespace tie::vm

