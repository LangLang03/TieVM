#include "tie/vm/runtime/reflection_registry.hpp"

namespace tie::vm {

Status ReflectionRegistry::RegisterClass(ClassDescriptor descriptor) {
    return model_->RegisterClass(std::move(descriptor));
}

StatusOr<ObjectId> ReflectionRegistry::NewObject(std::string_view class_name) {
    return model_->NewObject(class_name);
}

StatusOr<std::vector<std::string>> ReflectionRegistry::ListClasses() const {
    return model_->ListClassNames();
}

StatusOr<std::vector<std::string>> ReflectionRegistry::ListMethods(
    std::string_view class_name) const {
    auto class_or = model_->GetClass(class_name);
    if (!class_or.ok()) {
        return class_or.status();
    }
    std::vector<std::string> methods;
    for (const auto& [name, _] : class_or.value().methods) {
        methods.push_back(name);
    }
    return methods;
}

StatusOr<Value> ReflectionRegistry::Invoke(
    ObjectId object_id, std::string_view method_name, const std::vector<Value>& args) const {
    return model_->Invoke(object_id, method_name, args);
}

}  // namespace tie::vm

