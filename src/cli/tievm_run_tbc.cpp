#include "tievm_commands.hpp"

#include <iostream>

#include "tie/vm/api.hpp"

namespace tie::vm::cli {

int RunTbcFile(VmInstance& vm, const std::filesystem::path& input) {
    const auto stdlib_candidate = input.parent_path() / "stdlib.tlbs";
    if (std::filesystem::exists(stdlib_candidate)) {
        auto load_status = vm.loader().LoadTlbsFile(stdlib_candidate);
        if (!load_status.ok()) {
            std::cerr << "warning: auto-load stdlib.tlbs failed: " << load_status.message()
                      << "\n";
        }
    }

    auto module_or = Serializer::DeserializeFromFile(input);
    if (!module_or.ok()) {
        std::cerr << "deserialize failed: " << module_or.status().message() << "\n";
        return 2;
    }

    auto value_or = vm.ExecuteModule(module_or.value());
    if (!value_or.ok()) {
        std::cerr << "runtime failed: ";
        if (value_or.status().vm_error().has_value()) {
            std::cerr << value_or.status().vm_error()->Format() << "\n";
        } else {
            std::cerr << value_or.status().message() << "\n";
        }
        return 3;
    }

    std::cout << value_or.value().ToString() << "\n";
    return 0;
}

}  // namespace tie::vm::cli
