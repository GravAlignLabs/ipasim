#include <ffi.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

struct SyntheticArm64State {
    std::array<std::uint64_t, 9> x{};
    alignas(16) std::array<std::array<unsigned char, 16>, 8> v{};
};

template <typename T>
T readVectorLane(const SyntheticArm64State& state, std::size_t index) {
    static_assert(sizeof(T) <= 16, "synthetic SIMD lane value is too large");
    T value{};
    std::memcpy(&value, state.v.at(index).data(), sizeof(T));
    return value;
}

template <typename T>
void writeVectorLane(SyntheticArm64State& state, std::size_t index, T value) {
    static_assert(sizeof(T) <= 16, "synthetic SIMD lane value is too large");
    state.v.at(index).fill(0);
    std::memcpy(state.v.at(index).data(), &value, sizeof(T));
}

bool prepareCif(
    const char* label,
    ffi_cif& cif,
    ffi_type* resultType,
    unsigned argumentCount,
    ffi_type** argumentTypes) {
    const ffi_status status = ffi_prep_cif(
        &cif,
        FFI_DEFAULT_ABI,
        argumentCount,
        resultType,
        argumentTypes);
    if (status == FFI_OK) {
        return true;
    }
    std::fprintf(
        stderr,
        "[libffi-bridge-smoke] %s: ffi_prep_cif failed with status %d\n",
        label,
        static_cast<int>(status));
    return false;
}

extern "C" std::uint64_t hostAddU64(std::uint64_t left, std::uint64_t right) {
    return left + right;
}

extern "C" double hostMixedScalar(std::uint64_t whole, double fraction) {
    return static_cast<double>(whole) + fraction;
}

extern "C" void* hostIdentityPointer(void* value) {
    return value;
}

extern "C" void hostStoreIndirect(void* result, std::uint64_t value) {
    auto* output = static_cast<std::uint64_t*>(result);
    *output = value;
}

struct Pair64 {
    std::uint64_t first;
    std::uint64_t second;
};

extern "C" Pair64 hostRotatePair(Pair64 value) {
    return Pair64{value.second + 1, value.first + 2};
}

struct FloatPair {
    float first;
    float second;
};

extern "C" FloatPair hostTransformFloatPair(FloatPair value) {
    return FloatPair{value.first + 0.5f, value.second * 2.0f};
}

bool proveIntegerRegisters() {
    SyntheticArm64State guest;
    guest.x[0] = 40;
    guest.x[1] = 2;

    std::uint64_t left = guest.x[0];
    std::uint64_t right = guest.x[1];
    std::uint64_t result = 0;
    ffi_type* argumentTypes[] = {&ffi_type_uint64, &ffi_type_uint64};
    void* arguments[] = {&left, &right};
    ffi_cif cif{};
    if (!prepareCif("integer-registers", cif, &ffi_type_uint64, 2, argumentTypes)) {
        return false;
    }

    ffi_call(&cif, FFI_FN(hostAddU64), &result, arguments);
    guest.x[0] = result;

    if (guest.x[0] != 42) {
        std::fprintf(
            stderr,
            "[libffi-bridge-smoke] integer-registers: expected x0=42, got %llu\n",
            static_cast<unsigned long long>(guest.x[0]));
        return false;
    }
    return true;
}

bool proveMixedRegisterBanks() {
    // AAPCS64 supplies these values independently in x0 and v0. Win64 uses
    // positional slots, so libffi must marshal the second argument through
    // XMM1 rather than treating the guest v0 location as a host ABI location.
    SyntheticArm64State guest;
    guest.x[0] = 40;
    writeVectorLane<double>(guest, 0, 2.5);

    std::uint64_t whole = guest.x[0];
    double fraction = readVectorLane<double>(guest, 0);
    double result = 0.0;
    ffi_type* argumentTypes[] = {&ffi_type_uint64, &ffi_type_double};
    void* arguments[] = {&whole, &fraction};
    ffi_cif cif{};
    if (!prepareCif("mixed-register-banks", cif, &ffi_type_double, 2, argumentTypes)) {
        return false;
    }

    ffi_call(&cif, FFI_FN(hostMixedScalar), &result, arguments);
    writeVectorLane<double>(guest, 0, result);

    const double committed = readVectorLane<double>(guest, 0);
    if (std::fabs(committed - 42.5) > 0.000001) {
        std::fprintf(
            stderr,
            "[libffi-bridge-smoke] mixed-register-banks: expected v0=42.5, got %.17g\n",
            committed);
        return false;
    }
    return true;
}

bool proveOpaquePointerRoundTrip() {
    // The host function never dereferences the guest-carried address. The only
    // pointer admitted by this proof belongs to this process and is checked for
    // exact identity after libffi returns it.
    SyntheticArm64State guest;
    std::uint64_t controlledStorage = 0x1122334455667788ULL;
    guest.x[0] = reinterpret_cast<std::uint64_t>(&controlledStorage);

    void* pointerValue = reinterpret_cast<void*>(guest.x[0]);
    void* result = nullptr;
    ffi_type* argumentTypes[] = {&ffi_type_pointer};
    void* arguments[] = {&pointerValue};
    ffi_cif cif{};
    if (!prepareCif("opaque-pointer", cif, &ffi_type_pointer, 1, argumentTypes)) {
        return false;
    }

    ffi_call(&cif, FFI_FN(hostIdentityPointer), &result, arguments);
    guest.x[0] = reinterpret_cast<std::uint64_t>(result);

    if (guest.x[0] != reinterpret_cast<std::uint64_t>(&controlledStorage)) {
        std::fprintf(stderr, "[libffi-bridge-smoke] opaque-pointer: pointer identity changed\n");
        return false;
    }
    if (controlledStorage != 0x1122334455667788ULL) {
        std::fprintf(stderr, "[libffi-bridge-smoke] opaque-pointer: storage was unexpectedly modified\n");
        return false;
    }
    return true;
}

bool proveIndirectResultPointer() {
    // PR #45 canonicalizes an ARM64 x8 indirect-result pointer into the first
    // logical host carrier argument. This uses only an owned result buffer.
    SyntheticArm64State guest;
    std::uint64_t controlledResult = 0;
    guest.x[8] = reinterpret_cast<std::uint64_t>(&controlledResult);
    guest.x[0] = 42;

    void* resultPointer = reinterpret_cast<void*>(guest.x[8]);
    std::uint64_t value = guest.x[0];
    ffi_type* argumentTypes[] = {&ffi_type_pointer, &ffi_type_uint64};
    void* arguments[] = {&resultPointer, &value};
    ffi_cif cif{};
    if (!prepareCif("x8-indirect-result", cif, &ffi_type_void, 2, argumentTypes)) {
        return false;
    }

    ffi_call(&cif, FFI_FN(hostStoreIndirect), nullptr, arguments);

    if (controlledResult != 42) {
        std::fprintf(
            stderr,
            "[libffi-bridge-smoke] x8-indirect-result: expected owned result=42, got %llu\n",
            static_cast<unsigned long long>(controlledResult));
        return false;
    }
    if (guest.x[8] != reinterpret_cast<std::uint64_t>(&controlledResult)) {
        std::fprintf(stderr, "[libffi-bridge-smoke] x8-indirect-result: x8 pointer changed\n");
        return false;
    }
    return true;
}

bool proveTwoWordAggregate() {
    // The guest aggregate is captured from x0/x1 into canonical storage. The
    // Win64 libffi backend then owns the ABI-specific by-value/sret mechanics.
    SyntheticArm64State guest;
    guest.x[0] = 10;
    guest.x[1] = 20;

    ffi_type* pairElements[] = {&ffi_type_uint64, &ffi_type_uint64, nullptr};
    ffi_type pairType{};
    pairType.type = FFI_TYPE_STRUCT;
    pairType.elements = pairElements;

    Pair64 input{guest.x[0], guest.x[1]};
    Pair64 result{};
    ffi_type* argumentTypes[] = {&pairType};
    void* arguments[] = {&input};
    ffi_cif cif{};
    if (!prepareCif("two-word-aggregate", cif, &pairType, 1, argumentTypes)) {
        return false;
    }

    ffi_call(&cif, FFI_FN(hostRotatePair), &result, arguments);
    guest.x[0] = result.first;
    guest.x[1] = result.second;

    if (guest.x[0] != 21 || guest.x[1] != 12) {
        std::fprintf(
            stderr,
            "[libffi-bridge-smoke] two-word-aggregate: expected x0/x1=21/12, got %llu/%llu\n",
            static_cast<unsigned long long>(guest.x[0]),
            static_cast<unsigned long long>(guest.x[1]));
        return false;
    }
    return true;
}

bool proveHfaRepack() {
    // A two-float HFA occupies v0/v1 on AAPCS64, but Win64 treats this 8-byte
    // aggregate as an integer-class carrier. Canonical struct storage lets
    // libffi perform the host repack while the guest side remains explicit.
    SyntheticArm64State guest;
    writeVectorLane<float>(guest, 0, 1.5f);
    writeVectorLane<float>(guest, 1, 3.0f);

    ffi_type* pairElements[] = {&ffi_type_float, &ffi_type_float, nullptr};
    ffi_type pairType{};
    pairType.type = FFI_TYPE_STRUCT;
    pairType.elements = pairElements;

    FloatPair input{
        readVectorLane<float>(guest, 0),
        readVectorLane<float>(guest, 1)};
    FloatPair result{};
    ffi_type* argumentTypes[] = {&pairType};
    void* arguments[] = {&input};
    ffi_cif cif{};
    if (!prepareCif("hfa-repack", cif, &pairType, 1, argumentTypes)) {
        return false;
    }

    ffi_call(&cif, FFI_FN(hostTransformFloatPair), &result, arguments);
    writeVectorLane<float>(guest, 0, result.first);
    writeVectorLane<float>(guest, 1, result.second);

    const float first = readVectorLane<float>(guest, 0);
    const float second = readVectorLane<float>(guest, 1);
    if (std::fabs(first - 2.0f) > 0.00001f || std::fabs(second - 6.0f) > 0.00001f) {
        std::fprintf(
            stderr,
            "[libffi-bridge-smoke] hfa-repack: expected v0/v1=2/6, got %.9g/%.9g\n",
            static_cast<double>(first),
            static_cast<double>(second));
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::puts("[libffi-bridge-smoke] controlled Win64 libffi bridge proof begin");

    if (!proveIntegerRegisters() ||
        !proveMixedRegisterBanks() ||
        !proveOpaquePointerRoundTrip() ||
        !proveIndirectResultPointer() ||
        !proveTwoWordAggregate() ||
        !proveHfaRepack()) {
        return 1;
    }

    std::puts("[libffi-bridge-smoke] all controlled bridge proofs passed");
    return 0;
}
