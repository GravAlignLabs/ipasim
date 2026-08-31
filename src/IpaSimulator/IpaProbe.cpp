#include "HostImportInventoryV2.hpp"
#include "StaticSymbolAudit.hpp"
#include "StaticSymbolAuditSelfTest.hpp"
#include "ipasim/Probe.hpp"

#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    const bool execute = argc >= 2 && std::strcmp(argv[1], "--execute") == 0;
    const int minArgs = execute ? 3 : 2;
    const int maxArgs = execute ? 4 : 3;

    if (argc < minArgs || argc > maxArgs) {
        std::fprintf(stderr,
                     "Usage:\n"
                     "  IpaProbe.exe <path-to-extracted-Mach-O> [path-to-iOS-runtime-root]\n"
                     "  IpaProbe.exe --execute <path-to-extracted-Mach-O> [path-to-iOS-runtime-root]\n");
        return 64;
    }

    // Keep the audit's namespace rules executable, not merely documented. The
    // synthetic IPA workflow runs IpaProbe on Windows, so a regression that
    // accidentally turns two-level imports into a global symbol union fails the
    // same public path contributors use.
    if (!ipasim::probe::runStaticSymbolAuditSelfTest())
        return 70;

    const char *imagePath = argv[execute ? 2 : 1];
    const char *runtimeRoot = argc == maxArgs ? argv[execute ? 3 : 2] : nullptr;

    if (runtimeRoot) {
        std::printf("[ipasim-probe] iOS runtime root: %s\n", runtimeRoot);

        // Walk the app's complete dependency closure statically before the
        // normal dyld-style loader starts. Unlike a global symbol union, this
        // audit preserves Mach-O's two-level namespace: each positive ordinal
        // is checked only against the library named by that ordinal, including
        // ipaSim's explicit Darwin-host -> native bridge mapping. It is
        // diagnostic only and never turns a missing runtime binding into
        // success. The normal loader still supplies the authoritative first
        // execution boundary below.
        ipasim::probe::reportStaticClosureSymbolAudit(imagePath, runtimeRoot);

        // Retain the focused simulator-host inventory as a compact compatibility
        // checkpoint for the three libsystem_sim_* layers. The closure-wide
        // audit above covers the rest of the dependency graph; this historical
        // view remains useful for tracking that native host surface separately.
        ipasim::probe::reportDarwinHostImportInventoryV2(runtimeRoot);

        const int runtimeResult = ipaSim_setRuntimeRoot(runtimeRoot);
        if (runtimeResult != 0) {
            std::fprintf(stderr,
                         "[ipasim-probe] runtime root rejected with code %d.\n",
                         runtimeResult);
            return runtimeResult;
        }
    } else {
        // Deliberately leave the root unset. DynamicLoader will name the first
        // absolute iOS dependency that requires one instead of falling back to
        // the historical generated-wrapper tree.
        ipaSim_setRuntimeRoot("");
    }

    std::printf("[ipasim-probe] loading: %s\n", imagePath);

    if (execute) {
        uint64_t returnValue = 0;
        const int result = ipaSim_executeImage(imagePath, &returnValue);
        if (result != 0) {
            std::fprintf(stderr,
                         "[ipasim-probe] execution stopped with code %d.\n",
                         result);
            return result;
        }

        std::printf("[ipasim-probe] guest entry point returned X0=%llu.\n",
                    static_cast<unsigned long long>(returnValue));
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
