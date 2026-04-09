#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "tie/vm/bytecode/module.hpp"

namespace tie::vm {

class InstructionBuilder {
  public:
    explicit InstructionBuilder(BasicBlock& block) : block_(&block) {}

    InstructionBuilder& Nop();
    InstructionBuilder& Mov(uint32_t dst, uint32_t src);
    InstructionBuilder& LoadK(uint32_t dst, uint32_t constant_idx);
    InstructionBuilder& Add(uint32_t dst, uint32_t lhs, uint32_t rhs);
    InstructionBuilder& Sub(uint32_t dst, uint32_t lhs, uint32_t rhs);
    InstructionBuilder& Mul(uint32_t dst, uint32_t lhs, uint32_t rhs);
    InstructionBuilder& Div(uint32_t dst, uint32_t lhs, uint32_t rhs);
    InstructionBuilder& AddImm(uint32_t dst, uint32_t src, int32_t imm);
    InstructionBuilder& SubImm(uint32_t dst, uint32_t src, int32_t imm);
    InstructionBuilder& CmpEq(uint32_t dst, uint32_t lhs, uint32_t rhs);
    InstructionBuilder& Jmp(int32_t offset);
    InstructionBuilder& JmpIf(uint32_t cond, int32_t offset);
    InstructionBuilder& JmpIfZero(uint32_t reg, int32_t offset);
    InstructionBuilder& JmpIfNotZero(uint32_t reg, int32_t offset);
    InstructionBuilder& DecJnz(uint32_t reg, int32_t offset);
    InstructionBuilder& AddDecJnz(uint32_t acc_reg, uint32_t counter_reg, int32_t offset);
    InstructionBuilder& Inc(uint32_t reg);
    InstructionBuilder& Dec(uint32_t reg);
    InstructionBuilder& SubImmJnz(uint32_t reg, int32_t imm, int32_t offset);
    InstructionBuilder& AddImmJnz(uint32_t reg, int32_t imm, int32_t offset);
    InstructionBuilder& Closure(
        uint32_t dst, uint32_t function_index, uint32_t first_upvalue_reg,
        uint8_t upvalue_count);
    InstructionBuilder& GetUpval(uint32_t dst, uint32_t upvalue_index);
    InstructionBuilder& SetUpval(uint32_t src, uint32_t upvalue_index);
    InstructionBuilder& CallClosure(uint32_t ret_reg, uint32_t closure_reg, uint32_t arg_count);
    InstructionBuilder& TailCall(uint32_t ret_reg, uint32_t function_index, uint32_t arg_count);
    InstructionBuilder& TailCallClosure(
        uint32_t ret_reg, uint32_t closure_reg, uint32_t arg_count);
    InstructionBuilder& VarArg(uint32_t dst, uint32_t vararg_start, uint32_t count);
    InstructionBuilder& StrLen(uint32_t dst, uint32_t src);
    InstructionBuilder& StrConcat(uint32_t dst, uint32_t lhs, uint32_t rhs);
    InstructionBuilder& BitAnd(uint32_t dst, uint32_t lhs, uint32_t rhs);
    InstructionBuilder& BitOr(uint32_t dst, uint32_t lhs, uint32_t rhs);
    InstructionBuilder& BitXor(uint32_t dst, uint32_t lhs, uint32_t rhs);
    InstructionBuilder& BitNot(uint32_t dst, uint32_t src);
    InstructionBuilder& BitShl(uint32_t dst, uint32_t lhs, uint32_t rhs);
    InstructionBuilder& BitShr(uint32_t dst, uint32_t lhs, uint32_t rhs);
    InstructionBuilder& Call(uint32_t ret_reg, uint32_t function_index, uint32_t arg_count);
    InstructionBuilder& FfiCall(uint32_t ret_reg, uint32_t symbol_index, uint32_t arg_count);
    InstructionBuilder& NewObject(uint32_t dst, uint32_t class_name_constant);
    InstructionBuilder& Invoke(uint32_t object_reg, uint32_t method_constant, uint32_t arg_count);
    InstructionBuilder& Throw(uint32_t message_reg);
    InstructionBuilder& Ret(uint32_t src_reg);
    InstructionBuilder& Halt();

  private:
    BasicBlock* block_;
};

class DebugSectionBuilder {
  public:
    explicit DebugSectionBuilder(Module& module) : module_(&module) {}
    void AddLine(uint32_t function_idx, uint32_t instruction_idx, uint32_t line, uint32_t col);

  private:
    Module* module_;
};

class FfiMetadataBuilder {
  public:
    explicit FfiMetadataBuilder(Module& module) : module_(&module) {}

    uint32_t AddLibraryPath(std::string path);
    uint32_t AddSignature(FunctionSignature signature);
    uint32_t AddBinding(FfiSymbolBinding binding);
    void BindFunctionHeader(
        Function& function, CallingConvention convention, uint32_t signature_index,
        uint32_t binding_index);

  private:
    Module* module_;
};

}  // namespace tie::vm
