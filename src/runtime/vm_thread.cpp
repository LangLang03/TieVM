#include "tie/vm/runtime/vm_thread.hpp"

#include <stdexcept>

#include "tie/vm/runtime/vm_exception.hpp"
#include "tie/vm/runtime/vm_instance.hpp"

namespace tie::vm {

StatusOr<Value> VmThread::Execute(
    const Module& module, uint32_t entry_function, const std::vector<Value>& args) {
    if (entry_function >= module.functions().size()) {
        return Status::InvalidArgument("entry function index out of range");
    }
    return ExecuteFunction(module, entry_function, args);
}

StatusOr<Value> VmThread::ExecuteFunction(
    const Module& module, uint32_t function_index, const std::vector<Value>& args) {
    if (function_index >= module.functions().size()) {
        return Status::InvalidArgument("function index out of range");
    }

    const Function& function = module.functions()[function_index];
    std::vector<Value> regs(function.reg_count(), Value::Null());
    for (size_t i = 0; i < args.size() && i < regs.size(); ++i) {
        regs[i] = args[i];
    }
    const auto code = function.FlattenedInstructions();
    if (code.empty()) {
        return Status::RuntimeError("function has no instruction");
    }

    int64_t pc = 0;
    while (pc >= 0 && pc < static_cast<int64_t>(code.size())) {
        const auto& inst = code[static_cast<size_t>(pc)];
        auto reg = [&](uint32_t idx) -> Value& {
            if (idx >= regs.size()) {
                throw VmException("register index out of bounds");
            }
            return regs[idx];
        };
        auto reg_const = [&](uint32_t idx) -> const Value& {
            if (idx >= regs.size()) {
                throw VmException("register index out of bounds");
            }
            return regs[idx];
        };

        switch (inst.opcode) {
            case OpCode::kNop:
                ++pc;
                break;
            case OpCode::kMov:
                reg(inst.a) = reg_const(inst.b);
                ++pc;
                break;
            case OpCode::kLoadK: {
                if (inst.b >= module.constants().size()) {
                    throw VmException("constant index out of bounds");
                }
                const auto& constant = module.constants()[inst.b];
                switch (constant.type) {
                    case ConstantType::kInt64:
                        reg(inst.a) = Value::Int64(constant.int64_value);
                        break;
                    case ConstantType::kFloat64:
                        reg(inst.a) = Value::Float64(constant.float64_value);
                        break;
                    case ConstantType::kUtf8: {
                        auto utf8_or = owner_->InternString(constant.utf8_value);
                        if (!utf8_or.ok()) {
                            throw VmException(utf8_or.status().message());
                        }
                        reg(inst.a) = utf8_or.value();
                        break;
                    }
                }
                ++pc;
                break;
            }
            case OpCode::kAdd:
                reg(inst.a) = Value::Int64(reg_const(inst.b).AsInt64() + reg_const(inst.c).AsInt64());
                ++pc;
                break;
            case OpCode::kSub:
                reg(inst.a) = Value::Int64(reg_const(inst.b).AsInt64() - reg_const(inst.c).AsInt64());
                ++pc;
                break;
            case OpCode::kMul:
                reg(inst.a) = Value::Int64(reg_const(inst.b).AsInt64() * reg_const(inst.c).AsInt64());
                ++pc;
                break;
            case OpCode::kDiv: {
                const int64_t divisor = reg_const(inst.c).AsInt64();
                if (divisor == 0) {
                    throw VmException("division by zero");
                }
                reg(inst.a) = Value::Int64(reg_const(inst.b).AsInt64() / divisor);
                ++pc;
                break;
            }
            case OpCode::kCmpEq:
                reg(inst.a) = Value::Bool(reg_const(inst.b) == reg_const(inst.c));
                ++pc;
                break;
            case OpCode::kJmp: {
                const int64_t next = pc + static_cast<int32_t>(inst.a);
                if (next < 0 || next >= static_cast<int64_t>(code.size())) {
                    throw VmException("jmp target out of range");
                }
                pc = next;
                break;
            }
            case OpCode::kJmpIf: {
                if (reg_const(inst.a).IsTruthy()) {
                    const int64_t next = pc + static_cast<int32_t>(inst.b);
                    if (next < 0 || next >= static_cast<int64_t>(code.size())) {
                        throw VmException("jmp_if target out of range");
                    }
                    pc = next;
                } else {
                    ++pc;
                }
                break;
            }
            case OpCode::kCall: {
                std::vector<Value> call_args;
                call_args.reserve(inst.c);
                for (uint32_t i = 0; i < inst.c; ++i) {
                    call_args.push_back(reg_const(inst.a + 1 + i));
                }
                auto result_or = ExecuteFunction(module, inst.b, call_args);
                if (!result_or.ok()) {
                    return result_or.status();
                }
                reg(inst.a) = result_or.value();
                ++pc;
                break;
            }
            case OpCode::kFfiCall: {
                if (inst.b >= module.constants().size() ||
                    module.constants()[inst.b].type != ConstantType::kUtf8) {
                    throw VmException("ffi_call requires utf8 symbol constant");
                }
                const auto& symbol = module.constants()[inst.b].utf8_value;
                std::vector<Value> ffi_args;
                ffi_args.reserve(inst.c);
                for (uint32_t i = 0; i < inst.c; ++i) {
                    ffi_args.push_back(reg_const(inst.a + 1 + i));
                }
                auto ffi_or = owner_->ffi().CallFunction(symbol, *this, ffi_args);
                if (!ffi_or.ok()) {
                    return ffi_or.status();
                }
                reg(inst.a) = ffi_or.value();
                ++pc;
                break;
            }
            case OpCode::kRet:
                return reg_const(inst.a);
            case OpCode::kThrow:
                throw VmException("vm throw: " + reg_const(inst.a).ToString());
            case OpCode::kHalt:
                return Value::Null();
            case OpCode::kNewObject: {
                if (inst.b >= module.constants().size() ||
                    module.constants()[inst.b].type != ConstantType::kUtf8) {
                    throw VmException("new_object requires utf8 class constant");
                }
                const auto& class_name = module.constants()[inst.b].utf8_value;
                auto obj_or = owner_->reflection().NewObject(class_name);
                if (!obj_or.ok()) {
                    return obj_or.status();
                }
                reg(inst.a) = Value::Object(obj_or.value());
                ++pc;
                break;
            }
            case OpCode::kInvoke: {
                if (inst.b >= module.constants().size() ||
                    module.constants()[inst.b].type != ConstantType::kUtf8) {
                    throw VmException("invoke requires utf8 method constant");
                }
                const auto& method_name = module.constants()[inst.b].utf8_value;
                ObjectId object_id = reg_const(inst.a).AsObject();
                std::vector<Value> method_args;
                method_args.reserve(inst.c);
                for (uint32_t i = 0; i < inst.c; ++i) {
                    method_args.push_back(reg_const(inst.a + 1 + i));
                }
                auto result_or =
                    owner_->reflection().Invoke(object_id, method_name, method_args);
                if (!result_or.ok()) {
                    return result_or.status();
                }
                reg(inst.a) = result_or.value();
                ++pc;
                break;
            }
        }
    }
    return Status::RuntimeError("program counter escaped function");
}

}  // namespace tie::vm
