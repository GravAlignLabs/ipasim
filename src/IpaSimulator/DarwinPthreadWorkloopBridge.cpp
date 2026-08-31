// DarwinPthreadWorkloopBridge.cpp: Darwin pthread workloop control-plane
// semantics for the iOS Simulator pthread host boundary.
//
// Darwin's _pthread_workloop_create() validates scheduling metadata and asks
// the kernel to reserve a unique workloop ID. Windows has no kqueue workloop
// object, so ipaSim preserves the observable control-plane contract in a
// process-wide registry: validation, uniqueness, lifetime, and destroy errors
// are real state transitions. Event delivery through Darwin kevent/workqueue
// machinery remains a separate runtime boundary and is deliberately not
// fabricated here.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>

namespace {

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

struct Registry {
  std::mutex Mutex;
  std::unordered_map<std::uint64_t, WorkloopState> Workloops;
};

Registry &registry() {
  // Keep state alive for the process lifetime. The host bridge is unloadable,
  // and a normal C++ static destructor must not run after its code is unmapped.
  static Registry *State = new Registry();
  return *State;
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
  // consume it when issuing KQ_WORKLOOP_CREATE; preserve that observable
  // behavior rather than inventing Windows-specific option semantics.
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
