#pragma once

#include <cstdint>

#include "tie/vm/bytecode/opcode.hpp"

namespace tie::vm {

inline constexpr uint32_t kInstructionSize = 16;

struct Instruction {
    OpCode opcode = OpCode::kNop;
    uint8_t flags = 0;
    uint16_t reserved = 0;
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
};

[[nodiscard]] constexpr Instruction MakeInstruction(
    OpCode opcode, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint8_t flags = 0) {
    return Instruction{opcode, flags, 0, a, b, c};
}

}  // namespace tie::vm

