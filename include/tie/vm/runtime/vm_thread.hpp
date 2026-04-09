#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
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
    [[nodiscard]] VmInstance& owner() { return *owner_; }
    [[nodiscard]] const VmInstance& owner() const { return *owner_; }

  private:
    struct ClosureData {
        uint32_t function_index = 0;
        std::vector<Value> upvalues;
    };
    using ClosureRef = std::shared_ptr<ClosureData>;
    using InstructionCache = std::unordered_map<uint32_t, std::vector<Instruction>>;
    using DebugLineMap = std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>>;

    [[nodiscard]] StatusOr<Value> ExecuteFunction(
        const Module& module, uint32_t function_index, const std::vector<Value>& args,
        InstructionCache& instruction_cache, const DebugLineMap& debug_line_map,
        ClosureRef closure_ref = nullptr);

    VmInstance* owner_;
    uint64_t next_closure_id_ = 1;
    std::unordered_map<uint64_t, ClosureRef> closures_;
};

}  // namespace tie::vm
