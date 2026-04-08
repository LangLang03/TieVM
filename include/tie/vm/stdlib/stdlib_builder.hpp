#pragma once

#include <filesystem>

#include "tie/vm/common/status.hpp"
#include "tie/vm/tlb/tlb_container.hpp"

namespace tie::vm {

[[nodiscard]] StatusOr<TlbContainer> BuildStdlibContainer();
[[nodiscard]] Status BuildStdlibTlb(const std::filesystem::path& path);

}  // namespace tie::vm

