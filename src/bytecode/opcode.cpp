#include "tie/vm/bytecode/opcode.hpp"

namespace tie::vm {

std::string_view OpCodeName(OpCode opcode) {
    switch (opcode) {
        case OpCode::kNop:
            return "nop";
        case OpCode::kMov:
            return "mov";
        case OpCode::kLoadK:
            return "loadk";
        case OpCode::kAdd:
            return "add";
        case OpCode::kSub:
            return "sub";
        case OpCode::kMul:
            return "mul";
        case OpCode::kDiv:
            return "div";
        case OpCode::kCmpEq:
            return "cmpeq";
        case OpCode::kJmp:
            return "jmp";
        case OpCode::kJmpIf:
            return "jmp_if";
        case OpCode::kCall:
            return "call";
        case OpCode::kFfiCall:
            return "ffi_call";
        case OpCode::kRet:
            return "ret";
        case OpCode::kThrow:
            return "throw";
        case OpCode::kHalt:
            return "halt";
        case OpCode::kNewObject:
            return "new_object";
        case OpCode::kInvoke:
            return "invoke";
    }
    return "unknown";
}

}  // namespace tie::vm

