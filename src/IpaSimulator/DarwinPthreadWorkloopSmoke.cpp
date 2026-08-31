// DarwinPthreadWorkloopSmoke.cpp: export and semantic checks for Darwin pthread
// workloop create/destroy behavior in the simulator host bridge.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <thread>

namespace {

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

constexpr std::uint32_t AttrQosSet = 1u << 25;
constexpr std::uint32_t AttrSchedSet = 1u << 24;
constexpr std::uint32_t AttrPolicySet = 1u << 26;
constexpr std::uint32_t AttrCpuPercentSet = 1u << 27;
constexpr unsigned AttrPolicyShift = 16;
constexpr unsigned CpuRefillShift = 8;

constexpr std::uint32_t PolicyTimeshare = 1;
constexpr std::uint32_t PolicyRoundRobin = 2;
constexpr std::uint32_t PolicyFifo = 4;

int fail(const char *Message) {
  std::fprintf(stderr, "[darwin-pthread-workloop-smoke] FAIL: %s\n", Message);
  return 1;
}

FARPROC requireExport(HMODULE Module, const char *Name) {
  FARPROC Proc = GetProcAddress(Module, Name);
  if (!Proc)
    std::fprintf(stderr,
                 "[darwin-pthread-workloop-smoke] missing export: %s\n",
                 Name);
  return Proc;
}

DarwinPthreadAttr priorityAttr(std::int32_t Priority) {
  DarwinPthreadAttr Attr{};
  Attr.SchedParam.SchedPriority = Priority;
  Attr.Flags = AttrSchedSet;
  return Attr;
}

DarwinPthreadAttr policyAttr(std::uint32_t Policy) {
  DarwinPthreadAttr Attr{};
  Attr.Flags = AttrPolicySet | (Policy << AttrPolicyShift);
  return Attr;
}

DarwinPthreadAttr cpuAttr(std::uint32_t Percent, std::uint32_t RefillMs) {
  DarwinPthreadAttr Attr{};
  Attr.Flags = AttrCpuPercentSet;
  Attr.CpuConfig = (RefillMs << CpuRefillShift) | (Percent & 0xffu);
  return Attr;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return fail("expected IpaSimDarwinHost.dll path");

  HMODULE Host = LoadLibraryA(argv[1]);
  if (!Host)
    return fail("could not load IpaSimDarwinHost.dll");

  using Create = int (*)(std::uint64_t, std::uint64_t, DarwinPthreadAttr *);
  using Destroy = int (*)(std::uint64_t);

  auto CreateWorkloop = reinterpret_cast<Create>(
      requireExport(Host, "_pthread_workloop_create"));
  auto DestroyWorkloop = reinterpret_cast<Destroy>(
      requireExport(Host, "_pthread_workloop_destroy"));
  if (!CreateWorkloop || !DestroyWorkloop) {
    FreeLibrary(Host);
    return 1;
  }

  errno = EDOM;

  DarwinPthreadAttr ValidPriority = priorityAttr(31);
  if (CreateWorkloop(1, 0, nullptr) != EINVAL) {
    FreeLibrary(Host);
    return fail("null pthread_attr_t was not rejected");
  }
  if (CreateWorkloop(0, 0, &ValidPriority) != EINVAL ||
      CreateWorkloop(std::numeric_limits<std::uint64_t>::max(), 0,
                     &ValidPriority) != EINVAL ||
      DestroyWorkloop(0) != EINVAL ||
      DestroyWorkloop(std::numeric_limits<std::uint64_t>::max()) != EINVAL) {
    FreeLibrary(Host);
    return fail("reserved workloop IDs were not rejected");
  }

  DarwinPthreadAttr Empty{};
  DarwinPthreadAttr QosOnly{};
  QosOnly.Flags = AttrQosSet;
  if (CreateWorkloop(2, 0, &Empty) != EINVAL ||
      CreateWorkloop(2, 0, &QosOnly) != EINVAL) {
    FreeLibrary(Host);
    return fail("create accepted an attribute with no workloop scheduling parameter");
  }

  DarwinPthreadAttr PriorityZero = priorityAttr(0);
  DarwinPthreadAttr PriorityOne = priorityAttr(1);
  DarwinPthreadAttr PriorityMax = priorityAttr(63);
  DarwinPthreadAttr PriorityHigh = priorityAttr(64);
  if (CreateWorkloop(3, 0, &PriorityZero) != EINVAL ||
      CreateWorkloop(3, 0, &PriorityHigh) != EINVAL ||
      CreateWorkloop(3, 0, &PriorityOne) != 0 ||
      DestroyWorkloop(3) != 0 ||
      CreateWorkloop(3, 0, &PriorityMax) != 0 ||
      DestroyWorkloop(3) != 0) {
    FreeLibrary(Host);
    return fail("scheduling-priority range semantics failed");
  }

  const std::uint32_t Policies[] = {PolicyTimeshare, PolicyRoundRobin,
                                    PolicyFifo};
  std::uint64_t PolicyId = 10;
  for (std::uint32_t Policy : Policies) {
    DarwinPthreadAttr Attr = policyAttr(Policy);
    if (CreateWorkloop(PolicyId, 0, &Attr) != 0 ||
        DestroyWorkloop(PolicyId) != 0) {
      FreeLibrary(Host);
      return fail("valid Darwin scheduling policy was rejected");
    }
    ++PolicyId;
  }

  DarwinPthreadAttr PolicyZero = policyAttr(0);
  DarwinPthreadAttr PolicyThree = policyAttr(3);
  DarwinPthreadAttr PolicyFive = policyAttr(5);
  if (CreateWorkloop(20, 0, &PolicyZero) != EINVAL ||
      CreateWorkloop(20, 0, &PolicyThree) != EINVAL ||
      CreateWorkloop(20, 0, &PolicyFive) != EINVAL) {
    FreeLibrary(Host);
    return fail("invalid Darwin scheduling policy was accepted");
  }

  DarwinPthreadAttr CpuPercentZero = cpuAttr(0, 1);
  DarwinPthreadAttr CpuPercentHigh = cpuAttr(101, 1);
  DarwinPthreadAttr CpuRefillZero = cpuAttr(50, 0);
  DarwinPthreadAttr CpuMinimum = cpuAttr(1, 1);
  DarwinPthreadAttr CpuMaximum = cpuAttr(100, 0x00ffffffu);
  if (CreateWorkloop(30, 0, &CpuPercentZero) != EINVAL ||
      CreateWorkloop(30, 0, &CpuPercentHigh) != EINVAL ||
      CreateWorkloop(30, 0, &CpuRefillZero) != EINVAL ||
      CreateWorkloop(30, 0, &CpuMinimum) != 0 ||
      DestroyWorkloop(30) != 0 ||
      CreateWorkloop(30, 0, &CpuMaximum) != 0 ||
      DestroyWorkloop(30) != 0) {
    FreeLibrary(Host);
    return fail("CPU percentage/refill validation failed");
  }

  // Current XNU passes the options argument through the control call without
  // defining option-bit validation. Preserve that behavior while still making
  // the workloop itself a real, stateful registry object.
  constexpr std::uint64_t ArbitraryOptions = 0xfedcba9876543210ULL;
  if (CreateWorkloop(40, ArbitraryOptions, &ValidPriority) != 0 ||
      CreateWorkloop(40, 0, &ValidPriority) != EEXIST ||
      DestroyWorkloop(40) != 0 || DestroyWorkloop(40) != ENOENT) {
    FreeLibrary(Host);
    return fail("workloop ID lifecycle/duplicate semantics failed");
  }

  // Creation is process-wide and atomic: racing the same ID must produce one
  // live workloop and one EEXIST result, never two successful reservations.
  std::atomic<bool> Start{false};
  std::atomic<int> ResultA{-1};
  std::atomic<int> ResultB{-1};
  auto Racer = [&](std::atomic<int> &Result) {
    DarwinPthreadAttr Attr = priorityAttr(20);
    while (!Start.load())
      std::this_thread::yield();
    Result = CreateWorkloop(50, 0, &Attr);
  };
  std::thread A(Racer, std::ref(ResultA));
  std::thread B(Racer, std::ref(ResultB));
  Start = true;
  A.join();
  B.join();

  const int AResult = ResultA.load();
  const int BResult = ResultB.load();
  if (!((AResult == 0 && BResult == EEXIST) ||
        (AResult == EEXIST && BResult == 0)) ||
      DestroyWorkloop(50) != 0 || DestroyWorkloop(50) != ENOENT) {
    FreeLibrary(Host);
    return fail("concurrent workloop reservation was not atomic");
  }

  // Multiple scheduling fields may be supplied together. The signature and
  // unrelated pthread_attr_t fields are intentionally not validated here,
  // matching libpthread's workloop wrapper before it enters the kernel.
  DarwinPthreadAttr Combined = priorityAttr(42);
  Combined.Flags |= AttrPolicySet | (PolicyRoundRobin << AttrPolicyShift) |
                    AttrCpuPercentSet;
  Combined.CpuConfig = (250u << CpuRefillShift) | 75u;
  Combined.Sig = 0x1122334455667788LL;
  if (CreateWorkloop(60, 7, &Combined) != 0 ||
      DestroyWorkloop(60) != 0) {
    FreeLibrary(Host);
    return fail("combined scheduling metadata was rejected");
  }

  if (errno != EDOM) {
    FreeLibrary(Host);
    return fail("pthread workloop bridge modified host errno");
  }

  std::printf("[darwin-pthread-workloop-smoke] create/destroy lifecycle and scheduling validation passed\n");
  FreeLibrary(Host);
  return 0;
}
