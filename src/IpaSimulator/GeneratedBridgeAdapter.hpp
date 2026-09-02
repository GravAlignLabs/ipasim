#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
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
// must be captured before execution. Stack sizing is derived from the generated
// carrier type itself rather than guessed from the symbol or a second signature
// table. The supported TypeSpec vocabulary has ordinary C scalar/pointer
// alignment; structures are laid out recursively with natural member alignment.
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
        std::string* error = nullptr) const {
        const auto found = adapters_.find(symbol);
        if (found == adapters_.end()) {
            if (error) {
                *error = symbol + ": no generated runtime adapter is registered";
            }
            return false;
        }

        struct Layout {
            std::size_t size{0};
            std::size_t alignment{1};
        };
        const auto alignUp = [](std::size_t value, std::size_t alignment,
                                std::size_t& result) {
            if (alignment == 0) {
                return false;
            }
            const std::size_t remainder = value % alignment;
            const std::size_t add = remainder == 0 ? 0 : alignment - remainder;
            if (value > std::numeric_limits<std::size_t>::max() - add) {
                return false;
            }
            result = value + add;
            return true;
        };

        std::function<bool(const TypeSpec&, Layout&)> layoutType;
        layoutType = [&](const TypeSpec& type, Layout& layout) -> bool {
            switch (type.kind) {
            case ValueTypeKind::Void:
                layout = {0, 1};
                return true;
            case ValueTypeKind::UInt8:
            case ValueTypeKind::SInt8:
                layout = {1, 1};
                return true;
            case ValueTypeKind::UInt16:
            case ValueTypeKind::SInt16:
                layout = {2, 2};
                return true;
            case ValueTypeKind::UInt32:
            case ValueTypeKind::SInt32:
            case ValueTypeKind::Float:
                layout = {4, 4};
                return true;
            case ValueTypeKind::UInt64:
            case ValueTypeKind::SInt64:
            case ValueTypeKind::Double:
            case ValueTypeKind::Pointer:
                layout = {8, 8};
                return true;
            case ValueTypeKind::Structure:
                break;
            }

            if (type.elements.empty()) {
                return false;
            }
            std::size_t cursor = 0;
            std::size_t structureAlignment = 1;
            for (const auto& element : type.elements) {
                Layout child;
                if (!layoutType(element, child)) {
                    return false;
                }
                std::size_t aligned = 0;
                if (!alignUp(cursor, child.alignment, aligned) ||
                    aligned > std::numeric_limits<std::size_t>::max() - child.size) {
                    return false;
                }
                cursor = aligned + child.size;
                structureAlignment = std::max(structureAlignment, child.alignment);
            }
            std::size_t finalSize = 0;
            if (!alignUp(cursor, structureAlignment, finalSize)) {
                return false;
            }
            layout = {finalSize, structureAlignment};
            return true;
        };

        const AdapterRecord& adapter = found->second;
        AdapterExecutionRequirements candidate;
        candidate.requiresPointerValidation = adapter.requiresPointerValidation;

        const auto includeStackSpan = [&](std::size_t offset, const TypeSpec& type,
                                          std::size_t& required) {
            Layout layout;
            if (!layoutType(type, layout) ||
                offset > std::numeric_limits<std::size_t>::max() - layout.size) {
                return false;
            }
            required = std::max(required, offset + layout.size);
            return true;
        };

        for (const auto& argument : adapter.arguments) {
            if (argument.capture.kind == CaptureKind::Registers &&
                argument.capture.bank == GuestBank::Simd) {
                candidate.usesSimd = true;
            }
            if (argument.capture.kind == CaptureKind::GuestStack &&
                !includeStackSpan(
                    argument.capture.stackOffset,
                    argument.type,
                    candidate.guestStackBytes)) {
                if (error) {
                    *error = symbol + ": could not derive generated guest stack argument span";
                }
                return false;
            }
        }

        if (adapter.result.commit.kind == CommitKind::Registers &&
            adapter.result.commit.bank == GuestBank::Simd) {
            candidate.usesSimd = true;
        }
        if (adapter.result.commit.kind == CommitKind::GuestStack &&
            !includeStackSpan(
                adapter.result.commit.stackOffset,
                adapter.result.type,
                candidate.guestStackBytes)) {
            if (error) {
                *error = symbol + ": could not derive generated guest stack result span";
            }
            return false;
        }

        requirements = candidate;
        if (error) {
            error->clear();
        }
        return true;
    }

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
