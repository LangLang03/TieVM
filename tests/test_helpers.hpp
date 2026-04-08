#pragma once

#include <chrono>
#include <filesystem>
#include <string>

#include "tie/vm/api.hpp"

namespace tie::vm::test {

inline std::filesystem::path TempPath(const std::string& suffix) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("tievm_" + std::to_string(now) + "_" + suffix);
    return path;
}

inline Module BuildAddModule(int64_t lhs, int64_t rhs) {
    Module module("test.math");
    module.version() = SemanticVersion{0, 1, 0};
    const auto lhs_idx = module.AddConstant(Constant::Int64(lhs));
    const auto rhs_idx = module.AddConstant(Constant::Int64(rhs));
    auto& fn = module.AddFunction("entry", 8, 0);
    auto& bb = fn.AddBlock("entry");
    InstructionBuilder(bb).LoadK(1, lhs_idx).LoadK(2, rhs_idx).Add(0, 1, 2).Ret(0);
    module.set_entry_function(0);
    return module;
}

}  // namespace tie::vm::test

