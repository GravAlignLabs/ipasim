// DarwinHostCompatBridge.cpp: simulator-host interposition entry points that
// sit above the already implemented Darwin kernel/platform subsystems.
//
// Real mappings are used where Windows or ipaSim has equivalent semantics.
// Interfaces that fundamentally depend on Darwin-only facilities fail visibly
// and explicitly; none return fabricated success merely to satisfy the loader.

#include "MachIpc.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <windows.h>

// QueryInterruptTimePrecise is exported by Kernel32 but is provided to desktop
// applications through the Windows SDK's Mincore import library. Keep the real
// API rather than replacing the clock with a lower-fidelity timer.
#pragma comment(lib, "Mincore.lib")

namespace {

constexpr std::size_t DarwinOsAllocOnceKeyMax = 100;

// XNU/libplatform define each alloc-once slot as Darwin `long` plus a pointer.
// Darwin LP64 long is 64-bit while Windows long remains 32-bit, so use
// intptr_t explicitly to preserve the target ABI rather than the host C type.
struct DarwinOsAllocOnceEntry {
  std::intptr_t Once;
  void *Ptr;
};
static_assert(sizeof(DarwinOsAllocOnceEntry) == 16,
              "Darwin _os_alloc_once_s layout changed unexpectedly");

void reportUnsupported(const char *Name) {
  std::fprintf(stderr,
               "[darwin-host-unsupported] %s has no faithful Windows semantic mapping\n",
               Name);
  std::fflush(stderr);
}

[[noreturn]] void failUnsupportedVoidBoundary(const char *Name) {
  reportUnsupported(Name);
  RaiseFailFastException(nullptr, nullptr, 0);
  TerminateProcess(GetCurrentProcess(), 0xC0000409u);
  std::abort();
}

std::uint32_t low32(std::uint64_t Value) {
  return static_cast<std::uint32_t>(Value & 0xffffffffULL);
}

std::uint32_t high32(std::uint64_t Value) {
  return static_cast<std::uint32_t>(Value >> 32);
}

} // namespace

extern "C" {

// XNU keeps process-lifetime os_alloc_once state in a 100-entry writable table.
// This is real guest-visible storage, not a success stub: the PE DATA export is
// mapped into Unicorn with the host bridge and remains writable for the process
// lifetime exactly like the Darwin global. Zero initialization is the required
// pre-allocation state. The __os_alloc_once function itself is intentionally not
// implemented until execution proves that separate compatibility boundary.
DarwinOsAllocOnceEntry _os_alloc_once_table[DarwinOsAllocOnceKeyMax] = {};

// csops/csops_audittoken expose XNU code-signing state. Windows Authenticode
// and PE trust policy are not equivalent to Darwin csflags/entitlement blobs,
// so pretending the current process is ad-hoc/platform signed would be worse
// than failing. Bind the ABI, report the unsupported semantic, and return the
// normal syscall failure shape with ENOTSUP if code reaches this boundary.
__declspec(dllexport) int
__interposition_sim_system_csops(int Pid, unsigned int Ops, void *UserAddress,
                                 std::size_t UserSize) {
  (void)Pid;
  (void)Ops;
  (void)UserAddress;
  (void)UserSize;
  reportUnsupported("csops");
  errno = ENOTSUP;
  return -1;
}

__declspec(dllexport) int __interposition_sim_system_csops_audittoken(
    int Pid, unsigned int Ops, void *UserAddress, std::size_t UserSize,
    const void *AuditToken) {
  (void)Pid;
  (void)Ops;
  (void)UserAddress;
  (void)UserSize;
  (void)AuditToken;
  reportUnsupported("csops_audittoken");
  errno = ENOTSUP;
  return -1;
}

// freadlink requires an fd opened on a Darwin symlink object itself. ipaSim's
// current filesystem descriptor namespace opens regular backing files and does
// not yet retain an O_SYMLINK/reparse-point descriptor contract. Return an
// explicit unsupported error rather than incorrectly returning a resolved path.
__declspec(dllexport) std::intptr_t
__interposition_sim_system_freadlink(int Fd, char *Buffer,
                                     std::size_t BufferSize) {
  (void)Fd;
  (void)Buffer;
  (void)BufferSize;
  reportUnsupported("freadlink");
  errno = ENOTSUP;
  return -1;
}

// Current XNU packs the 64-bit mach_msg2 trap interface into eight integer
// arguments. ipaSim's Mach subsystem already implements the scalar inline
// mach_msg_overwrite semantics, so unpack the fields required by that subsystem
// rather than treating mach_msg2 as a success-only shim.
__declspec(dllexport) std::int32_t
__interposition_sim_system_mach_msg2_internal(
    void *Data, std::uint64_t Option64, std::uint64_t BitsAndSendSize,
    std::uint64_t RemoteAndLocalPort, std::uint64_t VoucherAndId,
    std::uint64_t DescriptorCountAndReceiveName,
    std::uint64_t ReceiveSizeAndPriority, std::uint64_t Timeout) {
  (void)RemoteAndLocalPort;
  (void)VoucherAndId;

  // XNU's 64-bit option space retains MACH_SEND_MSG/MACH_RCV_MSG in the low
  // bits for scalar messages. Vector/auxiliary messages require descriptor and
  // auxiliary-buffer semantics that the current Mach subsystem deliberately
  // rejects instead of silently flattening.
  constexpr std::uint64_t Mach64MsgVector = 0x0000000100000000ULL;
  if ((Option64 & Mach64MsgVector) != 0) {
    return ipasim::mach::SendInvalidOptions;
  }

  // MACH_MSG2_SHIFT_ARGS(lo, hi) is (uint64_t(hi) << 32) | uint32_t(lo).
  // Keep this orientation exact: message bits/descriptors/receive size occupy
  // the low halves, while send size/receive name/priority occupy the high halves.
  const std::uint32_t SendSize = high32(BitsAndSendSize);
  const std::uint32_t DescriptorCount = low32(DescriptorCountAndReceiveName);
  const std::uint32_t ReceiveName = high32(DescriptorCountAndReceiveName);
  const std::uint32_t ReceiveSize = low32(ReceiveSizeAndPriority);
  const std::uint32_t Priority = high32(ReceiveSizeAndPriority);
  if (DescriptorCount != 0)
    return ipasim::mach::SendInvalidData;

  return ipasim::mach::messageOverwrite(
      static_cast<ipasim::mach::MessageHeader *>(Data),
      static_cast<ipasim::mach::MessageOption>(low32(Option64)), SendSize,
      ReceiveSize, ReceiveName, static_cast<std::uint32_t>(Timeout), Priority,
      nullptr, 0);
}

// record_system_event_as_kernel is an event-recording side effect. Route it to
// the Windows debugger stream and stderr with its category values preserved so
// the event is genuinely recorded and remains visible in CI/runtime logs.
__declspec(dllexport) int
__interposition_sim_system_record_system_event_as_kernel(
    std::uint32_t Type, std::uint32_t Subsystem, const char *Event,
    const char *Payload) {
  if (!Event || !Payload) {
    errno = EFAULT;
    return -1;
  }
  // Public XNU limits are 64 bytes for the event and 96 for the payload.
  if (std::strlen(Event) >= 64 || std::strlen(Payload) >= 96) {
    errno = EINVAL;
    return -1;
  }

  char Line[256] = {};
  const int Written = std::snprintf(
      Line, sizeof(Line),
      "[darwin-system-event] type=%u subsystem=%u event=%s payload=%s\n",
      Type, Subsystem, Event, Payload);
  if (Written <= 0 || static_cast<std::size_t>(Written) >= sizeof(Line)) {
    errno = EOVERFLOW;
    return -1;
  }
  OutputDebugStringA(Line);
  std::fputs(Line, stderr);
  std::fflush(stderr);
  return 0;
}

// libplatform's aligned8 primitive is a zero-comparison operation. Preserve the
// observable memcmp contract: zero for an all-zero range and positive for the
// first non-zero byte. The implementation is independent of host vector width.
__declspec(dllexport) int
__interposition_sim_system__platform_memcmp_zero_aligned8(
    const void *Buffer, std::size_t Size) {
  const auto *Bytes = static_cast<const unsigned char *>(Buffer);
  for (std::size_t Index = 0; Index < Size; ++Index) {
    if (Bytes[Index] != 0)
      return static_cast<int>(Bytes[Index]);
  }
  return 0;
}

// Workgroup creation requires joining Darwin work-interval/workgroup state and
// invoking a guest ARM64 start routine. The current native bridge does not yet
// have that subsystem. pthread-style functions can report ENOTSUP directly.
__declspec(dllexport) int
__interposition_sim_system_pthread_create_with_workgroup_np(
    void *Thread, void *Workgroup, const void *Attributes, void *StartRoutine,
    void *Argument) {
  (void)Thread;
  (void)Workgroup;
  (void)Attributes;
  (void)StartRoutine;
  (void)Argument;
  reportUnsupported("pthread_create_with_workgroup_np");
  return ENOTSUP;
}

// pthread_install_workgroup_functions_np is void, so returning fabricated
// success is impossible to make visible to its caller. Fail fast with a direct
// diagnostic if execution reaches it; merely binding the symbol remains safe.
__declspec(dllexport) void
__interposition_sim_system_pthread_install_workgroup_functions_np(
    const void *Functions) {
  (void)Functions;
  failUnsupportedVoidBoundary("pthread_install_workgroup_functions_np");
}

__declspec(dllexport) int
__interposition_sim_system_pthread_prefer_alternate_cluster_self(void) {
  reportUnsupported("pthread_prefer_alternate_cluster_self");
  return ENOTSUP;
}

} // extern "C"