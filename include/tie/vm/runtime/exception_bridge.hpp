#pragma once

#include <functional>

#include "tie/vm/common/status.hpp"
#include "tie/vm/runtime/value.hpp"

namespace tie::vm {

class ExceptionBridge {
  public:
    [[nodiscard]] StatusOr<Value> Run(const std::function<StatusOr<Value>()>& fn) const;
};

}  // namespace tie::vm

