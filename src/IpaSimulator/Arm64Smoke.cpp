#include <unicorn/arm64.h>
#include <unicorn/unicorn.h>

#include <array>
#include <cstdint>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void stage(const char *message) {
    std::fprintf(stderr, "[arm64-smoke] %s\n", message);
    std::fflush(stderr);
}

bool check(uc_err err, const char *what) {
    if (err == UC_ERR_OK)
        return true;

    std::fprintf(stderr, "[arm64-smoke] %s failed: %s (%d)\n", what,
                 uc_strerror(err), static_cast<int>(err));
    std::fflush(stderr);
    return false;
}

#ifdef _WIN32
void printWindowsAddressDetails(void *address) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != 0) {
        std::fprintf(stderr,
                     "[arm64-smoke] address memory: base=%p allocation=%p size=0x%llX state=0x%08lX protect=0x%08lX type=0x%08lX\n",
                     memory.BaseAddress,
                     memory.AllocationBase,
                     static_cast<unsigned long long>(memory.RegionSize),
                     static_cast<unsigned long>(memory.State),
                     static_cast<unsigned long>(memory.Protect),
                     static_cast<unsigned long>(memory.Type));
    } else {
        std::fprintf(stderr,
                     "[arm64-smoke] VirtualQuery failed for %p: error=%lu\n",
                     address,
                     static_cast<unsigned long>(GetLastError()));
    }

    HMODULE module = nullptr;
    const auto moduleAddress = reinterpret_cast<LPCSTR>(address);
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           moduleAddress, &module)) {
        char modulePath[MAX_PATH] = {};
        const DWORD pathLength = GetModuleFileNameA(module, modulePath, MAX_PATH);
        const auto offset = reinterpret_cast<std::uintptr_t>(address) -
                            reinterpret_cast<std::uintptr_t>(module);
        if (pathLength != 0) {
            std::fprintf(stderr,
                         "[arm64-smoke] address owner: module=%s base=%p offset=0x%llX\n",
                         modulePath,
                         static_cast<void *>(module),
                         static_cast<unsigned long long>(offset));
        } else {
            std::fprintf(stderr,
                         "[arm64-smoke] address owner: module base=%p offset=0x%llX (GetModuleFileName error=%lu)\n",
                         static_cast<void *>(module),
                         static_cast<unsigned long long>(offset),
                         static_cast<unsigned long>(GetLastError()));
        }
    } else {
        std::fprintf(stderr,
                     "[arm64-smoke] address owner: no loaded PE module (likely generated/private code)\n");
    }

#if defined(_M_X64) || defined(__x86_64__)
    DWORD64 unwindImageBase = 0;
    PRUNTIME_FUNCTION runtimeFunction = RtlLookupFunctionEntry(
        static_cast<DWORD64>(reinterpret_cast<std::uintptr_t>(address)),
        &unwindImageBase, nullptr);
    if (runtimeFunction) {
        std::fprintf(stderr,
                     "[arm64-smoke] unwind entry: imageBase=0x%llX begin=0x%08lX end=0x%08lX unwind=0x%08lX\n",
                     static_cast<unsigned long long>(unwindImageBase),
                     static_cast<unsigned long>(runtimeFunction->BeginAddress),
                     static_cast<unsigned long>(runtimeFunction->EndAddress),
                     static_cast<unsigned long>(runtimeFunction->UnwindData));
    } else {
        std::fprintf(stderr,
                     "[arm64-smoke] unwind entry: none for exception address\n");
    }
#endif

    std::fflush(stderr);
}

LONG CALLBACK exceptionProbe(EXCEPTION_POINTERS *info) {
    if (info && info->ExceptionRecord) {
        const EXCEPTION_RECORD *record = info->ExceptionRecord;
        std::fprintf(stderr,
                     "[arm64-smoke] Windows exception 0x%08lX at %p flags=0x%08lX parameters=%lu\n",
                     static_cast<unsigned long>(record->ExceptionCode),
                     record->ExceptionAddress,
                     static_cast<unsigned long>(record->ExceptionFlags),
                     static_cast<unsigned long>(record->NumberParameters));

        for (DWORD index = 0; index < record->NumberParameters; ++index) {
            std::fprintf(stderr,
                         "[arm64-smoke] exception parameter[%lu]=0x%llX\n",
                         static_cast<unsigned long>(index),
                         static_cast<unsigned long long>(record->ExceptionInformation[index]));
        }

#if defined(_M_X64) || defined(__x86_64__)
        if (info->ContextRecord) {
            const CONTEXT *context = info->ContextRecord;
            std::fprintf(stderr,
                         "[arm64-smoke] host context: RIP=0x%llX RSP=0x%llX RBP=0x%llX RCX=0x%llX RDX=0x%llX\n",
                         static_cast<unsigned long long>(context->Rip),
                         static_cast<unsigned long long>(context->Rsp),
                         static_cast<unsigned long long>(context->Rbp),
                         static_cast<unsigned long long>(context->Rcx),
                         static_cast<unsigned long long>(context->Rdx));
        }
#endif

        printWindowsAddressDetails(record->ExceptionAddress);
    }

    // Diagnostic only. Never swallow an exception or turn a crash into success.
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

} // namespace

int main() {
#ifdef _WIN32
    void *exceptionHandler = AddVectoredExceptionHandler(1, exceptionProbe);
    if (!exceptionHandler)
        stage("warning: AddVectoredExceptionHandler failed");
#endif

    stage("entered main");
    stage("checking ARM64 architecture support");
    if (!uc_arch_supported(UC_ARCH_ARM64)) {
        std::fprintf(stderr, "[arm64-smoke] Unicorn reports ARM64 as unsupported.\n");
        return 1;
    }
    stage("ARM64 architecture is reported as supported");

    // Opening both endian modes proves that the paired AArch64 backends are
    // present in the DLL. Modern iOS device Mach-O uses little-endian ARM64.
    uc_engine *bigEndian = nullptr;
    stage("opening ARM64 big-endian engine");
    if (!check(uc_open(UC_ARCH_ARM64, UC_MODE_BIG_ENDIAN, &bigEndian),
               "uc_open(ARM64 big-endian)"))
        return 2;
    stage("opened ARM64 big-endian engine");

    stage("closing ARM64 big-endian engine");
    if (!check(uc_close(bigEndian), "uc_close(ARM64 big-endian)"))
        return 3;
    stage("closed ARM64 big-endian engine");

    uc_engine *uc = nullptr;
    stage("opening ARM64 little-endian engine");
    if (!check(uc_open(UC_ARCH_ARM64, UC_MODE_LITTLE_ENDIAN, &uc),
               "uc_open(ARM64 little-endian)"))
        return 4;
    stage("opened ARM64 little-endian engine");

    constexpr uint64_t base = 0x100000;
    constexpr size_t pageSize = 0x1000;
    constexpr std::array<uint8_t, 8> code = {
        0x40, 0x05, 0x80, 0xD2, // mov x0, #42
        0x00, 0x04, 0x00, 0x91  // add x0, x0, #1
    };

    stage("mapping guest page");
    if (!check(uc_mem_map(uc, base, pageSize, UC_PROT_ALL), "uc_mem_map")) {
        uc_close(uc);
        return 5;
    }
    stage("mapped guest page");

    stage("writing AArch64 instructions");
    if (!check(uc_mem_write(uc, base, code.data(), code.size()), "uc_mem_write")) {
        uc_close(uc);
        return 5;
    }
    stage("wrote AArch64 instructions");

    stage("starting AArch64 emulation");
    if (!check(uc_emu_start(uc, base, base + code.size(), 0, 2), "uc_emu_start")) {
        uc_close(uc);
        return 5;
    }
    stage("AArch64 emulation returned normally");

    uint64_t x0 = 0;
    stage("reading X0");
    if (!check(uc_reg_read(uc, UC_ARM64_REG_X0, &x0), "uc_reg_read(X0)")) {
        uc_close(uc);
        return 6;
    }
    stage("read X0");

    stage("closing ARM64 little-endian engine");
    if (!check(uc_close(uc), "uc_close(ARM64 little-endian)"))
        return 7;
    stage("closed ARM64 little-endian engine");

    if (x0 != 43) {
        std::fprintf(stderr,
                     "[arm64-smoke] ARM64 execution returned X0=%llu; expected 43.\n",
                     static_cast<unsigned long long>(x0));
        std::fflush(stderr);
        return 8;
    }

    std::printf("ARM64 Unicorn smoke passed: X0=%llu.\n",
                static_cast<unsigned long long>(x0));
    std::fflush(stdout);

#ifdef _WIN32
    if (exceptionHandler)
        RemoveVectoredExceptionHandler(exceptionHandler);
#endif

    return 0;
}
