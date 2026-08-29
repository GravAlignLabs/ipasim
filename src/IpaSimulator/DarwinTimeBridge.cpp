// DarwinTimeBridge.cpp: Darwin wall-clock, continuous monotonic time, and
// process-lifetime alloc-once ABIs backed by explicit Windows semantics.
//
// libsystem_kernel exports __gettimeofday as the traditional two-argument
// entry point. On current Darwin the assembly wrapper deliberately clears the
// newer third mach-absolute-time syscall argument, so the externally visible
// ABI remains (timeval *, timezone *). Keep that ABI exact here instead of
// forwarding to a Windows CRT structure whose field widths differ from Darwin.
//
// mach_continuous_time advances across system sleep. Windows biased interrupt
// time has the same observable property and QueryInterruptTimePrecise exposes
// it in stable 100-nanosecond units. mach_timebase_info therefore reports the
// exact 100 ns -> 1 ns conversion as numer=100, denom=1 so guest conversion is
// coherent with the ticks returned by mach_continuous_time.
//
// libsystem_platform exports _os_alloc_once. Apple implements it as a
// process-lifetime allocator guarded by an os_once_t: the allocation is
// zero-filled, stored in the caller's slot before the optional initializer is
// invoked, and the completed once word becomes OS_ONCE_DONE. ipaSim mirrors
// that contract with 16-byte-aligned process-lifetime storage and an explicit
// per-slot synchronization state. Native host callbacks are called directly;
// ARM64 guest callbacks are routed through IpaSimLibrary's existing backcaller
// instead of ever executing guest instructions as x64 code.
//
// Darwin also exports _os_alloc_once_table as OS_ALLOC_ONCE_KEY_MAX (100)
// process-global slots. That is real writable data, not a callable shim; the
// simulator's chained fixups bind directly to the exported PE storage below.

#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <unordered_map>
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
static_assert(sizeof(DarwinTimezone) == 8,
              "Darwin timezone ABI layout changed unexpectedly");

struct DarwinMachTimebaseInfo {
  std::uint32_t Numer;
  std::uint32_t Denom;
};
static_assert(sizeof(DarwinMachTimebaseInfo) == 8,
              "Darwin mach_timebase_info_data_t layout changed unexpectedly");

constexpr std::uint64_t WindowsToUnixEpoch100ns = 116444736000000000ULL;
constexpr std::uint64_t Ticks100nsPerSecond = 10000000ULL;
constexpr std::uint64_t Ticks100nsPerMicrosecond = 10ULL;
constexpr std::uint32_t ContinuousTimeNumer = 100;
constexpr std::uint32_t ContinuousTimeDenom = 1;
constexpr int DarwinKernSuccess = 0;
constexpr int DarwinKernInvalidArgument = 4;

[[noreturn]] void failFastContinuousTime(const char *Reason) {
  std::fprintf(stderr, "[darwin-host-continuous-time] %s\n", Reason);
  std::fflush(stderr);
  RaiseFailFastException(nullptr, nullptr, 0);
  TerminateProcess(GetCurrentProcess(), 0xC0000409u);
  std::abort();
}

std::uint64_t currentContinuousTime100ns() {
  using QueryInterruptTimePreciseFn = void(WINAPI *)(PULONGLONG);
  static QueryInterruptTimePreciseFn QueryPrecise = []() {
    HMODULE Kernel = GetModuleHandleW(L"kernel32.dll");
    if (!Kernel)
      return static_cast<QueryInterruptTimePreciseFn>(nullptr);
    return reinterpret_cast<QueryInterruptTimePreciseFn>(
        GetProcAddress(Kernel, "QueryInterruptTimePrecise"));
  }();

  if (!QueryPrecise)
    failFastContinuousTime(
        "QueryInterruptTimePrecise is unavailable on this Windows host");

  ULONGLONG Value = 0;
  QueryPrecise(&Value);
  return static_cast<std::uint64_t>(Value);
}

bool currentUnixTime(DarwinTimeval &Value) {
  FILETIME FileTime{};
  GetSystemTimePreciseAsFileTime(&FileTime);

  ULARGE_INTEGER Ticks{};
  Ticks.LowPart = FileTime.dwLowDateTime;
  Ticks.HighPart = FileTime.dwHighDateTime;
  if (Ticks.QuadPart < WindowsToUnixEpoch100ns) {
    errno = EIO;
    return false;
  }

  const std::uint64_t UnixTicks = Ticks.QuadPart - WindowsToUnixEpoch100ns;
  Value.Seconds =
      static_cast<std::int64_t>(UnixTicks / Ticks100nsPerSecond);
  Value.Microseconds = static_cast<std::int32_t>(
      (UnixTicks % Ticks100nsPerSecond) / Ticks100nsPerMicrosecond);
  return true;
}

bool currentTimezone(DarwinTimezone &Value) {
  TIME_ZONE_INFORMATION Information{};
  const DWORD State = GetTimeZoneInformation(&Information);
  if (State == TIME_ZONE_ID_INVALID) {
    errno = EIO;
    return false;
  }

  LONG EffectiveBias = Information.Bias;
  if (State == TIME_ZONE_ID_STANDARD)
    EffectiveBias += Information.StandardBias;
  else if (State == TIME_ZONE_ID_DAYLIGHT)
    EffectiveBias += Information.DaylightBias;

  // Win32 Bias is UTC - local time, expressed in minutes, which is exactly
  // Darwin's historical tz_minuteswest convention. Darwin's tz_dsttime is an
  // integer DST indicator for this boundary; preserve whether daylight rules
  // are active rather than inventing a regional DST rule code.
  Value.MinutesWest = static_cast<std::int32_t>(EffectiveBias);
  Value.DstTime = State == TIME_ZONE_ID_DAYLIGHT ? 1 : 0;
  return true;
}

// Darwin arm64 uses a 64-bit long followed by a pointer for _os_alloc_once_s.
// Keep this definition independent of Windows' 32-bit long.
struct DarwinAllocOnceSlot {
  std::int64_t Once;
  void *Ptr;
};
static_assert(sizeof(DarwinAllocOnceSlot) == 16,
              "Darwin arm64 _os_alloc_once_s layout changed unexpectedly");
static_assert(offsetof(DarwinAllocOnceSlot, Ptr) == 8,
              "Darwin arm64 _os_alloc_once_s pointer offset must remain 8");

using DarwinAllocOnceInitializer = void (*)(void *);
using IpaSimGuestCallback1 = void (*)(void *, void *);

constexpr std::int64_t DarwinOsOnceInit = 0;
constexpr std::int64_t DarwinOsOnceDone = -1;
constexpr std::size_t DarwinAllocOnceAlignment = 16;
constexpr std::size_t DarwinAllocOnceKeyMax = 100;

enum class AllocOncePhase { Uninitialized, Initializing, Initialized };

struct AllocOnceState {
  std::mutex Mutex;
  std::condition_variable Condition;
  AllocOncePhase Phase = AllocOncePhase::Uninitialized;
  DWORD OwnerThreadId = 0;
};

std::mutex AllocOnceRegistryMutex;
std::unordered_map<DarwinAllocOnceSlot *, std::shared_ptr<AllocOnceState>>
    AllocOnceRegistry;

std::shared_ptr<AllocOnceState> allocOnceState(DarwinAllocOnceSlot *Slot) {
  std::lock_guard<std::mutex> Guard(AllocOnceRegistryMutex);
  auto &Entry = AllocOnceRegistry[Slot];
  if (!Entry)
    Entry = std::make_shared<AllocOnceState>();
  return Entry;
}

[[noreturn]] void failFastAllocOnce(const char *Reason,
                                    const DarwinAllocOnceSlot *Slot,
                                    std::size_t Size,
                                    const void *Initializer) {
  std::fprintf(stderr,
               "[darwin-host-os-alloc-once] %s; slot=%p size=%zu init=%p\n",
               Reason, static_cast<const void *>(Slot), Size, Initializer);
  std::fflush(stderr);
  RaiseFailFastException(nullptr, nullptr, 0);
  TerminateProcess(GetCurrentProcess(), 0xC0000409u);
  std::abort();
}

bool isNativeExecutableCallback(DarwinAllocOnceInitializer Initializer) {
  const auto Address = reinterpret_cast<std::uintptr_t>(Initializer);
  MEMORY_BASIC_INFORMATION Information{};
  if (VirtualQuery(reinterpret_cast<const void *>(Address), &Information,
                   sizeof(Information)) == 0 ||
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

void invokeAllocOnceInitializer(DarwinAllocOnceSlot *Slot, std::size_t Size,
                                DarwinAllocOnceInitializer Initializer,
                                void *Storage) {
  if (!Initializer)
    return;

  // CI semantic smokes use a real host callback, and future native host users
  // may legitimately do the same. Guest Mach-O code, by contrast, is mapped
  // into Unicorn from ordinary heap pages and must never be executed by x64.
  if (isNativeExecutableCallback(Initializer)) {
    Initializer(Storage);
    return;
  }

  HMODULE Core = GetModuleHandleW(L"IpaSimLibrary.dll");
  if (!Core)
    failFastAllocOnce("ARM64 initializer requires loaded IpaSimLibrary.dll",
                      Slot, Size,
                      reinterpret_cast<const void *>(
                          reinterpret_cast<std::uintptr_t>(Initializer)));

  auto BackCaller = reinterpret_cast<IpaSimGuestCallback1>(
      GetProcAddress(Core, "ipaSim_callBack1"));
  if (!BackCaller)
    failFastAllocOnce("IpaSimLibrary.dll is missing ipaSim_callBack1", Slot,
                      Size,
                      reinterpret_cast<const void *>(
                          reinterpret_cast<std::uintptr_t>(Initializer)));

  BackCaller(reinterpret_cast<void *>(
                 reinterpret_cast<std::uintptr_t>(Initializer)),
             Storage);
}

void *allocateOnceStorage(DarwinAllocOnceSlot *Slot, std::size_t Size,
                          DarwinAllocOnceInitializer Initializer) {
  if (Size == 0 ||
      Size > (std::numeric_limits<std::size_t>::max)() -
                 (DarwinAllocOnceAlignment - 1))
    failFastAllocOnce("invalid allocation size", Slot, Size,
                      reinterpret_cast<const void *>(
                          reinterpret_cast<std::uintptr_t>(Initializer)));

  const std::size_t AlignedSize =
      (Size + DarwinAllocOnceAlignment - 1) &
      ~(DarwinAllocOnceAlignment - 1);
  void *Storage = _aligned_malloc(AlignedSize, DarwinAllocOnceAlignment);
  if (!Storage)
    failFastAllocOnce("process-lifetime allocation failed", Slot, Size,
                      reinterpret_cast<const void *>(
                          reinterpret_cast<std::uintptr_t>(Initializer)));

  // Apple's backing pages arrive zero-filled. Preserve that observable
  // contract before publishing the pointer to the initializer.
  std::memset(Storage, 0, AlignedSize);
  return Storage;
}

} // namespace

// Mach-O __os_alloc_once_table normalizes to PE _os_alloc_once_table. Keep the
// complete 100-entry process-global writable table so token-based libplatform
// callers bind to real slot storage rather than to a fabricated address.
extern "C" {
alignas(16) __declspec(dllexport)
DarwinAllocOnceSlot _os_alloc_once_table[DarwinAllocOnceKeyMax] = {};
}

extern "C" __declspec(dllexport) int
__gettimeofday(DarwinTimeval *TimeValue, DarwinTimezone *Timezone) {
  if (TimeValue && !currentUnixTime(*TimeValue))
    return -1;
  if (Timezone && !currentTimezone(*Timezone))
    return -1;
  return 0;
}

extern "C" __declspec(dllexport) std::uint64_t mach_continuous_time() {
  return currentContinuousTime100ns();
}

extern "C" __declspec(dllexport) int
mach_timebase_info(DarwinMachTimebaseInfo *Info) {
  if (!Info)
    return DarwinKernInvalidArgument;

  Info->Numer = ContinuousTimeNumer;
  Info->Denom = ContinuousTimeDenom;
  return DarwinKernSuccess;
}

// Mach-O symbol __os_alloc_once is normalized by ipaSim to the PE export
// _os_alloc_once. The allocation deliberately has process lifetime, matching
// Apple's bump allocator: callers never free storage returned by this API.
extern "C" __declspec(dllexport) void *
_os_alloc_once(DarwinAllocOnceSlot *Slot, std::size_t Size,
               DarwinAllocOnceInitializer Initializer) {
  if (!Slot)
    failFastAllocOnce("null slot", Slot, Size,
                      reinterpret_cast<const void *>(
                          reinterpret_cast<std::uintptr_t>(Initializer)));

  const DWORD CurrentThread = GetCurrentThreadId();
  const std::shared_ptr<AllocOnceState> State = allocOnceState(Slot);
  std::unique_lock<std::mutex> Lock(State->Mutex);

  if (State->Phase == AllocOncePhase::Initialized) {
    if (!Slot->Ptr || Slot->Once != DarwinOsOnceDone)
      failFastAllocOnce("initialized slot state is corrupt", Slot, Size,
                        reinterpret_cast<const void *>(
                            reinterpret_cast<std::uintptr_t>(Initializer)));
    return Slot->Ptr;
  }

  while (State->Phase == AllocOncePhase::Initializing) {
    if (State->OwnerThreadId == CurrentThread)
      failFastAllocOnce("recursive initialization of the same slot", Slot,
                        Size,
                        reinterpret_cast<const void *>(
                            reinterpret_cast<std::uintptr_t>(Initializer)));
    State->Condition.wait(Lock);
    if (State->Phase == AllocOncePhase::Initialized) {
      if (!Slot->Ptr || Slot->Once != DarwinOsOnceDone)
        failFastAllocOnce("initialized slot state is corrupt", Slot, Size,
                          reinterpret_cast<const void *>(
                              reinterpret_cast<std::uintptr_t>(Initializer)));
      return Slot->Ptr;
    }
  }

  // A slot may have been initialized before this bridge first observed it.
  // Accept only the completed Darwin shape; any partial/nonzero once state with
  // no registry owner is corruption rather than a reason to invent success.
  if (Slot->Once == DarwinOsOnceDone && Slot->Ptr) {
    State->Phase = AllocOncePhase::Initialized;
    return Slot->Ptr;
  }
  if (Slot->Once != DarwinOsOnceInit || Slot->Ptr)
    failFastAllocOnce("unowned slot contains a partial/corrupt once state",
                      Slot, Size,
                      reinterpret_cast<const void *>(
                          reinterpret_cast<std::uintptr_t>(Initializer)));

  State->Phase = AllocOncePhase::Initializing;
  State->OwnerThreadId = CurrentThread;
  Lock.unlock();

  void *Storage = allocateOnceStorage(Slot, Size, Initializer);
  // Apple publishes slot->ptr before invoking the initializer. This matters to
  // initializer code that consults its own global alloc-once slot.
  Slot->Ptr = Storage;
  invokeAllocOnceInitializer(Slot, Size, Initializer, Storage);

  // _os_once completes with OS_ONCE_DONE (~0l on Darwin arm64) only after the
  // initializer returns. InterlockedExchange64 supplies the release boundary
  // before waiting callers are woken.
  InterlockedExchange64(reinterpret_cast<volatile LONG64 *>(&Slot->Once),
                        static_cast<LONG64>(DarwinOsOnceDone));

  Lock.lock();
  State->OwnerThreadId = 0;
  State->Phase = AllocOncePhase::Initialized;
  Lock.unlock();
  State->Condition.notify_all();
  return Storage;
}
