#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "tie/vm/common/status.hpp"
#include "tie/vm/common/version.hpp"

namespace tie::vm {

struct TlbsManifest {
    std::string name;
    SemanticVersion version{0, 1, 0};
    std::vector<std::string> modules;
    std::unordered_map<std::string, std::string> metadata;
};

class TlbsBundle {
  public:
    [[nodiscard]] const TlbsManifest& manifest() const { return manifest_; }
    [[nodiscard]] TlbsManifest& manifest() { return manifest_; }
    [[nodiscard]] const std::unordered_map<std::string, std::vector<uint8_t>>& modules() const {
        return modules_;
    }
    [[nodiscard]] const std::unordered_map<std::string, std::vector<uint8_t>>& libraries() const {
        return libraries_;
    }

    void SetModule(std::string relative_path, std::vector<uint8_t> bytes);
    void SetLibrary(std::string relative_path, std::vector<uint8_t> bytes);

    [[nodiscard]] Status SerializeToDirectory(const std::filesystem::path& root) const;
    [[nodiscard]] Status SerializeToZip(const std::filesystem::path& zip_path) const;

    [[nodiscard]] static StatusOr<TlbsBundle> Deserialize(const std::filesystem::path& path);

  private:
    TlbsManifest manifest_;
    std::unordered_map<std::string, std::vector<uint8_t>> modules_;
    std::unordered_map<std::string, std::vector<uint8_t>> libraries_;
};

[[nodiscard]] std::string CurrentPlatformName();
[[nodiscard]] std::string CurrentArchName();

}  // namespace tie::vm
