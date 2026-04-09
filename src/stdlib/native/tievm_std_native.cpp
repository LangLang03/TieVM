#include <chrono>
#include <cstring>
#include <cstdint>
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

TIEVM_STD_EXPORT void tie_std_concurrent_sleep_ms(int64_t millis) {
    if (millis < 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

TIEVM_STD_EXPORT bool tie_std_net_is_ipv4(const char* text) { return IsIpv4(text); }

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
