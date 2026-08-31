#include "GeneratedBridgeAdapter.hpp"

#include <ffi.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <sstream>
#include <utility>

namespace ipasim::bridge {
namespace {

void setError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

const char* bankName(GuestBank bank) {
    return bank == GuestBank::Gpr ? "GPR" : "SIMD";
}

bool validateType(const TypeSpec& type, bool nested, std::string* error) {
    if (type.kind == ValueTypeKind::Structure) {
        if (type.elements.empty()) {
            setError(error, "generated bridge structure type has no elements");
            return false;
        }
        for (const auto& element : type.elements) {
            if (!validateType(element, true, error)) {
                return false;
            }
        }
        return true;
    }
    if (!type.elements.empty()) {
        setError(error, "generated bridge builtin type unexpectedly has elements");
        return false;
    }
    if (nested && type.kind == ValueTypeKind::Pointer) {
        setError(
            error,
            "generated bridge structure contains a pointer without proven field layout policy");
        return false;
    }
    return true;
}

bool validateRegisters(
    GuestBank bank,
    const std::vector<unsigned>& registers,
    std::size_t width,
    std::string* error) {
    if (registers.empty()) {
        setError(error, "generated bridge register operation has no registers");
        return false;
    }
    if (width == 0 || width > (bank == GuestBank::Gpr ? 8u : 16u)) {
        std::ostringstream stream;
        stream << "generated bridge " << bankName(bank)
               << " element width is outside the guest register lane";
        setError(error, stream.str());
        return false;
    }
    std::vector<unsigned> seen;
    for (unsigned index : registers) {
        const unsigned limit = bank == GuestBank::Gpr ? 9u : 8u;
        if (index >= limit) {
            std::ostringstream stream;
            stream << "generated bridge " << bankName(bank)
                   << " register index " << index << " is out of range";
            setError(error, stream.str());
            return false;
        }
        if (std::find(seen.begin(), seen.end(), index) != seen.end()) {
            setError(error, "generated bridge repeats a guest register");
            return false;
        }
        seen.push_back(index);
    }
    return true;
}

bool validateAdapter(const AdapterRecord& record, std::string* error) {
    if (record.symbol.empty()) {
        setError(error, "generated bridge adapter has an empty symbol");
        return false;
    }

    bool requiresPointerValidation = false;
    for (std::size_t index = 0; index < record.arguments.size(); ++index) {
        const auto& argument = record.arguments[index];
        if (argument.logicalIndex != static_cast<int>(index)) {
            setError(error, record.symbol + ": logical argument indexes are not dense");
            return false;
        }
        if (argument.hasSourceIndex && argument.sourceIndex < 0) {
            setError(error, record.symbol + ": source index is marked present but is negative");
            return false;
        }
        if (!validateType(argument.type, false, error)) {
            return false;
        }
        requiresPointerValidation |= argument.type.kind == ValueTypeKind::Pointer;

        switch (argument.capture.kind) {
        case CaptureKind::Registers:
            if (!validateRegisters(
                    argument.capture.bank,
                    argument.capture.registers,
                    argument.capture.elementWidthBytes,
                    error)) {
                return false;
            }
            break;
        case CaptureKind::ResultPointer:
            if (argument.type.kind != ValueTypeKind::Pointer) {
                setError(error, record.symbol + ": x8 result-pointer capture is not pointer typed");
                return false;
            }
            requiresPointerValidation = true;
            break;
        case CaptureKind::GuestStack:
            break;
        }
    }

    if (!validateType(record.result.type, false, error)) {
        return false;
    }
    requiresPointerValidation |= record.result.type.kind == ValueTypeKind::Pointer;

    switch (record.result.commit.kind) {
    case CommitKind::None:
        if (record.result.type.kind != ValueTypeKind::Void) {
            setError(error, record.symbol + ": non-void result has no guest commit operation");
            return false;
        }
        break;
    case CommitKind::Registers:
        if (!validateRegisters(
                record.result.commit.bank,
                record.result.commit.registers,
                record.result.commit.elementWidthBytes,
                error)) {
            return false;
        }
        break;
    case CommitKind::GuestStack:
        break;
    case CommitKind::CalleeWritesResultPointer:
        if (record.result.type.kind != ValueTypeKind::Void) {
            setError(error, record.symbol + ": indirect guest result must be void to libffi");
            return false;
        }
        requiresPointerValidation = true;
        break;
    }

    if (record.requiresPointerValidation != requiresPointerValidation) {
        setError(
            error,
            record.symbol +
                ": generated pointer-validation flag disagrees with the adapter type/capture surface");
        return false;
    }
    return true;
}

ffi_type* builtinFfiType(ValueTypeKind kind) {
    switch (kind) {
    case ValueTypeKind::Void:
        return &ffi_type_void;
    case ValueTypeKind::UInt8:
        return &ffi_type_uint8;
    case ValueTypeKind::SInt8:
        return &ffi_type_sint8;
    case ValueTypeKind::UInt16:
        return &ffi_type_uint16;
    case ValueTypeKind::SInt16:
        return &ffi_type_sint16;
    case ValueTypeKind::UInt32:
        return &ffi_type_uint32;
    case ValueTypeKind::SInt32:
        return &ffi_type_sint32;
    case ValueTypeKind::UInt64:
        return &ffi_type_uint64;
    case ValueTypeKind::SInt64:
        return &ffi_type_sint64;
    case ValueTypeKind::Float:
        return &ffi_type_float;
    case ValueTypeKind::Double:
        return &ffi_type_double;
    case ValueTypeKind::Pointer:
        return &ffi_type_pointer;
    case ValueTypeKind::Structure:
        return nullptr;
    }
    return nullptr;
}

class FfiTypeNode {
public:
    explicit FfiTypeNode(const TypeSpec& type) {
        if (type.kind != ValueTypeKind::Structure) {
            type_ = builtinFfiType(type.kind);
            return;
        }

        structure_.size = 0;
        structure_.alignment = 0;
        structure_.type = FFI_TYPE_STRUCT;
        for (const auto& element : type.elements) {
            children_.push_back(std::make_unique<FfiTypeNode>(element));
        }
        for (const auto& child : children_) {
            elements_.push_back(child->get());
        }
        elements_.push_back(nullptr);
        structure_.elements = elements_.data();
        type_ = &structure_;
    }

    ffi_type* get() const {
        return type_;
    }

private:
    ffi_type structure_{};
    ffi_type* type_{nullptr};
    std::vector<std::unique_ptr<FfiTypeNode>> children_;
    std::vector<ffi_type*> elements_;
};

class StorageBuffer {
public:
    explicit StorageBuffer(std::size_t bytes)
        : bytes_(bytes),
          words_((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t)) {
        if (!words_.empty()) {
            std::memset(words_.data(), 0, words_.size() * sizeof(std::max_align_t));
        }
    }

    void* data() {
        return words_.empty() ? nullptr : words_.data();
    }

    const void* data() const {
        return words_.empty() ? nullptr : words_.data();
    }

    unsigned char* bytes() {
        return static_cast<unsigned char*>(data());
    }

    const unsigned char* bytes() const {
        return static_cast<const unsigned char*>(data());
    }

    std::size_t size() const {
        return bytes_;
    }

private:
    std::size_t bytes_{0};
    std::vector<std::max_align_t> words_;
};

bool copyGuestRegistersToStorage(
    const CaptureSpec& capture,
    const SyntheticGuestState& guest,
    StorageBuffer& storage,
    std::string* error) {
    const std::size_t required = capture.registers.size() * capture.elementWidthBytes;
    if (required != storage.size()) {
        setError(
            error,
            "generated bridge register capture does not describe the complete libffi carrier layout");
        return false;
    }

    for (std::size_t i = 0; i < capture.registers.size(); ++i) {
        const auto index = capture.registers[i];
        const void* source = capture.bank == GuestBank::Gpr
            ? static_cast<const void*>(&guest.x[index])
            : static_cast<const void*>(guest.v[index].data());
        std::memcpy(
            storage.bytes() + i * capture.elementWidthBytes,
            source,
            capture.elementWidthBytes);
    }
    return true;
}

bool copyStorageToGuestRegisters(
    const CommitSpec& commit,
    const StorageBuffer& storage,
    SyntheticGuestState& guest,
    std::string* error) {
    const std::size_t required = commit.registers.size() * commit.elementWidthBytes;
    if (required != storage.size()) {
        setError(
            error,
            "generated bridge result commit does not describe the complete libffi carrier layout");
        return false;
    }

    for (std::size_t i = 0; i < commit.registers.size(); ++i) {
        const auto index = commit.registers[i];
        if (commit.bank == GuestBank::Gpr) {
            guest.x[index] = 0;
            std::memcpy(
                &guest.x[index],
                storage.bytes() + i * commit.elementWidthBytes,
                commit.elementWidthBytes);
        } else {
            guest.v[index].fill(0);
            std::memcpy(
                guest.v[index].data(),
                storage.bytes() + i * commit.elementWidthBytes,
                commit.elementWidthBytes);
        }
    }
    return true;
}

bool captureArgument(
    const ArgumentSpec& argument,
    ffi_type* ffiType,
    const SyntheticGuestState& guest,
    StorageBuffer& storage,
    std::string* error) {
    if (!ffiType || ffiType->size != storage.size()) {
        setError(error, "generated bridge libffi argument size was not prepared correctly");
        return false;
    }

    switch (argument.capture.kind) {
    case CaptureKind::Registers:
        return copyGuestRegistersToStorage(argument.capture, guest, storage, error);
    case CaptureKind::ResultPointer: {
        if (storage.size() != sizeof(void*)) {
            setError(error, "generated bridge x8 result pointer is not host pointer sized");
            return false;
        }
        void* value = reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(guest.x[8]));
        std::memcpy(storage.data(), &value, sizeof(value));
        return true;
    }
    case CaptureKind::GuestStack:
        if (argument.capture.stackOffset > guest.stack.size() ||
            storage.size() > guest.stack.size() - argument.capture.stackOffset) {
            setError(error, "generated bridge guest stack capture exceeds supplied guest stack view");
            return false;
        }
        std::memcpy(
            storage.data(),
            guest.stack.data() + argument.capture.stackOffset,
            storage.size());
        return true;
    }
    setError(error, "generated bridge has an unknown capture operation");
    return false;
}

std::uint64_t pointerValue(const StorageBuffer& storage) {
    void* value = nullptr;
    if (storage.size() == sizeof(value)) {
        std::memcpy(&value, storage.data(), sizeof(value));
    }
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value));
}

bool validateArgumentPointer(
    const ArgumentSpec& argument,
    const StorageBuffer& storage,
    const PointerValidator& validator,
    std::string* error) {
    if (argument.type.kind != ValueTypeKind::Pointer) {
        return true;
    }
    const PointerUse use = argument.capture.kind == CaptureKind::ResultPointer
        ? PointerUse::IndirectResult
        : PointerUse::Argument;
    const auto address = pointerValue(storage);
    if (!validator || !validator(address, use)) {
        setError(error, "generated bridge pointer validator rejected an argument address");
        return false;
    }
    return true;
}

bool commitResult(
    const ResultSpec& result,
    const StorageBuffer& storage,
    SyntheticGuestState& guest,
    std::string* error) {
    switch (result.commit.kind) {
    case CommitKind::None:
    case CommitKind::CalleeWritesResultPointer:
        return true;
    case CommitKind::Registers:
        return copyStorageToGuestRegisters(result.commit, storage, guest, error);
    case CommitKind::GuestStack:
        if (result.commit.stackOffset > guest.stack.size() ||
            storage.size() > guest.stack.size() - result.commit.stackOffset) {
            setError(error, "generated bridge guest stack commit exceeds supplied guest stack view");
            return false;
        }
        std::memcpy(
            guest.stack.data() + result.commit.stackOffset,
            storage.data(),
            storage.size());
        return true;
    }
    setError(error, "generated bridge has an unknown result commit operation");
    return false;
}

} // namespace

TypeSpec TypeSpec::builtin(ValueTypeKind kind) {
    return TypeSpec{kind, {}};
}

TypeSpec TypeSpec::structure(std::initializer_list<TypeSpec> elements) {
    return TypeSpec{ValueTypeKind::Structure, std::vector<TypeSpec>(elements)};
}

bool TypeSpec::containsPointer() const {
    if (kind == ValueTypeKind::Pointer) {
        return true;
    }
    for (const auto& element : elements) {
        if (element.containsPointer()) {
            return true;
        }
    }
    return false;
}

CaptureSpec CaptureSpec::fromRegisters(
    GuestBank bank,
    std::initializer_list<unsigned> registers,
    std::size_t elementWidthBytes) {
    CaptureSpec result;
    result.kind = CaptureKind::Registers;
    result.bank = bank;
    result.registers = registers;
    result.elementWidthBytes = elementWidthBytes;
    return result;
}

CaptureSpec CaptureSpec::fromResultPointer() {
    CaptureSpec result;
    result.kind = CaptureKind::ResultPointer;
    result.bank = GuestBank::Gpr;
    result.registers = {8};
    result.elementWidthBytes = sizeof(std::uint64_t);
    return result;
}

CaptureSpec CaptureSpec::fromStack(std::size_t stackOffset) {
    CaptureSpec result;
    result.kind = CaptureKind::GuestStack;
    result.stackOffset = stackOffset;
    return result;
}

CommitSpec CommitSpec::none() {
    return CommitSpec{};
}

CommitSpec CommitSpec::toRegisters(
    GuestBank bank,
    std::initializer_list<unsigned> registers,
    std::size_t elementWidthBytes) {
    CommitSpec result;
    result.kind = CommitKind::Registers;
    result.bank = bank;
    result.registers = registers;
    result.elementWidthBytes = elementWidthBytes;
    return result;
}

CommitSpec CommitSpec::toStack(std::size_t stackOffset) {
    CommitSpec result;
    result.kind = CommitKind::GuestStack;
    result.stackOffset = stackOffset;
    return result;
}

CommitSpec CommitSpec::calleeWritesResultPointer() {
    CommitSpec result;
    result.kind = CommitKind::CalleeWritesResultPointer;
    return result;
}

bool AdapterRegistry::registerAdapter(AdapterRecord record, std::string* error) {
    if (!validateAdapter(record, error)) {
        return false;
    }
    if (adapters_.find(record.symbol) != adapters_.end()) {
        setError(error, record.symbol + ": generated adapter is already registered");
        return false;
    }
    adapters_.emplace(record.symbol, std::move(record));
    return true;
}

bool AdapterRegistry::registerAdapters(
    const std::vector<AdapterRecord>& records,
    std::string* error) {
    auto candidate = adapters_;
    for (const auto& record : records) {
        if (!validateAdapter(record, error)) {
            return false;
        }
        if (candidate.find(record.symbol) != candidate.end()) {
            setError(error, record.symbol + ": generated adapter is already registered");
            return false;
        }
        candidate.emplace(record.symbol, record);
    }
    adapters_.swap(candidate);
    return true;
}

bool AdapterRegistry::bindHostImplementation(
    const std::string& symbol,
    HostFunction function,
    BindingKind kind,
    std::string owner,
    std::string* error) {
    if (adapters_.find(symbol) == adapters_.end()) {
        setError(error, symbol + ": cannot bind a host function without a generated adapter");
        return false;
    }
    if (!function) {
        setError(error, symbol + ": host implementation pointer is null");
        return false;
    }
    if (owner.empty()) {
        setError(error, symbol + ": host implementation must name its semantic owner");
        return false;
    }
    if (bindings_.find(symbol) != bindings_.end()) {
        setError(error, symbol + ": host implementation is already bound");
        return false;
    }
    bindings_.emplace(symbol, Binding{function, kind, std::move(owner)});
    return true;
}

bool AdapterRegistry::execute(
    const std::string& symbol,
    SyntheticGuestState& guest,
    const PointerValidator& pointerValidator,
    std::string* error) const {
    const auto adapterIt = adapters_.find(symbol);
    if (adapterIt == adapters_.end()) {
        setError(error, symbol + ": no generated runtime adapter is registered");
        return false;
    }
    const auto bindingIt = bindings_.find(symbol);
    if (bindingIt == bindings_.end()) {
        setError(error, symbol + ": generated adapter has no explicit host implementation binding");
        return false;
    }

    const AdapterRecord& adapter = adapterIt->second;
    if (adapter.requiresPointerValidation && !pointerValidator) {
        setError(error, symbol + ": pointer validation is required before host execution");
        return false;
    }

    std::vector<std::unique_ptr<FfiTypeNode>> argumentNodes;
    std::vector<ffi_type*> argumentTypes;
    argumentNodes.reserve(adapter.arguments.size());
    argumentTypes.reserve(adapter.arguments.size());
    for (const auto& argument : adapter.arguments) {
        argumentNodes.push_back(std::make_unique<FfiTypeNode>(argument.type));
        if (!argumentNodes.back()->get()) {
            setError(error, symbol + ": could not construct a libffi argument type");
            return false;
        }
        argumentTypes.push_back(argumentNodes.back()->get());
    }

    FfiTypeNode resultNode(adapter.result.type);
    ffi_type* resultType = resultNode.get();
    if (!resultType) {
        setError(error, symbol + ": could not construct the libffi result type");
        return false;
    }

    ffi_cif cif{};
    const ffi_status prep = ffi_prep_cif(
        &cif,
        FFI_DEFAULT_ABI,
        static_cast<unsigned>(argumentTypes.size()),
        resultType,
        argumentTypes.empty() ? nullptr : argumentTypes.data());
    if (prep != FFI_OK) {
        std::ostringstream stream;
        stream << symbol << ": ffi_prep_cif failed with status " << static_cast<int>(prep);
        setError(error, stream.str());
        return false;
    }

    std::vector<StorageBuffer> argumentStorage;
    std::vector<void*> argumentValues;
    argumentStorage.reserve(adapter.arguments.size());
    argumentValues.reserve(adapter.arguments.size());
    for (std::size_t index = 0; index < adapter.arguments.size(); ++index) {
        const auto& argument = adapter.arguments[index];
        ffi_type* ffiType = argumentTypes[index];
        argumentStorage.emplace_back(ffiType->size);
        if (!captureArgument(argument, ffiType, guest, argumentStorage.back(), error)) {
            return false;
        }
        if (!validateArgumentPointer(
                argument,
                argumentStorage.back(),
                pointerValidator,
                error)) {
            return false;
        }
        argumentValues.push_back(argumentStorage.back().data());
    }

    const std::size_t resultSize = adapter.result.type.kind == ValueTypeKind::Void
        ? 0u
        : resultType->size;
    StorageBuffer resultStorage(resultSize);

    ffi_call(
        &cif,
        FFI_FN(bindingIt->second.function),
        resultStorage.data(),
        argumentValues.empty() ? nullptr : argumentValues.data());

    if (adapter.result.type.kind == ValueTypeKind::Pointer) {
        const auto address = pointerValue(resultStorage);
        if (!pointerValidator || !pointerValidator(address, PointerUse::ReturnedPointer)) {
            setError(error, symbol + ": pointer validator rejected the host return address");
            return false;
        }
    }

    return commitResult(adapter.result, resultStorage, guest, error);
}

std::size_t AdapterRegistry::adapterCount() const noexcept {
    return adapters_.size();
}

bool AdapterRegistry::hasAdapter(const std::string& symbol) const {
    return adapters_.find(symbol) != adapters_.end();
}

bool AdapterRegistry::hasBinding(const std::string& symbol) const {
    return bindings_.find(symbol) != bindings_.end();
}

} // namespace ipasim::bridge
