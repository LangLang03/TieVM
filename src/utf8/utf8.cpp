#include "tie/vm/utf8/utf8.hpp"

namespace tie::vm::utf8 {

namespace {

StatusOr<uint32_t> DecodeOne(std::string_view text, size_t* index) {
    if (*index >= text.size()) {
        return Status::InvalidArgument("decode index out of range");
    }
    const auto b0 = static_cast<uint8_t>(text[*index]);
    if (b0 <= 0x7F) {
        (*index) += 1;
        return b0;
    }

    auto need_cont = [&](size_t n) -> bool { return *index + n < text.size(); };
    auto cont = [&](size_t i) -> uint8_t { return static_cast<uint8_t>(text[*index + i]); };

    if ((b0 & 0xE0u) == 0xC0u) {
        if (!need_cont(1)) {
            return Status::InvalidArgument("truncated utf8 2-byte sequence");
        }
        const uint8_t b1 = cont(1);
        if ((b1 & 0xC0u) != 0x80u) {
            return Status::InvalidArgument("invalid continuation byte");
        }
        const uint32_t cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
        if (cp < 0x80) {
            return Status::InvalidArgument("overlong utf8 sequence");
        }
        (*index) += 2;
        return cp;
    }

    if ((b0 & 0xF0u) == 0xE0u) {
        if (!need_cont(2)) {
            return Status::InvalidArgument("truncated utf8 3-byte sequence");
        }
        const uint8_t b1 = cont(1);
        const uint8_t b2 = cont(2);
        if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) {
            return Status::InvalidArgument("invalid continuation byte");
        }
        const uint32_t cp =
            ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
        if (cp < 0x800) {
            return Status::InvalidArgument("overlong utf8 sequence");
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return Status::InvalidArgument("utf16 surrogate not allowed in utf8");
        }
        (*index) += 3;
        return cp;
    }

    if ((b0 & 0xF8u) == 0xF0u) {
        if (!need_cont(3)) {
            return Status::InvalidArgument("truncated utf8 4-byte sequence");
        }
        const uint8_t b1 = cont(1);
        const uint8_t b2 = cont(2);
        const uint8_t b3 = cont(3);
        if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u ||
            (b3 & 0xC0u) != 0x80u) {
            return Status::InvalidArgument("invalid continuation byte");
        }
        const uint32_t cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) |
                            ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
        if (cp < 0x10000 || cp > 0x10FFFF) {
            return Status::InvalidArgument("invalid utf8 scalar");
        }
        (*index) += 4;
        return cp;
    }

    return Status::InvalidArgument("invalid utf8 leading byte");
}

}  // namespace

Status Validate(std::string_view text) {
    size_t index = 0;
    while (index < text.size()) {
        auto cp_or = DecodeOne(text, &index);
        if (!cp_or.ok()) {
            return cp_or.status();
        }
    }
    return Status::Ok();
}

StatusOr<size_t> CountCodePoints(std::string_view text) {
    size_t index = 0;
    size_t count = 0;
    while (index < text.size()) {
        auto cp_or = DecodeOne(text, &index);
        if (!cp_or.ok()) {
            return cp_or.status();
        }
        ++count;
    }
    return count;
}

StatusOr<std::vector<uint32_t>> DecodeCodePoints(std::string_view text) {
    size_t index = 0;
    std::vector<uint32_t> out;
    while (index < text.size()) {
        auto cp_or = DecodeOne(text, &index);
        if (!cp_or.ok()) {
            return cp_or.status();
        }
        out.push_back(cp_or.value());
    }
    return out;
}

}  // namespace tie::vm::utf8

