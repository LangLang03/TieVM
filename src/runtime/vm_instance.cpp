#include "tie/vm/runtime/vm_instance.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "tie/vm/runtime/vm_thread.hpp"
#include "tie/vm/stdlib/stdlib_registry.hpp"

namespace tie::vm {

VmInstance::VmInstance()
    : output_sink_([](const std::string& line) { std::cout << line << "\n"; }),
      reflection_(&object_model_) {
    const auto register_status = StdlibRegistry::RegisterCore(this);
    if (!register_status.ok()) {
        throw std::runtime_error(
            "failed to register stdlib: " + register_status.message());
    }
}

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

StatusOr<Value> VmInstance::InternString(const std::string& value) {
    std::lock_guard<std::mutex> lock(runtime_mu_);
    auto it = string_ids_.find(value);
    if (it != string_ids_.end()) {
        return Value::String(it->second);
    }
    const uint64_t id = next_string_id_++;
    strings_[id] = value;
    string_ids_[value] = id;
    return Value::String(id);
}

StatusOr<std::string> VmInstance::ResolveString(const Value& value) const {
    if (value.type() != Value::Type::kString) {
        return Status::InvalidArgument("value is not utf8 string");
    }
    std::lock_guard<std::mutex> lock(runtime_mu_);
    auto it = strings_.find(value.AsStringHandle());
    if (it == strings_.end()) {
        return Status::NotFound("string handle not found");
    }
    return it->second;
}

StatusOr<Value> VmInstance::CreateArray() {
    std::lock_guard<std::mutex> lock(runtime_mu_);
    const uint64_t id = next_array_id_++;
    arrays_[id] = {};
    return Value::Pointer(id);
}

StatusOr<int64_t> VmInstance::ArrayPush(Value array_handle, Value element) {
    if (array_handle.type() != Value::Type::kPointer) {
        return Status::InvalidArgument("array handle must be pointer");
    }
    std::lock_guard<std::mutex> lock(runtime_mu_);
    auto it = arrays_.find(array_handle.AsPointer());
    if (it == arrays_.end()) {
        return Status::NotFound("array handle not found");
    }
    it->second.push_back(element);
    return static_cast<int64_t>(it->second.size());
}

StatusOr<Value> VmInstance::ArrayGet(Value array_handle, int64_t index) const {
    if (array_handle.type() != Value::Type::kPointer) {
        return Status::InvalidArgument("array handle must be pointer");
    }
    if (index < 0) {
        return Status::InvalidArgument("array index cannot be negative");
    }
    std::lock_guard<std::mutex> lock(runtime_mu_);
    auto it = arrays_.find(array_handle.AsPointer());
    if (it == arrays_.end()) {
        return Status::NotFound("array handle not found");
    }
    const auto idx = static_cast<size_t>(index);
    if (idx >= it->second.size()) {
        return Status::NotFound("array index out of range");
    }
    return it->second[idx];
}

StatusOr<int64_t> VmInstance::ArraySize(Value array_handle) const {
    if (array_handle.type() != Value::Type::kPointer) {
        return Status::InvalidArgument("array handle must be pointer");
    }
    std::lock_guard<std::mutex> lock(runtime_mu_);
    auto it = arrays_.find(array_handle.AsPointer());
    if (it == arrays_.end()) {
        return Status::NotFound("array handle not found");
    }
    return static_cast<int64_t>(it->second.size());
}

StatusOr<Value> VmInstance::CreateMap() {
    std::lock_guard<std::mutex> lock(runtime_mu_);
    const uint64_t id = next_map_id_++;
    maps_[id] = {};
    return Value::Pointer(id);
}

Status VmInstance::MapSet(Value map_handle, const std::string& key, Value element) {
    if (map_handle.type() != Value::Type::kPointer) {
        return Status::InvalidArgument("map handle must be pointer");
    }
    std::lock_guard<std::mutex> lock(runtime_mu_);
    auto it = maps_.find(map_handle.AsPointer());
    if (it == maps_.end()) {
        return Status::NotFound("map handle not found");
    }
    it->second[key] = element;
    return Status::Ok();
}

StatusOr<Value> VmInstance::MapGet(Value map_handle, const std::string& key) const {
    if (map_handle.type() != Value::Type::kPointer) {
        return Status::InvalidArgument("map handle must be pointer");
    }
    std::lock_guard<std::mutex> lock(runtime_mu_);
    auto it = maps_.find(map_handle.AsPointer());
    if (it == maps_.end()) {
        return Status::NotFound("map handle not found");
    }
    auto value_it = it->second.find(key);
    if (value_it == it->second.end()) {
        return Value::Null();
    }
    return value_it->second;
}

StatusOr<bool> VmInstance::MapHas(Value map_handle, const std::string& key) const {
    if (map_handle.type() != Value::Type::kPointer) {
        return Status::InvalidArgument("map handle must be pointer");
    }
    std::lock_guard<std::mutex> lock(runtime_mu_);
    auto it = maps_.find(map_handle.AsPointer());
    if (it == maps_.end()) {
        return Status::NotFound("map handle not found");
    }
    return it->second.contains(key);
}

void VmInstance::SetOutputSink(OutputSink sink) {
    std::lock_guard<std::mutex> lock(runtime_mu_);
    output_sink_ = std::move(sink);
}

void VmInstance::EmitOutputLine(const std::string& text) const {
    OutputSink sink;
    {
        std::lock_guard<std::mutex> lock(runtime_mu_);
        sink = output_sink_;
    }
    if (sink) {
        sink(text);
    }
}

}  // namespace tie::vm
