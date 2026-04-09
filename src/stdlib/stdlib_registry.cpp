#include "tie/vm/stdlib/stdlib_registry.hpp"

namespace tie::vm {

Status StdlibRegistry::RegisterCore(VmInstance* vm) {
    (void)vm;
    return Status::Unsupported(
        "builtin stdlib registry removed; load stdlib from .tlbs bundle");
}

}  // namespace tie::vm
