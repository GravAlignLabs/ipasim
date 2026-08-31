#pragma once

#include "GeneratedBridgeAdapter.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ipasim::bridge {

struct SemanticProviderSpec {
    std::string guestSymbol;
    std::string hostExport;
    std::string owner;
};

// Owns the loaded host module for as long as AdapterRegistry may call any
// function pointer bound from it. Binding is explicit: no guest-symbol spelling
// normalization or broad PE-export auto-routing is performed here.
class SemanticProviderModule {
public:
    SemanticProviderModule() = default;
    ~SemanticProviderModule();

    SemanticProviderModule(const SemanticProviderModule&) = delete;
    SemanticProviderModule& operator=(const SemanticProviderModule&) = delete;

    SemanticProviderModule(SemanticProviderModule&& other) noexcept;
    SemanticProviderModule& operator=(SemanticProviderModule&& other) noexcept;

    bool loadAndBind(
        const std::filesystem::path& modulePath,
        AdapterRegistry& registry,
        const std::vector<SemanticProviderSpec>& providers,
        std::string* error = nullptr);

    bool loaded() const noexcept;
    const std::filesystem::path& path() const noexcept;

private:
    void reset() noexcept;

    void* module_{nullptr};
    std::filesystem::path path_;
};

} // namespace ipasim::bridge
