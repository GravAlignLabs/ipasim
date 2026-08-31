// DarwinKernelSmoke.cpp: semantic and export checks for process/time/VM host
// boundaries exercised by modern iOS simulator runtime compatibility.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <process.h>
#include <windows.h>

namespace {

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
  char Command[16];
  char Name[32];
  std::uint32_t OpenFileCount;
  std::uint32_t ProcessGroupId;
  std::uint32_t JobControlCount;
  std::uint32_t TerminalDevice;
  std::uint32_t TerminalProcessGroupId;
  std::int32_t Nice;
  std::uint64_t StartSeconds;
  std::uint64_t StartMicroseconds;
};
static_assert(sizeof(DarwinProcBsdInfo) == 136);

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
static_assert(sizeof(DarwinProcTaskInfo) == 96);

struct DarwinProcFdInfo {
  std::int32_t Descriptor;
  std::uint32_t Type;
};
static_assert(sizeof(DarwinProcFdInfo) == 8);

struct DarwinNdrRecord {
  std::uint8_t MigVersion;
  std::uint8_t InterfaceVersion;
  std::uint8_t Reserved1;
  std::uint8_t MigEncoding;
  std::uint8_t IntegerRepresentation;
  std::uint8_t CharacterRepresentation;
  std::uint8_t FloatRepresentation;
  std::uint8_t Reserved2;
};
static_assert(sizeof(DarwinNdrRecord) == 8);

int fail(const char *Message) {
  std::fprintf(stderr, "[darwin-kernel-smoke] FAIL: %s\n", Message);
  return 1;
}

FARPROC requireExport(HMODULE Module, const char *Name) {
  FARPROC Proc = GetProcAddress(Module, Name);
  if (!Proc)
    std::fprintf(stderr, "[darwin-kernel-smoke] missing export: %s\n", Name);
  return Proc;
}

bool descriptorListContains(const DarwinProcFdInfo *Fds, int Bytes,
                            int Descriptor) {
  if (Bytes <= 0 ||
      (Bytes % static_cast<int>(sizeof(DarwinProcFdInfo))) != 0)
    return false;
  const int Count = Bytes / static_cast<int>(sizeof(DarwinProcFdInfo));
  for (int Index = 0; Index < Count; ++Index) {
    if (Fds[Index].Descriptor == Descriptor)
      return true;
  }
  return false;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return fail("expected IpaSimDarwinHost.dll path");

  HMODULE Host = LoadLibraryA(argv[1]);
  if (!Host)
    return fail("could not load IpaSimDarwinHost.dll");

  const char *Required[] = {
      "__error",
      "mach_continuous_time",
      "proc_pidinfo",
      "vm_allocate",
      "vm_deallocate",
      "vm_page_size",
      "NDR_record",
      "open",
      "open$NOCANCEL",
      "close",
      "close$NOCANCEL",
      "fcntl",
      "fcntl$NOCANCEL",
      "lseek",
      "pread",
      "pread$NOCANCEL",
      "pwrite",
      "pwrite$NOCANCEL",
      "read",
      "read$NOCANCEL",
      "write",
      "write$NOCANCEL",
      "sigsuspend$NOCANCEL",
      "socket",
      "__interposition_sim_system_csops",
      "__interposition_sim_system_csops_audittoken",
      "__interposition_sim_system_freadlink",
      "__interposition_sim_system_mach_msg2_internal",
      "__interposition_sim_system_record_system_event_as_kernel",
      "__interposition_sim_system__platform_memcmp_zero_aligned8",
      "__interposition_sim_system_pthread_create_with_workgroup_np",
      "__interposition_sim_system_pthread_install_workgroup_functions_np",
      "__interposition_sim_system_pthread_prefer_alternate_cluster_self",
  };
  for (const char *Name : Required) {
    if (!requireExport(Host, Name)) {
      FreeLibrary(Host);
      return 1;
    }
  }

  struct AliasPair {
    const char *Normal;
    const char *NoCancel;
  };
  const AliasPair AliasPairs[] = {
      {"open", "open$NOCANCEL"},
      {"close", "close$NOCANCEL"},
      {"fcntl", "fcntl$NOCANCEL"},
      {"pread", "pread$NOCANCEL"},
      {"pwrite", "pwrite$NOCANCEL"},
      {"read", "read$NOCANCEL"},
      {"write", "write$NOCANCEL"},
  };
  for (const AliasPair &Pair : AliasPairs) {
    if (GetProcAddress(Host, Pair.Normal) != GetProcAddress(Host, Pair.NoCancel)) {
      FreeLibrary(Host);
      return fail("a no-cancel file syscall is not an alias of its base operation");
    }
  }

  using DarwinError = int *(*)();
  auto ErrorPointer =
      reinterpret_cast<DarwinError>(GetProcAddress(Host, "__error"));
  int *HostErrno = ErrorPointer();
  if (!HostErrno) {
    FreeLibrary(Host);
    return fail("Darwin __error did not return thread-local errno storage");
  }

  using ContinuousTime = std::uint64_t (*)();
  auto Continuous = reinterpret_cast<ContinuousTime>(
      GetProcAddress(Host, "mach_continuous_time"));
  const std::uint64_t T0 = Continuous();
  Sleep(1);
  const std::uint64_t T1 = Continuous();
  if (T1 < T0) {
    FreeLibrary(Host);
    return fail("mach_continuous_time moved backwards");
  }

  auto *PageSize = reinterpret_cast<std::uint32_t *>(
      GetProcAddress(Host, "vm_page_size"));
  auto *TaskSelf = reinterpret_cast<std::uint32_t *>(
      GetProcAddress(Host, "mach_task_self_"));
  if (!PageSize || *PageSize == 0 || ((*PageSize & (*PageSize - 1)) != 0)) {
    FreeLibrary(Host);
    return fail("vm_page_size is not a non-zero power of two");
  }
  if (!TaskSelf || *TaskSelf == 0) {
    FreeLibrary(Host);
    return fail("mach_task_self_ is unavailable");
  }

  auto *Ndr = reinterpret_cast<DarwinNdrRecord *>(
      GetProcAddress(Host, "NDR_record"));
  const DarwinNdrRecord ExpectedNdr = {0, 0, 0, 0, 1, 0, 0, 0};
  if (!Ndr || std::memcmp(Ndr, &ExpectedNdr, sizeof(ExpectedNdr)) != 0) {
    FreeLibrary(Host);
    return fail("NDR_record does not match ARM64 Darwin NDR 2.0 representation");
  }

  using VmAllocate = std::int32_t (*)(std::uint32_t, std::uint64_t *,
                                      std::uint64_t, std::int32_t);
  using VmDeallocate =
      std::int32_t (*)(std::uint32_t, std::uint64_t, std::uint64_t);
  auto Allocate =
      reinterpret_cast<VmAllocate>(GetProcAddress(Host, "vm_allocate"));
  auto Deallocate =
      reinterpret_cast<VmDeallocate>(GetProcAddress(Host, "vm_deallocate"));
  std::uint64_t Address = 0;
  if (Allocate(*TaskSelf, &Address, *PageSize, 1) != 0 || Address == 0) {
    FreeLibrary(Host);
    return fail("vm_allocate did not allocate task-self memory");
  }
  std::memset(reinterpret_cast<void *>(static_cast<std::uintptr_t>(Address)),
              0x5a, *PageSize);
  if (Deallocate(*TaskSelf, Address, *PageSize) != 0) {
    FreeLibrary(Host);
    return fail("vm_deallocate did not release task-self memory");
  }

  using ProcPidInfo = int (*)(int, int, std::uint64_t, void *, int);
  auto ProcInfo =
      reinterpret_cast<ProcPidInfo>(GetProcAddress(Host, "proc_pidinfo"));
  DarwinProcBsdInfo Bsd{};
  if (ProcInfo(_getpid(), 3, 0, &Bsd, sizeof(Bsd)) != sizeof(Bsd) ||
      Bsd.Pid != static_cast<std::uint32_t>(_getpid()) || Bsd.Name[0] == '\0') {
    FreeLibrary(Host);
    return fail("PROC_PIDTBSDINFO did not report the host process");
  }

  DarwinProcTaskInfo Task{};
  if (ProcInfo(_getpid(), 4, 0, &Task, sizeof(Task)) != sizeof(Task) ||
      Task.ResidentSize == 0 || Task.ThreadCount <= 0) {
    FreeLibrary(Host);
    return fail("PROC_PIDTASKINFO lacks real process metrics");
  }

  // Keep descriptor creation and use inside the Darwin host bridge. A DLL and
  // its smoke executable are not required to share a private CRT fd table, so a
  // host-created _pipe is not a valid way to test the guest descriptor
  // namespace. This mirrors the guest-owned descriptor model used by mature
  // HLEs: open -> list/write/seek/read -> close all resolve in one subsystem.
  using DarwinOpen = int (*)(const char *, int, std::uint16_t);
  using DarwinClose = int (*)(int);
  using DarwinSeek = std::int64_t (*)(int, std::int64_t, int);
  using DarwinRead = std::intptr_t (*)(int, void *, std::size_t);
  using DarwinWrite = std::intptr_t (*)(int, const void *, std::size_t);
  using DarwinPread =
      std::intptr_t (*)(int, void *, std::size_t, std::int64_t);
  using DarwinPwrite =
      std::intptr_t (*)(int, const void *, std::size_t, std::int64_t);
  auto Open = reinterpret_cast<DarwinOpen>(GetProcAddress(Host, "open$NOCANCEL"));
  auto Close =
      reinterpret_cast<DarwinClose>(GetProcAddress(Host, "close$NOCANCEL"));
  auto Seek = reinterpret_cast<DarwinSeek>(GetProcAddress(Host, "lseek"));
  auto Read =
      reinterpret_cast<DarwinRead>(GetProcAddress(Host, "read$NOCANCEL"));
  auto Write =
      reinterpret_cast<DarwinWrite>(GetProcAddress(Host, "write$NOCANCEL"));
  auto Pread =
      reinterpret_cast<DarwinPread>(GetProcAddress(Host, "pread$NOCANCEL"));
  auto Pwrite =
      reinterpret_cast<DarwinPwrite>(GetProcAddress(Host, "pwrite$NOCANCEL"));

  constexpr int DarwinOpenReadWrite = 0x00000002;
  constexpr int DarwinOpenCreate = 0x00000200;
  constexpr int DarwinOpenTruncate = 0x00000400;
  constexpr int DarwinSeekSet = 0;
  constexpr int DarwinSeekCurrent = 1;
  const int GuestFd = Open("/darwin-kernel-smoke", DarwinOpenReadWrite |
                                                        DarwinOpenCreate |
                                                        DarwinOpenTruncate,
                           0600);
  if (GuestFd < 0) {
    FreeLibrary(Host);
    return fail("Darwin no-cancel open did not create a bridge-owned descriptor");
  }

  DarwinProcFdInfo Fds[64]{};
  const int FdBytes = ProcInfo(_getpid(), 1, 0, Fds, sizeof(Fds));
  if (FdBytes <= 0 || !descriptorListContains(Fds, FdBytes, GuestFd)) {
    Close(GuestFd);
    FreeLibrary(Host);
    return fail("PROC_PIDLISTFDS omitted the descriptor created by Darwin open");
  }

  const char Payload[] = "abcdef";
  if (Write(GuestFd, Payload, 6) != 6 || Seek(GuestFd, 4, DarwinSeekSet) != 4) {
    Close(GuestFd);
    FreeLibrary(Host);
    return fail("Darwin no-cancel write/lseek did not preserve descriptor semantics");
  }

  char PositionedRead[2] = {};
  if (Pread(GuestFd, PositionedRead, sizeof(PositionedRead), 1) != 2 ||
      std::memcmp(PositionedRead, "bc", 2) != 0 ||
      Seek(GuestFd, 0, DarwinSeekCurrent) != 4) {
    Close(GuestFd);
    FreeLibrary(Host);
    return fail("pread did not read from its explicit offset without moving fd position");
  }

  const char Replacement[] = "XY";
  if (Pwrite(GuestFd, Replacement, 2, 2) != 2 ||
      Seek(GuestFd, 0, DarwinSeekCurrent) != 4) {
    Close(GuestFd);
    FreeLibrary(Host);
    return fail("pwrite did not write at its explicit offset without moving fd position");
  }

  *HostErrno = 0;
  if (Pread(GuestFd, PositionedRead, 1, -1) != -1 || *HostErrno != EINVAL) {
    Close(GuestFd);
    FreeLibrary(Host);
    return fail("pread negative offset did not report EINVAL");
  }

  if (Seek(GuestFd, 0, DarwinSeekSet) != 0) {
    Close(GuestFd);
    FreeLibrary(Host);
    return fail("Darwin lseek could not rewind positional-I/O test file");
  }
  char Readback[6] = {};
  const std::intptr_t ReadCount = Read(GuestFd, Readback, sizeof(Readback));
  if (ReadCount != 6 || std::memcmp(Readback, "abXYef", 6) != 0) {
    Close(GuestFd);
    FreeLibrary(Host);
    return fail("pread/pwrite test payload did not persist at the expected offsets");
  }

  if (Close(GuestFd) != 0) {
    FreeLibrary(Host);
    return fail("Darwin no-cancel close did not release the bridge-owned descriptor");
  }
  std::memset(Fds, 0, sizeof(Fds));
  const int AfterCloseBytes = ProcInfo(_getpid(), 1, 0, Fds, sizeof(Fds));
  if (descriptorListContains(Fds, AfterCloseBytes, GuestFd)) {
    FreeLibrary(Host);
    return fail("closed descriptor remained visible in PROC_PIDLISTFDS");
  }

  using SigSuspendNoCancel = int (*)(std::uint32_t);
  auto SigSuspend = reinterpret_cast<SigSuspendNoCancel>(
      GetProcAddress(Host, "sigsuspend$NOCANCEL"));
  *HostErrno = 0;
  if (SigSuspend(0) != -1 || *HostErrno != ENOTSUP) {
    FreeLibrary(Host);
    return fail("sigsuspend no-cancel must report ENOTSUP until signal masks exist");
  }

  using MemcmpZero = int (*)(const void *, std::size_t);
  auto CompareZero = reinterpret_cast<MemcmpZero>(GetProcAddress(
      Host, "__interposition_sim_system__platform_memcmp_zero_aligned8"));
  std::uint64_t ZeroWords[2] = {};
  if (CompareZero(ZeroWords, sizeof(ZeroWords)) != 0) {
    FreeLibrary(Host);
    return fail("memcmp_zero reported non-zero for zero storage");
  }
  ZeroWords[1] = 1;
  if (CompareZero(ZeroWords, sizeof(ZeroWords)) == 0) {
    FreeLibrary(Host);
    return fail("memcmp_zero missed non-zero storage");
  }

  using Csops = int (*)(int, unsigned int, void *, std::size_t);
  auto CodeSign = reinterpret_cast<Csops>(
      GetProcAddress(Host, "__interposition_sim_system_csops"));
  *HostErrno = 0;
  if (CodeSign(_getpid(), 0, nullptr, 0) != -1 || *HostErrno != ENOTSUP) {
    FreeLibrary(Host);
    return fail("csops must fail explicitly until a code-sign semantic exists");
  }

  using CreateWithWorkgroup = int (*)(void *, void *, const void *, void *, void *);
  auto CreateWorkgroupThread = reinterpret_cast<CreateWithWorkgroup>(GetProcAddress(
      Host, "__interposition_sim_system_pthread_create_with_workgroup_np"));
  if (CreateWorkgroupThread(nullptr, nullptr, nullptr, nullptr, nullptr) != ENOTSUP) {
    FreeLibrary(Host);
    return fail("workgroup creation must report ENOTSUP rather than fake success");
  }

  std::printf("[darwin-kernel-smoke] process/time/VM/no-cancel file semantics passed\n");
  FreeLibrary(Host);
  return 0;
}