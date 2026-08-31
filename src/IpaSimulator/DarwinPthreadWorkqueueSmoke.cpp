// DarwinPthreadWorkqueueSmoke.cpp: executable specification for the Windows
// Darwin pthread workqueue bridge used by libdispatch.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using DarwinPriority = std::uint64_t;
using MachPort = std::uint32_t;

constexpr int DarwinErrnoPermission = 1;
constexpr int DarwinErrnoNoSuchProcess = 3;
constexpr int DarwinErrnoBusy = 16;
constexpr int DarwinErrnoFault = 14;
constexpr int DarwinErrnoInvalid = 22;
constexpr int DarwinErrnoNotSupported = 45;
constexpr int DarwinErrnoStale = 70;

constexpr int WorkqFeatureDispatchFunc = 0x01;
constexpr int WorkqFeatureFinePriority = 0x02;
constexpr int WorkqFeatureMaintenance = 0x10;
constexpr int WorkqFeatureKevent = 0x40;
constexpr int WorkqFeatureWorkloop = 0x80;
constexpr int ExpectedFeatures = WorkqFeatureDispatchFunc |
                                 WorkqFeatureFinePriority |
                                 WorkqFeatureMaintenance;

struct DarwinWorkqueueConfig {
  std::uint32_t Flags;
  std::uint32_t Version;
  void *KeventCallback;
  void *WorkloopCallback;
  void *WorkqueueCallback;
  std::uint64_t QueueSerialNumberOffset;
  std::uint64_t QueueLabelOffset;
};
static_assert(sizeof(DarwinWorkqueueConfig) == 48);
static_assert(offsetof(DarwinWorkqueueConfig, QueueLabelOffset) == 40);

using SetupFn = int (*)(const DarwinWorkqueueConfig *, std::size_t);
using SupportedFn = int (*)();
using AddThreadsFn = int (*)(std::int32_t, DarwinPriority);
using ShouldNarrowFn = bool (*)(DarwinPriority);
using SetManagerPriorityFn = int (*)(DarwinPriority);
using OverrideStartFn = int (*)(MachPort, DarwinPriority);
using OverrideStartOwnerFn = int (*)(MachPort, DarwinPriority, MachPort *);
using OverrideResetFn = int (*)();

AddThreadsFn AddThreads = nullptr;
std::vector<DarwinPriority> SeenPriorities;
int CallbackDepth = 0;
int MaximumCallbackDepth = 0;
bool InjectRecursiveRequest = false;

[[noreturn]] void fail(const char *Message) {
  std::fprintf(stderr, "[darwin-pthread-workqueue-smoke] FAIL: %s\n", Message);
  std::exit(1);
}

void require(bool Condition, const char *Message) {
  if (!Condition)
    fail(Message);
}

template <typename Function>
Function requireExport(HMODULE Dll, const char *Name) {
  auto Result = reinterpret_cast<Function>(GetProcAddress(Dll, Name));
  if (!Result) {
    std::fprintf(stderr,
                 "[darwin-pthread-workqueue-smoke] missing export: %s\n",
                 Name);
    std::exit(1);
  }
  return Result;
}

void workerCallback(DarwinPriority Priority) {
  ++CallbackDepth;
  if (CallbackDepth > MaximumCallbackDepth)
    MaximumCallbackDepth = CallbackDepth;
  SeenPriorities.push_back(Priority);

  if (InjectRecursiveRequest) {
    InjectRecursiveRequest = false;
    require(AddThreads(2, 0x000004ffULL) == 0,
            "recursive worker request failed");
  }

  --CallbackDepth;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    fail("expected path to IpaSimDarwinHost.dll");

  HMODULE Dll = LoadLibraryA(argv[1]);
  if (!Dll)
    fail("could not load IpaSimDarwinHost.dll");

  auto Setup = requireExport<SetupFn>(Dll, "pthread_workqueue_setup");
  auto Supported =
      requireExport<SupportedFn>(Dll, "_pthread_workqueue_supported");
  AddThreads =
      requireExport<AddThreadsFn>(Dll, "_pthread_workqueue_addthreads");
  auto ShouldNarrow = requireExport<ShouldNarrowFn>(
      Dll, "_pthread_workqueue_should_narrow");
  auto SetManagerPriority = requireExport<SetManagerPriorityFn>(
      Dll, "_pthread_workqueue_set_event_manager_priority");
  auto OverrideStart = requireExport<OverrideStartFn>(
      Dll, "_pthread_workqueue_override_start_direct");
  auto OverrideStartOwner = requireExport<OverrideStartOwnerFn>(
      Dll, "_pthread_workqueue_override_start_direct_check_owner");
  auto OverrideReset = requireExport<OverrideResetFn>(
      Dll, "_pthread_workqueue_override_reset");

  // The bridge must expose only delivery modes it can actually execute. In
  // particular, advertising KEVENT/WORKLOOP would make libdispatch select a
  // callback path this cooperative executor does not implement.
  const int Features = Supported();
  require((Features & ExpectedFeatures) == ExpectedFeatures,
          "required dispatch/fine-priority/maintenance features missing");
  require((Features & (WorkqFeatureKevent | WorkqFeatureWorkloop)) == 0,
          "unsupported kevent/workloop feature advertised");

  // Apple returns EPERM when workers are requested before dispatch has
  // registered its callback.
  require(AddThreads(1, 0x000004ffULL) == DarwinErrnoPermission,
          "unconfigured addthreads did not return EPERM");

  DarwinWorkqueueConfig Config{};
  Config.Version = 2;
  Config.WorkqueueCallback = reinterpret_cast<void *>(&workerCallback);
  Config.QueueSerialNumberOffset = 0x28;
  Config.QueueLabelOffset = 0x30;

  require(Setup(nullptr, sizeof(Config)) == DarwinErrnoInvalid,
          "null setup config did not return EINVAL");
  require(Setup(&Config, 0) == DarwinErrnoInvalid,
          "short setup config did not return EINVAL");

  DarwinWorkqueueConfig BadVersion = Config;
  BadVersion.Version = 3;
  require(Setup(&BadVersion, sizeof(BadVersion)) == DarwinErrnoInvalid,
          "unknown setup version did not return EINVAL");

  DarwinWorkqueueConfig BadFlags = Config;
  BadFlags.Flags = 1;
  require(Setup(&BadFlags, sizeof(BadFlags)) == DarwinErrnoNotSupported,
          "unsupported setup flags did not return ENOTSUP");

  DarwinWorkqueueConfig Version1 = Config;
  Version1.Version = 1;
  require(Setup(&Version1,
                offsetof(DarwinWorkqueueConfig, QueueLabelOffset) - 1) ==
              DarwinErrnoInvalid,
          "short v1 setup did not return EINVAL");

  errno = 1234;
  require(Setup(&Config, sizeof(Config)) == 0, "valid setup failed");
  require(errno == 1234, "valid setup changed host errno");
  require(Setup(&Config, sizeof(Config)) == DarwinErrnoBusy,
          "second setup did not return EBUSY");

  require(AddThreads(0, 0x000004ffULL) == DarwinErrnoInvalid,
          "zero worker request did not return EINVAL");
  require(AddThreads(1, 0x100000000ULL) == DarwinErrnoInvalid,
          "non-compact pthread priority was truncated");

  // The worker callback must really run, and recursive worker requests must be
  // queued behind the current callback rather than recursively entering the
  // guest execution engine.
  SeenPriorities.clear();
  MaximumCallbackDepth = 0;
  InjectRecursiveRequest = true;
  errno = 2345;
  require(AddThreads(1, 0x000020ffULL) == 0,
          "worker request failed");
  require(errno == 2345, "successful addthreads changed host errno");
  require(SeenPriorities.size() == 3,
          "cooperative executor did not drain recursive worker requests");
  require(SeenPriorities[0] == 0x000020ffULL &&
              SeenPriorities[1] == 0x000004ffULL &&
              SeenPriorities[2] == 0x000004ffULL,
          "worker priorities were not propagated intact");
  require(MaximumCallbackDepth == 1,
          "recursive addthreads nested worker callback execution");

  require(!ShouldNarrow(0x000004ffULL),
          "single-lane cooperative scheduler unexpectedly narrowed");

  errno = 3456;
  require(SetManagerPriority(0x00000200ULL) == 0,
          "event manager priority update failed");
  require(errno == 3456, "manager priority update changed host errno");
  require(SetManagerPriority(0x100000000ULL) == DarwinErrnoInvalid,
          "manager priority truncated high bits");

  constexpr MachPort Thread = 0x1237u;
  constexpr DarwinPriority Priority = 0x000004ffULL;
  require(OverrideStart(0, Priority) == DarwinErrnoNoSuchProcess,
          "null override thread did not return ESRCH");
  require(OverrideStart(Thread, 0) == DarwinErrnoInvalid,
          "zero override priority did not return EINVAL");

  errno = 4567;
  require(OverrideStart(Thread, Priority) == 0,
          "workqueue override start failed");
  require(errno == 4567, "override start changed host errno");

  alignas(4) MachPort MatchingOwner = Thread & ~0x3u;
  require(OverrideStartOwner(Thread, Priority, &MatchingOwner) == 0,
          "owner-checked override rejected matching ulock owner");

  alignas(4) MachPort StaleOwner = 0x9994u;
  require(OverrideStartOwner(Thread, Priority, &StaleOwner) == DarwinErrnoStale,
          "owner-checked override did not detect stale owner");

  auto *Misaligned = reinterpret_cast<MachPort *>(
      reinterpret_cast<std::uintptr_t>(&MatchingOwner) + 1);
  require(OverrideStartOwner(Thread, Priority, Misaligned) == DarwinErrnoInvalid,
          "misaligned ulock owner did not return EINVAL");

  // An unreadable aligned address must fail closed rather than being
  // dereferenced by the host process.
  require(OverrideStartOwner(Thread, Priority,
                             reinterpret_cast<MachPort *>(0x1000)) ==
              DarwinErrnoFault,
          "unreadable ulock owner did not return EFAULT");

  errno = 5678;
  require(OverrideReset() == 0, "workqueue override reset failed");
  require(errno == 5678, "override reset changed host errno");

  FreeLibrary(Dll);
  std::printf("[darwin-pthread-workqueue-smoke] passed\n");
  return 0;
}
