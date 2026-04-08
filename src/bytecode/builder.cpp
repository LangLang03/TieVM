#include "tie/vm/bytecode/builder.hpp"

namespace tie::vm {

InstructionBuilder& InstructionBuilder::Nop() {
    block_->Append(MakeInstruction(OpCode::kNop));
    return *this;
}

InstructionBuilder& InstructionBuilder::Mov(uint32_t dst, uint32_t src) {
    block_->Append(MakeInstruction(OpCode::kMov, dst, src, 0));
    return *this;
}

InstructionBuilder& InstructionBuilder::LoadK(uint32_t dst, uint32_t constant_idx) {
    block_->Append(MakeInstruction(OpCode::kLoadK, dst, constant_idx, 0));
    return *this;
}

InstructionBuilder& InstructionBuilder::Add(uint32_t dst, uint32_t lhs, uint32_t rhs) {
    block_->Append(MakeInstruction(OpCode::kAdd, dst, lhs, rhs));
    return *this;
}

InstructionBuilder& InstructionBuilder::Sub(uint32_t dst, uint32_t lhs, uint32_t rhs) {
    block_->Append(MakeInstruction(OpCode::kSub, dst, lhs, rhs));
    return *this;
}

InstructionBuilder& InstructionBuilder::Mul(uint32_t dst, uint32_t lhs, uint32_t rhs) {
    block_->Append(MakeInstruction(OpCode::kMul, dst, lhs, rhs));
    return *this;
}

InstructionBuilder& InstructionBuilder::Div(uint32_t dst, uint32_t lhs, uint32_t rhs) {
    block_->Append(MakeInstruction(OpCode::kDiv, dst, lhs, rhs));
    return *this;
}

InstructionBuilder& InstructionBuilder::CmpEq(uint32_t dst, uint32_t lhs, uint32_t rhs) {
    block_->Append(MakeInstruction(OpCode::kCmpEq, dst, lhs, rhs));
    return *this;
}

InstructionBuilder& InstructionBuilder::Jmp(int32_t offset) {
    block_->Append(MakeInstruction(OpCode::kJmp, static_cast<uint32_t>(offset), 0, 0));
    return *this;
}

InstructionBuilder& InstructionBuilder::JmpIf(uint32_t cond, int32_t offset) {
    block_->Append(
        MakeInstruction(OpCode::kJmpIf, cond, static_cast<uint32_t>(offset), 0));
    return *this;
}

InstructionBuilder& InstructionBuilder::Call(
    uint32_t ret_reg, uint32_t function_index, uint32_t arg_count) {
    block_->Append(MakeInstruction(OpCode::kCall, ret_reg, function_index, arg_count));
    return *this;
}

InstructionBuilder& InstructionBuilder::FfiCall(
    uint32_t ret_reg, uint32_t symbol_index, uint32_t arg_count) {
    block_->Append(
        MakeInstruction(OpCode::kFfiCall, ret_reg, symbol_index, arg_count));
    return *this;
}

InstructionBuilder& InstructionBuilder::NewObject(
    uint32_t dst, uint32_t class_name_constant) {
    block_->Append(MakeInstruction(OpCode::kNewObject, dst, class_name_constant, 0));
    return *this;
}

InstructionBuilder& InstructionBuilder::Invoke(
    uint32_t object_reg, uint32_t method_constant, uint32_t arg_count) {
    block_->Append(
        MakeInstruction(OpCode::kInvoke, object_reg, method_constant, arg_count));
    return *this;
}

InstructionBuilder& InstructionBuilder::Throw(uint32_t message_reg) {
    block_->Append(MakeInstruction(OpCode::kThrow, message_reg, 0, 0));
    return *this;
}

InstructionBuilder& InstructionBuilder::Ret(uint32_t src_reg) {
    block_->Append(MakeInstruction(OpCode::kRet, src_reg, 0, 0));
    return *this;
}

InstructionBuilder& InstructionBuilder::Halt() {
    block_->Append(MakeInstruction(OpCode::kHalt, 0, 0, 0));
    return *this;
}

void DebugSectionBuilder::AddLine(
    uint32_t function_idx, uint32_t instruction_idx, uint32_t line, uint32_t col) {
    module_->AddDebugLine(DebugLineEntry{function_idx, instruction_idx, line, col});
}

}  // namespace tie::vm

