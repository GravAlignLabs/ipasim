// DarwinVoucherSmoke.cpp: semantic checks for the libkernel/libdispatch
// voucher callback registration and Mach voucher creation boundaries.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <cstdio>
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
static_assert(sizeof(DarwinLibkernelVoucherFunctions) == 48);

#pragma pack(push, 1)
struct DarwinVoucherRecipe {
  std::uint32_t Key;
  std::uint32_t Command;
  std::uint32_t PreviousVoucher;
  std::uint32_t ContentSize;
};
#pragma pack(pop)
static_assert(sizeof(DarwinVoucherRecipe) == 16);

constexpr std::int32_t KernelSuccess = 0;
constexpr std::int32_t KernelInvalidArgument = 4;
constexpr std::int32_t KernelMemoryError = 10;
constexpr std::int32_t KernelInvalidCapability = 20;
constexpr std::int32_t KernelNotSupported = 46;
constexpr std::int32_t MigArrayTooLarge = -307;
constexpr std::int32_t MachSendInvalidDestination = 0x10000003;
constexpr std::uint32_t VoucherKeyAll = 0xffffffffU;
constexpr std::uint32_t VoucherKeyBank = 3;
constexpr std::uint32_t VoucherCommandCopy = 1;

int SetCalls = 0;
int ClearCalls = 0;
int AdoptCalls = 0;
int RevertCalls = 0;
int FillAuxCalls = 0;
void *ExpectedMessage = reinterpret_cast<void *>(0x12340000ULL);
void *ExpectedState = reinterpret_cast<void *>(0x56780000ULL);
void *ObservedRevertState = nullptr;

std::uint64_t testSet(void *Message) {
  ++SetCalls;
  return Message == ExpectedMessage ? 1 : 0;
}

void testClear(void *Message) {
  if (Message == ExpectedMessage)
    ++ClearCalls;
}

std::uint64_t testAdopt(void *Message) {
  ++AdoptCalls;
  return Message == ExpectedMessage
             ? reinterpret_cast<std::uintptr_t>(ExpectedState)
             : (std::numeric_limits<std::uintptr_t>::max)();
}

void testRevert(void *State) {
  ++RevertCalls;
  ObservedRevertState = State;
}

std::uint64_t testFillAux(void *AuxHeader, std::uint64_t Size) {
  ++FillAuxCalls;
  if (!AuxHeader || Size < sizeof(std::uint32_t))
    return 0;
  *static_cast<std::uint32_t *>(AuxHeader) = 0xA11CE55u;
  return sizeof(std::uint32_t);
}

int fail(const char *Message) {
  std::fprintf(stderr, "[darwin-voucher-smoke] FAIL: %s\n", Message);
  return 1;
}

FARPROC requireExport(HMODULE Module, const char *Name) {
  FARPROC Proc = GetProcAddress(Module, Name);
  if (!Proc)
    std::fprintf(stderr, "[darwin-voucher-smoke] missing export: %s\n", Name);
  return Proc;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return fail("expected IpaSimDarwinHost.dll path");

  HMODULE Host = LoadLibraryA(argv[1]);
  if (!Host)
    return fail("could not load IpaSimDarwinHost.dll");

  using Init = std::int32_t (*)(const DarwinLibkernelVoucherFunctions *);
  using MachHostSelf = std::uint32_t (*)();
  using HostCreateVoucher = std::int32_t (*)(std::uint32_t, const std::uint8_t *,
                                              std::uint32_t, std::uint32_t *);
  using Set = std::int32_t (*)(void *);
  using Clear = void (*)(void *);
  using Adopt = void *(*)(void *);
  using Revert = void (*)(void *);
  using FillAux = std::uint32_t (*)(void *, std::uint32_t);
  using FillAuxSupported = std::int32_t (*)();

  auto Initialize = reinterpret_cast<Init>(requireExport(Host, "__libkernel_voucher_init"));
  auto GetHost = reinterpret_cast<MachHostSelf>(requireExport(Host, "mach_host_self"));
  auto CreateVoucher = reinterpret_cast<HostCreateVoucher>(
      requireExport(Host, "host_create_mach_voucher"));
  auto SetVoucher = reinterpret_cast<Set>(requireExport(Host, "voucher_mach_msg_set"));
  auto ClearVoucher = reinterpret_cast<Clear>(requireExport(Host, "voucher_mach_msg_clear"));
  auto AdoptVoucher = reinterpret_cast<Adopt>(requireExport(Host, "voucher_mach_msg_adopt"));
  auto RevertVoucher = reinterpret_cast<Revert>(requireExport(Host, "voucher_mach_msg_revert"));
  auto FillVoucherAux = reinterpret_cast<FillAux>(requireExport(Host, "voucher_mach_msg_fill_aux"));
  auto IsFillAuxSupported = reinterpret_cast<FillAuxSupported>(
      requireExport(Host, "voucher_mach_msg_fill_aux_supported"));
  if (!Initialize || !GetHost || !CreateVoucher || !SetVoucher ||
      !ClearVoucher || !AdoptVoucher || !RevertVoucher || !FillVoucherAux ||
      !IsFillAuxSupported) {
    FreeLibrary(Host);
    return 1;
  }

  const std::uint32_t HostPort = GetHost();
  if (HostPort == 0 || GetHost() != HostPort) {
    FreeLibrary(Host);
    return fail("mach_host_self did not return one stable host port name");
  }

  std::uint32_t Voucher = 0xfeedbeefU;
  if (CreateVoucher(0, nullptr, 0, &Voucher) != MachSendInvalidDestination ||
      Voucher != 0xfeedbeefU) {
    FreeLibrary(Host);
    return fail("invalid host did not return MACH_SEND_INVALID_DEST without copyout");
  }

  Voucher = 0xfeedbeefU;
  if (CreateVoucher(HostPort, nullptr, 0, &Voucher) != KernelSuccess ||
      Voucher != 0) {
    FreeLibrary(Host);
    return fail("empty voucher recipe did not preserve XNU null-voucher success");
  }

  DarwinVoucherRecipe CopyEmpty{
      VoucherKeyAll,
      VoucherCommandCopy,
      0,
      0,
  };
  if (CreateVoucher(HostPort,
                    reinterpret_cast<const std::uint8_t *>(&CopyEmpty),
                    sizeof(CopyEmpty), &Voucher) != KernelSuccess ||
      Voucher == 0) {
    FreeLibrary(Host);
    return fail("COPY recipe did not create a real voucher port identity");
  }
  const std::uint32_t FirstVoucher = Voucher;

  DarwinVoucherRecipe CopyExisting{
      VoucherKeyAll,
      VoucherCommandCopy,
      FirstVoucher,
      0,
  };
  Voucher = 0;
  if (CreateVoucher(HostPort,
                    reinterpret_cast<const std::uint8_t *>(&CopyExisting),
                    sizeof(CopyExisting), &Voucher) != KernelSuccess ||
      Voucher == 0) {
    FreeLibrary(Host);
    return fail("COPY recipe did not resolve a previous voucher capability");
  }

  DarwinVoucherRecipe InvalidPrevious = CopyExisting;
  InvalidPrevious.PreviousVoucher = 0xfffffff0U;
  Voucher = 0xfeedbeefU;
  if (CreateVoucher(HostPort,
                    reinterpret_cast<const std::uint8_t *>(&InvalidPrevious),
                    sizeof(InvalidPrevious), &Voucher) != KernelInvalidCapability ||
      Voucher != 0xfeedbeefU) {
    FreeLibrary(Host);
    return fail("invalid previous voucher did not fail without copyout");
  }

  DarwinVoucherRecipe Malformed = CopyEmpty;
  Malformed.ContentSize = 1;
  Voucher = 0xfeedbeefU;
  if (CreateVoucher(HostPort,
                    reinterpret_cast<const std::uint8_t *>(&Malformed),
                    sizeof(Malformed), &Voucher) != KernelInvalidArgument ||
      Voucher != 0xfeedbeefU) {
    FreeLibrary(Host);
    return fail("truncated voucher recipe content was not rejected without copyout");
  }

  DarwinVoucherRecipe ManagerSpecific{
      VoucherKeyBank,
      0x7fffffffU,
      0,
      0,
  };
  Voucher = 0xfeedbeefU;
  if (CreateVoucher(HostPort,
                    reinterpret_cast<const std::uint8_t *>(&ManagerSpecific),
                    sizeof(ManagerSpecific), &Voucher) != KernelNotSupported ||
      Voucher != 0xfeedbeefU) {
    FreeLibrary(Host);
    return fail("unsupported voucher attribute-manager command did not fail closed");
  }

  std::uint8_t OversizedRecipes[5121]{};
  Voucher = 0xfeedbeefU;
  if (CreateVoucher(HostPort, OversizedRecipes, sizeof(OversizedRecipes),
                    &Voucher) != MigArrayTooLarge ||
      Voucher != 0xfeedbeefU) {
    FreeLibrary(Host);
    return fail("oversized raw voucher recipe did not return MIG_ARRAY_TOO_LARGE");
  }

  Voucher = 0xfeedbeefU;
  if (CreateVoucher(HostPort, reinterpret_cast<const std::uint8_t *>(1),
                    sizeof(DarwinVoucherRecipe), &Voucher) != KernelMemoryError ||
      Voucher != 0xfeedbeefU) {
    FreeLibrary(Host);
    return fail("unreadable guest voucher recipe did not return KERN_MEMORY_ERROR");
  }
  if (CreateVoucher(HostPort, nullptr, 0, nullptr) != KernelMemoryError) {
    FreeLibrary(Host);
    return fail("null voucher output did not return KERN_MEMORY_ERROR");
  }

  DarwinLibkernelVoucherFunctions Version1{
      1,
      reinterpret_cast<void *>(&testSet),
      reinterpret_cast<void *>(&testClear),
      reinterpret_cast<void *>(&testAdopt),
      reinterpret_cast<void *>(&testRevert),
      nullptr,
  };
  if (Initialize(&Version1) != 0) {
    FreeLibrary(Host);
    return fail("version-1 __libkernel_voucher_init did not return KERN_SUCCESS");
  }
  if (SetVoucher(ExpectedMessage) != 1 || SetCalls != 1) {
    FreeLibrary(Host);
    return fail("voucher_mach_msg_set did not forward to registered callback");
  }
  ClearVoucher(ExpectedMessage);
  if (ClearCalls != 1) {
    FreeLibrary(Host);
    return fail("voucher_mach_msg_clear did not forward to registered callback");
  }
  if (AdoptVoucher(ExpectedMessage) != ExpectedState || AdoptCalls != 1) {
    FreeLibrary(Host);
    return fail("voucher_mach_msg_adopt did not preserve pointer-width state");
  }
  RevertVoucher(ExpectedState);
  if (RevertCalls != 1 || ObservedRevertState != ExpectedState) {
    FreeLibrary(Host);
    return fail("voucher_mach_msg_revert did not preserve state pointer");
  }
  std::uint32_t AuxWord = 0;
  if (IsFillAuxSupported() != 0 || FillVoucherAux(&AuxWord, sizeof(AuxWord)) != 0 ||
      FillAuxCalls != 0) {
    FreeLibrary(Host);
    return fail("version-1 table incorrectly exposed version-3 fill_aux");
  }

  DarwinLibkernelVoucherFunctions Version3 = Version1;
  Version3.Version = 3;
  Version3.VoucherMachMsgFillAux = reinterpret_cast<void *>(&testFillAux);
  if (Initialize(&Version3) != 0 || IsFillAuxSupported() != 1) {
    FreeLibrary(Host);
    return fail("version-3 voucher callback table was not registered");
  }
  if (FillVoucherAux(&AuxWord, sizeof(AuxWord)) != sizeof(std::uint32_t) ||
      AuxWord != 0xA11CE55u || FillAuxCalls != 1) {
    FreeLibrary(Host);
    return fail("version-3 fill_aux callback did not preserve arguments/return");
  }

  std::printf("[darwin-voucher-smoke] creation/callback semantics passed\n");
  FreeLibrary(Host);
  return 0;
}
