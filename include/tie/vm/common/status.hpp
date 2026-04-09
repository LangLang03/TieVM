#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "tie/vm/common/vm_error.hpp"

namespace tie::vm {

enum class ErrorCode : uint32_t {
    kOk = 0,
    kInvalidArgument,
    kInvalidState,
    kNotFound,
    kAlreadyExists,
    kVerificationFailed,
    kSerializationError,
    kRuntimeError,
    kGcError,
    kFfiError,
    kUnsupported,
};

class Status {
  public:
    Status() = default;
    Status(ErrorCode code, std::string message)
        : code_(code), message_(std::move(message)) {
        if (code_ != ErrorCode::kOk) {
            error_ = VmError{message_, {}};
        }
    }
    Status(ErrorCode code, std::string message, VmError error)
        : code_(code), message_(std::move(message)), error_(std::move(error)) {}

    static Status Ok() { return {}; }

    static Status InvalidArgument(std::string message) {
        return {ErrorCode::kInvalidArgument, std::move(message)};
    }

    static Status InvalidState(std::string message) {
        return {ErrorCode::kInvalidState, std::move(message)};
    }

    static Status NotFound(std::string message) {
        return {ErrorCode::kNotFound, std::move(message)};
    }

    static Status AlreadyExists(std::string message) {
        return {ErrorCode::kAlreadyExists, std::move(message)};
    }

    static Status VerificationFailed(std::string message) {
        return {ErrorCode::kVerificationFailed, std::move(message)};
    }

    static Status RuntimeError(std::string message) {
        return {ErrorCode::kRuntimeError, std::move(message)};
    }

    static Status RuntimeError(VmError error) {
        return {ErrorCode::kRuntimeError, error.message, std::move(error)};
    }

    static Status SerializationError(std::string message) {
        return {ErrorCode::kSerializationError, std::move(message)};
    }

    static Status GcError(std::string message) {
        return {ErrorCode::kGcError, std::move(message)};
    }

    static Status FfiError(std::string message) {
        return {ErrorCode::kFfiError, std::move(message)};
    }

    static Status FfiError(VmError error) {
        return {ErrorCode::kFfiError, error.message, std::move(error)};
    }

    static Status Unsupported(std::string message) {
        return {ErrorCode::kUnsupported, std::move(message)};
    }

    [[nodiscard]] bool ok() const { return code_ == ErrorCode::kOk; }
    [[nodiscard]] ErrorCode code() const { return code_; }
    [[nodiscard]] const std::string& message() const { return message_; }
    [[nodiscard]] const std::optional<VmError>& vm_error() const { return error_; }

    [[nodiscard]] Status WithFrame(StackFrame frame) const {
        if (ok()) {
            return *this;
        }
        Status copy = *this;
        if (!copy.error_.has_value()) {
            copy.error_ = VmError{copy.message_, {}};
        }
        copy.error_->PushFrame(std::move(frame));
        return copy;
    }

  private:
    ErrorCode code_ = ErrorCode::kOk;
    std::string message_;
    std::optional<VmError> error_;
};

template <typename T>
class [[nodiscard]] StatusOr {
  public:
    StatusOr(Status status) : status_(std::move(status)) {}
    StatusOr(const T& value) : status_(Status::Ok()), value_(value) {}
    StatusOr(T&& value) : status_(Status::Ok()), value_(std::move(value)) {}

    [[nodiscard]] bool ok() const { return status_.ok(); }
    [[nodiscard]] const Status& status() const { return status_; }

    [[nodiscard]] const T& value() const& { return *value_; }
    [[nodiscard]] T& value() & { return *value_; }
    [[nodiscard]] T&& value() && { return std::move(*value_); }

  private:
    Status status_;
    std::optional<T> value_;
};

}  // namespace tie::vm
