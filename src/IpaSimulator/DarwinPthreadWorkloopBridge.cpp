// DarwinPthreadWorkloopBridge.cpp: Darwin pthread workqueue/workloop semantics
// for the iOS Simulator pthread host boundary.
//
// ipaSim has one ARM64 Unicorn execution context today. A Windows thread pool
// cannot safely run several guest worker callbacks at once against that shared
// context, so the host boundary implements a serialized cooperative workqueue:
// libdispatch can register its real worker callback and request workers, and
// those requests are drained through IpaSimLibrary's existing ARM64 backcaller
// outside the active Unicorn run. Nested requests are queued rather than
// recursively re-entering the emulator. This executes real dispatch worker
// code without pretending the emulator already supports parallel guest threads.
//
// Kevent/workloop worker delivery is deliberately not advertised in the
// supported-feature mask yet. The separate _pthread_workloop_create/destroy
// control-plane API remains stateful for binaries that reference it directly.

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
#include <new>
#include <unordered_map>

namespace {

using DarwinPriority = std::uint64_t;

// private/pthread/workqueue_private.h
constexpr int WorkqFeatureDispatchFunc = 0x01;
constexpr int WorkqFeatureFinePriority = 0x02;
constexpr int WorkqFeatureMaintenance = 0x10;
constexpr int WorkqFeatureKevent = 0x40;
constexpr int WorkqFeatureWorkloop = 0x80;
constexpr int WorkqFeatureCooperative = 0x100;
constexpr int SupportedWorkqueueFeatures =
    WorkqFeatureDispatchFunc | WorkqFeatureFinePriority |
    WorkqFeatureMaintenance;

constexpr std::uint32_t WorkqueueConfigVersion = 2;
constexpr std::uint32_t WorkqueueConfigMinVersion = 1;
constexpr std::uint32_t WorkqueueConfigSupportedFlags = 0;

constexpr int WorkqHighPriorityQueue = 0;
constexpr int WorkqDefaultPriorityQueue = 1;
constexpr int WorkqLowPriorityQueue = 2;
constexpr int WorkqBackgroundPriorityQueue = 3;
constexpr int WorkqNonInteractivePriorityQueue = 128;
constexpr int WorkqAddthreadsOptionOvercommit = 0x00000001;

constexpr DarwinPriority PriorityOvercommitFlag = 0x80000000ULL;
constexpr DarwinPriority PriorityCooperativeFlag = 0x08000000ULL;
constexpr unsigned PriorityQosShift = 8;
constexpr DarwinPriority PriorityQosMask = 0x00003f00ULL;

constexpr std::uint8_t ThreadQosBackground = 2;
constexpr std::uint8_t ThreadQosUtility = 3;
constexpr std::uint8_t ThreadQosLegacy = 4;
constexpr std::uint8_t ThreadQosUserInitiated = 5;
constexpr std::uint8_t ThreadQosUserInteractive = 6;

// Darwin LP64 pthread_workqueue_config. The public SPI is fixed-width through
// pointer fields, so do not use Windows pthread declarations here.
struct DarwinWorkqueueConfig {
  std::uint32_t Flags;
  std::uint32_t Version;
  void *KeventCallback;
  void *WorkloopCallback;
  void *WorkerCallback;
  std::uint64_t QueueSerialNumberOffset;
  std::uint64_t QueueLabelOffset;
};
static_assert(sizeof(DarwinWorkqueueConfig) == 48,
              "Darwin workqueue config LP64 layout changed");
static_assert(offsetof(DarwinWorkqueueConfig, QueueLabelOffset) == 40,
              "Darwin workqueue config v1 size changed");

// Darwin LP64 pthread_attr_t layout from libpthread's pthread_attr_s. Keep the
// guest ABI explicit instead of relying on Windows' LLP64 long/bitfield layout.
struct DarwinPthreadAttr {
  std::int64_t Sig;
  std::uint64_t GuardSize;
  std::uint64_t StackAddress;
  std::uint64_t StackSize;
  struct {
    std::int32_t SchedPriority;
    std::int32_t Quantum;
  } SchedParam;
  std::uint32_t Flags;
  std::uint32_t CpuConfig;
  std::uint32_t Reserved[4];
};

static_assert(sizeof(DarwinPthreadAttr) == 64,
              "Darwin LP64 pthread_attr_t must remain 64 bytes");
static_assert(offsetof(DarwinPthreadAttr, Flags) == 40,
              "Darwin pthread_attr_t flags offset changed");
static_assert(offsetof(DarwinPthreadAttr, CpuConfig) == 44,
              "Darwin pthread_attr_t CPU config offset changed");

constexpr std::uint32_t AttrPolicyMask = 0x00ff0000u;
constexpr unsigned AttrPolicyShift = 16;
constexpr std::uint32_t AttrSchedSet = 1u << 24;
constexpr std::uint32_t AttrPolicySet = 1u << 26;
constexpr std::uint32_t AttrCpuPercentSet = 1u << 27;

constexpr std::uint32_t CpuPercentMask = 0x000000ffu;
constexpr unsigned CpuRefillShift = 8;

// mach/policy.h: POLICY_TIMESHARE=1, POLICY_RR=2, POLICY_FIFO=4.
constexpr std::uint32_t PolicyTimeshare = 1;
constexpr std::uint32_t PolicyRoundRobin = 2;
constexpr std::uint32_t PolicyFifo = 4;

constexpr std::int32_t MinimumSchedPriority = 1;
constexpr std::int32_t MaximumSchedPriority = 63;
constexpr std::uint32_t MinimumCpuPercent = 1;
constexpr std::uint32_t MaximumCpuPercent = 100;
constexpr std::uint32_t MinimumRefillMilliseconds = 1;

struct WorkloopState {
  bool HasPriority = false;
  std::int32_t Priority = 0;
  bool HasPolicy = false;
  std::uint32_t Policy = 0;
  bool HasCpuLimit = false;
  std::uint32_t CpuPercent = 0;
  std::uint32_t RefillMilliseconds = 0;
};

struct WorkRequest {
  DarwinPriority Priority = 0;
  int LegacyQueuePriority = WorkqDefaultPriorityQueue;
  int LegacyOptions = 0;
};

struct Registry {
  std::mutex Mutex;
  std::unordered_map<std::uint64_t, WorkloopState> Workloops;

  void *WorkerCallback = nullptr;
  void *KeventCallback = nullptr;
  void *WorkloopCallback = nullptr;
  bool LegacyWorker = false;
  int DispatchOffset = 0;
  std::uint64_t QueueSerialNumberOffset = 0;
  std::uint64_t QueueLabelOffset = 0;
  DarwinPriority EventManagerPriority = 0;
  bool KillEnabled = false;

  std::deque<WorkRequest> PendingWorkers;
  bool DrainingWorkers = false;
};

Registry &registry() {
  // Keep state alive for the process lifetime. The host bridge is unloadable,
  // and a normal C++ static destructor must not run after its code is unmapped.
  static Registry *State = new Registry();
  return *State;
}

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
    failGuestCallback("guest callback requires loaded IpaSimLibrary.dll",
                      Callback);
  return Core;
}

void invokeVoid1(void *Callback, std::uint64_t Arg0) {
  if (isNativeExecutableCallback(Callback)) {
    reinterpret_cast<void (*)(std::uint64_t)>(Callback)(Arg0);
    return;
  }

  using BackCaller = void (*)(void *, void *);
  auto Call = reinterpret_cast<BackCaller>(
      GetProcAddress(requireCore(Callback), "ipaSim_callBack1"));
  if (!Call)
    failGuestCallback("IpaSimLibrary.dll is missing ipaSim_callBack1",
                      Callback);
  Call(Callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(Arg0)));
}

void invokeVoid3(void *Callback, std::uint64_t Arg0, std::uint64_t Arg1,
                 std::uint64_t Arg2) {
  if (isNativeExecutableCallback(Callback)) {
    reinterpret_cast<void (*)(std::uint64_t, std::uint64_t, std::uint64_t)>(
        Callback)(Arg0, Arg1, Arg2);
    return;
  }

  using BackCaller = void (*)(void *, void *, void *, void *);
  auto Call = reinterpret_cast<BackCaller>(
      GetProcAddress(requireCore(Callback), "ipaSim_callBack3"));
  if (!Call)
    failGuestCallback("IpaSimLibrary.dll is missing ipaSim_callBack3",
                      Callback);
  Call(Callback, reinterpret_cast<void *>(static_cast<std::uintptr_t>(Arg0)),
       reinterpret_cast<void *>(static_cast<std::uintptr_t>(Arg1)),
       reinterpret_cast<void *>(static_cast<std::uintptr_t>(Arg2)));
}

std::uint8_t priorityThreadQos(DarwinPriority Priority) {
  const DarwinPriority Bits = (Priority & PriorityQosMask) >> PriorityQosShift;
  for (std::uint8_t Qos = 1; Qos <= ThreadQosUserInteractive; ++Qos) {
    if (Bits & (DarwinPriority{1} << (Qos - 1)))
      return Qos;
  }
  return 0;
}

DarwinPriority makePriority(std::uint8_t Qos, DarwinPriority Flags) {
  DarwinPriority Result = Flags;
  if (Qos != 0) {
    Result |= DarwinPriority{1} << (PriorityQosShift + Qos - 1);
    // libpthread encodes a relative priority of zero as 0xff.
    Result |= 0xffu;
  }
  return Result;
}

DarwinPriority legacyPriorityToModern(int QueuePriority, int Options) {
  std::uint8_t Qos = 0;
  switch (QueuePriority) {
  case WorkqHighPriorityQueue:
    Qos = ThreadQosUserInitiated;
    break;
  case WorkqDefaultPriorityQueue:
    Qos = ThreadQosLegacy;
    break;
  case WorkqNonInteractivePriorityQueue:
  case WorkqLowPriorityQueue:
    Qos = ThreadQosUtility;
    break;
  case WorkqBackgroundPriorityQueue:
    Qos = ThreadQosBackground;
    break;
  default:
    return 0;
  }

  DarwinPriority Flags = 0;
  if ((Options & WorkqAddthreadsOptionOvercommit) != 0)
    Flags |= PriorityOvercommitFlag;
  return makePriority(Qos, Flags);
}

void modernPriorityToLegacy(DarwinPriority Priority, int &QueuePriority,
                            int &Options) {
  Options = (Priority & PriorityOvercommitFlag)
                ? WorkqAddthreadsOptionOvercommit
                : 0;
  switch (priorityThreadQos(Priority)) {
  case ThreadQosUserInteractive:
  case ThreadQosUserInitiated:
    QueuePriority = WorkqHighPriorityQueue;
    break;
  case ThreadQosLegacy:
    QueuePriority = WorkqDefaultPriorityQueue;
    break;
  case ThreadQosUtility:
    QueuePriority = WorkqLowPriorityQueue;
    break;
  case ThreadQosBackground:
    QueuePriority = WorkqBackgroundPriorityQueue;
    break;
  default:
    QueuePriority = WorkqDefaultPriorityQueue;
    break;
  }
}

int registerWorker(void *WorkerCallback, void *KeventCallback,
                   void *WorkloopCallback, std::uint64_t SerialOffset,
                   std::uint64_t LabelOffset, bool LegacyWorker) {
  if (!WorkerCallback)
    return EINVAL;

  Registry &R = registry();
  std::lock_guard<std::mutex> Guard(R.Mutex);
  if (R.WorkerCallback)
    return EBUSY;

  R.WorkerCallback = WorkerCallback;
  R.KeventCallback = KeventCallback;
  R.WorkloopCallback = WorkloopCallback;
  R.LegacyWorker = LegacyWorker;
  R.QueueSerialNumberOffset = SerialOffset;
  R.QueueLabelOffset = LabelOffset;
  return 0;
}

void drainWorkerRequests() {
  Registry &R = registry();

  for (;;) {
    WorkRequest Request;
    void *Worker = nullptr;
    bool Legacy = false;
    {
      std::lock_guard<std::mutex> Guard(R.Mutex);
      if (R.PendingWorkers.empty()) {
        R.DrainingWorkers = false;
        return;
      }
      Request = R.PendingWorkers.front();
      R.PendingWorkers.pop_front();
      Worker = R.WorkerCallback;
      Legacy = R.LegacyWorker;
    }

    // Registration cannot change after success, so Worker remains valid for
    // the process lifetime just like Darwin libpthread's dispatch callback.
    if (Legacy) {
      invokeVoid3(Worker,
                  static_cast<std::uint64_t>(Request.LegacyQueuePriority),
                  static_cast<std::uint64_t>(Request.LegacyOptions), 0);
    } else {
      invokeVoid1(Worker, Request.Priority);
    }
  }
}

int enqueueWorkers(int Count, DarwinPriority Priority, int LegacyQueuePriority,
                   int LegacyOptions) {
  if (Count <= 0)
    return EINVAL;

  Registry &R = registry();
  bool BecomeDrainer = false;
  {
    std::lock_guard<std::mutex> Guard(R.Mutex);
    if (!R.WorkerCallback)
      return EPERM;

    try {
      for (int I = 0; I != Count; ++I)
        R.PendingWorkers.push_back(
            WorkRequest{Priority, LegacyQueuePriority, LegacyOptions});
    } catch (const std::bad_alloc &) {
      return ENOMEM;
    }

    if (!R.DrainingWorkers) {
      R.DrainingWorkers = true;
      BecomeDrainer = true;
    }
  }

  // Calls into the ARM64 guest happen with no registry mutex held. A nested
  // _pthread_workqueue_addthreads therefore appends work and returns; the outer
  // drain loop executes it after the current callback unwinds.
  if (BecomeDrainer)
    drainWorkerRequests();
  return 0;
}

int setupWorkqueue(const DarwinWorkqueueConfig *Config, std::size_t ConfigSize) {
  if (!Config || ConfigSize < sizeof(std::uint32_t) * 2)
    return EINVAL;

  std::size_t MinimumSize = 0;
  switch (Config->Version) {
  case 1:
    MinimumSize = offsetof(DarwinWorkqueueConfig, QueueLabelOffset);
    break;
  case WorkqueueConfigVersion:
    MinimumSize = sizeof(DarwinWorkqueueConfig);
    break;
  default:
    return EINVAL;
  }

  if (ConfigSize < MinimumSize)
    return EINVAL;
  if ((Config->Flags & ~WorkqueueConfigSupportedFlags) != 0 ||
      Config->Version < WorkqueueConfigMinVersion)
    return ENOTSUP;

  const std::uint64_t LabelOffset =
      Config->Version >= 2 ? Config->QueueLabelOffset : 0;
  return registerWorker(Config->WorkerCallback, Config->KeventCallback,
                        Config->WorkloopCallback,
                        Config->QueueSerialNumberOffset, LabelOffset, false);
}

bool validWorkloopId(std::uint64_t Id) {
  return Id != 0 && Id != std::numeric_limits<std::uint64_t>::max();
}

bool validPolicy(std::uint32_t Policy) {
  return Policy == PolicyTimeshare || Policy == PolicyRoundRobin ||
         Policy == PolicyFifo;
}

int decodeParameters(const DarwinPthreadAttr *Attr, WorkloopState &State) {
  if (!Attr)
    return EINVAL;

  const bool HasPriority = (Attr->Flags & AttrSchedSet) != 0;
  const bool HasPolicy = (Attr->Flags & AttrPolicySet) != 0;
  const bool HasCpuLimit = (Attr->Flags & AttrCpuPercentSet) != 0;

  // XNU rejects KQ_WORKLOOP_CREATE requests that specify no scheduling
  // parameter at all. qosset is not one of the fields consumed by libpthread's
  // workloop-create wrapper and therefore does not satisfy this requirement.
  if (!HasPriority && !HasPolicy && !HasCpuLimit)
    return EINVAL;

  WorkloopState Next;

  if (HasPriority) {
    const std::int32_t Priority = Attr->SchedParam.SchedPriority;
    if (Priority < MinimumSchedPriority || Priority > MaximumSchedPriority)
      return EINVAL;
    Next.HasPriority = true;
    Next.Priority = Priority;
  }

  if (HasPolicy) {
    const std::uint32_t Policy =
        (Attr->Flags & AttrPolicyMask) >> AttrPolicyShift;
    if (!validPolicy(Policy))
      return EINVAL;
    Next.HasPolicy = true;
    Next.Policy = Policy;
  }

  if (HasCpuLimit) {
    const std::uint32_t Percent = Attr->CpuConfig & CpuPercentMask;
    const std::uint32_t Refill = Attr->CpuConfig >> CpuRefillShift;
    if (Percent < MinimumCpuPercent || Percent > MaximumCpuPercent ||
        Refill < MinimumRefillMilliseconds)
      return EINVAL;
    Next.HasCpuLimit = true;
    Next.CpuPercent = Percent;
    Next.RefillMilliseconds = Refill;
  }

  State = Next;
  return 0;
}

int createWorkloop(std::uint64_t Id, std::uint64_t Options,
                   const DarwinPthreadAttr *Attr) {
  if (!validWorkloopId(Id))
    return EINVAL;

  // Apple's current libpthread ABI accepts an options argument but does not
  // consume it when issuing KQ_WORKLOOP_CREATE.
  (void)Options;

  WorkloopState State;
  const int Validation = decodeParameters(Attr, State);
  if (Validation != 0)
    return Validation;

  Registry &R = registry();
  std::lock_guard<std::mutex> Guard(R.Mutex);
  if (R.Workloops.find(Id) != R.Workloops.end())
    return EEXIST;

  try {
    R.Workloops.emplace(Id, State);
  } catch (const std::bad_alloc &) {
    return ENOMEM;
  }
  return 0;
}

int destroyWorkloop(std::uint64_t Id) {
  if (!validWorkloopId(Id))
    return EINVAL;

  Registry &R = registry();
  std::lock_guard<std::mutex> Guard(R.Mutex);
  const auto It = R.Workloops.find(Id);
  if (It == R.Workloops.end())
    return ENOENT;

  R.Workloops.erase(It);
  return 0;
}

} // namespace

extern "C" {

__declspec(dllexport) int _pthread_workqueue_supported(void) {
  return SupportedWorkqueueFeatures;
}

__declspec(dllexport) int
pthread_workqueue_setup(DarwinWorkqueueConfig *Config, std::size_t ConfigSize) {
  return setupWorkqueue(Config, ConfigSize);
}

__declspec(dllexport) int _pthread_workqueue_init_with_workloop(
    void *QueueCallback, void *KeventCallback, void *WorkloopCallback,
    int Offset, int Flags) {
  (void)Flags;
  DarwinWorkqueueConfig Config{};
  Config.Version = WorkqueueConfigVersion;
  Config.WorkerCallback = QueueCallback;
  Config.KeventCallback = KeventCallback;
  Config.WorkloopCallback = WorkloopCallback;
  Config.QueueSerialNumberOffset = static_cast<std::uint64_t>(Offset);
  return setupWorkqueue(&Config, sizeof(Config));
}

__declspec(dllexport) int _pthread_workqueue_init_with_kevent(
    void *QueueCallback, void *KeventCallback, int Offset, int Flags) {
  return _pthread_workqueue_init_with_workloop(
      QueueCallback, KeventCallback, nullptr, Offset, Flags);
}

__declspec(dllexport) int _pthread_workqueue_init(void *QueueCallback,
                                                  int Offset, int Flags) {
  return _pthread_workqueue_init_with_kevent(QueueCallback, nullptr, Offset,
                                              Flags);
}

__declspec(dllexport) int
pthread_workqueue_setdispatch_np(void *WorkerCallback) {
  return registerWorker(WorkerCallback, nullptr, nullptr, 0, 0, true);
}

__declspec(dllexport) void pthread_workqueue_setdispatchoffset_np(int Offset) {
  Registry &R = registry();
  std::lock_guard<std::mutex> Guard(R.Mutex);
  R.DispatchOffset = Offset;
}

__declspec(dllexport) int
pthread_workqueue_addthreads_np(int QueuePriority, int Options, int NumThreads) {
  const DarwinPriority Priority =
      legacyPriorityToModern(QueuePriority, Options);
  if (Priority == 0)
    return EINVAL;

  Registry &R = registry();
  bool Legacy = false;
  {
    std::lock_guard<std::mutex> Guard(R.Mutex);
    Legacy = R.LegacyWorker;
  }
  return enqueueWorkers(NumThreads, Priority,
                        Legacy ? QueuePriority : WorkqDefaultPriorityQueue,
                        Legacy ? Options : 0);
}

__declspec(dllexport) int _pthread_workqueue_addthreads(
    int NumThreads, DarwinPriority Priority) {
  int QueuePriority = WorkqDefaultPriorityQueue;
  int Options = 0;
  modernPriorityToLegacy(Priority, QueuePriority, Options);
  return enqueueWorkers(NumThreads, Priority, QueuePriority, Options);
}

__declspec(dllexport) int _pthread_workqueue_add_cooperativethreads(
    int NumThreads, DarwinPriority Priority) {
  Priority |= PriorityCooperativeFlag;
  int QueuePriority = WorkqDefaultPriorityQueue;
  int Options = 0;
  modernPriorityToLegacy(Priority, QueuePriority, Options);
  return enqueueWorkers(NumThreads, Priority, QueuePriority, Options);
}

__declspec(dllexport) bool
_pthread_workqueue_should_narrow(DarwinPriority Priority) {
  (void)Priority;
  // ipaSim currently serializes all guest work onto one emulator context, so
  // there is no narrower concurrency tier to request from the host scheduler.
  return false;
}

__declspec(dllexport) int _pthread_workqueue_set_event_manager_priority(
    DarwinPriority Priority) {
  Registry &R = registry();
  std::lock_guard<std::mutex> Guard(R.Mutex);
  R.EventManagerPriority = Priority;
  return 0;
}

__declspec(dllexport) int __pthread_workqueue_setkill(int Enabled) {
  Registry &R = registry();
  std::lock_guard<std::mutex> Guard(R.Mutex);
  R.KillEnabled = Enabled != 0;
  return 0;
}

__declspec(dllexport) int
_pthread_workloop_create(std::uint64_t WorkloopId, std::uint64_t Options,
                         DarwinPthreadAttr *Attr) {
  // pthread SPI returns Darwin errno values directly; do not write host errno.
  return createWorkloop(WorkloopId, Options, Attr);
}

__declspec(dllexport) int _pthread_workloop_destroy(std::uint64_t WorkloopId) {
  return destroyWorkloop(WorkloopId);
}

} // extern "C"
