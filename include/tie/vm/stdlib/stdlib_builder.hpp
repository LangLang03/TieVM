#pragma once

#include <filesystem>

#include "tie/vm/common/status.hpp"
#include "tie/vm/tlb/tlb_container.hpp"
#include "tie/vm/tlb/tlbs_bundle.hpp"

namespace tie::vm {

[[nodiscard]] StatusOr<TlbContainer> BuildStdlibContainer();
[[nodiscard]] Status BuildStdlibTlb(const std::filesystem::path& path);
[[nodiscard]] StatusOr<TlbsBundle> BuildStdlibBundle();
[[nodiscard]] Status BuildStdlibTlbs(const std::filesystem::path& path);

}  // namespace tie::vm
