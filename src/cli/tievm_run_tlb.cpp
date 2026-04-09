#include "tievm_commands.hpp"

#include <algorithm>
#include <iostream>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

int RunTlbFile(
    VmInstance& vm,
    const std::filesystem::path& input,
    const std::optional<std::string>& module_name_override) {
    std::optional<std::string> manifest_entry_module;
    Status status = Status::Ok();
    if (input.extension() == ".tlbs") {
        auto bundle_or = TlbsBundle::Deserialize(input);
        if (!bundle_or.ok()) {
            std::cerr << "bundle load failed: " << bundle_or.status().message() << "\n";
            return 4;
        }
        manifest_entry_module = bundle_or.value().manifest().entry_module;
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
        if (manifest_entry_module.has_value()) {
            const auto file_name = std::filesystem::path(*manifest_entry_module).filename().string();
            const auto ext = std::filesystem::path(file_name).extension().string();
            if (ext == ".tbc") {
                module_name = std::filesystem::path(file_name).stem().string();
            }
        }
        auto names = vm.loader().ActiveModuleNames();
        if (names.empty()) {
            std::cerr << "bundle has no module\n";
            return 5;
        }
        if (!module_name.empty()) {
            const bool found =
                std::find(names.begin(), names.end(), module_name) != names.end();
            if (!found) {
                module_name.clear();
            }
        }
        if (module_name.empty()) {
            module_name = names.front();
        }
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
