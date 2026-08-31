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

// The first production live-guest profile is deliberately narrow: _getpid has
// no arguments and commits one generated signed 32-bit result into ARM64 x0.
// Future symbols must extend live guest capture/commit deliberately rather than
// bypassing the generated adapter with another handwritten signature table.
bool executeSelectedGeneratedSemanticImport(
    std::uint64_t address,
    std::uint64_t& guestX0,
    std::string* error = nullptr);

} // namespace ipasim::bridge
