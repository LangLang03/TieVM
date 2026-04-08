#pragma once

#include <filesystem>
#include <vector>

#include "tie/vm/bytecode/module.hpp"
#include "tie/vm/common/status.hpp"

namespace tie::vm {

class Serializer {
  public:
    [[nodiscard]] static StatusOr<std::vector<uint8_t>> Serialize(
        const Module& module, bool include_debug_section = true);

    [[nodiscard]] static StatusOr<Module> Deserialize(const std::vector<uint8_t>& bytes);

    [[nodiscard]] static Status SerializeToFile(
        const Module& module, const std::filesystem::path& path,
        bool include_debug_section = true);

    [[nodiscard]] static StatusOr<Module> DeserializeFromFile(
        const std::filesystem::path& path);
};

}  // namespace tie::vm

