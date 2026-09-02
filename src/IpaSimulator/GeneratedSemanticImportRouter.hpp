#pragma once

#include "GeneratedBridgeAdapter.hpp"

#include <cstdint>
#include <string>

namespace ipasim::bridge {

// Loader selection is intentionally tri-state. A non-candidate stays on the
// existing compatibility path, while a candidate that fails generated/provider
// validation is rejected and must not silently fall back to a handwritten ABI.
enum class GeneratedSemanticImportSelection {
    NotCandidate,
    Selected,
    Rejected,
};

GeneratedSemanticImportSelection selectGeneratedSemanticImport(
    const std::string& hostLookupName,
    void* resolvedModule,
    std::uint64_t resolvedAddress,
    std::string* error = nullptr);

bool isSelectedGeneratedSemanticImport(std::uint64_t address);

// Ask the selected generated AdapterRecord which pieces of live AAPCS64 state
// SysTranslator must snapshot. This is deliberately adapter-driven: the runtime
// does not keep a second argument-count or return-register signature table.
bool getSelectedGeneratedSemanticImportRequirements(
    std::uint64_t address,
    AdapterExecutionRequirements& requirements,
    std::string* error = nullptr);

// Execute an already loader-selected, explicitly approved provider using the
// complete generated guest capture/result-commit record. Pointer-bearing
// adapters remain fail-closed unless the live runtime supplies a validator.
bool executeSelectedGeneratedSemanticImport(
    std::uint64_t address,
    SyntheticGuestState& guest,
    const PointerValidator& pointerValidator = {},
    std::string* error = nullptr);

} // namespace ipasim::bridge
