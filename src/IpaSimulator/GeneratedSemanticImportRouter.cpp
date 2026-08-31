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
#include <unordered_map>
#include <utility>
#include <vector>

#include "GeneratedSemanticProviderAdapters.inc"

namespace ipasim::bridge {
namespace {

constexpr const char* GuestGetpid = "_getpid";
constexpr const char* HostGetpid = "getpid";
constexpr const wchar_t* DarwinHostBridgeFilename = L"ipasimdarwinhost.dll";

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

bool isDarwinHostBridgePath(const std::filesystem::path& path) {
    std::wstring filename = path.filename().wstring();
    std::transform(
        filename.begin(),
        filename.end(),
        filename.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return filename == DarwinHostBridgeFilename;
}

bool sameFile(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(left, right, error);
    return !error && equivalent;
}

bool validateLiveGetpidProfile(
    const AdapterRecord& record,
    std::string* error) {
    const CommitSpec& commit = record.result.commit;
    if (record.symbol != GuestGetpid ||
        record.requiresPointerValidation ||
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
            "generated _getpid adapter no longer matches the supported live ARM64 profile");
        return false;
    }
    return true;
}

struct RouterState {
    std::mutex mutex;
    AdapterRegistry registry;
    SemanticProviderModule providerModule;
    std::unordered_map<std::uint64_t, std::string> selectedRoutes;
    bool initializationAttempted{false};
    bool ready{false};
    std::string initializationFailure;
};

RouterState& routerState() {
    static RouterState state;
    return state;
}

bool initializeRouterLocked(
    RouterState& state,
    const std::filesystem::path& modulePath,
    std::string* error) {
    if (state.initializationAttempted) {
        if (!state.ready) {
            setError(error, state.initializationFailure);
        }
        return state.ready;
    }

    state.initializationAttempted = true;

    const auto generated = makeGeneratedSemanticProviderAdapters();
    if (generated.size() != 1) {
        state.initializationFailure =
            "production generated semantic adapter table must contain exactly _getpid";
        setError(error, state.initializationFailure);
        return false;
    }

    std::string setupError;
    if (!validateLiveGetpidProfile(generated.front(), &setupError)) {
        state.initializationFailure = setupError;
        setError(error, state.initializationFailure);
        return false;
    }
    if (!state.registry.registerAdapters(generated, &setupError)) {
        state.initializationFailure =
            "generated semantic adapter registration failed: " + setupError;
        setError(error, state.initializationFailure);
        return false;
    }

    const std::vector<SemanticProviderSpec> providers = {
        {GuestGetpid, HostGetpid, "DarwinHostBridge.getpid"},
    };
    if (!state.providerModule.loadAndBind(
            modulePath,
            state.registry,
            providers,
            &setupError)) {
        state.initializationFailure =
            "approved semantic provider binding failed: " + setupError;
        setError(error, state.initializationFailure);
        return false;
    }

    state.ready = true;
    return true;
}

} // namespace

GeneratedSemanticImportSelection selectGeneratedSemanticImport(
    const std::string& hostLookupName,
    void* resolvedModule,
    std::uint64_t resolvedAddress,
    std::string* error) {
    clearError(error);

    // There is intentionally no spelling normalization or PE export sweep here.
    // Only the explicitly migrated loader spelling is a production candidate.
    if (hostLookupName != HostGetpid) {
        return GeneratedSemanticImportSelection::NotCandidate;
    }
    if (!resolvedModule || resolvedAddress == 0) {
        setError(error, "loader resolved _getpid without a module/code address");
        return GeneratedSemanticImportSelection::Rejected;
    }

    auto module = static_cast<HMODULE>(resolvedModule);
    std::filesystem::path modulePath;
    if (!modulePathFromHandle(module, modulePath, error) ||
        !isDarwinHostBridgePath(modulePath)) {
        if (error && error->empty()) {
            setError(error, "_getpid candidate did not resolve from IpaSimDarwinHost.dll");
        }
        return GeneratedSemanticImportSelection::Rejected;
    }

    FARPROC expected = GetProcAddress(module, HostGetpid);
    if (!expected ||
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(expected)) !=
            resolvedAddress) {
        setError(
            error,
            "_getpid candidate address does not match IpaSimDarwinHost.dll!getpid");
        return GeneratedSemanticImportSelection::Rejected;
    }

    RouterState& state = routerState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!initializeRouterLocked(state, modulePath, error)) {
        return GeneratedSemanticImportSelection::Rejected;
    }
    if (!sameFile(modulePath, state.providerModule.path())) {
        setError(
            error,
            "_getpid candidate module differs from the approved semantic provider module");
        return GeneratedSemanticImportSelection::Rejected;
    }

    state.selectedRoutes[resolvedAddress] = GuestGetpid;
    return GeneratedSemanticImportSelection::Selected;
}

bool isSelectedGeneratedSemanticImport(std::uint64_t address) {
    RouterState& state = routerState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.ready && state.selectedRoutes.find(address) != state.selectedRoutes.end();
}

bool executeSelectedGeneratedSemanticImport(
    std::uint64_t address,
    std::uint64_t& guestX0,
    std::string* error) {
    clearError(error);

    RouterState& state = routerState();
    std::string symbol;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.ready) {
            setError(error, "generated semantic import router is not initialized");
            return false;
        }
        const auto route = state.selectedRoutes.find(address);
        if (route == state.selectedRoutes.end()) {
            setError(error, "host address was not selected by the loader semantic route");
            return false;
        }
        symbol = route->second;
    }

    SyntheticGuestState guest;
    guest.x[0] = guestX0;
    if (!state.registry.execute(symbol, guest, {}, error)) {
        return false;
    }

    guestX0 = guest.x[0];
    return true;
}

} // namespace ipasim::bridge
