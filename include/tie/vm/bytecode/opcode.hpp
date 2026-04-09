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
    kAddDecJnz = 22,
    kInc = 23,
    kDec = 24,
    kSubImmJnz = 25,
    kAddImmJnz = 26,
    kClosure = 27,
    kGetUpval = 28,
    kSetUpval = 29,
    kCallClosure = 30,
    kTailCall = 31,
    kTailCallClosure = 32,
    kVarArg = 33,
    kStrLen = 34,
    kStrConcat = 35,
    kBitAnd = 36,
    kBitOr = 37,
    kBitXor = 38,
    kBitNot = 39,
    kBitShl = 40,
    kBitShr = 41,
};

[[nodiscard]] std::string_view OpCodeName(OpCode opcode);

}  // namespace tie::vm
