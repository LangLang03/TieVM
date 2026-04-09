#include "tie/vm/bytecode/verifier.hpp"

#include <unordered_set>
#include <sstream>

namespace tie::vm {

namespace {

bool IsControlFlow(OpCode opcode) {
    return opcode == OpCode::kJmp || opcode == OpCode::kJmpIf || opcode == OpCode::kRet ||
           opcode == OpCode::kThrow || opcode == OpCode::kHalt ||
           opcode == OpCode::kJmpIfZero || opcode == OpCode::kJmpIfNotZero ||
           opcode == OpCode::kDecJnz;
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
                case OpCode::kCall:
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
                    if (module.functions()[inst.b].param_count() != inst.c) {
                        result.status =
                            Status::VerificationFailed("call argument count mismatch");
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
