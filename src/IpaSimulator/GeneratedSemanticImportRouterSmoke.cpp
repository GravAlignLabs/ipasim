#include "GeneratedSemanticImportRouter.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <process.h>
#include <string>

namespace {

using ipasim::bridge::AdapterExecutionRequirements;
using ipasim::bridge::GeneratedSemanticImportSelection;
using ipasim::bridge::SyntheticGuestState;
using ipasim::bridge::executeSelectedGeneratedSemanticImport;
using ipasim::bridge::getSelectedGeneratedSemanticImportRequirements;
using ipasim::bridge::isSelectedGeneratedSemanticImport;
using ipasim::bridge::selectGeneratedSemanticImport;

std::uint64_t addressOf(FARPROC proc) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(proc));
}

bool proveUnapprovedSymbolStaysOnExistingPath(HMODULE bridge) {
    std::puts("[generated-semantic-import-router-smoke] unapproved route begin");

    FARPROC close = GetProcAddress(bridge, "close");
    if (!close) {
        std::fprintf(stderr, "[generated-semantic-import-router-smoke] bridge close export is missing\n");
        return false;
    }

    std::string error;
    const auto selection = selectGeneratedSemanticImport(
        "close", bridge, addressOf(close), &error);
    if (selection != GeneratedSemanticImportSelection::NotCandidate || !error.empty()) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] unapproved symbol changed routing: %s\n",
            error.c_str());
        return false;
    }
    if (isSelectedGeneratedSemanticImport(addressOf(close))) {
        std::fprintf(stderr, "[generated-semantic-import-router-smoke] unapproved address was selected\n");
        return false;
    }

    std::puts("[generated-semantic-import-router-smoke] unapproved route passed");
    return true;
}

bool proveApprovedSymbolWithoutModuleFailsClosed(HMODULE bridge) {
    std::puts("[generated-semantic-import-router-smoke] approved route missing module begin");

    FARPROC getpid = GetProcAddress(bridge, "getpid");
    if (!getpid) {
        std::fprintf(stderr, "[generated-semantic-import-router-smoke] bridge getpid export is missing\n");
        return false;
    }

    std::string error;
    const auto selection = selectGeneratedSemanticImport(
        "getpid", nullptr, addressOf(getpid), &error);
    if (selection != GeneratedSemanticImportSelection::Rejected ||
        error.find("without a module") == std::string::npos) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] approved route without module did not fail closed: %s\n",
            error.c_str());
        return false;
    }

    std::puts("[generated-semantic-import-router-smoke] approved route missing module passed");
    return true;
}

bool proveSameSpellingOtherModuleStaysOnExistingPath() {
    std::puts("[generated-semantic-import-router-smoke] non-provider module begin");

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC unrelated = kernel32 ? GetProcAddress(kernel32, "GetCurrentProcessId") : nullptr;
    if (!kernel32 || !unrelated) {
        std::fprintf(stderr, "[generated-semantic-import-router-smoke] kernel32 control export is unavailable\n");
        return false;
    }

    std::string error;
    const auto selection = selectGeneratedSemanticImport(
        "getpid", kernel32, addressOf(unrelated), &error);
    if (selection != GeneratedSemanticImportSelection::NotCandidate || !error.empty()) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] same spelling in another module did not preserve fallback: %s\n",
            error.c_str());
        return false;
    }

    std::puts("[generated-semantic-import-router-smoke] non-provider module passed");
    return true;
}

bool proveApprovedProviderMismatchFailsClosed(HMODULE bridge) {
    std::puts("[generated-semantic-import-router-smoke] provider address mismatch begin");

    FARPROC getuid = GetProcAddress(bridge, "getuid");
    if (!getuid) {
        std::fprintf(stderr, "[generated-semantic-import-router-smoke] bridge getuid export is missing\n");
        return false;
    }

    std::string error;
    const auto selection = selectGeneratedSemanticImport(
        "getpid", bridge, addressOf(getuid), &error);
    if (selection != GeneratedSemanticImportSelection::Rejected ||
        error.find("does not match") == std::string::npos) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] mismatched approved-provider address did not fail closed: %s\n",
            error.c_str());
        return false;
    }
    if (isSelectedGeneratedSemanticImport(addressOf(getuid))) {
        std::fprintf(stderr, "[generated-semantic-import-router-smoke] rejected address became selected\n");
        return false;
    }

    std::puts("[generated-semantic-import-router-smoke] provider address mismatch passed");
    return true;
}

bool proveGeneratedProcessIdentityRoutes(HMODULE bridge) {
    std::puts("[generated-semantic-import-router-smoke] process identity routes begin");

    struct Route {
        const char* hostExport;
        const char* guestSymbol;
    };
    const Route routes[] = {
        {"getpid", "_getpid"},
        {"getuid", "_getuid"},
        {"geteuid", "_geteuid"},
        {"getgid", "_getgid"},
        {"getegid", "_getegid"},
    };

    using IdentityFunction = std::uint32_t (*)();
    for (const Route& route : routes) {
        FARPROC proc = GetProcAddress(bridge, route.hostExport);
        if (!proc) {
            std::fprintf(
                stderr,
                "[generated-semantic-import-router-smoke] bridge %s export is missing\n",
                route.hostExport);
            return false;
        }
        const std::uint64_t address = addressOf(proc);
        const std::uint32_t expected =
            reinterpret_cast<IdentityFunction>(proc)();

        std::string error;
        const auto selection = selectGeneratedSemanticImport(
            route.hostExport, bridge, address, &error);
        if (selection != GeneratedSemanticImportSelection::Selected || !error.empty()) {
            std::fprintf(
                stderr,
                "[generated-semantic-import-router-smoke] approved route %s was not selected: %s\n",
                route.guestSymbol,
                error.c_str());
            return false;
        }
        if (!isSelectedGeneratedSemanticImport(address)) {
            std::fprintf(
                stderr,
                "[generated-semantic-import-router-smoke] selected route %s was not retained\n",
                route.guestSymbol);
            return false;
        }

        AdapterExecutionRequirements requirements;
        if (!getSelectedGeneratedSemanticImportRequirements(
                address, requirements, &error)) {
            std::fprintf(
                stderr,
                "[generated-semantic-import-router-smoke] generated requirements for %s failed: %s\n",
                route.guestSymbol,
                error.c_str());
            return false;
        }
        if (requirements.guestStackBytes != 0 || requirements.usesSimd ||
            requirements.requiresPointerValidation) {
            std::fprintf(
                stderr,
                "[generated-semantic-import-router-smoke] %s requirements were not derived as a no-argument generated adapter\n",
                route.guestSymbol);
            return false;
        }

        SyntheticGuestState guest;
        guest.x[0] = ~std::uint64_t{0};
        if (!executeSelectedGeneratedSemanticImport(address, guest, {}, &error)) {
            std::fprintf(
                stderr,
                "[generated-semantic-import-router-smoke] generated execution for %s failed: %s\n",
                route.guestSymbol,
                error.c_str());
            return false;
        }
        if (guest.x[0] != expected) {
            std::fprintf(
                stderr,
                "[generated-semantic-import-router-smoke] %s expected guest x0=%lu, got %llu\n",
                route.guestSymbol,
                static_cast<unsigned long>(expected),
                static_cast<unsigned long long>(guest.x[0]));
            return false;
        }
    }

    std::puts("[generated-semantic-import-router-smoke] process identity routes passed");
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::puts("[generated-semantic-import-router-smoke] table-driven loader route proof begin");

    if (argc != 2 || !argv[1] || !*argv[1]) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] expected path to IpaSimDarwinHost.dll\n");
        return 2;
    }

    std::error_code pathError;
    const std::filesystem::path bridgePath =
        std::filesystem::absolute(std::filesystem::path(argv[1]), pathError);
    if (pathError || bridgePath.empty()) {
        std::fprintf(stderr, "[generated-semantic-import-router-smoke] bridge path could not be resolved\n");
        return 2;
    }

    HMODULE bridge = LoadLibraryExW(
        bridgePath.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!bridge) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] LoadLibraryExW failed with Win32 error %lu\n",
            static_cast<unsigned long>(GetLastError()));
        return 2;
    }

    const bool passed =
        proveUnapprovedSymbolStaysOnExistingPath(bridge) &&
        proveApprovedSymbolWithoutModuleFailsClosed(bridge) &&
        proveSameSpellingOtherModuleStaysOnExistingPath() &&
        proveApprovedProviderMismatchFailsClosed(bridge) &&
        proveGeneratedProcessIdentityRoutes(bridge);

    FreeLibrary(bridge);
    if (!passed) {
        return 1;
    }

    std::puts("[generated-semantic-import-router-smoke] all table-driven loader route proofs passed");
    return 0;
}
