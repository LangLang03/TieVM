#include "tie/vm/bytecode/optimizer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tie/vm/bytecode/verifier.hpp"

namespace tie::vm {

namespace {

constexpr uint32_t kInvalidTryTarget = 0xFFFFFFFFu;
constexpr size_t kO3ConstExecMaxSteps = 200000;

struct KnownValue {
    enum class Kind : uint8_t {
        kUnknown = 0,
        kInt64 = 1,
        kBool = 2,
    };

    Kind kind = Kind::kUnknown;
    int64_t int64_value = 0;
    bool bool_value = false;

    static KnownValue Unknown() { return {}; }
    static KnownValue Int64(int64_t value) {
        KnownValue out;
        out.kind = Kind::kInt64;
        out.int64_value = value;
        return out;
    }
    static KnownValue Bool(bool value) {
        KnownValue out;
        out.kind = Kind::kBool;
        out.bool_value = value;
        return out;
    }
};

struct PassRunResult {
    bool changed = false;
    size_t rewritten = 0;
    size_t removed = 0;
    size_t inlined = 0;
};

void MergePassResult(PassRunResult* dst, const PassRunResult& src) {
    dst->changed = dst->changed || src.changed;
    dst->rewritten += src.rewritten;
    dst->removed += src.removed;
    dst->inlined += src.inlined;
}

bool IsTryOpcode(OpCode opcode) {
    return opcode == OpCode::kTryBegin || opcode == OpCode::kTryEnd ||
           opcode == OpCode::kEndCatch || opcode == OpCode::kEndFinally;
}

bool ContainsTryInstructions(const std::vector<Instruction>& code) {
    for (const auto& inst : code) {
        if (IsTryOpcode(inst.opcode)) {
            return true;
        }
    }
    return false;
}

bool IsRelativeJumpOpcode(OpCode opcode) {
    return opcode == OpCode::kJmp || opcode == OpCode::kJmpIf ||
           opcode == OpCode::kJmpIfZero || opcode == OpCode::kJmpIfNotZero ||
           opcode == OpCode::kDecJnz || opcode == OpCode::kAddDecJnz ||
           opcode == OpCode::kSubImmJnz || opcode == OpCode::kAddImmJnz;
}

int32_t RelativeJumpDelta(const Instruction& inst) {
    switch (inst.opcode) {
        case OpCode::kJmp:
            return static_cast<int32_t>(inst.a);
        case OpCode::kJmpIf:
        case OpCode::kJmpIfZero:
        case OpCode::kJmpIfNotZero:
        case OpCode::kDecJnz:
            return static_cast<int32_t>(inst.b);
        case OpCode::kAddDecJnz:
        case OpCode::kSubImmJnz:
        case OpCode::kAddImmJnz:
            return static_cast<int32_t>(inst.c);
        default:
            return 0;
    }
}

void SetRelativeJumpDelta(Instruction* inst, int32_t delta) {
    switch (inst->opcode) {
        case OpCode::kJmp:
            inst->a = static_cast<uint32_t>(delta);
            break;
        case OpCode::kJmpIf:
        case OpCode::kJmpIfZero:
        case OpCode::kJmpIfNotZero:
        case OpCode::kDecJnz:
            inst->b = static_cast<uint32_t>(delta);
            break;
        case OpCode::kAddDecJnz:
        case OpCode::kSubImmJnz:
        case OpCode::kAddImmJnz:
            inst->c = static_cast<uint32_t>(delta);
            break;
        default:
            break;
    }
}

bool IsTerminator(OpCode opcode) {
    return opcode == OpCode::kRet || opcode == OpCode::kThrow || opcode == OpCode::kHalt ||
           opcode == OpCode::kTailCall || opcode == OpCode::kTailCallClosure;
}

std::vector<size_t> CollectJumpTargets(const std::vector<Instruction>& code) {
    std::vector<size_t> targets;
    targets.reserve(code.size() / 2 + 1);
    for (size_t i = 0; i < code.size(); ++i) {
        const auto& inst = code[i];
        if (IsRelativeJumpOpcode(inst.opcode)) {
            const int64_t target =
                static_cast<int64_t>(i) + static_cast<int64_t>(RelativeJumpDelta(inst));
            if (target >= 0 && target < static_cast<int64_t>(code.size())) {
                targets.push_back(static_cast<size_t>(target));
            }
        }
        if (inst.opcode == OpCode::kTryBegin) {
            if (inst.a != kInvalidTryTarget && inst.a < code.size()) {
                targets.push_back(inst.a);
            }
            if (inst.b != kInvalidTryTarget && inst.b < code.size()) {
                targets.push_back(inst.b);
            }
            if (inst.c != kInvalidTryTarget && inst.c < code.size()) {
                targets.push_back(inst.c);
            }
        }
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

bool IsLabelTarget(const std::vector<size_t>& labels, size_t pc) {
    return std::binary_search(labels.begin(), labels.end(), pc);
}

void RebuildFunctionCode(Function* function, const std::vector<Instruction>& code) {
    std::string block_name = "entry";
    if (!function->blocks().empty()) {
        block_name = function->blocks().front().name();
    }
    function->blocks().clear();
    auto& block = function->AddBlock(block_name);
    if (code.empty()) {
        block.Append(MakeInstruction(OpCode::kNop));
        return;
    }
    for (const auto& inst : code) {
        block.Append(inst);
    }
}

uint32_t ResolveAlias(
    uint32_t reg, const std::unordered_map<uint32_t, uint32_t>& alias_map) {
    uint32_t current = reg;
    std::unordered_set<uint32_t> seen;
    while (true) {
        auto it = alias_map.find(current);
        if (it == alias_map.end()) {
            return current;
        }
        if (!seen.insert(current).second) {
            return current;
        }
        current = it->second;
    }
}

void KillAliasForWrite(
    std::unordered_map<uint32_t, uint32_t>* alias_map, uint32_t reg) {
    alias_map->erase(reg);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = alias_map->begin(); it != alias_map->end();) {
            if (it->second == reg) {
                it = alias_map->erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }
}

void SetKnown(
    std::unordered_map<uint32_t, KnownValue>* known_map, uint32_t reg, KnownValue value) {
    if (value.kind == KnownValue::Kind::kUnknown) {
        known_map->erase(reg);
        return;
    }
    known_map->insert_or_assign(reg, value);
}

KnownValue GetKnown(
    const std::unordered_map<uint32_t, KnownValue>& known_map, uint32_t reg) {
    auto it = known_map.find(reg);
    if (it == known_map.end()) {
        return KnownValue::Unknown();
    }
    return it->second;
}

bool KnownTruthy(KnownValue value) {
    switch (value.kind) {
        case KnownValue::Kind::kBool:
            return value.bool_value;
        case KnownValue::Kind::kInt64:
            return value.int64_value != 0;
        case KnownValue::Kind::kUnknown:
            return false;
    }
    return false;
}

std::optional<int64_t> KnownInt64(KnownValue value) {
    if (value.kind == KnownValue::Kind::kInt64) {
        return value.int64_value;
    }
    return std::nullopt;
}

bool TryReadKnownReg(
    const std::vector<KnownValue>& regs, uint32_t index, KnownValue* out) {
    if (index >= regs.size()) {
        return false;
    }
    *out = regs[index];
    return out->kind != KnownValue::Kind::kUnknown;
}

bool TryReadInt64Reg(
    const std::vector<KnownValue>& regs, uint32_t index, int64_t* out) {
    if (index >= regs.size()) {
        return false;
    }
    auto value = KnownInt64(regs[index]);
    if (!value.has_value()) {
        return false;
    }
    *out = *value;
    return true;
}

bool IsO3ConstExecEligible(const Function& function, const std::vector<Instruction>& code) {
    if (function.param_count() != 0 || function.is_vararg() || function.upvalue_count() != 0) {
        return false;
    }
    if (function.reg_count() == 0) {
        return false;
    }
    if (function.return_type() != BytecodeValueType::kAny &&
        function.return_type() != BytecodeValueType::kInt64) {
        return false;
    }
    if (code.empty() || ContainsTryInstructions(code)) {
        return false;
    }
    return true;
}

uint32_t GetOrAddInt64Constant(
    Module* module, std::unordered_map<int64_t, uint32_t>* int64_cache,
    int64_t value) {
    auto it = int64_cache->find(value);
    if (it != int64_cache->end()) {
        return it->second;
    }
    const uint32_t idx = module->AddConstant(Constant::Int64(value));
    int64_cache->insert_or_assign(value, idx);
    return idx;
}

bool RemapSourceRegsWithAlias(
    Instruction* inst, const std::unordered_map<uint32_t, uint32_t>& alias_map) {
    bool changed = false;
    auto remap_one = [&](uint32_t* reg_field) {
        const uint32_t before = *reg_field;
        const uint32_t after = ResolveAlias(before, alias_map);
        if (before != after) {
            *reg_field = after;
            changed = true;
        }
    };

    switch (inst->opcode) {
        case OpCode::kMov:
            remap_one(&inst->b);
            break;
        case OpCode::kAdd:
        case OpCode::kSub:
        case OpCode::kMul:
        case OpCode::kDiv:
        case OpCode::kCmpEq:
        case OpCode::kStrConcat:
        case OpCode::kBitAnd:
        case OpCode::kBitOr:
        case OpCode::kBitXor:
        case OpCode::kBitShl:
        case OpCode::kBitShr:
            remap_one(&inst->b);
            remap_one(&inst->c);
            break;
        case OpCode::kAddImm:
        case OpCode::kSubImm:
        case OpCode::kStrLen:
        case OpCode::kBitNot:
            remap_one(&inst->b);
            break;
        case OpCode::kInc:
        case OpCode::kDec:
        case OpCode::kJmpIf:
        case OpCode::kJmpIfZero:
        case OpCode::kJmpIfNotZero:
        case OpCode::kDecJnz:
        case OpCode::kSubImmJnz:
        case OpCode::kAddImmJnz:
        case OpCode::kRet:
        case OpCode::kThrow:
        case OpCode::kSetUpval:
            remap_one(&inst->a);
            break;
        case OpCode::kAddDecJnz:
            remap_one(&inst->a);
            remap_one(&inst->b);
            break;
        case OpCode::kCallClosure:
        case OpCode::kTailCallClosure:
            remap_one(&inst->b);
            break;
        default:
            break;
    }

    return changed;
}

bool IsInlineSafeOpcode(OpCode opcode) {
    switch (opcode) {
        case OpCode::kNop:
        case OpCode::kMov:
        case OpCode::kLoadK:
        case OpCode::kAdd:
        case OpCode::kSub:
        case OpCode::kMul:
        case OpCode::kDiv:
        case OpCode::kCmpEq:
        case OpCode::kAddImm:
        case OpCode::kSubImm:
        case OpCode::kInc:
        case OpCode::kDec:
        case OpCode::kStrLen:
        case OpCode::kStrConcat:
        case OpCode::kBitAnd:
        case OpCode::kBitOr:
        case OpCode::kBitXor:
        case OpCode::kBitNot:
        case OpCode::kBitShl:
        case OpCode::kBitShr:
        case OpCode::kJmp:
        case OpCode::kJmpIf:
        case OpCode::kJmpIfZero:
        case OpCode::kJmpIfNotZero:
        case OpCode::kDecJnz:
        case OpCode::kAddDecJnz:
        case OpCode::kSubImmJnz:
        case OpCode::kAddImmJnz:
        case OpCode::kRet:
            return true;
        default:
            return false;
    }
}

bool InstructionWritesReg(const Instruction& inst, uint32_t reg) {
    switch (inst.opcode) {
        case OpCode::kMov:
        case OpCode::kLoadK:
        case OpCode::kAdd:
        case OpCode::kSub:
        case OpCode::kMul:
        case OpCode::kDiv:
        case OpCode::kCmpEq:
        case OpCode::kAddImm:
        case OpCode::kSubImm:
        case OpCode::kInc:
        case OpCode::kDec:
        case OpCode::kSubImmJnz:
        case OpCode::kAddImmJnz:
        case OpCode::kStrLen:
        case OpCode::kStrConcat:
        case OpCode::kBitAnd:
        case OpCode::kBitOr:
        case OpCode::kBitXor:
        case OpCode::kBitNot:
        case OpCode::kBitShl:
        case OpCode::kBitShr:
        case OpCode::kCall:
        case OpCode::kCallClosure:
        case OpCode::kFfiCall:
        case OpCode::kClosure:
        case OpCode::kGetUpval:
        case OpCode::kNewObject:
        case OpCode::kInvoke:
            return inst.a == reg;
        case OpCode::kDecJnz:
            return inst.a == reg;
        case OpCode::kAddDecJnz:
            return inst.a == reg || inst.b == reg;
        case OpCode::kVarArg:
            if (inst.c == 0) {
                return false;
            }
            if (inst.a == reg) {
                return true;
            }
            return reg >= inst.a && reg < inst.a + inst.c;
        default:
            return false;
    }
}

uint32_t MapInlineReg(uint32_t reg, uint32_t scratch_base) {
    return scratch_base + reg;
}

bool RemapInlineInstructionRegs(Instruction* inst, uint32_t scratch_base) {
    auto map_reg = [&](uint32_t* field) {
        *field = MapInlineReg(*field, scratch_base);
    };

    switch (inst->opcode) {
        case OpCode::kNop:
        case OpCode::kJmp:
            return true;
        case OpCode::kMov:
            map_reg(&inst->a);
            map_reg(&inst->b);
            return true;
        case OpCode::kLoadK:
            map_reg(&inst->a);
            return true;
        case OpCode::kAdd:
        case OpCode::kSub:
        case OpCode::kMul:
        case OpCode::kDiv:
        case OpCode::kCmpEq:
        case OpCode::kStrConcat:
        case OpCode::kBitAnd:
        case OpCode::kBitOr:
        case OpCode::kBitXor:
        case OpCode::kBitShl:
        case OpCode::kBitShr:
            map_reg(&inst->a);
            map_reg(&inst->b);
            map_reg(&inst->c);
            return true;
        case OpCode::kAddImm:
        case OpCode::kSubImm:
        case OpCode::kStrLen:
        case OpCode::kBitNot:
            map_reg(&inst->a);
            map_reg(&inst->b);
            return true;
        case OpCode::kInc:
        case OpCode::kDec:
        case OpCode::kRet:
        case OpCode::kThrow:
        case OpCode::kJmpIf:
        case OpCode::kJmpIfZero:
        case OpCode::kJmpIfNotZero:
        case OpCode::kDecJnz:
        case OpCode::kSubImmJnz:
        case OpCode::kAddImmJnz:
            map_reg(&inst->a);
            return true;
        case OpCode::kAddDecJnz:
            map_reg(&inst->a);
            map_reg(&inst->b);
            return true;
        default:
            return false;
    }
}

bool IsDirectRecursive(const Module& module, size_t function_index) {
    const auto& function = module.functions()[function_index];
    const auto code = function.FlattenedInstructions();
    for (const auto& inst : code) {
        if ((inst.opcode == OpCode::kCall || inst.opcode == OpCode::kTailCall) &&
            inst.b == function_index) {
            return true;
        }
    }
    return false;
}

bool IsInlineCandidate(
    const Module& module, size_t function_index, uint32_t inline_max_inst) {
    const auto& function = module.functions()[function_index];
    if (function.is_exported() || function.is_vararg() || function.upvalue_count() != 0) {
        return false;
    }
    if (function.reg_count() == 0) {
        return false;
    }
    if (IsDirectRecursive(module, function_index)) {
        return false;
    }

    const auto code = function.FlattenedInstructions();
    if (code.empty() || code.size() > inline_max_inst) {
        return false;
    }
    if (ContainsTryInstructions(code)) {
        return false;
    }

    size_t ret_count = 0;
    for (size_t i = 0; i < code.size(); ++i) {
        const auto& inst = code[i];
        if (!IsInlineSafeOpcode(inst.opcode)) {
            return false;
        }
        if (IsRelativeJumpOpcode(inst.opcode)) {
            const int64_t target =
                static_cast<int64_t>(i) + static_cast<int64_t>(RelativeJumpDelta(inst));
            if (target < 0 || target >= static_cast<int64_t>(code.size())) {
                return false;
            }
        }
        if (inst.opcode == OpCode::kRet) {
            if (inst.a >= function.reg_count()) {
                return false;
            }
            ++ret_count;
            if (i + 1 != code.size()) {
                return false;
            }
            continue;
        }
        if (IsTerminator(inst.opcode)) {
            return false;
        }
    }
    if (ret_count != 1) {
        return false;
    }
    return true;
}

struct FfiForwarderInfo {
    uint32_t symbol_const_index = 0;
    uint32_t arity = 0;
};

std::optional<FfiForwarderInfo> AnalyzeFfiForwarderFunction(
    const Module& module, size_t function_index) {
    const auto& function = module.functions()[function_index];
    if (function.is_exported() || function.is_vararg() || function.upvalue_count() != 0) {
        return std::nullopt;
    }

    const uint32_t arity = function.param_count();
    const auto code = function.FlattenedInstructions();
    if (code.size() != static_cast<size_t>(arity) + 2 || ContainsTryInstructions(code)) {
        return std::nullopt;
    }

    for (uint32_t i = 0; i < arity; ++i) {
        const auto& inst = code[i];
        if (inst.opcode != OpCode::kMov || inst.a != i + 1 || inst.b != i) {
            return std::nullopt;
        }
    }

    const auto& ffi_inst = code[arity];
    if (ffi_inst.opcode != OpCode::kFfiCall || ffi_inst.a != 0 || ffi_inst.c != arity) {
        return std::nullopt;
    }
    if (ffi_inst.b >= module.constants().size() ||
        module.constants()[ffi_inst.b].type != ConstantType::kUtf8) {
        return std::nullopt;
    }

    const auto& ret_inst = code[arity + 1];
    if (ret_inst.opcode != OpCode::kRet || ret_inst.a != 0) {
        return std::nullopt;
    }

    return FfiForwarderInfo{ffi_inst.b, arity};
}

std::vector<BytecodeOptPass> BasePassesForLevel(BytecodeOptLevel level) {
    switch (level) {
        case BytecodeOptLevel::kO0:
            return {};
        case BytecodeOptLevel::kO1:
            return {
                BytecodeOptPass::kPeephole,
                BytecodeOptPass::kTailcall,
                BytecodeOptPass::kLoopFusion,
                BytecodeOptPass::kCleanup,
            };
        case BytecodeOptLevel::kO2:
            return {
                BytecodeOptPass::kPeephole,
                BytecodeOptPass::kTailcall,
                BytecodeOptPass::kLoopFusion,
                BytecodeOptPass::kCleanup,
                BytecodeOptPass::kConstFold,
                BytecodeOptPass::kCopyProp,
                BytecodeOptPass::kDce,
                BytecodeOptPass::kCleanup,
            };
        case BytecodeOptLevel::kO3:
            return {
                BytecodeOptPass::kPeephole,
                BytecodeOptPass::kTailcall,
                BytecodeOptPass::kLoopFusion,
                BytecodeOptPass::kCleanup,
                BytecodeOptPass::kConstFold,
                BytecodeOptPass::kCopyProp,
                BytecodeOptPass::kDce,
                BytecodeOptPass::kCleanup,
                BytecodeOptPass::kInlineSmall,
                BytecodeOptPass::kCopyProp,
                BytecodeOptPass::kDce,
                BytecodeOptPass::kCleanup,
            };
    }
    return {};
}

std::vector<BytecodeOptPass> BuildPassPipeline(const BytecodeOptOptions& options) {
    auto pipeline = BasePassesForLevel(options.level);
    for (auto pass : options.enable_passes) {
        if (std::find(pipeline.begin(), pipeline.end(), pass) == pipeline.end()) {
            pipeline.push_back(pass);
        }
    }
    if (!options.disable_passes.empty()) {
        std::unordered_set<BytecodeOptPass> disabled(
            options.disable_passes.begin(), options.disable_passes.end());
        pipeline.erase(
            std::remove_if(
                pipeline.begin(),
                pipeline.end(),
                [&](BytecodeOptPass pass) { return disabled.contains(pass); }),
            pipeline.end());
    }
    return pipeline;
}

PassRunResult RunPeepholePass(Function* function) {
    PassRunResult result;
    auto code = function->FlattenedInstructions();
    for (auto& inst : code) {
        switch (inst.opcode) {
            case OpCode::kMov:
                if (inst.a == inst.b) {
                    inst = MakeInstruction(OpCode::kNop);
                    result.changed = true;
                    ++result.rewritten;
                }
                break;
            case OpCode::kJmp:
                if (static_cast<int32_t>(inst.a) == 1) {
                    inst = MakeInstruction(OpCode::kNop);
                    result.changed = true;
                    ++result.rewritten;
                }
                break;
            case OpCode::kJmpIf:
            case OpCode::kJmpIfZero:
            case OpCode::kJmpIfNotZero:
                if (static_cast<int32_t>(inst.b) == 1) {
                    inst = MakeInstruction(OpCode::kNop);
                    result.changed = true;
                    ++result.rewritten;
                }
                break;
            case OpCode::kAddImm:
            case OpCode::kSubImm:
                if (static_cast<int32_t>(inst.c) == 0) {
                    if (inst.a == inst.b) {
                        inst = MakeInstruction(OpCode::kNop);
                    } else {
                        inst = MakeInstruction(OpCode::kMov, inst.a, inst.b, 0);
                    }
                    result.changed = true;
                    ++result.rewritten;
                }
                break;
            default:
                break;
        }
    }

    if (result.changed) {
        RebuildFunctionCode(function, code);
    }
    return result;
}

PassRunResult RunTailcallPass(Function* function) {
    PassRunResult result;
    auto code = function->FlattenedInstructions();
    for (size_t i = 0; i + 1 < code.size(); ++i) {
        auto& call_inst = code[i];
        auto& ret_inst = code[i + 1];
        if (ret_inst.opcode != OpCode::kRet || call_inst.a != ret_inst.a) {
            continue;
        }
        if (call_inst.opcode == OpCode::kCall) {
            call_inst.opcode = OpCode::kTailCall;
            ret_inst = MakeInstruction(OpCode::kNop);
            result.changed = true;
            result.rewritten += 2;
        } else if (call_inst.opcode == OpCode::kCallClosure) {
            call_inst.opcode = OpCode::kTailCallClosure;
            ret_inst = MakeInstruction(OpCode::kNop);
            result.changed = true;
            result.rewritten += 2;
        }
    }

    if (result.changed) {
        RebuildFunctionCode(function, code);
    }
    return result;
}

PassRunResult RunLoopFusionPass(Function* function) {
    PassRunResult result;
    auto code = function->FlattenedInstructions();
    for (size_t i = 0; i + 1 < code.size(); ++i) {
        auto& first = code[i];
        auto& second = code[i + 1];

        if (first.opcode == OpCode::kAdd && second.opcode == OpCode::kDecJnz &&
            first.a == first.b && first.c == second.a) {
            const int32_t target_delta = static_cast<int32_t>(second.b) + 1;
            first = MakeInstruction(
                OpCode::kAddDecJnz,
                first.a,
                first.c,
                static_cast<uint32_t>(target_delta));
            second = MakeInstruction(OpCode::kNop);
            result.changed = true;
            result.rewritten += 2;
            continue;
        }

        if (first.opcode == OpCode::kSubImm && second.opcode == OpCode::kJmpIfNotZero &&
            first.a == first.b && second.a == first.a) {
            const int32_t target_delta = static_cast<int32_t>(second.b) + 1;
            first = MakeInstruction(
                OpCode::kSubImmJnz,
                first.a,
                first.c,
                static_cast<uint32_t>(target_delta));
            second = MakeInstruction(OpCode::kNop);
            result.changed = true;
            result.rewritten += 2;
            continue;
        }

        if (first.opcode == OpCode::kAddImm && second.opcode == OpCode::kJmpIfNotZero &&
            first.a == first.b && second.a == first.a) {
            const int32_t target_delta = static_cast<int32_t>(second.b) + 1;
            first = MakeInstruction(
                OpCode::kAddImmJnz,
                first.a,
                first.c,
                static_cast<uint32_t>(target_delta));
            second = MakeInstruction(OpCode::kNop);
            result.changed = true;
            result.rewritten += 2;
            continue;
        }
    }

    if (result.changed) {
        RebuildFunctionCode(function, code);
    }
    return result;
}

bool KnownValueEquals(const KnownValue& lhs, const KnownValue& rhs) {
    if (lhs.kind != rhs.kind) {
        return false;
    }
    switch (lhs.kind) {
        case KnownValue::Kind::kUnknown:
            return true;
        case KnownValue::Kind::kInt64:
            return lhs.int64_value == rhs.int64_value;
        case KnownValue::Kind::kBool:
            return lhs.bool_value == rhs.bool_value;
    }
    return false;
}

KnownValue MergeKnownValues(KnownValue lhs, KnownValue rhs) {
    if (lhs.kind == KnownValue::Kind::kUnknown || rhs.kind == KnownValue::Kind::kUnknown) {
        return KnownValue::Unknown();
    }
    if (lhs.kind != rhs.kind) {
        return KnownValue::Unknown();
    }
    if (KnownValueEquals(lhs, rhs)) {
        return lhs;
    }
    return KnownValue::Unknown();
}

std::optional<int64_t> FoldIntBinary(OpCode opcode, int64_t lhs, int64_t rhs) {
    switch (opcode) {
        case OpCode::kAdd:
            return lhs + rhs;
        case OpCode::kSub:
            return lhs - rhs;
        case OpCode::kMul:
            return lhs * rhs;
        case OpCode::kDiv:
            if (rhs == 0) {
                return std::nullopt;
            }
            return lhs / rhs;
        case OpCode::kBitAnd:
            return lhs & rhs;
        case OpCode::kBitOr:
            return lhs | rhs;
        case OpCode::kBitXor:
            return lhs ^ rhs;
        case OpCode::kBitShl:
            return lhs << (static_cast<uint32_t>(rhs) & 63u);
        case OpCode::kBitShr:
            return lhs >> (static_cast<uint32_t>(rhs) & 63u);
        default:
            return std::nullopt;
    }
}

std::vector<std::vector<size_t>> CollectPredecessors(const std::vector<Instruction>& code) {
    std::vector<std::vector<size_t>> preds(code.size());
    auto add_edge = [&](size_t from, int64_t to) {
        if (to < 0 || to >= static_cast<int64_t>(code.size())) {
            return;
        }
        preds[static_cast<size_t>(to)].push_back(from);
    };

    for (size_t i = 0; i < code.size(); ++i) {
        const auto& inst = code[i];
        switch (inst.opcode) {
            case OpCode::kJmp:
                add_edge(i, static_cast<int64_t>(i) + static_cast<int32_t>(inst.a));
                break;
            case OpCode::kJmpIf:
            case OpCode::kJmpIfZero:
            case OpCode::kJmpIfNotZero:
            case OpCode::kDecJnz:
                add_edge(i, static_cast<int64_t>(i) + static_cast<int32_t>(inst.b));
                add_edge(i, static_cast<int64_t>(i) + 1);
                break;
            case OpCode::kAddDecJnz:
            case OpCode::kSubImmJnz:
            case OpCode::kAddImmJnz:
                add_edge(i, static_cast<int64_t>(i) + static_cast<int32_t>(inst.c));
                add_edge(i, static_cast<int64_t>(i) + 1);
                break;
            default:
                if (!IsTerminator(inst.opcode)) {
                    add_edge(i, static_cast<int64_t>(i) + 1);
                }
                break;
        }
    }

    for (auto& p : preds) {
        std::sort(p.begin(), p.end());
        p.erase(std::unique(p.begin(), p.end()), p.end());
    }
    return preds;
}

PassRunResult RunLicmHeaderLoadPass(Function* function) {
    PassRunResult result;
    auto code = function->FlattenedInstructions();
    if (code.empty() || ContainsTryInstructions(code)) {
        return result;
    }

    const auto preds = CollectPredecessors(code);
    for (size_t back_pc = 0; back_pc < code.size(); ++back_pc) {
        const auto& jump_inst = code[back_pc];
        if (!IsRelativeJumpOpcode(jump_inst.opcode)) {
            continue;
        }
        const int64_t target =
            static_cast<int64_t>(back_pc) + static_cast<int64_t>(RelativeJumpDelta(jump_inst));
        if (target <= 0 || target >= static_cast<int64_t>(back_pc)) {
            continue;
        }
        const size_t header = static_cast<size_t>(target);
        if (header + 1 >= code.size()) {
            continue;
        }
        if (preds[header].size() != 2) {
            continue;
        }
        const bool has_preheader = preds[header][0] == header - 1 || preds[header][1] == header - 1;
        const bool has_backedge = preds[header][0] == back_pc || preds[header][1] == back_pc;
        if (!has_preheader || !has_backedge) {
            continue;
        }

        const auto& header_inst = code[header];
        if (header_inst.opcode != OpCode::kLoadK) {
            continue;
        }
        const uint32_t dst = header_inst.a;
        bool rewritten_in_loop = false;
        for (size_t i = header + 1; i <= back_pc; ++i) {
            if (InstructionWritesReg(code[i], dst)) {
                rewritten_in_loop = true;
                break;
            }
        }
        if (rewritten_in_loop) {
            continue;
        }

        const int64_t new_delta = static_cast<int64_t>(header + 1) - static_cast<int64_t>(back_pc);
        if (new_delta < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
            new_delta > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
            continue;
        }
        if (new_delta == RelativeJumpDelta(code[back_pc])) {
            continue;
        }
        SetRelativeJumpDelta(&code[back_pc], static_cast<int32_t>(new_delta));
        result.changed = true;
        ++result.rewritten;
    }

    if (result.changed) {
        RebuildFunctionCode(function, code);
    }
    return result;
}

PassRunResult RunStrengthReductionPass(
    Function* function, Module* module,
    std::unordered_map<int64_t, uint32_t>* int64_cache) {
    PassRunResult result;
    auto code = function->FlattenedInstructions();
    if (code.empty()) {
        return result;
    }
    const auto labels = CollectJumpTargets(code);

    std::unordered_map<uint32_t, KnownValue> known;
    auto write_int64_loadk = [&](Instruction* inst, uint32_t dst, int64_t value) {
        const uint32_t const_idx = GetOrAddInt64Constant(module, int64_cache, value);
        *inst = MakeInstruction(OpCode::kLoadK, dst, const_idx, 0);
    };

    for (size_t i = 0; i < code.size(); ++i) {
        auto& inst = code[i];
        if (IsLabelTarget(labels, i)) {
            known.clear();
        }

        bool inst_changed = false;
        switch (inst.opcode) {
            case OpCode::kLoadK:
                if (inst.b < module->constants().size() &&
                    module->constants()[inst.b].type == ConstantType::kInt64) {
                    SetKnown(
                        &known,
                        inst.a,
                        KnownValue::Int64(module->constants()[inst.b].int64_value));
                } else {
                    SetKnown(&known, inst.a, KnownValue::Unknown());
                }
                break;
            case OpCode::kMov:
                SetKnown(&known, inst.a, GetKnown(known, inst.b));
                break;
            case OpCode::kMul: {
                const auto lhs = KnownInt64(GetKnown(known, inst.b));
                const auto rhs = KnownInt64(GetKnown(known, inst.c));
                if (lhs.has_value() || rhs.has_value()) {
                    const bool lhs_const = lhs.has_value();
                    const int64_t k = lhs_const ? *lhs : *rhs;
                    const uint32_t x = lhs_const ? inst.c : inst.b;
                    if (k == 0) {
                        write_int64_loadk(&inst, inst.a, 0);
                        SetKnown(&known, inst.a, KnownValue::Int64(0));
                        inst_changed = true;
                    } else if (k == 1) {
                        inst = MakeInstruction(OpCode::kMov, inst.a, x, 0);
                        SetKnown(&known, inst.a, GetKnown(known, x));
                        inst_changed = true;
                    } else if (k == 2) {
                        inst = MakeInstruction(OpCode::kAdd, inst.a, x, x);
                        const auto x_known = KnownInt64(GetKnown(known, x));
                        if (x_known.has_value()) {
                            SetKnown(&known, inst.a, KnownValue::Int64(*x_known + *x_known));
                        } else {
                            SetKnown(&known, inst.a, KnownValue::Unknown());
                        }
                        inst_changed = true;
                    } else {
                        SetKnown(&known, inst.a, KnownValue::Unknown());
                    }
                } else {
                    SetKnown(&known, inst.a, KnownValue::Unknown());
                }
                break;
            }
            case OpCode::kAdd:
            case OpCode::kSub:
            case OpCode::kDiv:
            case OpCode::kBitAnd:
            case OpCode::kBitOr:
            case OpCode::kBitXor:
            case OpCode::kBitShl:
            case OpCode::kBitShr: {
                const auto lhs = KnownInt64(GetKnown(known, inst.b));
                const auto rhs = KnownInt64(GetKnown(known, inst.c));
                if (lhs.has_value() && rhs.has_value()) {
                    auto folded = FoldIntBinary(inst.opcode, *lhs, *rhs);
                    if (folded.has_value()) {
                        SetKnown(&known, inst.a, KnownValue::Int64(*folded));
                    } else {
                        SetKnown(&known, inst.a, KnownValue::Unknown());
                    }
                } else {
                    SetKnown(&known, inst.a, KnownValue::Unknown());
                }
                break;
            }
            case OpCode::kAddImm:
            case OpCode::kSubImm: {
                const auto lhs = KnownInt64(GetKnown(known, inst.b));
                if (lhs.has_value()) {
                    const int64_t imm = static_cast<int32_t>(inst.c);
                    const int64_t out =
                        inst.opcode == OpCode::kAddImm ? (*lhs + imm) : (*lhs - imm);
                    SetKnown(&known, inst.a, KnownValue::Int64(out));
                } else {
                    SetKnown(&known, inst.a, KnownValue::Unknown());
                }
                break;
            }
            case OpCode::kInc:
            case OpCode::kDec: {
                const auto value = KnownInt64(GetKnown(known, inst.a));
                if (value.has_value()) {
                    SetKnown(
                        &known,
                        inst.a,
                        KnownValue::Int64(
                            inst.opcode == OpCode::kInc ? *value + 1 : *value - 1));
                } else {
                    SetKnown(&known, inst.a, KnownValue::Unknown());
                }
                break;
            }
            case OpCode::kCmpEq: {
                const auto lhs = KnownInt64(GetKnown(known, inst.b));
                const auto rhs = KnownInt64(GetKnown(known, inst.c));
                if (lhs.has_value() && rhs.has_value()) {
                    SetKnown(&known, inst.a, KnownValue::Bool(*lhs == *rhs));
                } else {
                    SetKnown(&known, inst.a, KnownValue::Unknown());
                }
                break;
            }
            case OpCode::kCall:
            case OpCode::kCallClosure:
            case OpCode::kFfiCall:
            case OpCode::kClosure:
            case OpCode::kGetUpval:
            case OpCode::kVarArg:
            case OpCode::kStrLen:
            case OpCode::kStrConcat:
            case OpCode::kBitNot:
            case OpCode::kNewObject:
            case OpCode::kInvoke:
                SetKnown(&known, inst.a, KnownValue::Unknown());
                break;
            case OpCode::kAddDecJnz:
                SetKnown(&known, inst.a, KnownValue::Unknown());
                SetKnown(&known, inst.b, KnownValue::Unknown());
                break;
            case OpCode::kSubImmJnz:
            case OpCode::kAddImmJnz:
            case OpCode::kDecJnz:
                SetKnown(&known, inst.a, KnownValue::Unknown());
                break;
            default:
                break;
        }

        if (inst_changed) {
            result.changed = true;
            ++result.rewritten;
        }
        if (IsRelativeJumpOpcode(inst.opcode) || IsTerminator(inst.opcode)) {
            known.clear();
        }
    }

    if (result.changed) {
        RebuildFunctionCode(function, code);
    }
    return result;
}

PassRunResult RunSccpPass(
    Function* function, Module* module,
    std::unordered_map<int64_t, uint32_t>* int64_cache) {
    PassRunResult result;
    auto code = function->FlattenedInstructions();
    if (code.empty()) {
        return result;
    }

    const size_t reg_count = function->reg_count();
    if (reg_count == 0) {
        return result;
    }
    using RegState = std::vector<KnownValue>;
    const RegState unknown_state(reg_count, KnownValue::Unknown());

    std::vector<RegState> in_states(code.size(), unknown_state);
    std::vector<bool> reachable(code.size(), false);
    std::vector<size_t> worklist;
    worklist.push_back(0);
    reachable[0] = true;

    auto merge_into = [&](size_t target, const RegState& incoming) {
        if (target >= code.size()) {
            return;
        }
        bool changed = false;
        if (!reachable[target]) {
            in_states[target] = incoming;
            reachable[target] = true;
            changed = true;
        } else {
            for (size_t r = 0; r < reg_count; ++r) {
                const auto merged = MergeKnownValues(in_states[target][r], incoming[r]);
                if (!KnownValueEquals(merged, in_states[target][r])) {
                    in_states[target][r] = merged;
                    changed = true;
                }
            }
        }
        if (changed) {
            worklist.push_back(target);
        }
    };

    auto set_unknown = [&](RegState* state, uint32_t reg) {
        if (reg < reg_count) {
            (*state)[reg] = KnownValue::Unknown();
        }
    };
    auto set_unknown_range = [&](RegState* state, uint32_t begin, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            set_unknown(state, begin + i);
        }
    };

    while (!worklist.empty()) {
        const size_t pc = worklist.back();
        worklist.pop_back();
        if (!reachable[pc]) {
            continue;
        }
        const auto& inst = code[pc];
        RegState out = in_states[pc];
        std::optional<bool> branch_taken;

        auto read_reg = [&](uint32_t reg) -> KnownValue {
            if (reg >= reg_count) {
                return KnownValue::Unknown();
            }
            return out[reg];
        };

        switch (inst.opcode) {
            case OpCode::kLoadK:
                if (inst.a < reg_count && inst.b < module->constants().size() &&
                    module->constants()[inst.b].type == ConstantType::kInt64) {
                    out[inst.a] = KnownValue::Int64(module->constants()[inst.b].int64_value);
                } else {
                    set_unknown(&out, inst.a);
                }
                break;
            case OpCode::kMov:
                if (inst.a < reg_count && inst.b < reg_count) {
                    out[inst.a] = out[inst.b];
                } else {
                    set_unknown(&out, inst.a);
                }
                break;
            case OpCode::kAdd:
            case OpCode::kSub:
            case OpCode::kMul:
            case OpCode::kDiv:
            case OpCode::kBitAnd:
            case OpCode::kBitOr:
            case OpCode::kBitXor:
            case OpCode::kBitShl:
            case OpCode::kBitShr: {
                const auto lhs = KnownInt64(read_reg(inst.b));
                const auto rhs = KnownInt64(read_reg(inst.c));
                if (lhs.has_value() && rhs.has_value()) {
                    auto folded = FoldIntBinary(inst.opcode, *lhs, *rhs);
                    if (folded.has_value()) {
                        out[inst.a] = KnownValue::Int64(*folded);
                    } else {
                        set_unknown(&out, inst.a);
                    }
                } else {
                    set_unknown(&out, inst.a);
                }
                break;
            }
            case OpCode::kAddImm:
            case OpCode::kSubImm: {
                const auto lhs = KnownInt64(read_reg(inst.b));
                if (lhs.has_value()) {
                    const int64_t imm = static_cast<int32_t>(inst.c);
                    out[inst.a] = KnownValue::Int64(
                        inst.opcode == OpCode::kAddImm ? *lhs + imm : *lhs - imm);
                } else {
                    set_unknown(&out, inst.a);
                }
                break;
            }
            case OpCode::kInc:
            case OpCode::kDec: {
                const auto v = KnownInt64(read_reg(inst.a));
                if (v.has_value()) {
                    out[inst.a] = KnownValue::Int64(
                        inst.opcode == OpCode::kInc ? *v + 1 : *v - 1);
                } else {
                    set_unknown(&out, inst.a);
                }
                break;
            }
            case OpCode::kCmpEq: {
                const auto lhs = KnownInt64(read_reg(inst.b));
                const auto rhs = KnownInt64(read_reg(inst.c));
                if (lhs.has_value() && rhs.has_value()) {
                    out[inst.a] = KnownValue::Bool(*lhs == *rhs);
                } else {
                    set_unknown(&out, inst.a);
                }
                break;
            }
            case OpCode::kJmpIf: {
                const auto cond = read_reg(inst.a);
                if (cond.kind != KnownValue::Kind::kUnknown) {
                    branch_taken = KnownTruthy(cond);
                }
                break;
            }
            case OpCode::kJmpIfZero:
            case OpCode::kJmpIfNotZero: {
                const auto cond = KnownInt64(read_reg(inst.a));
                if (cond.has_value()) {
                    branch_taken =
                        inst.opcode == OpCode::kJmpIfZero ? (*cond == 0) : (*cond != 0);
                }
                break;
            }
            case OpCode::kDecJnz: {
                const auto counter = KnownInt64(read_reg(inst.a));
                if (counter.has_value()) {
                    const int64_t next = *counter - 1;
                    out[inst.a] = KnownValue::Int64(next);
                    branch_taken = next != 0;
                } else {
                    set_unknown(&out, inst.a);
                }
                break;
            }
            case OpCode::kAddDecJnz: {
                const auto acc = KnownInt64(read_reg(inst.a));
                const auto counter = KnownInt64(read_reg(inst.b));
                if (acc.has_value() && counter.has_value()) {
                    out[inst.a] = KnownValue::Int64(*acc + *counter);
                    const int64_t next = *counter - 1;
                    out[inst.b] = KnownValue::Int64(next);
                    branch_taken = next != 0;
                } else {
                    set_unknown(&out, inst.a);
                    set_unknown(&out, inst.b);
                }
                break;
            }
            case OpCode::kSubImmJnz:
            case OpCode::kAddImmJnz: {
                const auto counter = KnownInt64(read_reg(inst.a));
                if (counter.has_value()) {
                    const int64_t imm = static_cast<int32_t>(inst.b);
                    const int64_t next = inst.opcode == OpCode::kSubImmJnz
                                             ? *counter - imm
                                             : *counter + imm;
                    out[inst.a] = KnownValue::Int64(next);
                    branch_taken = next != 0;
                } else {
                    set_unknown(&out, inst.a);
                }
                break;
            }
            case OpCode::kCall:
            case OpCode::kCallClosure:
            case OpCode::kFfiCall:
            case OpCode::kClosure:
            case OpCode::kGetUpval:
            case OpCode::kStrLen:
            case OpCode::kStrConcat:
            case OpCode::kBitNot:
            case OpCode::kNewObject:
            case OpCode::kInvoke:
                set_unknown(&out, inst.a);
                break;
            case OpCode::kVarArg:
                set_unknown_range(&out, inst.a, inst.c);
                break;
            default:
                break;
        }

        const auto push_fallthrough = [&]() {
            if (pc + 1 < code.size()) {
                merge_into(pc + 1, out);
            }
        };
        const auto push_jump = [&](int32_t delta) {
            const int64_t target = static_cast<int64_t>(pc) + static_cast<int64_t>(delta);
            if (target < 0 || target >= static_cast<int64_t>(code.size())) {
                return;
            }
            merge_into(static_cast<size_t>(target), out);
        };

        switch (inst.opcode) {
            case OpCode::kJmp:
                push_jump(static_cast<int32_t>(inst.a));
                break;
            case OpCode::kJmpIf:
            case OpCode::kJmpIfZero:
            case OpCode::kJmpIfNotZero:
            case OpCode::kDecJnz:
                if (branch_taken.has_value()) {
                    if (*branch_taken) {
                        push_jump(static_cast<int32_t>(inst.b));
                    } else {
                        push_fallthrough();
                    }
                } else {
                    push_jump(static_cast<int32_t>(inst.b));
                    push_fallthrough();
                }
                break;
            case OpCode::kAddDecJnz:
            case OpCode::kSubImmJnz:
            case OpCode::kAddImmJnz:
                if (branch_taken.has_value()) {
                    if (*branch_taken) {
                        push_jump(static_cast<int32_t>(inst.c));
                    } else {
                        push_fallthrough();
                    }
                } else {
                    push_jump(static_cast<int32_t>(inst.c));
                    push_fallthrough();
                }
                break;
            default:
                if (!IsTerminator(inst.opcode)) {
                    push_fallthrough();
                }
                break;
        }
    }

    auto write_int64_loadk = [&](Instruction* inst, uint32_t dst, int64_t value) {
        const uint32_t const_idx = GetOrAddInt64Constant(module, int64_cache, value);
        *inst = MakeInstruction(OpCode::kLoadK, dst, const_idx, 0);
    };

    for (size_t pc = 0; pc < code.size(); ++pc) {
        auto& inst = code[pc];
        if (!reachable[pc]) {
            if (inst.opcode != OpCode::kNop) {
                inst = MakeInstruction(OpCode::kNop);
                result.changed = true;
                ++result.rewritten;
            }
            continue;
        }
        const auto& in = in_states[pc];
        bool inst_changed = false;

        switch (inst.opcode) {
            case OpCode::kMov: {
                if (inst.b < reg_count) {
                    const auto v = KnownInt64(in[inst.b]);
                    if (v.has_value()) {
                        write_int64_loadk(&inst, inst.a, *v);
                        inst_changed = true;
                    }
                }
                break;
            }
            case OpCode::kAdd:
            case OpCode::kSub:
            case OpCode::kMul:
            case OpCode::kDiv:
            case OpCode::kBitAnd:
            case OpCode::kBitOr:
            case OpCode::kBitXor:
            case OpCode::kBitShl:
            case OpCode::kBitShr: {
                if (inst.b < reg_count && inst.c < reg_count) {
                    const auto lhs = KnownInt64(in[inst.b]);
                    const auto rhs = KnownInt64(in[inst.c]);
                    if (lhs.has_value() && rhs.has_value()) {
                        auto folded = FoldIntBinary(inst.opcode, *lhs, *rhs);
                        if (folded.has_value()) {
                            write_int64_loadk(&inst, inst.a, *folded);
                            inst_changed = true;
                        }
                    }
                }
                break;
            }
            case OpCode::kAddImm:
            case OpCode::kSubImm: {
                if (inst.b < reg_count) {
                    const auto src = KnownInt64(in[inst.b]);
                    if (src.has_value()) {
                        const int64_t imm = static_cast<int32_t>(inst.c);
                        write_int64_loadk(
                            &inst,
                            inst.a,
                            inst.opcode == OpCode::kAddImm ? *src + imm : *src - imm);
                        inst_changed = true;
                    }
                }
                break;
            }
            case OpCode::kInc:
            case OpCode::kDec: {
                if (inst.a < reg_count) {
                    const auto src = KnownInt64(in[inst.a]);
                    if (src.has_value()) {
                        write_int64_loadk(
                            &inst,
                            inst.a,
                            inst.opcode == OpCode::kInc ? *src + 1 : *src - 1);
                        inst_changed = true;
                    }
                }
                break;
            }
            case OpCode::kJmpIf: {
                if (inst.a < reg_count && in[inst.a].kind != KnownValue::Kind::kUnknown) {
                    if (KnownTruthy(in[inst.a])) {
                        inst = MakeInstruction(OpCode::kJmp, inst.b, 0, 0);
                    } else {
                        inst = MakeInstruction(OpCode::kNop);
                    }
                    inst_changed = true;
                }
                break;
            }
            case OpCode::kJmpIfZero:
            case OpCode::kJmpIfNotZero: {
                if (inst.a < reg_count) {
                    const auto cond = KnownInt64(in[inst.a]);
                    if (cond.has_value()) {
                        const bool jump =
                            inst.opcode == OpCode::kJmpIfZero ? (*cond == 0) : (*cond != 0);
                        if (jump) {
                            inst = MakeInstruction(OpCode::kJmp, inst.b, 0, 0);
                        } else {
                            inst = MakeInstruction(OpCode::kNop);
                        }
                        inst_changed = true;
                    }
                }
                break;
            }
            default:
                break;
        }

        if (inst_changed) {
            result.changed = true;
            ++result.rewritten;
        }
    }

    if (result.changed) {
        RebuildFunctionCode(function, code);
    }
    return result;
}

PassRunResult RunO3ConstExecPass(
    Function* function, Module* module,
    std::unordered_map<int64_t, uint32_t>* int64_cache) {
    PassRunResult result;
    const auto code = function->FlattenedInstructions();
    if (!IsO3ConstExecEligible(*function, code)) {
        return result;
    }

    std::vector<KnownValue> regs(function->reg_count(), KnownValue::Unknown());
    int64_t pc = 0;
    size_t step_count = 0;

    auto jump_to = [&](int32_t delta) -> bool {
        const int64_t target = pc + static_cast<int64_t>(delta);
        if (target < 0 || target >= static_cast<int64_t>(code.size())) {
            return false;
        }
        pc = target;
        return true;
    };

    while (pc >= 0 && pc < static_cast<int64_t>(code.size())) {
        if (++step_count > kO3ConstExecMaxSteps) {
            return {};
        }
        const auto& inst = code[static_cast<size_t>(pc)];

        switch (inst.opcode) {
            case OpCode::kNop:
                ++pc;
                break;
            case OpCode::kMov:
                if (inst.a >= regs.size() || inst.b >= regs.size()) {
                    return {};
                }
                regs[inst.a] = regs[inst.b];
                ++pc;
                break;
            case OpCode::kLoadK:
                if (inst.a >= regs.size() || inst.b >= module->constants().size()) {
                    return {};
                }
                if (module->constants()[inst.b].type != ConstantType::kInt64) {
                    return {};
                }
                regs[inst.a] =
                    KnownValue::Int64(module->constants()[inst.b].int64_value);
                ++pc;
                break;
            case OpCode::kAdd:
            case OpCode::kSub:
            case OpCode::kMul:
            case OpCode::kDiv:
            case OpCode::kBitAnd:
            case OpCode::kBitOr:
            case OpCode::kBitXor:
            case OpCode::kBitShl:
            case OpCode::kBitShr: {
                if (inst.a >= regs.size()) {
                    return {};
                }
                int64_t lhs = 0;
                int64_t rhs = 0;
                if (!TryReadInt64Reg(regs, inst.b, &lhs) ||
                    !TryReadInt64Reg(regs, inst.c, &rhs)) {
                    return {};
                }
                int64_t out = 0;
                switch (inst.opcode) {
                    case OpCode::kAdd:
                        out = lhs + rhs;
                        break;
                    case OpCode::kSub:
                        out = lhs - rhs;
                        break;
                    case OpCode::kMul:
                        out = lhs * rhs;
                        break;
                    case OpCode::kDiv:
                        if (rhs == 0) {
                            return {};
                        }
                        out = lhs / rhs;
                        break;
                    case OpCode::kBitAnd:
                        out = lhs & rhs;
                        break;
                    case OpCode::kBitOr:
                        out = lhs | rhs;
                        break;
                    case OpCode::kBitXor:
                        out = lhs ^ rhs;
                        break;
                    case OpCode::kBitShl:
                        out = lhs << (static_cast<uint32_t>(rhs) & 63u);
                        break;
                    case OpCode::kBitShr:
                        out = lhs >> (static_cast<uint32_t>(rhs) & 63u);
                        break;
                    default:
                        return {};
                }
                regs[inst.a] = KnownValue::Int64(out);
                ++pc;
                break;
            }
            case OpCode::kAddImm:
            case OpCode::kSubImm: {
                if (inst.a >= regs.size()) {
                    return {};
                }
                int64_t src = 0;
                if (!TryReadInt64Reg(regs, inst.b, &src)) {
                    return {};
                }
                const int64_t imm = static_cast<int32_t>(inst.c);
                regs[inst.a] = KnownValue::Int64(
                    inst.opcode == OpCode::kAddImm ? src + imm : src - imm);
                ++pc;
                break;
            }
            case OpCode::kInc:
            case OpCode::kDec: {
                if (inst.a >= regs.size()) {
                    return {};
                }
                int64_t value = 0;
                if (!TryReadInt64Reg(regs, inst.a, &value)) {
                    return {};
                }
                regs[inst.a] = KnownValue::Int64(
                    inst.opcode == OpCode::kInc ? value + 1 : value - 1);
                ++pc;
                break;
            }
            case OpCode::kCmpEq: {
                if (inst.a >= regs.size()) {
                    return {};
                }
                int64_t lhs = 0;
                int64_t rhs = 0;
                if (!TryReadInt64Reg(regs, inst.b, &lhs) ||
                    !TryReadInt64Reg(regs, inst.c, &rhs)) {
                    return {};
                }
                regs[inst.a] = KnownValue::Bool(lhs == rhs);
                ++pc;
                break;
            }
            case OpCode::kJmp:
                if (!jump_to(static_cast<int32_t>(inst.a))) {
                    return {};
                }
                break;
            case OpCode::kJmpIf: {
                KnownValue cond;
                if (!TryReadKnownReg(regs, inst.a, &cond)) {
                    return {};
                }
                if (KnownTruthy(cond)) {
                    if (!jump_to(static_cast<int32_t>(inst.b))) {
                        return {};
                    }
                } else {
                    ++pc;
                }
                break;
            }
            case OpCode::kJmpIfZero:
            case OpCode::kJmpIfNotZero: {
                int64_t cond = 0;
                if (!TryReadInt64Reg(regs, inst.a, &cond)) {
                    return {};
                }
                const bool jump =
                    (inst.opcode == OpCode::kJmpIfZero) ? (cond == 0) : (cond != 0);
                if (jump) {
                    if (!jump_to(static_cast<int32_t>(inst.b))) {
                        return {};
                    }
                } else {
                    ++pc;
                }
                break;
            }
            case OpCode::kDecJnz: {
                if (inst.a >= regs.size()) {
                    return {};
                }
                int64_t counter = 0;
                if (!TryReadInt64Reg(regs, inst.a, &counter)) {
                    return {};
                }
                const int64_t next = counter - 1;
                regs[inst.a] = KnownValue::Int64(next);
                if (next != 0) {
                    if (!jump_to(static_cast<int32_t>(inst.b))) {
                        return {};
                    }
                } else {
                    ++pc;
                }
                break;
            }
            case OpCode::kAddDecJnz: {
                if (inst.a >= regs.size() || inst.b >= regs.size()) {
                    return {};
                }
                int64_t acc = 0;
                int64_t counter = 0;
                if (!TryReadInt64Reg(regs, inst.a, &acc) ||
                    !TryReadInt64Reg(regs, inst.b, &counter)) {
                    return {};
                }
                regs[inst.a] = KnownValue::Int64(acc + counter);
                const int64_t next_counter = counter - 1;
                regs[inst.b] = KnownValue::Int64(next_counter);
                if (next_counter != 0) {
                    if (!jump_to(static_cast<int32_t>(inst.c))) {
                        return {};
                    }
                } else {
                    ++pc;
                }
                break;
            }
            case OpCode::kSubImmJnz:
            case OpCode::kAddImmJnz: {
                if (inst.a >= regs.size()) {
                    return {};
                }
                int64_t counter = 0;
                if (!TryReadInt64Reg(regs, inst.a, &counter)) {
                    return {};
                }
                const int64_t imm = static_cast<int32_t>(inst.b);
                const int64_t next = inst.opcode == OpCode::kSubImmJnz
                                         ? counter - imm
                                         : counter + imm;
                regs[inst.a] = KnownValue::Int64(next);
                if (next != 0) {
                    if (!jump_to(static_cast<int32_t>(inst.c))) {
                        return {};
                    }
                } else {
                    ++pc;
                }
                break;
            }
            case OpCode::kRet: {
                int64_t ret_value = 0;
                if (!TryReadInt64Reg(regs, inst.a, &ret_value)) {
                    return {};
                }
                const uint32_t const_idx =
                    GetOrAddInt64Constant(module, int64_cache, ret_value);
                std::vector<Instruction> folded_code;
                folded_code.reserve(2);
                folded_code.push_back(MakeInstruction(OpCode::kLoadK, 0, const_idx, 0));
                folded_code.push_back(MakeInstruction(OpCode::kRet, 0, 0, 0));

                if (code.size() == folded_code.size() &&
                    code[0].opcode == folded_code[0].opcode &&
                    code[0].a == folded_code[0].a &&
                    code[0].b == folded_code[0].b &&
                    code[1].opcode == folded_code[1].opcode &&
                    code[1].a == folded_code[1].a) {
                    return {};
                }

                RebuildFunctionCode(function, folded_code);
                result.changed = true;
                result.rewritten = 2;
                if (code.size() > folded_code.size()) {
                    result.removed = code.size() - folded_code.size();
                }
                return result;
            }
            default:
                return {};
        }
    }

    return {};
}

PassRunResult RunConstFoldPass(
    Function* function, Module* module,
    std::unordered_map<int64_t, uint32_t>* int64_cache,
    bool allow_o3_const_exec) {
    PassRunResult result;
    auto code = function->FlattenedInstructions();
    const auto labels = CollectJumpTargets(code);

    std::unordered_map<uint32_t, KnownValue> known;

    auto write_int64_loadk = [&](Instruction* inst, uint32_t dst, int64_t value) {
        const uint32_t const_idx = GetOrAddInt64Constant(module, int64_cache, value);
        *inst = MakeInstruction(OpCode::kLoadK, dst, const_idx, 0);
    };

    for (size_t i = 0; i < code.size(); ++i) {
        auto& inst = code[i];
        if (IsLabelTarget(labels, i)) {
            known.clear();
        }

        bool inst_changed = false;

        switch (inst.opcode) {
            case OpCode::kLoadK: {
                if (inst.b < module->constants().size() &&
                    module->constants()[inst.b].type == ConstantType::kInt64) {
                    SetKnown(
                        &known,
                        inst.a,
                        KnownValue::Int64(module->constants()[inst.b].int64_value));
                } else {
                    SetKnown(&known, inst.a, KnownValue::Unknown());
                }
                break;
            }
            case OpCode::kMov: {
                SetKnown(&known, inst.a, GetKnown(known, inst.b));
                break;
            }
            case OpCode::kAdd:
            case OpCode::kSub:
            case OpCode::kMul:
            case OpCode::kDiv:
            case OpCode::kBitAnd:
            case OpCode::kBitOr:
            case OpCode::kBitXor:
            case OpCode::kBitShl:
            case OpCode::kBitShr: {
                const auto lhs = KnownInt64(GetKnown(known, inst.b));
                const auto rhs = KnownInt64(GetKnown(known, inst.c));
                if (lhs.has_value() && rhs.has_value()) {
                    int64_t folded = 0;
                    bool ok = true;
                    switch (inst.opcode) {
                        case OpCode::kAdd:
                            folded = *lhs + *rhs;
                            break;
                        case OpCode::kSub:
                            folded = *lhs - *rhs;
                            break;
                        case OpCode::kMul:
                            folded = *lhs * *rhs;
                            break;
                        case OpCode::kDiv:
                            if (*rhs == 0) {
                                ok = false;
                            } else {
                                folded = *lhs / *rhs;
                            }
                            break;
                        case OpCode::kBitAnd:
                            folded = *lhs & *rhs;
                            break;
                        case OpCode::kBitOr:
                            folded = *lhs | *rhs;
                            break;
                        case OpCode::kBitXor:
                            folded = *lhs ^ *rhs;
                            break;
                        case OpCode::kBitShl:
                            folded = *lhs << (static_cast<uint32_t>(*rhs) & 63u);
                            break;
                        case OpCode::kBitShr:
                            folded = *lhs >> (static_cast<uint32_t>(*rhs) & 63u);
                            break;
                        default:
                            ok = false;
                            break;
                    }
                    if (ok) {
                        write_int64_loadk(&inst, inst.a, folded);
                        SetKnown(&known, inst.a, KnownValue::Int64(folded));
                        inst_changed = true;
                        break;
                    }
                }
                SetKnown(&known, inst.a, KnownValue::Unknown());
                break;
            }
            case OpCode::kAddImm: {
                const auto lhs = KnownInt64(GetKnown(known, inst.b));
                if (lhs.has_value()) {
                    const int64_t folded = *lhs + static_cast<int32_t>(inst.c);
                    write_int64_loadk(&inst, inst.a, folded);
                    SetKnown(&known, inst.a, KnownValue::Int64(folded));
                    inst_changed = true;
                } else {
                    SetKnown(&known, inst.a, KnownValue::Unknown());
                }
                break;
            }
            case OpCode::kSubImm: {
                const auto lhs = KnownInt64(GetKnown(known, inst.b));
                if (lhs.has_value()) {
                    const int64_t folded = *lhs - static_cast<int32_t>(inst.c);
                    write_int64_loadk(&inst, inst.a, folded);
                    SetKnown(&known, inst.a, KnownValue::Int64(folded));
                    inst_changed = true;
                } else {
                    SetKnown(&known, inst.a, KnownValue::Unknown());
                }
                break;
            }
            case OpCode::kInc: {
                const auto value = KnownInt64(GetKnown(known, inst.a));
                if (value.has_value()) {
                    const int64_t folded = *value + 1;
                    write_int64_loadk(&inst, inst.a, folded);
                    SetKnown(&known, inst.a, KnownValue::Int64(folded));
                    inst_changed = true;
                } else {
                    SetKnown(&known, inst.a, KnownValue::Unknown());
                }
                break;
            }
            case OpCode::kDec: {
                const auto value = KnownInt64(GetKnown(known, inst.a));
                if (value.has_value()) {
                    const int64_t folded = *value - 1;
                    write_int64_loadk(&inst, inst.a, folded);
                    SetKnown(&known, inst.a, KnownValue::Int64(folded));
                    inst_changed = true;
                } else {
                    SetKnown(&known, inst.a, KnownValue::Unknown());
                }
                break;
            }
            case OpCode::kCmpEq: {
                const auto lhs = KnownInt64(GetKnown(known, inst.b));
                const auto rhs = KnownInt64(GetKnown(known, inst.c));
                if (lhs.has_value() && rhs.has_value()) {
                    SetKnown(&known, inst.a, KnownValue::Bool(*lhs == *rhs));
                } else {
                    SetKnown(&known, inst.a, KnownValue::Unknown());
                }
                break;
            }
            case OpCode::kJmpIf: {
                const auto cond = GetKnown(known, inst.a);
                if (cond.kind != KnownValue::Kind::kUnknown) {
                    const int32_t delta = static_cast<int32_t>(inst.b);
                    if (KnownTruthy(cond)) {
                        inst = MakeInstruction(OpCode::kJmp, static_cast<uint32_t>(delta), 0, 0);
                    } else {
                        inst = MakeInstruction(OpCode::kNop);
                    }
                    inst_changed = true;
                }
                break;
            }
            case OpCode::kJmpIfZero: {
                const auto cond = KnownInt64(GetKnown(known, inst.a));
                if (cond.has_value()) {
                    if (*cond == 0) {
                        const int32_t delta = static_cast<int32_t>(inst.b);
                        inst = MakeInstruction(OpCode::kJmp, static_cast<uint32_t>(delta), 0, 0);
                    } else {
                        inst = MakeInstruction(OpCode::kNop);
                    }
                    inst_changed = true;
                }
                break;
            }
            case OpCode::kJmpIfNotZero: {
                const auto cond = KnownInt64(GetKnown(known, inst.a));
                if (cond.has_value()) {
                    if (*cond != 0) {
                        const int32_t delta = static_cast<int32_t>(inst.b);
                        inst = MakeInstruction(OpCode::kJmp, static_cast<uint32_t>(delta), 0, 0);
                    } else {
                        inst = MakeInstruction(OpCode::kNop);
                    }
                    inst_changed = true;
                }
                break;
            }
            case OpCode::kCall:
            case OpCode::kCallClosure:
            case OpCode::kFfiCall:
            case OpCode::kClosure:
            case OpCode::kGetUpval:
            case OpCode::kVarArg:
            case OpCode::kStrLen:
            case OpCode::kStrConcat:
            case OpCode::kBitNot:
            case OpCode::kNewObject:
            case OpCode::kInvoke:
                SetKnown(&known, inst.a, KnownValue::Unknown());
                break;
            case OpCode::kAddDecJnz:
                SetKnown(&known, inst.a, KnownValue::Unknown());
                SetKnown(&known, inst.b, KnownValue::Unknown());
                break;
            case OpCode::kSubImmJnz:
            case OpCode::kAddImmJnz:
            case OpCode::kDecJnz:
                SetKnown(&known, inst.a, KnownValue::Unknown());
                break;
            default:
                break;
        }

        if (inst_changed) {
            result.changed = true;
            ++result.rewritten;
        }

        if (IsRelativeJumpOpcode(inst.opcode) || IsTerminator(inst.opcode)) {
            known.clear();
        }
    }

    if (result.changed) {
        RebuildFunctionCode(function, code);
    }

    if (allow_o3_const_exec) {
        MergePassResult(&result, RunSccpPass(function, module, int64_cache));
        MergePassResult(&result, RunO3ConstExecPass(function, module, int64_cache));
    }

    return result;
}

PassRunResult RunCopyPropPass(Function* function) {
    PassRunResult result;
    auto code = function->FlattenedInstructions();
    const auto labels = CollectJumpTargets(code);

    std::unordered_map<uint32_t, uint32_t> alias;

    for (size_t i = 0; i < code.size(); ++i) {
        auto& inst = code[i];
        if (IsLabelTarget(labels, i)) {
            alias.clear();
        }

        if (RemapSourceRegsWithAlias(&inst, alias)) {
            result.changed = true;
            ++result.rewritten;
        }

        switch (inst.opcode) {
            case OpCode::kMov:
                KillAliasForWrite(&alias, inst.a);
                if (inst.a == inst.b) {
                    inst = MakeInstruction(OpCode::kNop);
                    result.changed = true;
                    ++result.rewritten;
                } else {
                    alias.insert_or_assign(inst.a, inst.b);
                }
                break;
            case OpCode::kLoadK:
            case OpCode::kAdd:
            case OpCode::kSub:
            case OpCode::kMul:
            case OpCode::kDiv:
            case OpCode::kCmpEq:
            case OpCode::kAddImm:
            case OpCode::kSubImm:
            case OpCode::kInc:
            case OpCode::kDec:
            case OpCode::kStrLen:
            case OpCode::kStrConcat:
            case OpCode::kBitAnd:
            case OpCode::kBitOr:
            case OpCode::kBitXor:
            case OpCode::kBitNot:
            case OpCode::kBitShl:
            case OpCode::kBitShr:
            case OpCode::kCall:
            case OpCode::kCallClosure:
            case OpCode::kFfiCall:
            case OpCode::kClosure:
            case OpCode::kGetUpval:
            case OpCode::kSubImmJnz:
            case OpCode::kAddImmJnz:
            case OpCode::kDecJnz:
            case OpCode::kVarArg:
            case OpCode::kNewObject:
            case OpCode::kInvoke:
                KillAliasForWrite(&alias, inst.a);
                break;
            case OpCode::kAddDecJnz:
                KillAliasForWrite(&alias, inst.a);
                KillAliasForWrite(&alias, inst.b);
                break;
            default:
                break;
        }

        if (IsRelativeJumpOpcode(inst.opcode) || IsTerminator(inst.opcode)) {
            alias.clear();
        }
    }

    if (result.changed) {
        RebuildFunctionCode(function, code);
    }
    return result;
}

PassRunResult RunDcePass(Function* function) {
    PassRunResult result;
    auto code = function->FlattenedInstructions();
    if (code.empty() || ContainsTryInstructions(code)) {
        return result;
    }

    std::vector<bool> reachable(code.size(), false);
    std::vector<size_t> worklist;
    reachable[0] = true;
    worklist.push_back(0);

    auto push_if_valid = [&](int64_t target) {
        if (target < 0 || target >= static_cast<int64_t>(code.size())) {
            return;
        }
        const size_t idx = static_cast<size_t>(target);
        if (!reachable[idx]) {
            reachable[idx] = true;
            worklist.push_back(idx);
        }
    };

    while (!worklist.empty()) {
        const size_t pc = worklist.back();
        worklist.pop_back();
        const auto& inst = code[pc];

        switch (inst.opcode) {
            case OpCode::kJmp:
                push_if_valid(static_cast<int64_t>(pc) + static_cast<int32_t>(inst.a));
                break;
            case OpCode::kJmpIf:
            case OpCode::kJmpIfZero:
            case OpCode::kJmpIfNotZero:
            case OpCode::kDecJnz:
                push_if_valid(static_cast<int64_t>(pc) + static_cast<int32_t>(inst.b));
                push_if_valid(static_cast<int64_t>(pc) + 1);
                break;
            case OpCode::kAddDecJnz:
            case OpCode::kSubImmJnz:
            case OpCode::kAddImmJnz:
                push_if_valid(static_cast<int64_t>(pc) + static_cast<int32_t>(inst.c));
                push_if_valid(static_cast<int64_t>(pc) + 1);
                break;
            default:
                if (!IsTerminator(inst.opcode)) {
                    push_if_valid(static_cast<int64_t>(pc) + 1);
                }
                break;
        }
    }

    for (size_t i = 0; i < code.size(); ++i) {
        if (!reachable[i] && code[i].opcode != OpCode::kNop) {
            code[i] = MakeInstruction(OpCode::kNop);
            result.changed = true;
            ++result.rewritten;
        }
    }

    if (result.changed) {
        RebuildFunctionCode(function, code);
    }
    return result;
}

std::optional<size_t> FindMappedTarget(
    const std::vector<int64_t>& old_to_new, size_t old_target) {
    if (old_target >= old_to_new.size()) {
        return std::nullopt;
    }
    if (old_to_new[old_target] >= 0) {
        return static_cast<size_t>(old_to_new[old_target]);
    }
    for (size_t i = old_target + 1; i < old_to_new.size(); ++i) {
        if (old_to_new[i] >= 0) {
            return static_cast<size_t>(old_to_new[i]);
        }
    }
    if (old_target > 0) {
        for (size_t i = old_target; i-- > 0;) {
            if (old_to_new[i] >= 0) {
                return static_cast<size_t>(old_to_new[i]);
            }
        }
    }
    return std::nullopt;
}

PassRunResult RunCleanupPass(Function* function) {
    PassRunResult result;
    const auto old_code = function->FlattenedInstructions();
    if (old_code.empty() || ContainsTryInstructions(old_code)) {
        return result;
    }

    size_t nop_count = 0;
    for (const auto& inst : old_code) {
        if (inst.opcode == OpCode::kNop) {
            ++nop_count;
        }
    }
    if (nop_count == 0) {
        return result;
    }

    std::vector<Instruction> new_code;
    new_code.reserve(old_code.size() - nop_count);
    std::vector<int64_t> old_to_new(old_code.size(), -1);
    std::vector<size_t> new_to_old;
    new_to_old.reserve(old_code.size() - nop_count);

    for (size_t i = 0; i < old_code.size(); ++i) {
        if (old_code[i].opcode == OpCode::kNop) {
            continue;
        }
        old_to_new[i] = static_cast<int64_t>(new_code.size());
        new_code.push_back(old_code[i]);
        new_to_old.push_back(i);
    }

    if (new_code.empty()) {
        return result;
    }

    size_t rewritten_jumps = 0;
    for (size_t i = 0; i < new_code.size(); ++i) {
        auto& inst = new_code[i];
        if (!IsRelativeJumpOpcode(inst.opcode)) {
            continue;
        }
        const size_t old_pc = new_to_old[i];
        const int64_t old_target =
            static_cast<int64_t>(old_pc) + static_cast<int64_t>(RelativeJumpDelta(inst));
        if (old_target < 0 || old_target >= static_cast<int64_t>(old_code.size())) {
            return result;
        }
        auto target_new_or = FindMappedTarget(old_to_new, static_cast<size_t>(old_target));
        if (!target_new_or.has_value()) {
            return result;
        }
        const int64_t new_delta =
            static_cast<int64_t>(*target_new_or) - static_cast<int64_t>(i);
        if (new_delta < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
            new_delta > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
            return result;
        }
        if (new_delta != RelativeJumpDelta(inst)) {
            SetRelativeJumpDelta(&inst, static_cast<int32_t>(new_delta));
            ++rewritten_jumps;
        }
    }

    result.changed = true;
    result.removed = old_code.size() - new_code.size();
    result.rewritten = rewritten_jumps;
    RebuildFunctionCode(function, new_code);
    return result;
}

PassRunResult RunFfiForwarderBypassPass(Module* module) {
    PassRunResult result;
    if (module->functions().empty()) {
        return result;
    }

    std::vector<std::optional<FfiForwarderInfo>> forwarders(module->functions().size());
    for (size_t i = 0; i < module->functions().size(); ++i) {
        forwarders[i] = AnalyzeFfiForwarderFunction(*module, i);
    }

    for (size_t caller_index = 0; caller_index < module->functions().size(); ++caller_index) {
        auto& caller = module->functions()[caller_index];
        auto code = caller.FlattenedInstructions();
        if (code.empty()) {
            continue;
        }

        bool changed = false;
        for (auto& inst : code) {
            if (inst.opcode != OpCode::kCall || inst.b >= forwarders.size()) {
                continue;
            }
            const auto& forwarder = forwarders[inst.b];
            if (!forwarder.has_value() || inst.c != forwarder->arity) {
                continue;
            }
            inst = MakeInstruction(OpCode::kFfiCall, inst.a, forwarder->symbol_const_index, inst.c);
            changed = true;
            ++result.rewritten;
        }

        if (!changed) {
            continue;
        }
        RebuildFunctionCode(&caller, code);
        result.changed = true;
    }

    return result;
}

PassRunResult RunInlineSmallPass(Module* module, uint32_t inline_max_inst) {
    PassRunResult result;
    if (module->functions().empty()) {
        return result;
    }

    std::vector<bool> can_inline(module->functions().size(), false);
    for (size_t i = 0; i < module->functions().size(); ++i) {
        can_inline[i] = IsInlineCandidate(*module, i, inline_max_inst);
    }

    for (size_t caller_index = 0; caller_index < module->functions().size(); ++caller_index) {
        auto& caller = module->functions()[caller_index];
        auto old_code = caller.FlattenedInstructions();
        if (old_code.empty() || ContainsTryInstructions(old_code)) {
            continue;
        }

        uint32_t max_callee_regs = 0;
        for (const auto& inst : old_code) {
            if (inst.opcode != OpCode::kCall || inst.b >= module->functions().size() ||
                !can_inline[inst.b]) {
                continue;
            }
            const auto& callee = module->functions()[inst.b];
            if (callee.param_count() != inst.c) {
                continue;
            }
            max_callee_regs = std::max(max_callee_regs, static_cast<uint32_t>(callee.reg_count()));
        }
        if (max_callee_regs == 0) {
            continue;
        }

        const uint32_t scratch_base = caller.reg_count();
        const uint32_t required_regs = scratch_base + max_callee_regs;
        if (required_regs > std::numeric_limits<uint16_t>::max()) {
            continue;
        }

        bool caller_changed = false;
        std::vector<Instruction> new_code;
        new_code.reserve(old_code.size() + 32);
        std::vector<size_t> old_to_new_start(old_code.size(), 0);

        for (size_t old_pc = 0; old_pc < old_code.size(); ++old_pc) {
            const auto& inst = old_code[old_pc];
            old_to_new_start[old_pc] = new_code.size();

            if (inst.opcode != OpCode::kCall || inst.b >= module->functions().size() ||
                !can_inline[inst.b]) {
                new_code.push_back(inst);
                continue;
            }

            const auto& callee = module->functions()[inst.b];
            if (callee.param_count() != inst.c) {
                new_code.push_back(inst);
                continue;
            }
            if (scratch_base + static_cast<uint32_t>(callee.reg_count()) >
                std::numeric_limits<uint16_t>::max()) {
                new_code.push_back(inst);
                continue;
            }

            const auto callee_code = callee.FlattenedInstructions();
            if (callee_code.empty() || callee_code.back().opcode != OpCode::kRet) {
                new_code.push_back(inst);
                continue;
            }

            bool inline_ok = true;
            for (uint32_t arg_i = 0; arg_i < inst.c; ++arg_i) {
                new_code.push_back(
                    MakeInstruction(OpCode::kMov, scratch_base + arg_i, inst.a + 1 + arg_i, 0));
            }
            for (size_t i = 0; i + 1 < callee_code.size(); ++i) {
                Instruction cloned = callee_code[i];
                if (!RemapInlineInstructionRegs(&cloned, scratch_base)) {
                    inline_ok = false;
                    break;
                }
                new_code.push_back(cloned);
            }

            if (!inline_ok) {
                new_code.resize(old_to_new_start[old_pc]);
                new_code.push_back(inst);
                continue;
            }

            const uint32_t mapped_ret = scratch_base + callee_code.back().a;
            new_code.push_back(MakeInstruction(OpCode::kMov, inst.a, mapped_ret, 0));
            caller_changed = true;
            ++result.inlined;
        }

        if (!caller_changed) {
            continue;
        }

        for (size_t old_pc = 0; old_pc < old_code.size(); ++old_pc) {
            const auto& old_inst = old_code[old_pc];
            if (!IsRelativeJumpOpcode(old_inst.opcode)) {
                continue;
            }
            const size_t new_pc = old_to_new_start[old_pc];
            if (new_pc >= new_code.size()) {
                continue;
            }
            const int64_t old_target =
                static_cast<int64_t>(old_pc) + static_cast<int64_t>(RelativeJumpDelta(old_inst));
            if (old_target < 0 || old_target >= static_cast<int64_t>(old_code.size())) {
                continue;
            }
            const size_t new_target = old_to_new_start[static_cast<size_t>(old_target)];
            const int64_t new_delta =
                static_cast<int64_t>(new_target) - static_cast<int64_t>(new_pc);
            if (new_delta < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
                new_delta > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
                continue;
            }
            auto& new_inst = new_code[new_pc];
            const int32_t before = RelativeJumpDelta(new_inst);
            const int32_t after = static_cast<int32_t>(new_delta);
            if (before != after) {
                SetRelativeJumpDelta(&new_inst, after);
                ++result.rewritten;
            }
        }

        RebuildFunctionCode(&caller, new_code);
        if (required_regs > caller.reg_count()) {
            caller.set_reg_count(static_cast<uint16_t>(required_regs));
        }
        result.changed = true;
    }

    return result;
}

}  // namespace

std::string_view BytecodeOptLevelName(BytecodeOptLevel level) {
    switch (level) {
        case BytecodeOptLevel::kO0:
            return "O0";
        case BytecodeOptLevel::kO1:
            return "O1";
        case BytecodeOptLevel::kO2:
            return "O2";
        case BytecodeOptLevel::kO3:
            return "O3";
    }
    return "unknown";
}

std::string_view BytecodeOptPassName(BytecodeOptPass pass) {
    switch (pass) {
        case BytecodeOptPass::kPeephole:
            return "peephole";
        case BytecodeOptPass::kLoopFusion:
            return "loop_fusion";
        case BytecodeOptPass::kConstFold:
            return "const_fold";
        case BytecodeOptPass::kCopyProp:
            return "copy_prop";
        case BytecodeOptPass::kDce:
            return "dce";
        case BytecodeOptPass::kTailcall:
            return "tailcall";
        case BytecodeOptPass::kInlineSmall:
            return "inline_small";
        case BytecodeOptPass::kCleanup:
            return "cleanup";
    }
    return "unknown";
}

StatusOr<BytecodeOptLevel> ParseBytecodeOptLevel(std::string_view text) {
    if (text == "O0" || text == "o0") {
        return BytecodeOptLevel::kO0;
    }
    if (text == "O1" || text == "o1") {
        return BytecodeOptLevel::kO1;
    }
    if (text == "O2" || text == "o2") {
        return BytecodeOptLevel::kO2;
    }
    if (text == "O3" || text == "o3") {
        return BytecodeOptLevel::kO3;
    }
    return Status::InvalidArgument("opt level must be one of O0/O1/O2/O3");
}

StatusOr<BytecodeOptPass> ParseBytecodeOptPass(std::string_view text) {
    std::string normalized(text);
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
            if (ch == '-') {
                return static_cast<char>('_');
            }
            return static_cast<char>(std::tolower(ch));
        });

    if (normalized == "peephole") {
        return BytecodeOptPass::kPeephole;
    }
    if (normalized == "loop_fusion") {
        return BytecodeOptPass::kLoopFusion;
    }
    if (normalized == "const_fold") {
        return BytecodeOptPass::kConstFold;
    }
    if (normalized == "copy_prop") {
        return BytecodeOptPass::kCopyProp;
    }
    if (normalized == "dce") {
        return BytecodeOptPass::kDce;
    }
    if (normalized == "tailcall") {
        return BytecodeOptPass::kTailcall;
    }
    if (normalized == "inline_small") {
        return BytecodeOptPass::kInlineSmall;
    }
    if (normalized == "cleanup") {
        return BytecodeOptPass::kCleanup;
    }

    return Status::InvalidArgument("unknown optimization pass: " + std::string(text));
}

StatusOr<BytecodeOptStats> OptimizeBytecodeModuleInPlace(
    Module* module, const BytecodeOptOptions& options) {
    if (module == nullptr) {
        return Status::InvalidArgument("optimize module pointer cannot be null");
    }
    if (options.inline_max_inst == 0) {
        return Status::InvalidArgument("inline_max_inst must be >= 1");
    }

    if (options.verify_input) {
        const auto verify_input = Verifier::Verify(*module);
        if (!verify_input.status.ok()) {
            return verify_input.status;
        }
    }

    BytecodeOptStats stats;
    stats.module_function_count = module->functions().size();
    stats.executed_passes = BuildPassPipeline(options);

    std::vector<bool> function_changed(module->functions().size(), false);

    std::unordered_map<int64_t, uint32_t> int64_cache;
    for (uint32_t i = 0; i < module->constants().size(); ++i) {
        const auto& c = module->constants()[i];
        if (c.type == ConstantType::kInt64) {
            int64_cache.insert_or_assign(c.int64_value, i);
        }
    }

    for (const auto pass : stats.executed_passes) {
        if (pass == BytecodeOptPass::kInlineSmall) {
            PassRunResult pass_result = RunFfiForwarderBypassPass(module);
            MergePassResult(&pass_result, RunInlineSmallPass(module, options.inline_max_inst));
            if (pass_result.changed) {
                std::fill(function_changed.begin(), function_changed.end(), true);
            }
            stats.rewritten_instruction_count += pass_result.rewritten;
            stats.removed_instruction_count += pass_result.removed;
            stats.inlined_callsite_count += pass_result.inlined;
            continue;
        }

        for (size_t fn_index = 0; fn_index < module->functions().size(); ++fn_index) {
            auto& function = module->functions()[fn_index];
            PassRunResult pass_result;
            switch (pass) {
                case BytecodeOptPass::kPeephole:
                    pass_result = RunPeepholePass(&function);
                    break;
                case BytecodeOptPass::kLoopFusion:
                    pass_result = RunLoopFusionPass(&function);
                    if (options.level == BytecodeOptLevel::kO3) {
                        MergePassResult(&pass_result, RunLicmHeaderLoadPass(&function));
                        MergePassResult(
                            &pass_result,
                            RunStrengthReductionPass(&function, module, &int64_cache));
                    }
                    break;
                case BytecodeOptPass::kConstFold:
                    pass_result = RunConstFoldPass(
                        &function,
                        module,
                        &int64_cache,
                        options.level == BytecodeOptLevel::kO3);
                    break;
                case BytecodeOptPass::kCopyProp:
                    pass_result = RunCopyPropPass(&function);
                    break;
                case BytecodeOptPass::kDce:
                    pass_result = RunDcePass(&function);
                    break;
                case BytecodeOptPass::kTailcall:
                    pass_result = RunTailcallPass(&function);
                    break;
                case BytecodeOptPass::kCleanup:
                    pass_result = RunCleanupPass(&function);
                    break;
                case BytecodeOptPass::kInlineSmall:
                    break;
            }

            if (pass_result.changed) {
                function_changed[fn_index] = true;
            }
            stats.rewritten_instruction_count += pass_result.rewritten;
            stats.removed_instruction_count += pass_result.removed;
            stats.inlined_callsite_count += pass_result.inlined;
        }
    }

    stats.optimized_function_count =
        static_cast<size_t>(std::count(function_changed.begin(), function_changed.end(), true));

    module->debug_lines().clear();

    const auto verify_output = Verifier::Verify(*module);
    if (!verify_output.status.ok()) {
        return verify_output.status;
    }

    return stats;
}

StatusOr<BytecodeOptResult> OptimizeBytecodeModule(
    const Module& module, const BytecodeOptOptions& options) {
    BytecodeOptResult result;
    result.module = module;

    auto stats_or = OptimizeBytecodeModuleInPlace(&result.module, options);
    if (!stats_or.ok()) {
        return stats_or.status();
    }
    result.stats = std::move(stats_or.value());
    return result;
}

}  // namespace tie::vm
