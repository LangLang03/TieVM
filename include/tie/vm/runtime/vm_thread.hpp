#pragma once

#include <cstddef>
#include <vector>

#include "tie/vm/bytecode/module.hpp"
#include "tie/vm/common/status.hpp"
#include "tie/vm/runtime/value.hpp"

namespace tie::vm {

class VmInstance;

class VmThread {
  public:
    explicit VmThread(VmInstance* owner) : owner_(owner) {}

    [[nodiscard]] StatusOr<Value> Execute(
        const Module& module, uint32_t entry_function, const std::vector<Value>& args = {});

  private:
    [[nodiscard]] StatusOr<Value> ExecuteFunction(
        const Module& module, uint32_t function_index, const std::vector<Value>& args);

    VmInstance* owner_;
};

}  // namespace tie::vm

