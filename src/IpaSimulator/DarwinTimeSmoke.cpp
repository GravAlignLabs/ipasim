// DarwinTimeSmoke.cpp: semantic validation for the Darwin __gettimeofday,
// mach_continuous_time, mach_timebase_info, _os_alloc_once, and
// _os_alloc_once_table host bridges. The smoke calls the built DLL exports
// directly so symbol-only shims cannot satisfy CI without real timing, once
// allocation, and writable global table behavior.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <windows.h>

namespace {

struct DarwinTimeval {
  std::int64_t Seconds;
  std::int32_t Microseconds;
};
static_assert(sizeof(DarwinTimeval) == 16,
              "Darwin arm64 timeval must retain 64-bit alignment");

struct DarwinTimezone {
  std::int32_t MinutesWest;
  std::int32_t DstTime;
};

struct DarwinMachTimebaseInfo {
  std::uint32_t Numer;
  std::uint32_t Denom;
};
static_assert(sizeof(DarwinMachTimebaseInfo) == 8,
              "Darwin mach_timebase_info_data_t layout changed unexpectedly");

struct DarwinAllocOnceSlot {
  std::int64_t Once;
  void *Ptr;
};
static_assert(sizeof(DarwinAllocOnceSlot) == 16,
              "Darwin arm64 _os_alloc_once_s layout changed unexpectedly");
static_assert(offsetof(DarwinAllocOnceSlot, Ptr) == 8,
              "Darwin arm64 _os_alloc_once_s pointer offset must remain 8");

using DarwinAllocOnceInitializer = void (*)(void *);
using DarwinAllocOnce = void *(*)(DarwinAllocOnceSlot *, std::size_t,
                                  DarwinAllocOnceInitializer);

constexpr std::uint64_t WindowsToUnixEpoch100ns = 116444736000000000ULL;
constexpr std::uint64_t PrimarySentinel = 0x1122334455667788ULL;
constexpr std::uint64_t ConcurrentSentinel = 0x8877665544332211ULL;
constexpr std::uint64_t TableSentinel = 0xA55A5AA55AA55AA5ULL;
constexpr std::size_t DarwinAllocOnceKeyMax = 100;
constexpr int DarwinKernSuccess = 0;
constexpr int DarwinKernInvalidArgument = 4;
constexpr std::uint32_t ExpectedContinuousNumer = 100;
constexpr std::uint32_t ExpectedContinuousDenom = 1;

std::atomic<int> PrimaryInitCalls{0};
std::atomic<int> SecondaryInitCalls{0};
std::atomic<int> ConcurrentInitCalls{0};
std::atomic<int> TableInitCalls{0};
std::atomic<int> TableSecondInitCalls{0};
std::atomic<bool> PrimarySawZeroFill{false};

std::uint64_t currentUnixMicroseconds() {
  FILETIME FileTime{};
  GetSystemTimePreciseAsFileTime(&FileTime);
  ULARGE_INTEGER Ticks{};
  Ticks.LowPart = FileTime.dwLowDateTime;
  Ticks.HighPart = FileTime.dwHighDateTime;
  if (Ticks.QuadPart < WindowsToUnixEpoch100ns)
    return 0;
  return (Ticks.QuadPart - WindowsToUnixEpoch100ns) / 10ULL;
}

void primaryInitializer(void *Storage) {
  bool ZeroFilled = true;
  const auto *Bytes = static_cast<const unsigned char *>(Storage);
  for (std::size_t I = 0; I != 37; ++I) {
    if (Bytes[I] != 0) {
      ZeroFilled = false;
      break;
    }
  }
  PrimarySawZeroFill.store(ZeroFilled, std::memory_order_relaxed);
  *static_cast<std::uint64_t *>(Storage) = PrimarySentinel;
  PrimaryInitCalls.fetch_add(1, std::memory_order_relaxed);
}

void secondaryInitializer(void *) {
  SecondaryInitCalls.fetch_add(1, std::memory_order_relaxed);
}

void concurrentInitializer(void *Storage) {
  ConcurrentInitCalls.fetch_add(1, std::memory_order_relaxed);
  Sleep(25);
  *static_cast<std::uint64_t *>(Storage) = ConcurrentSentinel;
}

void tableInitializer(void *Storage) {
  TableInitCalls.fetch_add(1, std::memory_order_relaxed);
  *static_cast<std::uint64_t *>(Storage) = TableSentinel;
}

void tableSecondInitializer(void *) {
  TableSecondInitCalls.fetch_add(1, std::memory_order_relaxed);
}

bool bytesAreZero(const void *Storage, std::size_t Count) {
  const auto *Bytes = static_cast<const unsigned char *>(Storage);
  for (std::size_t I = 0; I != Count; ++I)
    if (Bytes[I] != 0)
      return false;
  return true;
}

} // namespace

int main(int ArgC, char **ArgV) {
  if (ArgC != 2) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] expected IpaSimDarwinHost.dll path\n");
    return 1;
  }

  HMODULE DarwinHost = LoadLibraryA(ArgV[1]);
  if (!DarwinHost) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] could not load Darwin host bridge (error %lu)\n",
                 static_cast<unsigned long>(GetLastError()));
    return 2;
  }

  using GetTimeOfDay = int (*)(DarwinTimeval *, DarwinTimezone *);
  auto DarwinGetTimeOfDay = reinterpret_cast<GetTimeOfDay>(
      GetProcAddress(DarwinHost, "__gettimeofday"));
  if (!DarwinGetTimeOfDay) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] __gettimeofday export was missing\n");
    FreeLibrary(DarwinHost);
    return 3;
  }

  if (DarwinGetTimeOfDay(nullptr, nullptr) != 0) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] __gettimeofday rejected null optional outputs\n");
    FreeLibrary(DarwinHost);
    return 4;
  }

  const std::uint64_t Before = currentUnixMicroseconds();
  DarwinTimeval Time{};
  if (DarwinGetTimeOfDay(&Time, nullptr) != 0) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] __gettimeofday wall-clock call failed\n");
    FreeLibrary(DarwinHost);
    return 5;
  }
  const std::uint64_t After = currentUnixMicroseconds();

  if (Time.Seconds < 0 || Time.Microseconds < 0 ||
      Time.Microseconds >= 1000000) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] invalid timeval %lld.%06d\n",
                 static_cast<long long>(Time.Seconds), Time.Microseconds);
    FreeLibrary(DarwinHost);
    return 6;
  }

  const std::uint64_t Observed =
      static_cast<std::uint64_t>(Time.Seconds) * 1000000ULL +
      static_cast<std::uint32_t>(Time.Microseconds);
  constexpr std::uint64_t ClockTolerance = 2000000ULL;
  if (Observed + ClockTolerance < Before ||
      Observed > After + ClockTolerance) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] wall clock does not track Windows system time\n");
    FreeLibrary(DarwinHost);
    return 7;
  }

  DarwinTimezone Timezone{};
  if (DarwinGetTimeOfDay(nullptr, &Timezone) != 0) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] __gettimeofday timezone call failed\n");
    FreeLibrary(DarwinHost);
    return 8;
  }

  TIME_ZONE_INFORMATION Information{};
  const DWORD State = GetTimeZoneInformation(&Information);
  if (State == TIME_ZONE_ID_INVALID) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] Windows timezone query failed\n");
    FreeLibrary(DarwinHost);
    return 9;
  }

  LONG ExpectedBias = Information.Bias;
  if (State == TIME_ZONE_ID_STANDARD)
    ExpectedBias += Information.StandardBias;
  else if (State == TIME_ZONE_ID_DAYLIGHT)
    ExpectedBias += Information.DaylightBias;
  const std::int32_t ExpectedDst =
      State == TIME_ZONE_ID_DAYLIGHT ? 1 : 0;

  if (Timezone.MinutesWest != ExpectedBias ||
      Timezone.DstTime != ExpectedDst) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] timezone mismatch: got west=%d dst=%d, expected west=%ld dst=%d\n",
                 Timezone.MinutesWest, Timezone.DstTime,
                 static_cast<long>(ExpectedBias), ExpectedDst);
    FreeLibrary(DarwinHost);
    return 10;
  }

  using MachContinuousTime = std::uint64_t (*)();
  using MachTimebaseInfo = int (*)(DarwinMachTimebaseInfo *);
  auto ContinuousTime = reinterpret_cast<MachContinuousTime>(
      GetProcAddress(DarwinHost, "mach_continuous_time"));
  auto TimebaseInfo = reinterpret_cast<MachTimebaseInfo>(
      GetProcAddress(DarwinHost, "mach_timebase_info"));
  if (!ContinuousTime || !TimebaseInfo) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] continuous-time exports were missing\n");
    FreeLibrary(DarwinHost);
    return 24;
  }

  if (TimebaseInfo(nullptr) != DarwinKernInvalidArgument) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] mach_timebase_info null argument did not return KERN_INVALID_ARGUMENT\n");
    FreeLibrary(DarwinHost);
    return 25;
  }

  DarwinMachTimebaseInfo Timebase{};
  if (TimebaseInfo(&Timebase) != DarwinKernSuccess ||
      Timebase.Numer != ExpectedContinuousNumer ||
      Timebase.Denom != ExpectedContinuousDenom) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] mach_timebase_info mismatch: got %u/%u, expected %u/%u\n",
                 Timebase.Numer, Timebase.Denom, ExpectedContinuousNumer,
                 ExpectedContinuousDenom);
    FreeLibrary(DarwinHost);
    return 26;
  }

  using QueryInterruptTimePreciseFn = void(WINAPI *)(PULONGLONG);
  HMODULE Kernel = GetModuleHandleW(L"kernel32.dll");
  auto QueryPrecise = Kernel ? reinterpret_cast<QueryInterruptTimePreciseFn>(
                                   GetProcAddress(Kernel,
                                                  "QueryInterruptTimePrecise"))
                             : nullptr;
  if (!QueryPrecise) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] Windows QueryInterruptTimePrecise was unavailable\n");
    FreeLibrary(DarwinHost);
    return 27;
  }

  ULONGLONG ContinuousBefore = 0;
  ULONGLONG ContinuousAfter = 0;
  QueryPrecise(&ContinuousBefore);
  const std::uint64_t DarwinContinuous = ContinuousTime();
  QueryPrecise(&ContinuousAfter);
  if (DarwinContinuous < ContinuousBefore ||
      DarwinContinuous > ContinuousAfter) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] mach_continuous_time does not track Windows biased interrupt time\n");
    FreeLibrary(DarwinHost);
    return 28;
  }

  Sleep(20);
  const std::uint64_t DarwinContinuousLater = ContinuousTime();
  if (DarwinContinuousLater <= DarwinContinuous) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] mach_continuous_time was not monotonic\n");
    FreeLibrary(DarwinHost);
    return 29;
  }

  const std::uint64_t ContinuousElapsedNs =
      (DarwinContinuousLater - DarwinContinuous) * Timebase.Numer /
      Timebase.Denom;
  if (ContinuousElapsedNs < 1000000ULL) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] continuous-time/timebase conversion did not advance plausibly\n");
    FreeLibrary(DarwinHost);
    return 30;
  }

  auto AllocOnce = reinterpret_cast<DarwinAllocOnce>(
      GetProcAddress(DarwinHost, "_os_alloc_once"));
  if (!AllocOnce) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] _os_alloc_once export was missing\n");
    FreeLibrary(DarwinHost);
    return 11;
  }

  DarwinAllocOnceSlot PrimarySlot{};
  void *Primary = AllocOnce(&PrimarySlot, 37, primaryInitializer);
  if (!Primary || PrimarySlot.Ptr != Primary || PrimarySlot.Once != -1) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] _os_alloc_once did not publish a completed slot\n");
    FreeLibrary(DarwinHost);
    return 12;
  }
  if ((reinterpret_cast<std::uintptr_t>(Primary) & 0xfu) != 0) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] _os_alloc_once result was not 16-byte aligned\n");
    FreeLibrary(DarwinHost);
    return 13;
  }
  if (!PrimarySawZeroFill.load(std::memory_order_relaxed) ||
      PrimaryInitCalls.load(std::memory_order_relaxed) != 1 ||
      *static_cast<std::uint64_t *>(Primary) != PrimarySentinel) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] _os_alloc_once initializer/zero-fill contract failed\n");
    FreeLibrary(DarwinHost);
    return 14;
  }

  void *PrimaryAgain = AllocOnce(&PrimarySlot, 4096, secondaryInitializer);
  if (PrimaryAgain != Primary ||
      PrimaryInitCalls.load(std::memory_order_relaxed) != 1 ||
      SecondaryInitCalls.load(std::memory_order_relaxed) != 0 ||
      *static_cast<std::uint64_t *>(PrimaryAgain) != PrimarySentinel) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] _os_alloc_once repeated a completed initialization\n");
    FreeLibrary(DarwinHost);
    return 15;
  }

  DarwinAllocOnceSlot ZeroSlot{};
  void *ZeroStorage = AllocOnce(&ZeroSlot, 64, nullptr);
  if (!ZeroStorage || ZeroSlot.Ptr != ZeroStorage || ZeroSlot.Once != -1 ||
      !bytesAreZero(ZeroStorage, 64)) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] _os_alloc_once null-initializer zero-fill contract failed\n");
    FreeLibrary(DarwinHost);
    return 16;
  }

  DarwinAllocOnceSlot ConcurrentSlot{};
  constexpr std::size_t ThreadCount = 8;
  std::vector<void *> Results(ThreadCount, nullptr);
  std::vector<std::thread> Threads;
  Threads.reserve(ThreadCount);
  for (std::size_t I = 0; I != ThreadCount; ++I) {
    Threads.emplace_back([&, I]() {
      Results[I] = AllocOnce(&ConcurrentSlot, 80, concurrentInitializer);
    });
  }
  for (std::thread &Thread : Threads)
    Thread.join();

  if (!ConcurrentSlot.Ptr || ConcurrentSlot.Once != -1 ||
      ConcurrentInitCalls.load(std::memory_order_relaxed) != 1 ||
      *static_cast<std::uint64_t *>(ConcurrentSlot.Ptr) != ConcurrentSentinel) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] _os_alloc_once concurrent once contract failed\n");
    FreeLibrary(DarwinHost);
    return 17;
  }
  for (void *Result : Results) {
    if (Result != ConcurrentSlot.Ptr) {
      std::fprintf(stderr,
                   "[darwin-time-smoke] _os_alloc_once concurrent callers received different storage\n");
      FreeLibrary(DarwinHost);
      return 18;
    }
  }

  FARPROC TableExport = GetProcAddress(DarwinHost, "_os_alloc_once_table");
  if (!TableExport) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] _os_alloc_once_table data export was missing\n");
    FreeLibrary(DarwinHost);
    return 19;
  }
  auto *AllocOnceTable = reinterpret_cast<DarwinAllocOnceSlot *>(TableExport);
  if ((reinterpret_cast<std::uintptr_t>(AllocOnceTable) & 0xfu) != 0) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] _os_alloc_once_table was not 16-byte aligned\n");
    FreeLibrary(DarwinHost);
    return 20;
  }
  for (std::size_t I = 0; I != DarwinAllocOnceKeyMax; ++I) {
    if (AllocOnceTable[I].Once != 0 || AllocOnceTable[I].Ptr != nullptr) {
      std::fprintf(stderr,
                   "[darwin-time-smoke] _os_alloc_once_table slot %zu was not zero-initialized\n",
                   I);
      FreeLibrary(DarwinHost);
      return 21;
    }
  }

  DarwinAllocOnceSlot &LastTableSlot =
      AllocOnceTable[DarwinAllocOnceKeyMax - 1];
  void *TableStorage = AllocOnce(&LastTableSlot, 48, tableInitializer);
  if (!TableStorage || LastTableSlot.Ptr != TableStorage ||
      LastTableSlot.Once != -1 ||
      TableInitCalls.load(std::memory_order_relaxed) != 1 ||
      *static_cast<std::uint64_t *>(TableStorage) != TableSentinel) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] _os_alloc_once_table last slot did not initialize through _os_alloc_once\n");
    FreeLibrary(DarwinHost);
    return 22;
  }

  void *TableStorageAgain =
      AllocOnce(&LastTableSlot, 96, tableSecondInitializer);
  if (TableStorageAgain != TableStorage ||
      TableInitCalls.load(std::memory_order_relaxed) != 1 ||
      TableSecondInitCalls.load(std::memory_order_relaxed) != 0 ||
      *static_cast<std::uint64_t *>(TableStorageAgain) != TableSentinel) {
    std::fprintf(stderr,
                 "[darwin-time-smoke] _os_alloc_once_table slot repeated initialization\n");
    FreeLibrary(DarwinHost);
    return 23;
  }

  std::printf("Darwin time + continuous time + alloc-once smoke passed: wall=%lld.%06d, continuous=%llu ticks, timebase=%u/%u, west=%d dst=%d.\n",
              static_cast<long long>(Time.Seconds), Time.Microseconds,
              static_cast<unsigned long long>(DarwinContinuousLater),
              Timebase.Numer, Timebase.Denom, Timezone.MinutesWest,
              Timezone.DstTime);
  FreeLibrary(DarwinHost);
  return 0;
}
