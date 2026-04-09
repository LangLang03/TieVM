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
