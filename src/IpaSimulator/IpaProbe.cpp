#include "HostImportInventoryV2.hpp"
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

    const char *imagePath = argv[execute ? 2 : 1];
    const char *runtimeRoot = argc == maxArgs ? argv[execute ? 3 : 2] : nullptr;

    if (runtimeRoot) {
        std::printf("[ipasim-probe] iOS runtime root: %s\n", runtimeRoot);

        // Inventory the complete simulator-host chained-import surface before
        // the normal loader runs. The simulator libsystem layers bind through
        // libsystem_sim_*_host proxy dylibs, which then re-export the macOS host
        // libSystem surface. The inventory recognizes both that proxy layer and
        // direct host install names, but remains read-only: it never patches an
        // import or turns a missing symbol into success.
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
