#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tie/vm/common/status.hpp"
#include "tie/vm/runtime/value.hpp"

namespace tie::vm {

class GcController {
  public:
    struct Config {
        size_t young_threshold = 128;
        size_t old_threshold = 1024;
    };

    GcController();
    explicit GcController(Config config);
    ~GcController();

    [[nodiscard]] StatusOr<ObjectId> Allocate();
    Status AddReference(ObjectId from, ObjectId to);
    Status RemoveReference(ObjectId from, ObjectId to);

    Status AddRoot(ObjectId id);
    Status RemoveRoot(ObjectId id);

    Status Pin(ObjectId id);
    Status Unpin(ObjectId id);

    Status RegisterFinalizer(ObjectId id, std::function<void(ObjectId)> finalizer);

    [[nodiscard]] StatusOr<uint64_t> CreateWeakRef(ObjectId id);
    [[nodiscard]] StatusOr<uint64_t> CreatePhantomRef(ObjectId id);
    [[nodiscard]] std::optional<ObjectId> ResolveWeakRef(uint64_t ref_id) const;
    [[nodiscard]] std::vector<ObjectId> DrainPhantomQueue();

    Status CollectMinor();
    Status CollectMajorAsync();
    Status WaitForMajor();

    [[nodiscard]] size_t LiveObjects() const;

  private:
    struct Cell {
        ObjectId id = 0;
        bool marked = false;
        bool pinned = false;
        uint8_t generation = 0;  // 0: young, 1: old
        std::unordered_set<ObjectId> edges;
        std::function<void(ObjectId)> finalizer;
    };

    void RunMajorWorker();
    void MarkFromRoots(std::unordered_set<ObjectId>* marked);
    void Sweep(bool young_only, std::vector<std::function<void()>>* finalizers);
    void PromoteYoung();

    Config config_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable major_done_cv_;
    std::unordered_map<ObjectId, Cell> heap_;
    std::unordered_set<ObjectId> roots_;
    std::unordered_map<uint64_t, ObjectId> weak_refs_;
    std::unordered_map<uint64_t, ObjectId> phantom_refs_;
    std::vector<ObjectId> phantom_queue_;
    ObjectId next_id_ = 1;
    uint64_t next_ref_id_ = 1;
    size_t young_count_ = 0;

    std::thread major_worker_;
    std::atomic<bool> stop_{false};
    bool major_requested_ = false;
    bool major_running_ = false;
};

}  // namespace tie::vm
