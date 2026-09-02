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
using ipasim::bridge::PointerUse;
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

    FARPROC read = GetProcAddress(bridge, "read");
    if (!read) {
        std::fprintf(stderr, "[generated-semantic-import-router-smoke] bridge read export is missing\n");
        return false;
    }

    std::string error;
    const auto selection = selectGeneratedSemanticImport(
        "read", bridge, addressOf(read), &error);
    if (selection != GeneratedSemanticImportSelection::NotCandidate || !error.empty()) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] unapproved symbol changed routing: %s\n",
            error.c_str());
        return false;
    }
    if (isSelectedGeneratedSemanticImport(addressOf(read))) {
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

bool proveGeneratedScalarDescriptorRoutes(HMODULE bridge) {
    std::puts("[generated-semantic-import-router-smoke] scalar descriptor routes begin");

    FARPROC openProc = GetProcAddress(bridge, "open");
    FARPROC closeProc = GetProcAddress(bridge, "close");
    FARPROC lseekProc = GetProcAddress(bridge, "lseek");
    if (!openProc || !closeProc || !lseekProc) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] scalar descriptor exports are incomplete\n");
        return false;
    }

    using OpenFunction = int (*)(const char*, int, std::uint16_t);
    using CloseFunction = int (*)(int);
    auto open = reinterpret_cast<OpenFunction>(openProc);
    auto directClose = reinterpret_cast<CloseFunction>(closeProc);

    constexpr int DarwinOpenReadWrite = 0x00000002;
    constexpr int DarwinOpenCreate = 0x00000200;
    constexpr int DarwinOpenTruncate = 0x00000400;
    const int fd = open(
        "/generated-semantic-route-scalar",
        DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenTruncate,
        0600);
    if (fd < 0) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] could not create descriptor fixture\n");
        return false;
    }

    bool descriptorOpen = true;
    const auto cleanup = [&]() {
        if (descriptorOpen)
            directClose(fd);
    };

    const std::uint64_t closeAddress = addressOf(closeProc);
    const std::uint64_t lseekAddress = addressOf(lseekProc);
    std::string error;

    for (const auto& route : {
             std::pair<const char*, std::uint64_t>{"close", closeAddress},
             std::pair<const char*, std::uint64_t>{"lseek", lseekAddress},
         }) {
        const auto selection = selectGeneratedSemanticImport(
            route.first, bridge, route.second, &error);
        if (selection != GeneratedSemanticImportSelection::Selected || !error.empty()) {
            std::fprintf(
                stderr,
                "[generated-semantic-import-router-smoke] scalar route %s was not selected: %s\n",
                route.first,
                error.c_str());
            cleanup();
            return false;
        }
    }

    AdapterExecutionRequirements closeRequirements;
    AdapterExecutionRequirements seekRequirements;
    if (!getSelectedGeneratedSemanticImportRequirements(
            closeAddress, closeRequirements, &error) ||
        !getSelectedGeneratedSemanticImportRequirements(
            lseekAddress, seekRequirements, &error)) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] scalar descriptor requirements failed: %s\n",
            error.c_str());
        cleanup();
        return false;
    }
    if (closeRequirements.guestStackBytes != 0 || closeRequirements.usesSimd ||
        closeRequirements.requiresPointerValidation ||
        seekRequirements.guestStackBytes != 0 || seekRequirements.usesSimd ||
        seekRequirements.requiresPointerValidation) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] scalar descriptor requirements unexpectedly need stack/SIMD/pointer validation\n");
        cleanup();
        return false;
    }

    constexpr std::uint64_t LargeOffset = 0x100000005ULL;
    SyntheticGuestState seekGuest;
    seekGuest.x[0] = static_cast<std::uint32_t>(fd);
    seekGuest.x[1] = LargeOffset;
    seekGuest.x[2] = 0; // SEEK_SET on Darwin and UCRT.
    if (!executeSelectedGeneratedSemanticImport(
            lseekAddress, seekGuest, {}, &error)) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] generated lseek execution failed: %s\n",
            error.c_str());
        cleanup();
        return false;
    }
    if (seekGuest.x[0] != LargeOffset) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] generated lseek truncated 64-bit offset/result: expected %llu got %llu\n",
            static_cast<unsigned long long>(LargeOffset),
            static_cast<unsigned long long>(seekGuest.x[0]));
        cleanup();
        return false;
    }

    SyntheticGuestState closeGuest;
    closeGuest.x[0] = static_cast<std::uint32_t>(fd);
    if (!executeSelectedGeneratedSemanticImport(
            closeAddress, closeGuest, {}, &error)) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] generated close execution failed: %s\n",
            error.c_str());
        cleanup();
        return false;
    }
    if (static_cast<std::uint32_t>(closeGuest.x[0]) != 0) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] generated close returned %llu instead of success\n",
            static_cast<unsigned long long>(closeGuest.x[0]));
        cleanup();
        return false;
    }
    descriptorOpen = false;

    std::puts("[generated-semantic-import-router-smoke] scalar descriptor routes passed");
    return true;
}

bool proveGeneratedWriteRoute(HMODULE bridge) {
    std::puts("[generated-semantic-import-router-smoke] pointer write route begin");

    FARPROC openProc = GetProcAddress(bridge, "open");
    FARPROC closeProc = GetProcAddress(bridge, "close");
    FARPROC lseekProc = GetProcAddress(bridge, "lseek");
    FARPROC readProc = GetProcAddress(bridge, "read");
    FARPROC writeProc = GetProcAddress(bridge, "write");
    if (!openProc || !closeProc || !lseekProc || !readProc || !writeProc) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] pointer write fixture exports are incomplete\n");
        return false;
    }

    using OpenFunction = int (*)(const char*, int, std::uint16_t);
    using CloseFunction = int (*)(int);
    using LseekFunction = std::int64_t (*)(int, std::int64_t, int);
    using ReadFunction = std::intptr_t (*)(int, void*, std::size_t);
    auto open = reinterpret_cast<OpenFunction>(openProc);
    auto directClose = reinterpret_cast<CloseFunction>(closeProc);
    auto directLseek = reinterpret_cast<LseekFunction>(lseekProc);
    auto directRead = reinterpret_cast<ReadFunction>(readProc);

    constexpr int DarwinOpenReadWrite = 0x00000002;
    constexpr int DarwinOpenCreate = 0x00000200;
    constexpr int DarwinOpenTruncate = 0x00000400;
    const int fd = open(
        "/generated-semantic-route-write",
        DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenTruncate,
        0600);
    if (fd < 0) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] could not create pointer write fixture\n");
        return false;
    }

    const auto cleanup = [&]() { directClose(fd); };
    const std::uint64_t writeAddress = addressOf(writeProc);
    std::string error;
    const auto selection = selectGeneratedSemanticImport(
        "write", bridge, writeAddress, &error);
    if (selection != GeneratedSemanticImportSelection::Selected || !error.empty()) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] generated write route was not selected: %s\n",
            error.c_str());
        cleanup();
        return false;
    }

    AdapterExecutionRequirements requirements;
    if (!getSelectedGeneratedSemanticImportRequirements(
            writeAddress, requirements, &error)) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] generated write requirements failed: %s\n",
            error.c_str());
        cleanup();
        return false;
    }
    if (requirements.guestStackBytes != 0 || requirements.usesSimd ||
        !requirements.requiresPointerValidation) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] generated write did not require pointer validation\n");
        cleanup();
        return false;
    }

    constexpr char Payload[] = "generated-pointer-write";
    constexpr std::size_t PayloadSize = sizeof(Payload) - 1;
    const std::uint64_t payloadAddress = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(Payload));

    const auto makeGuest = [&]() {
        SyntheticGuestState guest;
        guest.x[0] = static_cast<std::uint32_t>(fd);
        guest.x[1] = payloadAddress;
        guest.x[2] = PayloadSize;
        return guest;
    };

    SyntheticGuestState missingValidator = makeGuest();
    error.clear();
    if (executeSelectedGeneratedSemanticImport(
            writeAddress, missingValidator, {}, &error) ||
        error.find("pointer validator rejected") == std::string::npos) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] generated write did not fail closed without pointer validation: %s\n",
            error.c_str());
        cleanup();
        return false;
    }
    if (directLseek(fd, 0, SEEK_CUR) != 0) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] missing pointer validator reached the write provider\n");
        cleanup();
        return false;
    }

    SyntheticGuestState rejectedValidator = makeGuest();
    error.clear();
    if (executeSelectedGeneratedSemanticImport(
            writeAddress,
            rejectedValidator,
            [](std::uint64_t, PointerUse) { return false; },
            &error) ||
        error.find("pointer validator rejected") == std::string::npos) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] generated write did not honor a rejecting pointer validator: %s\n",
            error.c_str());
        cleanup();
        return false;
    }
    if (directLseek(fd, 0, SEEK_CUR) != 0) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] rejected pointer validation still reached the write provider\n");
        cleanup();
        return false;
    }

    bool validatedPointer = false;
    SyntheticGuestState writeGuest = makeGuest();
    error.clear();
    if (!executeSelectedGeneratedSemanticImport(
            writeAddress,
            writeGuest,
            [&](std::uint64_t address, PointerUse use) {
                validatedPointer = address == payloadAddress &&
                    use == PointerUse::Argument;
                return validatedPointer;
            },
            &error)) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] generated write execution failed: %s\n",
            error.c_str());
        cleanup();
        return false;
    }
    if (!validatedPointer || writeGuest.x[0] != PayloadSize) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] generated write pointer/result commit was incorrect\n");
        cleanup();
        return false;
    }

    if (directLseek(fd, 0, SEEK_SET) != 0) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] could not rewind generated write fixture\n");
        cleanup();
        return false;
    }
    char readback[PayloadSize] = {};
    if (directRead(fd, readback, PayloadSize) !=
            static_cast<std::intptr_t>(PayloadSize) ||
        std::string(readback, PayloadSize) != std::string(Payload, PayloadSize)) {
        std::fprintf(
            stderr,
            "[generated-semantic-import-router-smoke] generated write did not transfer real file data\n");
        cleanup();
        return false;
    }

    cleanup();
    std::puts("[generated-semantic-import-router-smoke] pointer write route passed");
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
        proveGeneratedProcessIdentityRoutes(bridge) &&
        proveGeneratedScalarDescriptorRoutes(bridge) &&
        proveGeneratedWriteRoute(bridge);

    FreeLibrary(bridge);
    if (!passed) {
        return 1;
    }

    std::puts("[generated-semantic-import-router-smoke] all table-driven loader route proofs passed");
    return 0;
}
