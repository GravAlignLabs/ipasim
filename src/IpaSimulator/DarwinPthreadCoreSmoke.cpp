// DarwinPthreadCoreSmoke.cpp: executable contract for the Darwin pthread core
// host bridge. Native callbacks exercise the same Win32 thread/lifetime path
// used by guest pthread_create without requiring a loaded guest Mach-O.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using DarwinPthread = void *;
using DarwinQosClass = std::uint32_t;
using DarwinUnsignedLong = std::uint64_t;
using DarwinSigset = std::uint32_t;

constexpr std::uint64_t PthreadAttrSig = 0x54484441ULL;
constexpr int PthreadCreateJoinable = 1;
constexpr int PthreadCreateDetached = 2;
constexpr int SchedOther = 1;
constexpr int SchedRR = 2;
constexpr DarwinQosClass QosClassDefault = 0x15;
constexpr DarwinQosClass QosClassUnspecified = 0x00;

struct DarwinSchedParam {
  std::int32_t SchedPriority;
  std::int32_t Quantum;
};

struct DarwinPthreadAttr {
  std::uint64_t Sig;
  std::uint64_t GuardSize;
  void *StackAddress;
  std::uint64_t StackSize;
  union {
    DarwinSchedParam Param;
    std::uint64_t QosClass;
  } Scheduling;
  std::uint32_t Flags;
  std::uint32_t CpuPercentAndRefill;
  std::uint32_t Reserved[4];
};
static_assert(sizeof(DarwinPthreadAttr) == 64);
static_assert(offsetof(DarwinPthreadAttr, Flags) == 40);
static_assert(offsetof(DarwinPthreadAttr, CpuPercentAndRefill) == 44);

using AttrInitFn = int (*)(DarwinPthreadAttr *);
using AttrDestroyFn = int (*)(DarwinPthreadAttr *);
using AttrGetSchedParamFn = int (*)(const DarwinPthreadAttr *, DarwinSchedParam *);
using AttrGetSchedPolicyFn = int (*)(const DarwinPthreadAttr *, int *);
using AttrSetDetachFn = int (*)(DarwinPthreadAttr *, int);
using AttrSetSchedParamFn = int (*)(DarwinPthreadAttr *, const DarwinSchedParam *);
using AttrSetSchedPolicyFn = int (*)(DarwinPthreadAttr *, int);
using AttrSetCpuFn = int (*)(DarwinPthreadAttr *, int, DarwinUnsignedLong);
using AttrGetQosFn = int (*)(const DarwinPthreadAttr *, DarwinQosClass *, int *);
using SelfFn = DarwinPthread (*)();
using MainFn = int (*)();
using ThreadIdFn = int (*)(DarwinPthread, std::uint64_t *);
using CreateFn = int (*)(DarwinPthread *, const DarwinPthreadAttr *, void *, void *);
using JoinFn = int (*)(DarwinPthread, void **);
using DetachFn = int (*)(DarwinPthread);
using ExitFn = void (*)(void *);
using GetStackFn = void *(*)(DarwinPthread);
using SetNameFn = int (*)(const char *);
using SigmaskFn = int (*)(int, const DarwinSigset *, DarwinSigset *);
using ParallelFn = int (*)(DarwinQosClass, DarwinUnsignedLong);
using RealtimeParallelFn = int (*)(DarwinUnsignedLong);
using MainQosFn = DarwinQosClass (*)();
using CpuNumberFn = int (*)(std::size_t *);

SelfFn PthreadSelf = nullptr;
ThreadIdFn PthreadThreadId = nullptr;
ExitFn PthreadExit = nullptr;

[[noreturn]] void fail(const char *Message) {
  std::fprintf(stderr, "[darwin-pthread-core-smoke] FAIL: %s\n", Message);
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
    std::fprintf(stderr, "[darwin-pthread-core-smoke] missing export: %s\n",
                 Name);
    std::exit(1);
  }
  return Result;
}

void *returnArgument(void *Argument) {
  require(PthreadSelf() != nullptr, "worker pthread_self returned null");
  std::uint64_t ThreadId = 0;
  require(PthreadThreadId(nullptr, &ThreadId) == 0 && ThreadId != 0,
          "worker thread id missing");
  return Argument;
}

void *exitExplicitly(void *Argument) {
  PthreadExit(Argument);
  return nullptr;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    fail("expected path to IpaSimDarwinHost.dll");

  HMODULE Dll = LoadLibraryA(argv[1]);
  if (!Dll)
    fail("could not load IpaSimDarwinHost.dll");

  auto AttrInit = requireExport<AttrInitFn>(Dll, "pthread_attr_init");
  auto AttrDestroy = requireExport<AttrDestroyFn>(Dll, "pthread_attr_destroy");
  auto AttrGetSchedParam = requireExport<AttrGetSchedParamFn>(
      Dll, "pthread_attr_getschedparam");
  auto AttrGetSchedPolicy = requireExport<AttrGetSchedPolicyFn>(
      Dll, "pthread_attr_getschedpolicy");
  auto AttrSetDetach = requireExport<AttrSetDetachFn>(
      Dll, "pthread_attr_setdetachstate");
  auto AttrSetSchedParam = requireExport<AttrSetSchedParamFn>(
      Dll, "pthread_attr_setschedparam");
  auto AttrSetSchedPolicy = requireExport<AttrSetSchedPolicyFn>(
      Dll, "pthread_attr_setschedpolicy");
  auto AttrSetCpu = requireExport<AttrSetCpuFn>(
      Dll, "pthread_attr_setcpupercent_np");
  auto AttrGetQos = requireExport<AttrGetQosFn>(
      Dll, "pthread_attr_get_qos_class_np");
  PthreadSelf = requireExport<SelfFn>(Dll, "pthread_self");
  auto PthreadMain = requireExport<MainFn>(Dll, "pthread_main_np");
  PthreadThreadId = requireExport<ThreadIdFn>(Dll, "pthread_threadid_np");
  auto PthreadCreate = requireExport<CreateFn>(Dll, "pthread_create");
  auto PthreadJoin = requireExport<JoinFn>(Dll, "pthread_join");
  auto PthreadDetach = requireExport<DetachFn>(Dll, "pthread_detach");
  PthreadExit = requireExport<ExitFn>(Dll, "pthread_exit");
  auto GetStack = requireExport<GetStackFn>(Dll, "pthread_get_stackaddr_np");
  auto SetName = requireExport<SetNameFn>(Dll, "pthread_setname_np");
  auto Sigmask = requireExport<SigmaskFn>(Dll, "pthread_sigmask");
  auto Parallel = requireExport<ParallelFn>(Dll, "pthread_qos_max_parallelism");
  auto RealtimeParallel = requireExport<RealtimeParallelFn>(
      Dll, "pthread_time_constraint_max_parallelism");
  auto MainQos = requireExport<MainQosFn>(Dll, "qos_class_main");
  auto CpuNumber = requireExport<CpuNumberFn>(Dll, "pthread_cpu_number_np");

  DarwinPthreadAttr Attr{};
  require(AttrInit(nullptr) == EINVAL, "null attr init did not fail");
  require(AttrInit(&Attr) == 0, "attr init failed");
  require(Attr.Sig == PthreadAttrSig, "attr signature incorrect");

  int Policy = 0;
  require(AttrGetSchedPolicy(&Attr, &Policy) == 0 && Policy == SchedOther,
          "default sched policy incorrect");
  DarwinSchedParam Param{};
  require(AttrGetSchedParam(&Attr, &Param) == 0 &&
              Param.SchedPriority == 31 && Param.Quantum == 10,
          "default sched param incorrect");

  require(AttrSetCpu(&Attr, 50, 10) == EINVAL,
          "cpu percent accepted without fixed policy");
  require(AttrSetSchedPolicy(&Attr, SchedRR) == 0,
          "failed to set RR policy");
  DarwinSchedParam Requested{42, 7};
  require(AttrSetSchedParam(&Attr, &Requested) == 0,
          "failed to set sched params");
  require(AttrSetCpu(&Attr, 50, 10) == 0,
          "valid cpu percent/refill rejected");
  require((Attr.Flags & 0x08000000u) != 0,
          "cpu-percent-set flag not recorded");
  require(AttrSetSchedPolicy(&Attr, SchedOther) == 0,
          "failed to restore timeshare policy");
  require((Attr.Flags & 0x08000000u) == 0,
          "timeshare policy did not clear cpu-percent state");

  DarwinQosClass AttrQos = QosClassDefault;
  int AttrRelative = 99;
  require(AttrGetQos(&Attr, &AttrQos, &AttrRelative) == 0 &&
              AttrQos == QosClassUnspecified && AttrRelative == 0,
          "default attr incorrectly reported explicit QoS");

  require(PthreadSelf() != nullptr, "main pthread_self returned null");
  require(PthreadMain() == 1, "initial thread not recognized as main");
  std::uint64_t MainThreadId = 0;
  require(PthreadThreadId(nullptr, &MainThreadId) == 0 && MainThreadId != 0,
          "main thread id missing");
  require(GetStack(nullptr) != nullptr, "stack identity missing");
  require(SetName("ipaSim pthread smoke") == 0, "setname failed");

  DarwinSigset AddMask = 0x5u;
  DarwinSigset OldMask = 0;
  require(Sigmask(1, &AddMask, &OldMask) == 0 && OldMask == 0,
          "signal block semantics failed");
  DarwinSigset CurrentMask = 0;
  require(Sigmask(3, nullptr, &CurrentMask) == 0 && CurrentMask == AddMask,
          "signal mask query failed");

  require(Parallel(QosClassDefault, 0) >= 1,
          "logical QoS parallelism invalid");
  require(Parallel(QosClassDefault, 1) >= 1,
          "physical QoS parallelism invalid");
  errno = 0;
  require(Parallel(QosClassDefault, 2) == -1 && errno == EINVAL,
          "invalid parallelism flags were accepted");
  require(RealtimeParallel(0) >= 1, "realtime parallelism invalid");
  require(MainQos() == QosClassDefault, "main QoS class incorrect");
  std::size_t Cpu = static_cast<std::size_t>(-1);
  require(CpuNumber(&Cpu) == 0, "cpu-number query failed");

  // Normal start-routine return must preserve the returned pointer through join.
  DarwinPthread Thread = nullptr;
  void *Expected = reinterpret_cast<void *>(0x12345678ULL);
  require(PthreadCreate(&Thread, nullptr,
                        reinterpret_cast<void *>(&returnArgument), Expected) == 0,
          "pthread_create failed");
  require(Thread != nullptr, "pthread_create returned null pthread_t");
  std::uint64_t WorkerThreadId = 0;
  require(PthreadThreadId(Thread, &WorkerThreadId) == 0 &&
              WorkerThreadId != 0 && WorkerThreadId != MainThreadId,
          "created thread identity invalid");
  void *Joined = nullptr;
  require(PthreadJoin(Thread, &Joined) == 0 && Joined == Expected,
          "pthread_join did not preserve start-routine return");

  // Explicit pthread_exit is non-returning but the joiner must still observe
  // the requested exit value.
  Thread = nullptr;
  Expected = reinterpret_cast<void *>(0x87654321ULL);
  require(PthreadCreate(&Thread, nullptr,
                        reinterpret_cast<void *>(&exitExplicitly), Expected) == 0,
          "pthread_create for explicit-exit test failed");
  Joined = nullptr;
  require(PthreadJoin(Thread, &Joined) == 0 && Joined == Expected,
          "pthread_exit value was not visible to joiner");

  // Detached threads reject join and second detach without becoming fake
  // joinable objects.
  require(AttrInit(&Attr) == 0, "second attr init failed");
  require(AttrSetDetach(&Attr, PthreadCreateDetached) == 0,
          "detached attr setup failed");
  Thread = nullptr;
  require(PthreadCreate(&Thread, &Attr,
                        reinterpret_cast<void *>(&returnArgument), nullptr) == 0,
          "detached pthread_create failed");
  const int JoinDetached = PthreadJoin(Thread, nullptr);
  require(JoinDetached == EINVAL || JoinDetached == ESRCH,
          "detached thread unexpectedly joined");
  const int DetachAgain = PthreadDetach(Thread);
  require(DetachAgain == EINVAL || DetachAgain == ESRCH,
          "second detach unexpectedly succeeded");

  require(AttrDestroy(&Attr) == 0, "attr destroy failed");
  require(AttrDestroy(&Attr) == EINVAL,
          "destroyed attr remained valid");

  FreeLibrary(Dll);
  std::printf("[darwin-pthread-core-smoke] passed\n");
  return 0;
}
