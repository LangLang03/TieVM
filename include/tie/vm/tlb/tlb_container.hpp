#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "tie/vm/common/status.hpp"
#include "tie/vm/common/version.hpp"

namespace tie::vm {

struct TlbModuleEntry {
    std::string module_name;
    SemanticVersion version;
    std::vector<uint8_t> bytecode;
    std::vector<std::string> native_plugins;
};

class TlbContainer {
  public:
    void AddModule(TlbModuleEntry entry);
    [[nodiscard]] const std::vector<TlbModuleEntry>& modules() const { return modules_; }

    [[nodiscard]] StatusOr<std::vector<uint8_t>> Serialize() const;
    [[nodiscard]] static StatusOr<TlbContainer> Deserialize(const std::vector<uint8_t>& bytes);

    [[nodiscard]] Status SerializeToFile(const std::filesystem::path& path) const;
    [[nodiscard]] static StatusOr<TlbContainer> DeserializeFromFile(
        const std::filesystem::path& path);

  private:
    std::vector<TlbModuleEntry> modules_;
};

}  // namespace tie::vm

