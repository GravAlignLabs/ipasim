#include "HostImportInventoryV2.hpp"
#include "StaticSymbolAudit.hpp"
#include "StaticSymbolAuditLegacyDyldInfo.hpp"
#include "StaticSymbolAuditSelfTest.hpp"
#include "ipasim/Probe.hpp"

#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    const bool execute = argc >= 2 && std::strcmp(argv[1], "--execute") == 0;
    const bool executeThreaded =
        argc >= 2 && std::strcmp(argv[1], "--execute-threaded") == 0;
    const bool executionMode = execute || executeThreaded;
    const int imageIndex = executionMode ? 2 : 1;

    if (argc <= imageIndex) {
        std::fprintf(stderr,
                     "Usage:\n"
                     "  IpaProbe.exe <path-to-extracted-Mach-O> [path-to-iOS-runtime-root]\n"
                     "  IpaProbe.exe --execute <path-to-extracted-Mach-O> [path-to-iOS-runtime-root]\n"
                     "  IpaProbe.exe --execute-threaded <path-to-extracted-Mach-O> [path-to-iOS-runtime-root]\n"
                     "  IpaProbe.exe [--execute|--execute-threaded] <path-to-extracted-Mach-O> --runtime-root-dir <path-to-iOS-runtime-root>\n"
                     "  IpaProbe.exe [--execute|--execute-threaded] <path-to-extracted-Mach-O> --runtime-root-dwarfs <RuntimeRoot.dwarfs> <IpaSimDwarfsReader.dll>\n");
        return 64;
    }

    const int runtimeArgCount = argc - (imageIndex + 1);
    // Preserve the existing single-positional-directory form used by the
    // published tester while giving storage/parity work an explicit selector.
    const bool legacyDirectoryRuntime = runtimeArgCount == 1;
    const bool explicitDirectoryRuntime =
        runtimeArgCount == 2 &&
        std::strcmp(argv[imageIndex + 1], "--runtime-root-dir") == 0;
    const bool directoryRuntime =
        legacyDirectoryRuntime || explicitDirectoryRuntime;
    const bool dwarfsRuntime =
        runtimeArgCount == 3 &&
        std::strcmp(argv[imageIndex + 1], "--runtime-root-dwarfs") == 0;
    if (runtimeArgCount != 0 && !directoryRuntime && !dwarfsRuntime) {
        std::fprintf(stderr,
                     "[ipasim-probe] invalid RuntimeRoot arguments. Use one legacy directory path, explicit --runtime-root-dir <directory>, or explicit --runtime-root-dwarfs <image> <reader-bridge>.\n");
        return 64;
    }

    // Keep the audit's namespace and parser rules executable, not merely
    // documented. Synthetic IPA validation runs IpaProbe on Windows, so both a
    // two-level namespace regression and loss of legacy LC_DYLD_INFO export
    // coverage fail the same public path contributors use.
    if (!ipasim::probe::runStaticSymbolAuditSelfTest() ||
        !ipasim::probe::runLegacyDyldInfoAuditSelfTest())
        return 70;

    const char *imagePath = argv[imageIndex];

    if (directoryRuntime) {
        const char *runtimeRoot = argv[imageIndex + (explicitDirectoryRuntime ? 2 : 1)];
        std::printf("[ipasim-probe] iOS runtime root: %s\n", runtimeRoot);
        std::fflush(stdout);

        ipasim::probe::reportStaticClosureSymbolAuditComplete(imagePath,
                                                              runtimeRoot);
        ipasim::probe::reportDarwinHostImportInventoryV2(runtimeRoot);

        const int runtimeResult = ipaSim_setRuntimeRoot(runtimeRoot);
        if (runtimeResult != 0) {
            std::fprintf(stderr,
                         "[ipasim-probe] runtime root rejected with code %d.\n",
                         runtimeResult);
            return runtimeResult;
        }
    } else if (dwarfsRuntime) {
        const char *runtimeImage = argv[imageIndex + 2];
        const char *readerBridge = argv[imageIndex + 3];
        std::printf("[ipasim-probe] DwarFS iOS runtime image: %s\n",
                    runtimeImage);
        std::printf("[ipasim-probe] DwarFS reader bridge: %s\n",
                    readerBridge);
        std::fflush(stdout);
        std::fprintf(
            stderr,
            "[ipasim-probe] NOTE: static closure and host-import inventory preflights are currently directory-backed and are not claimed in DwarFS mode; the real DynamicLoader result below remains authoritative for this storage experiment.\n");

        const int runtimeResult =
            ipaSim_setDwarfsRuntimeRoot(runtimeImage, readerBridge);
        if (runtimeResult != 0) {
            std::fprintf(stderr,
                         "[ipasim-probe] DwarFS runtime root rejected with code %d.\n",
                         runtimeResult);
            return runtimeResult;
        }
    } else {
        ipaSim_setRuntimeRoot("");
    }

    std::printf("[ipasim-probe] loading: %s\n", imagePath);
    std::fflush(stdout);

    if (executionMode) {
        uint64_t returnValue = 0;
        const int result = executeThreaded
                               ? ipaSim_executeImageThreaded(imagePath,
                                                            &returnValue)
                               : ipaSim_executeImage(imagePath, &returnValue);
        if (result != 0) {
            std::fprintf(stderr,
                         "[ipasim-probe] %s execution stopped with code %d.\n",
                         executeThreaded ? "threaded" : "guest", result);
            return result;
        }

        if (executeThreaded) {
            std::printf(
                "[ipasim-probe] threaded guest entry point returned X0=%llu.\n",
                static_cast<unsigned long long>(returnValue));
        } else {
            std::printf("[ipasim-probe] guest entry point returned X0=%llu.\n",
                        static_cast<unsigned long long>(returnValue));
        }
        return 0;
    }

    const int result = ipaSim_probeImage(imagePath);
    if (result != 0) {
        std::fprintf(stderr,
                     "[ipasim-probe] loader stopped with code %d before app execution.\n",
                     result);
        return result;
    }

    std::printf("[ipasim-probe] Mach-O load completed. Execution intentionally not started.\n");
    return 0;
}