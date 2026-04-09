#include "tiebc_commands.hpp"

#include <iostream>
#include <string>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

int Usage() {
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
    std::cerr << "  tiebc build-stdlib <output.tlb>\n";
    std::cerr << "  tiebc build-stdlib-tlbs <output.tlbs>\n";
    std::cerr << "  tiebc emit-hello <output.tbc>\n";
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
    return Usage();
}

}  // namespace tie::vm::cli
