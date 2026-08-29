#include "FifoAdapter.hpp"
#include "MachIpc.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <thread>
#include <windows.h>

namespace {

using namespace ipasim::mach;

struct InlineMessage {
  MessageHeader Header;
  std::uint64_t Payload;
};
static_assert(sizeof(InlineMessage) == 32,
              "Mach IPC smoke message layout changed unexpectedly");

bool expect(MessageReturn Actual, MessageReturn Expected, const char *Name) {
  if (Actual == Expected)
    return true;
  std::fprintf(stderr,
               "[mach-ipc-smoke] %s returned 0x%08X; expected 0x%08X\n",
               Name, static_cast<unsigned>(Actual),
               static_cast<unsigned>(Expected));
  return false;
}

void trace(const char *Message) {
  std::fprintf(stderr, "[mach-ipc-smoke] TRACE %s\n", Message);
  std::fflush(stderr);
}

} // namespace

int main(int ArgC, char **ArgV) {
  using namespace ipasim::mach;

  const PortName TaskSelf = taskSelfPort();
  if (TaskSelf == PortNull || taskSelfPort() != TaskSelf) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] task self port was null or unstable\n");
    return 1;
  }
  if (deallocateReceiveRight(TaskSelf)) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] task self receive right was user-owned\n");
    return 2;
  }

  std::int32_t DirectPid = -1;
  if (pidForTask(TaskSelf, &DirectPid) != KernelSuccess ||
      DirectPid != _getpid()) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] pid_for_task did not map task-self to the real process id\n");
    return 3;
  }
  std::int32_t InvalidPid = 123;
  if (pidForTask(0x7ffffffeU, &InvalidPid) != KernelFailure ||
      InvalidPid != -1) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] pid_for_task invalid-port semantics changed\n");
    return 4;
  }

  InlineMessage TaskMessage{};
  TaskMessage.Header.Bits = MessageTypeCopySend;
  TaskMessage.Header.Size = sizeof(TaskMessage);
  TaskMessage.Header.RemotePort = TaskSelf;
  TaskMessage.Header.Id = 50;
  if (!expect(messageOverwrite(&TaskMessage.Header, MachSendMsg,
                               sizeof(TaskMessage), 0, PortNull, 0,
                               PortNull, nullptr, 0),
              MessageSuccess, "task-self send right"))
    return 5;

  if (ArgC != 2) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] expected IpaSimDarwinHost.dll path\n");
    return 6;
  }

  HMODULE DarwinHost = LoadLibraryA(ArgV[1]);
  if (!DarwinHost) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] could not load Darwin host bridge (error %lu)\n",
                 static_cast<unsigned long>(GetLastError()));
    return 7;
  }

  FARPROC TaskSelfExport = GetProcAddress(DarwinHost, "mach_task_self_");
  if (!TaskSelfExport) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] mach_task_self_ DATA export was missing\n");
    FreeLibrary(DarwinHost);
    return 8;
  }
  const PortName ExportedTaskSelf =
      *reinterpret_cast<const PortName *>(TaskSelfExport);
  if (ExportedTaskSelf == PortNull) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] mach_task_self_ DATA export was zero\n");
    FreeLibrary(DarwinHost);
    return 9;
  }

  using PidForTask = std::int32_t (*)(std::uint32_t, std::int32_t *);
  auto HostPidForTask = reinterpret_cast<PidForTask>(
      GetProcAddress(DarwinHost, "pid_for_task"));
  if (!HostPidForTask) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] pid_for_task export was missing\n");
    FreeLibrary(DarwinHost);
    return 10;
  }
  std::int32_t ExportedPid = -1;
  if (HostPidForTask(ExportedTaskSelf, &ExportedPid) != KernelSuccess ||
      ExportedPid != _getpid()) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] pid_for_task export did not preserve task/process identity\n");
    FreeLibrary(DarwinHost);
    return 11;
  }

  using ProcPidPath = int (*)(int, void *, std::uint32_t);
  auto HostProcPidPath = reinterpret_cast<ProcPidPath>(
      GetProcAddress(DarwinHost, "proc_pidpath"));
  using DarwinError = int *(*)();
  auto HostError = reinterpret_cast<DarwinError>(
      GetProcAddress(DarwinHost, "__error"));
  if (!HostProcPidPath || !HostError) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] proc_pidpath or __error export was missing\n");
    FreeLibrary(DarwinHost);
    return 12;
  }

  char ProcessPath[4096] = {};
  const int ProcessPathLength =
      HostProcPidPath(_getpid(), ProcessPath, sizeof(ProcessPath));
  if (ProcessPathLength <= 0 ||
      static_cast<std::size_t>(ProcessPathLength) != std::strlen(ProcessPath) ||
      std::strstr(ProcessPath, "MachIpcSmoke") == nullptr ||
      std::strchr(ProcessPath, '\\') != nullptr) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] proc_pidpath did not return the real Darwin-facing host process path: %s\n",
                 ProcessPath);
    FreeLibrary(DarwinHost);
    return 13;
  }

  char TooSmall[1023] = {};
  *HostError() = 0;
  if (HostProcPidPath(_getpid(), TooSmall, sizeof(TooSmall)) != 0 ||
      *HostError() != ENOMEM) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] proc_pidpath undersized-buffer errno contract changed\n");
    FreeLibrary(DarwinHost);
    return 14;
  }

  using HostOpen = int (*)(const char *, int, std::uint16_t);
  using HostRead = std::intptr_t (*)(int, void *, std::size_t);
  using HostClose = int (*)(int);
  using HostReadlink = std::intptr_t (*)(const char *, char *, std::size_t);
  using PlatformStrchr = char *(*)(char *, int);
  using PlatformStrcmp = int (*)(const char *, const char *);
  using PlatformStrlcpy = std::size_t (*)(char *, const char *, std::size_t);
  using PlatformStrlen = std::size_t (*)(const char *);
  using PlatformStrncmp = int (*)(const char *, const char *, std::size_t);
  using MachTaskIsSelf = std::int32_t (*)(std::uint32_t);
  using PthreadCpuNumber = int (*)(std::size_t *);
  using UnfairLockWithOptions = void (*)(std::uint32_t *, std::uint32_t);
  using UnfairUnlock = void (*)(std::uint32_t *);

  auto DarwinOpen = reinterpret_cast<HostOpen>(GetProcAddress(DarwinHost, "open"));
  auto DarwinRead = reinterpret_cast<HostRead>(GetProcAddress(DarwinHost, "read"));
  auto DarwinClose = reinterpret_cast<HostClose>(GetProcAddress(DarwinHost, "close"));
  auto DarwinReadlink = reinterpret_cast<HostReadlink>(
      GetProcAddress(DarwinHost, "readlink"));
  auto PlatformStrchrFn = reinterpret_cast<PlatformStrchr>(
      GetProcAddress(DarwinHost, "_platform_strchr"));
  auto PlatformStrcmpFn = reinterpret_cast<PlatformStrcmp>(
      GetProcAddress(DarwinHost, "_platform_strcmp"));
  auto PlatformStrlcpyFn = reinterpret_cast<PlatformStrlcpy>(
      GetProcAddress(DarwinHost, "_platform_strlcpy"));
  auto PlatformStrlenFn = reinterpret_cast<PlatformStrlen>(
      GetProcAddress(DarwinHost, "_platform_strlen"));
  auto PlatformStrncmpFn = reinterpret_cast<PlatformStrncmp>(
      GetProcAddress(DarwinHost, "_platform_strncmp"));
  auto MachTaskIsSelfFn = reinterpret_cast<MachTaskIsSelf>(GetProcAddress(
      DarwinHost, "__interposition_sim_system_mach_task_is_self"));
  auto PthreadCpuNumberFn = reinterpret_cast<PthreadCpuNumber>(GetProcAddress(
      DarwinHost, "__interposition_sim_system_pthread_cpu_number_np"));
  auto UnfairLockFn = reinterpret_cast<UnfairLockWithOptions>(GetProcAddress(
      DarwinHost, "os_unfair_lock_lock_with_options"));
  auto UnfairUnlockFn = reinterpret_cast<UnfairUnlock>(
      GetProcAddress(DarwinHost, "os_unfair_lock_unlock"));

  if (!DarwinOpen || !DarwinRead || !DarwinClose || !DarwinReadlink ||
      !PlatformStrchrFn || !PlatformStrcmpFn || !PlatformStrlcpyFn ||
      !PlatformStrlenFn || !PlatformStrncmpFn || !MachTaskIsSelfFn ||
      !PthreadCpuNumberFn || !UnfairLockFn || !UnfairUnlockFn) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] first simulator host ABI batch export was missing\n");
    FreeLibrary(DarwinHost);
    return 15;
  }

  // Keep the complete descriptor lifecycle inside IpaSimDarwinHost. CRT file
  // descriptors belong to the CRT instance that created them, so the smoke must
  // not manufacture a descriptor in its own executable and pass it into the
  // bridge. Create a real empty regular node through the Darwin filesystem
  // namespace, then validate read EOF/error semantics through that same bridge.
  constexpr const char *ReadGuestPath = "/ipasim-read-smoke";
  const int ReadFlags = ipasim::darwinfs::DarwinOpenReadOnly |
                        ipasim::darwinfs::DarwinOpenCreate |
                        ipasim::darwinfs::DarwinOpenExclusive;
  trace("read bridge open call");
  const int ReadFd = DarwinOpen(ReadGuestPath, ReadFlags, 0600);
  trace("read bridge open returned");
  if (ReadFd == -1) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] Darwin open could not create read smoke node %s (errno %d)\n",
                 ReadGuestPath, *HostError());
    FreeLibrary(DarwinHost);
    return 16;
  }

  char ReadBuffer[2] = {};
  trace("read EOF exported call");
  const std::intptr_t DarwinReadCount =
      DarwinRead(ReadFd, ReadBuffer, sizeof(ReadBuffer));
  trace("read EOF exported call returned");
  if (DarwinReadCount != 0) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] Darwin read returned %lld for an empty regular node; expected EOF 0 (errno %d)\n",
                 static_cast<long long>(DarwinReadCount), *HostError());
    DarwinClose(ReadFd);
    FreeLibrary(DarwinHost);
    return 19;
  }

  *HostError() = 0;
  trace("read null-buffer exported call");
  const std::intptr_t NullReadCount = DarwinRead(ReadFd, nullptr, 1);
  trace("read null-buffer exported call returned");
  if (NullReadCount != -1 || *HostError() != EFAULT) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] Darwin read null-buffer contract changed: result %lld errno %d\n",
                 static_cast<long long>(NullReadCount), *HostError());
    DarwinClose(ReadFd);
    FreeLibrary(DarwinHost);
    return 19;
  }

  trace("read zero-length exported call");
  const std::intptr_t ZeroReadCount = DarwinRead(ReadFd, nullptr, 0);
  trace("read zero-length exported call returned");
  if (ZeroReadCount != 0) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] Darwin zero-length read returned %lld; expected 0 (errno %d)\n",
                 static_cast<long long>(ZeroReadCount), *HostError());
    DarwinClose(ReadFd);
    FreeLibrary(DarwinHost);
    return 19;
  }

  trace("read bridge close call");
  if (DarwinClose(ReadFd) != 0) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] Darwin close failed after read smoke (errno %d)\n",
                 *HostError());
    FreeLibrary(DarwinHost);
    return 19;
  }
  trace("read bridge close returned");
  trace("read passed");

  trace("libplatform strings begin");
  char MutableText[] = "alpha-beta";
  char Copy[6] = {};
  if (PlatformStrlenFn(MutableText) != 10 ||
      PlatformStrcmpFn("same", "same") != 0 ||
      PlatformStrcmpFn("a", "b") >= 0 ||
      PlatformStrncmpFn("abcdef", "abcxyz", 3) != 0 ||
      PlatformStrchrFn(MutableText, '-') != MutableText + 5 ||
      PlatformStrlcpyFn(Copy, "abcdef", sizeof(Copy)) != 6 ||
      std::strcmp(Copy, "abcde") != 0) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] libplatform string primitive semantics changed\n");
    FreeLibrary(DarwinHost);
    return 20;
  }
  trace("libplatform strings passed");

  trace("readlink contracts begin");
  *HostError() = 0;
  char LinkBuffer[8] = {};
  if (DarwinReadlink(nullptr, LinkBuffer, sizeof(LinkBuffer)) != -1 ||
      *HostError() != EFAULT) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] readlink null-path errno contract changed\n");
    FreeLibrary(DarwinHost);
    return 21;
  }
  *HostError() = 0;
  if (DarwinReadlink(".", LinkBuffer, 0) != -1 || *HostError() != EINVAL) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] readlink zero-buffer errno contract changed\n");
    FreeLibrary(DarwinHost);
    return 22;
  }
  trace("readlink contracts passed");

  trace("mach_task_is_self begin");
  if (MachTaskIsSelfFn(ExportedTaskSelf) != 1 ||
      MachTaskIsSelfFn(0x7ffffffeU) != 0) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] mach_task_is_self identity semantics changed\n");
    FreeLibrary(DarwinHost);
    return 23;
  }
  trace("mach_task_is_self passed");

  trace("pthread_cpu_number_np begin");
  std::size_t CpuNumber = static_cast<std::size_t>(-1);
  const DWORD ActiveCpuCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  if (PthreadCpuNumberFn(&CpuNumber) != 0 || ActiveCpuCount == 0 ||
      ActiveCpuCount == 0xffffffffU || CpuNumber >= ActiveCpuCount) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] pthread_cpu_number_np returned invalid CPU %zu of %lu\n",
                 CpuNumber, static_cast<unsigned long>(ActiveCpuCount));
    FreeLibrary(DarwinHost);
    return 24;
  }
  trace("pthread_cpu_number_np passed");

  trace("unfair lock main acquire begin");
  std::uint32_t UnfairWord = 0;
  UnfairLockFn(&UnfairWord, 0);
  trace("unfair lock main acquire returned");
  if (UnfairWord == 0) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] unfair lock did not mark ownership\n");
    FreeLibrary(DarwinHost);
    return 25;
  }

  std::atomic<bool> WaiterStarted{false};
  std::atomic<bool> WaiterAcquired{false};
  std::thread Waiter([&]() {
    trace("unfair lock waiter thread entered");
    WaiterStarted.store(true, std::memory_order_release);
    trace("unfair lock waiter acquire begin");
    UnfairLockFn(&UnfairWord, 0);
    trace("unfair lock waiter acquire returned");
    WaiterAcquired.store(true, std::memory_order_release);
    trace("unfair lock waiter unlock begin");
    UnfairUnlockFn(&UnfairWord);
    trace("unfair lock waiter unlock returned");
  });
  while (!WaiterStarted.load(std::memory_order_acquire))
    Sleep(1);
  Sleep(20);
  if (WaiterAcquired.load(std::memory_order_acquire)) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] unfair lock did not block a competing thread\n");
    TerminateProcess(GetCurrentProcess(), 26);
  }
  trace("unfair lock main unlock begin");
  UnfairUnlockFn(&UnfairWord);
  trace("unfair lock main unlock returned");
  Waiter.join();
  trace("unfair lock waiter joined");
  if (!WaiterAcquired.load(std::memory_order_acquire) || UnfairWord != 0) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] unfair lock handoff/unlock semantics changed\n");
    FreeLibrary(DarwinHost);
    return 27;
  }
  trace("unfair lock handoff passed");

  FreeLibrary(DarwinHost);

  const PortName Port = allocateReceivePort();
  if (Port == PortNull || !insertSendRight(Port)) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] failed to allocate receive/send rights\n");
    return 28;
  }

  InlineMessage InvalidDestination{};
  InvalidDestination.Header.Bits = MessageTypeCopySend;
  InvalidDestination.Header.Size = sizeof(InvalidDestination);
  InvalidDestination.Header.RemotePort = 0x7ffffffeU;
  InvalidDestination.Header.Id = 100;
  if (!expect(messageOverwrite(&InvalidDestination.Header, MachSendMsg,
                               sizeof(InvalidDestination), 0, PortNull, 0,
                               PortNull, nullptr, 0),
              SendInvalidDestination, "invalid destination"))
    return 29;

  InlineMessage Prioritized{};
  Prioritized.Header.Bits = MessageTypeCopySend;
  Prioritized.Header.Size = sizeof(Prioritized);
  Prioritized.Header.RemotePort = Port;
  Prioritized.Header.Id = 200;
  if (!expect(messageOverwrite(&Prioritized.Header, MachSendMsg,
                               sizeof(Prioritized), 0, PortNull, 0,
                               /* priority */ 1, nullptr, 0),
              SendInvalidOptions, "unsupported send priority"))
    return 30;

  InlineMessage Sent{};
  Sent.Header.Bits = MessageTypeCopySend;
  Sent.Header.Size = sizeof(Sent);
  Sent.Header.RemotePort = Port;
  Sent.Header.Id = 4242;
  Sent.Payload = 0x1122334455667788ULL;

  if (!expect(messageOverwrite(&Sent.Header, MachSendMsg, sizeof(Sent), 0,
                               PortNull, 0, PortNull, nullptr, 0),
              MessageSuccess, "inline send"))
    return 31;

  InlineMessage Received{};
  if (!expect(messageOverwrite(nullptr, MachReceiveMsg | MachReceiveTimeout, 0,
                               sizeof(Received), Port, 0, PortNull,
                               &Received.Header, 0),
              MessageSuccess, "overwrite receive"))
    return 32;

  if (Received.Header.Id != Sent.Header.Id || Received.Payload != Sent.Payload ||
      Received.Header.LocalPort != Port ||
      Received.Header.RemotePort != PortNull) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] received header/payload did not match send\n");
    return 33;
  }

  InlineMessage EmptyReceive{};
  if (!expect(messageOverwrite(nullptr, MachReceiveMsg | MachReceiveTimeout, 0,
                               sizeof(EmptyReceive), Port, 0, PortNull,
                               &EmptyReceive.Header, 0),
              ReceiveTimedOut, "empty receive timeout"))
    return 34;

  InlineMessage Complex{};
  Complex.Header.Bits = MessageBitsComplex | MessageTypeCopySend;
  Complex.Header.Size = sizeof(Complex);
  Complex.Header.RemotePort = Port;
  if (!expect(messageOverwrite(&Complex.Header, MachSendMsg, sizeof(Complex), 0,
                               PortNull, 0, PortNull, nullptr, 0),
              SendInvalidData, "complex message rejection"))
    return 35;

  if (!deallocateReceiveRight(Port)) {
    std::fprintf(stderr,
                 "[mach-ipc-smoke] failed to deallocate receive right\n");
    return 36;
  }

  std::printf("Mach IPC smoke passed: task/process identity, proc_pidpath, first simulator host ABI batch (read/readlink contracts, libplatform strings, task-self identity, CPU number, unfair-lock mutual exclusion), real task send right, inline send/receive, timeout, and explicit unsupported boundaries.\n");
  return 0;
}
