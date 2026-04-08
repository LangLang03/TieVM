#pragma once

#include <string>
#include <vector>

#include "tie/vm/bytecode/module.hpp"
#include "tie/vm/common/status.hpp"

namespace tie::vm {

struct VerificationResult {
    Status status = Status::Ok();
    std::vector<std::string> warnings;
};

class Verifier {
  public:
    [[nodiscard]] static VerificationResult Verify(const Module& module);
};

}  // namespace tie::vm

