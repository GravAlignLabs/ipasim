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
    const int minArgs = executionMode ? 3 : 2;
    const int maxArgs = executionMode ? 4 : 3;

    if (argc < minArgs || argc > maxArgs) {
        std::fprintf(stderr,
                     "Usage:\n"
                     "  IpaProbe.exe <path-to-extracted-Mach-O> [path-to-iOS-runtime-root]\n"
                     "  IpaProbe.exe --execute <path-to-extracted-Mach-O> [path-to-iOS-runtime-root]\n"
                     "  IpaProbe.exe --execute-threaded <path-to-extracted-Mach-O> [path-to-iOS-runtime-root]\n");
        return 64;
    }

    // Keep the audit's namespace and parser rules executable, not merely
    // documented. Synthetic IPA validation runs IpaProbe on Windows, so both a
    // two-level namespace regression and loss of legacy LC_DYLD_INFO export
    // coverage fail the same public path contributors use.
    if (!ipasim::probe::runStaticSymbolAuditSelfTest() ||
        !ipasim::probe::runLegacyDyldInfoAuditSelfTest())
        return 70;

    const char *imagePath = argv[executionMode ? 2 : 1];
    const char *runtimeRoot =
        argc == maxArgs ? argv[executionMode ? 3 : 2] : nullptr;

    if (runtimeRoot) {
        std::printf("[ipasim-probe] iOS runtime root: %s\n", runtimeRoot);

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
    } else {
        ipaSim_setRuntimeRoot("");
    }

    std::printf("[ipasim-probe] loading: %s\n", imagePath);

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
