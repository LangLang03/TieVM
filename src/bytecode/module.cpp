#include "tie/vm/bytecode/module.hpp"

namespace tie::vm {

BasicBlock& Function::AddBlock(std::string name) {
    blocks_.emplace_back(std::move(name));
    return blocks_.back();
}

std::vector<Instruction> Function::FlattenedInstructions() const {
    size_t total = 0;
    for (const auto& block : blocks_) {
        total += block.instructions().size();
    }
    std::vector<Instruction> flat;
    flat.reserve(total);
    for (const auto& block : blocks_) {
        for (const auto& inst : block.instructions()) {
            flat.push_back(inst);
        }
    }
    return flat;
}

Function& Module::AddFunction(
    std::string name, uint16_t reg_count, uint16_t param_count,
    uint16_t upvalue_count, bool is_vararg) {
    functions_.emplace_back(
        std::move(name), reg_count, param_count, upvalue_count, is_vararg);
    return functions_.back();
}

uint32_t Module::AddConstant(Constant constant) {
    constants_.push_back(std::move(constant));
    return static_cast<uint32_t>(constants_.size() - 1);
}

void Module::AddDebugLine(DebugLineEntry entry) { debug_lines_.push_back(entry); }

uint32_t Module::AddFfiLibraryPath(std::string path) {
    ffi_library_paths_.push_back(std::move(path));
    return static_cast<uint32_t>(ffi_library_paths_.size() - 1);
}

uint32_t Module::AddFfiStruct(FfiStructLayout layout) {
    ffi_structs_.push_back(std::move(layout));
    return static_cast<uint32_t>(ffi_structs_.size() - 1);
}

uint32_t Module::AddFfiSignature(FunctionSignature signature) {
    ffi_signatures_.push_back(std::move(signature));
    return static_cast<uint32_t>(ffi_signatures_.size() - 1);
}

uint32_t Module::AddFfiBinding(FfiSymbolBinding binding) {
    ffi_bindings_.push_back(std::move(binding));
    return static_cast<uint32_t>(ffi_bindings_.size() - 1);
}

uint32_t Module::AddClass(BytecodeClassDecl class_decl) {
    classes_.push_back(std::move(class_decl));
    return static_cast<uint32_t>(classes_.size() - 1);
}

}  // namespace tie::vm
