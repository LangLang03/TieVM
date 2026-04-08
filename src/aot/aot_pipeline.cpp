#include "tie/vm/aot/aot_pipeline.hpp"

#include <fstream>

namespace tie::vm {

Status AotMetadataEmitter::Emit(const AotUnit& unit, const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return Status::SerializationError("failed opening aot metadata output");
    }
    out << "module=" << unit.module_name << "\n";
    out << "ir_size=" << unit.ir_payload.size() << "\n";
    for (const auto& [k, v] : unit.metadata) {
        out << k << "=" << v << "\n";
    }
    return Status::Ok();
}

Status AotPipeline::AddUnit(AotUnit unit) {
    for (const auto& existing : units_) {
        if (existing.module_name == unit.module_name) {
            return Status::AlreadyExists("aot unit already exists");
        }
    }
    units_.push_back(std::move(unit));
    return Status::Ok();
}

StatusOr<std::vector<AotUnit>> AotPipeline::SnapshotUnits() const { return units_; }

Status AotPipeline::EmitMetadataDirectory(const std::filesystem::path& dir) const {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return Status::SerializationError("failed creating aot metadata directory");
    }
    AotMetadataEmitter emitter;
    for (const auto& unit : units_) {
        const auto file = dir / (unit.module_name + ".aotmeta");
        auto status = emitter.Emit(unit, file);
        if (!status.ok()) {
            return status;
        }
    }
    return Status::Ok();
}

}  // namespace tie::vm

