#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "tie/vm/common/status.hpp"

namespace tie::vm {

struct AotUnit {
    std::string module_name;
    std::vector<uint8_t> ir_payload;
    std::unordered_map<std::string, std::string> metadata;
};

class AotMetadataEmitter {
  public:
    [[nodiscard]] Status Emit(const AotUnit& unit, const std::filesystem::path& path) const;
};

class AotPipeline {
  public:
    Status AddUnit(AotUnit unit);
    [[nodiscard]] StatusOr<std::vector<AotUnit>> SnapshotUnits() const;
    [[nodiscard]] Status EmitMetadataDirectory(const std::filesystem::path& dir) const;

  private:
    std::vector<AotUnit> units_;
};

}  // namespace tie::vm

