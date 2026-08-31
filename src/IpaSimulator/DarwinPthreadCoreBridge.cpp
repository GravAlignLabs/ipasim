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

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
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

constexpr std::uint64_t PthreadAttrSig = 0x54484441ULL; // 'THDA'
constexpr std::uint64_t PthreadSig = 0x54485244ULL;     // 'THRD'
constexpr std::size_t DarwinPthreadAttrSize = 64;
constexpr std::size_t DarwinPthreadTsdOffset = 224;
constexpr std::size_t DarwinPthreadTsdSlots = 768; // simulator/macOS layout
constexpr std::size_t DarwinPthreadObjectSize =
    DarwinPthreadTsdOffset + DarwinPthreadTsdSlots * sizeof(void *);

constexpr int PthreadCreateJoinable = 1;
constexpr int PthreadCreateDetached = 2;
constexpr int PthreadInheritSched = 1;
constexpr int PthreadExplicitSched = 2;
constexpr int SchedOther = 1; // POLICY_TIMESHARE
constexpr int SchedRR = 2;    // POLICY_RR
constexpr int SchedFIFO = 4;  // POLICY_FIFO
constexpr int DefaultSchedPriority = 31;
constexpr int DefaultSchedQuantum = 10;
constexpr std::size_t DefaultDarwinStackSize = 512 * 1024;
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
constexpr int MinimumRelativePriority = -15;

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
  bool OwnedThread = false;
  void *ExitValue = nullptr;
  void *StartRoutine = nullptr;
  void *StartArgument = nullptr;
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

std::mutex WorkgroupMutex;
void *InstalledWorkgroupFunctions = nullptr;

std::uint8_t attrDetached(const DarwinPthreadAttr &Attr) {
  return static_cast<std::uint8_t>((Attr.Flags & DetachedMask) >> DetachedShift);
}
std::uint8_t attrInherit(const DarwinPthreadAttr &Attr) {
  return static_cast<std::uint8_t>((Attr.Flags & InheritMask) >> InheritShift);
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

bool validAttr(const DarwinPthreadAttr *Attr) {
  return Attr && Attr->Sig == PthreadAttrSig;
}

bool fixedPolicy(int Policy) { return Policy == SchedRR || Policy == SchedFIFO; }

DarwinQosClass qosClassFromCompact(std::uint64_t Priority) {
  const std::uint64_t Bits = (Priority & ValidQosMask) >> QosShift;
  unsigned Index = 0;
  for (unsigned I = 1; I <= 6; ++I) {
    if (Bits & (std::uint64_t{1} << (I - 1))) {
      Index = I;
      break;
    }
  }
  switch (Index) {
  case 1: return QosClassMaintenance;
  case 2: return QosClassBackground;
  case 3: return QosClassUtility;
  case 4: return QosClassDefault;
  case 5: return QosClassUserInitiated;
  case 6: return QosClassUserInteractive;
  default: return QosClassUnspecified;
  }
}

int relativePriorityFromCompact(std::uint64_t Priority) {
  if ((Priority & ValidQosMask) == 0)
    return 0;
  return static_cast<std::int8_t>(Priority & RelativePriorityMask) + 1;
}

void initializeGuestObject(ThreadRecord &Record) {
  std::memset(Record.Guest.Bytes.data(), 0, Record.Guest.Bytes.size());
  std::memcpy(Record.Guest.Bytes.data(), &PthreadSig, sizeof(PthreadSig));
  // Apple keeps thread_id immediately before the LP64 TSD region.
  static_assert(DarwinPthreadTsdOffset >= sizeof(std::uint64_t));
  std::memcpy(Record.Guest.Bytes.data() + DarwinPthreadTsdOffset - 8,
              &Record.ThreadId, sizeof(Record.ThreadId));
}

DarwinPthread pthreadIdentity(ThreadRecord &Record) {
  return static_cast<void *>(&Record.Guest);
}

std::shared_ptr<ThreadRecord> lookupRecord(DarwinPthread Thread) {
  std::lock_guard<std::mutex> Guard(RegistryMutex);
  auto It = Threads.find(Thread);
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

std::shared_ptr<ThreadRecord> currentRecordShared() {
  if (CurrentThread) {
    auto Existing = lookupRecord(pthreadIdentity(*CurrentThread));
    if (Existing)
      return Existing;
  }

  auto Record = std::make_shared<ThreadRecord>();
  Record->ThreadId = NextThreadId.fetch_add(1, std::memory_order_relaxed);
  Record->WindowsThreadId = GetCurrentThreadId();
  Record->IsMain = Record->WindowsThreadId == BridgeLoadThreadId;
  Record->Detached = !Record->IsMain;
  Record->OwnedThread = false;
  initializeGuestObject(*Record);
  registerRecord(Record);
  CurrentThread = Record.get();
  return Record;
}

HMODULE requireCore() {
  HMODULE Core = GetModuleHandleW(L"IpaSimLibrary.dll");
  if (!Core)
    Core = GetModuleHandleW(L"libIpaSimLibrary.dll");
  return Core;
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
  if (isNativeExecutableCallback(Routine))
    return reinterpret_cast<void *(*)(void *)>(Routine)(Argument);

  HMODULE Core = requireCore();
  if (!Core) {
    CoreResult = ENOENT;
    return nullptr;
  }
  using RunGuestPthread = int (*)(void *, void *, void **);
  auto Run = reinterpret_cast<RunGuestPthread>(
      GetProcAddress(Core, "ipaSim_runGuestPthread"));
  if (!Run) {
    CoreResult = ENOSYS;
    return nullptr;
  }

  void *ReturnValue = nullptr;
  CoreResult = Run(Routine, Argument, &ReturnValue);
  return ReturnValue;
}

void finishRecord(ThreadRecord &Record, void *ExitValue) {
  bool Cleanup = false;
  HANDLE Handle = nullptr;
  DarwinPthread Identity = pthreadIdentity(Record);
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
  CurrentThread = Record;

  int CoreResult = 0;
  void *ExitValue = invokeStartRoutine(Record->StartRoutine,
                                       Record->StartArgument, CoreResult);
  if (CoreResult != 0)
    ExitValue = reinterpret_cast<void *>(static_cast<std::uintptr_t>(CoreResult));
  finishRecord(*Record, ExitValue);
  CurrentThread = nullptr;
  return 0;
}

unsigned logicalProcessorCount() {
  DWORD Count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
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

} // namespace

extern "C" {

__declspec(dllexport) int pthread_attr_init(DarwinPthreadAttr *Attr) {
  if (!Attr)
    return EINVAL;
  *Attr = defaultAttr();
  return 0;
}

__declspec(dllexport) int pthread_attr_destroy(DarwinPthreadAttr *Attr) {
  if (!validAttr(Attr))
    return EINVAL;
  Attr->Sig = 0;
  return 0;
}

__declspec(dllexport) int
pthread_attr_getschedparam(const DarwinPthreadAttr *Attr, DarwinSchedParam *Param) {
  if (!validAttr(Attr) || !Param)
    return EINVAL;
  if (Attr->Flags & SchedSetBit)
    *Param = Attr->Scheduling.Param;
  else
    *Param = DarwinSchedParam{DefaultSchedPriority, DefaultSchedQuantum};
  return 0;
}

__declspec(dllexport) int
pthread_attr_getschedpolicy(const DarwinPthreadAttr *Attr, int *Policy) {
  if (!validAttr(Attr) || !Policy)
    return EINVAL;
  *Policy = attrPolicy(*Attr);
  return 0;
}

__declspec(dllexport) int
pthread_attr_setdetachstate(DarwinPthreadAttr *Attr, int Detached) {
  if (!validAttr(Attr) ||
      (Detached != PthreadCreateJoinable && Detached != PthreadCreateDetached))
    return EINVAL;
  setAttrDetached(*Attr, static_cast<std::uint8_t>(Detached));
  return 0;
}

__declspec(dllexport) int
pthread_attr_setschedparam(DarwinPthreadAttr *Attr,
                           const DarwinSchedParam *Param) {
  if (!validAttr(Attr) || !Param)
    return EINVAL;
  Attr->Scheduling.Param = *Param;
  Attr->Flags |= SchedSetBit;
  Attr->Flags &= ~QosSetBit;
  return 0;
}

__declspec(dllexport) int
pthread_attr_setschedpolicy(DarwinPthreadAttr *Attr, int Policy) {
  if (!validAttr(Attr) ||
      (Policy != SchedOther && Policy != SchedRR && Policy != SchedFIFO))
    return EINVAL;
  if (!fixedPolicy(Policy))
    Attr->Flags &= ~CpuPercentSetBit;
  setAttrPolicy(*Attr, static_cast<std::uint8_t>(Policy));
  Attr->Flags |= PolicySetBit;
  return 0;
}

__declspec(dllexport) int
pthread_attr_setcpupercent_np(DarwinPthreadAttr *Attr, int Percent,
                              DarwinUnsignedLong RefillMilliseconds) {
  if (!validAttr(Attr) || Percent < 0 || Percent >= 255 ||
      RefillMilliseconds >= MaxRefillMilliseconds ||
      !(Attr->Flags & PolicySetBit) || !fixedPolicy(attrPolicy(*Attr)))
    return EINVAL;
  Attr->CpuPercentAndRefill =
      static_cast<std::uint32_t>(Percent) |
      (static_cast<std::uint32_t>(RefillMilliseconds & 0x00ffffffULL) << 8);
  Attr->Flags |= CpuPercentSetBit;
  return 0;
}

__declspec(dllexport) int
pthread_attr_get_qos_class_np(const DarwinPthreadAttr *Attr,
                              DarwinQosClass *Qos, int *RelativePriority) {
  if (!validAttr(Attr))
    return EINVAL;
  const std::uint64_t Priority = Attr->Scheduling.QosClass;
  if (Qos)
    *Qos = (Attr->Flags & QosSetBit) ? qosClassFromCompact(Priority)
                                     : QosClassUnspecified;
  if (RelativePriority)
    *RelativePriority = (Attr->Flags & QosSetBit)
                            ? relativePriorityFromCompact(Priority)
                            : 0;
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
    return EINVAL;
  auto Record = Thread ? lookupRecord(Thread) : currentRecordShared();
  if (!Record)
    return ESRCH;
  *ThreadId = Record->ThreadId;
  return 0;
}

__declspec(dllexport) int
pthread_get_qos_class_np(DarwinPthread Thread, DarwinQosClass *Qos,
                         int *RelativePriority) {
  auto Record = Thread ? lookupRecord(Thread) : currentRecordShared();
  if (!Record)
    return ESRCH;
  if (Qos)
    *Qos = Record->QosClass;
  if (RelativePriority)
    *RelativePriority = Record->RelativePriority;
  return 0;
}

__declspec(dllexport) void *pthread_get_stackaddr_np(DarwinPthread Thread) {
  auto Record = Thread ? lookupRecord(Thread) : currentRecordShared();
  if (!Record)
    return nullptr;

  // The guest CPU stack is allocated by the ipaSim core. Until its exact bounds
  // are exported, return a stable page-aligned process address associated with
  // this pthread rather than a Windows native stack pointer. This is suitable
  // for libdispatch's identity/introspection use but is deliberately not used
  // for guest memory mapping or overflow checks.
  const std::uintptr_t Base = reinterpret_cast<std::uintptr_t>(&Record->Guest);
  return reinterpret_cast<void *>((Base + 0xfffULL) & ~0xfffULL);
}

__declspec(dllexport) int pthread_setname_np(const char *Name) {
  if (!Name)
    return EINVAL;
  auto Record = currentRecordShared();
  const std::size_t Length = std::strlen(Name);
  if (Length >= Record->Name.size())
    return ERANGE;

  std::memset(Record->Name.data(), 0, Record->Name.size());
  std::memcpy(Record->Name.data(), Name, Length);

  int WideLength = MultiByteToWideChar(CP_UTF8, 0, Name, -1, nullptr, 0);
  if (WideLength > 0) {
    std::wstring Wide(static_cast<std::size_t>(WideLength), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, Name, -1, Wide.data(), WideLength) > 0) {
      HANDLE Handle = Record->Handle ? Record->Handle : GetCurrentThread();
      SetThreadDescription(Handle, Wide.c_str());
    }
  }
  return 0;
}

__declspec(dllexport) int pthread_sigmask(int How, const DarwinSigset *Set,
                                          DarwinSigset *OldSet) {
  if (OldSet)
    *OldSet = CurrentSignalMask;
  if (!Set)
    return 0;

  switch (How) {
  case DarwinSigBlock:
    CurrentSignalMask |= *Set;
    return 0;
  case DarwinSigUnblock:
    CurrentSignalMask &= ~*Set;
    return 0;
  case DarwinSigSetMask:
    CurrentSignalMask = *Set;
    return 0;
  default:
    return EINVAL;
  }
}

__declspec(dllexport) int pthread_create(DarwinPthread *Thread,
                                         const DarwinPthreadAttr *Attr,
                                         void *StartRoutine, void *Argument) {
  if (!Thread || !StartRoutine)
    return EINVAL;
  DarwinPthreadAttr Effective = Attr ? *Attr : defaultAttr();
  if (!validAttr(&Effective))
    return EINVAL;

  auto Record = std::make_shared<ThreadRecord>();
  Record->ThreadId = NextThreadId.fetch_add(1, std::memory_order_relaxed);
  Record->Detached = attrDetached(Effective) == PthreadCreateDetached;
  Record->OwnedThread = true;
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
    return EAGAIN;
  }

  Record->Handle = Handle;
  Record->WindowsThreadId = WindowsThreadId;
  *Thread = pthreadIdentity(*Record);

  if (ResumeThread(Handle) == static_cast<DWORD>(-1)) {
    eraseRecord(*Thread);
    CloseHandle(Handle);
    *Thread = nullptr;
    return EAGAIN;
  }
  return 0;
}

__declspec(dllexport) int pthread_detach(DarwinPthread Thread) {
  auto Record = lookupRecord(Thread);
  if (!Record || Record->IsMain)
    return ESRCH;

  bool Cleanup = false;
  HANDLE Handle = nullptr;
  {
    std::lock_guard<std::mutex> Guard(Record->Mutex);
    if (Record->Detached)
      return EINVAL;
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
    return ESRCH;
  if (CurrentThread == Record.get())
    return EDEADLK;

  HANDLE Handle = nullptr;
  {
    std::lock_guard<std::mutex> Guard(Record->Mutex);
    if (Record->Detached)
      return EINVAL;
    Handle = Record->Handle;
  }
  if (!Handle)
    return ESRCH;

  const DWORD Wait = WaitForSingleObject(Handle, INFINITE);
  if (Wait != WAIT_OBJECT_0)
    return EINVAL;

  {
    std::lock_guard<std::mutex> Guard(Record->Mutex);
    if (ExitValue)
      *ExitValue = Record->ExitValue;
    Record->Detached = true;
    Record->Handle = nullptr;
  }
  eraseRecord(Thread);
  CloseHandle(Handle);
  return 0;
}

__declspec(dllexport) __declspec(noreturn) void pthread_exit(void *ExitValue) {
  auto Record = currentRecordShared();
  finishRecord(*Record, ExitValue);
  CurrentThread = nullptr;
  // pthread_exit is semantically non-returning. Ending the backing Windows
  // thread preserves that contract even when called from deep inside a guest ->
  // native bridge continuation. The current Unicorn context is abandoned on
  // this explicit-exit path; normal start-routine return performs full RAII
  // teardown through ipaSim_runGuestPthread().
  ExitThread(0);
  __assume(0);
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
    return EINVAL;
  PROCESSOR_NUMBER Number{};
  GetCurrentProcessorNumberEx(&Number);
  *CpuNumber = static_cast<std::size_t>(Number.Number);
  return 0;
}

__declspec(dllexport) int
pthread_install_workgroup_functions_np(void *Functions) {
  if (!Functions)
    return EINVAL;
  std::lock_guard<std::mutex> Guard(WorkgroupMutex);
  if (InstalledWorkgroupFunctions && InstalledWorkgroupFunctions != Functions)
    return EBUSY;
  InstalledWorkgroupFunctions = Functions;
  return 0;
}

} // extern "C"
