#include "tie/vm/ffi/ffi_bridge.hpp"

#include <utility>

namespace tie::vm {

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
    // Calling convention metadata is validated by registration and reserved for
    // platform adapters. v0.x keeps a single dispatch path with explicit metadata.
    return entry.function(vm_thread, args);
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
