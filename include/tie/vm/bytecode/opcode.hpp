#pragma once

#include <cstdint>
#include <string_view>

namespace tie::vm {

enum class OpCode : uint8_t {
    kNop = 0,
    kMov = 1,
    kLoadK = 2,
    kAdd = 3,
    kSub = 4,
    kMul = 5,
    kDiv = 6,
    kCmpEq = 7,
    kJmp = 8,
    kJmpIf = 9,
    kCall = 10,
    kFfiCall = 11,
    kRet = 12,
    kThrow = 13,
    kHalt = 14,
    kNewObject = 15,
    kInvoke = 16,
    kAddImm = 17,
    kSubImm = 18,
    kJmpIfZero = 19,
    kJmpIfNotZero = 20,
    kDecJnz = 21,
};

[[nodiscard]] std::string_view OpCodeName(OpCode opcode);

}  // namespace tie::vm
