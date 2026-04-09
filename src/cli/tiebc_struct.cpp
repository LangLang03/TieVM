#include "tiebc_commands.hpp"

#include <filesystem>
#include <iostream>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

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

int PrintTlbsStruct() {
    std::cout << "TLBS package layout:\n";
    std::cout << "  manifest.toml\n";
    std::cout << "    name/version/modules/(optional) entry_module\n";
    std::cout << "  modules/\n";
    std::cout << "    *.tbc (ffi metadata embedded in bytecode extension section)\n";
    std::cout << "  libs/<platform>/<arch>/\n";
    std::cout << "    *.dll|*.so|*.dylib\n";
    std::cout << "Formats:\n";
    std::cout << "  - directory bundle: .tlbs directory\n";
    std::cout << "  - zip bundle: .tlbs file (zip store/deflate mode)\n";
    return 0;
}

int CheckTlbs(const std::filesystem::path& path) {
    auto bundle_or = TlbsBundle::Deserialize(path);
    if (!bundle_or.ok()) {
        std::cerr << "tlbs check failed: " << bundle_or.status().message() << "\n";
        return 2;
    }
    const auto& bundle = bundle_or.value();
    std::cout << "name: " << bundle.manifest().name << "\n";
    std::cout << "version: " << bundle.manifest().version.ToString() << "\n";
    if (bundle.manifest().entry_module.has_value()) {
        std::cout << "entry_module: " << *bundle.manifest().entry_module << "\n";
    }
    std::cout << "modules: " << bundle.manifest().modules.size() << "\n";
    std::cout << "libraries: " << bundle.libraries().size() << "\n";
    return 0;
}

int PackTlbs(const std::filesystem::path& input_dir, const std::filesystem::path& output_file) {
    auto bundle_or = TlbsBundle::Deserialize(input_dir);
    if (!bundle_or.ok()) {
        std::cerr << "tlbs pack failed: " << bundle_or.status().message() << "\n";
        return 2;
    }
    auto status = bundle_or.value().SerializeToZip(output_file);
    if (!status.ok()) {
        std::cerr << "tlbs pack failed: " << status.message() << "\n";
        return 2;
    }
    std::cout << "wrote " << output_file.string() << "\n";
    return 0;
}

int UnpackTlbs(
    const std::filesystem::path& input_file, const std::filesystem::path& output_dir) {
    auto bundle_or = TlbsBundle::Deserialize(input_file);
    if (!bundle_or.ok()) {
        std::cerr << "tlbs unpack failed: " << bundle_or.status().message() << "\n";
        return 2;
    }
    auto status = bundle_or.value().SerializeToDirectory(output_dir);
    if (!status.ok()) {
        std::cerr << "tlbs unpack failed: " << status.message() << "\n";
        return 2;
    }
    std::cout << "wrote " << output_dir.string() << "\n";
    return 0;
}

}  // namespace tie::vm::cli
