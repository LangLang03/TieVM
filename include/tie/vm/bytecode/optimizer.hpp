#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "tie/vm/bytecode/module.hpp"
#include "tie/vm/common/status.hpp"

namespace tie::vm {

enum class BytecodeOptLevel : uint8_t {
    kO0 = 0,
    kO1 = 1,
    kO2 = 2,
    kO3 = 3,
};

enum class BytecodeOptPass : uint8_t {
    kPeephole = 0,
    kLoopFusion = 1,
    kConstFold = 2,
    kCopyProp = 3,
    kDce = 4,
    kTailcall = 5,
    kInlineSmall = 6,
    kCleanup = 7,
};

[[nodiscard]] std::string_view BytecodeOptLevelName(BytecodeOptLevel level);
[[nodiscard]] std::string_view BytecodeOptPassName(BytecodeOptPass pass);
[[nodiscard]] StatusOr<BytecodeOptLevel> ParseBytecodeOptLevel(std::string_view text);
[[nodiscard]] StatusOr<BytecodeOptPass> ParseBytecodeOptPass(std::string_view text);

struct BytecodeOptOptions {
    BytecodeOptLevel level = BytecodeOptLevel::kO2;
    std::vector<BytecodeOptPass> enable_passes;
    std::vector<BytecodeOptPass> disable_passes;
    uint32_t inline_max_inst = 8;
    bool verify_input = true;
};

struct BytecodeOptStats {
    size_t module_function_count = 0;
    size_t optimized_function_count = 0;
    size_t rewritten_instruction_count = 0;
    size_t removed_instruction_count = 0;
    size_t inlined_callsite_count = 0;
    std::vector<BytecodeOptPass> executed_passes;
};

struct BytecodeOptResult {
    Module module{"opt.invalid"};
    BytecodeOptStats stats;
};

[[nodiscard]] StatusOr<BytecodeOptResult> OptimizeBytecodeModule(
    const Module& module, const BytecodeOptOptions& options = {});

[[nodiscard]] StatusOr<BytecodeOptStats> OptimizeBytecodeModuleInPlace(
    Module* module, const BytecodeOptOptions& options = {});

}  // namespace tie::vm

