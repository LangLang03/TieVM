#include "tie/vm/gc/gc_controller.hpp"

#include <queue>

namespace tie::vm {

GcController::GcController() : GcController(Config{}) {}

GcController::GcController(Config config) : config_(config) {
    major_worker_ = std::thread(&GcController::RunMajorWorker, this);
}

GcController::~GcController() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_.store(true);
        cv_.notify_all();
    }
    if (major_worker_.joinable()) {
        major_worker_.join();
    }
}

StatusOr<ObjectId> GcController::Allocate() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        const ObjectId id = next_id_++;
        Cell cell;
        cell.id = id;
        cell.generation = 0;
        heap_.insert({id, std::move(cell)});
        if (heap_.size() > config_.old_threshold) {
            major_requested_ = true;
            cv_.notify_one();
        }
        return id;
    }
}

Status GcController::AddReference(ObjectId from, ObjectId to) {
    std::lock_guard<std::mutex> lock(mu_);
    auto from_it = heap_.find(from);
    auto to_it = heap_.find(to);
    if (from_it == heap_.end() || to_it == heap_.end()) {
        return Status::NotFound("gc object not found for edge");
    }
    from_it->second.edges.insert(to);
    return Status::Ok();
}

Status GcController::RemoveReference(ObjectId from, ObjectId to) {
    std::lock_guard<std::mutex> lock(mu_);
    auto from_it = heap_.find(from);
    if (from_it == heap_.end()) {
        return Status::NotFound("gc object not found for edge removal");
    }
    from_it->second.edges.erase(to);
    return Status::Ok();
}

Status GcController::AddRoot(ObjectId id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!heap_.contains(id)) {
        return Status::NotFound("gc object not found for root");
    }
    roots_.insert(id);
    return Status::Ok();
}

Status GcController::RemoveRoot(ObjectId id) {
    std::lock_guard<std::mutex> lock(mu_);
    roots_.erase(id);
    return Status::Ok();
}

Status GcController::Pin(ObjectId id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = heap_.find(id);
    if (it == heap_.end()) {
        return Status::NotFound("gc object not found for pin");
    }
    it->second.pinned = true;
    return Status::Ok();
}

Status GcController::Unpin(ObjectId id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = heap_.find(id);
    if (it == heap_.end()) {
        return Status::NotFound("gc object not found for unpin");
    }
    it->second.pinned = false;
    return Status::Ok();
}

Status GcController::RegisterFinalizer(
    ObjectId id, std::function<void(ObjectId)> finalizer) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = heap_.find(id);
    if (it == heap_.end()) {
        return Status::NotFound("gc object not found for finalizer");
    }
    it->second.finalizer = std::move(finalizer);
    return Status::Ok();
}

StatusOr<uint64_t> GcController::CreateWeakRef(ObjectId id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!heap_.contains(id)) {
        return Status::NotFound("gc object not found for weak ref");
    }
    const uint64_t ref_id = next_ref_id_++;
    weak_refs_[ref_id] = id;
    return ref_id;
}

StatusOr<uint64_t> GcController::CreatePhantomRef(ObjectId id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!heap_.contains(id)) {
        return Status::NotFound("gc object not found for phantom ref");
    }
    const uint64_t ref_id = next_ref_id_++;
    phantom_refs_[ref_id] = id;
    return ref_id;
}

std::optional<ObjectId> GcController::ResolveWeakRef(uint64_t ref_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = weak_refs_.find(ref_id);
    if (it == weak_refs_.end()) {
        return std::nullopt;
    }
    if (!heap_.contains(it->second)) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<ObjectId> GcController::DrainPhantomQueue() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ObjectId> out = std::move(phantom_queue_);
    phantom_queue_.clear();
    return out;
}

void GcController::MarkFromRoots(std::unordered_set<ObjectId>* marked) {
    std::queue<ObjectId> q;
    for (auto root : roots_) {
        if (heap_.contains(root)) {
            q.push(root);
        }
    }
    for (const auto& [id, cell] : heap_) {
        if (cell.pinned) {
            q.push(id);
        }
    }

    while (!q.empty()) {
        const ObjectId id = q.front();
        q.pop();
        if (marked->contains(id)) {
            continue;
        }
        marked->insert(id);
        auto it = heap_.find(id);
        if (it == heap_.end()) {
            continue;
        }
        for (const auto edge : it->second.edges) {
            if (heap_.contains(edge) && !marked->contains(edge)) {
                q.push(edge);
            }
        }
    }
}

void GcController::Sweep(bool young_only) {
    std::vector<std::function<void()>> finalizers;
    std::unordered_set<ObjectId> reclaimed;
    for (auto it = heap_.begin(); it != heap_.end();) {
        auto& cell = it->second;
        if (young_only && cell.generation > 0) {
            cell.marked = false;
            ++it;
            continue;
        }
        if (!cell.marked && !cell.pinned) {
            if (cell.finalizer) {
                const ObjectId id = cell.id;
                auto fn = cell.finalizer;
                finalizers.emplace_back([id, fn = std::move(fn)]() { fn(id); });
            }
            reclaimed.insert(cell.id);
            it = heap_.erase(it);
        } else {
            cell.marked = false;
            ++it;
        }
    }

    for (auto it = weak_refs_.begin(); it != weak_refs_.end();) {
        if (reclaimed.contains(it->second)) {
            it = weak_refs_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = phantom_refs_.begin(); it != phantom_refs_.end();) {
        if (reclaimed.contains(it->second)) {
            phantom_queue_.push_back(it->second);
            it = phantom_refs_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto id : reclaimed) {
        roots_.erase(id);
    }

    for (auto& fn : finalizers) {
        fn();
    }
}

void GcController::PromoteYoung() {
    for (auto& [_, cell] : heap_) {
        if (cell.marked && cell.generation == 0) {
            cell.generation = 1;
        }
    }
}

Status GcController::CollectMinor() {
    std::lock_guard<std::mutex> lock(mu_);
    std::unordered_set<ObjectId> marked;
    MarkFromRoots(&marked);
    for (auto id : marked) {
        auto it = heap_.find(id);
        if (it != heap_.end()) {
            it->second.marked = true;
        }
    }
    PromoteYoung();
    Sweep(true);
    return Status::Ok();
}

Status GcController::CollectMajorAsync() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        major_requested_ = true;
    }
    cv_.notify_one();
    return Status::Ok();
}

Status GcController::WaitForMajor() {
    std::unique_lock<std::mutex> lock(mu_);
    major_done_cv_.wait(lock, [&]() { return !major_running_ && !major_requested_; });
    return Status::Ok();
}

size_t GcController::LiveObjects() const {
    std::lock_guard<std::mutex> lock(mu_);
    return heap_.size();
}

void GcController::RunMajorWorker() {
    while (true) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&]() { return stop_.load() || major_requested_; });
        if (stop_.load()) {
            return;
        }
        major_requested_ = false;
        major_running_ = true;
        std::unordered_set<ObjectId> marked;
        MarkFromRoots(&marked);
        for (auto id : marked) {
            auto it = heap_.find(id);
            if (it != heap_.end()) {
                it->second.marked = true;
            }
        }
        Sweep(false);
        major_running_ = false;
        lock.unlock();
        major_done_cv_.notify_all();
    }
}

}  // namespace tie::vm
