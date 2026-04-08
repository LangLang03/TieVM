#pragma once

#include <cstdint>
#include <string>

namespace tie::vm {

using ObjectId = uint64_t;

class Value {
  public:
    enum class Type : uint8_t {
        kNull = 0,
        kInt64 = 1,
        kFloat64 = 2,
        kBool = 3,
        kObject = 4,
        kPointer = 5,
        kString = 6,
    };

    Value() = default;

    static Value Null();
    static Value Int64(int64_t v);
    static Value Float64(double v);
    static Value Bool(bool v);
    static Value Object(ObjectId object_id);
    static Value Pointer(uint64_t ptr_bits);
    static Value String(uint64_t string_handle);

    [[nodiscard]] Type type() const { return type_; }
    [[nodiscard]] bool IsTruthy() const;
    [[nodiscard]] int64_t AsInt64() const;
    [[nodiscard]] double AsFloat64() const;
    [[nodiscard]] bool AsBool() const;
    [[nodiscard]] ObjectId AsObject() const;
    [[nodiscard]] uint64_t AsPointer() const;
    [[nodiscard]] uint64_t AsStringHandle() const;
    [[nodiscard]] std::string ToString() const;

    friend bool operator==(const Value& lhs, const Value& rhs);
    friend bool operator!=(const Value& lhs, const Value& rhs) { return !(lhs == rhs); }

  private:
    Type type_ = Type::kNull;
    uint64_t bits_ = 0;
};

}  // namespace tie::vm
