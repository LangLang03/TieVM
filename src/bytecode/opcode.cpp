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
        case OpCode::kAddImm:
            return "add_imm";
        case OpCode::kSubImm:
            return "sub_imm";
        case OpCode::kJmpIfZero:
            return "jmp_if_zero";
        case OpCode::kJmpIfNotZero:
            return "jmp_if_not_zero";
        case OpCode::kDecJnz:
            return "dec_jnz";
        case OpCode::kAddDecJnz:
            return "add_dec_jnz";
        case OpCode::kInc:
            return "inc";
        case OpCode::kDec:
            return "dec";
        case OpCode::kSubImmJnz:
            return "sub_imm_jnz";
        case OpCode::kAddImmJnz:
            return "add_imm_jnz";
        case OpCode::kClosure:
            return "closure";
        case OpCode::kGetUpval:
            return "get_upval";
        case OpCode::kSetUpval:
            return "set_upval";
        case OpCode::kCallClosure:
            return "call_closure";
        case OpCode::kTailCall:
            return "tail_call";
        case OpCode::kTailCallClosure:
            return "tail_call_closure";
        case OpCode::kVarArg:
            return "vararg";
        case OpCode::kStrLen:
            return "str_len";
        case OpCode::kStrConcat:
            return "str_concat";
        case OpCode::kBitAnd:
            return "bit_and";
        case OpCode::kBitOr:
            return "bit_or";
        case OpCode::kBitXor:
            return "bit_xor";
        case OpCode::kBitNot:
            return "bit_not";
        case OpCode::kBitShl:
            return "bit_shl";
        case OpCode::kBitShr:
            return "bit_shr";
    }
    return "unknown";
}

}  // namespace tie::vm
