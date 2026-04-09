#include "tie/vm/runtime/value.hpp"

#include <cstring>
#include <stdexcept>

namespace tie::vm {

Value Value::Null() { return Value{}; }

Value Value::Int64(int64_t v) {
    Value value;
    value.type_ = Type::kInt64;
    value.bits_ = static_cast<uint64_t>(v);
    return value;
}

Value Value::Float64(double v) {
    Value value;
    value.type_ = Type::kFloat64;
    std::memcpy(&value.bits_, &v, sizeof(v));
    return value;
}

Value Value::Bool(bool v) {
    Value value;
    value.type_ = Type::kBool;
    value.bits_ = v ? 1 : 0;
    return value;
}

Value Value::Object(ObjectId object_id) {
    Value value;
    value.type_ = Type::kObject;
    value.bits_ = object_id;
    return value;
}

Value Value::Pointer(uint64_t ptr_bits) {
    Value value;
    value.type_ = Type::kPointer;
    value.bits_ = ptr_bits;
    return value;
}

Value Value::String(uint64_t string_handle) {
    Value value;
    value.type_ = Type::kString;
    value.bits_ = string_handle;
    return value;
}

Value Value::Closure(uint64_t closure_handle) {
    Value value;
    value.type_ = Type::kClosure;
    value.bits_ = closure_handle;
    return value;
}

bool Value::IsTruthy() const {
    switch (type_) {
        case Type::kNull:
            return false;
        case Type::kBool:
            return bits_ != 0;
        case Type::kInt64:
            return static_cast<int64_t>(bits_) != 0;
        case Type::kFloat64:
            return AsFloat64() != 0.0;
        default:
            return true;
    }
}

int64_t Value::AsInt64() const {
    if (type_ != Type::kInt64) {
        throw std::runtime_error("value is not int64");
    }
    return static_cast<int64_t>(bits_);
}

double Value::AsFloat64() const {
    if (type_ != Type::kFloat64) {
        throw std::runtime_error("value is not float64");
    }
    double out = 0.0;
    std::memcpy(&out, &bits_, sizeof(out));
    return out;
}

bool Value::AsBool() const {
    if (type_ != Type::kBool) {
        throw std::runtime_error("value is not bool");
    }
    return bits_ != 0;
}

ObjectId Value::AsObject() const {
    if (type_ != Type::kObject) {
        throw std::runtime_error("value is not object");
    }
    return bits_;
}

uint64_t Value::AsPointer() const {
    if (type_ != Type::kPointer) {
        throw std::runtime_error("value is not pointer");
    }
    return bits_;
}

uint64_t Value::AsStringHandle() const {
    if (type_ != Type::kString) {
        throw std::runtime_error("value is not string");
    }
    return bits_;
}

uint64_t Value::AsClosureHandle() const {
    if (type_ != Type::kClosure) {
        throw std::runtime_error("value is not closure");
    }
    return bits_;
}

std::string Value::ToString() const {
    switch (type_) {
        case Type::kNull:
            return "null";
        case Type::kInt64:
            return std::to_string(static_cast<int64_t>(bits_));
        case Type::kFloat64:
            return std::to_string(AsFloat64());
        case Type::kBool:
            return bits_ != 0 ? "true" : "false";
        case Type::kObject:
            return "object#" + std::to_string(bits_);
        case Type::kPointer:
            return "ptr#" + std::to_string(bits_);
        case Type::kString:
            return "str#" + std::to_string(bits_);
        case Type::kClosure:
            return "closure#" + std::to_string(bits_);
    }
    return "unknown";
}

bool operator==(const Value& lhs, const Value& rhs) {
    return lhs.type_ == rhs.type_ && lhs.bits_ == rhs.bits_;
}

}  // namespace tie::vm
