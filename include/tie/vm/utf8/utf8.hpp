#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "tie/vm/common/status.hpp"

namespace tie::vm::utf8 {

[[nodiscard]] Status Validate(std::string_view text);
[[nodiscard]] StatusOr<size_t> CountCodePoints(std::string_view text);
[[nodiscard]] StatusOr<std::vector<uint32_t>> DecodeCodePoints(std::string_view text);

}  // namespace tie::vm::utf8

