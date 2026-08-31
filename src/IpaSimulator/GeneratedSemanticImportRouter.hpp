#pragma once

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

// Live execution is profile-driven by the approved route table. The current
// production profile has no arguments and commits one signed 32-bit result into
// ARM64 x0. Additional profiles must extend guest capture/commit deliberately;
// they must not bypass generated ABI records with a handwritten signature table.
bool executeSelectedGeneratedSemanticImport(
    std::uint64_t address,
    std::uint64_t& guestX0,
    std::string* error = nullptr);

} // namespace ipasim::bridge
