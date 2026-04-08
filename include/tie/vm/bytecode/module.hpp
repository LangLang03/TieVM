#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "tie/vm/bytecode/instruction.hpp"
#include "tie/vm/common/version.hpp"

namespace tie::vm {

enum class ConstantType : uint8_t {
    kInt64 = 0,
    kFloat64 = 1,
    kUtf8 = 2,
};

struct Constant {
    ConstantType type = ConstantType::kInt64;
    int64_t int64_value = 0;
    double float64_value = 0.0;
    std::string utf8_value;

    static Constant Int64(int64_t v) {
        Constant c;
        c.type = ConstantType::kInt64;
        c.int64_value = v;
        return c;
    }

    static Constant Float64(double v) {
        Constant c;
        c.type = ConstantType::kFloat64;
        c.float64_value = v;
        return c;
    }

    static Constant Utf8(std::string v) {
        Constant c;
        c.type = ConstantType::kUtf8;
        c.utf8_value = std::move(v);
        return c;
    }
};

struct DebugLineEntry {
    uint32_t function_index = 0;
    uint32_t instruction_index = 0;
    uint32_t line = 0;
    uint32_t column = 0;
};

class BasicBlock {
  public:
    explicit BasicBlock(std::string name) : name_(std::move(name)) {}

    void Append(Instruction inst) { instructions_.push_back(inst); }

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const std::vector<Instruction>& instructions() const {
        return instructions_;
    }

  private:
    std::string name_;
    std::vector<Instruction> instructions_;
};

class Function {
  public:
    Function(std::string name, uint16_t reg_count, uint16_t param_count)
        : name_(std::move(name)), reg_count_(reg_count), param_count_(param_count) {}

    BasicBlock& AddBlock(std::string name);
    [[nodiscard]] std::vector<Instruction> FlattenedInstructions() const;

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] uint16_t reg_count() const { return reg_count_; }
    [[nodiscard]] uint16_t param_count() const { return param_count_; }
    [[nodiscard]] const std::vector<BasicBlock>& blocks() const { return blocks_; }
    [[nodiscard]] std::vector<BasicBlock>& blocks() { return blocks_; }

  private:
    std::string name_;
    uint16_t reg_count_ = 0;
    uint16_t param_count_ = 0;
    std::vector<BasicBlock> blocks_;
};

class Module {
  public:
    explicit Module(std::string name) : name_(std::move(name)) {}

    Function& AddFunction(std::string name, uint16_t reg_count, uint16_t param_count);
    uint32_t AddConstant(Constant constant);
    void AddDebugLine(DebugLineEntry entry);

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const SemanticVersion& version() const { return version_; }
    [[nodiscard]] SemanticVersion& version() { return version_; }
    [[nodiscard]] const std::vector<Function>& functions() const { return functions_; }
    [[nodiscard]] std::vector<Function>& functions() { return functions_; }
    [[nodiscard]] const std::vector<Constant>& constants() const { return constants_; }
    [[nodiscard]] const std::vector<DebugLineEntry>& debug_lines() const {
        return debug_lines_;
    }
    [[nodiscard]] uint32_t entry_function() const { return entry_function_; }

    void set_entry_function(uint32_t idx) { entry_function_ = idx; }

  private:
    std::string name_;
    SemanticVersion version_{0, 1, 0};
    std::vector<Function> functions_;
    std::vector<Constant> constants_;
    std::vector<DebugLineEntry> debug_lines_;
    uint32_t entry_function_ = 0;
};

}  // namespace tie::vm

