#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace tie::vm {
class VmInstance;
}

namespace tie::vm::cli {

int RunTievm(int argc, char** argv);
int RunTbcFile(VmInstance& vm, const std::filesystem::path& input);
int RunTlbFile(
    VmInstance& vm,
    const std::filesystem::path& input,
    const std::optional<std::string>& module_name_override);

}  // namespace tie::vm::cli
