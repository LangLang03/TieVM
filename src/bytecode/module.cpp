#include "tie/vm/bytecode/module.hpp"

namespace tie::vm {

BasicBlock& Function::AddBlock(std::string name) {
    blocks_.emplace_back(std::move(name));
    return blocks_.back();
}

std::vector<Instruction> Function::FlattenedInstructions() const {
    std::vector<Instruction> flat;
    for (const auto& block : blocks_) {
        for (const auto& inst : block.instructions()) {
            flat.push_back(inst);
        }
    }
    return flat;
}

Function& Module::AddFunction(std::string name, uint16_t reg_count, uint16_t param_count) {
    functions_.emplace_back(std::move(name), reg_count, param_count);
    return functions_.back();
}

uint32_t Module::AddConstant(Constant constant) {
    constants_.push_back(std::move(constant));
    return static_cast<uint32_t>(constants_.size() - 1);
}

void Module::AddDebugLine(DebugLineEntry entry) { debug_lines_.push_back(entry); }

}  // namespace tie::vm

