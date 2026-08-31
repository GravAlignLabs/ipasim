#include "GeneratedSemanticProvider.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <set>
#include <sstream>
#include <utility>

namespace ipasim::bridge {
namespace {

void setError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

bool executableProtection(DWORD protection) {
    const DWORD base = protection & 0xffu;
    return base == PAGE_EXECUTE ||
        base == PAGE_EXECUTE_READ ||
        base == PAGE_EXECUTE_READWRITE ||
        base == PAGE_EXECUTE_WRITECOPY;
}

std::string win32Failure(const char* operation, DWORD code) {
    std::ostringstream stream;
    stream << operation << " failed with Win32 error " << code;
    return stream.str();
}

struct PendingBinding {
    const SemanticProviderSpec* spec{nullptr};
    HostFunction function{nullptr};
};

} // namespace

SemanticProviderModule::~SemanticProviderModule() {
    reset();
}

SemanticProviderModule::SemanticProviderModule(
    SemanticProviderModule&& other) noexcept
    : module_(other.module_), path_(std::move(other.path_)) {
    other.module_ = nullptr;
    other.path_.clear();
}

SemanticProviderModule& SemanticProviderModule::operator=(
    SemanticProviderModule&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    module_ = other.module_;
    path_ = std::move(other.path_);
    other.module_ = nullptr;
    other.path_.clear();
    return *this;
}

bool SemanticProviderModule::loadAndBind(
    const std::filesystem::path& modulePath,
    AdapterRegistry& registry,
    const std::vector<SemanticProviderSpec>& providers,
    std::string* error) {
    if (module_) {
        setError(error, "semantic provider module is already loaded");
        return false;
    }
    if (modulePath.empty()) {
        setError(error, "semantic provider module path is empty");
        return false;
    }
    if (providers.empty()) {
        setError(error, "semantic provider allowlist is empty");
        return false;
    }

    std::error_code pathError;
    const std::filesystem::path absolutePath =
        std::filesystem::absolute(modulePath, pathError);
    if (pathError || absolutePath.empty()) {
        setError(error, "could not resolve semantic provider module path");
        return false;
    }

    HMODULE module = LoadLibraryExW(
        absolutePath.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        setError(error, win32Failure("LoadLibraryExW", GetLastError()));
        return false;
    }

    std::set<std::string> guestSymbols;
    std::vector<PendingBinding> pending;
    pending.reserve(providers.size());

    for (const auto& provider : providers) {
        if (provider.guestSymbol.empty() ||
            provider.hostExport.empty() ||
            provider.owner.empty()) {
            FreeLibrary(module);
            setError(
                error,
                "semantic provider entries require guest symbol, host export, and owner");
            return false;
        }
        if (!guestSymbols.insert(provider.guestSymbol).second) {
            FreeLibrary(module);
            setError(
                error,
                "semantic provider allowlist repeats guest symbol " +
                    provider.guestSymbol);
            return false;
        }
        if (!registry.hasAdapter(provider.guestSymbol)) {
            FreeLibrary(module);
            setError(
                error,
                "semantic provider has no generated adapter for " +
                    provider.guestSymbol);
            return false;
        }
        if (registry.hasBinding(provider.guestSymbol)) {
            FreeLibrary(module);
            setError(
                error,
                "semantic provider adapter is already bound: " +
                    provider.guestSymbol);
            return false;
        }

        FARPROC address = GetProcAddress(module, provider.hostExport.c_str());
        if (!address) {
            FreeLibrary(module);
            setError(
                error,
                "semantic provider export is missing: " + provider.hostExport);
            return false;
        }

        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(address),
                &memory,
                sizeof(memory)) != sizeof(memory)) {
            const DWORD code = GetLastError();
            FreeLibrary(module);
            setError(error, win32Failure("VirtualQuery", code));
            return false;
        }
        if (memory.AllocationBase != module) {
            FreeLibrary(module);
            setError(
                error,
                "semantic provider export resolves outside the approved module: " +
                    provider.hostExport);
            return false;
        }
        if (!executableProtection(memory.Protect) ||
            (memory.Protect & PAGE_GUARD) != 0 ||
            memory.State != MEM_COMMIT) {
            FreeLibrary(module);
            setError(
                error,
                "semantic provider export is not executable code: " +
                    provider.hostExport);
            return false;
        }

        pending.push_back(PendingBinding{
            &provider,
            reinterpret_cast<HostFunction>(address)});
    }

    // All fallible module/export/adapter validation is complete before the
    // registry is mutated. AdapterRegistry is not concurrently modified by this
    // setup path, so these bindings cannot leave a partial allowlist behind.
    for (const auto& item : pending) {
        std::string bindError;
        if (!registry.bindHostImplementation(
                item.spec->guestSymbol,
                item.function,
                BindingKind::SemanticProvider,
                item.spec->owner,
                &bindError)) {
            FreeLibrary(module);
            setError(
                error,
                "semantic provider binding failed for " +
                    item.spec->guestSymbol + ": " + bindError);
            return false;
        }
    }

    module_ = module;
    path_ = absolutePath;
    return true;
}

bool SemanticProviderModule::loaded() const noexcept {
    return module_ != nullptr;
}

const std::filesystem::path& SemanticProviderModule::path() const noexcept {
    return path_;
}

void SemanticProviderModule::reset() noexcept {
    if (module_) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
    path_.clear();
}

} // namespace ipasim::bridge
