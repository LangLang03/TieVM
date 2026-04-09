#pragma once

#include <stdexcept>
#include <string>

#include "tie/vm/common/vm_error.hpp"

namespace tie::vm {

class VmException : public std::runtime_error {
  public:
    explicit VmException(std::string message)
        : std::runtime_error(std::move(message)), error_{std::runtime_error::what(), {}} {}
    explicit VmException(VmError error)
        : std::runtime_error(error.message), error_(std::move(error)) {}

    void PushFrame(StackFrame frame) { error_.PushFrame(std::move(frame)); }
    [[nodiscard]] const VmError& error() const { return error_; }

  private:
    VmError error_;
};

}  // namespace tie::vm
