#pragma once

#include "tie/vm/common/status.hpp"

namespace tie::vm {

class VmInstance;

class StdlibRegistry {
  public:
    static Status RegisterCore(VmInstance* vm);
};

}  // namespace tie::vm

