// DarwinPthreadCoreBridge.cpp: core Darwin pthread semantics used by
// libdispatch/libdyld when Apple's simulator pthread layer reexports its macOS
// host pthread dependency onto the Windows ipaSim host.
//
// The ABI is intentionally explicit. Darwin is LP64 while Win64 is LLP64, so
// pthread_attr_t, pthread_t identifiers, unsigned-long flags and sigset_t never
// use Windows host typedefs accidentally. Guest pthread start routines execute
// on real Windows threads and enter fresh Unicorn ARM64 CPU contexts through the
// ipaSim core's synchronous ipaSim_runGuestPthread() boundary.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "DarwinGuestMemory.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using DarwinPthread = void *;
using DarwinQosClass = std::uint32_t;
using DarwinUnsignedLong = std::uint64_t;
using DarwinSigset = std::uint32_t;

// Darwin errno values returned directly by pthread SPI. Keep explicit values
// where the Windows CRT differs (notably EDEADLK/EAGAIN/ENOTSUP).
constexpr int DarwinErrnoNoSuchProcess = 3;  // ESRCH
constexpr int DarwinErrnoDeadlock = 11;      // EDEADLK
constexpr int DarwinErrnoFault = 14;         // EFAULT
constexpr int DarwinErrnoBusy = 16;          // EBUSY
constexpr int DarwinErrnoInvalid = 22;       // EINVAL
constexpr int DarwinErrnoRange = 34;         // ERANGE
constexpr int DarwinErrnoTryAgain = 35;      // EAGAIN
constexpr int DarwinErrnoNotSupported = 45;  // ENOTSUP

constexpr std::uint64_t PthreadAttrSig = 0x54484441ULL; // 'THDA'
constexpr std::uint64_t PthreadSig = 0x54485244ULL;     // 'THRD'
constexpr std::size_t DarwinPthreadAttrSize = 64;
constexpr std::size_t DarwinPthreadTsdOffset = 224;
constexpr std::size_t DarwinPthreadTsdSlots = 768;
constexpr std::size_t DarwinPthreadObjectSize =
    DarwinPthreadTsdOffset + DarwinPthreadTsdSlots * sizeof(void *);

constexpr int PthreadCreateJoinable = 1;
constexpr int PthreadCreateDetached = 2;
constexpr int PthreadInheritSched = 1;
constexpr int SchedOther = 1; // POLICY_TIMESHARE
constexpr int SchedRR = 2;    // POLICY_RR
constexpr int SchedFIFO = 4;  // POLICY_FIFO
constexpr int DefaultSchedPriority = 31;
constexpr int DefaultSchedQuantum = 10;
constexpr std::uint32_t MaxRefillMilliseconds = (2u << 24) - 1u;

constexpr std::uint32_t DetachedMask = 0x000000ffu;
constexpr unsigned DetachedShift = 0;
constexpr std::uint32_t InheritMask = 0x0000ff00u;
constexpr unsigned InheritShift = 8;
constexpr std::uint32_t PolicyMask = 0x00ff0000u;
constexpr unsigned PolicyShift = 16;
constexpr std::uint32_t SchedSetBit = 0x01000000u;
constexpr std::uint32_t QosSetBit = 0x02000000u;
constexpr std::uint32_t PolicySetBit = 0x04000000u;
constexpr std::uint32_t CpuPercentSetBit = 0x08000000u;
constexpr std::uint32_t DefaultGuardPageBit = 0x10000000u;

constexpr DarwinQosClass QosClassUnspecified = 0x00;
constexpr DarwinQosClass QosClassMaintenance = 0x05;
constexpr DarwinQosClass QosClassBackground = 0x09;
constexpr DarwinQosClass QosClassUtility = 0x11;
constexpr DarwinQosClass QosClassDefault = 0x15;
constexpr DarwinQosClass QosClassUserInitiated = 0x19;
constexpr DarwinQosClass QosClassUserInteractive = 0x21;
constexpr std::uint64_t DefaultQosPriority = 0x000008ffULL;
constexpr std::uint64_t ValidQosMask = 0x00003f00ULL;
constexpr unsigned QosShift = 8;
constexpr std::uint64_t RelativePriorityMask = 0xffULL;

constexpr DarwinUnsignedLong MaxParallelismPhysical = 0x1ULL;

constexpr int DarwinSigBlock = 1;
constexpr int DarwinSigUnblock = 2;
constexpr int DarwinSigSetMask = 3;

struct DarwinSchedParam {
  std::int32_t SchedPriority;
  std::int32_t Quantum;
};
static_assert(sizeof(DarwinSchedParam) == 8,
              "Darwin sched_param LP64 shape changed");

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
static_assert(sizeof(DarwinPthreadAttr) == DarwinPthreadAttrSize,
              "Darwin pthread_attr_t must remain 64 bytes on LP64");
static_assert(offsetof(DarwinPthreadAttr, Flags) == 40,
              "Darwin pthread_attr flags offset changed");
static_assert(offsetof(DarwinPthreadAttr, CpuPercentAndRefill) == 44,
              "Darwin pthread_attr cpu/refill offset changed");

struct alignas(64) DarwinPthreadObject {
  std::array<std::uint8_t, DarwinPthreadObjectSize> Bytes{};
};

struct ThreadRecord {
  DarwinPthreadObject Guest;
  std::uint64_t ThreadId = 0;
  DWORD WindowsThreadId = 0;
  HANDLE Handle = nullptr;
  bool IsMain = false;
  bool Detached = false;
  bool Exited = false;
  bool JoinInProgress = false;
  void *ExitValue = nullptr;
  void *StartRoutine = nullptr;
  void *StartArgument = nullptr;
  void *ObservedStackTop = nullptr;
  DarwinQosClass QosClass = QosClassDefault;
  int RelativePriority = 0;
  std::array<char, 64> Name{};
  std::mutex Mutex;
};
static_assert(offsetof(ThreadRecord, Guest) == 0,
              "pthread_t must remain the address of the Darwin guest object");

std::mutex RegistryMutex;
std::unordered_map<DarwinPthread, std::shared_ptr<ThreadRecord>> Threads;
std::atomic<std::uint64_t> NextThreadId{1};
const DWORD BridgeLoadThreadId = GetCurrentThreadId();
thread_local ThreadRecord *CurrentThread = nullptr;
thread_local DarwinSigset CurrentSignalMask = 0;
thread_local bool CurrentRoutineIsNative = false;

std::mutex WorkgroupMutex;
void *InstalledWorkgroupFunctions = nullptr;

bool readable(const void *Address, std::size_t Size) {
  return ipasim::darwinmem::readableSpan(Address, Size);
}

bool writable(void *Address, std::size_t Size) {
  return ipasim::darwinmem::writableSpan(Address, Size);
}

template <typename T> bool copyIn(const T *Address, T &Value) {
  if (!Address || !readable(Address, sizeof(T)))
    return false;
  std::memcpy(&Value, Address, sizeof(T));
  return true;
}

template <typename T> bool copyOut(T *Address, const T &Value) {
  if (!Address || !writable(Address, sizeof(T)))
    return false;
  std::memcpy(Address, &Value, sizeof(T));
  return true;
}

bool readValidAttr(const DarwinPthreadAttr *Address, DarwinPthreadAttr &Attr,
                   int &Error) {
  if (!Address) {
    Error = DarwinErrnoInvalid;
    return false;
  }
  if (!copyIn(Address, Attr)) {
    Error = DarwinErrnoFault;
    return false;
  }
  if (Attr.Sig != PthreadAttrSig) {
    Error = DarwinErrnoInvalid;
    return false;
  }
  return true;
}

bool writeAttr(DarwinPthreadAttr *Address, const DarwinPthreadAttr &Attr,
               int &Error) {
  if (!Address) {
    Error = DarwinErrnoInvalid;
    return false;
  }
  if (!copyOut(Address, Attr)) {
    Error = DarwinErrnoFault;
    return false;
  }
  return true;
}

std::uint8_t attrDetached(const DarwinPthreadAttr &Attr) {
  return static_cast<std::uint8_t>((Attr.Flags & DetachedMask) >> DetachedShift);
}

std::uint8_t attrPolicy(const DarwinPthreadAttr &Attr) {
  return static_cast<std::uint8_t>((Attr.Flags & PolicyMask) >> PolicyShift);
}

void setAttrDetached(DarwinPthreadAttr &Attr, std::uint8_t Value) {
  Attr.Flags = (Attr.Flags & ~DetachedMask) |
               (std::uint32_t{Value} << DetachedShift);
}

void setAttrInherit(DarwinPthreadAttr &Attr, std::uint8_t Value) {
  Attr.Flags = (Attr.Flags & ~InheritMask) |
               (std::uint32_t{Value} << InheritShift);
}

void setAttrPolicy(DarwinPthreadAttr &Attr, std::uint8_t Value) {
  Attr.Flags = (Attr.Flags & ~PolicyMask) |
               (std::uint32_t{Value} << PolicyShift);
}

DarwinPthreadAttr defaultAttr() {
  DarwinPthreadAttr Attr{};
  Attr.Sig = PthreadAttrSig;
  Attr.Scheduling.QosClass = DefaultQosPriority;
  Attr.Flags = DefaultGuardPageBit;
  setAttrDetached(Attr, PthreadCreateJoinable);
  setAttrInherit(Attr, PthreadInheritSched);
  setAttrPolicy(Attr, SchedOther);
  return Attr;
}

bool fixedPolicy(int Policy) { return Policy == SchedRR || Policy == SchedFIFO; }

DarwinQosClass qosClassFromCompact(std::uint64_t Priority) {
  const std::uint64_t Bits = (Priority & ValidQosMask) >> QosShift;
  for (unsigned I = 1; I <= 6; ++I) {
    if ((Bits & (std::uint64_t{1} << (I - 1))) == 0)
      continue;
    switch (I) {
    case 1: return QosClassMaintenance;
    case 2: return QosClassBackground;
    case 3: return QosClassUtility;
    case 4: return QosClassDefault;
    case 5: return QosClassUserInitiated;
    case 6: return QosClassUserInteractive;
    default: break;
    }
  }
  return QosClassUnspecified;
}

int relativePriorityFromCompact(std::uint64_t Priority) {
  if ((Priority & ValidQosMask) == 0)
    return 0;
  return static_cast<std::int8_t>(Priority & RelativePriorityMask) + 1;
}

void initializeGuestObject(ThreadRecord &Record) {
  std::memset(Record.Guest.Bytes.data(), 0, Record.Guest.Bytes.size());
  std::memcpy(Record.Guest.Bytes.data(), &PthreadSig, sizeof(PthreadSig));
  static_assert(DarwinPthreadTsdOffset >= sizeof(std::uint64_t));
  std::memcpy(Record.Guest.Bytes.data() + DarwinPthreadTsdOffset - 8,
              &Record.ThreadId, sizeof(Record.ThreadId));
}

DarwinPthread pthreadIdentity(ThreadRecord &Record) {
  return static_cast<void *>(&Record.Guest);
}

std::shared_ptr<ThreadRecord> lookupRecord(DarwinPthread Thread) {
  std::lock_guard<std::mutex> Guard(RegistryMutex);
  const auto It = Threads.find(Thread);
  return It == Threads.end() ? nullptr : It->second;
}

void registerRecord(const std::shared_ptr<ThreadRecord> &Record) {
  std::lock_guard<std::mutex> Guard(RegistryMutex);
  Threads[pthreadIdentity(*Record)] = Record;
}

void eraseRecord(DarwinPthread Thread) {
  std::lock_guard<std::mutex> Guard(RegistryMutex);
  Threads.erase(Thread);
}

HMODULE requireCore() {
  HMODULE Core = GetModuleHandleW(L"IpaSimLibrary.dll");
  if (!Core)
    Core = GetModuleHandleW(L"libIpaSimLibrary.dll");
  return Core;
}

int queryMainGuestContext() {
  HMODULE Core = requireCore();
  if (!Core)
    return -1;
  using Query = int (*)();
  auto Function = reinterpret_cast<Query>(
      GetProcAddress(Core, "ipaSim_isMainGuestContext"));
  return Function ? Function() : -1;
}

void *queryCurrentGuestStackTop() {
  HMODULE Core = requireCore();
  if (!Core)
    return nullptr;
  using Query = void *(*)();
  auto Function = reinterpret_cast<Query>(
      GetProcAddress(Core, "ipaSim_currentGuestStackTop"));
  return Function ? Function() : nullptr;
}

bool requestGuestPthreadExit(void *ExitValue) {
  HMODULE Core = requireCore();
  if (!Core)
    return false;
  using Request = int (*)(void *);
  auto Function = reinterpret_cast<Request>(
      GetProcAddress(Core, "ipaSim_requestGuestPthreadExit"));
  return Function && Function(ExitValue) == 0;
}

std::shared_ptr<ThreadRecord> currentRecordShared() {
  if (CurrentThread) {
    auto Existing = lookupRecord(pthreadIdentity(*CurrentThread));
    if (Existing)
      return Existing;
  }

  auto Record = std::make_shared<ThreadRecord>();
  Record->ThreadId = NextThreadId.fetch_add(1, std::memory_order_relaxed);
  Record->WindowsThreadId = GetCurrentThreadId();
  const int MainGuest = queryMainGuestContext();
  Record->IsMain = MainGuest >= 0 ? MainGuest == 1
                                  : Record->WindowsThreadId == BridgeLoadThreadId;
  Record->Detached = !Record->IsMain;
  initializeGuestObject(*Record);
  registerRecord(Record);
  CurrentThread = Record.get();
  return Record;
}

bool isNativeExecutableCallback(void *Callback) {
  if (!Callback)
    return false;
  MEMORY_BASIC_INFORMATION Information{};
  if (!VirtualQuery(Callback, &Information, sizeof(Information)) ||
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

void *invokeStartRoutine(void *Routine, void *Argument, int &CoreResult) {
  CoreResult = 0;
  CurrentRoutineIsNative = isNativeExecutableCallback(Routine);
  if (CurrentRoutineIsNative)
    return reinterpret_cast<void *(*)(void *)>(Routine)(Argument);

  HMODULE Core = requireCore();
  if (!Core) {
    CoreResult = DarwinErrnoNoSuchProcess;
    return nullptr;
  }
  using RunGuestPthread = int (*)(void *, void *, void **);
  auto Run = reinterpret_cast<RunGuestPthread>(
      GetProcAddress(Core, "ipaSim_runGuestPthread"));
  if (!Run) {
    CoreResult = DarwinErrnoNotSupported;
    return nullptr;
  }

  void *ReturnValue = nullptr;
  CoreResult = Run(Routine, Argument, &ReturnValue);
  return ReturnValue;
}

void finishRecord(ThreadRecord &Record, void *ExitValue) {
  bool Cleanup = false;
  HANDLE Handle = nullptr;
  const DarwinPthread Identity = pthreadIdentity(Record);
  {
    std::lock_guard<std::mutex> Guard(Record.Mutex);
    Record.ExitValue = ExitValue;
    Record.Exited = true;
    Cleanup = Record.Detached;
    Handle = Record.Handle;
    if (Cleanup)
      Record.Handle = nullptr;
  }
  if (Cleanup) {
    eraseRecord(Identity);
    if (Handle)
      CloseHandle(Handle);
  }
}

DWORD WINAPI pthreadStartThunk(void *Parameter) {
  auto *Record = static_cast<ThreadRecord *>(Parameter);
  {
    const auto Registered = lookupRecord(pthreadIdentity(*Record));
    if (!Registered || Registered.get() != Record)
      return static_cast<DWORD>(DarwinErrnoNoSuchProcess);
  }

  CurrentThread = Record;
  void *ExitValue = nullptr;
  int CoreResult = 0;
  ExitValue = invokeStartRoutine(Record->StartRoutine,
                                 Record->StartArgument, CoreResult);
  CurrentRoutineIsNative = false;
  if (CoreResult != 0)
    ExitValue = reinterpret_cast<void *>(static_cast<std::uintptr_t>(CoreResult));
  finishRecord(*Record, ExitValue);
  CurrentThread = nullptr;
  return 0;
}

unsigned logicalProcessorCount() {
  const DWORD Count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  return Count ? Count : 1u;
}

unsigned physicalProcessorCount() {
  DWORD Bytes = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &Bytes);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || Bytes == 0)
    return logicalProcessorCount();

  std::vector<std::uint8_t> Storage(Bytes);
  auto *Info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
      Storage.data());
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, Info, &Bytes))
    return logicalProcessorCount();

  unsigned Count = 0;
  std::size_t Offset = 0;
  while (Offset < Bytes) {
    auto *Entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
        Storage.data() + Offset);
    if (Entry->Relationship == RelationProcessorCore)
      ++Count;
    if (Entry->Size == 0)
      break;
    Offset += Entry->Size;
  }
  return Count ? Count : logicalProcessorCount();
}

int maxParallelism(DarwinUnsignedLong Flags) {
  if ((Flags & ~MaxParallelismPhysical) != 0) {
    errno = EINVAL;
    return -1;
  }
  return static_cast<int>((Flags & MaxParallelismPhysical)
                              ? physicalProcessorCount()
                              : logicalProcessorCount());
}

int copyThreadName(const char *Name, std::array<char, 64> &Destination) {
  if (!Name)
    return DarwinErrnoInvalid;

  Destination.fill('\0');
  for (std::size_t I = 0; I != Destination.size(); ++I) {
    if (!readable(Name + I, 1))
      return DarwinErrnoFault;
    const char Character = Name[I];
    if (Character == '\0')
      return 0;
    if (I == Destination.size() - 1)
      return DarwinErrnoRange;
    Destination[I] = Character;
  }
  return DarwinErrnoRange;
}

} // namespace

extern "C" {

__declspec(dllexport) int pthread_attr_init(DarwinPthreadAttr *Attr) {
  if (!Attr)
    return DarwinErrnoInvalid;
  const DarwinPthreadAttr Initial = defaultAttr();
  return copyOut(Attr, Initial) ? 0 : DarwinErrnoFault;
}

__declspec(dllexport) int pthread_attr_destroy(DarwinPthreadAttr *Attr) {
  DarwinPthreadAttr Current{};
  int Error = 0;
  if (!readValidAttr(Attr, Current, Error))
    return Error;
  Current.Sig = 0;
  return writeAttr(Attr, Current, Error) ? 0 : Error;
}

__declspec(dllexport) int
pthread_attr_getschedparam(const DarwinPthreadAttr *Attr, DarwinSchedParam *Param) {
  DarwinPthreadAttr Current{};
  int Error = 0;
  if (!readValidAttr(Attr, Current, Error))
    return Error;
  if (!Param)
    return DarwinErrnoInvalid;
  const DarwinSchedParam Value = (Current.Flags & SchedSetBit)
                                     ? Current.Scheduling.Param
                                     : DarwinSchedParam{DefaultSchedPriority,
                                                        DefaultSchedQuantum};
  return copyOut(Param, Value) ? 0 : DarwinErrnoFault;
}

__declspec(dllexport) int
pthread_attr_getschedpolicy(const DarwinPthreadAttr *Attr, int *Policy) {
  DarwinPthreadAttr Current{};
  int Error = 0;
  if (!readValidAttr(Attr, Current, Error))
    return Error;
  if (!Policy)
    return DarwinErrnoInvalid;
  const int Value = attrPolicy(Current);
  return copyOut(Policy, Value) ? 0 : DarwinErrnoFault;
}

__declspec(dllexport) int
pthread_attr_setdetachstate(DarwinPthreadAttr *Attr, int Detached) {
  DarwinPthreadAttr Current{};
  int Error = 0;
  if (!readValidAttr(Attr, Current, Error))
    return Error;
  if (Detached != PthreadCreateJoinable && Detached != PthreadCreateDetached)
    return DarwinErrnoInvalid;
  setAttrDetached(Current, static_cast<std::uint8_t>(Detached));
  return writeAttr(Attr, Current, Error) ? 0 : Error;
}

__declspec(dllexport) int
pthread_attr_setschedparam(DarwinPthreadAttr *Attr,
                           const DarwinSchedParam *Param) {
  DarwinPthreadAttr Current{};
  int Error = 0;
  if (!readValidAttr(Attr, Current, Error))
    return Error;
  DarwinSchedParam Requested{};
  if (!Param)
    return DarwinErrnoInvalid;
  if (!copyIn(Param, Requested))
    return DarwinErrnoFault;
  Current.Scheduling.Param = Requested;
  Current.Flags |= SchedSetBit;
  Current.Flags &= ~QosSetBit;
  return writeAttr(Attr, Current, Error) ? 0 : Error;
}

__declspec(dllexport) int
pthread_attr_setschedpolicy(DarwinPthreadAttr *Attr, int Policy) {
  DarwinPthreadAttr Current{};
  int Error = 0;
  if (!readValidAttr(Attr, Current, Error))
    return Error;
  if (Policy != SchedOther && Policy != SchedRR && Policy != SchedFIFO)
    return DarwinErrnoInvalid;
  if (!fixedPolicy(Policy))
    Current.Flags &= ~CpuPercentSetBit;
  setAttrPolicy(Current, static_cast<std::uint8_t>(Policy));
  Current.Flags |= PolicySetBit;
  return writeAttr(Attr, Current, Error) ? 0 : Error;
}

__declspec(dllexport) int
pthread_attr_setcpupercent_np(DarwinPthreadAttr *Attr, int Percent,
                              DarwinUnsignedLong RefillMilliseconds) {
  DarwinPthreadAttr Current{};
  int Error = 0;
  if (!readValidAttr(Attr, Current, Error))
    return Error;
  if (Percent < 0 || Percent >= 255 ||
      RefillMilliseconds >= MaxRefillMilliseconds ||
      !(Current.Flags & PolicySetBit) || !fixedPolicy(attrPolicy(Current)))
    return DarwinErrnoInvalid;
  Current.CpuPercentAndRefill =
      static_cast<std::uint32_t>(Percent) |
      (static_cast<std::uint32_t>(RefillMilliseconds & 0x00ffffffULL) << 8);
  Current.Flags |= CpuPercentSetBit;
  return writeAttr(Attr, Current, Error) ? 0 : Error;
}

__declspec(dllexport) int
pthread_attr_get_qos_class_np(const DarwinPthreadAttr *Attr,
                              DarwinQosClass *Qos, int *RelativePriority) {
  DarwinPthreadAttr Current{};
  int Error = 0;
  if (!readValidAttr(Attr, Current, Error))
    return Error;
  if (Qos && !writable(Qos, sizeof(*Qos)))
    return DarwinErrnoFault;
  if (RelativePriority && !writable(RelativePriority, sizeof(*RelativePriority)))
    return DarwinErrnoFault;

  const std::uint64_t Priority = Current.Scheduling.QosClass;
  if (Qos) {
    const DarwinQosClass Value = (Current.Flags & QosSetBit)
                                     ? qosClassFromCompact(Priority)
                                     : QosClassUnspecified;
    std::memcpy(Qos, &Value, sizeof(Value));
  }
  if (RelativePriority) {
    const int Value = (Current.Flags & QosSetBit)
                          ? relativePriorityFromCompact(Priority)
                          : 0;
    std::memcpy(RelativePriority, &Value, sizeof(Value));
  }
  return 0;
}

__declspec(dllexport) DarwinPthread pthread_self(void) {
  return pthreadIdentity(*currentRecordShared());
}

__declspec(dllexport) int pthread_main_np(void) {
  return currentRecordShared()->IsMain ? 1 : 0;
}

__declspec(dllexport) int
pthread_threadid_np(DarwinPthread Thread, std::uint64_t *ThreadId) {
  if (!ThreadId)
    return DarwinErrnoInvalid;
  if (!writable(ThreadId, sizeof(*ThreadId)))
    return DarwinErrnoFault;
  auto Record = Thread ? lookupRecord(Thread) : currentRecordShared();
  if (!Record)
    return DarwinErrnoNoSuchProcess;
  std::memcpy(ThreadId, &Record->ThreadId, sizeof(Record->ThreadId));
  return 0;
}

__declspec(dllexport) int
pthread_get_qos_class_np(DarwinPthread Thread, DarwinQosClass *Qos,
                         int *RelativePriority) {
  if (Qos && !writable(Qos, sizeof(*Qos)))
    return DarwinErrnoFault;
  if (RelativePriority && !writable(RelativePriority, sizeof(*RelativePriority)))
    return DarwinErrnoFault;
  auto Record = Thread ? lookupRecord(Thread) : currentRecordShared();
  if (!Record)
    return DarwinErrnoNoSuchProcess;
  if (Qos)
    std::memcpy(Qos, &Record->QosClass, sizeof(Record->QosClass));
  if (RelativePriority)
    std::memcpy(RelativePriority, &Record->RelativePriority,
                sizeof(Record->RelativePriority));
  return 0;
}

__declspec(dllexport) void *pthread_get_stackaddr_np(DarwinPthread Thread) {
  auto Record = Thread ? lookupRecord(Thread) : currentRecordShared();
  if (!Record)
    return nullptr;

  // Apple exposes the actual top address of the downward-growing pthread stack.
  // Only the currently executing guest thread can query its active SysTranslator
  // directly. Never manufacture a process address for another thread merely to
  // make this API non-null.
  if (CurrentThread == Record.get()) {
    if (void *StackTop = queryCurrentGuestStackTop()) {
      std::lock_guard<std::mutex> Guard(Record->Mutex);
      Record->ObservedStackTop = StackTop;
      return StackTop;
    }
  }

  std::lock_guard<std::mutex> Guard(Record->Mutex);
  return Record->ObservedStackTop;
}

__declspec(dllexport) int pthread_setname_np(const char *Name) {
  auto Record = currentRecordShared();
  std::array<char, 64> Requested{};
  const int CopyResult = copyThreadName(Name, Requested);
  if (CopyResult != 0)
    return CopyResult;

  {
    std::lock_guard<std::mutex> Guard(Record->Mutex);
    Record->Name = Requested;
  }

  const char *Utf8 = Requested.data();
  const int WideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              Utf8, -1, nullptr, 0);
  if (WideLength > 0) {
    std::wstring Wide(static_cast<std::size_t>(WideLength), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Utf8, -1,
                            Wide.data(), WideLength) > 0) {
      HANDLE Handle = Record->Handle ? Record->Handle : GetCurrentThread();
      SetThreadDescription(Handle, Wide.c_str());
    }
  }
  return 0;
}

__declspec(dllexport) int pthread_sigmask(int How, const DarwinSigset *Set,
                                          DarwinSigset *OldSet) {
  if (Set && !readable(Set, sizeof(*Set)))
    return DarwinErrnoFault;
  if (OldSet && !writable(OldSet, sizeof(*OldSet)))
    return DarwinErrnoFault;

  DarwinSigset Requested = 0;
  if (Set)
    std::memcpy(&Requested, Set, sizeof(Requested));
  if (OldSet)
    std::memcpy(OldSet, &CurrentSignalMask, sizeof(CurrentSignalMask));
  if (!Set)
    return 0;

  // Signal delivery remains a separate subsystem; this preserves the observable
  // per-thread mask state without claiming that Windows signals are Darwin
  // signals or that unsupported waits are implemented.
  switch (How) {
  case DarwinSigBlock:
    CurrentSignalMask |= Requested;
    return 0;
  case DarwinSigUnblock:
    CurrentSignalMask &= ~Requested;
    return 0;
  case DarwinSigSetMask:
    CurrentSignalMask = Requested;
    return 0;
  default:
    return DarwinErrnoInvalid;
  }
}

__declspec(dllexport) int pthread_create(DarwinPthread *Thread,
                                         const DarwinPthreadAttr *Attr,
                                         void *StartRoutine, void *Argument) {
  if (!Thread || !StartRoutine)
    return DarwinErrnoInvalid;
  if (!writable(Thread, sizeof(*Thread)))
    return DarwinErrnoFault;

  DarwinPthreadAttr Effective = defaultAttr();
  if (Attr) {
    int Error = 0;
    if (!readValidAttr(Attr, Effective, Error))
      return Error;
  }

  // The current execution-context allocator owns the guest stack. Do not ignore
  // a caller-provided stack or explicit fixed-priority/CPU-limit request and then
  // report success as if Darwin honored it.
  if (Effective.StackAddress || Effective.StackSize ||
      (Effective.Flags & (SchedSetBit | CpuPercentSetBit)) != 0 ||
      ((Effective.Flags & PolicySetBit) != 0 &&
       fixedPolicy(attrPolicy(Effective))))
    return DarwinErrnoNotSupported;

  auto Record = std::make_shared<ThreadRecord>();
  Record->ThreadId = NextThreadId.fetch_add(1, std::memory_order_relaxed);
  Record->Detached = attrDetached(Effective) == PthreadCreateDetached;
  Record->StartRoutine = StartRoutine;
  Record->StartArgument = Argument;
  if (Effective.Flags & QosSetBit) {
    Record->QosClass = qosClassFromCompact(Effective.Scheduling.QosClass);
    Record->RelativePriority =
        relativePriorityFromCompact(Effective.Scheduling.QosClass);
  }
  initializeGuestObject(*Record);
  registerRecord(Record);

  DWORD WindowsThreadId = 0;
  HANDLE Handle = CreateThread(nullptr, 0, pthreadStartThunk, Record.get(),
                               CREATE_SUSPENDED, &WindowsThreadId);
  if (!Handle) {
    eraseRecord(pthreadIdentity(*Record));
    return DarwinErrnoTryAgain;
  }

  Record->Handle = Handle;
  Record->WindowsThreadId = WindowsThreadId;
  const DarwinPthread Identity = pthreadIdentity(*Record);
  std::memcpy(Thread, &Identity, sizeof(Identity));

  if (ResumeThread(Handle) == static_cast<DWORD>(-1)) {
    TerminateThread(Handle, static_cast<DWORD>(DarwinErrnoTryAgain));
    eraseRecord(Identity);
    CloseHandle(Handle);
    const DarwinPthread NullThread = nullptr;
    std::memcpy(Thread, &NullThread, sizeof(NullThread));
    return DarwinErrnoTryAgain;
  }
  return 0;
}

__declspec(dllexport) int pthread_detach(DarwinPthread Thread) {
  auto Record = lookupRecord(Thread);
  if (!Record || Record->IsMain)
    return DarwinErrnoNoSuchProcess;

  bool Cleanup = false;
  HANDLE Handle = nullptr;
  {
    std::lock_guard<std::mutex> Guard(Record->Mutex);
    if (Record->Detached || Record->JoinInProgress)
      return DarwinErrnoInvalid;
    Record->Detached = true;
    Cleanup = Record->Exited;
    if (Cleanup) {
      Handle = Record->Handle;
      Record->Handle = nullptr;
    }
  }
  if (Cleanup) {
    eraseRecord(Thread);
    if (Handle)
      CloseHandle(Handle);
  }
  return 0;
}

__declspec(dllexport) int pthread_join(DarwinPthread Thread, void **ExitValue) {
  auto Record = lookupRecord(Thread);
  if (!Record || Record->IsMain)
    return DarwinErrnoNoSuchProcess;
  if (CurrentThread == Record.get())
    return DarwinErrnoDeadlock;
  if (ExitValue && !writable(ExitValue, sizeof(*ExitValue)))
    return DarwinErrnoFault;

  HANDLE Handle = nullptr;
  {
    std::lock_guard<std::mutex> Guard(Record->Mutex);
    if (Record->Detached || Record->JoinInProgress)
      return DarwinErrnoInvalid;
    Handle = Record->Handle;
    if (!Handle)
      return DarwinErrnoNoSuchProcess;
    Record->JoinInProgress = true;
  }

  const DWORD Wait = WaitForSingleObject(Handle, INFINITE);
  if (Wait != WAIT_OBJECT_0) {
    std::lock_guard<std::mutex> Guard(Record->Mutex);
    Record->JoinInProgress = false;
    return DarwinErrnoInvalid;
  }

  void *Value = nullptr;
  {
    std::lock_guard<std::mutex> Guard(Record->Mutex);
    Value = Record->ExitValue;
    Record->Detached = true;
    Record->JoinInProgress = false;
    Record->Handle = nullptr;
  }
  if (ExitValue)
    std::memcpy(ExitValue, &Value, sizeof(Value));
  eraseRecord(Thread);
  CloseHandle(Handle);
  return 0;
}

// A native executable start routine exists only for the standalone provider
// smoke and host-side semantic tests; it owns no GuestExecutionContext. Record
// its pthread result before ending that backing Windows thread. A real ARM64
// guest call instead requests logical guest execution termination below so the
// Unicorn context and its C++ stack/CPU ownership unwind normally.
__declspec(dllexport) void pthread_exit(void *ExitValue) {
  if (CurrentRoutineIsNative && CurrentThread) {
    ThreadRecord *Record = CurrentThread;
    CurrentRoutineIsNative = false;
    CurrentThread = nullptr;
    finishRecord(*Record, ExitValue);
    ExitThread(0);
    __assume(0);
  }

  if (requestGuestPthreadExit(ExitValue))
    return;

  std::fprintf(stderr,
               "[darwin-pthread-core] pthread_exit reached without an active "
               "guest execution context\n");
  std::fflush(stderr);
  RaiseFailFastException(nullptr, nullptr, 0);
  TerminateProcess(GetCurrentProcess(), 0xC0000409u);
}

__declspec(dllexport) int pthread_qos_max_parallelism(DarwinQosClass Qos,
                                                       DarwinUnsignedLong Flags) {
  switch (Qos) {
  case QosClassUnspecified:
  case QosClassMaintenance:
  case QosClassBackground:
  case QosClassUtility:
  case QosClassDefault:
  case QosClassUserInitiated:
  case QosClassUserInteractive:
    return maxParallelism(Flags);
  default:
    errno = EINVAL;
    return -1;
  }
}

__declspec(dllexport) int
pthread_time_constraint_max_parallelism(DarwinUnsignedLong Flags) {
  return maxParallelism(Flags);
}

__declspec(dllexport) DarwinQosClass qos_class_main(void) {
  return QosClassDefault;
}

__declspec(dllexport) int pthread_cpu_number_np(std::size_t *CpuNumber) {
  if (!CpuNumber)
    return DarwinErrnoInvalid;
  if (!writable(CpuNumber, sizeof(*CpuNumber)))
    return DarwinErrnoFault;
  PROCESSOR_NUMBER Number{};
  GetCurrentProcessorNumberEx(&Number);
  const std::size_t Value = static_cast<std::size_t>(Number.Number);
  std::memcpy(CpuNumber, &Value, sizeof(Value));
  return 0;
}

__declspec(dllexport) int
pthread_install_workgroup_functions_np(void *Functions) {
  if (!Functions)
    return DarwinErrnoInvalid;
  std::lock_guard<std::mutex> Guard(WorkgroupMutex);
  if (InstalledWorkgroupFunctions && InstalledWorkgroupFunctions != Functions)
    return DarwinErrnoBusy;
  // Installation is process-visible state. Workgroup creation/dispatch remains
  // fail-closed elsewhere until the installed callbacks have a real guest-aware
  // execution path.
  InstalledWorkgroupFunctions = Functions;
  return 0;
}

} // extern "C"
