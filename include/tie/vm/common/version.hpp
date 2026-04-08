#pragma once

#include <cstdint>
#include <string>

namespace tie::vm {

struct SemanticVersion {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;

    [[nodiscard]] std::string ToString() const;
};

}  // namespace tie::vm

