#include "tiebc_commands.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

namespace {

std::string EscapeText(const std::string& text) {
    std::ostringstream out;
    for (unsigned char ch : text) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '\"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch >= 0x20 && ch <= 0x7E) {
                    out << static_cast<char>(ch);
                } else {
                    out << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec;
                }
                break;
        }
    }
    return out.str();
}

std::string ConstantSummary(const Module& module, uint32_t index) {
    if (index >= module.constants().size()) {
        return "<const out-of-range>";
    }
    const auto& c = module.constants()[index];
    std::ostringstream out;
    switch (c.type) {
        case ConstantType::kInt64:
            out << "i64(" << c.int64_value << ")";
            break;
        case ConstantType::kFloat64:
            out << "f64(" << c.float64_value << ")";
            break;
        case ConstantType::kUtf8:
            out << "utf8(\"" << EscapeText(c.utf8_value) << "\")";
            break;
    }
    return out.str();
}

std::string Utf8ConstantOrFallback(const Module& module, uint32_t index) {
    if (index >= module.constants().size()) {
        return "<const out-of-range>";
    }
    const auto& c = module.constants()[index];
    if (c.type != ConstantType::kUtf8) {
        return "<const not-utf8>";
    }
    return c.utf8_value;
}

std::string FunctionNameOrFallback(const Module& module, uint32_t index) {
    if (index >= module.functions().size()) {
        return "<func out-of-range>";
    }
    return module.functions()[index].name();
}

std::string Reg(uint32_t idx) { return "r" + std::to_string(idx); }

std::string FormatArgs(uint32_t base_register, uint32_t argc) {
    if (argc == 0) {
        return "()";
    }
    std::ostringstream out;
    out << "(";
    for (uint32_t i = 0; i < argc; ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << Reg(base_register + 1 + i);
    }
    out << ")";
    return out.str();
}

std::string FormatTarget(int64_t target, size_t code_size) {
    if (target < 0 || target >= static_cast<int64_t>(code_size)) {
        return "<out-of-range>";
    }
    return "#" + std::to_string(target);
}

std::string FormatInstruction(
    const Module& module, const Instruction& inst, size_t pc, size_t code_size) {
    std::ostringstream out;
    switch (inst.opcode) {
        case OpCode::kNop:
            out << "nop";
            break;
        case OpCode::kMov:
            out << Reg(inst.a) << " <- " << Reg(inst.b);
            break;
        case OpCode::kLoadK:
            out << Reg(inst.a) << " <- const[" << inst.b << "] "
                << ConstantSummary(module, inst.b);
            break;
        case OpCode::kAdd:
            out << Reg(inst.a) << " <- " << Reg(inst.b) << " + " << Reg(inst.c);
            break;
        case OpCode::kSub:
            out << Reg(inst.a) << " <- " << Reg(inst.b) << " - " << Reg(inst.c);
            break;
        case OpCode::kMul:
            out << Reg(inst.a) << " <- " << Reg(inst.b) << " * " << Reg(inst.c);
            break;
        case OpCode::kDiv:
            out << Reg(inst.a) << " <- " << Reg(inst.b) << " / " << Reg(inst.c);
            break;
        case OpCode::kAddImm:
            out << Reg(inst.a) << " <- " << Reg(inst.b) << " + "
                << static_cast<int32_t>(inst.c);
            break;
        case OpCode::kSubImm:
            out << Reg(inst.a) << " <- " << Reg(inst.b) << " - "
                << static_cast<int32_t>(inst.c);
            break;
        case OpCode::kInc:
            out << "++" << Reg(inst.a);
            break;
        case OpCode::kDec:
            out << "--" << Reg(inst.a);
            break;
        case OpCode::kCmpEq:
            out << Reg(inst.a) << " <- (" << Reg(inst.b) << " == " << Reg(inst.c) << ")";
            break;
        case OpCode::kJmp: {
            const int64_t target = static_cast<int64_t>(pc) + static_cast<int32_t>(inst.a);
            out << "jmp " << static_cast<int32_t>(inst.a) << " -> "
                << FormatTarget(target, code_size);
            break;
        }
        case OpCode::kJmpIf: {
            const int64_t target = static_cast<int64_t>(pc) + static_cast<int32_t>(inst.b);
            out << "jmp_if " << Reg(inst.a) << ", " << static_cast<int32_t>(inst.b) << " -> "
                << FormatTarget(target, code_size);
            break;
        }
        case OpCode::kJmpIfZero: {
            const int64_t target = static_cast<int64_t>(pc) + static_cast<int32_t>(inst.b);
            out << "jmp_if_zero " << Reg(inst.a) << ", " << static_cast<int32_t>(inst.b)
                << " -> " << FormatTarget(target, code_size);
            break;
        }
        case OpCode::kJmpIfNotZero: {
            const int64_t target = static_cast<int64_t>(pc) + static_cast<int32_t>(inst.b);
            out << "jmp_if_not_zero " << Reg(inst.a) << ", " << static_cast<int32_t>(inst.b)
                << " -> " << FormatTarget(target, code_size);
            break;
        }
        case OpCode::kDecJnz: {
            const int64_t target = static_cast<int64_t>(pc) + static_cast<int32_t>(inst.b);
            out << "dec_jnz " << Reg(inst.a) << ", " << static_cast<int32_t>(inst.b) << " -> "
                << FormatTarget(target, code_size);
            break;
        }
        case OpCode::kAddDecJnz: {
            const int64_t target = static_cast<int64_t>(pc) + static_cast<int32_t>(inst.c);
            out << Reg(inst.a) << " += " << Reg(inst.b) << "; --" << Reg(inst.b)
                << "; jnz " << static_cast<int32_t>(inst.c) << " -> "
                << FormatTarget(target, code_size);
            break;
        }
        case OpCode::kSubImmJnz: {
            const int64_t target = static_cast<int64_t>(pc) + static_cast<int32_t>(inst.c);
            out << Reg(inst.a) << " -= " << static_cast<int32_t>(inst.b)
                << "; jnz " << static_cast<int32_t>(inst.c) << " -> "
                << FormatTarget(target, code_size);
            break;
        }
        case OpCode::kAddImmJnz: {
            const int64_t target = static_cast<int64_t>(pc) + static_cast<int32_t>(inst.c);
            out << Reg(inst.a) << " += " << static_cast<int32_t>(inst.b)
                << "; jnz " << static_cast<int32_t>(inst.c) << " -> "
                << FormatTarget(target, code_size);
            break;
        }
        case OpCode::kClosure:
            out << Reg(inst.a) << " <- closure func[" << inst.b << "] "
                << FunctionNameOrFallback(module, inst.b)
                << " capture_start=" << Reg(inst.c)
                << " upvalues=" << static_cast<uint32_t>(inst.flags);
            break;
        case OpCode::kGetUpval:
            out << Reg(inst.a) << " <- upval[" << inst.b << "]";
            break;
        case OpCode::kSetUpval:
            out << "upval[" << inst.b << "] <- " << Reg(inst.a);
            break;
        case OpCode::kCallClosure:
            out << Reg(inst.a) << " <- call_closure " << Reg(inst.b) << " "
                << FormatArgs(inst.a, inst.c);
            break;
        case OpCode::kTailCall:
            out << "tail_call func[" << inst.b << "] "
                << FunctionNameOrFallback(module, inst.b) << " "
                << FormatArgs(inst.a, inst.c);
            break;
        case OpCode::kTailCallClosure:
            out << "tail_call_closure " << Reg(inst.b) << " "
                << FormatArgs(inst.a, inst.c);
            break;
        case OpCode::kVarArg:
            out << "vararg " << Reg(inst.a) << ".. count=" << inst.c << " start=" << inst.b;
            break;
        case OpCode::kStrLen:
            out << Reg(inst.a) << " <- strlen(" << Reg(inst.b) << ")";
            break;
        case OpCode::kStrConcat:
            out << Reg(inst.a) << " <- concat(" << Reg(inst.b) << ", " << Reg(inst.c) << ")";
            break;
        case OpCode::kBitAnd:
            out << Reg(inst.a) << " <- " << Reg(inst.b) << " & " << Reg(inst.c);
            break;
        case OpCode::kBitOr:
            out << Reg(inst.a) << " <- " << Reg(inst.b) << " | " << Reg(inst.c);
            break;
        case OpCode::kBitXor:
            out << Reg(inst.a) << " <- " << Reg(inst.b) << " ^ " << Reg(inst.c);
            break;
        case OpCode::kBitNot:
            out << Reg(inst.a) << " <- ~" << Reg(inst.b);
            break;
        case OpCode::kBitShl:
            out << Reg(inst.a) << " <- " << Reg(inst.b) << " << " << Reg(inst.c);
            break;
        case OpCode::kBitShr:
            out << Reg(inst.a) << " <- " << Reg(inst.b) << " >> " << Reg(inst.c);
            break;
        case OpCode::kTryBegin:
            out << "try_begin catch=#" << inst.a << " finally=#" << inst.b << " end=#"
                << inst.c;
            break;
        case OpCode::kTryEnd:
            out << "try_end";
            break;
        case OpCode::kEndCatch:
            out << "end_catch";
            break;
        case OpCode::kEndFinally:
            out << "end_finally";
            break;
        case OpCode::kCall:
            out << Reg(inst.a) << " <- call func[" << inst.b << "] "
                << FunctionNameOrFallback(module, inst.b) << FormatArgs(inst.a, inst.c);
            break;
        case OpCode::kFfiCall:
            out << Reg(inst.a) << " <- ffi \"" << EscapeText(Utf8ConstantOrFallback(module, inst.b))
                << "\" " << FormatArgs(inst.a, inst.c);
            break;
        case OpCode::kRet:
            out << "ret " << Reg(inst.a);
            break;
        case OpCode::kThrow:
            out << "throw " << Reg(inst.a);
            break;
        case OpCode::kHalt:
            out << "halt";
            break;
        case OpCode::kNewObject:
            out << Reg(inst.a) << " <- new \"" << EscapeText(Utf8ConstantOrFallback(module, inst.b))
                << "\"";
            break;
        case OpCode::kInvoke:
            out << Reg(inst.a) << " <- invoke " << Reg(inst.a) << "."
                << EscapeText(Utf8ConstantOrFallback(module, inst.b))
                << FormatArgs(inst.a, inst.c);
            break;
    }
    return out.str();
}

void PrintConstantTable(const Module& module) {
    std::cout << "constants (" << module.constants().size() << "):\n";
    for (size_t i = 0; i < module.constants().size(); ++i) {
        std::cout << "  const[" << i << "] = "
                  << ConstantSummary(module, static_cast<uint32_t>(i)) << "\n";
    }
}

void PrintFfiMap(const Module& module) {
    std::cout << "ffi-map:\n";
    if (!module.ffi_library_paths().empty()) {
        std::cout << "  ffi-libraries:\n";
        for (size_t i = 0; i < module.ffi_library_paths().size(); ++i) {
            std::cout << "    lib[" << i << "] = " << module.ffi_library_paths()[i] << "\n";
        }
    }
    if (!module.ffi_signatures().empty()) {
        std::cout << "  ffi-signatures:\n";
        for (size_t i = 0; i < module.ffi_signatures().size(); ++i) {
            const auto& sig = module.ffi_signatures()[i];
            std::cout << "    sig[" << i << "] " << sig.name
                      << " cc=" << static_cast<uint32_t>(sig.convention)
                      << " params=" << sig.params.size() << "\n";
        }
    }
    if (!module.ffi_bindings().empty()) {
        std::cout << "  ffi-bindings:\n";
        for (size_t i = 0; i < module.ffi_bindings().size(); ++i) {
            const auto& binding = module.ffi_bindings()[i];
            std::cout << "    bind[" << i << "] " << binding.vm_symbol << " -> "
                      << binding.native_symbol << " lib=" << binding.library_index
                      << " sig=" << binding.signature_index << "\n";
        }
    }
    for (const auto& fn : module.functions()) {
        std::unordered_set<std::string> dedup;
        const auto code = fn.FlattenedInstructions();
        for (const auto& inst : code) {
            if (inst.opcode != OpCode::kFfiCall) {
                continue;
            }
            if (inst.b >= module.constants().size()) {
                continue;
            }
            const auto& c = module.constants()[inst.b];
            if (c.type != ConstantType::kUtf8) {
                continue;
            }
            dedup.insert(c.utf8_value);
        }
        if (dedup.empty()) {
            continue;
        }
        for (const auto& sym : dedup) {
            std::cout << "  " << fn.name() << " -> " << sym << "\n";
        }
        if (fn.ffi_binding().enabled) {
            std::cout << "  " << fn.name() << " [ffi-header] cc="
                      << static_cast<uint32_t>(fn.ffi_binding().convention)
                      << " sig=" << fn.ffi_binding().signature_index
                      << " bind=" << fn.ffi_binding().binding_index << "\n";
        }
    }
}

void PrintClassMap(const Module& module) {
    std::cout << "class-map:\n";
    if (module.classes().empty()) {
        std::cout << "  <none>\n";
        return;
    }
    for (size_t i = 0; i < module.classes().size(); ++i) {
        const auto& klass = module.classes()[i];
        std::cout << "  class[" << i << "] " << klass.name;
        if (!klass.base_classes.empty()) {
            std::cout << " : ";
            for (size_t b = 0; b < klass.base_classes.size(); ++b) {
                if (b > 0) {
                    std::cout << ", ";
                }
                std::cout << klass.base_classes[b];
            }
        }
        std::cout << "\n";
        for (const auto& method : klass.methods) {
            std::cout << "    method " << method.name
                      << " -> func[" << method.function_index << "] "
                      << FunctionNameOrFallback(module, method.function_index)
                      << " access=" << static_cast<uint32_t>(method.access)
                      << " virtual=" << (method.is_virtual ? "yes" : "no")
                      << "\n";
        }
    }
}

}  // namespace

int Check(const std::filesystem::path& path) {
    auto module_or = Serializer::DeserializeFromFile(path);
    if (!module_or.ok()) {
        std::cerr << "deserialize failed: " << module_or.status().message() << "\n";
        return 2;
    }
    auto verify = Verifier::Verify(module_or.value());
    if (!verify.status.ok()) {
        std::cerr << "verify failed: " << verify.status.message() << "\n";
        return 3;
    }
    std::cout << "OK\n";
    for (const auto& warning : verify.warnings) {
        std::cout << "warning: " << warning << "\n";
    }
    return 0;
}

int DisasmTbc(const std::filesystem::path& path) {
    auto module_or = Serializer::DeserializeFromFile(path);
    if (!module_or.ok()) {
        std::cerr << "deserialize failed: " << module_or.status().message() << "\n";
        return 2;
    }
    const auto& module = module_or.value();
    std::cout << "module " << module.name() << " v" << module.version().ToString() << "\n";
    PrintConstantTable(module);
    for (size_t fn_i = 0; fn_i < module.functions().size(); ++fn_i) {
        const auto& fn = module.functions()[fn_i];
        std::cout << "func[" << fn_i << "] " << fn.name() << " regs=" << fn.reg_count()
                  << " params=" << fn.param_count()
                  << " upvalues=" << fn.upvalue_count()
                  << " vararg=" << (fn.is_vararg() ? "yes" : "no")
                  << "\n";
        const auto code = fn.FlattenedInstructions();
        for (size_t i = 0; i < code.size(); ++i) {
            const auto& inst = code[i];
            std::cout << "  " << i << ": " << OpCodeName(inst.opcode) << " " << inst.a
                      << ", " << inst.b << ", " << inst.c << "    ; "
                      << FormatInstruction(module, inst, i, code.size()) << "\n";
        }
    }
    PrintFfiMap(module);
    PrintClassMap(module);
    return 0;
}

int DisasmTlb(const std::filesystem::path& path) {
    if (path.extension() == ".tlbs") {
        auto bundle_or = TlbsBundle::Deserialize(path);
        if (!bundle_or.ok()) {
            std::cerr << "tlbs deserialize failed: " << bundle_or.status().message() << "\n";
            return 2;
        }
        const auto& bundle = bundle_or.value();
        std::cout << "tlbs " << bundle.manifest().name << " v"
                  << bundle.manifest().version.ToString() << "\n";
        std::cout << "modules=" << bundle.manifest().modules.size()
                  << " libraries=" << bundle.libraries().size() << "\n";
        for (size_t i = 0; i < bundle.manifest().modules.size(); ++i) {
            const auto& module_path = bundle.manifest().modules[i];
            auto mod_it = bundle.modules().find(module_path);
            if (mod_it == bundle.modules().end()) {
                std::cout << "module[" << i << "] missing bytes for " << module_path << "\n";
                continue;
            }
            auto module_or = Serializer::Deserialize(mod_it->second);
            if (!module_or.ok()) {
                std::cout << "module[" << i << "] parse failed: "
                          << module_or.status().message() << "\n";
                continue;
            }
            const auto& module = module_or.value();
            std::cout << "module[" << i << "] path=" << module_path << " name=" << module.name()
                      << " funcs=" << module.functions().size()
                      << " consts=" << module.constants().size() << "\n";
            PrintConstantTable(module);
            PrintFfiMap(module);
        }
        return 0;
    }

    auto tlb_or = TlbContainer::DeserializeFromFile(path);
    if (!tlb_or.ok()) {
        std::cerr << "tlb deserialize failed: " << tlb_or.status().message() << "\n";
        return 2;
    }
    const auto& container = tlb_or.value();
    std::cout << "tlb modules=" << container.modules().size() << "\n";
    for (size_t i = 0; i < container.modules().size(); ++i) {
        const auto& entry = container.modules()[i];
        std::cout << "module[" << i << "] " << entry.module_name << " v"
                  << entry.version.ToString() << " payload=" << entry.bytecode.size()
                  << " bytes\n";
        if (!entry.native_plugins.empty()) {
            std::cout << "  native-plugins:";
            for (const auto& p : entry.native_plugins) {
                std::cout << " " << p;
            }
            std::cout << "\n";
        }

        auto module_or = Serializer::Deserialize(entry.bytecode);
        if (!module_or.ok()) {
            std::cout << "  parse-bytecode-failed: " << module_or.status().message()
                      << "\n";
            continue;
        }
        const auto& module = module_or.value();
        std::cout << "  bytecode module " << module.name() << " funcs="
                  << module.functions().size() << " consts=" << module.constants().size()
                  << "\n";
        PrintConstantTable(module);
        PrintFfiMap(module);
    }
    return 0;
}

}  // namespace tie::vm::cli
