#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tie/vm/common/status.hpp"
#include "tie/vm/runtime/value.hpp"

namespace tie::vm {

class VmThread;

enum class CallingConvention : uint8_t {
    kSystem = 0,
    kCdecl = 1,
    kStdcall = 2,
    kFastcall = 3,
};

enum class OwnershipQualifier : uint8_t {
    kBorrowed = 0,
    kOwned = 1,
    kPinned = 2,
};

enum class AbiValueKind : uint8_t {
    kVoid = 0,
    kI64 = 1,
    kF64 = 2,
    kBool = 3,
    kPointer = 4,
    kUtf8 = 5,
    kObject = 6,
};

struct AbiType {
    AbiValueKind kind = AbiValueKind::kVoid;
    OwnershipQualifier ownership = OwnershipQualifier::kBorrowed;
};

struct FunctionSignature {
    std::string name;
    CallingConvention convention = CallingConvention::kSystem;
    AbiType return_type;
    std::vector<AbiType> params;
};

class FfiBridge {
  public:
    using NativeFunction = std::function<StatusOr<Value>(VmThread&, const std::vector<Value>&)>;
    using VmCallback = std::function<StatusOr<Value>(const std::vector<Value>&)>;

    Status RegisterFunction(FunctionSignature signature, NativeFunction fn);
    [[nodiscard]] StatusOr<Value> CallFunction(
        std::string_view name, VmThread& vm_thread, const std::vector<Value>& args) const;

    Status RegisterVmCallback(std::string name, VmCallback callback);
    [[nodiscard]] StatusOr<Value> InvokeVmCallback(
        std::string_view name, const std::vector<Value>& args) const;

    Status AttachCurrentThread();
    Status DetachCurrentThread();
    [[nodiscard]] bool IsCurrentThreadAttached() const;

  private:
    struct Entry {
        FunctionSignature signature;
        NativeFunction function;
    };

    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> native_functions_;
    std::unordered_map<std::string, VmCallback> callbacks_;
    std::unordered_set<std::thread::id> attached_threads_;
};

}  // namespace tie::vm

