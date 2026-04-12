#include "tie/vm/bytecode/verifier.hpp"

#include <unordered_set>
#include <sstream>

namespace tie::vm {

namespace {

constexpr uint32_t kInvalidTryTarget = 0xFFFFFFFFu;

bool IsControlFlow(OpCode opcode) {
    return opcode == OpCode::kJmp || opcode == OpCode::kJmpIf || opcode == OpCode::kRet ||
           opcode == OpCode::kThrow || opcode == OpCode::kHalt ||
           opcode == OpCode::kJmpIfZero || opcode == OpCode::kJmpIfNotZero ||
           opcode == OpCode::kDecJnz || opcode == OpCode::kAddDecJnz ||
           opcode == OpCode::kSubImmJnz || opcode == OpCode::kAddImmJnz ||
           opcode == OpCode::kTailCall || opcode == OpCode::kTailCallClosure ||
           opcode == OpCode::kTryEnd || opcode == OpCode::kEndCatch ||
           opcode == OpCode::kEndFinally;
}

}  // namespace

VerificationResult Verifier::Verify(const Module& module) {
    VerificationResult result;

    if (module.functions().empty()) {
        result.status = Status::VerificationFailed("module has no function");
        return result;
    }
    if (module.entry_function() >= module.functions().size()) {
        result.status = Status::VerificationFailed("entry function index out of range");
        return result;
    }

    if (module.ffi_bindings().size() > 0 && module.ffi_signatures().empty()) {
        result.status = Status::VerificationFailed("ffi bindings exist but signatures are missing");
        return result;
    }

    std::unordered_set<std::string> ffi_symbol_names;
    for (const auto& binding : module.ffi_bindings()) {
        if (binding.vm_symbol.empty()) {
            result.status = Status::VerificationFailed("ffi binding vm_symbol is empty");
            return result;
        }
        if (!ffi_symbol_names.insert(binding.vm_symbol).second) {
            result.status = Status::VerificationFailed("duplicate ffi vm_symbol binding");
            return result;
        }
        if (binding.library_index >= module.ffi_library_paths().size()) {
            result.status = Status::VerificationFailed("ffi binding library index out of range");
            return result;
        }
        if (binding.signature_index >= module.ffi_signatures().size()) {
            result.status = Status::VerificationFailed("ffi binding signature index out of range");
            return result;
        }
    }

    std::unordered_set<std::string> class_names;
    for (const auto& class_decl : module.classes()) {
        if (class_decl.name.empty()) {
            result.status = Status::VerificationFailed("class name cannot be empty");
            return result;
        }
        if (!class_names.insert(class_decl.name).second) {
            result.status = Status::VerificationFailed("duplicate class name in metadata");
            return result;
        }
    }
    for (const auto& class_decl : module.classes()) {
        for (const auto& base_name : class_decl.base_classes) {
            if (!class_names.contains(base_name)) {
                result.status = Status::VerificationFailed(
                    "class base not found in metadata: " + base_name);
                return result;
            }
        }
        std::unordered_set<std::string> method_names;
        for (const auto& method : class_decl.methods) {
            if (method.name.empty()) {
                result.status = Status::VerificationFailed("class method name cannot be empty");
                return result;
            }
            if (static_cast<uint8_t>(method.access) >
                static_cast<uint8_t>(BytecodeAccessModifier::kPrivate)) {
                result.status =
                    Status::VerificationFailed("class method access modifier out of range");
                return result;
            }
            if (!method_names.insert(method.name).second) {
                result.status = Status::VerificationFailed(
                    "duplicate class method name in metadata");
                return result;
            }
            if (method.function_index >= module.functions().size()) {
                result.status = Status::VerificationFailed(
                    "class method function index out of range");
                return result;
            }
            const auto& method_fn = module.functions()[method.function_index];
            if (!method_fn.is_vararg() && method_fn.param_count() == 0) {
                result.status = Status::VerificationFailed(
                    "class method function must accept at least self param");
                return result;
            }
        }
    }

    for (size_t fn_idx = 0; fn_idx < module.functions().size(); ++fn_idx) {
        const auto& function = module.functions()[fn_idx];
        if (function.ffi_binding().enabled) {
            if (function.ffi_binding().binding_index >= module.ffi_bindings().size()) {
                result.status =
                    Status::VerificationFailed("function ffi binding index out of range");
                return result;
            }
            if (function.ffi_binding().signature_index >= module.ffi_signatures().size()) {
                result.status =
                    Status::VerificationFailed("function ffi signature index out of range");
                return result;
            }
        }
        if (function.reg_count() == 0) {
            result.status = Status::VerificationFailed("function has zero register count");
            return result;
        }
        if (function.param_count() > function.reg_count()) {
            result.status = Status::VerificationFailed("function param_count exceeds reg_count");
            return result;
        }
        const auto instructions = function.FlattenedInstructions();
        if (instructions.empty()) {
            result.status = Status::VerificationFailed("function has no instruction");
            return result;
        }
        for (size_t i = 0; i < instructions.size(); ++i) {
            const auto& inst = instructions[i];
            auto ensure_reg = [&](uint32_t reg) -> bool {
                return reg < function.reg_count();
            };
            auto ensure_arg_window = [&](uint32_t base_reg, uint32_t arg_count) -> bool {
                const uint64_t max_reg =
                    static_cast<uint64_t>(base_reg) + static_cast<uint64_t>(arg_count);
                return max_reg < function.reg_count();
            };
            auto ensure_try_target = [&](uint32_t target) -> bool {
                return target == kInvalidTryTarget ||
                       target < static_cast<uint32_t>(instructions.size());
            };
            switch (inst.opcode) {
                case OpCode::kMov:
                    if (!ensure_reg(inst.a) || !ensure_reg(inst.b)) {
                        result.status = Status::VerificationFailed(
                            "register out of range for mov");
                        return result;
                    }
                    break;
                case OpCode::kLoadK:
                    if (!ensure_reg(inst.a) || inst.b >= module.constants().size()) {
                        result.status = Status::VerificationFailed(
                            "loadk requires dst register and valid constant index");
                        return result;
                    }
                    break;
                case OpCode::kAdd:
                case OpCode::kSub:
                case OpCode::kMul:
                case OpCode::kDiv:
                case OpCode::kCmpEq:
                case OpCode::kBitAnd:
                case OpCode::kBitOr:
                case OpCode::kBitXor:
                case OpCode::kBitShl:
                case OpCode::kBitShr:
                case OpCode::kStrConcat:
                    if (!ensure_reg(inst.a) || !ensure_reg(inst.b) || !ensure_reg(inst.c)) {
                        result.status =
                            Status::VerificationFailed("register out of range for arithmetic");
                        return result;
                    }
                    break;
                case OpCode::kAddImm:
                case OpCode::kSubImm:
                    if (!ensure_reg(inst.a) || !ensure_reg(inst.b)) {
                        result.status =
                            Status::VerificationFailed("register out of range for imm arithmetic");
                        return result;
                    }
                    break;
                case OpCode::kInc:
                case OpCode::kDec:
                case OpCode::kStrLen:
                case OpCode::kBitNot:
                    if (!ensure_reg(inst.a)) {
                        result.status =
                            Status::VerificationFailed("register out of range for inc/dec");
                        return result;
                    }
                    if ((inst.opcode == OpCode::kStrLen || inst.opcode == OpCode::kBitNot) &&
                        !ensure_reg(inst.b)) {
                        result.status =
                            Status::VerificationFailed("register out of range for unary op");
                        return result;
                    }
                    break;
                case OpCode::kRet:
                case OpCode::kThrow:
                    if (!ensure_reg(inst.a)) {
                        result.status = Status::VerificationFailed(
                            "register out of range for ret/throw");
                        return result;
                    }
                    break;
                case OpCode::kJmp: {
                    const int32_t delta = static_cast<int32_t>(inst.a);
                    const int64_t next = static_cast<int64_t>(i) + delta;
                    if (next < 0 || next >= static_cast<int64_t>(instructions.size())) {
                        result.status = Status::VerificationFailed("jmp target out of range");
                        return result;
                    }
                    break;
                }
                case OpCode::kJmpIf: {
                    if (!ensure_reg(inst.a)) {
                        result.status =
                            Status::VerificationFailed("condition register out of range");
                        return result;
                    }
                    const int32_t delta = static_cast<int32_t>(inst.b);
                    const int64_t next = static_cast<int64_t>(i) + delta;
                    if (next < 0 || next >= static_cast<int64_t>(instructions.size())) {
                        result.status = Status::VerificationFailed("jmp_if target out of range");
                        return result;
                    }
                    break;
                }
                case OpCode::kJmpIfZero:
                case OpCode::kJmpIfNotZero: {
                    if (!ensure_reg(inst.a)) {
                        result.status =
                            Status::VerificationFailed("condition register out of range");
                        return result;
                    }
                    const int32_t delta = static_cast<int32_t>(inst.b);
                    const int64_t next = static_cast<int64_t>(i) + delta;
                    if (next < 0 || next >= static_cast<int64_t>(instructions.size())) {
                        result.status = Status::VerificationFailed("conditional jump target out of range");
                        return result;
                    }
                    break;
                }
                case OpCode::kDecJnz: {
                    if (!ensure_reg(inst.a)) {
                        result.status =
                            Status::VerificationFailed("counter register out of range");
                        return result;
                    }
                    const int32_t delta = static_cast<int32_t>(inst.b);
                    const int64_t next = static_cast<int64_t>(i) + delta;
                    if (next < 0 || next >= static_cast<int64_t>(instructions.size())) {
                        result.status = Status::VerificationFailed("dec_jnz target out of range");
                        return result;
                    }
                    break;
                }
                case OpCode::kAddDecJnz: {
                    if (!ensure_reg(inst.a) || !ensure_reg(inst.b)) {
                        result.status = Status::VerificationFailed(
                            "add_dec_jnz register out of range");
                        return result;
                    }
                    const int32_t delta = static_cast<int32_t>(inst.c);
                    const int64_t next = static_cast<int64_t>(i) + delta;
                    if (next < 0 || next >= static_cast<int64_t>(instructions.size())) {
                        result.status = Status::VerificationFailed(
                            "add_dec_jnz target out of range");
                        return result;
                    }
                    break;
                }
                case OpCode::kSubImmJnz:
                case OpCode::kAddImmJnz: {
                    if (!ensure_reg(inst.a)) {
                        result.status =
                            Status::VerificationFailed("imm_jnz register out of range");
                        return result;
                    }
                    const int32_t delta = static_cast<int32_t>(inst.c);
                    const int64_t next = static_cast<int64_t>(i) + delta;
                    if (next < 0 || next >= static_cast<int64_t>(instructions.size())) {
                        result.status =
                            Status::VerificationFailed("imm_jnz target out of range");
                        return result;
                    }
                    break;
                }
                case OpCode::kCall:
                case OpCode::kTailCall: {
                    if (inst.b >= module.functions().size()) {
                        result.status =
                            Status::VerificationFailed("call target function index out of range");
                        return result;
                    }
                    if (!ensure_arg_window(inst.a, inst.c)) {
                        result.status =
                            Status::VerificationFailed("call register window out of range");
                        return result;
                    }
                    const auto& callee = module.functions()[inst.b];
                    if (!callee.is_vararg() && callee.param_count() != inst.c) {
                        result.status =
                            Status::VerificationFailed("call argument count mismatch");
                        return result;
                    }
                    if (callee.is_vararg() && inst.c < callee.param_count()) {
                        result.status = Status::VerificationFailed(
                            "call argument count less than fixed params for vararg callee");
                        return result;
                    }
                    break;
                }
                case OpCode::kCallClosure:
                case OpCode::kTailCallClosure:
                    if (!ensure_reg(inst.b) || !ensure_arg_window(inst.a, inst.c)) {
                        result.status = Status::VerificationFailed(
                            "call_closure register window out of range");
                        return result;
                    }
                    break;
                case OpCode::kFfiCall:
                    if (!ensure_arg_window(inst.a, inst.c)) {
                        result.status =
                            Status::VerificationFailed("ffi_call register window out of range");
                        return result;
                    }
                    if (inst.b >= module.constants().size() ||
                        module.constants()[inst.b].type != ConstantType::kUtf8) {
                        result.status =
                            Status::VerificationFailed("ffi_call requires utf8 symbol constant");
                        return result;
                    }
                    if (!module.ffi_bindings().empty()) {
                        const auto& symbol = module.constants()[inst.b].utf8_value;
                        if (!ffi_symbol_names.contains(symbol)) {
                            result.status = Status::VerificationFailed(
                                "ffi_call symbol missing from ffi binding table");
                            return result;
                        }
                    }
                    break;
                case OpCode::kClosure: {
                    if (!ensure_reg(inst.a) || inst.b >= module.functions().size()) {
                        result.status = Status::VerificationFailed(
                            "closure requires valid dst register and function index");
                        return result;
                    }
                    const uint32_t upvalue_count = inst.flags;
                    if (upvalue_count > 0 &&
                        !ensure_arg_window(inst.c, upvalue_count - 1)) {
                        result.status = Status::VerificationFailed(
                            "closure upvalue capture window out of range");
                        return result;
                    }
                    if (module.functions()[inst.b].upvalue_count() != upvalue_count) {
                        result.status = Status::VerificationFailed(
                            "closure upvalue count mismatch with target function");
                        return result;
                    }
                    break;
                }
                case OpCode::kGetUpval:
                case OpCode::kSetUpval:
                    if (!ensure_reg(inst.a)) {
                        result.status = Status::VerificationFailed(
                            "upvalue instruction register out of range");
                        return result;
                    }
                    if (inst.b >= function.upvalue_count()) {
                        result.status = Status::VerificationFailed(
                            "upvalue index out of range");
                        return result;
                    }
                    break;
                case OpCode::kVarArg:
                    if (!function.is_vararg()) {
                        result.status = Status::VerificationFailed(
                            "vararg instruction requires vararg function");
                        return result;
                    }
                    if (inst.c > 0 && !ensure_arg_window(inst.a, inst.c - 1)) {
                        result.status = Status::VerificationFailed(
                            "vararg destination window out of range");
                        return result;
                    }
                    break;
                case OpCode::kNewObject:
                    if (!ensure_reg(inst.a) || inst.b >= module.constants().size() ||
                        module.constants()[inst.b].type != ConstantType::kUtf8) {
                        result.status = Status::VerificationFailed(
                            "new_object requires dst register and utf8 class constant");
                        return result;
                    }
                    break;
                case OpCode::kInvoke:
                    if (!ensure_reg(inst.a) || inst.b >= module.constants().size() ||
                        module.constants()[inst.b].type != ConstantType::kUtf8 ||
                        !ensure_arg_window(inst.a, inst.c)) {
                        result.status = Status::VerificationFailed(
                            "invoke requires valid object register, args and utf8 method constant");
                        return result;
                    }
                    break;
                case OpCode::kTryBegin:
                    if (!ensure_try_target(inst.a) || !ensure_try_target(inst.b) ||
                        !ensure_try_target(inst.c)) {
                        result.status =
                            Status::VerificationFailed("try_begin target out of range");
                        return result;
                    }
                    if (inst.a == kInvalidTryTarget && inst.b == kInvalidTryTarget) {
                        result.status = Status::VerificationFailed(
                            "try_begin requires catch or finally target");
                        return result;
                    }
                    if (inst.c == kInvalidTryTarget) {
                        result.status = Status::VerificationFailed(
                            "try_begin requires end target");
                        return result;
                    }
                    break;
                case OpCode::kTryEnd:
                case OpCode::kEndCatch:
                case OpCode::kEndFinally:
                    break;
                default:
                    break;
            }
            if (IsControlFlow(inst.opcode) && i + 1 < instructions.size()) {
                result.warnings.emplace_back(
                    "control-flow instruction appears before function end");
            }
        }
    }

    result.status = Status::Ok();
    return result;
}

}  // namespace tie::vm
