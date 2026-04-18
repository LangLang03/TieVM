#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>

#include "tie/vm/bytecode/module.hpp"
#include "tie/vm/common/status.hpp"

namespace tie::vm {

enum class AotOutputKind : uint8_t {
    kExecutable = 0,
    kSharedLibrary = 1,
};

struct AotCompileOptions {
    std::filesystem::path input_path;
    std::optional<std::string> module_name_override;
    std::filesystem::path output_executable;
    AotOutputKind output_kind = AotOutputKind::kExecutable;
    std::string clang_path = "clang";
    std::optional<std::string> target_triple;
    std::optional<std::filesystem::path> sysroot;
    std::string opt_level = "O3";
    std::vector<std::string> cflags;
    std::vector<std::string> ldflags;
    std::optional<std::filesystem::path> emit_ir;
    std::optional<std::filesystem::path> emit_obj;
    std::optional<std::filesystem::path> emit_header;
    bool verify = true;
};

struct AotCompileResult {
    std::filesystem::path output_executable;
    AotOutputKind output_kind = AotOutputKind::kExecutable;
    std::optional<std::filesystem::path> emitted_ir;
    std::optional<std::filesystem::path> emitted_obj;
    std::optional<std::filesystem::path> emitted_header;
    std::string target_triple;
    std::string compiled_module;
    std::vector<std::filesystem::path> linked_libraries;
    std::vector<std::string> exported_functions;
};

class AotCompiler {
  public:
    [[nodiscard]] StatusOr<AotCompileResult> Compile(const AotCompileOptions& options) const;
};

// Placeholder API retained only for short-term source compatibility.
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
