#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace tie::vm {
class VmInstance;
}

namespace tie::vm::cli {

struct RunConfig {
    bool trusted = false;
    std::optional<std::filesystem::path> cache_dir;
};

int RunTievm(int argc, char** argv);
int RunTbcFile(
    VmInstance& vm, const std::filesystem::path& input, const RunConfig& config);
int RunTlbFile(
    VmInstance& vm,
    const std::filesystem::path& input,
    const std::optional<std::string>& module_name_override,
    const RunConfig& config);

}  // namespace tie::vm::cli
