#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tie/vm/common/status.hpp"
#include "tie/vm/runtime/value.hpp"

namespace tie::vm {

class GcController;

enum class AccessModifier : uint8_t {
    kPublic = 0,
    kProtected = 1,
    kPrivate = 2,
};

struct MethodDescriptor {
    std::string name;
    AccessModifier access = AccessModifier::kPublic;
    bool is_virtual = true;
    std::function<StatusOr<Value>(ObjectId, const std::vector<Value>&)> body;
};

struct ClassDescriptor {
    std::string name;
    std::vector<std::string> base_classes;
    std::vector<std::string> interfaces;
    std::unordered_map<std::string, MethodDescriptor> methods;
};

struct ObjectInstance {
    ObjectId id = 0;
    std::string class_name;
    std::unordered_map<std::string, Value> fields;
};

class ObjectModel {
  public:
    void SetGcController(GcController* gc) { gc_ = gc; }

    Status RegisterClass(ClassDescriptor descriptor);

    [[nodiscard]] StatusOr<ObjectId> NewObject(std::string_view class_name);
    Status SetField(ObjectId object_id, std::string_view field, Value value);
    [[nodiscard]] StatusOr<Value> GetField(ObjectId object_id, std::string_view field) const;
    [[nodiscard]] StatusOr<Value> Invoke(
        ObjectId object_id, std::string_view method, const std::vector<Value>& args,
        bool allow_private = false) const;

    [[nodiscard]] StatusOr<std::vector<std::string>> ComputeMro(std::string_view class_name) const;
    [[nodiscard]] StatusOr<ClassDescriptor> GetClass(std::string_view class_name) const;
    [[nodiscard]] std::vector<std::string> ListClassNames() const;

  private:
    [[nodiscard]] StatusOr<std::vector<std::string>> Linearize(std::string_view class_name) const;
    [[nodiscard]] static StatusOr<std::vector<std::string>> MergeC3(
        std::vector<std::vector<std::string>> seqs);

    mutable std::mutex mu_;
    std::unordered_map<std::string, ClassDescriptor> classes_;
    std::unordered_map<ObjectId, ObjectInstance> objects_;
    mutable std::unordered_map<std::string, std::vector<std::string>> mro_cache_;
    ObjectId next_object_id_ = 1;
    GcController* gc_ = nullptr;
};

}  // namespace tie::vm
