#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tie::vm {

enum class StackFrameKind : uint8_t {
    kVm = 0,
    kFfi = 1,
};

struct StackFrame {
    StackFrameKind kind = StackFrameKind::kVm;
    std::string module_name;
    std::string function_name;
    uint32_t program_counter = 0;
    uint32_t line = 0;
    uint32_t column = 0;
    std::string library_path;
    std::string symbol_name;
};

struct VmError {
    std::string message;
    std::vector<StackFrame> frames;

    [[nodiscard]] std::string Format() const;
    void PushFrame(StackFrame frame);
};

}  // namespace tie::vm
