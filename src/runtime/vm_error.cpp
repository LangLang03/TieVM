#include "tie/vm/common/vm_error.hpp"

#include <sstream>

namespace tie::vm {

namespace {

std::string FormatVmFrame(const StackFrame& frame) {
    std::ostringstream out;
    out << "VM " << frame.module_name << "::" << frame.function_name << " pc="
        << frame.program_counter;
    if (frame.line > 0) {
        out << " @" << frame.line << ":" << frame.column;
    }
    return out.str();
}

std::string FormatFfiFrame(const StackFrame& frame) {
    std::ostringstream out;
    out << "FFI " << frame.symbol_name;
    if (!frame.library_path.empty()) {
        out << " [" << frame.library_path << "]";
    }
    return out.str();
}

}  // namespace

void VmError::PushFrame(StackFrame frame) { frames.push_back(std::move(frame)); }

std::string VmError::Format() const {
    std::ostringstream out;
    out << message;
    for (const auto& frame : frames) {
        out << "\n  at ";
        if (frame.kind == StackFrameKind::kVm) {
            out << FormatVmFrame(frame);
        } else {
            out << FormatFfiFrame(frame);
        }
    }
    return out.str();
}

}  // namespace tie::vm
