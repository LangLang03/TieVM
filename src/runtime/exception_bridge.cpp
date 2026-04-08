#include "tie/vm/runtime/exception_bridge.hpp"

#include <exception>
#include <stdexcept>

#include "tie/vm/runtime/vm_exception.hpp"

namespace tie::vm {

StatusOr<Value> ExceptionBridge::Run(const std::function<StatusOr<Value>()>& fn) const {
    try {
        return fn();
    } catch (const VmException& ex) {
        return Status::RuntimeError(ex.what());
    } catch (const std::exception& ex) {
        return Status::RuntimeError(ex.what());
    } catch (...) {
        return Status::RuntimeError("unknown vm exception");
    }
}

}  // namespace tie::vm

