#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tie/vm/common/status.hpp"
#include "tie/vm/runtime/object_model.hpp"

namespace tie::vm {

class ReflectionRegistry {
  public:
    explicit ReflectionRegistry(ObjectModel* model) : model_(model) {}

    Status RegisterClass(ClassDescriptor descriptor);
    [[nodiscard]] StatusOr<ObjectId> NewObject(std::string_view class_name);
    [[nodiscard]] StatusOr<std::vector<std::string>> ListClasses() const;
    [[nodiscard]] StatusOr<std::vector<std::string>> ListMethods(
        std::string_view class_name) const;
    [[nodiscard]] StatusOr<Value> Invoke(
        ObjectId object_id, std::string_view method_name, const std::vector<Value>& args) const;

  private:
    ObjectModel* model_;
};

}  // namespace tie::vm

