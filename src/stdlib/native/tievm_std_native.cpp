#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define TIEVM_STD_EXPORT extern "C" __declspec(dllexport)
#else
#define TIEVM_STD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

thread_local std::string g_tievm_std_tls_text;

struct TieArray {
    std::vector<int64_t> data;
};

struct TieMap {
    std::unordered_map<std::string, int64_t> data;
};

std::string SliceUtf8ByBytes(const char* text, int64_t start, int64_t len) {
    if (text == nullptr || start < 0 || len < 0) {
        return {};
    }
    const std::string source(text);
    const auto begin = static_cast<size_t>(start);
    if (begin >= source.size()) {
        return {};
    }
    const auto count = static_cast<size_t>(len);
    return source.substr(begin, count);
}

bool IsIpv4(const char* text) {
    if (text == nullptr) {
        return false;
    }
    std::istringstream stream(text);
    std::string part;
    int count = 0;
    while (std::getline(stream, part, '.')) {
        if (part.empty() || part.size() > 3) {
            return false;
        }
        for (const char ch : part) {
            if (ch < '0' || ch > '9') {
                return false;
            }
        }
        const int value = std::stoi(part);
        if (value < 0 || value > 255) {
            return false;
        }
        ++count;
    }
    return count == 4;
}

bool IsIpv6(const char* text) {
    if (text == nullptr) {
        return false;
    }
    std::string s(text);
    if (s.empty()) {
        return false;
    }
    int groups = 0;
    bool compressed = false;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == ':') {
            if (i + 1 < s.size() && s[i + 1] == ':') {
                if (compressed) {
                    return false;
                }
                compressed = true;
                i += 2;
                ++groups;
                continue;
            }
            return false;
        }
        size_t j = i;
        while (j < s.size() && s[j] != ':') {
            const char ch = s[j];
            const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                             (ch >= 'A' && ch <= 'F');
            if (!hex) {
                return false;
            }
            ++j;
        }
        const size_t len = j - i;
        if (len == 0 || len > 4) {
            return false;
        }
        ++groups;
        if (j >= s.size()) {
            i = j;
            break;
        }
        i = j + 1;
    }
    if (compressed) {
        return groups <= 8;
    }
    return groups == 8;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    if (prefix.size() > text.size()) {
        return false;
    }
    return std::equal(prefix.begin(), prefix.end(), text.begin());
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    if (suffix.size() > text.size()) {
        return false;
    }
    return std::equal(suffix.rbegin(), suffix.rend(), text.rbegin());
}

std::string ToLowerAscii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string ToUpperAscii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return text;
}

int64_t PowInt64(int64_t base, int64_t exp) {
    if (exp < 0) {
        return 0;
    }
    int64_t result = 1;
    int64_t b = base;
    int64_t e = exp;
    while (e > 0) {
        if ((e & 1) != 0) {
            result *= b;
        }
        e >>= 1;
        if (e > 0) {
            b *= b;
        }
    }
    return result;
}

int64_t GcdInt64(int64_t a, int64_t b) {
    if (a < 0) {
        a = -a;
    }
    if (b < 0) {
        b = -b;
    }
    while (b != 0) {
        const int64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

}  // namespace

TIEVM_STD_EXPORT void tie_std_io_print(const char* text) {
    if (text == nullptr) {
        std::cout << "null\n";
        return;
    }
    std::cout << text << "\n";
}

TIEVM_STD_EXPORT const char* tie_std_io_read_text(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return nullptr;
    }
    g_tievm_std_tls_text.assign(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return g_tievm_std_tls_text.c_str();
}

TIEVM_STD_EXPORT bool tie_std_io_write_text(const char* path, const char* text) {
    if (path == nullptr || text == nullptr) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(text, static_cast<std::streamsize>(std::strlen(text)));
    return out.good();
}

TIEVM_STD_EXPORT bool tie_std_io_exists(const char* path) {
    if (path == nullptr) {
        return false;
    }
    return std::filesystem::exists(std::filesystem::path(path));
}

TIEVM_STD_EXPORT bool tie_std_io_append_text(const char* path, const char* text) {
    if (path == nullptr || text == nullptr) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        return false;
    }
    out.write(text, static_cast<std::streamsize>(std::strlen(text)));
    return out.good();
}

TIEVM_STD_EXPORT uint64_t tie_std_collections_array_new() {
    auto* array = new TieArray();
    return reinterpret_cast<uint64_t>(array);
}

TIEVM_STD_EXPORT int64_t tie_std_collections_array_push(uint64_t array_handle, int64_t value) {
    auto* array = reinterpret_cast<TieArray*>(array_handle);
    if (array == nullptr) {
        return -1;
    }
    array->data.push_back(value);
    return static_cast<int64_t>(array->data.size());
}

TIEVM_STD_EXPORT int64_t tie_std_collections_array_get(uint64_t array_handle, int64_t index) {
    auto* array = reinterpret_cast<TieArray*>(array_handle);
    if (array == nullptr || index < 0 || static_cast<size_t>(index) >= array->data.size()) {
        return 0;
    }
    return array->data[static_cast<size_t>(index)];
}

TIEVM_STD_EXPORT int64_t tie_std_collections_array_size(uint64_t array_handle) {
    auto* array = reinterpret_cast<TieArray*>(array_handle);
    if (array == nullptr) {
        return 0;
    }
    return static_cast<int64_t>(array->data.size());
}

TIEVM_STD_EXPORT int64_t tie_std_collections_array_pop(uint64_t array_handle) {
    auto* array = reinterpret_cast<TieArray*>(array_handle);
    if (array == nullptr || array->data.empty()) {
        return 0;
    }
    const int64_t value = array->data.back();
    array->data.pop_back();
    return value;
}

TIEVM_STD_EXPORT bool tie_std_collections_array_clear(uint64_t array_handle) {
    auto* array = reinterpret_cast<TieArray*>(array_handle);
    if (array == nullptr) {
        return false;
    }
    array->data.clear();
    return true;
}

TIEVM_STD_EXPORT bool tie_std_collections_array_free(uint64_t array_handle) {
    auto* array = reinterpret_cast<TieArray*>(array_handle);
    if (array == nullptr) {
        return false;
    }
    delete array;
    return true;
}

TIEVM_STD_EXPORT uint64_t tie_std_collections_map_new() {
    auto* map = new TieMap();
    return reinterpret_cast<uint64_t>(map);
}

TIEVM_STD_EXPORT bool tie_std_collections_map_set(
    uint64_t map_handle, const char* key, int64_t value) {
    auto* map = reinterpret_cast<TieMap*>(map_handle);
    if (map == nullptr || key == nullptr) {
        return false;
    }
    map->data[std::string(key)] = value;
    return true;
}

TIEVM_STD_EXPORT int64_t tie_std_collections_map_get(uint64_t map_handle, const char* key) {
    auto* map = reinterpret_cast<TieMap*>(map_handle);
    if (map == nullptr || key == nullptr) {
        return 0;
    }
    const auto it = map->data.find(std::string(key));
    if (it == map->data.end()) {
        return 0;
    }
    return it->second;
}

TIEVM_STD_EXPORT bool tie_std_collections_map_has(uint64_t map_handle, const char* key) {
    auto* map = reinterpret_cast<TieMap*>(map_handle);
    if (map == nullptr || key == nullptr) {
        return false;
    }
    return map->data.contains(std::string(key));
}

TIEVM_STD_EXPORT int64_t tie_std_collections_map_size(uint64_t map_handle) {
    auto* map = reinterpret_cast<TieMap*>(map_handle);
    if (map == nullptr) {
        return 0;
    }
    return static_cast<int64_t>(map->data.size());
}

TIEVM_STD_EXPORT bool tie_std_collections_map_remove(uint64_t map_handle, const char* key) {
    auto* map = reinterpret_cast<TieMap*>(map_handle);
    if (map == nullptr || key == nullptr) {
        return false;
    }
    return map->data.erase(std::string(key)) > 0;
}

TIEVM_STD_EXPORT bool tie_std_collections_map_clear(uint64_t map_handle) {
    auto* map = reinterpret_cast<TieMap*>(map_handle);
    if (map == nullptr) {
        return false;
    }
    map->data.clear();
    return true;
}

TIEVM_STD_EXPORT bool tie_std_collections_map_free(uint64_t map_handle) {
    auto* map = reinterpret_cast<TieMap*>(map_handle);
    if (map == nullptr) {
        return false;
    }
    delete map;
    return true;
}

TIEVM_STD_EXPORT const char* tie_std_string_concat(const char* lhs, const char* rhs) {
    g_tievm_std_tls_text = std::string(lhs == nullptr ? "" : lhs) + std::string(rhs == nullptr ? "" : rhs);
    return g_tievm_std_tls_text.c_str();
}

TIEVM_STD_EXPORT int64_t tie_std_string_length(const char* text) {
    if (text == nullptr) {
        return 0;
    }
    return static_cast<int64_t>(std::strlen(text));
}

TIEVM_STD_EXPORT bool tie_std_string_utf8_validate(const char* text) {
    if (text == nullptr) {
        return false;
    }
    const auto* ptr = reinterpret_cast<const unsigned char*>(text);
    while (*ptr != 0) {
        if ((*ptr & 0x80u) == 0) {
            ++ptr;
            continue;
        }
        if ((*ptr & 0xE0u) == 0xC0u) {
            if ((ptr[1] & 0xC0u) != 0x80u) {
                return false;
            }
            ptr += 2;
            continue;
        }
        if ((*ptr & 0xF0u) == 0xE0u) {
            if ((ptr[1] & 0xC0u) != 0x80u || (ptr[2] & 0xC0u) != 0x80u) {
                return false;
            }
            ptr += 3;
            continue;
        }
        if ((*ptr & 0xF8u) == 0xF0u) {
            if ((ptr[1] & 0xC0u) != 0x80u || (ptr[2] & 0xC0u) != 0x80u ||
                (ptr[3] & 0xC0u) != 0x80u) {
                return false;
            }
            ptr += 4;
            continue;
        }
        return false;
    }
    return true;
}

TIEVM_STD_EXPORT int64_t tie_std_string_codepoints(const char* text) {
    if (text == nullptr) {
        return 0;
    }
    int64_t count = 0;
    const auto* ptr = reinterpret_cast<const unsigned char*>(text);
    while (*ptr != 0) {
        if ((*ptr & 0x80u) == 0) {
            ++ptr;
        } else if ((*ptr & 0xE0u) == 0xC0u) {
            ptr += 2;
        } else if ((*ptr & 0xF0u) == 0xE0u) {
            ptr += 3;
        } else if ((*ptr & 0xF8u) == 0xF0u) {
            ptr += 4;
        } else {
            return -1;
        }
        ++count;
    }
    return count;
}

TIEVM_STD_EXPORT const char* tie_std_string_slice(const char* text, int64_t start, int64_t len) {
    g_tievm_std_tls_text = SliceUtf8ByBytes(text, start, len);
    return g_tievm_std_tls_text.c_str();
}

TIEVM_STD_EXPORT bool tie_std_string_starts_with(const char* text, const char* prefix) {
    return StartsWith(
        text == nullptr ? std::string() : std::string(text),
        prefix == nullptr ? std::string() : std::string(prefix));
}

TIEVM_STD_EXPORT bool tie_std_string_ends_with(const char* text, const char* suffix) {
    return EndsWith(
        text == nullptr ? std::string() : std::string(text),
        suffix == nullptr ? std::string() : std::string(suffix));
}

TIEVM_STD_EXPORT int64_t tie_std_string_find(const char* text, const char* needle) {
    if (text == nullptr || needle == nullptr) {
        return -1;
    }
    const std::string haystack(text);
    const std::string target(needle);
    const size_t index = haystack.find(target);
    if (index == std::string::npos) {
        return -1;
    }
    return static_cast<int64_t>(index);
}

TIEVM_STD_EXPORT const char* tie_std_string_replace(
    const char* text, const char* from, const char* to) {
    const std::string source = text == nullptr ? "" : std::string(text);
    const std::string from_s = from == nullptr ? "" : std::string(from);
    const std::string to_s = to == nullptr ? "" : std::string(to);
    if (from_s.empty()) {
        g_tievm_std_tls_text = source;
        return g_tievm_std_tls_text.c_str();
    }
    g_tievm_std_tls_text = source;
    size_t pos = 0;
    while ((pos = g_tievm_std_tls_text.find(from_s, pos)) != std::string::npos) {
        g_tievm_std_tls_text.replace(pos, from_s.size(), to_s);
        pos += to_s.size();
    }
    return g_tievm_std_tls_text.c_str();
}

TIEVM_STD_EXPORT const char* tie_std_string_lower_ascii(const char* text) {
    g_tievm_std_tls_text = ToLowerAscii(text == nullptr ? std::string() : std::string(text));
    return g_tievm_std_tls_text.c_str();
}

TIEVM_STD_EXPORT const char* tie_std_string_upper_ascii(const char* text) {
    g_tievm_std_tls_text = ToUpperAscii(text == nullptr ? std::string() : std::string(text));
    return g_tievm_std_tls_text.c_str();
}

TIEVM_STD_EXPORT void tie_std_concurrent_sleep_ms(int64_t millis) {
    if (millis < 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

TIEVM_STD_EXPORT bool tie_std_net_is_ipv4(const char* text) { return IsIpv4(text); }

TIEVM_STD_EXPORT bool tie_std_net_is_ipv6(const char* text) { return IsIpv6(text); }

TIEVM_STD_EXPORT int64_t tie_std_math_abs(int64_t value) {
    return value < 0 ? -value : value;
}

TIEVM_STD_EXPORT int64_t tie_std_math_min(int64_t lhs, int64_t rhs) {
    return lhs < rhs ? lhs : rhs;
}

TIEVM_STD_EXPORT int64_t tie_std_math_max(int64_t lhs, int64_t rhs) {
    return lhs > rhs ? lhs : rhs;
}

TIEVM_STD_EXPORT int64_t tie_std_math_clamp(int64_t value, int64_t lo, int64_t hi) {
    if (lo > hi) {
        const int64_t t = lo;
        lo = hi;
        hi = t;
    }
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

TIEVM_STD_EXPORT int64_t tie_std_math_pow_i(int64_t base, int64_t exp) {
    return PowInt64(base, exp);
}

TIEVM_STD_EXPORT int64_t tie_std_math_gcd(int64_t a, int64_t b) {
    return GcdInt64(a, b);
}

TIEVM_STD_EXPORT int64_t tie_std_time_now_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

TIEVM_STD_EXPORT int32_t tie_demo_add(int32_t lhs, int32_t rhs) { return lhs + rhs; }

TIEVM_STD_EXPORT void tie_demo_inout_i64(int64_t* value, int64_t delta) {
    if (value == nullptr) {
        return;
    }
    *value += delta;
}

struct TiePair32 {
    int32_t a;
    int32_t b;
};

TIEVM_STD_EXPORT int32_t tie_demo_pair_sum(TiePair32 pair) { return pair.a + pair.b; }
