#include "tiebc_commands.hpp"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

namespace {

int Usage() {
    std::cerr << "Usage:\n";
    std::cerr << "  tiebc check <file.tbc>\n";
    std::cerr << "  tiebc disasm <file.tbc>\n";
    std::cerr << "  tiebc disasm <file.tlb>\n";
    std::cerr << "  tiebc map <file.tlb>\n";
    std::cerr << "  tiebc tlb-struct\n";
    std::cerr << "  tiebc tbc-struct\n";
    std::cerr << "  tiebc build-stdlib <output.tlb>\n";
    std::cerr << "  tiebc emit-hello <output.tbc>\n";
    return 1;
}

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
            out << Reg(inst.a) << " <- const[" << inst.b << "] " << ConstantSummary(module, inst.b);
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
                << EscapeText(Utf8ConstantOrFallback(module, inst.b)) << FormatArgs(inst.a, inst.c);
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
    }
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
                  << " params=" << fn.param_count() << "\n";
        const auto code = fn.FlattenedInstructions();
        for (size_t i = 0; i < code.size(); ++i) {
            const auto& inst = code[i];
            std::cout << "  " << i << ": " << OpCodeName(inst.opcode) << " " << inst.a << ", "
                      << inst.b << ", " << inst.c << "    ; "
                      << FormatInstruction(module, inst, i, code.size()) << "\n";
        }
    }
    PrintFfiMap(module);
    return 0;
}

int DisasmTlb(const std::filesystem::path& path) {
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
            std::cout << "  parse-bytecode-failed: " << module_or.status().message() << "\n";
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

int EmitHello(const std::filesystem::path& path) {
    Module module("demo.hello");
    module.version() = SemanticVersion{0, 1, 0};
    const auto print_symbol = module.AddConstant(Constant::Utf8("tie.std.io.print"));
    const auto hello = module.AddConstant(Constant::Utf8("Hello, World!"));
    const auto chinese = module.AddConstant(
        Constant::Utf8(
            "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C"
            "\xE4\xB8\x96\xE7\x95\x8C\xEF\xBC\x81"));

    auto& fn = module.AddFunction("entry", 6, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb)
        .LoadK(1, hello)
        .FfiCall(0, print_symbol, 1)
        .LoadK(1, chinese)
        .FfiCall(0, print_symbol, 1)
        .Ret(0);
    module.set_entry_function(0);

    auto status = Serializer::SerializeToFile(module, path, true);
    if (!status.ok()) {
        std::cerr << "emit hello failed: " << status.message() << "\n";
        return 2;
    }
    std::cout << "wrote " << path.string() << "\n";
    return 0;
}

int PrintTlbStruct() {
    std::cout << "TLB binary layout (little-endian):\n";
    std::cout << "  header:\n";
    std::cout << "    magic[4]            = 'TLB0'\n";
    std::cout << "    version_major(u16)\n";
    std::cout << "    version_minor(u16)\n";
    std::cout << "    module_count(u32)\n";
    std::cout << "  repeated module_count times:\n";
    std::cout << "    module_name         = string(u32 len + bytes)\n";
    std::cout << "    semver_major(u32)\n";
    std::cout << "    semver_minor(u32)\n";
    std::cout << "    semver_patch(u32)\n";
    std::cout << "    plugin_count(u32)\n";
    std::cout << "    native_plugins      = repeated string\n";
    std::cout << "    bytecode_len(u32)\n";
    std::cout << "    bytecode_payload    = raw .tbc bytes\n";
    return 0;
}

int PrintTbcStruct() {
    std::cout << "TBC binary layout (little-endian):\n";
    std::cout << "  header:\n";
    std::cout << "    magic[4]            = 'TBC0'\n";
    std::cout << "    format_major(u16)\n";
    std::cout << "    format_minor(u16)\n";
    std::cout << "    flags(u32)\n";
    std::cout << "    module_name         = string(u32 len + bytes)\n";
    std::cout << "    module_semver       = major/minor/patch (u32 x3)\n";
    std::cout << "    entry_function(u32)\n";
    std::cout << "    constant_count(u32)\n";
    std::cout << "    function_count(u32)\n";
    std::cout << "  constants:\n";
    std::cout << "    type(u8) + payload (i64/f64/utf8-string)\n";
    std::cout << "  functions:\n";
    std::cout << "    name(string), reg_count(u16), param_count(u16), inst_count(u32)\n";
    std::cout << "    instructions         = fixed 16 bytes each: opcode(u8), flags(u8),"
                 " reserved(u16), a(u32), b(u32), c(u32)\n";
    std::cout << "  optional debug section (when flag enabled):\n";
    std::cout << "    debug_line_count(u32), entries(function_idx, inst_idx, line, col)\n";
    return 0;
}

}  // namespace

int RunTiebc(int argc, char** argv) {
    if (argc < 2) {
        return Usage();
    }
    const std::string cmd = argv[1];
    if (cmd == "tlb-struct") {
        return PrintTlbStruct();
    }
    if (cmd == "tbc-struct") {
        return PrintTbcStruct();
    }
    if (argc < 3) {
        return Usage();
    }
    const std::filesystem::path path = argv[2];
    if (cmd == "check") {
        return Check(path);
    }
    if (cmd == "disasm") {
        if (path.extension() == ".tlb") {
            return DisasmTlb(path);
        }
        return DisasmTbc(path);
    }
    if (cmd == "map") {
        return DisasmTlb(path);
    }
    if (cmd == "build-stdlib") {
        auto status = BuildStdlibTlb(path);
        if (!status.ok()) {
            std::cerr << "build stdlib failed: " << status.message() << "\n";
            return 2;
        }
        std::cout << "wrote " << path.string() << "\n";
        return 0;
    }
    if (cmd == "emit-hello") {
        return EmitHello(path);
    }
    return Usage();
}

}  // namespace tie::vm::cli

