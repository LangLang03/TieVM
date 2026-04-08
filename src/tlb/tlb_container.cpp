#include "tie/vm/tlb/tlb_container.hpp"

#include <algorithm>
#include <fstream>

namespace tie::vm {

namespace {

constexpr uint8_t kMagic[4] = {'T', 'L', 'B', '0'};
constexpr uint16_t kVersionMajor = 0;
constexpr uint16_t kVersionMinor = 1;

void WriteU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void WriteU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFFu));
    }
}

void WriteString(std::vector<uint8_t>& out, const std::string& value) {
    WriteU32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

StatusOr<uint16_t> ReadU16(const std::vector<uint8_t>& bytes, size_t* offset) {
    if (*offset + 2 > bytes.size()) {
        return Status::SerializationError("unexpected eof reading u16");
    }
    const uint16_t v =
        static_cast<uint16_t>(bytes[*offset]) |
        static_cast<uint16_t>(bytes[*offset + 1] << 8);
    *offset += 2;
    return v;
}

StatusOr<uint32_t> ReadU32(const std::vector<uint8_t>& bytes, size_t* offset) {
    if (*offset + 4 > bytes.size()) {
        return Status::SerializationError("unexpected eof reading u32");
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(bytes[*offset + i]) << (i * 8);
    }
    *offset += 4;
    return v;
}

StatusOr<std::string> ReadString(const std::vector<uint8_t>& bytes, size_t* offset) {
    auto len_or = ReadU32(bytes, offset);
    if (!len_or.ok()) {
        return len_or.status();
    }
    if (*offset + len_or.value() > bytes.size()) {
        return Status::SerializationError("unexpected eof reading string");
    }
    std::string out(
        reinterpret_cast<const char*>(bytes.data() + static_cast<ptrdiff_t>(*offset)),
        len_or.value());
    *offset += len_or.value();
    return out;
}

}  // namespace

void TlbContainer::AddModule(TlbModuleEntry entry) { modules_.push_back(std::move(entry)); }

StatusOr<std::vector<uint8_t>> TlbContainer::Serialize() const {
    std::vector<uint8_t> out;
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    WriteU16(out, kVersionMajor);
    WriteU16(out, kVersionMinor);
    WriteU32(out, static_cast<uint32_t>(modules_.size()));

    for (const auto& item : modules_) {
        WriteString(out, item.module_name);
        WriteU32(out, item.version.major);
        WriteU32(out, item.version.minor);
        WriteU32(out, item.version.patch);
        WriteU32(out, static_cast<uint32_t>(item.native_plugins.size()));
        for (const auto& plugin : item.native_plugins) {
            WriteString(out, plugin);
        }
        WriteU32(out, static_cast<uint32_t>(item.bytecode.size()));
        out.insert(out.end(), item.bytecode.begin(), item.bytecode.end());
    }
    return out;
}

StatusOr<TlbContainer> TlbContainer::Deserialize(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 8 || !std::equal(std::begin(kMagic), std::end(kMagic), bytes.begin())) {
        return Status::SerializationError("invalid tlb magic");
    }
    size_t offset = 4;
    auto vmaj_or = ReadU16(bytes, &offset);
    auto vmin_or = ReadU16(bytes, &offset);
    if (!vmaj_or.ok() || !vmin_or.ok()) {
        return Status::SerializationError("invalid tlb version section");
    }
    if (vmaj_or.value() != kVersionMajor) {
        return Status::SerializationError("unsupported tlb major version");
    }
    auto count_or = ReadU32(bytes, &offset);
    if (!count_or.ok()) {
        return count_or.status();
    }

    TlbContainer container;
    for (uint32_t i = 0; i < count_or.value(); ++i) {
        auto name_or = ReadString(bytes, &offset);
        auto maj_or = ReadU32(bytes, &offset);
        auto min_or = ReadU32(bytes, &offset);
        auto patch_or = ReadU32(bytes, &offset);
        auto plugin_count_or = ReadU32(bytes, &offset);
        if (!name_or.ok() || !maj_or.ok() || !min_or.ok() || !patch_or.ok() ||
            !plugin_count_or.ok()) {
            return Status::SerializationError("invalid tlb module header");
        }
        TlbModuleEntry entry;
        entry.module_name = name_or.value();
        entry.version = SemanticVersion{maj_or.value(), min_or.value(), patch_or.value()};
        for (uint32_t p = 0; p < plugin_count_or.value(); ++p) {
            auto plugin_or = ReadString(bytes, &offset);
            if (!plugin_or.ok()) {
                return plugin_or.status();
            }
            entry.native_plugins.push_back(std::move(plugin_or.value()));
        }
        auto bytecode_len_or = ReadU32(bytes, &offset);
        if (!bytecode_len_or.ok()) {
            return bytecode_len_or.status();
        }
        if (offset + bytecode_len_or.value() > bytes.size()) {
            return Status::SerializationError("invalid tlb bytecode payload");
        }
        entry.bytecode.assign(
            bytes.begin() + static_cast<ptrdiff_t>(offset),
            bytes.begin() + static_cast<ptrdiff_t>(offset + bytecode_len_or.value()));
        offset += bytecode_len_or.value();
        container.AddModule(std::move(entry));
    }
    return container;
}

Status TlbContainer::SerializeToFile(const std::filesystem::path& path) const {
    auto bytes_or = Serialize();
    if (!bytes_or.ok()) {
        return bytes_or.status();
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return Status::SerializationError("failed opening tlb output file");
    }
    const auto& bytes = bytes_or.value();
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out.good()) {
        return Status::SerializationError("failed writing tlb file");
    }
    return Status::Ok();
}

StatusOr<TlbContainer> TlbContainer::DeserializeFromFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return Status::NotFound("tlb file not found");
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Deserialize(bytes);
}

}  // namespace tie::vm
