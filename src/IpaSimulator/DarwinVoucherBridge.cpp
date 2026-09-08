// DarwinVoucherBridge.cpp: libkernel/libdispatch Mach-voucher semantics for the
// Windows host bridge.
//
// Darwin libdispatch registers a process-lifetime table of voucher callbacks
// through __libkernel_voucher_init. libkernel later forwards the public
// voucher_mach_msg_* operations through that table. Keep the ARM64 LP64 table
// layout exact and route guest function pointers through IpaSimLibrary's
// backcaller rather than ever executing ARM64 addresses as x64 code.
//
// The same subsystem also owns host_create_mach_voucher: creation is backed by
// IpaSimMachIpc's typed Mach namespace, validates the packed public recipe ABI,
// implements manager-independent COPY/REMOVE semantics, and fails explicitly
// for attribute-manager behavior that ipaSim does not yet model.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "DarwinGuestMemory.hpp"
#include "MachIpc.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <windows.h>

namespace {

struct DarwinLibkernelVoucherFunctions {
  std::uint64_t Version;
  void *VoucherMachMsgSet;
  void *VoucherMachMsgClear;
  void *VoucherMachMsgAdopt;
  void *VoucherMachMsgRevert;
  void *VoucherMachMsgFillAux;
};
static_assert(sizeof(DarwinLibkernelVoucherFunctions) == 48,
              "Darwin arm64 voucher callback table layout changed");
static_assert(offsetof(DarwinLibkernelVoucherFunctions, VoucherMachMsgSet) == 8,
              "Darwin voucher callback table pointer offset changed");

constexpr std::int32_t DarwinKernelSuccess = 0;
constexpr std::int32_t DarwinMigArrayTooLarge = -307;
constexpr std::uint32_t DarwinVoucherMaxRawRecipeArraySize = 5120;
constexpr std::uint64_t DarwinVoucherVersionWithFillAux = 3;
constexpr std::uintptr_t DarwinVoucherStateUnchanged =
    (std::numeric_limits<std::uintptr_t>::max)();

std::atomic<const DarwinLibkernelVoucherFunctions *> VoucherFunctions{nullptr};

bool isNativeExecutableCallback(void *Callback) {
  if (!Callback)
    return false;
  MEMORY_BASIC_INFORMATION Information{};
  if (VirtualQuery(Callback, &Information, sizeof(Information)) == 0 ||
      Information.State != MEM_COMMIT ||
      (Information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    return false;

  switch (Information.Protect & 0xffu) {
  case PAGE_EXECUTE:
  case PAGE_EXECUTE_READ:
  case PAGE_EXECUTE_READWRITE:
  case PAGE_EXECUTE_WRITECOPY:
    return true;
  default:
    return false;
  }
}

[[noreturn]] void failGuestCallback(const char *Reason, void *Callback) {
  std::fprintf(stderr, "[darwin-host-voucher] %s; callback=%p\n", Reason,
               Callback);
  std::fflush(stderr);
  RaiseFailFastException(nullptr, nullptr, 0);
  TerminateProcess(GetCurrentProcess(), 0xC0000409u);
  std::abort();
}

HMODULE requireCore(void *Callback) {
  HMODULE Core = GetModuleHandleW(L"IpaSimLibrary.dll");
  if (!Core)
    failGuestCallback("guest callback requires loaded IpaSimLibrary.dll",
                      Callback);
  return Core;
}

void invokeVoid1(void *Callback, void *Arg0) {
  if (!Callback)
    return;
  if (isNativeExecutableCallback(Callback)) {
    reinterpret_cast<void (*)(void *)>(Callback)(Arg0);
    return;
  }

  using BackCaller = void (*)(void *, void *);
  auto Call = reinterpret_cast<BackCaller>(
      GetProcAddress(requireCore(Callback), "ipaSim_callBack1"));
  if (!Call)
    failGuestCallback("IpaSimLibrary.dll is missing ipaSim_callBack1",
                      Callback);
  Call(Callback, Arg0);
}

std::uint64_t invokeReturn1(void *Callback, void *Arg0) {
  if (!Callback)
    return 0;
  if (isNativeExecutableCallback(Callback))
    return reinterpret_cast<std::uint64_t (*)(void *)>(Callback)(Arg0);

  using BackCaller = void *(*)(void *, void *);
  auto Call = reinterpret_cast<BackCaller>(
      GetProcAddress(requireCore(Callback), "ipaSim_callBack1r"));
  if (!Call)
    failGuestCallback("IpaSimLibrary.dll is missing ipaSim_callBack1r",
                      Callback);
  return reinterpret_cast<std::uintptr_t>(Call(Callback, Arg0));
}

std::uint64_t invokeReturn2(void *Callback, void *Arg0, std::uint64_t Arg1) {
  if (!Callback)
    return 0;
  if (isNativeExecutableCallback(Callback))
    return reinterpret_cast<std::uint64_t (*)(void *, std::uint64_t)>(Callback)(
        Arg0, Arg1);

  using BackCaller = void *(*)(void *, void *, void *);
  auto Call = reinterpret_cast<BackCaller>(
      GetProcAddress(requireCore(Callback), "ipaSim_callBack2r"));
  if (!Call)
    failGuestCallback("IpaSimLibrary.dll is missing ipaSim_callBack2r",
                      Callback);
  return reinterpret_cast<std::uintptr_t>(
      Call(Callback, Arg0, reinterpret_cast<void *>(Arg1)));
}

const DarwinLibkernelVoucherFunctions *registeredFunctions() {
  return VoucherFunctions.load(std::memory_order_acquire);
}

} // namespace

extern "C" {

// Apple's libkernel implementation stores this process-lifetime table pointer
// and returns KERN_SUCCESS. libdispatch supplies static storage, so retaining
// the pointer (rather than copying a guessed future table size) preserves the
// versioned ABI and allows newer versions to append fields safely.
__declspec(dllexport) std::int32_t
__libkernel_voucher_init(const DarwinLibkernelVoucherFunctions *Functions) {
  VoucherFunctions.store(Functions, std::memory_order_release);
  return DarwinKernelSuccess;
}

// arm64 libsyscall exposes mach_host_self() as a function returning a send right
// to the host kernel object. Keep that object typed inside the same Mach namespace
// used by task/message/voucher ports so the trap facade can reject arbitrary
// names with MACH_SEND_INVALID_DEST, matching XNU's host-port conversion path.
__declspec(dllexport) std::uint32_t mach_host_self(void) {
  return ipasim::mach::hostSelfPort();
}

// Model libsyscall's host_create_mach_voucher -> Mach-trap contract, not merely
// the deeper XNU host routine. Error ordering matters: host conversion happens
// first, the trap rejects oversized/copyin-faulting recipes before construction,
// and the voucher-name copyout happens only after successful construction.
// Consequently failed calls do not eagerly overwrite the caller's output slot.
__declspec(dllexport) std::int32_t
host_create_mach_voucher(std::uint32_t Host, const std::uint8_t *Recipes,
                         std::uint32_t RecipeSize, std::uint32_t *Voucher) {
  if (Host != ipasim::mach::hostSelfPort())
    return ipasim::mach::SendInvalidDestination;
  if (RecipeSize > DarwinVoucherMaxRawRecipeArraySize)
    return DarwinMigArrayTooLarge;
  if (RecipeSize != 0 &&
      !ipasim::darwinmem::readableSpan(Recipes, RecipeSize))
    return ipasim::mach::KernelMemoryError;

  ipasim::mach::PortName NewVoucher = ipasim::mach::PortNull;
  const ipasim::mach::KernelReturn Result =
      ipasim::mach::createVoucher(Host, Recipes, RecipeSize, &NewVoucher);
  if (Result == ipasim::mach::KernelInvalidHost)
    return ipasim::mach::SendInvalidDestination;
  if (Result != ipasim::mach::KernelSuccess)
    return Result;

  if (!Voucher ||
      !ipasim::darwinmem::writableSpan(Voucher, sizeof(*Voucher)))
    return ipasim::mach::KernelMemoryError;
  *Voucher = NewVoucher;
  return ipasim::mach::KernelSuccess;
}

__declspec(dllexport) std::int32_t voucher_mach_msg_set(void *Message) {
  const auto *Functions = registeredFunctions();
  if (!Functions || !Functions->VoucherMachMsgSet)
    return 0;
  return static_cast<std::int32_t>(
      invokeReturn1(Functions->VoucherMachMsgSet, Message));
}

__declspec(dllexport) void voucher_mach_msg_clear(void *Message) {
  const auto *Functions = registeredFunctions();
  if (Functions)
    invokeVoid1(Functions->VoucherMachMsgClear, Message);
}

__declspec(dllexport) void *voucher_mach_msg_adopt(void *Message) {
  const auto *Functions = registeredFunctions();
  if (!Functions || !Functions->VoucherMachMsgAdopt)
    return reinterpret_cast<void *>(DarwinVoucherStateUnchanged);
  return reinterpret_cast<void *>(static_cast<std::uintptr_t>(
      invokeReturn1(Functions->VoucherMachMsgAdopt, Message)));
}

__declspec(dllexport) void voucher_mach_msg_revert(void *State) {
  const auto *Functions = registeredFunctions();
  if (Functions)
    invokeVoid1(Functions->VoucherMachMsgRevert, State);
}

__declspec(dllexport) std::uint32_t voucher_mach_msg_fill_aux(void *AuxHeader,
                                                              std::uint32_t Size) {
  const auto *Functions = registeredFunctions();
  if (!Functions || Functions->Version < DarwinVoucherVersionWithFillAux ||
      !Functions->VoucherMachMsgFillAux)
    return 0;
  return static_cast<std::uint32_t>(
      invokeReturn2(Functions->VoucherMachMsgFillAux, AuxHeader, Size));
}

__declspec(dllexport) std::int32_t voucher_mach_msg_fill_aux_supported(void) {
  const auto *Functions = registeredFunctions();
  return Functions && Functions->Version >= DarwinVoucherVersionWithFillAux &&
                 Functions->VoucherMachMsgFillAux
             ? 1
             : 0;
}

} // extern "C"
