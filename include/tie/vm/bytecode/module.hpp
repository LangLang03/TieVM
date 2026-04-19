#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tie/vm/bytecode/instruction.hpp"
#include "tie/vm/common/version.hpp"
#include "tie/vm/ffi/ffi_types.hpp"

namespace tie::vm {

enum class ConstantType : uint8_t {
    kInt64 = 0,
    kFloat64 = 1,
    kUtf8 = 2,
};

enum class BytecodeValueType : uint8_t {
    kAny = 0,
    kNull = 1,
    kInt64 = 2,
    kFloat64 = 3,
    kBool = 4,
    kObject = 5,
    kPointer = 6,
    kString = 7,
    kClosure = 8,
};

[[nodiscard]] constexpr bool IsValidBytecodeValueType(BytecodeValueType type) {
    return static_cast<uint8_t>(type) <= static_cast<uint8_t>(BytecodeValueType::kClosure);
}

[[nodiscard]] constexpr std::string_view BytecodeValueTypeName(BytecodeValueType type) {
    switch (type) {
        case BytecodeValueType::kAny:
            return "any";
        case BytecodeValueType::kNull:
            return "null";
        case BytecodeValueType::kInt64:
            return "int64";
        case BytecodeValueType::kFloat64:
            return "float64";
        case BytecodeValueType::kBool:
            return "bool";
        case BytecodeValueType::kObject:
            return "object";
        case BytecodeValueType::kPointer:
            return "pointer";
        case BytecodeValueType::kString:
            return "string";
        case BytecodeValueType::kClosure:
            return "closure";
    }
    return "unknown";
}

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

enum class BytecodeAccessModifier : uint8_t {
    kPublic = 0,
    kProtected = 1,
    kPrivate = 2,
};

struct BytecodeMethodDecl {
    std::string name;
    uint32_t function_index = 0;
    BytecodeAccessModifier access = BytecodeAccessModifier::kPublic;
    bool is_virtual = true;
};

struct BytecodeClassDecl {
    std::string name;
    std::vector<std::string> base_classes;
    std::vector<BytecodeMethodDecl> methods;
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
    Function(
        std::string name, uint16_t reg_count, uint16_t param_count,
        uint16_t upvalue_count = 0, bool is_vararg = false, bool is_exported = false,
        std::vector<BytecodeValueType> param_types = {},
        BytecodeValueType return_type = BytecodeValueType::kAny)
        : name_(std::move(name)),
          reg_count_(reg_count),
          param_count_(param_count),
          upvalue_count_(upvalue_count),
          is_vararg_(is_vararg),
          is_exported_(is_exported),
          param_types_(std::move(param_types)),
          return_type_(return_type) {
        if (param_types_.empty()) {
            param_types_.assign(param_count_, BytecodeValueType::kAny);
        }
    }

    BasicBlock& AddBlock(std::string name);
    [[nodiscard]] std::vector<Instruction> FlattenedInstructions() const;

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] uint16_t reg_count() const { return reg_count_; }
    [[nodiscard]] uint16_t param_count() const { return param_count_; }
    [[nodiscard]] uint16_t upvalue_count() const { return upvalue_count_; }
    [[nodiscard]] bool is_vararg() const { return is_vararg_; }
    [[nodiscard]] bool is_exported() const { return is_exported_; }
    [[nodiscard]] BytecodeValueType return_type() const { return return_type_; }
    [[nodiscard]] const std::vector<BytecodeValueType>& param_types() const {
        return param_types_;
    }
    [[nodiscard]] std::vector<BytecodeValueType>& param_types() { return param_types_; }
    void set_upvalue_count(uint16_t upvalue_count) { upvalue_count_ = upvalue_count; }
    void set_is_vararg(bool is_vararg) { is_vararg_ = is_vararg; }
    void set_is_exported(bool is_exported) { is_exported_ = is_exported; }
    void set_return_type(BytecodeValueType return_type) { return_type_ = return_type; }
    void set_param_types(std::vector<BytecodeValueType> param_types) {
        param_types_ = std::move(param_types);
        if (param_types_.empty()) {
            param_types_.assign(param_count_, BytecodeValueType::kAny);
        }
    }
    [[nodiscard]] const std::vector<BasicBlock>& blocks() const { return blocks_; }
    [[nodiscard]] std::vector<BasicBlock>& blocks() { return blocks_; }
    [[nodiscard]] const FunctionFfiBindingHeader& ffi_binding() const {
        return ffi_binding_;
    }
    [[nodiscard]] FunctionFfiBindingHeader& ffi_binding() { return ffi_binding_; }

  private:
    std::string name_;
    uint16_t reg_count_ = 0;
    uint16_t param_count_ = 0;
    uint16_t upvalue_count_ = 0;
    bool is_vararg_ = false;
    bool is_exported_ = false;
    std::vector<BytecodeValueType> param_types_;
    BytecodeValueType return_type_ = BytecodeValueType::kAny;
    std::vector<BasicBlock> blocks_;
    FunctionFfiBindingHeader ffi_binding_;
};

class Module {
  public:
    explicit Module(std::string name) : name_(std::move(name)) {}

    Function& AddFunction(
        std::string name, uint16_t reg_count, uint16_t param_count,
        uint16_t upvalue_count = 0, bool is_vararg = false, bool is_exported = false,
        std::vector<BytecodeValueType> param_types = {},
        BytecodeValueType return_type = BytecodeValueType::kAny);
    uint32_t AddConstant(Constant constant);
    void AddDebugLine(DebugLineEntry entry);
    uint32_t AddFfiLibraryPath(std::string path);
    uint32_t AddFfiStruct(FfiStructLayout layout);
    uint32_t AddFfiSignature(FunctionSignature signature);
    uint32_t AddFfiBinding(FfiSymbolBinding binding);
    uint32_t AddClass(BytecodeClassDecl class_decl);

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const SemanticVersion& version() const { return version_; }
    [[nodiscard]] SemanticVersion& version() { return version_; }
    [[nodiscard]] const std::vector<Function>& functions() const { return functions_; }
    [[nodiscard]] std::vector<Function>& functions() { return functions_; }
    [[nodiscard]] const std::vector<Constant>& constants() const { return constants_; }
    [[nodiscard]] const std::vector<DebugLineEntry>& debug_lines() const {
        return debug_lines_;
    }
    [[nodiscard]] std::vector<DebugLineEntry>& debug_lines() { return debug_lines_; }
    [[nodiscard]] const std::vector<std::string>& ffi_library_paths() const {
        return ffi_library_paths_;
    }
    [[nodiscard]] std::vector<std::string>& ffi_library_paths() {
        return ffi_library_paths_;
    }
    [[nodiscard]] const std::vector<FfiStructLayout>& ffi_structs() const {
        return ffi_structs_;
    }
    [[nodiscard]] std::vector<FfiStructLayout>& ffi_structs() { return ffi_structs_; }
    [[nodiscard]] const std::vector<FunctionSignature>& ffi_signatures() const {
        return ffi_signatures_;
    }
    [[nodiscard]] std::vector<FunctionSignature>& ffi_signatures() {
        return ffi_signatures_;
    }
    [[nodiscard]] const std::vector<FfiSymbolBinding>& ffi_bindings() const {
        return ffi_bindings_;
    }
    [[nodiscard]] std::vector<FfiSymbolBinding>& ffi_bindings() { return ffi_bindings_; }
    [[nodiscard]] const std::vector<BytecodeClassDecl>& classes() const { return classes_; }
    [[nodiscard]] std::vector<BytecodeClassDecl>& classes() { return classes_; }
    [[nodiscard]] uint32_t entry_function() const { return entry_function_; }

    void set_entry_function(uint32_t idx) { entry_function_ = idx; }

  private:
    std::string name_;
    SemanticVersion version_{0, 1, 0};
    std::vector<Function> functions_;
    std::vector<Constant> constants_;
    std::vector<DebugLineEntry> debug_lines_;
    std::vector<std::string> ffi_library_paths_;
    std::vector<FfiStructLayout> ffi_structs_;
    std::vector<FunctionSignature> ffi_signatures_;
    std::vector<FfiSymbolBinding> ffi_bindings_;
    std::vector<BytecodeClassDecl> classes_;
    uint32_t entry_function_ = 0;
};

}  // namespace tie::vm
