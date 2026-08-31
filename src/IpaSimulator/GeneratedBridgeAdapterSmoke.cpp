#include "GeneratedBridgeAdapter.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime_adapter_fixture.inc"

namespace {

using namespace ipasim::bridge;

struct Pair64 {
    std::uint64_t first;
    std::uint64_t second;
};

struct FloatPair {
    float first;
    float second;
};

int gPointerIdentityCalls = 0;

extern "C" std::uint64_t hostAddU64(std::uint64_t left, std::uint64_t right) {
    return left + right;
}

extern "C" double hostMixed(std::uint64_t whole, double fraction) {
    return static_cast<double>(whole) + fraction;
}

extern "C" Pair64 hostRotatePair(Pair64 value) {
    return Pair64{value.second + 1, value.first + 2};
}

extern "C" FloatPair hostTransformHfa(FloatPair value) {
    return FloatPair{value.first + 0.5f, value.second * 2.0f};
}

extern "C" void* hostIdentityPointer(void* value) {
    ++gPointerIdentityCalls;
    return value;
}

extern "C" void hostStoreIndirect(void* result, std::uint64_t value) {
    auto* output = static_cast<std::uint64_t*>(result);
    *output = value;
}

template <typename T>
T readVectorLane(const SyntheticGuestState& guest, std::size_t index) {
    T value{};
    std::memcpy(&value, guest.v.at(index).data(), sizeof(value));
    return value;
}

template <typename T>
void writeVectorLane(SyntheticGuestState& guest, std::size_t index, T value) {
    guest.v.at(index).fill(0);
    std::memcpy(guest.v.at(index).data(), &value, sizeof(value));
}

HostFunction asHostFunction(auto function) {
    return reinterpret_cast<HostFunction>(function);
}

bool bindControlled(
    AdapterRegistry& registry,
    const char* symbol,
    HostFunction function) {
    std::string error;
    if (!registry.bindHostImplementation(
            symbol,
            function,
            BindingKind::ControlledTest,
            "GeneratedBridgeAdapterSmoke",
            &error)) {
        std::fprintf(
            stderr,
            "[generated-bridge-adapter-smoke] bind %s failed: %s\n",
            symbol,
            error.c_str());
        return false;
    }
    return true;
}

bool executeExpectedSuccess(
    AdapterRegistry& registry,
    const char* symbol,
    SyntheticGuestState& guest,
    const PointerValidator& validator = {}) {
    std::string error;
    if (!registry.execute(symbol, guest, validator, &error)) {
        std::fprintf(
            stderr,
            "[generated-bridge-adapter-smoke] execute %s failed: %s\n",
            symbol,
            error.c_str());
        return false;
    }
    return true;
}

bool proveRegistryPolicy(AdapterRegistry& registry) {
    std::puts("[generated-bridge-adapter-smoke] registry policy begin");
    const auto generated = makeGeneratedBridgeAdapterFixture();
    std::string error;
    if (!registry.registerAdapters(generated, &error)) {
        std::fprintf(
            stderr,
            "[generated-bridge-adapter-smoke] generated table registration failed: %s\n",
            error.c_str());
        return false;
    }
    if (registry.adapterCount() != generated.size() || generated.size() != 6) {
        std::fprintf(
            stderr,
            "[generated-bridge-adapter-smoke] expected 6 generated adapters, got %zu\n",
            registry.adapterCount());
        return false;
    }

    error.clear();
    if (registry.registerAdapter(generated.front(), &error)) {
        std::fprintf(stderr, "[generated-bridge-adapter-smoke] duplicate adapter was accepted\n");
        return false;
    }

    SyntheticGuestState unboundGuest;
    unboundGuest.x[0] = 1;
    unboundGuest.x[1] = 2;
    error.clear();
    if (registry.execute("_auto_add_u64", unboundGuest, {}, &error)) {
        std::fprintf(stderr, "[generated-bridge-adapter-smoke] unbound adapter executed\n");
        return false;
    }

    error.clear();
    if (registry.bindHostImplementation(
            "_does_not_exist",
            asHostFunction(hostAddU64),
            BindingKind::ControlledTest,
            "GeneratedBridgeAdapterSmoke",
            &error)) {
        std::fprintf(stderr, "[generated-bridge-adapter-smoke] unknown adapter accepted a binding\n");
        return false;
    }

    std::puts("[generated-bridge-adapter-smoke] registry policy passed");
    return true;
}

bool bindAll(AdapterRegistry& registry) {
    return
        bindControlled(registry, "_auto_add_u64", asHostFunction(hostAddU64)) &&
        bindControlled(registry, "_auto_mixed", asHostFunction(hostMixed)) &&
        bindControlled(registry, "_auto_pair", asHostFunction(hostRotatePair)) &&
        bindControlled(registry, "_auto_hfa", asHostFunction(hostTransformHfa)) &&
        bindControlled(registry, "_auto_identity_pointer", asHostFunction(hostIdentityPointer)) &&
        bindControlled(registry, "_auto_indirect_store", asHostFunction(hostStoreIndirect));
}

bool proveScalarAndMixed(AdapterRegistry& registry) {
    std::puts("[generated-bridge-adapter-smoke] scalar/mixed execution begin");

    SyntheticGuestState addGuest;
    addGuest.x[0] = 40;
    addGuest.x[1] = 2;
    if (!executeExpectedSuccess(registry, "_auto_add_u64", addGuest) || addGuest.x[0] != 42) {
        std::fprintf(
            stderr,
            "[generated-bridge-adapter-smoke] auto add expected x0=42, got %llu\n",
            static_cast<unsigned long long>(addGuest.x[0]));
        return false;
    }

    SyntheticGuestState mixedGuest;
    mixedGuest.x[0] = 40;
    writeVectorLane<double>(mixedGuest, 0, 2.5);
    if (!executeExpectedSuccess(registry, "_auto_mixed", mixedGuest)) {
        return false;
    }
    const double mixedResult = readVectorLane<double>(mixedGuest, 0);
    if (std::fabs(mixedResult - 42.5) > 0.000001) {
        std::fprintf(
            stderr,
            "[generated-bridge-adapter-smoke] mixed expected v0=42.5, got %.17g\n",
            mixedResult);
        return false;
    }

    std::puts("[generated-bridge-adapter-smoke] scalar/mixed execution passed");
    return true;
}

bool proveAggregateRepacking(AdapterRegistry& registry) {
    std::puts("[generated-bridge-adapter-smoke] aggregate execution begin");

    SyntheticGuestState pairGuest;
    pairGuest.x[0] = 10;
    pairGuest.x[1] = 20;
    if (!executeExpectedSuccess(registry, "_auto_pair", pairGuest)) {
        return false;
    }
    if (pairGuest.x[0] != 21 || pairGuest.x[1] != 12) {
        std::fprintf(
            stderr,
            "[generated-bridge-adapter-smoke] pair expected x0/x1=21/12, got %llu/%llu\n",
            static_cast<unsigned long long>(pairGuest.x[0]),
            static_cast<unsigned long long>(pairGuest.x[1]));
        return false;
    }

    SyntheticGuestState hfaGuest;
    writeVectorLane<float>(hfaGuest, 0, 1.5f);
    writeVectorLane<float>(hfaGuest, 1, 3.0f);
    if (!executeExpectedSuccess(registry, "_auto_hfa", hfaGuest)) {
        return false;
    }
    const float first = readVectorLane<float>(hfaGuest, 0);
    const float second = readVectorLane<float>(hfaGuest, 1);
    if (std::fabs(first - 2.0f) > 0.00001f || std::fabs(second - 6.0f) > 0.00001f) {
        std::fprintf(
            stderr,
            "[generated-bridge-adapter-smoke] HFA expected v0/v1=2/6, got %.9g/%.9g\n",
            static_cast<double>(first),
            static_cast<double>(second));
        return false;
    }

    std::puts("[generated-bridge-adapter-smoke] aggregate execution passed");
    return true;
}

bool provePointerGate(AdapterRegistry& registry) {
    std::puts("[generated-bridge-adapter-smoke] pointer validation begin");

    std::uint64_t controlledStorage = 0x1122334455667788ULL;
    const auto controlledAddress = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(&controlledStorage));

    SyntheticGuestState guest;
    guest.x[0] = controlledAddress;
    std::string error;
    gPointerIdentityCalls = 0;

    if (registry.execute("_auto_identity_pointer", guest, {}, &error)) {
        std::fprintf(stderr, "[generated-bridge-adapter-smoke] pointer adapter ran without validator\n");
        return false;
    }
    if (gPointerIdentityCalls != 0) {
        std::fprintf(stderr, "[generated-bridge-adapter-smoke] host pointer function ran before validation\n");
        return false;
    }

    const PointerValidator rejectAll = [](std::uint64_t, PointerUse) {
        return false;
    };
    error.clear();
    if (registry.execute("_auto_identity_pointer", guest, rejectAll, &error)) {
        std::fprintf(stderr, "[generated-bridge-adapter-smoke] rejected pointer still executed\n");
        return false;
    }
    if (gPointerIdentityCalls != 0) {
        std::fprintf(stderr, "[generated-bridge-adapter-smoke] host pointer function ran after rejection\n");
        return false;
    }

    const PointerValidator allowControlled = [controlledAddress](
        std::uint64_t address,
        PointerUse use) {
        return address == controlledAddress &&
            (use == PointerUse::Argument || use == PointerUse::ReturnedPointer);
    };
    if (!executeExpectedSuccess(
            registry,
            "_auto_identity_pointer",
            guest,
            allowControlled)) {
        return false;
    }
    if (guest.x[0] != controlledAddress || gPointerIdentityCalls != 1 ||
        controlledStorage != 0x1122334455667788ULL) {
        std::fprintf(stderr, "[generated-bridge-adapter-smoke] controlled pointer round-trip changed state\n");
        return false;
    }

    std::puts("[generated-bridge-adapter-smoke] pointer validation passed");
    return true;
}

bool proveIndirectResult(AdapterRegistry& registry) {
    std::puts("[generated-bridge-adapter-smoke] indirect result begin");

    std::uint64_t controlledResult = 0;
    const auto resultAddress = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(&controlledResult));

    SyntheticGuestState guest;
    guest.x[8] = resultAddress;
    guest.x[0] = 42;
    const PointerValidator validator = [resultAddress](
        std::uint64_t address,
        PointerUse use) {
        return address == resultAddress && use == PointerUse::IndirectResult;
    };
    if (!executeExpectedSuccess(
            registry,
            "_auto_indirect_store",
            guest,
            validator)) {
        return false;
    }
    if (controlledResult != 42 || guest.x[8] != resultAddress) {
        std::fprintf(
            stderr,
            "[generated-bridge-adapter-smoke] indirect result expected 42 with stable x8\n");
        return false;
    }

    std::puts("[generated-bridge-adapter-smoke] indirect result passed");
    return true;
}

} // namespace

int main() {
    std::puts("[generated-bridge-adapter-smoke] generated runtime adapter execution begin");

    AdapterRegistry registry;
    if (!proveRegistryPolicy(registry) ||
        !bindAll(registry) ||
        !proveScalarAndMixed(registry) ||
        !proveAggregateRepacking(registry) ||
        !provePointerGate(registry) ||
        !proveIndirectResult(registry)) {
        return 1;
    }

    std::puts("[generated-bridge-adapter-smoke] all generated runtime adapter proofs passed");
    return 0;
}
