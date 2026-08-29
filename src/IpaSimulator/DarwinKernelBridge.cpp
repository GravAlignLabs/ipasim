// DarwinKernelBridge.cpp: process, monotonic-time, and VM semantics used by
// modern iOS Simulator libsystem layers on the Windows host.
//
// This bridge intentionally implements only flavors with defensible Windows
// equivalents. Unsupported proc_pidinfo flavors fail explicitly with ENOTSUP;
// they are never represented by zero-filled success records.

#include "DarwinCredentialAdapter.hpp"
#include "DarwinSocketAdapter.hpp"
#include "FifoAdapter.hpp"
#include "MachIpc.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <io.h>
#include <limits>
#include <mutex>
#include <process.h>
#include <psapi.h>
#include <string>
#include <tlhelp32.h>
#include <unordered_map>
#include <vector>
#include <windows.h>

extern "C" int proc_pidpath(int Pid, void *Buffer, std::uint32_t BufferSize);

namespace {

constexpr int DarwinProcPidListFds = 1;
constexpr int DarwinProcPidTaskAllInfo = 2;
constexpr int DarwinProcPidBsdInfo = 3;
constexpr int DarwinProcPidTaskInfo = 4;
constexpr int DarwinProcPidVnodePathInfo = 9;
constexpr int DarwinProcPidPathInfo = 11;
constexpr int DarwinProcPidShortBsdInfo = 13;

constexpr std::uint32_t DarwinProcFlagLp64 = 0x10;
constexpr std::uint32_t DarwinProcessStateRunning = 2; // XNU SRUN.
constexpr std::size_t DarwinMaxComLen = 16;
constexpr std::uint32_t DarwinFdTypeVnode = 1;
constexpr std::uint32_t DarwinFdTypeSocket = 2;
constexpr std::uint64_t WindowsToUnixEpoch100ns = 116444736000000000ULL;
constexpr std::uint32_t ContinuousTimeNumer = 100;
constexpr std::uint32_t ContinuousTimeDenom = 1;

struct DarwinMachTimebaseInfo {
  std::uint32_t Numer;
  std::uint32_t Denom;
};
static_assert(sizeof(DarwinMachTimebaseInfo) == 8,
              "Darwin mach_timebase_info_data_t layout changed unexpectedly");

struct DarwinProcBsdInfo {
  std::uint32_t Flags;
  std::uint32_t Status;
  std::uint32_t ExitStatus;
  std::uint32_t Pid;
  std::uint32_t ParentPid;
  std::uint32_t Uid;
  std::uint32_t Gid;
  std::uint32_t RealUid;
  std::uint32_t RealGid;
  std::uint32_t SavedUid;
  std::uint32_t SavedGid;
  std::uint32_t Reserved;
  char Command[DarwinMaxComLen];
  char Name[2 * DarwinMaxComLen];
  std::uint32_t OpenFileCount;
  std::uint32_t ProcessGroupId;
  std::uint32_t JobControlCount;
  std::uint32_t TerminalDevice;
  std::uint32_t TerminalProcessGroupId;
  std::int32_t Nice;
  std::uint64_t StartSeconds;
  std::uint64_t StartMicroseconds;
};
static_assert(sizeof(DarwinProcBsdInfo) == 136,
              "Darwin proc_bsdinfo ABI layout changed unexpectedly");

struct DarwinProcShortBsdInfo {
  std::uint32_t Pid;
  std::uint32_t ParentPid;
  std::uint32_t ProcessGroupId;
  std::uint32_t Status;
  char Command[DarwinMaxComLen];
  std::uint32_t Flags;
  std::uint32_t Uid;
  std::uint32_t Gid;
  std::uint32_t RealUid;
  std::uint32_t RealGid;
  std::uint32_t SavedUid;
  std::uint32_t SavedGid;
  std::uint32_t Reserved;
};
static_assert(sizeof(DarwinProcShortBsdInfo) == 64,
              "Darwin proc_bsdshortinfo ABI layout changed unexpectedly");

struct DarwinProcTaskInfo {
  std::uint64_t VirtualSize;
  std::uint64_t ResidentSize;
  std::uint64_t TotalUser;
  std::uint64_t TotalSystem;
  std::uint64_t ThreadsUser;
  std::uint64_t ThreadsSystem;
  std::int32_t Policy;
  std::int32_t Faults;
  std::int32_t PageIns;
  std::int32_t CowFaults;
  std::int32_t MessagesSent;
  std::int32_t MessagesReceived;
  std::int32_t MachSyscalls;
  std::int32_t UnixSyscalls;
  std::int32_t ContextSwitches;
  std::int32_t ThreadCount;
  std::int32_t RunningThreadCount;
  std::int32_t Priority;
};
static_assert(sizeof(DarwinProcTaskInfo) == 96,
              "Darwin proc_taskinfo ABI layout changed unexpectedly");

struct DarwinProcTaskAllInfo {
  DarwinProcBsdInfo Bsd;
  DarwinProcTaskInfo Task;
};
static_assert(sizeof(DarwinProcTaskAllInfo) == 232,
              "Darwin proc_taskallinfo ABI layout changed unexpectedly");

struct DarwinProcFdInfo {
  std::int32_t Descriptor;
  std::uint32_t Type;
};
static_assert(sizeof(DarwinProcFdInfo) == 8,
              "Darwin proc_fdinfo ABI layout changed unexpectedly");

std::mutex VmRegistryMutex;
std::unordered_map<std::uint64_t, std::uint64_t> VmReservations;

void setErrnoFromProcessError(DWORD Error) {
  switch (Error) {
  case ERROR_INVALID_PARAMETER:
  case ERROR_NOT_FOUND:
    errno = ESRCH;
    break;
  case ERROR_ACCESS_DENIED:
    errno = EPERM;
    break;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:
    errno = ENOMEM;
    break;
  default:
    errno = EIO;
    break;
  }
}

bool openProcessForQuery(int Pid, HANDLE &Process, bool &MustClose) {
  MustClose = false;
  if (Pid <= 0) {
    errno = ESRCH;
    return false;
  }
  if (Pid == _getpid()) {
    Process = GetCurrentProcess();
    return true;
  }
  Process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                        static_cast<DWORD>(Pid));
  if (!Process) {
    Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                          static_cast<DWORD>(Pid));
  }
  if (!Process) {
    setErrnoFromProcessError(GetLastError());
    return false;
  }
  MustClose = true;
  return true;
}

std::uint64_t fileTimeTicks(const FILETIME &Value) {
  ULARGE_INTEGER Ticks{};
  Ticks.LowPart = Value.dwLowDateTime;
  Ticks.HighPart = Value.dwHighDateTime;
  return Ticks.QuadPart;
}

bool queryProcessSnapshot(DWORD Pid, DWORD &ParentPid, DWORD &ThreadCount,
                          std::wstring &ExecutableName) {
  HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (Snapshot == INVALID_HANDLE_VALUE) {
    setErrnoFromProcessError(GetLastError());
    return false;
  }

  PROCESSENTRY32W Entry{};
  Entry.dwSize = sizeof(Entry);
  bool Found = false;
  if (Process32FirstW(Snapshot, &Entry)) {
    do {
      if (Entry.th32ProcessID == Pid) {
        ParentPid = Entry.th32ParentProcessID;
        ThreadCount = Entry.cntThreads;
        ExecutableName = Entry.szExeFile;
        Found = true;
        break;
      }
    } while (Process32NextW(Snapshot, &Entry));
  }
  CloseHandle(Snapshot);
  if (!Found)
    errno = ESRCH;
  return Found;
}

std::string utf8FromWide(const std::wstring &Wide) {
  if (Wide.empty())
    return {};
  const int Required = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, Wide.data(), static_cast<int>(Wide.size()),
      nullptr, 0, nullptr, nullptr);
  if (Required <= 0)
    return {};
  std::string Result(static_cast<std::size_t>(Required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Wide.data(),
                          static_cast<int>(Wide.size()), Result.data(), Required,
                          nullptr, nullptr) != Required)
    return {};
  return Result;
}

void copyFixedString(char *Destination, std::size_t Capacity,
                     const std::string &Source) {
  if (Capacity == 0)
    return;
  const std::size_t Count = (std::min)(Capacity - 1, Source.size());
  if (Count)
    std::memcpy(Destination, Source.data(), Count);
  Destination[Count] = '\0';
}

bool queryTokenIds(HANDLE Process, std::uint32_t &Uid, std::uint32_t &Gid) {
  HANDLE Token = nullptr;
  if (!OpenProcessToken(Process, TOKEN_QUERY, &Token)) {
    setErrnoFromProcessError(GetLastError());
    return false;
  }

  DWORD UserBytes = 0;
  GetTokenInformation(Token, TokenUser, nullptr, 0, &UserBytes);
  if (UserBytes == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    CloseHandle(Token);
    errno = EIO;
    return false;
  }
  std::vector<std::uint8_t> UserStorage(UserBytes);
  if (!GetTokenInformation(Token, TokenUser, UserStorage.data(), UserBytes,
                           &UserBytes)) {
    CloseHandle(Token);
    errno = EIO;
    return false;
  }

  DWORD GroupBytes = 0;
  GetTokenInformation(Token, TokenPrimaryGroup, nullptr, 0, &GroupBytes);
  if (GroupBytes == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    CloseHandle(Token);
    errno = EIO;
    return false;
  }
  std::vector<std::uint8_t> GroupStorage(GroupBytes);
  if (!GetTokenInformation(Token, TokenPrimaryGroup, GroupStorage.data(),
                           GroupBytes, &GroupBytes)) {
    CloseHandle(Token);
    errno = EIO;
    return false;
  }
  CloseHandle(Token);

  const auto *User = reinterpret_cast<const TOKEN_USER *>(UserStorage.data());
  const auto *Group =
      reinterpret_cast<const TOKEN_PRIMARY_GROUP *>(GroupStorage.data());
  if (!ipasim::darwincred::sidTerminalRid(User->User.Sid, Uid) ||
      !ipasim::darwincred::sidTerminalRid(Group->PrimaryGroup, Gid)) {
    errno = EIO;
    return false;
  }
  return true;
}

std::uint32_t guestDescriptorCount() {
  const std::size_t Files = ipasim::darwinfs::listOpenNodeDescriptors().size();
  const std::size_t Sockets = ipasim::darwinsock::listDescriptors().size();
  return static_cast<std::uint32_t>((std::min)(
      Files + Sockets,
      static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
}

bool fillBsdInfo(int Pid, DarwinProcBsdInfo &Info) {
  HANDLE Process = nullptr;
  bool MustClose = false;
  if (!openProcessForQuery(Pid, Process, MustClose))
    return false;

  DWORD ParentPid = 0;
  DWORD ThreadCount = 0;
  std::wstring ExecutableName;
  if (!queryProcessSnapshot(static_cast<DWORD>(Pid), ParentPid, ThreadCount,
                            ExecutableName)) {
    if (MustClose)
      CloseHandle(Process);
    return false;
  }

  std::uint32_t Uid = 0;
  std::uint32_t Gid = 0;
  if (!queryTokenIds(Process, Uid, Gid)) {
    if (MustClose)
      CloseHandle(Process);
    return false;
  }

  FILETIME Created{}, Exited{}, Kernel{}, User{};
  if (!GetProcessTimes(Process, &Created, &Exited, &Kernel, &User)) {
    const DWORD Error = GetLastError();
    if (MustClose)
      CloseHandle(Process);
    setErrnoFromProcessError(Error);
    return false;
  }
  if (MustClose)
    CloseHandle(Process);

  std::memset(&Info, 0, sizeof(Info));
  Info.Flags = DarwinProcFlagLp64;
  Info.Status = DarwinProcessStateRunning;
  Info.Pid = static_cast<std::uint32_t>(Pid);
  Info.ParentPid = ParentPid;
  Info.Uid = Uid;
  Info.Gid = Gid;
  Info.RealUid = Uid;
  Info.RealGid = Gid;
  Info.SavedUid = Uid;
  Info.SavedGid = Gid;
  const std::string Name = utf8FromWide(ExecutableName);
  copyFixedString(Info.Command, sizeof(Info.Command), Name);
  copyFixedString(Info.Name, sizeof(Info.Name), Name);
  if (Pid == _getpid())
    Info.OpenFileCount = guestDescriptorCount();

  // Windows exposes job/process hierarchy but no public process-group identity
  // equivalent to Darwin pbi_pgid. Zero is the honest unavailable value; using
  // PID here would manufacture a group relationship that may not exist.
  Info.ProcessGroupId = 0;

  const std::uint64_t CreateTicks = fileTimeTicks(Created);
  if (CreateTicks >= WindowsToUnixEpoch100ns) {
    const std::uint64_t UnixTicks = CreateTicks - WindowsToUnixEpoch100ns;
    Info.StartSeconds = UnixTicks / 10000000ULL;
    Info.StartMicroseconds = (UnixTicks % 10000000ULL) / 10ULL;
  }
  return true;
}

std::uint64_t queryVirtualSize(HANDLE Process) {
  SYSTEM_INFO System{};
  GetSystemInfo(&System);
  std::uintptr_t Address =
      reinterpret_cast<std::uintptr_t>(System.lpMinimumApplicationAddress);
  const std::uintptr_t Maximum =
      reinterpret_cast<std::uintptr_t>(System.lpMaximumApplicationAddress);
  std::uint64_t Total = 0;
  MEMORY_BASIC_INFORMATION Information{};
  while (Address < Maximum) {
    const SIZE_T Result = VirtualQueryEx(Process,
                                         reinterpret_cast<const void *>(Address),
                                         &Information, sizeof(Information));
    if (Result == 0)
      break;
    if (Information.State != MEM_FREE) {
      const std::uint64_t Region =
          static_cast<std::uint64_t>(Information.RegionSize);
      if (Total <= (std::numeric_limits<std::uint64_t>::max)() - Region)
        Total += Region;
    }
    const std::uintptr_t Next =
        reinterpret_cast<std::uintptr_t>(Information.BaseAddress) +
        Information.RegionSize;
    if (Next <= Address)
      break;
    Address = Next;
  }
  return Total;
}

bool fillTaskInfo(int Pid, DarwinProcTaskInfo &Info) {
  HANDLE Process = nullptr;
  bool MustClose = false;
  if (!openProcessForQuery(Pid, Process, MustClose))
    return false;

  PROCESS_MEMORY_COUNTERS_EX Memory{};
  Memory.cb = sizeof(Memory);
  if (!GetProcessMemoryInfo(
          Process, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&Memory),
          sizeof(Memory))) {
    const DWORD Error = GetLastError();
    if (MustClose)
      CloseHandle(Process);
    setErrnoFromProcessError(Error);
    return false;
  }

  FILETIME Created{}, Exited{}, Kernel{}, User{};
  if (!GetProcessTimes(Process, &Created, &Exited, &Kernel, &User)) {
    const DWORD Error = GetLastError();
    if (MustClose)
      CloseHandle(Process);
    setErrnoFromProcessError(Error);
    return false;
  }

  DWORD ParentPid = 0;
  DWORD ThreadCount = 0;
  std::wstring ExecutableName;
  if (!queryProcessSnapshot(static_cast<DWORD>(Pid), ParentPid, ThreadCount,
                            ExecutableName)) {
    if (MustClose)
      CloseHandle(Process);
    return false;
  }

  const std::uint64_t VirtualSize = queryVirtualSize(Process);
  if (MustClose)
    CloseHandle(Process);

  std::memset(&Info, 0, sizeof(Info));
  Info.VirtualSize = VirtualSize;
  Info.ResidentSize = static_cast<std::uint64_t>(Memory.WorkingSetSize);
  // Windows process user/kernel times are 100ns units. Darwin proc_taskinfo
  // exposes nanoseconds for these accumulated time counters.
  Info.TotalUser = fileTimeTicks(User) * 100ULL;
  Info.TotalSystem = fileTimeTicks(Kernel) * 100ULL;
  Info.Faults = static_cast<std::int32_t>((std::min)(
      static_cast<std::uint64_t>(Memory.PageFaultCount),
      static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())));
  Info.ThreadCount = static_cast<std::int32_t>((std::min)(
      static_cast<std::uint64_t>(ThreadCount),
      static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())));
  // Windows has no stable public API for Darwin's instantaneous running-thread
  // count, Mach/Unix syscall split, COW faults, or Mach scheduling policy. Those
  // fields remain zero rather than being fabricated from unrelated counters.
  return true;
}

std::vector<DarwinProcFdInfo> guestDescriptorSnapshot() {
  std::vector<DarwinProcFdInfo> Entries;
  const std::vector<int> Files = ipasim::darwinfs::listOpenNodeDescriptors();
  const std::vector<int> Sockets = ipasim::darwinsock::listDescriptors();
  Entries.reserve(Files.size() + Sockets.size());
  for (const int Descriptor : Files)
    Entries.push_back({Descriptor, DarwinFdTypeVnode});
  for (const int Descriptor : Sockets)
    Entries.push_back({Descriptor, DarwinFdTypeSocket});
  std::sort(Entries.begin(), Entries.end(),
            [](const DarwinProcFdInfo &Left, const DarwinProcFdInfo &Right) {
              return Left.Descriptor < Right.Descriptor;
            });
  return Entries;
}

int listCurrentDescriptors(void *Buffer, int BufferSize) {
  if (BufferSize < 0 || (!Buffer && BufferSize != 0)) {
    errno = EFAULT;
    return 0;
  }

  const std::vector<DarwinProcFdInfo> Entries = guestDescriptorSnapshot();
  const std::size_t RequiredBytes = Entries.size() * sizeof(DarwinProcFdInfo);

  // libproc callers use a null/zero first call to size PROC_PIDLISTFDS. Return
  // the required guest-visible byte count without touching memory.
  if (!Buffer && BufferSize == 0) {
    if (RequiredBytes > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
      errno = EOVERFLOW;
      return 0;
    }
    return static_cast<int>(RequiredBytes);
  }

  const std::size_t Capacity =
      static_cast<std::size_t>(BufferSize) / sizeof(DarwinProcFdInfo);
  const std::size_t CopyCount = (std::min)(Capacity, Entries.size());
  if (CopyCount)
    std::memcpy(Buffer, Entries.data(), CopyCount * sizeof(DarwinProcFdInfo));
  return static_cast<int>(CopyCount * sizeof(DarwinProcFdInfo));
}

bool validateOutput(void *Buffer, int BufferSize, std::size_t Required) {
  if (!Buffer) {
    errno = EFAULT;
    return false;
  }
  if (BufferSize < 0 || static_cast<std::size_t>(BufferSize) < Required) {
    errno = ENOMEM;
    return false;
  }
  return true;
}

} // namespace

extern "C" {

// Darwin's mach_continuous_time is a monotonic boot-relative clock that keeps
// advancing while the machine sleeps. QueryInterruptTimePrecise provides the
// matching Windows property in 100ns ticks. Keep mach_timebase_info coherent
// with those ticks: each returned tick represents exactly 100 nanoseconds.
__declspec(dllexport) std::uint64_t mach_continuous_time(void) {
  ULONGLONG Ticks = 0;
  QueryInterruptTimePrecise(&Ticks);
  return static_cast<std::uint64_t>(Ticks);
}

__declspec(dllexport) std::int32_t
mach_timebase_info(DarwinMachTimebaseInfo *Info) {
  if (!Info)
    return ipasim::mach::KernelInvalidArgument;
  Info->Numer = ContinuousTimeNumer;
  Info->Denom = ContinuousTimeDenom;
  return ipasim::mach::KernelSuccess;
}

// vm_page_size is exported as data by libsystem_kernel. Keep it aligned with the
// actual host virtual-memory page size used by VirtualAlloc/VirtualQueryEx.
__declspec(dllexport) std::uint32_t vm_page_size = []() {
  SYSTEM_INFO Information{};
  GetSystemInfo(&Information);
  return static_cast<std::uint32_t>(Information.dwPageSize);
}();

__declspec(dllexport) std::int32_t vm_allocate(std::uint32_t Task,
                                               std::uint64_t *Address,
                                               std::uint64_t Size,
                                               std::int32_t Flags) {
  if (Task != ipasim::mach::taskSelfPort() || !Address || Size == 0 ||
      Size > static_cast<std::uint64_t>((std::numeric_limits<SIZE_T>::max)()))
    return ipasim::mach::KernelInvalidArgument;

  constexpr std::int32_t VmFlagsAnywhere = 1;
  void *Requested = (Flags & VmFlagsAnywhere)
                        ? nullptr
                        : reinterpret_cast<void *>(
                              static_cast<std::uintptr_t>(*Address));
  void *Allocated = VirtualAlloc(Requested, static_cast<SIZE_T>(Size),
                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!Allocated)
    return ipasim::mach::KernelFailure;
  if (!(Flags & VmFlagsAnywhere) && Allocated != Requested) {
    VirtualFree(Allocated, 0, MEM_RELEASE);
    return ipasim::mach::KernelFailure;
  }

  const std::uint64_t Result = reinterpret_cast<std::uint64_t>(Allocated);
  {
    std::lock_guard<std::mutex> Guard(VmRegistryMutex);
    VmReservations[Result] = Size;
  }
  *Address = Result;
  return ipasim::mach::KernelSuccess;
}

__declspec(dllexport) std::int32_t vm_deallocate(std::uint32_t Task,
                                                 std::uint64_t Address,
                                                 std::uint64_t Size) {
  if (Task != ipasim::mach::taskSelfPort() || Address == 0 || Size == 0)
    return ipasim::mach::KernelInvalidArgument;

  // MEM_RELEASE can only release a complete VirtualAlloc reservation. Track the
  // guest allocation explicitly and fail partial/mismatched deallocation rather
  // than silently releasing more memory than Darwin requested.
  {
    std::lock_guard<std::mutex> Guard(VmRegistryMutex);
    const auto It = VmReservations.find(Address);
    if (It == VmReservations.end() || It->second != Size)
      return ipasim::mach::KernelInvalidArgument;
  }

  if (!VirtualFree(reinterpret_cast<void *>(
                       static_cast<std::uintptr_t>(Address)),
                   0, MEM_RELEASE))
    return ipasim::mach::KernelFailure;

  {
    std::lock_guard<std::mutex> Guard(VmRegistryMutex);
    VmReservations.erase(Address);
  }
  return ipasim::mach::KernelSuccess;
}

// Source identifier differs because UCRT already owns a source-level write
// declaration. DarwinHostBridge.def aliases this to the PE export `write`.
std::intptr_t darwin_write(int Fd, const void *Buffer, std::size_t Count) {
  if (!Buffer && Count != 0) {
    errno = EFAULT;
    return -1;
  }
  if (Count == 0)
    return 0;

  if (ipasim::darwinsock::isSocketDescriptor(Fd))
    return ipasim::darwinsock::sendTo(Fd, Buffer, Count, 0, nullptr, 0);

  if (!ipasim::darwinfs::isOpenNodeDescriptor(Fd)) {
    errno = EBADF;
    return -1;
  }

  const unsigned int HostCount = static_cast<unsigned int>((std::min)(
      Count, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
  return static_cast<std::intptr_t>(_write(Fd, Buffer, HostCount));
}

// proc_pidinfo returns the number of bytes written, or 0 with errno set. The
// implemented flavors expose real Windows process identity, timing, memory,
// and guest descriptor data. Known flavors without a faithful mapping fail
// ENOTSUP so callers cannot mistake invented records for Darwin data.
__declspec(dllexport) int proc_pidinfo(int Pid, int Flavor, std::uint64_t Arg,
                                       void *Buffer, int BufferSize) {
  (void)Arg;
  switch (Flavor) {
  case DarwinProcPidBsdInfo: {
    if (!validateOutput(Buffer, BufferSize, sizeof(DarwinProcBsdInfo)))
      return 0;
    DarwinProcBsdInfo Info{};
    if (!fillBsdInfo(Pid, Info))
      return 0;
    std::memcpy(Buffer, &Info, sizeof(Info));
    return sizeof(Info);
  }
  case DarwinProcPidShortBsdInfo: {
    if (!validateOutput(Buffer, BufferSize, sizeof(DarwinProcShortBsdInfo)))
      return 0;
    DarwinProcBsdInfo Full{};
    if (!fillBsdInfo(Pid, Full))
      return 0;
    DarwinProcShortBsdInfo Short{};
    Short.Pid = Full.Pid;
    Short.ParentPid = Full.ParentPid;
    Short.ProcessGroupId = Full.ProcessGroupId;
    Short.Status = Full.Status;
    std::memcpy(Short.Command, Full.Command, sizeof(Short.Command));
    Short.Flags = Full.Flags;
    Short.Uid = Full.Uid;
    Short.Gid = Full.Gid;
    Short.RealUid = Full.RealUid;
    Short.RealGid = Full.RealGid;
    Short.SavedUid = Full.SavedUid;
    Short.SavedGid = Full.SavedGid;
    std::memcpy(Buffer, &Short, sizeof(Short));
    return sizeof(Short);
  }
  case DarwinProcPidTaskInfo: {
    if (!validateOutput(Buffer, BufferSize, sizeof(DarwinProcTaskInfo)))
      return 0;
    DarwinProcTaskInfo Info{};
    if (!fillTaskInfo(Pid, Info))
      return 0;
    std::memcpy(Buffer, &Info, sizeof(Info));
    return sizeof(Info);
  }
  case DarwinProcPidTaskAllInfo: {
    if (!validateOutput(Buffer, BufferSize, sizeof(DarwinProcTaskAllInfo)))
      return 0;
    DarwinProcTaskAllInfo Info{};
    if (!fillBsdInfo(Pid, Info.Bsd) || !fillTaskInfo(Pid, Info.Task))
      return 0;
    std::memcpy(Buffer, &Info, sizeof(Info));
    return sizeof(Info);
  }
  case DarwinProcPidListFds:
    if (Pid != _getpid()) {
      errno = ENOTSUP;
      return 0;
    }
    return listCurrentDescriptors(Buffer, BufferSize);
  case DarwinProcPidPathInfo:
    return proc_pidpath(Pid, Buffer, static_cast<std::uint32_t>(BufferSize));
  case DarwinProcPidVnodePathInfo:
    errno = ENOTSUP;
    return 0;
  default:
    errno = EINVAL;
    return 0;
  }
}

} // extern "C"
