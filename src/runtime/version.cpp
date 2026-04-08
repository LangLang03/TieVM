#include "tie/vm/common/version.hpp"

namespace tie::vm {

std::string SemanticVersion::ToString() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(patch);
}

}  // namespace tie::vm

