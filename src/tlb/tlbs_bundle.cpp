#include "tie/vm/tlb/tlbs_bundle.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>

#include <zlib.h>

namespace tie::vm {

namespace {

constexpr uint32_t kZipLocalHeader = 0x04034B50u;
constexpr uint32_t kZipCentralHeader = 0x02014B50u;
constexpr uint32_t kZipEndHeader = 0x06054B50u;
constexpr uint16_t kZipStoreMethod = 0;
constexpr uint16_t kZipDeflateMethod = 8;
constexpr uint16_t kZipVersion = 20;
constexpr uint16_t kZipFlagDataDescriptor = 0x0008;

void WriteU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void WriteU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

StatusOr<uint16_t> ReadU16(const std::vector<uint8_t>& bytes, size_t* offset) {
    if (*offset + 2 > bytes.size()) {
        return Status::SerializationError("unexpected eof reading u16");
    }
    const uint16_t value =
        static_cast<uint16_t>(bytes[*offset]) |
        static_cast<uint16_t>(bytes[*offset + 1] << 8);
    *offset += 2;
    return value;
}

StatusOr<uint32_t> ReadU32(const std::vector<uint8_t>& bytes, size_t* offset) {
    if (*offset + 4 > bytes.size()) {
        return Status::SerializationError("unexpected eof reading u32");
    }
    const uint32_t value =
        static_cast<uint32_t>(bytes[*offset]) |
        (static_cast<uint32_t>(bytes[*offset + 1]) << 8) |
        (static_cast<uint32_t>(bytes[*offset + 2]) << 16) |
        (static_cast<uint32_t>(bytes[*offset + 3]) << 24);
    *offset += 4;
    return value;
}

StatusOr<uInt> ToZlibSize(size_t value, const char* field_name) {
    if (value > static_cast<size_t>(std::numeric_limits<uInt>::max())) {
        return Status::SerializationError(
            std::string("zlib size overflow for ") + field_name);
    }
    return static_cast<uInt>(value);
}

StatusOr<uint32_t> ToZipSize(size_t value, const char* field_name) {
    if (value > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return Status::SerializationError(
            std::string("zip size overflow for ") + field_name);
    }
    return static_cast<uint32_t>(value);
}

StatusOr<uint32_t> Crc32(const std::vector<uint8_t>& data) {
    auto size_or = ToZlibSize(data.size(), "crc32");
    if (!size_or.ok()) {
        return size_or.status();
    }
    const auto* raw = data.empty() ? Z_NULL : reinterpret_cast<const Bytef*>(data.data());
    const auto crc = ::crc32(0L, raw, size_or.value());
    if (crc > static_cast<uLong>(std::numeric_limits<uint32_t>::max())) {
        return Status::SerializationError("crc32 overflow");
    }
    return static_cast<uint32_t>(crc);
}

StatusOr<std::vector<uint8_t>> DeflateRaw(const std::vector<uint8_t>& data) {
    auto input_size_or = ToZlibSize(data.size(), "deflate input");
    if (!input_size_or.ok()) {
        return input_size_or.status();
    }

    const auto bound = compressBound(static_cast<uLong>(data.size()));
    if (bound > static_cast<uLong>(std::numeric_limits<size_t>::max())) {
        return Status::SerializationError("deflate bound overflow");
    }
    std::vector<uint8_t> out(static_cast<size_t>(bound));

    z_stream stream{};
    const int init = deflateInit2(
        &stream,
        Z_BEST_SPEED,
        Z_DEFLATED,
        -MAX_WBITS,
        8,
        Z_DEFAULT_STRATEGY);
    if (init != Z_OK) {
        return Status::SerializationError("deflateInit2 failed");
    }

    stream.next_in = const_cast<Bytef*>(
        data.empty() ? Z_NULL : reinterpret_cast<const Bytef*>(data.data()));
    stream.avail_in = input_size_or.value();
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    auto out_size_or = ToZlibSize(out.size(), "deflate output");
    if (!out_size_or.ok()) {
        deflateEnd(&stream);
        return out_size_or.status();
    }
    stream.avail_out = out_size_or.value();

    const int code = deflate(&stream, Z_FINISH);
    if (code != Z_STREAM_END) {
        deflateEnd(&stream);
        return Status::SerializationError("deflate failed");
    }
    const int end_code = deflateEnd(&stream);
    if (end_code != Z_OK) {
        return Status::SerializationError("deflateEnd failed");
    }

    out.resize(static_cast<size_t>(stream.total_out));
    return out;
}

StatusOr<std::vector<uint8_t>> InflateRaw(
    const std::vector<uint8_t>& compressed, uint32_t uncompressed_size) {
    auto input_size_or = ToZlibSize(compressed.size(), "inflate input");
    if (!input_size_or.ok()) {
        return input_size_or.status();
    }

    std::vector<uint8_t> out;
    if (uncompressed_size == 0) {
        out.resize(1);
    } else {
        out.resize(uncompressed_size);
    }

    z_stream stream{};
    const int init = inflateInit2(&stream, -MAX_WBITS);
    if (init != Z_OK) {
        return Status::SerializationError("inflateInit2 failed");
    }

    stream.next_in = const_cast<Bytef*>(
        compressed.empty() ? Z_NULL : reinterpret_cast<const Bytef*>(compressed.data()));
    stream.avail_in = input_size_or.value();
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    auto output_size_or = ToZlibSize(out.size(), "inflate output");
    if (!output_size_or.ok()) {
        inflateEnd(&stream);
        return output_size_or.status();
    }
    stream.avail_out = output_size_or.value();

    const int code = inflate(&stream, Z_FINISH);
    if (code != Z_STREAM_END) {
        inflateEnd(&stream);
        return Status::SerializationError("inflate failed");
    }
    const int end_code = inflateEnd(&stream);
    if (end_code != Z_OK) {
        return Status::SerializationError("inflateEnd failed");
    }

    if (stream.total_out != uncompressed_size) {
        return Status::SerializationError("inflate size mismatch");
    }
    out.resize(uncompressed_size);
    return out;
}

std::string Trim(std::string text) {
    auto not_space = [](char c) { return !std::isspace(static_cast<unsigned char>(c)); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

std::string Unquote(std::string text) {
    text = Trim(std::move(text));
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

Status WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return Status::SerializationError("failed opening output file: " + path.string());
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out.good()) {
        return Status::SerializationError("failed writing output file: " + path.string());
    }
    return Status::Ok();
}

StatusOr<std::vector<uint8_t>> ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return Status::NotFound("file not found: " + path.string());
    }
    return std::vector<uint8_t>(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string RenderManifest(const TlbsManifest& manifest) {
    std::ostringstream out;
    out << "name = \"" << manifest.name << "\"\n";
    out << "version = \"" << manifest.version.ToString() << "\"\n";
    if (manifest.entry_module.has_value()) {
        out << "entry_module = \"" << *manifest.entry_module << "\"\n";
    }
    out << "modules = [";
    for (size_t i = 0; i < manifest.modules.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << "\"" << manifest.modules[i] << "\"";
    }
    out << "]\n";
    if (!manifest.metadata.empty()) {
        out << "[metadata]\n";
        for (const auto& [k, v] : manifest.metadata) {
            out << k << " = \"" << v << "\"\n";
        }
    }
    return out.str();
}

StatusOr<TlbsManifest> ParseManifest(const std::string& text) {
    TlbsManifest manifest;
    bool in_metadata = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        auto comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }
        line = Trim(std::move(line));
        if (line.empty()) {
            continue;
        }
        if (line == "[metadata]") {
            in_metadata = true;
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            return Status::SerializationError("invalid manifest line: " + line);
        }
        auto key = Trim(line.substr(0, eq));
        auto value = Trim(line.substr(eq + 1));
        if (in_metadata) {
            manifest.metadata[key] = Unquote(value);
            continue;
        }
        if (key == "name") {
            manifest.name = Unquote(value);
            continue;
        }
        if (key == "version") {
            auto version_text = Unquote(value);
            SemanticVersion parsed{};
#if defined(_WIN32)
            if (sscanf_s(
                    version_text.c_str(), "%u.%u.%u", &parsed.major, &parsed.minor,
                    &parsed.patch) != 3) {
#else
            if (std::sscanf(
                    version_text.c_str(), "%u.%u.%u", &parsed.major, &parsed.minor,
                    &parsed.patch) != 3) {
#endif
                return Status::SerializationError("invalid semantic version in manifest");
            }
            manifest.version = parsed;
            continue;
        }
        if (key == "entry_module") {
            manifest.entry_module = Unquote(value);
            continue;
        }
        if (key == "modules") {
            if (value.size() < 2 || value.front() != '[' || value.back() != ']') {
                return Status::SerializationError("manifest modules must be an array");
            }
            value = value.substr(1, value.size() - 2);
            std::stringstream values(value);
            std::string item;
            while (std::getline(values, item, ',')) {
                item = Unquote(item);
                if (!item.empty()) {
                    manifest.modules.push_back(item);
                }
            }
            continue;
        }
        return Status::SerializationError("unknown manifest key: " + key);
    }
    if (manifest.name.empty()) {
        return Status::SerializationError("manifest name is required");
    }
    if (manifest.entry_module.has_value()) {
        if (manifest.entry_module->empty()) {
            return Status::SerializationError("manifest entry_module cannot be empty");
        }
        if (std::find(
                manifest.modules.begin(),
                manifest.modules.end(),
                *manifest.entry_module) == manifest.modules.end()) {
            return Status::SerializationError("manifest entry_module not found in modules");
        }
    }
    return manifest;
}

struct ZipWriteEntry {
    std::string name;
    std::vector<uint8_t> data;
    uint16_t method = kZipStoreMethod;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint32_t crc = 0;
    uint32_t local_offset = 0;
};

Status WriteZip(
    const std::filesystem::path& path, std::vector<ZipWriteEntry> entries) {
    if (entries.size() > static_cast<size_t>(std::numeric_limits<uint16_t>::max())) {
        return Status::SerializationError("zip entry count exceeds 16-bit limit");
    }
    std::vector<uint8_t> out;
    out.reserve(4096);

    for (auto& entry : entries) {
        if (entry.name.size() > static_cast<size_t>(std::numeric_limits<uint16_t>::max())) {
            return Status::SerializationError("zip entry name too long");
        }
        auto crc_or = Crc32(entry.data);
        if (!crc_or.ok()) {
            return crc_or.status();
        }
        entry.crc = crc_or.value();
        auto uncompressed_size_or = ToZipSize(entry.data.size(), "uncompressed entry");
        if (!uncompressed_size_or.ok()) {
            return uncompressed_size_or.status();
        }
        entry.uncompressed_size = uncompressed_size_or.value();
        auto compressed_or = DeflateRaw(entry.data);
        if (!compressed_or.ok()) {
            return compressed_or.status();
        }
        if (compressed_or.value().size() < entry.data.size()) {
            entry.data = std::move(compressed_or.value());
            entry.method = kZipDeflateMethod;
        } else {
            entry.method = kZipStoreMethod;
        }
        auto compressed_size_or = ToZipSize(entry.data.size(), "compressed entry");
        if (!compressed_size_or.ok()) {
            return compressed_size_or.status();
        }
        entry.compressed_size = compressed_size_or.value();
        auto local_offset_or = ToZipSize(out.size(), "local header offset");
        if (!local_offset_or.ok()) {
            return local_offset_or.status();
        }
        entry.local_offset = local_offset_or.value();
        WriteU32(out, kZipLocalHeader);
        WriteU16(out, kZipVersion);
        WriteU16(out, 0);
        WriteU16(out, entry.method);
        WriteU16(out, 0);
        WriteU16(out, 0);
        WriteU32(out, entry.crc);
        WriteU32(out, entry.compressed_size);
        WriteU32(out, entry.uncompressed_size);
        WriteU16(out, static_cast<uint16_t>(entry.name.size()));
        WriteU16(out, 0);
        out.insert(out.end(), entry.name.begin(), entry.name.end());
        out.insert(out.end(), entry.data.begin(), entry.data.end());
    }

    auto central_offset_or = ToZipSize(out.size(), "central directory offset");
    if (!central_offset_or.ok()) {
        return central_offset_or.status();
    }
    const uint32_t central_offset = central_offset_or.value();
    for (const auto& entry : entries) {
        WriteU32(out, kZipCentralHeader);
        WriteU16(out, kZipVersion);
        WriteU16(out, kZipVersion);
        WriteU16(out, 0);
        WriteU16(out, entry.method);
        WriteU16(out, 0);
        WriteU16(out, 0);
        WriteU32(out, entry.crc);
        WriteU32(out, entry.compressed_size);
        WriteU32(out, entry.uncompressed_size);
        WriteU16(out, static_cast<uint16_t>(entry.name.size()));
        WriteU16(out, 0);
        WriteU16(out, 0);
        WriteU16(out, 0);
        WriteU16(out, 0);
        WriteU32(out, 0);
        WriteU32(out, entry.local_offset);
        out.insert(out.end(), entry.name.begin(), entry.name.end());
    }
    auto central_end_or = ToZipSize(out.size(), "central directory end");
    if (!central_end_or.ok()) {
        return central_end_or.status();
    }
    const uint32_t central_size = central_end_or.value() - central_offset;

    WriteU32(out, kZipEndHeader);
    WriteU16(out, 0);
    WriteU16(out, 0);
    WriteU16(out, static_cast<uint16_t>(entries.size()));
    WriteU16(out, static_cast<uint16_t>(entries.size()));
    WriteU32(out, central_size);
    WriteU32(out, central_offset);
    WriteU16(out, 0);

    std::ofstream zip(path, std::ios::binary | std::ios::trunc);
    if (!zip.is_open()) {
        return Status::SerializationError("failed opening zip output: " + path.string());
    }
    zip.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!zip.good()) {
        return Status::SerializationError("failed writing zip output: " + path.string());
    }
    return Status::Ok();
}

StatusOr<std::unordered_map<std::string, std::vector<uint8_t>>> ReadZip(
    const std::filesystem::path& path) {
    auto zip_or = ReadFile(path);
    if (!zip_or.ok()) {
        return zip_or.status();
    }
    const auto& bytes = zip_or.value();
    if (bytes.size() < 22) {
        return Status::SerializationError("invalid zip: too small");
    }

    size_t end_offset = std::string::npos;
    for (size_t pos = bytes.size() - 22; pos != static_cast<size_t>(-1); --pos) {
        if (pos + 4 <= bytes.size() &&
            bytes[pos] == 0x50 && bytes[pos + 1] == 0x4B &&
            bytes[pos + 2] == 0x05 && bytes[pos + 3] == 0x06) {
            end_offset = pos;
            break;
        }
        if (pos == 0) {
            break;
        }
    }
    if (end_offset == std::string::npos) {
        return Status::SerializationError("invalid zip: missing end record");
    }

    size_t cursor = end_offset + 4;
    auto disk_or = ReadU16(bytes, &cursor);
    auto disk_start_or = ReadU16(bytes, &cursor);
    auto entries_disk_or = ReadU16(bytes, &cursor);
    auto entries_total_or = ReadU16(bytes, &cursor);
    auto central_size_or = ReadU32(bytes, &cursor);
    auto central_offset_or = ReadU32(bytes, &cursor);
    auto comment_len_or = ReadU16(bytes, &cursor);
    if (!disk_or.ok() || !disk_start_or.ok() || !entries_disk_or.ok() || !entries_total_or.ok() ||
        !central_size_or.ok() || !central_offset_or.ok() || !comment_len_or.ok()) {
        return Status::SerializationError("invalid zip end record");
    }
    if (comment_len_or.value() != 0) {
        return Status::Unsupported("zip comments are not supported");
    }
    const size_t central_offset = static_cast<size_t>(central_offset_or.value());
    const size_t central_size = static_cast<size_t>(central_size_or.value());
    if (central_offset > bytes.size() || central_size > (bytes.size() - central_offset)) {
        return Status::SerializationError("invalid zip central directory bounds");
    }

    std::unordered_map<std::string, std::vector<uint8_t>> files;
    cursor = central_offset_or.value();
    for (uint32_t i = 0; i < entries_total_or.value(); ++i) {
        auto sig_or = ReadU32(bytes, &cursor);
        if (!sig_or.ok() || sig_or.value() != kZipCentralHeader) {
            return Status::SerializationError("invalid zip central header");
        }
        auto made_or = ReadU16(bytes, &cursor);
        auto need_or = ReadU16(bytes, &cursor);
        auto flags_or = ReadU16(bytes, &cursor);
        auto method_or = ReadU16(bytes, &cursor);
        auto time_or = ReadU16(bytes, &cursor);
        auto date_or = ReadU16(bytes, &cursor);
        auto crc_or = ReadU32(bytes, &cursor);
        auto comp_or = ReadU32(bytes, &cursor);
        auto uncomp_or = ReadU32(bytes, &cursor);
        auto name_len_or = ReadU16(bytes, &cursor);
        auto extra_len_or = ReadU16(bytes, &cursor);
        auto comment_len_or2 = ReadU16(bytes, &cursor);
        auto disk_no_or = ReadU16(bytes, &cursor);
        auto int_attr_or = ReadU16(bytes, &cursor);
        auto ext_attr_or = ReadU32(bytes, &cursor);
        auto local_offset_or = ReadU32(bytes, &cursor);
        if (!made_or.ok() || !need_or.ok() || !flags_or.ok() || !method_or.ok() || !time_or.ok() ||
            !date_or.ok() || !crc_or.ok() || !comp_or.ok() || !uncomp_or.ok() || !name_len_or.ok() ||
            !extra_len_or.ok() || !comment_len_or2.ok() || !disk_no_or.ok() || !int_attr_or.ok() ||
            !ext_attr_or.ok() || !local_offset_or.ok()) {
            return Status::SerializationError("invalid zip central record");
        }
        if ((flags_or.value() & kZipFlagDataDescriptor) != 0u) {
            return Status::Unsupported("zip data descriptor is not supported");
        }
        if (method_or.value() != kZipStoreMethod && method_or.value() != kZipDeflateMethod) {
            return Status::Unsupported("zip compression method is not supported");
        }
        if (cursor + name_len_or.value() + extra_len_or.value() + comment_len_or2.value() > bytes.size()) {
            return Status::SerializationError("invalid zip central name range");
        }
        std::string name(
            reinterpret_cast<const char*>(bytes.data() + static_cast<ptrdiff_t>(cursor)),
            name_len_or.value());
        cursor += name_len_or.value() + extra_len_or.value() + comment_len_or2.value();

        size_t local = local_offset_or.value();
        auto local_sig_or = ReadU32(bytes, &local);
        if (!local_sig_or.ok() || local_sig_or.value() != kZipLocalHeader) {
            return Status::SerializationError("invalid zip local header");
        }
        auto local_need_or = ReadU16(bytes, &local);
        auto local_flags_or = ReadU16(bytes, &local);
        auto local_method_or = ReadU16(bytes, &local);
        auto local_time_or = ReadU16(bytes, &local);
        auto local_date_or = ReadU16(bytes, &local);
        auto local_crc_or = ReadU32(bytes, &local);
        auto local_comp_or = ReadU32(bytes, &local);
        auto local_uncomp_or = ReadU32(bytes, &local);
        auto local_name_len_or = ReadU16(bytes, &local);
        auto local_extra_len_or = ReadU16(bytes, &local);
        if (!local_need_or.ok() || !local_flags_or.ok() || !local_method_or.ok() ||
            !local_time_or.ok() || !local_date_or.ok() || !local_crc_or.ok() ||
            !local_comp_or.ok() || !local_uncomp_or.ok() || !local_name_len_or.ok() ||
            !local_extra_len_or.ok()) {
            return Status::SerializationError("invalid zip local record");
        }
        if ((local_flags_or.value() & kZipFlagDataDescriptor) != 0u) {
            return Status::Unsupported("zip local data descriptor is not supported");
        }
        if (local_method_or.value() != method_or.value()) {
            return Status::SerializationError("zip local/central method mismatch");
        }
        if (local_comp_or.value() != comp_or.value() || local_uncomp_or.value() != uncomp_or.value()) {
            return Status::SerializationError("zip local/central size mismatch");
        }
        local += local_name_len_or.value() + local_extra_len_or.value();
        if (local + local_comp_or.value() > bytes.size()) {
            return Status::SerializationError("invalid zip local payload range");
        }
        std::vector<uint8_t> compressed(
            bytes.begin() + static_cast<ptrdiff_t>(local),
            bytes.begin() + static_cast<ptrdiff_t>(local + local_comp_or.value()));
        std::vector<uint8_t> payload;
        if (method_or.value() == kZipStoreMethod) {
            if (local_comp_or.value() != local_uncomp_or.value()) {
                return Status::SerializationError("store entry has mismatched sizes");
            }
            payload = std::move(compressed);
        } else {
            auto inflated_or = InflateRaw(compressed, local_uncomp_or.value());
            if (!inflated_or.ok()) {
                return inflated_or.status();
            }
            payload = std::move(inflated_or.value());
        }
        auto payload_crc_or = Crc32(payload);
        if (!payload_crc_or.ok()) {
            return payload_crc_or.status();
        }
        if (payload_crc_or.value() != local_crc_or.value() || payload_crc_or.value() != crc_or.value()) {
            return Status::SerializationError("zip crc mismatch");
        }
        files.insert({std::move(name), std::move(payload)});
    }

    return files;
}

}  // namespace

void TlbsBundle::SetModule(std::string relative_path, std::vector<uint8_t> bytes) {
    modules_.insert_or_assign(std::move(relative_path), std::move(bytes));
}

void TlbsBundle::SetLibrary(std::string relative_path, std::vector<uint8_t> bytes) {
    libraries_.insert_or_assign(std::move(relative_path), std::move(bytes));
}

Status TlbsBundle::SerializeToDirectory(const std::filesystem::path& root) const {
    std::filesystem::create_directories(root);
    const auto manifest_text = RenderManifest(manifest_);
    std::vector<uint8_t> manifest_bytes(manifest_text.begin(), manifest_text.end());
    auto status = WriteFile(root / "manifest.toml", manifest_bytes);
    if (!status.ok()) {
        return status;
    }
    for (const auto& [path, bytes] : modules_) {
        status = WriteFile(root / path, bytes);
        if (!status.ok()) {
            return status;
        }
    }
    for (const auto& [path, bytes] : libraries_) {
        status = WriteFile(root / path, bytes);
        if (!status.ok()) {
            return status;
        }
    }
    return Status::Ok();
}

Status TlbsBundle::SerializeToZip(const std::filesystem::path& zip_path) const {
    std::vector<ZipWriteEntry> entries;
    const auto manifest_text = RenderManifest(manifest_);
    entries.push_back(ZipWriteEntry{
        "manifest.toml",
        std::vector<uint8_t>(manifest_text.begin(), manifest_text.end())});
    for (const auto& [path, bytes] : modules_) {
        entries.push_back(ZipWriteEntry{path, bytes});
    }
    for (const auto& [path, bytes] : libraries_) {
        entries.push_back(ZipWriteEntry{path, bytes});
    }
    return WriteZip(zip_path, std::move(entries));
}

StatusOr<TlbsBundle> TlbsBundle::Deserialize(const std::filesystem::path& path) {
    TlbsBundle bundle;
    if (std::filesystem::is_directory(path)) {
        auto manifest_bytes_or = ReadFile(path / "manifest.toml");
        if (!manifest_bytes_or.ok()) {
            return manifest_bytes_or.status();
        }
        const std::string manifest_text(
            manifest_bytes_or.value().begin(), manifest_bytes_or.value().end());
        auto manifest_or = ParseManifest(manifest_text);
        if (!manifest_or.ok()) {
            return manifest_or.status();
        }
        bundle.manifest_ = std::move(manifest_or.value());
        for (const auto& module_path : bundle.manifest_.modules) {
            auto bytes_or = ReadFile(path / module_path);
            if (!bytes_or.ok()) {
                return bytes_or.status();
            }
            bundle.SetModule(module_path, std::move(bytes_or.value()));
        }
        const auto libs_root = path / "libs";
        if (std::filesystem::exists(libs_root)) {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(libs_root)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                auto bytes_or = ReadFile(entry.path());
                if (!bytes_or.ok()) {
                    return bytes_or.status();
                }
                auto rel = std::filesystem::relative(entry.path(), path).generic_string();
                bundle.SetLibrary(std::move(rel), std::move(bytes_or.value()));
            }
        }
        return bundle;
    }

    auto zip_or = ReadZip(path);
    if (!zip_or.ok()) {
        return zip_or.status();
    }
    const auto& files = zip_or.value();
    auto manifest_it = files.find("manifest.toml");
    if (manifest_it == files.end()) {
        return Status::SerializationError("tlbs zip missing manifest.toml");
    }
    const std::string manifest_text(manifest_it->second.begin(), manifest_it->second.end());
    auto manifest_or = ParseManifest(manifest_text);
    if (!manifest_or.ok()) {
        return manifest_or.status();
    }
    bundle.manifest_ = std::move(manifest_or.value());
    for (const auto& module_path : bundle.manifest_.modules) {
        auto it = files.find(module_path);
        if (it == files.end()) {
            return Status::NotFound("module missing from tlbs zip: " + module_path);
        }
        bundle.SetModule(module_path, it->second);
    }
    for (const auto& [name, bytes] : files) {
        if (name.rfind("libs/", 0) == 0) {
            bundle.SetLibrary(name, bytes);
        }
    }
    return bundle;
}

std::string CurrentPlatformName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string CurrentArchName() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#else
    return "unknown";
#endif
}

}  // namespace tie::vm
