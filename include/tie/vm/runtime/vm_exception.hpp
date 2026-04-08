#pragma once

#include <stdexcept>
#include <string>

namespace tie::vm {

class VmException : public std::runtime_error {
  public:
    explicit VmException(std::string message) : std::runtime_error(std::move(message)) {}
};

}  // namespace tie::vm

