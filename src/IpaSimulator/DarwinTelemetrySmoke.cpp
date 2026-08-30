// DarwinTelemetrySmoke.cpp: semantic/export checks for the Darwin telemetry(2)
// host boundary used by modern simulator libsystem/libdispatch.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <windows.h>

namespace {

int fail(const char *Message) {
  std::fprintf(stderr, "[darwin-telemetry-smoke] FAIL: %s\n", Message);
  return 1;
}

FARPROC requireExport(HMODULE Module, const char *Name) {
  FARPROC Proc = GetProcAddress(Module, Name);
  if (!Proc)
    std::fprintf(stderr, "[darwin-telemetry-smoke] missing export: %s\n", Name);
  return Proc;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return fail("expected IpaSimDarwinHost.dll path");

  HMODULE Host = LoadLibraryA(argv[1]);
  if (!Host)
    return fail("could not load IpaSimDarwinHost.dll");

  using Telemetry = int (*)(std::uint64_t, std::uint64_t, std::uint64_t,
                            std::uint64_t, std::uint64_t, std::uint64_t);
  using DarwinError = int *(*)();

  auto Call = reinterpret_cast<Telemetry>(requireExport(Host, "__telemetry"));
  auto ErrorPointer =
      reinterpret_cast<DarwinError>(requireExport(Host, "__error"));
  if (!Call || !ErrorPointer) {
    FreeLibrary(Host);
    return 1;
  }

  int *HostErrno = ErrorPointer();
  if (!HostErrno) {
    FreeLibrary(Host);
    return fail("Darwin __error did not return errno storage");
  }

  // XNU builds without CONFIG_TELEMETRY reject timer-event and PMI commands
  // with EINVAL. ipaSim deliberately claims that valid Darwin capability level
  // rather than pretending Windows provides microstackshot sampling.
  *HostErrno = 0;
  if (Call(1, 0x1122334455667788ULL, 17, 23, 29, 31) != -1 ||
      *HostErrno != EINVAL) {
    FreeLibrary(Host);
    return fail("timer-event telemetry did not fail with EINVAL");
  }

  *HostErrno = 0;
  if (Call(3, 1, 1000000, 0, 0, 0) != -1 || *HostErrno != EINVAL) {
    FreeLibrary(Host);
    return fail("PMI telemetry did not fail with EINVAL");
  }

  // MACH_PORT_NULL is a real voucher-name operation: it clears the current
  // thread's voucher association. Successful POSIX calls need not clear errno.
  *HostErrno = 77;
  if (Call(2, 0, 0, 0, 0, 0) != 0 || *HostErrno != 77) {
    FreeLibrary(Host);
    return fail("MACH_PORT_NULL voucher clear did not succeed cleanly");
  }

  // Non-null voucher names must not be accepted until ipaSim can resolve a real
  // Mach voucher right. MACH_PORT_DEAD is invalid on Darwin as well.
  *HostErrno = 0;
  if (Call(2, 0x123, 0, 0, 0, 0) != -1 || *HostErrno != EINVAL) {
    FreeLibrary(Host);
    return fail("unresolved voucher name did not fail explicitly");
  }

  *HostErrno = 0;
  if (Call(2, 0xffffffffULL, 0, 0, 0, 0) != -1 || *HostErrno != EINVAL) {
    FreeLibrary(Host);
    return fail("MACH_PORT_DEAD voucher name did not fail with EINVAL");
  }

  *HostErrno = 0;
  if (Call(0xffff, 0, 0, 0, 0, 0) != -1 || *HostErrno != EINVAL) {
    FreeLibrary(Host);
    return fail("unknown telemetry command did not fail with EINVAL");
  }

  std::printf("[darwin-telemetry-smoke] syscall semantics passed\n");
  FreeLibrary(Host);
  return 0;
}
