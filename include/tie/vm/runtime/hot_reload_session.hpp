#pragma once

#include <vector>

#include "tie/vm/runtime/module_loader.hpp"

namespace tie::vm {

class HotReloadSession {
  public:
    explicit HotReloadSession(ModuleLoader* loader) : loader_(loader) {}

    void Stage(LoadedModule module);
    [[nodiscard]] Status Commit();

  private:
    ModuleLoader* loader_;
    std::vector<LoadedModule> staged_;
};

}  // namespace tie::vm

