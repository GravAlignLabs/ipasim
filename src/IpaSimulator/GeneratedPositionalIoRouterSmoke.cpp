#include "GeneratedSemanticImportRouter.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>

namespace {

using ipasim::bridge::AdapterExecutionRequirements;
using ipasim::bridge::GeneratedSemanticImportSelection;
using ipasim::bridge::PointerUse;
using ipasim::bridge::SyntheticGuestState;
using ipasim::bridge::executeSelectedGeneratedSemanticImport;
using ipasim::bridge::getSelectedGeneratedSemanticImportRequirements;
using ipasim::bridge::selectGeneratedSemanticImport;

constexpr int DarwinOpenReadWrite = 0x00000002;
constexpr int DarwinOpenCreate = 0x00000200;
constexpr int DarwinOpenTruncate = 0x00000400;
constexpr int DarwinSeekSet = 0;
constexpr int DarwinSeekCurrent = 1;

std::uint64_t addressOf(FARPROC proc) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(proc));
}

int fail(const char* message) {
    std::fprintf(stderr, "[generated-positional-io-router-smoke] FAIL: %s\n", message);
    return 1;
}

FARPROC requireExport(HMODULE module, const char* name) {
    FARPROC proc = GetProcAddress(module, name);
    if (!proc) {
        std::fprintf(
            stderr,
            "[generated-positional-io-router-smoke] missing export: %s\n",
            name);
    }
    return proc;
}

bool selectRoute(
    HMODULE bridge,
    const char* hostExport,
    FARPROC proc,
    std::uint64_t& address,
    std::string& error) {
    address = addressOf(proc);
    error.clear();
    const auto selection = selectGeneratedSemanticImport(
        hostExport, bridge, address, &error);
    if (selection != GeneratedSemanticImportSelection::Selected || !error.empty()) {
        std::fprintf(
            stderr,
            "[generated-positional-io-router-smoke] route %s was not selected: %s\n",
            hostExport,
            error.c_str());
        return false;
    }
    return true;
}

bool checkRequirements(
    std::uint64_t address,
    const char* name,
    std::string& error) {
    AdapterExecutionRequirements requirements;
    error.clear();
    if (!getSelectedGeneratedSemanticImportRequirements(
            address, requirements, &error)) {
        std::fprintf(
            stderr,
            "[generated-positional-io-router-smoke] %s requirements failed: %s\n",
            name,
            error.c_str());
        return false;
    }
    if (requirements.guestStackBytes != 0 || requirements.usesSimd ||
        !requirements.requiresPointerValidation) {
        std::fprintf(
            stderr,
            "[generated-positional-io-router-smoke] %s requirements did not match the generated pointer-bearing AAPCS64 record\n",
            name);
        return false;
    }
    return true;
}

template <typename SeekFunction>
bool positionIs(SeekFunction seek, int fd, std::int64_t expected) {
    return seek(fd, 0, DarwinSeekCurrent) == expected;
}

} // namespace

int main(int argc, char** argv) {
    std::puts("[generated-positional-io-router-smoke] generated pread/pwrite proof begin");

    if (argc != 2 || !argv[1] || !*argv[1]) {
        return fail("expected path to IpaSimDarwinHost.dll");
    }

    std::error_code pathError;
    const std::filesystem::path bridgePath =
        std::filesystem::absolute(std::filesystem::path(argv[1]), pathError);
    if (pathError || bridgePath.empty()) {
        return fail("bridge path could not be resolved");
    }

    HMODULE bridge = LoadLibraryExW(
        bridgePath.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!bridge) {
        return fail("could not load IpaSimDarwinHost.dll");
    }

    FARPROC openProc = requireExport(bridge, "open");
    FARPROC closeProc = requireExport(bridge, "close");
    FARPROC lseekProc = requireExport(bridge, "lseek");
    FARPROC readProc = requireExport(bridge, "read");
    FARPROC writeProc = requireExport(bridge, "write");
    FARPROC preadProc = requireExport(bridge, "pread");
    FARPROC pwriteProc = requireExport(bridge, "pwrite");
    FARPROC errorProc = requireExport(bridge, "__error");
    if (!openProc || !closeProc || !lseekProc || !readProc || !writeProc ||
        !preadProc || !pwriteProc || !errorProc) {
        FreeLibrary(bridge);
        return 1;
    }

    using OpenFunction = int (*)(const char*, int, std::uint16_t);
    using CloseFunction = int (*)(int);
    using SeekFunction = std::int64_t (*)(int, std::int64_t, int);
    using ReadFunction = std::intptr_t (*)(int, void*, std::size_t);
    using WriteFunction = std::intptr_t (*)(int, const void*, std::size_t);
    using ErrorFunction = int* (*)();

    auto open = reinterpret_cast<OpenFunction>(openProc);
    auto close = reinterpret_cast<CloseFunction>(closeProc);
    auto seek = reinterpret_cast<SeekFunction>(lseekProc);
    auto directRead = reinterpret_cast<ReadFunction>(readProc);
    auto directWrite = reinterpret_cast<WriteFunction>(writeProc);
    auto errorPointer = reinterpret_cast<ErrorFunction>(errorProc);
    int* hostErrno = errorPointer();
    if (!hostErrno) {
        FreeLibrary(bridge);
        return fail("Darwin __error did not expose provider errno storage");
    }

    const int fd = open(
        "/generated-semantic-route-positional",
        DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenTruncate,
        0600);
    if (fd < 0) {
        FreeLibrary(bridge);
        return fail("could not create positional generated-route fixture");
    }

    bool descriptorOpen = true;
    const auto cleanup = [&]() {
        if (descriptorOpen) {
            close(fd);
            descriptorOpen = false;
        }
        FreeLibrary(bridge);
    };

    constexpr char Initial[] = "abcdef";
    if (directWrite(fd, Initial, 6) != 6 || seek(fd, 4, DarwinSeekSet) != 4) {
        cleanup();
        return fail("could not establish positional-I/O fixture state");
    }

    std::string error;
    std::uint64_t preadAddress = 0;
    std::uint64_t pwriteAddress = 0;
    if (!selectRoute(bridge, "pread", preadProc, preadAddress, error) ||
        !selectRoute(bridge, "pwrite", pwriteProc, pwriteAddress, error) ||
        !checkRequirements(preadAddress, "pread", error) ||
        !checkRequirements(pwriteAddress, "pwrite", error)) {
        cleanup();
        return 1;
    }

    constexpr char Replacement[] = "XY";
    const std::uint64_t replacementAddress = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(Replacement));

    SyntheticGuestState missingValidator;
    missingValidator.x[0] = static_cast<std::uint32_t>(fd);
    missingValidator.x[1] = replacementAddress;
    missingValidator.x[2] = 2;
    missingValidator.x[3] = 2;
    error.clear();
    if (executeSelectedGeneratedSemanticImport(
            pwriteAddress, missingValidator, {}, &error) ||
        error.find("pointer validation is required before host execution") ==
            std::string::npos ||
        !positionIs(seek, fd, 4)) {
        cleanup();
        return fail("pwrite did not fail closed before provider execution when pointer validation was absent");
    }

    char rejectedRead[2] = {};
    SyntheticGuestState rejectedValidator;
    rejectedValidator.x[0] = static_cast<std::uint32_t>(fd);
    rejectedValidator.x[1] = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(rejectedRead));
    rejectedValidator.x[2] = sizeof(rejectedRead);
    rejectedValidator.x[3] = 1;
    error.clear();
    if (executeSelectedGeneratedSemanticImport(
            preadAddress,
            rejectedValidator,
            [](std::uint64_t, PointerUse) { return false; },
            &error) ||
        error.find("pointer validator rejected an argument address") ==
            std::string::npos ||
        !positionIs(seek, fd, 4)) {
        cleanup();
        return fail("pread did not fail closed before provider execution when the pointer validator rejected the address");
    }

    const auto nullArgumentValidator = [](std::uint64_t address, PointerUse use) {
        return address == 0 && use == PointerUse::Argument;
    };
    for (const std::uint64_t route : {preadAddress, pwriteAddress}) {
        SyntheticGuestState zeroLength;
        zeroLength.x[0] = static_cast<std::uint32_t>(fd);
        zeroLength.x[1] = 0;
        zeroLength.x[2] = 0;
        zeroLength.x[3] = 2;
        error.clear();
        if (!executeSelectedGeneratedSemanticImport(
                route, zeroLength, nullArgumentValidator, &error) ||
            zeroLength.x[0] != 0 || !positionIs(seek, fd, 4)) {
            cleanup();
            return fail("zero-length/null positional I/O did not preserve the provider contract");
        }
    }

    SyntheticGuestState writeGuest;
    writeGuest.x[0] = static_cast<std::uint32_t>(fd);
    writeGuest.x[1] = replacementAddress;
    writeGuest.x[2] = 2;
    writeGuest.x[3] = 2;
    bool pwritePointerValidated = false;
    error.clear();
    if (!executeSelectedGeneratedSemanticImport(
            pwriteAddress,
            writeGuest,
            [&](std::uint64_t address, PointerUse use) {
                pwritePointerValidated =
                    address == replacementAddress && use == PointerUse::Argument;
                return pwritePointerValidated;
            },
            &error) ||
        !pwritePointerValidated || writeGuest.x[0] != 2 ||
        !positionIs(seek, fd, 4)) {
        cleanup();
        return fail("generated pwrite did not transfer data while preserving the caller descriptor position");
    }

    char positionedRead[2] = {};
    const std::uint64_t positionedReadAddress = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(positionedRead));
    SyntheticGuestState readGuest;
    readGuest.x[0] = static_cast<std::uint32_t>(fd);
    readGuest.x[1] = positionedReadAddress;
    readGuest.x[2] = sizeof(positionedRead);
    readGuest.x[3] = 1;
    bool preadPointerValidated = false;
    error.clear();
    if (!executeSelectedGeneratedSemanticImport(
            preadAddress,
            readGuest,
            [&](std::uint64_t address, PointerUse use) {
                preadPointerValidated =
                    address == positionedReadAddress && use == PointerUse::Argument;
                return preadPointerValidated;
            },
            &error) ||
        !preadPointerValidated || readGuest.x[0] != 2 ||
        std::memcmp(positionedRead, "bX", 2) != 0 ||
        !positionIs(seek, fd, 4)) {
        cleanup();
        return fail("generated pread did not read real data while preserving the caller descriptor position");
    }

    char beyondEof = 'Z';
    const std::uint64_t beyondEofAddress = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(&beyondEof));
    SyntheticGuestState largeOffsetRead;
    largeOffsetRead.x[0] = static_cast<std::uint32_t>(fd);
    largeOffsetRead.x[1] = beyondEofAddress;
    largeOffsetRead.x[2] = 1;
    largeOffsetRead.x[3] = 0x100000001ULL;
    error.clear();
    if (!executeSelectedGeneratedSemanticImport(
            preadAddress,
            largeOffsetRead,
            [&](std::uint64_t address, PointerUse use) {
                return address == beyondEofAddress && use == PointerUse::Argument;
            },
            &error) ||
        largeOffsetRead.x[0] != 0 || beyondEof != 'Z' ||
        !positionIs(seek, fd, 4)) {
        cleanup();
        return fail("generated pread truncated or otherwise mishandled a greater-than-32-bit offset");
    }

    *hostErrno = 0;
    SyntheticGuestState negativeOffsetRead;
    negativeOffsetRead.x[0] = static_cast<std::uint32_t>(fd);
    negativeOffsetRead.x[1] = beyondEofAddress;
    negativeOffsetRead.x[2] = 1;
    negativeOffsetRead.x[3] = (std::numeric_limits<std::uint64_t>::max)();
    error.clear();
    if (!executeSelectedGeneratedSemanticImport(
            preadAddress,
            negativeOffsetRead,
            [&](std::uint64_t address, PointerUse use) {
                return address == beyondEofAddress && use == PointerUse::Argument;
            },
            &error) ||
        negativeOffsetRead.x[0] !=
            (std::numeric_limits<std::uint64_t>::max)() ||
        *hostErrno != EINVAL || !positionIs(seek, fd, 4)) {
        cleanup();
        return fail("generated pread did not preserve the signed 64-bit negative-offset contract");
    }

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const std::size_t pageSize = systemInfo.dwPageSize;
    auto* crossing = static_cast<unsigned char*>(VirtualAlloc(
        nullptr,
        pageSize * 2,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
    if (!crossing) {
        cleanup();
        return fail("could not allocate cross-page pointer-validation fixture");
    }
    crossing[pageSize - 1] = 'Q';
    DWORD oldProtection = 0;
    if (!VirtualProtect(
            crossing + pageSize,
            pageSize,
            PAGE_NOACCESS,
            &oldProtection)) {
        VirtualFree(crossing, 0, MEM_RELEASE);
        cleanup();
        return fail("could not protect the second pointer-validation page");
    }

    const std::uint64_t crossingAddress = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(crossing + pageSize - 1));
    const auto crossingBaseValidator = [&](std::uint64_t address, PointerUse use) {
        return address == crossingAddress && use == PointerUse::Argument;
    };

    *hostErrno = 0;
    SyntheticGuestState crossingWrite;
    crossingWrite.x[0] = static_cast<std::uint32_t>(fd);
    crossingWrite.x[1] = crossingAddress;
    crossingWrite.x[2] = 2;
    crossingWrite.x[3] = 0;
    error.clear();
    if (!executeSelectedGeneratedSemanticImport(
            pwriteAddress, crossingWrite, crossingBaseValidator, &error) ||
        crossingWrite.x[0] != (std::numeric_limits<std::uint64_t>::max)() ||
        *hostErrno != EFAULT || !positionIs(seek, fd, 4)) {
        VirtualFree(crossing, 0, MEM_RELEASE);
        cleanup();
        return fail("generated pwrite did not reject a readable span crossing into PAGE_NOACCESS before host I/O");
    }

    *hostErrno = 0;
    SyntheticGuestState crossingRead;
    crossingRead.x[0] = static_cast<std::uint32_t>(fd);
    crossingRead.x[1] = crossingAddress;
    crossingRead.x[2] = 2;
    crossingRead.x[3] = 0;
    error.clear();
    if (!executeSelectedGeneratedSemanticImport(
            preadAddress, crossingRead, crossingBaseValidator, &error) ||
        crossingRead.x[0] != (std::numeric_limits<std::uint64_t>::max)() ||
        *hostErrno != EFAULT || !positionIs(seek, fd, 4)) {
        VirtualFree(crossing, 0, MEM_RELEASE);
        cleanup();
        return fail("generated pread did not reject a writable span crossing into PAGE_NOACCESS before host I/O");
    }
    VirtualFree(crossing, 0, MEM_RELEASE);

    auto* readOnly = static_cast<unsigned char*>(VirtualAlloc(
        nullptr,
        pageSize,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
    if (!readOnly) {
        cleanup();
        return fail("could not allocate read-only pointer-validation fixture");
    }
    readOnly[0] = 'R';
    if (!VirtualProtect(readOnly, pageSize, PAGE_READONLY, &oldProtection)) {
        VirtualFree(readOnly, 0, MEM_RELEASE);
        cleanup();
        return fail("could not apply read-only pointer-validation protection");
    }
    const std::uint64_t readOnlyAddress = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(readOnly));

    *hostErrno = 0;
    SyntheticGuestState readOnlyDestination;
    readOnlyDestination.x[0] = static_cast<std::uint32_t>(fd);
    readOnlyDestination.x[1] = readOnlyAddress;
    readOnlyDestination.x[2] = 1;
    readOnlyDestination.x[3] = 0;
    error.clear();
    if (!executeSelectedGeneratedSemanticImport(
            preadAddress,
            readOnlyDestination,
            [&](std::uint64_t address, PointerUse use) {
                return address == readOnlyAddress && use == PointerUse::Argument;
            },
            &error) ||
        readOnlyDestination.x[0] !=
            (std::numeric_limits<std::uint64_t>::max)() ||
        *hostErrno != EFAULT || !positionIs(seek, fd, 4)) {
        VirtualFree(readOnly, 0, MEM_RELEASE);
        cleanup();
        return fail("generated pread accepted a non-writable destination page");
    }
    VirtualFree(readOnly, 0, MEM_RELEASE);

    const std::uint64_t overflowAddress =
        (std::numeric_limits<std::uint64_t>::max)() - 1;
    *hostErrno = 0;
    SyntheticGuestState overflowRead;
    overflowRead.x[0] = static_cast<std::uint32_t>(fd);
    overflowRead.x[1] = overflowAddress;
    overflowRead.x[2] = 4;
    overflowRead.x[3] = 0;
    error.clear();
    if (!executeSelectedGeneratedSemanticImport(
            preadAddress,
            overflowRead,
            [&](std::uint64_t address, PointerUse use) {
                return address == overflowAddress && use == PointerUse::Argument;
            },
            &error) ||
        overflowRead.x[0] != (std::numeric_limits<std::uint64_t>::max)() ||
        *hostErrno != EFAULT || !positionIs(seek, fd, 4)) {
        cleanup();
        return fail("generated pread did not reject an overflowing guest buffer span");
    }

    if (seek(fd, 0, DarwinSeekSet) != 0) {
        cleanup();
        return fail("could not rewind positional generated-route fixture");
    }
    char finalReadback[6] = {};
    if (directRead(fd, finalReadback, sizeof(finalReadback)) != 6 ||
        std::memcmp(finalReadback, "abXYef", 6) != 0) {
        cleanup();
        return fail("generated positional writes did not persist at the expected explicit offset");
    }

    if (close(fd) != 0) {
        descriptorOpen = false;
        FreeLibrary(bridge);
        return fail("could not close positional generated-route fixture");
    }
    descriptorOpen = false;
    FreeLibrary(bridge);

    std::puts("[generated-positional-io-router-smoke] all generated pread/pwrite proofs passed");
    return 0;
}
