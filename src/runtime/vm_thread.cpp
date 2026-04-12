#include "tie/vm/runtime/vm_thread.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "tie/vm/runtime/vm_exception.hpp"
#include "tie/vm/runtime/vm_instance.hpp"

namespace tie::vm {

namespace {

uint64_t DebugLineKey(uint32_t function_index, uint32_t instruction_index) {
    return (static_cast<uint64_t>(function_index) << 32u) |
           static_cast<uint64_t>(instruction_index);
}

int32_t SignedU32(uint32_t value) { return static_cast<int32_t>(value); }
constexpr uint32_t kInvalidTryTarget = 0xFFFFFFFFu;

}  // namespace

StatusOr<Value> VmThread::Execute(
    const Module& module, uint32_t entry_function, const std::vector<Value>& args) {
    if (entry_function >= module.functions().size()) {
        return Status::InvalidArgument("entry function index out of range");
    }
    const size_t execution_index = active_execution_count_++;
    if (execution_scratch_pool_.size() <= execution_index) {
        execution_scratch_pool_.resize(execution_index + 1);
    }
    auto& scratch = execution_scratch_pool_[execution_index];
    struct ExecutionDepthGuard {
        size_t* active_count = nullptr;
        ~ExecutionDepthGuard() {
            if (active_count != nullptr) {
                --(*active_count);
            }
        }
    } depth_guard{&active_execution_count_};

    scratch.instruction_cache.clear();
    scratch.instruction_cache.reserve(module.functions().size());

    auto& debug_line_map = scratch.debug_line_map;
    debug_line_map.clear();
    if (!module.debug_lines().empty()) {
        debug_line_map.reserve(module.debug_lines().size());
        for (const auto& line : module.debug_lines()) {
            debug_line_map.insert_or_assign(
                DebugLineKey(line.function_index, line.instruction_index),
                std::make_pair(line.line, line.column));
        }
    }
    closures_.clear();
    next_closure_id_ = 1;
    return ExecuteFunction(module, entry_function, args, scratch, 0, nullptr);
}

StatusOr<Value> VmThread::ExecuteFunction(
    const Module& module, uint32_t function_index, const std::vector<Value>& args,
    ExecutionScratch& scratch, size_t frame_depth,
    ClosureRef closure_ref) {
    if (function_index >= module.functions().size()) {
        return Status::InvalidArgument("function index out of range");
    }

    const Function& function = module.functions()[function_index];
    if (!function.is_vararg() && args.size() != function.param_count()) {
        return Status::InvalidArgument("function argument count mismatch");
    }
    if (function.is_vararg() && args.size() < function.param_count()) {
        return Status::InvalidArgument("vararg function argument count mismatch");
    }

    if (scratch.frames.size() <= frame_depth) {
        scratch.frames.resize(frame_depth + 1);
    }
    auto& frame = scratch.frames[frame_depth];
    auto& regs = frame.regs;
    if (regs.size() != function.reg_count()) {
        regs.resize(function.reg_count());
    }
    std::fill(regs.begin(), regs.end(), Value::Null());
    if (!args.empty()) {
        const size_t count = std::min(args.size(), regs.size());
        std::copy_n(args.begin(), count, regs.begin());
    }

    auto& varargs = frame.varargs;
    varargs.clear();
    if (function.is_vararg() && args.size() > function.param_count()) {
        const size_t count = args.size() - function.param_count();
        if (varargs.capacity() < count) {
            varargs.reserve(count);
        }
        varargs.insert(
            varargs.end(), args.begin() + function.param_count(), args.end());
    }

    auto code_it = scratch.instruction_cache.find(function_index);
    if (code_it == scratch.instruction_cache.end()) {
        code_it = scratch.instruction_cache.emplace(
                                     function_index, function.FlattenedInstructions())
                      .first;
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
        const auto it = scratch.debug_line_map.find(DebugLineKey(function_index, frame_pc));
        if (it != scratch.debug_line_map.end()) {
            frame.line = it->second.first;
            frame.column = it->second.second;
        }
        return frame;
    };

    auto& call_args = frame.call_args;
    auto& ffi_args = frame.ffi_args;
    auto& method_args = frame.method_args;
    struct TryFrame {
        uint32_t catch_target = kInvalidTryTarget;
        uint32_t finally_target = kInvalidTryTarget;
        uint32_t end_target = kInvalidTryTarget;
        bool in_catch = false;
        bool in_finally = false;
        bool rethrow_after_finally = false;
        std::optional<VmError> pending_exception;
    };
    std::vector<TryFrame> try_stack;

    auto resolve_closure = [&](const Value& value) -> ClosureRef {
        if (value.type() != Value::Type::kClosure) {
            throw VmException("value is not closure");
        }
        const uint64_t handle = value.AsClosureHandle();
        auto it = closures_.find(handle);
        if (it == closures_.end() || !it->second) {
            throw VmException("closure handle not found");
        }
        return it->second;
    };

    try {
        const bool runtime_validate = owner_->runtime_validation_enabled();
        while (pc >= 0 && pc < static_cast<int64_t>(code.size())) {
            const auto& inst = code[static_cast<size_t>(pc)];
            auto reg = [&](uint32_t idx) -> Value& {
                if (runtime_validate && idx >= regs.size()) {
                    throw VmException("register index out of bounds");
                }
                return regs[idx];
            };
            auto reg_const = [&](uint32_t idx) -> const Value& {
                if (runtime_validate && idx >= regs.size()) {
                    throw VmException("register index out of bounds");
                }
                return regs[idx];
            };

            try {
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
                    reg(inst.a) = Value::Int64Fast(
                        reg_const(inst.b).AsInt64Fast() + reg_const(inst.c).AsInt64Fast());
                    ++pc;
                    break;
                case OpCode::kSub:
                    reg(inst.a) = Value::Int64Fast(
                        reg_const(inst.b).AsInt64Fast() - reg_const(inst.c).AsInt64Fast());
                    ++pc;
                    break;
                case OpCode::kMul:
                    reg(inst.a) = Value::Int64Fast(
                        reg_const(inst.b).AsInt64Fast() * reg_const(inst.c).AsInt64Fast());
                    ++pc;
                    break;
                case OpCode::kDiv: {
                    const int64_t divisor = reg_const(inst.c).AsInt64Fast();
                    if (divisor == 0) {
                        throw VmException("division by zero");
                    }
                    reg(inst.a) = Value::Int64Fast(reg_const(inst.b).AsInt64Fast() / divisor);
                    ++pc;
                    break;
                }
                case OpCode::kAddImm:
                    reg(inst.a) =
                        Value::Int64Fast(reg_const(inst.b).AsInt64Fast() + SignedU32(inst.c));
                    ++pc;
                    break;
                case OpCode::kSubImm:
                    reg(inst.a) =
                        Value::Int64Fast(reg_const(inst.b).AsInt64Fast() - SignedU32(inst.c));
                    ++pc;
                    break;
                case OpCode::kInc:
                    reg(inst.a) = Value::Int64Fast(reg_const(inst.a).AsInt64Fast() + 1);
                    ++pc;
                    break;
                case OpCode::kDec:
                    reg(inst.a) = Value::Int64Fast(reg_const(inst.a).AsInt64Fast() - 1);
                    ++pc;
                    break;
                case OpCode::kCmpEq: {
                    const Value& lhs = reg_const(inst.b);
                    const Value& rhs = reg_const(inst.c);
                    if (lhs.type() == Value::Type::kInt64 && rhs.type() == Value::Type::kInt64) {
                        reg(inst.a) = Value::BoolFast(lhs.AsInt64Fast() == rhs.AsInt64Fast());
                    } else {
                        reg(inst.a) = Value::Bool(lhs == rhs);
                    }
                    ++pc;
                    break;
                }
                case OpCode::kBitAnd:
                    reg(inst.a) = Value::Int64Fast(
                        reg_const(inst.b).AsInt64Fast() & reg_const(inst.c).AsInt64Fast());
                    ++pc;
                    break;
                case OpCode::kBitOr:
                    reg(inst.a) = Value::Int64Fast(
                        reg_const(inst.b).AsInt64Fast() | reg_const(inst.c).AsInt64Fast());
                    ++pc;
                    break;
                case OpCode::kBitXor:
                    reg(inst.a) = Value::Int64Fast(
                        reg_const(inst.b).AsInt64Fast() ^ reg_const(inst.c).AsInt64Fast());
                    ++pc;
                    break;
                case OpCode::kBitNot:
                    reg(inst.a) = Value::Int64Fast(~reg_const(inst.b).AsInt64Fast());
                    ++pc;
                    break;
                case OpCode::kBitShl: {
                    const uint32_t shift =
                        static_cast<uint32_t>(reg_const(inst.c).AsInt64Fast()) & 63u;
                    reg(inst.a) = Value::Int64Fast(reg_const(inst.b).AsInt64Fast() << shift);
                    ++pc;
                    break;
                }
                case OpCode::kBitShr: {
                    const uint32_t shift =
                        static_cast<uint32_t>(reg_const(inst.c).AsInt64Fast()) & 63u;
                    reg(inst.a) = Value::Int64Fast(reg_const(inst.b).AsInt64Fast() >> shift);
                    ++pc;
                    break;
                }
                case OpCode::kStrLen: {
                    auto str_or = owner_->ResolveStringPtr(reg_const(inst.b));
                    if (!str_or.ok()) {
                        throw VmException(str_or.status().message());
                    }
                    reg(inst.a) = Value::Int64Fast(static_cast<int64_t>(str_or.value()->size()));
                    ++pc;
                    break;
                }
                case OpCode::kStrConcat: {
                    auto lhs_or = owner_->ResolveStringPtr(reg_const(inst.b));
                    if (!lhs_or.ok()) {
                        throw VmException(lhs_or.status().message());
                    }
                    auto rhs_or = owner_->ResolveStringPtr(reg_const(inst.c));
                    if (!rhs_or.ok()) {
                        throw VmException(rhs_or.status().message());
                    }
                    std::string out;
                    out.reserve(lhs_or.value()->size() + rhs_or.value()->size());
                    out.append(*lhs_or.value());
                    out.append(*rhs_or.value());
                    auto string_or = owner_->InternString(out);
                    if (!string_or.ok()) {
                        throw VmException(string_or.status().message());
                    }
                    reg(inst.a) = string_or.value();
                    ++pc;
                    break;
                }
                case OpCode::kJmp:
                    if (runtime_validate) {
                        const int64_t next = pc + SignedU32(inst.a);
                        if (next < 0 || next >= static_cast<int64_t>(code.size())) {
                            throw VmException("jmp target out of range");
                        }
                        pc = next;
                    } else {
                        pc += SignedU32(inst.a);
                    }
                    break;
                case OpCode::kJmpIf: {
                    if (reg_const(inst.a).IsTruthy()) {
                        if (runtime_validate) {
                            const int64_t next = pc + SignedU32(inst.b);
                            if (next < 0 || next >= static_cast<int64_t>(code.size())) {
                                throw VmException("jmp_if target out of range");
                            }
                            pc = next;
                        } else {
                            pc += SignedU32(inst.b);
                        }
                    } else {
                        ++pc;
                    }
                    break;
                }
                case OpCode::kJmpIfZero:
                    if (reg_const(inst.a).AsInt64Fast() == 0) {
                        if (runtime_validate) {
                            const int64_t next = pc + SignedU32(inst.b);
                            if (next < 0 || next >= static_cast<int64_t>(code.size())) {
                                throw VmException("jmp_if_zero target out of range");
                            }
                            pc = next;
                        } else {
                            pc += SignedU32(inst.b);
                        }
                    } else {
                        ++pc;
                    }
                    break;
                case OpCode::kJmpIfNotZero:
                    if (reg_const(inst.a).AsInt64Fast() != 0) {
                        if (runtime_validate) {
                            const int64_t next = pc + SignedU32(inst.b);
                            if (next < 0 || next >= static_cast<int64_t>(code.size())) {
                                throw VmException("jmp_if_not_zero target out of range");
                            }
                            pc = next;
                        } else {
                            pc += SignedU32(inst.b);
                        }
                    } else {
                        ++pc;
                    }
                    break;
                case OpCode::kDecJnz: {
                    const int64_t next = reg_const(inst.a).AsInt64Fast() - 1;
                    reg(inst.a) = Value::Int64Fast(next);
                    if (next != 0) {
                        if (runtime_validate) {
                            const int64_t target = pc + SignedU32(inst.b);
                            if (target < 0 || target >= static_cast<int64_t>(code.size())) {
                                throw VmException("dec_jnz target out of range");
                            }
                            pc = target;
                        } else {
                            pc += SignedU32(inst.b);
                        }
                    } else {
                        ++pc;
                    }
                    break;
                }
                case OpCode::kAddDecJnz: {
                    const int64_t counter = reg_const(inst.b).AsInt64Fast();
                    const int64_t acc = reg_const(inst.a).AsInt64Fast();
                    reg(inst.a) = Value::Int64Fast(acc + counter);
                    const int64_t next_counter = counter - 1;
                    reg(inst.b) = Value::Int64Fast(next_counter);
                    if (next_counter != 0) {
                        if (runtime_validate) {
                            const int64_t target = pc + SignedU32(inst.c);
                            if (target < 0 || target >= static_cast<int64_t>(code.size())) {
                                throw VmException("add_dec_jnz target out of range");
                            }
                            pc = target;
                        } else {
                            pc += SignedU32(inst.c);
                        }
                    } else {
                        ++pc;
                    }
                    break;
                }
                case OpCode::kSubImmJnz: {
                    const int64_t next = reg_const(inst.a).AsInt64Fast() - SignedU32(inst.b);
                    reg(inst.a) = Value::Int64Fast(next);
                    if (next != 0) {
                        if (runtime_validate) {
                            const int64_t target = pc + SignedU32(inst.c);
                            if (target < 0 || target >= static_cast<int64_t>(code.size())) {
                                throw VmException("sub_imm_jnz target out of range");
                            }
                            pc = target;
                        } else {
                            pc += SignedU32(inst.c);
                        }
                    } else {
                        ++pc;
                    }
                    break;
                }
                case OpCode::kAddImmJnz: {
                    const int64_t next = reg_const(inst.a).AsInt64Fast() + SignedU32(inst.b);
                    reg(inst.a) = Value::Int64Fast(next);
                    if (next != 0) {
                        if (runtime_validate) {
                            const int64_t target = pc + SignedU32(inst.c);
                            if (target < 0 || target >= static_cast<int64_t>(code.size())) {
                                throw VmException("add_imm_jnz target out of range");
                            }
                            pc = target;
                        } else {
                            pc += SignedU32(inst.c);
                        }
                    } else {
                        ++pc;
                    }
                    break;
                }
                case OpCode::kTryBegin: {
                    auto is_valid_try_target = [&](uint32_t target) -> bool {
                        return target == kInvalidTryTarget ||
                               target < static_cast<uint32_t>(code.size());
                    };
                    if (!is_valid_try_target(inst.a) || !is_valid_try_target(inst.b) ||
                        !is_valid_try_target(inst.c) || inst.c == kInvalidTryTarget) {
                        throw VmException("try_begin target out of range");
                    }
                    try_stack.push_back(TryFrame{
                        inst.a,
                        inst.b,
                        inst.c,
                        false,
                        false,
                        false,
                        std::nullopt,
                    });
                    ++pc;
                    break;
                }
                case OpCode::kTryEnd: {
                    if (try_stack.empty()) {
                        throw VmException("try_end without active try frame");
                    }
                    auto& current_try = try_stack.back();
                    if (current_try.finally_target != kInvalidTryTarget) {
                        current_try.in_catch = false;
                        current_try.in_finally = true;
                        current_try.rethrow_after_finally = false;
                        current_try.pending_exception.reset();
                        pc = static_cast<int64_t>(current_try.finally_target);
                    } else {
                        const uint32_t end_target = current_try.end_target;
                        try_stack.pop_back();
                        pc = static_cast<int64_t>(end_target);
                    }
                    break;
                }
                case OpCode::kEndCatch: {
                    if (try_stack.empty()) {
                        throw VmException("end_catch without active try frame");
                    }
                    auto& current_try = try_stack.back();
                    if (current_try.finally_target != kInvalidTryTarget) {
                        current_try.in_catch = false;
                        current_try.in_finally = true;
                        current_try.rethrow_after_finally = false;
                        current_try.pending_exception.reset();
                        pc = static_cast<int64_t>(current_try.finally_target);
                    } else {
                        const uint32_t end_target = current_try.end_target;
                        try_stack.pop_back();
                        pc = static_cast<int64_t>(end_target);
                    }
                    break;
                }
                case OpCode::kEndFinally: {
                    if (try_stack.empty()) {
                        throw VmException("end_finally without active try frame");
                    }
                    auto completed_try = std::move(try_stack.back());
                    try_stack.pop_back();
                    if (completed_try.rethrow_after_finally &&
                        completed_try.pending_exception.has_value()) {
                        throw VmException(*completed_try.pending_exception);
                    }
                    pc = static_cast<int64_t>(completed_try.end_target);
                    break;
                }
                case OpCode::kClosure: {
                    if (inst.b >= module.functions().size()) {
                        throw VmException("closure function index out of range");
                    }
                    const uint32_t upvalue_count = inst.flags;
                    auto closure = std::make_shared<ClosureData>();
                    closure->function_index = inst.b;
                    closure->upvalues.reserve(upvalue_count);
                    for (uint32_t i = 0; i < upvalue_count; ++i) {
                        closure->upvalues.push_back(reg_const(inst.c + i));
                    }
                    const uint64_t handle = next_closure_id_++;
                    closures_.insert_or_assign(handle, closure);
                    reg(inst.a) = Value::Closure(handle);
                    ++pc;
                    break;
                }
                case OpCode::kGetUpval: {
                    if (!closure_ref) {
                        throw VmException("get_upval requires closure context");
                    }
                    if (inst.b >= closure_ref->upvalues.size()) {
                        throw VmException("get_upval index out of range");
                    }
                    reg(inst.a) = closure_ref->upvalues[inst.b];
                    ++pc;
                    break;
                }
                case OpCode::kSetUpval: {
                    if (!closure_ref) {
                        throw VmException("set_upval requires closure context");
                    }
                    if (inst.b >= closure_ref->upvalues.size()) {
                        throw VmException("set_upval index out of range");
                    }
                    closure_ref->upvalues[inst.b] = reg_const(inst.a);
                    ++pc;
                    break;
                }
                case OpCode::kVarArg: {
                    const uint32_t count = inst.c;
                    for (uint32_t i = 0; i < count; ++i) {
                        const uint32_t source_idx = inst.b + i;
                        if (source_idx < varargs.size()) {
                            reg(inst.a + i) = varargs[source_idx];
                        } else {
                            reg(inst.a + i) = Value::Null();
                        }
                    }
                    ++pc;
                    break;
                }
                case OpCode::kCall: {
                    call_args.resize(inst.c);
                    for (uint32_t i = 0; i < inst.c; ++i) {
                        call_args[i] = reg_const(inst.a + 1 + i);
                    }
                    auto result_or = ExecuteFunction(
                        module, inst.b, call_args, scratch, frame_depth + 1, nullptr);
                    if (!result_or.ok()) {
                        return result_or.status().WithFrame(
                            vm_frame_for_pc(static_cast<uint32_t>(pc)));
                    }
                    reg(inst.a) = result_or.value();
                    ++pc;
                    break;
                }
                case OpCode::kCallClosure: {
                    auto closure = resolve_closure(reg_const(inst.b));
                    call_args.resize(inst.c);
                    for (uint32_t i = 0; i < inst.c; ++i) {
                        call_args[i] = reg_const(inst.a + 1 + i);
                    }
                    auto result_or = ExecuteFunction(
                        module,
                        closure->function_index,
                        call_args,
                        scratch,
                        frame_depth + 1,
                        closure);
                    if (!result_or.ok()) {
                        return result_or.status().WithFrame(
                            vm_frame_for_pc(static_cast<uint32_t>(pc)));
                    }
                    reg(inst.a) = result_or.value();
                    ++pc;
                    break;
                }
                case OpCode::kTailCall: {
                    call_args.resize(inst.c);
                    for (uint32_t i = 0; i < inst.c; ++i) {
                        call_args[i] = reg_const(inst.a + 1 + i);
                    }
                    auto result_or = ExecuteFunction(
                        module, inst.b, call_args, scratch, frame_depth + 1, nullptr);
                    if (!result_or.ok()) {
                        return result_or.status().WithFrame(
                            vm_frame_for_pc(static_cast<uint32_t>(pc)));
                    }
                    return result_or.value();
                }
                case OpCode::kTailCallClosure: {
                    auto closure = resolve_closure(reg_const(inst.b));
                    call_args.resize(inst.c);
                    for (uint32_t i = 0; i < inst.c; ++i) {
                        call_args[i] = reg_const(inst.a + 1 + i);
                    }
                    auto result_or = ExecuteFunction(
                        module,
                        closure->function_index,
                        call_args,
                        scratch,
                        frame_depth + 1,
                        closure);
                    if (!result_or.ok()) {
                        return result_or.status().WithFrame(
                            vm_frame_for_pc(static_cast<uint32_t>(pc)));
                    }
                    return result_or.value();
                }
                case OpCode::kFfiCall: {
                    if (inst.b >= module.constants().size() ||
                        module.constants()[inst.b].type != ConstantType::kUtf8) {
                        throw VmException("ffi_call requires utf8 symbol constant");
                    }
                    const auto& symbol = module.constants()[inst.b].utf8_value;
                    ffi_args.resize(inst.c);
                    for (uint32_t i = 0; i < inst.c; ++i) {
                        ffi_args[i] = reg_const(inst.a + 1 + i);
                    }
                    auto ffi_or = owner_->ffi().CallBoundFunction(
                        module, function_index, symbol, *this, ffi_args);
                    if (!ffi_or.ok() && ffi_or.status().code() == ErrorCode::kNotFound) {
                        auto loaded = owner_->loader().FindModuleByFfiSymbol(symbol);
                        if (loaded.has_value()) {
                            ffi_or = owner_->ffi().CallBoundFunction(
                                *loaded, 0, symbol, *this, ffi_args);
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
                    method_args.resize(inst.c);
                    for (uint32_t i = 0; i < inst.c; ++i) {
                        method_args[i] = reg_const(inst.a + 1 + i);
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
            } catch (VmException& ex) {
                bool handled = false;
                while (!try_stack.empty()) {
                    auto& current_try = try_stack.back();
                    if (current_try.in_finally) {
                        try_stack.pop_back();
                        continue;
                    }
                    if (!current_try.in_catch &&
                        current_try.catch_target != kInvalidTryTarget) {
                        current_try.in_catch = true;
                        current_try.pending_exception = ex.error();
                        current_try.rethrow_after_finally = false;
                        pc = static_cast<int64_t>(current_try.catch_target);
                        handled = true;
                        break;
                    }
                    if (current_try.finally_target != kInvalidTryTarget) {
                        current_try.in_catch = false;
                        current_try.in_finally = true;
                        current_try.pending_exception = ex.error();
                        current_try.rethrow_after_finally = true;
                        pc = static_cast<int64_t>(current_try.finally_target);
                        handled = true;
                        break;
                    }
                    try_stack.pop_back();
                }
                if (handled) {
                    continue;
                }
                throw;
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
