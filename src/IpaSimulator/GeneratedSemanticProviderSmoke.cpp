#include "GeneratedBridgeAdapter.hpp"
#include "GeneratedSemanticProvider.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <process.h>
#include <string>
#include <vector>

#include "semantic_provider_fixture.inc"

namespace {

using namespace ipasim::bridge;

bool registerGeneratedAdapter(AdapterRegistry& registry) {
    std::string error;
    const auto generated = makeGeneratedSemanticProviderFixture();
    if (generated.size() != 1 || generated.front().symbol != "_getpid") {
        std::fprintf(
            stderr,
            "[generated-semantic-provider-smoke] generated fixture is not the expected _getpid record\n");
        return false;
    }
    if (!registry.registerAdapters(generated, &error)) {
        std::fprintf(
            stderr,
            "[generated-semantic-provider-smoke] adapter registration failed: %s\n",
            error.c_str());
        return false;
    }
    return true;
}

bool proveMissingExportFailsClosed(
    const std::filesystem::path& bridgePath,
    AdapterRegistry& registry) {
    std::puts("[generated-semantic-provider-smoke] missing export rejection begin");

    SemanticProviderModule module;
    std::string error;
    const std::vector<SemanticProviderSpec> providers = {
        {"_getpid", "__ipasim_missing_semantic_export__", "negative-test"},
    };
    if (module.loadAndBind(bridgePath, registry, providers, &error)) {
        std::fprintf(
            stderr,
            "[generated-semantic-provider-smoke] missing export unexpectedly bound\n");
        return false;
    }
    if (module.loaded() || registry.hasBinding("_getpid") ||
        error.find("export is missing") == std::string::npos) {
        std::fprintf(
            stderr,
            "[generated-semantic-provider-smoke] missing export did not fail cleanly: %s\n",
            error.c_str());
        return false;
    }

    std::puts("[generated-semantic-provider-smoke] missing export rejection passed");
    return true;
}

bool proveDataExportCannotBecomeFunction(
    const std::filesystem::path& bridgePath,
    AdapterRegistry& registry) {
    std::puts("[generated-semantic-provider-smoke] data export rejection begin");

    // mach_task_self_ is deliberately a DATA export in DarwinHostBridge.def.
    // A symbol existing in the PE export table is therefore not sufficient to
    // make it callable through the generated function bridge.
    SemanticProviderModule module;
    std::string error;
    const std::vector<SemanticProviderSpec> providers = {
        {"_getpid", "mach_task_self_", "negative-data-export-test"},
    };
    if (module.loadAndBind(bridgePath, registry, providers, &error)) {
        std::fprintf(
            stderr,
            "[generated-semantic-provider-smoke] PE data export was accepted as callable code\n");
        return false;
    }
    if (module.loaded() || registry.hasBinding("_getpid") ||
        error.find("not executable code") == std::string::npos) {
        std::fprintf(
            stderr,
            "[generated-semantic-provider-smoke] data export did not fail cleanly: %s\n",
            error.c_str());
        return false;
    }

    std::puts("[generated-semantic-provider-smoke] data export rejection passed");
    return true;
}

bool proveRealSemanticProviderExecution(
    const std::filesystem::path& bridgePath,
    AdapterRegistry& registry) {
    std::puts("[generated-semantic-provider-smoke] real semantic provider execution begin");

    SemanticProviderModule module;
    std::string error;
    const std::vector<SemanticProviderSpec> providers = {
        {"_getpid", "getpid", "DarwinHostBridge.getpid"},
    };
    if (!module.loadAndBind(bridgePath, registry, providers, &error)) {
        std::fprintf(
            stderr,
            "[generated-semantic-provider-smoke] semantic provider binding failed: %s\n",
            error.c_str());
        return false;
    }
    if (!module.loaded() || !registry.hasBinding("_getpid")) {
        std::fprintf(
            stderr,
            "[generated-semantic-provider-smoke] semantic provider did not retain its module/binding\n");
        return false;
    }

    SyntheticGuestState guest;
    guest.x[0] = ~std::uint64_t{0};
    if (!registry.execute("_getpid", guest, {}, &error)) {
        std::fprintf(
            stderr,
            "[generated-semantic-provider-smoke] generated _getpid execution failed: %s\n",
            error.c_str());
        return false;
    }

    const auto expected = static_cast<std::uint32_t>(_getpid());
    if (guest.x[0] != expected) {
        std::fprintf(
            stderr,
            "[generated-semantic-provider-smoke] expected guest x0=%lu, got %llu\n",
            static_cast<unsigned long>(expected),
            static_cast<unsigned long long>(guest.x[0]));
        return false;
    }

    std::puts("[generated-semantic-provider-smoke] real semantic provider execution passed");
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::puts("[generated-semantic-provider-smoke] generated-to-real-provider proof begin");

    if (argc != 2 || !argv[1] || !*argv[1]) {
        std::fprintf(
            stderr,
            "[generated-semantic-provider-smoke] expected path to IpaSimDarwinHost.dll\n");
        return 2;
    }

    const std::filesystem::path bridgePath(argv[1]);
    AdapterRegistry registry;
    if (!registerGeneratedAdapter(registry) ||
        !proveMissingExportFailsClosed(bridgePath, registry) ||
        !proveDataExportCannotBecomeFunction(bridgePath, registry) ||
        !proveRealSemanticProviderExecution(bridgePath, registry)) {
        return 1;
    }

    std::puts("[generated-semantic-provider-smoke] all generated semantic provider proofs passed");
    return 0;
}
