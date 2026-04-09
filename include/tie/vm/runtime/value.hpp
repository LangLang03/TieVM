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
        kClosure = 7,
    };

    Value() = default;

    static Value Null();
    static Value Int64(int64_t v);
    static Value Float64(double v);
    static Value Bool(bool v);
    static Value Object(ObjectId object_id);
    static Value Pointer(uint64_t ptr_bits);
    static Value String(uint64_t string_handle);
    static Value Closure(uint64_t closure_handle);
    static Value Int64Fast(int64_t v) {
        Value value;
        value.type_ = Type::kInt64;
        value.bits_ = static_cast<uint64_t>(v);
        return value;
    }
    static Value BoolFast(bool v) {
        Value value;
        value.type_ = Type::kBool;
        value.bits_ = v ? 1u : 0u;
        return value;
    }

    [[nodiscard]] Type type() const { return type_; }
    [[nodiscard]] bool IsTruthy() const;
    [[nodiscard]] int64_t AsInt64() const;
    [[nodiscard]] double AsFloat64() const;
    [[nodiscard]] bool AsBool() const;
    [[nodiscard]] ObjectId AsObject() const;
    [[nodiscard]] uint64_t AsPointer() const;
    [[nodiscard]] uint64_t AsStringHandle() const;
    [[nodiscard]] uint64_t AsClosureHandle() const;
    [[nodiscard]] int64_t AsInt64Fast() const { return static_cast<int64_t>(bits_); }
    [[nodiscard]] std::string ToString() const;

    friend bool operator==(const Value& lhs, const Value& rhs);
    friend bool operator!=(const Value& lhs, const Value& rhs) { return !(lhs == rhs); }

  private:
    Type type_ = Type::kNull;
    uint64_t bits_ = 0;
};

}  // namespace tie::vm
