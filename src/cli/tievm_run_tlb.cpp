#include "tievm_commands.hpp"

#include <iostream>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

int RunTlbFile(
    VmInstance& vm,
    const std::filesystem::path& input,
    const std::optional<std::string>& module_name_override) {
    Status status = Status::Ok();
    if (input.extension() == ".tlbs") {
        status = vm.loader().LoadTlbsFile(input);
    } else {
        status = vm.loader().LoadTlbFile(input);
    }
    if (!status.ok()) {
        std::cerr << "bundle load failed: " << status.message() << "\n";
        return 4;
    }

    std::string module_name;
    if (module_name_override.has_value()) {
        module_name = *module_name_override;
    } else {
        auto names = vm.loader().ActiveModuleNames();
        if (names.empty()) {
            std::cerr << "bundle has no module\n";
            return 5;
        }
        module_name = names.front();
    }

    auto value_or = vm.ExecuteLoadedModule(module_name);
    if (!value_or.ok()) {
        std::cerr << "runtime failed: ";
        if (value_or.status().vm_error().has_value()) {
            std::cerr << value_or.status().vm_error()->Format() << "\n";
        } else {
            std::cerr << value_or.status().message() << "\n";
        }
        return 6;
    }
    std::cout << value_or.value().ToString() << "\n";
    return 0;
}

}  // namespace tie::vm::cli
