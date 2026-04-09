#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tie::vm {

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
    kI8 = 1,
    kU8 = 2,
    kI16 = 3,
    kU16 = 4,
    kI32 = 5,
    kU32 = 6,
    kI64 = 7,
    kU64 = 8,
    kF32 = 9,
    kF64 = 10,
    kBool = 11,
    kPointer = 12,
    kUtf8 = 13,
    kStruct = 14,
    kObject = 15,
};

enum class FfiPassingMode : uint8_t {
    kValue = 0,
    kPointerIn = 1,
    kPointerOut = 2,
    kPointerInOut = 3,
};

struct AbiType {
    AbiValueKind kind = AbiValueKind::kVoid;
    OwnershipQualifier ownership = OwnershipQualifier::kBorrowed;
    FfiPassingMode passing = FfiPassingMode::kValue;
    uint32_t struct_index = 0;
    uint32_t size = 0;
};

struct FunctionSignature {
    std::string name;
    CallingConvention convention = CallingConvention::kSystem;
    AbiType return_type;
    std::vector<AbiType> params;
};

struct FfiStructField {
    uint32_t offset = 0;
    AbiType type;
};

struct FfiStructLayout {
    std::string name;
    uint32_t size = 0;
    uint32_t alignment = 1;
    std::vector<FfiStructField> fields;
};

struct FfiSymbolBinding {
    std::string vm_symbol;
    std::string native_symbol;
    uint32_t library_index = 0;
    uint32_t signature_index = 0;
};

struct FunctionFfiBindingHeader {
    bool enabled = false;
    CallingConvention convention = CallingConvention::kSystem;
    uint32_t signature_index = 0;
    uint32_t binding_index = 0;
};

}  // namespace tie::vm
