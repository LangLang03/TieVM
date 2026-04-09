#include "tie/vm/runtime/vm_thread.hpp"

#include <algorithm>
#include <stdexcept>

#include "tie/vm/runtime/vm_exception.hpp"
#include "tie/vm/runtime/vm_instance.hpp"

namespace tie::vm {

namespace {

uint64_t DebugLineKey(uint32_t function_index, uint32_t instruction_index) {
    return (static_cast<uint64_t>(function_index) << 32u) |
           static_cast<uint64_t>(instruction_index);
}

int32_t SignedU32(uint32_t value) { return static_cast<int32_t>(value); }

}  // namespace

StatusOr<Value> VmThread::Execute(
    const Module& module, uint32_t entry_function, const std::vector<Value>& args) {
    if (entry_function >= module.functions().size()) {
        return Status::InvalidArgument("entry function index out of range");
    }
    InstructionCache instruction_cache;
    instruction_cache.reserve(module.functions().size());

    DebugLineMap debug_line_map;
    if (!module.debug_lines().empty()) {
        debug_line_map.reserve(module.debug_lines().size());
        for (const auto& line : module.debug_lines()) {
            debug_line_map.insert_or_assign(
                DebugLineKey(line.function_index, line.instruction_index),
                std::make_pair(line.line, line.column));
        }
    }
    return ExecuteFunction(module, entry_function, args, instruction_cache, debug_line_map);
}

StatusOr<Value> VmThread::ExecuteFunction(
    const Module& module, uint32_t function_index, const std::vector<Value>& args,
    InstructionCache& instruction_cache, const DebugLineMap& debug_line_map) {
    if (function_index >= module.functions().size()) {
        return Status::InvalidArgument("function index out of range");
    }

    const Function& function = module.functions()[function_index];
    if (args.size() != function.param_count()) {
        return Status::InvalidArgument("function argument count mismatch");
    }
    std::vector<Value> regs(function.reg_count(), Value::Null());
    if (!args.empty()) {
        const size_t count = std::min(args.size(), regs.size());
        std::copy_n(args.begin(), count, regs.begin());
    }

    auto code_it = instruction_cache.find(function_index);
    if (code_it == instruction_cache.end()) {
        code_it =
            instruction_cache.emplace(function_index, function.FlattenedInstructions()).first;
    }
    const auto& code = code_it->second;
    if (code.empty()) {
        return Status::RuntimeError("function has no instruction");
    }

    int64_t pc = 0;
    auto vm_frame_for_pc = [&](uint32_t frame_pc) {
        StackFrame frame;
        frame.kind = StackFrameKind::kVm;
        frame.module_name = module.name();
        frame.function_name = function.name();
        frame.program_counter = frame_pc;
        const auto it = debug_line_map.find(DebugLineKey(function_index, frame_pc));
        if (it != debug_line_map.end()) {
            frame.line = it->second.first;
            frame.column = it->second.second;
        }
        return frame;
    };

    std::vector<Value> call_args;
    std::vector<Value> ffi_args;
    std::vector<Value> method_args;

    try {
        while (pc >= 0 && pc < static_cast<int64_t>(code.size())) {
            const auto& inst = code[static_cast<size_t>(pc)];
            auto reg = [&](uint32_t idx) -> Value& { return regs[idx]; };
            auto reg_const = [&](uint32_t idx) -> const Value& { return regs[idx]; };

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
                case OpCode::kAddImm:
                    reg(inst.a) =
                        Value::Int64(reg_const(inst.b).AsInt64() + SignedU32(inst.c));
                    ++pc;
                    break;
                case OpCode::kSubImm:
                    reg(inst.a) =
                        Value::Int64(reg_const(inst.b).AsInt64() - SignedU32(inst.c));
                    ++pc;
                    break;
                case OpCode::kCmpEq:
                    reg(inst.a) = Value::Bool(reg_const(inst.b) == reg_const(inst.c));
                    ++pc;
                    break;
                case OpCode::kJmp:
                    pc += SignedU32(inst.a);
                    break;
                case OpCode::kJmpIf: {
                    if (reg_const(inst.a).IsTruthy()) {
                        pc += SignedU32(inst.b);
                    } else {
                        ++pc;
                    }
                    break;
                }
                case OpCode::kJmpIfZero:
                    if (reg_const(inst.a).AsInt64() == 0) {
                        pc += SignedU32(inst.b);
                    } else {
                        ++pc;
                    }
                    break;
                case OpCode::kJmpIfNotZero:
                    if (reg_const(inst.a).AsInt64() != 0) {
                        pc += SignedU32(inst.b);
                    } else {
                        ++pc;
                    }
                    break;
                case OpCode::kDecJnz: {
                    const int64_t next = reg_const(inst.a).AsInt64() - 1;
                    reg(inst.a) = Value::Int64(next);
                    if (next != 0) {
                        pc += SignedU32(inst.b);
                    } else {
                        ++pc;
                    }
                    break;
                }
                case OpCode::kCall: {
                    call_args.clear();
                    if (call_args.capacity() < inst.c) {
                        call_args.reserve(inst.c);
                    }
                    for (uint32_t i = 0; i < inst.c; ++i) {
                        call_args.push_back(reg_const(inst.a + 1 + i));
                    }
                    auto result_or = ExecuteFunction(
                        module, inst.b, call_args, instruction_cache, debug_line_map);
                    if (!result_or.ok()) {
                        return result_or.status().WithFrame(
                            vm_frame_for_pc(static_cast<uint32_t>(pc)));
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
                    ffi_args.clear();
                    if (ffi_args.capacity() < inst.c) {
                        ffi_args.reserve(inst.c);
                    }
                    for (uint32_t i = 0; i < inst.c; ++i) {
                        ffi_args.push_back(reg_const(inst.a + 1 + i));
                    }
                    auto ffi_or = owner_->ffi().CallBoundFunction(
                        module, function_index, symbol, *this, ffi_args);
                    if (!ffi_or.ok() && ffi_or.status().code() == ErrorCode::kNotFound) {
                        auto loaded_opt = owner_->loader().FindModuleByFfiSymbol(symbol);
                        if (loaded_opt.has_value()) {
                            const auto& loaded = loaded_opt.value();
                            ffi_or = owner_->ffi().CallBoundFunction(
                                loaded, 0, symbol, *this, ffi_args);
                        }
                    }
                    if (!ffi_or.ok()) {
                        return ffi_or.status().WithFrame(
                            vm_frame_for_pc(static_cast<uint32_t>(pc)));
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
                        return obj_or.status().WithFrame(
                            vm_frame_for_pc(static_cast<uint32_t>(pc)));
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
                    method_args.clear();
                    if (method_args.capacity() < inst.c) {
                        method_args.reserve(inst.c);
                    }
                    for (uint32_t i = 0; i < inst.c; ++i) {
                        method_args.push_back(reg_const(inst.a + 1 + i));
                    }
                    auto result_or =
                        owner_->reflection().Invoke(object_id, method_name, method_args);
                    if (!result_or.ok()) {
                        return result_or.status().WithFrame(
                            vm_frame_for_pc(static_cast<uint32_t>(pc)));
                    }
                    reg(inst.a) = result_or.value();
                    ++pc;
                    break;
                }
                default:
                    throw VmException("unknown opcode");
            }
        }
    } catch (VmException& ex) {
        ex.PushFrame(vm_frame_for_pc(static_cast<uint32_t>(pc)));
        throw;
    } catch (const std::exception& ex) {
        VmError err{ex.what(), {}};
        err.PushFrame(vm_frame_for_pc(static_cast<uint32_t>(pc)));
        throw VmException(std::move(err));
    }
    return Status::RuntimeError("program counter escaped function");
}

}  // namespace tie::vm
