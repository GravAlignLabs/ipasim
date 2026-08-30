// DarwinUlockSmoke.cpp: semantic/export checks for Darwin address-keyed
// ulock synchronization used by libdispatch.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

constexpr std::uint32_t UlCompareAndWait = 1;
constexpr std::uint32_t UlUnfairLock = 2;
constexpr std::uint32_t UlCompareAndWaitShared = 3;
constexpr std::uint32_t UlCompareAndWait64 = 5;
constexpr std::uint32_t UlfWakeAll = 0x00000100;
constexpr std::uint32_t UlfWakeThread = 0x00000200;
constexpr std::uint32_t UlfNoErrno = 0x01000000;

int fail(const char *Message) {
  std::fprintf(stderr, "[darwin-ulock-smoke] FAIL: %s\n", Message);
  return 1;
}

FARPROC requireExport(HMODULE Module, const char *Name) {
  FARPROC Proc = GetProcAddress(Module, Name);
  if (!Proc)
    std::fprintf(stderr, "[darwin-ulock-smoke] missing export: %s\n", Name);
  return Proc;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return fail("expected IpaSimDarwinHost.dll path");

  HMODULE Host = LoadLibraryA(argv[1]);
  if (!Host)
    return fail("could not load IpaSimDarwinHost.dll");

  using UlockWait = int (*)(std::uint32_t, void *, std::uint64_t,
                            std::uint32_t);
  using UlockWait2 = int (*)(std::uint32_t, void *, std::uint64_t,
                             std::uint64_t, std::uint64_t);
  using UlockWake = int (*)(std::uint32_t, void *, std::uint64_t);
  using DarwinError = int *(*)();

  auto Wait = reinterpret_cast<UlockWait>(requireExport(Host, "__ulock_wait"));
  auto Wait2 =
      reinterpret_cast<UlockWait2>(requireExport(Host, "__ulock_wait2"));
  auto Wake = reinterpret_cast<UlockWake>(requireExport(Host, "__ulock_wake"));
  auto ErrorPointer =
      reinterpret_cast<DarwinError>(requireExport(Host, "__error"));
  if (!Wait || !Wait2 || !Wake || !ErrorPointer) {
    FreeLibrary(Host);
    return 1;
  }

  int *HostErrno = ErrorPointer();
  if (!HostErrno) {
    FreeLibrary(Host);
    return fail("Darwin __error did not return errno storage");
  }

  alignas(8) std::uint64_t Word64 = 0x1122334455667788ULL;
  auto *Word32 = reinterpret_cast<std::uint32_t *>(&Word64);
  *Word32 = 0x55667788U;

  // ULF_NO_ERRNO returns the error value directly and does not use -1/errno.
  if (Wake(UlCompareAndWait | UlfNoErrno, Word32, 0) != -ENOENT) {
    FreeLibrary(Host);
    return fail("wake without waiters did not return -ENOENT");
  }

  if (Wait(0xff | UlfNoErrno, Word32, *Word32, 1) != -EINVAL) {
    FreeLibrary(Host);
    return fail("invalid wait opcode did not return -EINVAL");
  }

  // Without ULF_NO_ERRNO the normal POSIX -1/errno shape is required.
  *HostErrno = 0;
  if (Wait(0xff, Word32, *Word32, 1) != -1 || *HostErrno != EINVAL) {
    FreeLibrary(Host);
    return fail("errno-mode invalid wait did not set EINVAL");
  }

  // A changed value must not block.
  if (Wait(UlCompareAndWait | UlfNoErrno, Word32,
           static_cast<std::uint64_t>(*Word32 + 1U), 100000) != 0) {
    FreeLibrary(Host);
    return fail("compare mismatch did not return immediately");
  }

  // Matching values block until timeout and return direct -ETIMEDOUT in the
  // libdispatch ULF_NO_ERRNO mode.
  if (Wait(UlCompareAndWait | UlfNoErrno, Word32, *Word32, 1000) !=
      -ETIMEDOUT) {
    FreeLibrary(Host);
    return fail("timed compare wait did not return -ETIMEDOUT");
  }

  // wait2 uses a nanosecond timeout and shares the same address-wait semantics.
  if (Wait2(UlCompareAndWait64 | UlfNoErrno, &Word64, Word64, 1000000, 0) !=
      -ETIMEDOUT) {
    FreeLibrary(Host);
    return fail("ulock_wait2 nanosecond timeout did not expire correctly");
  }

  // Prove a real blocked waiter can be discovered and woken even when the
  // userspace word itself has not changed yet. This catches missed-wakeup bugs
  // that a simple compare mismatch test would not expose.
  std::atomic<int> WaitResult{123456};
  std::thread Waiter([&]() {
    WaitResult.store(
        Wait(UlCompareAndWait | UlfNoErrno, Word32, *Word32, 2000000),
        std::memory_order_release);
  });

  bool Woke = false;
  for (int Attempt = 0; Attempt != 1000; ++Attempt) {
    const int Rc = Wake(UlCompareAndWait | UlfNoErrno, Word32, 0);
    if (Rc == 0) {
      Woke = true;
      break;
    }
    if (Rc != -ENOENT) {
      Waiter.join();
      FreeLibrary(Host);
      return fail("wake returned an unexpected error while waiter registered");
    }
    Sleep(1);
  }
  Waiter.join();
  if (!Woke || WaitResult.load(std::memory_order_acquire) != 0) {
    FreeLibrary(Host);
    return fail("registered compare waiter was not woken cleanly");
  }

  // The same host wait/wake mechanism backs functional unfair-lock blocking.
  // Darwin turnstile priority donation is intentionally not fabricated here.
  std::uint32_t UnfairWord = 0x1234U;
  WaitResult.store(123456, std::memory_order_release);
  std::thread UnfairWaiter([&]() {
    WaitResult.store(
        Wait(UlUnfairLock | UlfNoErrno, &UnfairWord, UnfairWord, 2000000),
        std::memory_order_release);
  });

  Woke = false;
  for (int Attempt = 0; Attempt != 1000; ++Attempt) {
    const int Rc = Wake(UlUnfairLock | UlfNoErrno, &UnfairWord, 0);
    if (Rc == 0) {
      Woke = true;
      break;
    }
    if (Rc != -ENOENT) {
      UnfairWaiter.join();
      FreeLibrary(Host);
      return fail("unfair wake returned an unexpected error");
    }
    Sleep(1);
  }
  UnfairWaiter.join();
  if (!Woke || WaitResult.load(std::memory_order_acquire) != 0) {
    FreeLibrary(Host);
    return fail("functional unfair-lock waiter was not woken");
  }

  // Cross-process shared ulocks and thread-targeted wakes need guest process/
  // thread identity that this subsystem does not yet expose; fail explicitly.
  if (Wait(UlCompareAndWaitShared | UlfNoErrno, Word32, *Word32, 1) !=
      -EINVAL) {
    FreeLibrary(Host);
    return fail("shared ulock did not fail explicitly");
  }
  if (Wake(UlCompareAndWait | UlfWakeThread | UlfNoErrno, Word32, 1) !=
      -EINVAL) {
    FreeLibrary(Host);
    return fail("thread-targeted wake did not fail explicitly");
  }

  // Exercise the wake-all flag through a registered waiter as well.
  WaitResult.store(123456, std::memory_order_release);
  std::thread WakeAllWaiter([&]() {
    WaitResult.store(
        Wait(UlCompareAndWait | UlfNoErrno, Word32, *Word32, 2000000),
        std::memory_order_release);
  });
  Woke = false;
  for (int Attempt = 0; Attempt != 1000; ++Attempt) {
    const int Rc =
        Wake(UlCompareAndWait | UlfWakeAll | UlfNoErrno, Word32, 0);
    if (Rc == 0) {
      Woke = true;
      break;
    }
    if (Rc != -ENOENT) {
      WakeAllWaiter.join();
      FreeLibrary(Host);
      return fail("wake-all returned an unexpected error");
    }
    Sleep(1);
  }
  WakeAllWaiter.join();
  if (!Woke || WaitResult.load(std::memory_order_acquire) != 0) {
    FreeLibrary(Host);
    return fail("wake-all did not release the registered waiter");
  }

  std::printf("[darwin-ulock-smoke] wait/wake semantics passed\n");
  FreeLibrary(Host);
  return 0;
}
