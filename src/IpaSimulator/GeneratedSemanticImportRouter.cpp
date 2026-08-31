#include "GeneratedSemanticImportRouter.hpp"

#include "GeneratedBridgeAdapter.hpp"
#include "GeneratedSemanticProvider.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "GeneratedSemanticProviderAdapters.inc"

namespace ipasim::bridge {
namespace {

enum class LiveGuestProfile {
    NoArgumentsSInt32ToX0,
};

struct ApprovedSemanticImportRoute {
    const char* guestSymbol;
    const char* hostExport;
    const wchar_t* providerModule;
    const char* adapterSymbol;
    const char* semanticOwner;
    LiveGuestProfile liveProfile;
};

#include "ApprovedSemanticImportRoutes.inc"

void setError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

void clearError(std::string* error) {
    if (error) {
        error->clear();
    }
}

std::string win32Failure(const char* operation, DWORD code) {
    std::ostringstream stream;
    stream << operation << " failed with Win32 error " << code;
    return stream.str();
}

bool modulePathFromHandle(
    HMODULE module,
    std::filesystem::path& path,
    std::string* error) {
    if (!module) {
        setError(error, "generated semantic import has no resolved module");
        return false;
    }

    wchar_t buffer[32768];
    constexpr DWORD count = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    const DWORD length = GetModuleFileNameW(module, buffer, count);
    if (length == 0) {
        setError(error, win32Failure("GetModuleFileNameW", GetLastError()));
        return false;
    }
    if (length >= count) {
        setError(error, "generated semantic provider module path exceeds Win32 buffer");
        return false;
    }

    path = std::filesystem::path(std::wstring(buffer, length));
    return true;
}

std::wstring lowercase(std::wstring value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](wchar_t item) { return static_cast<wchar_t>(std::towlower(item)); });
    return value;
}

bool moduleFilenameMatches(
    const std::filesystem::path& path,
    const wchar_t* approvedFilename) {
    if (!approvedFilename || !*approvedFilename) {
        return false;
    }
    return lowercase(path.filename().wstring()) ==
        lowercase(std::wstring(approvedFilename));
}

bool sameFile(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(left, right, error);
    return !error && equivalent;
}

const AdapterRecord* findGeneratedAdapter(
    const std::vector<AdapterRecord>& generated,
    const char* adapterSymbol) {
    if (!adapterSymbol) {
        return nullptr;
    }
    const auto found = std::find_if(
        generated.begin(),
        generated.end(),
        [adapterSymbol](const AdapterRecord& record) {
            return record.symbol == adapterSymbol;
        });
    return found == generated.end() ? nullptr : &*found;
}

bool validateLiveProfile(
    const AdapterRecord& record,
    LiveGuestProfile profile,
    std::string* error) {
    switch (profile) {
    case LiveGuestProfile::NoArgumentsSInt32ToX0: {
        const CommitSpec& commit = record.result.commit;
        if (record.requiresPointerValidation ||
            !record.arguments.empty() ||
            record.result.type.kind != ValueTypeKind::SInt32 ||
            !record.result.type.elements.empty() ||
            commit.kind != CommitKind::Registers ||
            commit.bank != GuestBank::Gpr ||
            commit.registers.size() != 1 ||
            commit.registers.front() != 0 ||
            commit.elementWidthBytes != 4) {
            setError(
                error,
                "generated adapter " + record.symbol +
                    " does not match live profile NoArgumentsSInt32ToX0");
            return false;
        }
        return true;
    }
    }

    setError(error, "approved semantic route uses an unsupported live guest profile");
    return false;
}

bool sameApprovedProviderIdentity(
    const ApprovedSemanticImportRoute& left,
    const ApprovedSemanticImportRoute& right) {
    return left.hostExport && right.hostExport &&
        std::string(left.hostExport) == right.hostExport &&
        left.providerModule && right.providerModule &&
        lowercase(std::wstring(left.providerModule)) ==
            lowercase(std::wstring(right.providerModule));
}

bool validateApprovedRouteTable(
    const std::vector<AdapterRecord>& generated,
    std::string* error) {
    constexpr std::size_t routeCount =
        sizeof(ApprovedSemanticImportRoutes) / sizeof(ApprovedSemanticImportRoutes[0]);
    if (routeCount == 0) {
        setError(error, "approved semantic import route table is empty");
        return false;
    }

    for (std::size_t index = 0; index < routeCount; ++index) {
        const auto& route = ApprovedSemanticImportRoutes[index];
        if (!route.guestSymbol || !*route.guestSymbol ||
            !route.hostExport || !*route.hostExport ||
            !route.providerModule || !*route.providerModule ||
            !route.adapterSymbol || !*route.adapterSymbol ||
            !route.semanticOwner || !*route.semanticOwner) {
            setError(
                error,
                "approved semantic import routes require guest symbol, host export, provider module, adapter symbol, and semantic owner");
            return false;
        }
        if (std::string(route.guestSymbol) != route.adapterSymbol) {
            setError(
                error,
                "approved semantic route guest symbol and generated adapter identity differ for " +
                    std::string(route.guestSymbol));
            return false;
        }

        for (std::size_t prior = 0; prior < index; ++prior) {
            const auto& previous = ApprovedSemanticImportRoutes[prior];
            if (std::string(previous.guestSymbol) == route.guestSymbol) {
                setError(
                    error,
                    "approved semantic import route table repeats guest symbol " +
                        std::string(route.guestSymbol));
                return false;
            }
            if (std::string(previous.adapterSymbol) == route.adapterSymbol) {
                setError(
                    error,
                    "approved semantic import route table repeats generated adapter " +
                        std::string(route.adapterSymbol));
                return false;
            }
            if (sameApprovedProviderIdentity(previous, route)) {
                setError(
                    error,
                    "approved semantic import route table repeats provider export " +
                        std::string(route.hostExport));
                return false;
            }
        }

        const AdapterRecord* adapter =
            findGeneratedAdapter(generated, route.adapterSymbol);
        if (!adapter) {
            setError(
                error,
                "approved semantic route has no generated adapter for " +
                    std::string(route.adapterSymbol));
            return false;
        }
        if (!validateLiveProfile(*adapter, route.liveProfile, error)) {
            return false;
        }
    }

    return true;
}

bool hasApprovedHostExport(const std::string& hostLookupName) {
    for (const auto& route : ApprovedSemanticImportRoutes) {
        if (hostLookupName == route.hostExport) {
            return true;
        }
    }
    return false;
}

const ApprovedSemanticImportRoute* findApprovedRoute(
    const std::string& hostLookupName,
    const std::filesystem::path& modulePath) {
    for (const auto& route : ApprovedSemanticImportRoutes) {
        if (hostLookupName == route.hostExport &&
            moduleFilenameMatches(modulePath, route.providerModule)) {
            return &route;
        }
    }
    return nullptr;
}

struct RouterState {
    std::mutex mutex;
    AdapterRegistry registry;
    std::unordered_map<std::string, SemanticProviderModule> providerModules;
    std::unordered_map<std::uint64_t, const ApprovedSemanticImportRoute*> selectedRoutes;
    bool adapterInitializationAttempted{false};
    bool adaptersReady{false};
    std::string adapterInitializationFailure;
};

RouterState& routerState() {
    static RouterState state;
    return state;
}

bool initializeAdaptersLocked(
    RouterState& state,
    std::string* error) {
    if (state.adapterInitializationAttempted) {
        if (!state.adaptersReady) {
            setError(error, state.adapterInitializationFailure);
        }
        return state.adaptersReady;
    }

    state.adapterInitializationAttempted = true;

    const auto generated = makeGeneratedSemanticProviderAdapters();
    std::string setupError;
    if (!validateApprovedRouteTable(generated, &setupError)) {
        state.adapterInitializationFailure = setupError;
        setError(error, state.adapterInitializationFailure);
        return false;
    }
    if (!state.registry.registerAdapters(generated, &setupError)) {
        state.adapterInitializationFailure =
            "generated semantic adapter registration failed: " + setupError;
        setError(error, state.adapterInitializationFailure);
        return false;
    }

    state.adaptersReady = true;
    return true;
}

bool ensureProviderBoundLocked(
    RouterState& state,
    const ApprovedSemanticImportRoute& route,
    const std::filesystem::path& modulePath,
    std::string* error) {
    const auto existing = state.providerModules.find(route.adapterSymbol);
    if (existing != state.providerModules.end()) {
        if (!sameFile(modulePath, existing->second.path())) {
            setError(
                error,
                std::string(route.guestSymbol) +
                    " candidate module differs from its approved semantic provider module");
            return false;
        }
        if (!state.registry.hasBinding(route.adapterSymbol)) {
            setError(
                error,
                "approved semantic provider lost its generated adapter binding for " +
                    std::string(route.adapterSymbol));
            return false;
        }
        return true;
    }

    SemanticProviderModule providerModule;
    const std::vector<SemanticProviderSpec> providers = {
        {route.adapterSymbol, route.hostExport, route.semanticOwner},
    };
    std::string setupError;
    if (!providerModule.loadAndBind(
            modulePath,
            state.registry,
            providers,
            &setupError)) {
        setError(
            error,
            "approved semantic provider binding failed for " +
                std::string(route.guestSymbol) + ": " + setupError);
        return false;
    }
    if (!sameFile(modulePath, providerModule.path())) {
        setError(
            error,
            std::string(route.guestSymbol) +
                " candidate module differs from its approved semantic provider module");
        return false;
    }

    const auto inserted = state.providerModules.emplace(
        route.adapterSymbol,
        std::move(providerModule));
    if (!inserted.second) {
        setError(
            error,
            "approved semantic provider state already exists for " +
                std::string(route.adapterSymbol));
        return false;
    }
    return true;
}

} // namespace

GeneratedSemanticImportSelection selectGeneratedSemanticImport(
    const std::string& hostLookupName,
    void* resolvedModule,
    std::uint64_t resolvedAddress,
    std::string* error) {
    clearError(error);

    // Generated ABI knowledge does not imply semantic approval. If the host
    // export is absent from the explicit route table, the existing loader path
    // remains authoritative and this router does nothing.
    if (!hasApprovedHostExport(hostLookupName)) {
        return GeneratedSemanticImportSelection::NotCandidate;
    }
    if (!resolvedModule) {
        setError(
            error,
            "loader resolved an approved generated import without a module");
        return GeneratedSemanticImportSelection::Rejected;
    }

    auto module = static_cast<HMODULE>(resolvedModule);
    std::filesystem::path modulePath;
    if (!modulePathFromHandle(module, modulePath, error)) {
        return GeneratedSemanticImportSelection::Rejected;
    }

    // Same-spelled exports in unrelated PE images remain non-candidates. A route
    // is eligible only when both its host export and approved provider module
    // identity match the loader's real resolution.
    const ApprovedSemanticImportRoute* route =
        findApprovedRoute(hostLookupName, modulePath);
    if (!route) {
        clearError(error);
        return GeneratedSemanticImportSelection::NotCandidate;
    }
    if (resolvedAddress == 0) {
        setError(
            error,
            std::string(route->guestSymbol) +
                " approved provider export did not resolve to callable code");
        return GeneratedSemanticImportSelection::Rejected;
    }

    FARPROC expected = GetProcAddress(module, route->hostExport);
    if (!expected ||
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(expected)) !=
            resolvedAddress) {
        setError(
            error,
            std::string(route->guestSymbol) +
                " candidate address does not match the approved live provider export " +
                route->hostExport);
        return GeneratedSemanticImportSelection::Rejected;
    }

    RouterState& state = routerState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!initializeAdaptersLocked(state, error)) {
        return GeneratedSemanticImportSelection::Rejected;
    }
    if (!state.registry.hasAdapter(route->adapterSymbol)) {
        setError(
            error,
            "approved semantic route has no registered generated adapter for " +
                std::string(route->adapterSymbol));
        return GeneratedSemanticImportSelection::Rejected;
    }
    if (!ensureProviderBoundLocked(state, *route, modulePath, error)) {
        return GeneratedSemanticImportSelection::Rejected;
    }

    state.selectedRoutes[resolvedAddress] = route;
    return GeneratedSemanticImportSelection::Selected;
}

bool isSelectedGeneratedSemanticImport(std::uint64_t address) {
    RouterState& state = routerState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.adaptersReady &&
        state.selectedRoutes.find(address) != state.selectedRoutes.end();
}

bool executeSelectedGeneratedSemanticImport(
    std::uint64_t address,
    std::uint64_t& guestX0,
    std::string* error) {
    clearError(error);

    RouterState& state = routerState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.adaptersReady) {
        setError(error, "generated semantic import router is not initialized");
        return false;
    }

    const auto selected = state.selectedRoutes.find(address);
    if (selected == state.selectedRoutes.end()) {
        setError(error, "host address was not selected by the loader semantic route");
        return false;
    }
    const ApprovedSemanticImportRoute& route = *selected->second;

    switch (route.liveProfile) {
    case LiveGuestProfile::NoArgumentsSInt32ToX0: {
        SyntheticGuestState guest;
        guest.x[0] = guestX0;
        if (!state.registry.execute(route.adapterSymbol, guest, {}, error)) {
            return false;
        }
        guestX0 = guest.x[0];
        return true;
    }
    }

    setError(error, "selected semantic import uses an unsupported live guest profile");
    return false;
}

} // namespace ipasim::bridge
