#include "tiebc_commands.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

namespace {

bool ParseInt64(std::string_view text, int64_t* out) {
    int64_t parsed = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc{} || ptr != last) {
        return false;
    }
    *out = parsed;
    return true;
}

}  // namespace

int Usage() {
#if defined(TIEVM_ENABLE_HELP)
    std::cerr << "Usage:\n";
    std::cerr << "  tiebc check <file.tbc>\n";
    std::cerr << "  tiebc disasm <file.tbc>\n";
    std::cerr << "  tiebc disasm <file.tlb>\n";
    std::cerr << "  tiebc disasm <file.tlbs>\n";
    std::cerr << "  tiebc map <file.tlb>\n";
    std::cerr << "  tiebc tlbs-check <file.tlbs>\n";
    std::cerr << "  tiebc tlbs-pack <input_dir.tlbs> <output.tlbs>\n";
    std::cerr << "  tiebc tlbs-unpack <input.tlbs> <output_dir.tlbs>\n";
    std::cerr << "  tiebc tlb-struct\n";
    std::cerr << "  tiebc tlbs-struct\n";
    std::cerr << "  tiebc tbc-struct\n";
    std::cerr << "  tiebc aot <input.{tbc|tlb|tlbs}> [module] -o <output> [--shared] [--target <triple>] [--cc <clang>] [--sysroot <path>] [--opt <O0|O1|O2|O3>] [--ldflag <flag>]... [--cflag <flag>]... [--emit-ir <file.ll>] [--emit-obj <file.o>] [--emit-header <file.h>]\n";
    std::cerr << "  tiebc build-stdlib <output.tlb>\n";
    std::cerr << "  tiebc build-stdlib-tlbs <output.tlbs>\n";
    std::cerr << "  tiebc emit-hello <output.tbc>\n";
    std::cerr << "  tiebc emit-opset <output.tbc>\n";
    std::cerr << "  tiebc emit-closure-upvalue <output.tbc>\n";
    std::cerr << "  tiebc emit-fib <output.tbc> [n]\n";
    std::cerr << "  tiebc emit-error-handling <output.tbc>\n";
    std::cerr << "  tiebc emit-oop-ok <output.tbc>\n";
    std::cerr << "  tiebc emit-oop-error <output.tbc>\n";
#endif
    return 1;
}

int RunTiebc(int argc, char** argv) {
    if (argc < 2) {
        return Usage();
    }
    const std::string cmd = argv[1];
    if (cmd == "tlb-struct") {
        return PrintTlbStruct();
    }
    if (cmd == "tlbs-struct") {
        return PrintTlbsStruct();
    }
    if (cmd == "tbc-struct") {
        return PrintTbcStruct();
    }
    if (cmd == "aot") {
        return AotCompileCmd(argc, argv);
    }
    if (argc < 3) {
        return Usage();
    }
    const std::filesystem::path path = argv[2];
    if (cmd == "check") {
        return Check(path);
    }
    if (cmd == "disasm") {
        if (path.extension() == ".tlb" || path.extension() == ".tlbs") {
            return DisasmTlb(path);
        }
        return DisasmTbc(path);
    }
    if (cmd == "map") {
        return DisasmTlb(path);
    }
    if (cmd == "tlbs-check") {
        return CheckTlbs(path);
    }
    if (cmd == "tlbs-pack") {
        if (argc < 4) {
            return Usage();
        }
        return PackTlbs(path, argv[3]);
    }
    if (cmd == "tlbs-unpack") {
        if (argc < 4) {
            return Usage();
        }
        return UnpackTlbs(path, argv[3]);
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
    if (cmd == "build-stdlib-tlbs") {
        return BuildStdlibTlbsCmd(path);
    }
    if (cmd == "emit-hello") {
        return EmitHello(path);
    }
    if (cmd == "emit-opset") {
        return EmitOpset(path);
    }
    if (cmd == "emit-closure-upvalue") {
        return EmitClosureUpvalue(path);
    }
    if (cmd == "emit-fib") {
        int64_t n = 40;
        if (argc >= 4) {
            if (!ParseInt64(argv[3], &n) || n < 0 || n > 92) {
                std::cerr << "emit-fib requires n in [0, 92]\n";
                return 1;
            }
        }
        return EmitFib(path, n);
    }
    if (cmd == "emit-error-handling") {
        return EmitErrorHandling(path);
    }
    if (cmd == "emit-oop-ok") {
        return EmitOopOk(path);
    }
    if (cmd == "emit-oop-error") {
        return EmitOopError(path);
    }
    return Usage();
}

}  // namespace tie::vm::cli
