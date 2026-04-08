#include "tie/vm/runtime/hot_reload_session.hpp"

namespace tie::vm {

void HotReloadSession::Stage(LoadedModule module) { staged_.push_back(std::move(module)); }

Status HotReloadSession::Commit() { return loader_->CommitHotReload(std::move(staged_)); }

}  // namespace tie::vm

