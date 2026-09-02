#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

namespace ipasim::bridge {

enum class ValueTypeKind {
    Void,
    UInt8,
    SInt8,
    UInt16,
    SInt16,
    UInt32,
    SInt32,
    UInt64,
    SInt64,
    Float,
    Double,
    Pointer,
    Structure,
};

enum class GuestBank {
    Gpr,
    Simd,
};

struct TypeSpec {
    ValueTypeKind kind{ValueTypeKind::Void};
    std::vector<TypeSpec> elements;

    static TypeSpec builtin(ValueTypeKind kind);
    static TypeSpec structure(std::initializer_list<TypeSpec> elements);

    bool containsPointer() const;
};

enum class CaptureKind {
    Registers,
    ResultPointer,
    GuestStack,
};

struct CaptureSpec {
    CaptureKind kind{CaptureKind::Registers};
    GuestBank bank{GuestBank::Gpr};
    std::vector<unsigned> registers;
    std::size_t elementWidthBytes{0};
    std::size_t stackOffset{0};

    static CaptureSpec fromRegisters(
        GuestBank bank,
        std::initializer_list<unsigned> registers,
        std::size_t elementWidthBytes);
    static CaptureSpec fromResultPointer();
    static CaptureSpec fromStack(std::size_t stackOffset);
};

enum class CommitKind {
    None,
    Registers,
    GuestStack,
    CalleeWritesResultPointer,
};

struct CommitSpec {
    CommitKind kind{CommitKind::None};
    GuestBank bank{GuestBank::Gpr};
    std::vector<unsigned> registers;
    std::size_t elementWidthBytes{0};
    std::size_t stackOffset{0};

    static CommitSpec none();
    static CommitSpec toRegisters(
        GuestBank bank,
        std::initializer_list<unsigned> registers,
        std::size_t elementWidthBytes);
    static CommitSpec toStack(std::size_t stackOffset);
    static CommitSpec calleeWritesResultPointer();
};

struct ArgumentSpec {
    int logicalIndex{0};
    int sourceIndex{-1};
    bool hasSourceIndex{false};
    TypeSpec type;
    CaptureSpec capture;
    bool indirectSourceAggregate{false};
};

struct ResultSpec {
    TypeSpec type;
    CommitSpec commit;
};

struct AdapterRecord {
    std::string symbol;
    bool requiresPointerValidation{false};
    std::vector<ArgumentSpec> arguments;
    ResultSpec result;
};

// This state is deliberately independent of Unicorn. Production routing
// populates this same mechanical view from the live ARM64 context, so generated
// adapters never need to know which emulator engine supplied the registers.
struct SyntheticGuestState {
    std::array<std::uint64_t, 9> x{};
    alignas(16) std::array<std::array<unsigned char, 16>, 8> v{};
    std::vector<unsigned char> stack;
};

// The runtime asks the registered adapter what portion of the live ARM64 state
// must be captured before execution. Stack sizing is derived after libffi has
// laid out the generated carrier types, rather than guessed from symbol names.
struct AdapterExecutionRequirements {
    std::size_t guestStackBytes{0};
    bool usesSimd{false};
    bool requiresPointerValidation{false};
};

enum class PointerUse {
    Argument,
    IndirectResult,
    ReturnedPointer,
};

using PointerValidator = std::function<bool(std::uint64_t, PointerUse)>;
using HostFunction = void (*)();

enum class BindingKind {
    ControlledTest,
    SemanticProvider,
};

class AdapterRegistry {
public:
    bool registerAdapter(AdapterRecord record, std::string* error = nullptr);
    bool registerAdapters(
        const std::vector<AdapterRecord>& records,
        std::string* error = nullptr);

    bool bindHostImplementation(
        const std::string& symbol,
        HostFunction function,
        BindingKind kind,
        std::string owner,
        std::string* error = nullptr);

    bool describeExecution(
        const std::string& symbol,
        AdapterExecutionRequirements& requirements,
        std::string* error = nullptr) const;

    bool execute(
        const std::string& symbol,
        SyntheticGuestState& guest,
        const PointerValidator& pointerValidator = {},
        std::string* error = nullptr) const;

    std::size_t adapterCount() const noexcept;
    bool hasAdapter(const std::string& symbol) const;
    bool hasBinding(const std::string& symbol) const;

private:
    struct Binding {
        HostFunction function{nullptr};
        BindingKind kind{BindingKind::ControlledTest};
        std::string owner;
    };

    std::unordered_map<std::string, AdapterRecord> adapters_;
    std::unordered_map<std::string, Binding> bindings_;
};

} // namespace ipasim::bridge
