#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
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
    struct FrameScratch {
        std::vector<Value> regs;
        std::vector<Value> varargs;
        std::vector<Value> call_args;
        std::vector<Value> ffi_args;
        std::vector<Value> method_args;
    };
    using ClosureRef = std::shared_ptr<ClosureData>;
    using InstructionCache = std::unordered_map<uint32_t, std::vector<Instruction>>;
    using DebugLineMap = std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>>;
    struct ExecutionScratch {
        InstructionCache instruction_cache;
        DebugLineMap debug_line_map;
        std::deque<FrameScratch> frames;
    };

    [[nodiscard]] StatusOr<Value> ExecuteFunction(
        const Module& module, uint32_t function_index, const std::vector<Value>& args,
        ExecutionScratch& scratch, size_t frame_depth,
        ClosureRef closure_ref = nullptr);

    VmInstance* owner_;
    uint64_t next_closure_id_ = 1;
    std::unordered_map<uint64_t, ClosureRef> closures_;
    std::vector<ExecutionScratch> execution_scratch_pool_;
    size_t active_execution_count_ = 0;
};

}  // namespace tie::vm
