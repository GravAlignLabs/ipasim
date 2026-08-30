// DarwinUlockBridge.cpp: Darwin __ulock_wait/__ulock_wake semantics for the
// Windows host bridge.
//
// XNU ulocks are address-keyed sleep/wake primitives used by libdispatch and
// os_unfair_lock. The synchronization property maps cleanly to a Windows host
// process, but Darwin also layers turnstile priority donation and Mach-thread
// owner bookkeeping on UL_UNFAIR_LOCK. ipaSim preserves the functional
// compare/wait/wake contract here; guest priority donation remains a scheduler
// concern and is not fabricated from Windows host-thread state.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {

constexpr std::uint32_t UlCompareAndWait = 1;
constexpr std::uint32_t UlUnfairLock = 2;
constexpr std::uint32_t UlCompareAndWaitShared = 3;
constexpr std::uint32_t UlUnfairLock64Shared = 4;
constexpr std::uint32_t UlCompareAndWait64 = 5;
constexpr std::uint32_t UlCompareAndWait64Shared = 6;

constexpr std::uint32_t UlfWakeAll = 0x00000100;
constexpr std::uint32_t UlfWakeThread = 0x00000200;
constexpr std::uint32_t UlfWakeAllowNonOwner = 0x00000400;
constexpr std::uint32_t UlfWaitWorkqDataContention = 0x00010000;
constexpr std::uint32_t UlfWaitCancelPoint = 0x00020000;
constexpr std::uint32_t UlfWaitAdaptiveSpin = 0x00040000;
constexpr std::uint32_t UlfNoErrno = 0x01000000;

constexpr std::uint32_t UlOpcodeMask = 0x000000ff;
constexpr std::uint32_t UlFlagsMask = 0xffffff00;
constexpr std::uint32_t UlfWaitMask =
    UlfNoErrno | UlfWaitWorkqDataContention | UlfWaitCancelPoint |
    UlfWaitAdaptiveSpin;
constexpr std::uint32_t UlfWakeMask =
    UlfNoErrno | UlfWakeAll | UlfWakeThread | UlfWakeAllowNonOwner;

struct UlockState {
  std::uint32_t Opcode = 0;
  std::size_t Width = 0;
  std::size_t Waiters = 0;
  std::size_t WakeCredits = 0;
  std::uint64_t WakeAllGeneration = 0;
  std::condition_variable Condition;
};

std::mutex UlockRegistryMutex;
std::unordered_map<void *, std::shared_ptr<UlockState>> UlockRegistry;

int returnError(int Error, bool NoErrno) {
  if (NoErrno)
    return -Error;
  errno = Error;
  return -1;
}

bool readAddressValue(void *Address, std::size_t Width, std::uint64_t &Value) {
  std::uint64_t Buffer = 0;
  SIZE_T BytesRead = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), Address, &Buffer, Width,
                         &BytesRead) ||
      BytesRead != Width) {
    return false;
  }

  Value = Width == sizeof(std::uint32_t)
              ? static_cast<std::uint32_t>(Buffer)
              : Buffer;
  return true;
}

struct OperationInfo {
  std::uint32_t Opcode = 0;
  std::size_t Width = 0;
  bool Unfair = false;
};

int decodeOperation(std::uint32_t Operation, OperationInfo &Info) {
  Info.Opcode = Operation & UlOpcodeMask;
  switch (Info.Opcode) {
  case UlCompareAndWait:
    Info.Width = sizeof(std::uint32_t);
    return 0;
  case UlUnfairLock:
    Info.Width = sizeof(std::uint32_t);
    Info.Unfair = true;
    return 0;
  case UlCompareAndWait64:
    Info.Width = sizeof(std::uint64_t);
    return 0;
  case UlCompareAndWaitShared:
  case UlUnfairLock64Shared:
  case UlCompareAndWait64Shared:
    // Windows process-local synchronization cannot honestly claim Darwin's
    // cross-process shared-memory ulock identity semantics.
    return EINVAL;
  default:
    return EINVAL;
  }
}

void removeWaiterLocked(void *Address,
                        const std::shared_ptr<UlockState> &State) {
  if (State->Waiters != 0)
    --State->Waiters;

  if (State->Waiters == 0) {
    auto It = UlockRegistry.find(Address);
    if (It != UlockRegistry.end() && It->second == State)
      UlockRegistry.erase(It);
  } else if (State->WakeCredits != 0) {
    // If a waiter timed out while a wake-one credit was racing with it, hand
    // that credit to another waiter instead of silently consuming the wake.
    State->Condition.notify_one();
  }
}

int ulockWaitNanoseconds(std::uint32_t Operation, void *Address,
                         std::uint64_t Expected, std::uint64_t TimeoutNs) {
  const std::uint32_t Flags = Operation & UlFlagsMask;
  const bool NoErrno = (Flags & UlfNoErrno) != 0;

  if ((Flags & UlfWaitMask) != Flags)
    return returnError(EINVAL, NoErrno);
  if (Flags & UlfWaitCancelPoint) {
    // Darwin makes this a pthread cancellation point. ipaSim does not yet have
    // a guest pthread cancellation subsystem, so do not claim that behavior.
    return returnError(EINVAL, NoErrno);
  }

  OperationInfo Info;
  if (const int Error = decodeOperation(Operation, Info))
    return returnError(Error, NoErrno);

  if (!Address ||
      (reinterpret_cast<std::uintptr_t>(Address) & (Info.Width - 1)) != 0)
    return returnError(EINVAL, NoErrno);

  if (Info.Width == sizeof(std::uint32_t))
    Expected = static_cast<std::uint32_t>(Expected);

  // XNU compares the userspace word before creating/incrementing its ulock
  // waiter record. Keep that ordering under the same registry lock used by
  // wake so a value mismatch is never transiently visible as a real waiter.
  std::unique_lock<std::mutex> Lock(UlockRegistryMutex);
  std::uint64_t Current = 0;
  if (!readAddressValue(Address, Info.Width, Current))
    return returnError(EFAULT, NoErrno);
  if (Current != Expected)
    return 0;

  std::shared_ptr<UlockState> State;
  auto It = UlockRegistry.find(Address);
  if (It == UlockRegistry.end()) {
    State = std::make_shared<UlockState>();
    State->Opcode = Info.Opcode;
    State->Width = Info.Width;
    UlockRegistry.emplace(Address, State);
  } else {
    State = It->second;
    if (State->Opcode != Info.Opcode)
      return returnError(EDOM, NoErrno);
    if (State->Width != Info.Width)
      return returnError(EINVAL, NoErrno);
  }

  ++State->Waiters;
  const std::uint64_t WakeAllGeneration = State->WakeAllGeneration;

  auto WasWoken = [&]() {
    return State->WakeCredits != 0 ||
           State->WakeAllGeneration != WakeAllGeneration;
  };

  bool TimedOut = false;
  if (TimeoutNs == 0) {
    State->Condition.wait(Lock, WasWoken);
  } else {
    using Rep = std::chrono::nanoseconds::rep;
    const auto MaxNs = static_cast<std::uint64_t>(
        (std::numeric_limits<Rep>::max)());
    const auto Duration = std::chrono::nanoseconds(
        static_cast<Rep>(TimeoutNs > MaxNs ? MaxNs : TimeoutNs));
    TimedOut = !State->Condition.wait_for(Lock, Duration, WasWoken);
  }

  if (!TimedOut && State->WakeCredits != 0)
    --State->WakeCredits;

  removeWaiterLocked(Address, State);
  const int Remaining = static_cast<int>(State->Waiters);
  Lock.unlock();

  if (TimedOut)
    return returnError(ETIMEDOUT, NoErrno);
  return Remaining;
}

} // namespace

extern "C" __declspec(dllexport) int
__ulock_wait(std::uint32_t Operation, void *Address, std::uint64_t Value,
             std::uint32_t TimeoutMicroseconds) {
  return ulockWaitNanoseconds(
      Operation, Address, Value,
      static_cast<std::uint64_t>(TimeoutMicroseconds) * 1000ULL);
}

extern "C" __declspec(dllexport) int
__ulock_wait2(std::uint32_t Operation, void *Address, std::uint64_t Value,
              std::uint64_t TimeoutNanoseconds, std::uint64_t Value2) {
  // XNU's current ulock_wait2 implementation routes the same supported
  // operations through the primary value/timeout path; value2 is reserved for
  // operation-specific extensions and is not consumed by those operations.
  (void)Value2;
  return ulockWaitNanoseconds(Operation, Address, Value, TimeoutNanoseconds);
}

extern "C" __declspec(dllexport) int
__ulock_wake(std::uint32_t Operation, void *Address,
             std::uint64_t WakeValue) {
  const std::uint32_t Flags = Operation & UlFlagsMask;
  const bool NoErrno = (Flags & UlfNoErrno) != 0;

  if ((Flags & UlfWakeMask) != Flags)
    return returnError(EINVAL, NoErrno);

  OperationInfo Info;
  if (const int Error = decodeOperation(Operation, Info))
    return returnError(Error, NoErrno);

  if (!Address ||
      (reinterpret_cast<std::uintptr_t>(Address) & (Info.Width - 1)) != 0)
    return returnError(EINVAL, NoErrno);

  if ((Flags & UlfWakeThread) && (Flags & UlfWakeAll))
    return returnError(EINVAL, NoErrno);
  if ((Flags & UlfWakeThread) && Info.Unfair)
    return returnError(EINVAL, NoErrno);
  if (Flags & UlfWakeThread) {
    // XNU targets a Mach thread-port name carried in WakeValue. The current
    // emulator has no guest thread-port selector for this subsystem.
    (void)WakeValue;
    return returnError(EINVAL, NoErrno);
  }
  if ((Flags & UlfWakeAllowNonOwner) && !Info.Unfair)
    return returnError(EINVAL, NoErrno);

  std::shared_ptr<UlockState> State;
  const bool WakeAll = (Flags & UlfWakeAll) != 0;
  {
    std::lock_guard<std::mutex> Guard(UlockRegistryMutex);
    auto It = UlockRegistry.find(Address);
    if (It == UlockRegistry.end() || It->second->Waiters == 0)
      return returnError(ENOENT, NoErrno);

    State = It->second;
    if (State->Opcode != Info.Opcode)
      return returnError(EDOM, NoErrno);

    if (WakeAll) {
      ++State->WakeAllGeneration;
    } else if (State->WakeCredits < State->Waiters) {
      ++State->WakeCredits;
    }
  }

  if (WakeAll)
    State->Condition.notify_all();
  else
    State->Condition.notify_one();

  return 0;
}
