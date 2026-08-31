// DarwinPthreadWorkqueueBridge.cpp: Darwin pthread workqueue compatibility for
// libdispatch on the Windows host.
//
// XNU normally owns Darwin's workqueue thread pool and invokes libdispatch's
// registered worker callback on kernel-created pthreads. ipaSim has one shared
// ARM64 Unicorn execution context, so concurrent host threads cannot safely run
// guest callbacks. Model the same request/callback contract as a serialized
// cooperative scheduler: requests are queued, the registered worker callback is
// actually executed through ipaSim's existing guest backcaller, and recursive
// worker requests are appended for the outer drain instead of nesting Unicorn.
// This preserves functional dispatch progress without fabricating parallel guest
// execution that the emulator cannot yet isolate.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace {

using DarwinPriority = std::uint64_t;
using MachPort = std::uint32_t;

// pthread/workqueue_private.h
constexpr std::uint32_t WorkqFeatureDispatchFunc = 0x01;
constexpr std::uint32_t WorkqFeatureFinePriority = 0x02;
constexpr std::uint32_t WorkqFeatureMaintenance = 0x10;
constexpr std::uint32_t WorkqFeatureKevent = 0x40;
constexpr std::uint32_t WorkqFeatureWorkloop = 0x80;
constexpr std::uint32_t WorkqFeatureCooperative = 0x100;

constexpr std::uint32_t WorkqueueConfigVersion = 2;
constexpr std::uint32_t WorkqueueConfigMinimumVersion = 1;
constexpr std::uint32_t WorkqueueConfigSupportedFlags = 0;

// Darwin errno ABI values used explicitly where the Windows CRT can differ.
constexpr int DarwinErrnoPermission = 1;   // EPERM
constexpr int DarwinErrnoNoSuchProcess = 3; // ESRCH
constexpr int DarwinErrnoBusy = 16;        // EBUSY
constexpr int DarwinErrnoInvalid = 22;     // EINVAL
constexpr int DarwinErrnoTryAgain = 35;    // EAGAIN
constexpr int DarwinErrnoNotSupported = 45; // ENOTSUP
constexpr int DarwinErrnoStale = 70;       // ESTALE
constexpr int DarwinErrnoFault = 14;       // EFAULT

constexpr std::size_t MaximumPendingWorkerRequests = 65536;

struct DarwinWorkqueueConfig {
  std::uint32_t Flags;
  std::uint32_t Version;
  void *KeventCallback;
  void *WorkloopCallback;
  void *WorkqueueCallback;
  std::uint64_t QueueSerialNumberOffset;
  std::uint64_t QueueLabelOffset;
};
static_assert(sizeof(DarwinWorkqueueConfig) == 48,
              "Darwin arm64 pthread_workqueue_config layout changed");
static_assert(offsetof(DarwinWorkqueueConfig, QueueLabelOffset) == 40,
              "Darwin pthread_workqueue_config v1 boundary changed");

struct OverrideState {
  std::uint32_t Count = 0;
  DarwinPriority PeakPriority = 0;
};

struct WorkqueueState {
  bool Configured = false;
  bool Draining = false;
  void *WorkqueueCallback = nullptr;
  void *KeventCallback = nullptr;
  void *WorkloopCallback = nullptr;
  std::uint64_t QueueSerialNumberOffset = 0;
  std::uint64_t QueueLabelOffset = 0;
  DarwinPriority EventManagerPriority = 0;
  std::deque<DarwinPriority> PendingWorkers;
  std::unordered_map<MachPort, OverrideState> Overrides;
};

std::mutex StateMutex;
WorkqueueState State;

// We implement a dispatch callback, fine-priority propagation and the
// maintenance QoS class. KEVENT/WORKLOOP delivery and cooperative workqueues
// require additional event-source semantics, so do not advertise them merely
// because their symbols exist elsewhere in the bridge.
constexpr std::uint32_t SupportedFeatures =
    WorkqFeatureDispatchFunc | WorkqFeatureFinePriority |
    WorkqFeatureMaintenance;
static_assert((SupportedFeatures &
               (WorkqFeatureKevent | WorkqFeatureWorkloop |
                WorkqFeatureCooperative)) == 0,
              "Do not advertise unimplemented workqueue delivery modes");

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
  std::fprintf(stderr, "[darwin-host-workqueue] %s; callback=%p\n", Reason,
               Callback);
  std::fflush(stderr);
  RaiseFailFastException(nullptr, nullptr, 0);
  TerminateProcess(GetCurrentProcess(), 0xC0000409u);
  std::abort();
}

HMODULE requireCore(void *Callback) {
  HMODULE Core = GetModuleHandleW(L"IpaSimLibrary.dll");
  if (!Core)
    failGuestCallback("guest worker requires loaded IpaSimLibrary.dll",
                      Callback);
  return Core;
}

void invokeWorker(void *Callback, DarwinPriority Priority) {
  if (!Callback)
    return;

  if (isNativeExecutableCallback(Callback)) {
    reinterpret_cast<void (*)(DarwinPriority)>(Callback)(Priority);
    return;
  }

  // The guest callback has one AAPCS64 integer-class argument. The existing
  // backcaller writes it to X0 and starts guest execution only after the outer
  // Unicorn run has stopped at the host boundary.
  using BackCaller = void (*)(void *, void *);
  auto Call = reinterpret_cast<BackCaller>(
      GetProcAddress(requireCore(Callback), "ipaSim_callBack1"));
  if (!Call)
    failGuestCallback("IpaSimLibrary.dll is missing ipaSim_callBack1",
                      Callback);
  Call(Callback, reinterpret_cast<void *>(
                     static_cast<std::uintptr_t>(Priority)));
}

bool validCompactPriority(DarwinPriority Priority) {
  // pthread_priority_t is unsigned long on Darwin LP64, but Apple's compact
  // workqueue encoding currently occupies the low 32 bits. Reject data in the
  // high half rather than silently truncating it at the Windows boundary.
  return (Priority >> 32) == 0;
}

int queueWorkers(std::int32_t NumThreads, DarwinPriority Priority) {
  const int SavedErrno = errno;

  if (NumThreads <= 0 || !validCompactPriority(Priority))
    return DarwinErrnoInvalid;

  void *Callback = nullptr;
  {
    std::lock_guard<std::mutex> Guard(StateMutex);
    if (!State.Configured || !State.WorkqueueCallback)
      return DarwinErrnoPermission;
    if (static_cast<std::size_t>(NumThreads) >
        MaximumPendingWorkerRequests - State.PendingWorkers.size())
      return DarwinErrnoTryAgain;

    for (std::int32_t I = 0; I < NumThreads; ++I)
      State.PendingWorkers.push_back(Priority);

    // Recursive requests from libdispatch while a worker callback is draining
    // are real requests, but must not recursively enter Unicorn. The active
    // outer drain will consume them before it returns.
    if (State.Draining) {
      errno = SavedErrno;
      return 0;
    }
    State.Draining = true;
  }

  for (;;) {
    DarwinPriority NextPriority = 0;
    {
      std::lock_guard<std::mutex> Guard(StateMutex);
      if (State.PendingWorkers.empty()) {
        State.Draining = false;
        errno = SavedErrno;
        return 0;
      }
      NextPriority = State.PendingWorkers.front();
      State.PendingWorkers.pop_front();
      Callback = State.WorkqueueCallback;
    }

    // Never hold StateMutex across guest execution. libdispatch may request
    // more workers, manipulate overrides, or update manager priority from the
    // worker callback itself.
    invokeWorker(Callback, NextPriority);
  }
}

int startWorkqueueOverride(MachPort Thread, DarwinPriority Priority) {
  if (Thread == 0)
    return DarwinErrnoNoSuchProcess;
  if (!validCompactPriority(Priority) || Priority == 0)
    return DarwinErrnoInvalid;

  std::lock_guard<std::mutex> Guard(StateMutex);
  OverrideState &Override = State.Overrides[Thread];
  if (Override.Count == (std::numeric_limits<std::uint32_t>::max)())
    return EOVERFLOW;
  ++Override.Count;
  if (Priority > Override.PeakPriority)
    Override.PeakPriority = Priority;
  return 0;
}

int validateUlockOwner(MachPort Thread, MachPort *UlockAddress) {
  if (!UlockAddress)
    return 0;
  if ((reinterpret_cast<std::uintptr_t>(UlockAddress) & 0x3u) != 0)
    return DarwinErrnoInvalid;

  std::uint32_t Value = 0;
  SIZE_T BytesRead = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), UlockAddress, &Value,
                         sizeof(Value), &BytesRead) ||
      BytesRead != sizeof(Value))
    return DarwinErrnoFault;

  // Darwin's userland ulock_owner_value_to_port_name() reconstructs the Mach
  // port name by restoring the low two ipc-entry bits.
  if ((Value | 0x3u) != Thread)
    return DarwinErrnoStale;
  return 0;
}

} // namespace

extern "C" {

__declspec(dllexport) int
pthread_workqueue_setup(const DarwinWorkqueueConfig *Config,
                        std::size_t ConfigSize) {
  const int SavedErrno = errno;

  if (!Config || ConfigSize < sizeof(std::uint32_t))
    return DarwinErrnoInvalid;

  std::size_t MinimumSize = 0;
  switch (Config->Version) {
  case 1:
    MinimumSize = offsetof(DarwinWorkqueueConfig, QueueLabelOffset);
    break;
  case WorkqueueConfigVersion:
    MinimumSize = sizeof(DarwinWorkqueueConfig);
    break;
  default:
    return DarwinErrnoInvalid;
  }

  if (ConfigSize < MinimumSize)
    return DarwinErrnoInvalid;
  if ((Config->Flags & ~WorkqueueConfigSupportedFlags) != 0 ||
      Config->Version < WorkqueueConfigMinimumVersion)
    return DarwinErrnoNotSupported;

  std::lock_guard<std::mutex> Guard(StateMutex);
  if (State.Configured)
    return DarwinErrnoBusy;

  State.Configured = true;
  State.WorkqueueCallback = Config->WorkqueueCallback;
  State.KeventCallback = Config->KeventCallback;
  State.WorkloopCallback = Config->WorkloopCallback;
  State.QueueSerialNumberOffset = Config->QueueSerialNumberOffset;
  State.QueueLabelOffset = Config->Version >= 2 ? Config->QueueLabelOffset : 0;
  errno = SavedErrno;
  return 0;
}

__declspec(dllexport) int _pthread_workqueue_supported(void) {
  return static_cast<int>(SupportedFeatures);
}

__declspec(dllexport) int
_pthread_workqueue_addthreads(std::int32_t NumThreads, DarwinPriority Priority) {
  return queueWorkers(NumThreads, Priority);
}

__declspec(dllexport) bool
_pthread_workqueue_should_narrow(DarwinPriority Priority) {
  (void)Priority;
  // The cooperative executor has one runnable guest lane. There is no smaller
  // width to narrow to, so the truthful scheduler answer is always false.
  return false;
}

__declspec(dllexport) int
_pthread_workqueue_set_event_manager_priority(DarwinPriority Priority) {
  const int SavedErrno = errno;
  if (!validCompactPriority(Priority))
    return DarwinErrnoInvalid;

  {
    std::lock_guard<std::mutex> Guard(StateMutex);
    State.EventManagerPriority = Priority;
  }
  errno = SavedErrno;
  return 0;
}

__declspec(dllexport) int
_pthread_workqueue_override_start_direct(MachPort Thread,
                                         DarwinPriority Priority) {
  const int SavedErrno = errno;
  const int Result = startWorkqueueOverride(Thread, Priority);
  if (Result == 0)
    errno = SavedErrno;
  return Result;
}

__declspec(dllexport) int
_pthread_workqueue_override_start_direct_check_owner(
    MachPort Thread, DarwinPriority Priority, MachPort *UlockAddress) {
  const int SavedErrno = errno;
  if (Thread == 0)
    return DarwinErrnoNoSuchProcess;
  if (!validCompactPriority(Priority) || Priority == 0)
    return DarwinErrnoInvalid;
  if (const int OwnerResult = validateUlockOwner(Thread, UlockAddress))
    return OwnerResult;

  const int Result = startWorkqueueOverride(Thread, Priority);
  if (Result == 0)
    errno = SavedErrno;
  return Result;
}

__declspec(dllexport) int _pthread_workqueue_override_reset(void) {
  const int SavedErrno = errno;
  {
    std::lock_guard<std::mutex> Guard(StateMutex);
    // ipaSim currently has a single serialized guest execution lane. All
    // dispatch workqueue overrides therefore belong to that runnable lane, so
    // reset has the same visible effect as XNU's current-workqueue-thread reset.
    State.Overrides.clear();
  }
  errno = SavedErrno;
  return 0;
}

} // extern "C"
