// DarwinHostBridge.cpp: Native Windows implementations for the small subset
// of macOS host libSystem semantics reached by the iOS Simulator runtime.
//
// This DLL is deliberately not a generic libSystem stub. Every exported entry
// point must have a real Windows semantic equivalent or a real compatibility
// subsystem and is added only when the target reaches that boundary.

#include "DarwinSocketAdapter.hpp"
#include "FifoAdapter.hpp"
#include "MachIpc.hpp"

#include <algorithm>
#include <atomic>
#include <errno.h>
#include <filesystem>
#include <io.h>
#include <limits.h>
#include <memory>
#include <mutex>
#include <process.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <windows.h>

namespace {

// Darwin fcntl command/flag values. Do not pass these integers through to a
// Windows API: the bridge translates each supported command explicitly.
constexpr int DarwinFGetFd = 1;
constexpr int DarwinFSetFd = 2;
constexpr intptr_t DarwinFdCloExec = 1;

// XNU proc_info.h defines PROC_PIDPATHINFO_SIZE as MAXPATHLEN and the public
// wrapper accepts at most four times MAXPATHLEN. Darwin MAXPATHLEN is 1024.
constexpr uint32_t DarwinProcPidPathInfoSize = 1024;
constexpr uint32_t DarwinProcPidPathInfoMaxSize = 4096;

// Darwin's __pthread_fchdir syscall changes the working directory used by one
// thread, not the process-wide current directory. Windows has no Win32
// equivalent, so the host bridge keeps the same semantic state explicitly as a
// duplicated CRT-backed directory descriptor. Filesystem bridge calls that
// resolve relative paths must consume this descriptor rather than calling
// SetCurrentDirectory().
struct DarwinThreadState {
  int WorkingDirectoryFd = -1;

  ~DarwinThreadState() {
    if (WorkingDirectoryFd != -1)
      _close(WorkingDirectoryFd);
  }
};

thread_local DarwinThreadState ThreadState;

void clearThreadWorkingDirectory() {
  if (ThreadState.WorkingDirectoryFd != -1) {
    _close(ThreadState.WorkingDirectoryFd);
    ThreadState.WorkingDirectoryFd = -1;
  }
}

bool getNativeHandle(int Fd, HANDLE &Handle) {
  const intptr_t NativeHandle = _get_osfhandle(Fd);
  if (NativeHandle == -1) {
    errno = EBADF;
    return false;
  }
  Handle = reinterpret_cast<HANDLE>(NativeHandle);
  return true;
}

void setErrnoFromHandleError(DWORD Error) {
  switch (Error) {
  case ERROR_INVALID_HANDLE:
    errno = EBADF;
    break;
  case ERROR_ACCESS_DENIED:
    errno = EACCES;
    break;
  default:
    errno = EIO;
    break;
  }
}

void setErrnoFromProcessQueryError(DWORD Error) {
  switch (Error) {
  case ERROR_INVALID_PARAMETER:
  case ERROR_NOT_FOUND:
    errno = ESRCH;
    break;
  case ERROR_ACCESS_DENIED:
    errno = EPERM;
    break;
  case ERROR_INSUFFICIENT_BUFFER:
    errno = ENOMEM;
    break;
  default:
    errno = EIO;
    break;
  }
}

void setErrnoFromFilesystemError(const std::error_code &Error) {
  if (!Error) {
    errno = EIO;
    return;
  }

  using std::errc;
  const std::error_condition Condition = Error.default_error_condition();
  if (Condition == std::make_error_condition(errc::no_such_file_or_directory))
    errno = ENOENT;
  else if (Condition == std::make_error_condition(errc::permission_denied))
    errno = EACCES;
  else if (Condition == std::make_error_condition(errc::not_a_directory))
    errno = ENOTDIR;
  else if (Condition == std::make_error_condition(errc::invalid_argument) ||
           Condition == std::make_error_condition(errc::operation_not_supported))
    errno = EINVAL;
  else
    errno = EIO;
}

// os_unfair_lock is a four-byte Darwin userspace word whose kernel slow path
// blocks waiters and enforces ownership. A Win32 SRWLOCK is pointer-sized and
// therefore cannot be written into Darwin's four-byte storage. Keep the real
// blocking state externally, keyed by the Darwin lock address. One SRW lock
// serializes owner transitions and one condition variable blocks contending
// threads until ownership is released. This keeps the four-byte guest-visible
// word separate from the host synchronization object without splitting the
// ownership and wakeup state across unrelated primitives.
struct UnfairLockState {
  SRWLOCK StateLock = SRWLOCK_INIT;
  CONDITION_VARIABLE Condition = CONDITION_VARIABLE_INIT;
  DWORD OwnerThreadId = 0;
};

std::mutex UnfairLockRegistryMutex;
std::unordered_map<const void *, std::shared_ptr<UnfairLockState>>
    UnfairLockRegistry;

std::shared_ptr<UnfairLockState> unfairLockState(const void *Address) {
  std::lock_guard<std::mutex> Guard(UnfairLockRegistryMutex);
  auto &Entry = UnfairLockRegistry[Address];
  if (!Entry)
    Entry = std::make_shared<UnfairLockState>();
  return Entry;
}

[[noreturn]] void failFastUnfairLockContract(const char *Reason,
                                              const void *Address,
                                              DWORD OwnerThreadId,
                                              DWORD CurrentThreadId) {
  std::fprintf(stderr,
               "[darwin-host-unfair-lock] %s; lock=%p owner=%lu current=%lu\n",
               Reason, Address, static_cast<unsigned long>(OwnerThreadId),
               static_cast<unsigned long>(CurrentThreadId));
  std::fflush(stderr);
  RaiseFailFastException(nullptr, nullptr, 0);
  TerminateProcess(GetCurrentProcess(), 0xC0000409u);
  ::abort();
}

} // namespace

extern "C" {

// Darwin libsyscall exposes mach_task_self_ as a mutable global mach_port_t,
// populated from task_self_trap() during Mach initialization. The simulator host
// already has that cached task-port send right by the time libsystem_sim_kernel
// links against it. ipaSim mirrors the same ABI with one stable non-null port
// name backed by IpaSimMachIpc. DarwinHostBridge.def marks this as a PE DATA
// export so chained fixups bind to the variable address rather than a function.
uint32_t mach_task_self_ = ipasim::mach::taskSelfPort();

// Darwin's ___error Mach-O symbol is the C function __error after removing
// Mach-O's leading symbol underscore. Both Darwin and the Windows CRT return a
// pointer to thread-local errno storage.
__declspec(dllexport) int *__error(void) { return _errno(); }

// Darwin syscall ___pthread_fchdir is exposed to PE lookup as
// __pthread_fchdir after ipaSim removes Mach-O's symbol prefix. Darwin uses -1
// to clear the per-thread override and inherit the process working directory.
// For a real descriptor, retain an independent reference exactly as the Darwin
// kernel does so closing the caller's descriptor does not invalidate the
// thread's working-directory state.
__declspec(dllexport) int __pthread_fchdir(int NewFd) {
  if (NewFd == -1) {
    clearThreadWorkingDirectory();
    return 0;
  }

  const int Duplicate = _dup(NewFd);
  if (Duplicate == -1)
    return -1; // _dup sets thread-local errno (for example EBADF).

  HANDLE NativeHandle;
  if (!getNativeHandle(Duplicate, NativeHandle)) {
    _close(Duplicate);
    return -1;
  }

  // Darwin marks its retained descriptor close-on-exec. Clear Windows handle
  // inheritance to preserve the same lifetime boundary for future spawn work.
  if (!SetHandleInformation(NativeHandle, HANDLE_FLAG_INHERIT, 0)) {
    const DWORD Error = GetLastError();
    _close(Duplicate);
    setErrnoFromHandleError(Error);
    return -1;
  }

  clearThreadWorkingDirectory();
  ThreadState.WorkingDirectoryFd = Duplicate;
  return 0;
}

// close participates in the same guest descriptor namespace as open/socket and
// proc_pidinfo. Dispatch sockets through the Winsock registry, and forget a
// filesystem descriptor only after the CRT close succeeds so stale fds cannot
// remain visible through PROC_PIDLISTFDS.
__declspec(dllexport) int close(int Fd) {
  if (ipasim::darwinsock::isSocketDescriptor(Fd))
    return ipasim::darwinsock::closeSocket(Fd);

  if (!ipasim::darwinfs::isOpenNodeDescriptor(Fd)) {
    errno = EBADF;
    return -1;
  }

  const int Result = _close(Fd);
  if (Result == 0)
    ipasim::darwinfs::forgetOpenNodeDescriptor(Fd);
  return Result;
}

// Darwin read operates on the same descriptor namespace as open/close/lseek.
// UCRT _read accepts a 32-bit request count; POSIX permits a successful read to
// return fewer bytes than requested, so cap a single host call at INT_MAX rather
// than splitting one Darwin read into multiple operations with different short-
// read/blocking semantics. The C identifier is intentionally distinct because
// UCRT already declares read with a different source-level return type; the .def
// file exports this function under the required PE name `read`.
intptr_t darwin_read(int Fd, void *Buffer, size_t Count) {
  if (!Buffer && Count != 0) {
    errno = EFAULT;
    return -1;
  }
  if (Count == 0)
    return 0;
  const unsigned int HostCount = static_cast<unsigned int>(
      std::min(Count, static_cast<size_t>(INT_MAX)));
  return static_cast<intptr_t>(_read(Fd, Buffer, HostCount));
}

// Darwin readlink returns the link contents without a terminating NUL and may
// truncate to the caller's buffer size. std::filesystem::read_symlink uses the
// native Windows reparse-point implementation, giving this bridge real symlink
// semantics instead of returning a resolved/fabricated path. Relative paths are
// fail-closed while the Darwin per-thread fchdir override is active because that
// override is intentionally not the Windows process CWD.
__declspec(dllexport) intptr_t readlink(const char *Path, char *Buffer,
                                        size_t BufferSize) {
  if (!Path || (!Buffer && BufferSize != 0)) {
    errno = EFAULT;
    return -1;
  }
  if (BufferSize == 0) {
    errno = EINVAL;
    return -1;
  }
  if (Path[0] != '/' && ThreadState.WorkingDirectoryFd != -1) {
    errno = ENOTSUP;
    return -1;
  }

  std::error_code Error;
  std::filesystem::path Target =
      std::filesystem::read_symlink(std::filesystem::u8path(Path), Error);
  if (Error) {
    setErrnoFromFilesystemError(Error);
    return -1;
  }

  std::string TargetUtf8 = Target.u8string();
  for (char &Character : TargetUtf8) {
    if (Character == '\\')
      Character = '/';
  }
  const size_t Bytes = std::min(BufferSize, TargetUtf8.size());
  if (Bytes != 0)
    ::memcpy(Buffer, TargetUtf8.data(), Bytes);
  return static_cast<intptr_t>(Bytes);
}

// Darwin fcntl is variadic in C, but the target-proven commands here are all in
// the AAPCS64 integer/pointer register class. Export a fixed third integer
// argument so ipaSim never relies on unsupported native varargs classification.
// F_GETFD ignores Argument. F_SETFD translates Darwin FD_CLOEXEC into the
// inverse Windows HANDLE_FLAG_INHERIT state. Unknown commands fail closed so a
// future target-required fcntl operation becomes an explicit boundary.
__declspec(dllexport) int fcntl(int Fd, int Command, intptr_t Argument) {
  HANDLE NativeHandle;
  if (!getNativeHandle(Fd, NativeHandle))
    return -1;

  switch (Command) {
  case DarwinFGetFd: {
    DWORD Flags = 0;
    if (!GetHandleInformation(NativeHandle, &Flags)) {
      setErrnoFromHandleError(GetLastError());
      return -1;
    }
    // Darwin FD_CLOEXEC means the descriptor must not cross exec. Windows
    // HANDLE_FLAG_INHERIT is the inverse state for a future child process.
    return (Flags & HANDLE_FLAG_INHERIT) ? 0 : static_cast<int>(DarwinFdCloExec);
  }

  case DarwinFSetFd: {
    if ((Argument & ~DarwinFdCloExec) != 0) {
      errno = EINVAL;
      return -1;
    }
    const bool Inherit = (Argument & DarwinFdCloExec) == 0;
    if (!SetHandleInformation(NativeHandle, HANDLE_FLAG_INHERIT,
                              Inherit ? HANDLE_FLAG_INHERIT : 0)) {
      setErrnoFromHandleError(GetLastError());
      return -1;
    }
    return 0;
  }

  default:
    errno = EINVAL;
    return -1;
  }
}

// Darwin getpid returns the process identifier for the calling process. The
// Windows CRT _getpid has the same process-scoped integer contract, so this is
// a direct semantic bridge rather than a fabricated simulator value.
__declspec(dllexport) int getpid(void) { return _getpid(); }

// Darwin proc_pidpath returns the full executable path for a process and the
// byte length of that path, excluding the terminator. XNU's libproc wrapper
// rejects buffers smaller than MAXPATHLEN (1024) with ENOMEM and buffers larger
// than 4*MAXPATHLEN with EOVERFLOW. Windows exposes the same process/path
// relationship through QueryFullProcessImageNameW. Convert the real host path
// to UTF-8 and POSIX separators for the Darwin-facing ABI; do not fabricate an
// app path when the requested PID does not exist or is inaccessible.
__declspec(dllexport) int proc_pidpath(int Pid, void *Buffer,
                                      uint32_t BufferSize) {
  if (!Buffer) {
    errno = EFAULT;
    return 0;
  }
  if (BufferSize < DarwinProcPidPathInfoSize) {
    errno = ENOMEM;
    return 0;
  }
  if (BufferSize > DarwinProcPidPathInfoMaxSize) {
    errno = EOVERFLOW;
    return 0;
  }

  HANDLE Process = nullptr;
  bool CloseProcess = false;
  if (Pid == _getpid()) {
    Process = GetCurrentProcess();
  } else {
    Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                          static_cast<DWORD>(Pid));
    if (!Process) {
      setErrnoFromProcessQueryError(GetLastError());
      return 0;
    }
    CloseProcess = true;
  }

  wchar_t WidePath[32768] = {};
  DWORD WideLength =
      static_cast<DWORD>(sizeof(WidePath) / sizeof(WidePath[0]));
  if (!QueryFullProcessImageNameW(Process, 0, WidePath, &WideLength)) {
    const DWORD Error = GetLastError();
    if (CloseProcess)
      CloseHandle(Process);
    setErrnoFromProcessQueryError(Error);
    return 0;
  }
  if (CloseProcess)
    CloseHandle(Process);

  const int Utf8Length = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, WidePath, static_cast<int>(WideLength),
      nullptr, 0, nullptr, nullptr);
  if (Utf8Length <= 0) {
    errno = EILSEQ;
    return 0;
  }
  if (static_cast<uint64_t>(Utf8Length) + 1 > BufferSize) {
    errno = ENOMEM;
    return 0;
  }

  std::string Utf8(static_cast<std::size_t>(Utf8Length), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, WidePath,
                          static_cast<int>(WideLength), Utf8.data(), Utf8Length,
                          nullptr, nullptr) != Utf8Length) {
    errno = EILSEQ;
    return 0;
  }

  // The ABI consumer is Darwin code, where '/' is the pathname separator.
  // Preserve the real Windows host path while translating only its separator.
  for (char &Character : Utf8) {
    if (Character == '\\')
      Character = '/';
  }

  ::memcpy(Buffer, Utf8.data(), Utf8.size());
  static_cast<char *>(Buffer)[Utf8.size()] = '\0';
  return Utf8Length;
}

// XNU pid_for_task resolves a Mach task port name to its BSD process id. This
// delegates to the same Mach port namespace that owns mach_task_self_, so the
// task-self port maps to the real host process id and non-task/unknown ports
// fail with KERN_FAILURE rather than manufacturing an identity.
__declspec(dllexport) int32_t pid_for_task(uint32_t Task, int32_t *ProcessId) {
  return ipasim::mach::pidForTask(Task, ProcessId);
}

// mach_task_is_self is a pure task-name identity test. The simulator's host
// interposition entry has the same ABI; compare against the same stable task
// port used by mach_task_self_ and pid_for_task.
__declspec(dllexport) int32_t
__interposition_sim_system_mach_task_is_self(uint32_t Task) {
  return Task != 0 && Task == mach_task_self_ ? 1 : 0;
}

// pthread_cpu_number_np reports the CPU on which the caller was executing. A
// Windows processor number is group-relative, so flatten all earlier processor
// groups to preserve Darwin's documented [0, ncpus) numbering contract.
__declspec(dllexport) int
__interposition_sim_system_pthread_cpu_number_np(size_t *CpuNumber) {
  if (!CpuNumber)
    return EINVAL;

  PROCESSOR_NUMBER Current{};
  GetCurrentProcessorNumberEx(&Current);
  size_t Flat = Current.Number;
  for (WORD Group = 0; Group < Current.Group; ++Group) {
    const DWORD Count = GetActiveProcessorCount(Group);
    if (Count == 0)
      return EIO;
    Flat += Count;
  }
  *CpuNumber = Flat;
  return 0;
}

// Darwin mkfifo creates a filesystem FIFO object at a pathname. NTFS has no
// POSIX FIFO file type, so this enters ipaSim's Darwin filesystem adapter. The
// guest pathname and mode metadata remain Darwin-facing while a real Win32
// named-pipe endpoint provides the transport. Later open/read/write/stat/unlink
// boundaries share the same registry rather than translating the path ad hoc.
__declspec(dllexport) int mkfifo(const char *Path, uint16_t Mode) {
  return ipasim::darwinfs::createFifo(Path, Mode);
}

// Darwin mknod creates a typed filesystem namespace object and carries a dev_t
// for character/block device nodes. The adapter preserves Darwin type bits,
// permissions, and device identity. FIFO nodes use the same named-pipe backing
// as mkfifo; regular nodes receive a private real backing file; device/socket
// nodes remain typed virtual namespace objects for later open/ioctl/socket
// dispatch instead of being misrepresented as ordinary NTFS files.
__declspec(dllexport) int mknod(const char *Path, uint16_t Mode,
                               int32_t Device) {
  return ipasim::darwinfs::createNode(Path, Mode, Device);
}

// Darwin open is variadic only because its mode_t argument is present when
// O_CREAT is set. The ARM64 bridge always marshals X0-X2 into this fixed entry;
// CreateMode is ignored when O_CREAT is absent. Regular virtual nodes return
// real CRT descriptors, so close/fcntl/lseek/read share one descriptor namespace.
// Relative-path resolution under __pthread_fchdir is deliberately fail-closed
// until that retained directory descriptor can be mapped into this namespace.
int darwin_open(const char *Path, int Flags, uint16_t CreateMode) {
  if (Path && Path[0] != '/' && ThreadState.WorkingDirectoryFd != -1) {
    errno = ENOTSUP;
    return -1;
  }
  return ipasim::darwinfs::openNode(Path, Flags, CreateMode);
}

// Darwin's mach_msg_overwrite is not mapped to a fake Windows syscall. It
// enters ipaSim's explicit Mach IPC subsystem, which owns a Mach-style port
// namespace and FIFO message queues and returns real Mach error codes for
// unsupported or invalid operations. The signature intentionally keeps all
// nine Darwin integer/pointer arguments visible at the PE boundary.
__declspec(dllexport) int32_t mach_msg_overwrite(
    ipasim::mach::MessageHeader *Message, int32_t Option, uint32_t SendSize,
    uint32_t ReceiveLimit, uint32_t ReceiveName, uint32_t Timeout,
    uint32_t Notify, ipasim::mach::MessageHeader *ReceiveMessage,
    uint32_t ReceiveScatterSize) {
  return ipasim::mach::messageOverwrite(
      Message, static_cast<ipasim::mach::MessageOption>(Option), SendSize,
      ReceiveLimit, ReceiveName, Timeout, Notify, ReceiveMessage,
      ReceiveScatterSize);
}

// Darwin off_t is signed 64-bit on modern arm64 targets. Windows' UCRT already
// declares a source-level lseek alias whose offset is 32-bit long, so the
// implementation uses a distinct C identifier and DarwinHostBridge.def exports
// it under the required PE name `lseek`. _lseeki64 preserves the target's
// 64-bit offset width, SEEK_SET / SEEK_CUR / SEEK_END behavior, and CRT errno.
int64_t darwin_lseek(int Fd, int64_t Offset, int Whence) {
  return static_cast<int64_t>(_lseeki64(Fd, Offset, Whence));
}

// libsystem_platform's basic string entry points are pure byte-string
// operations. UCRT has the same contracts; strlcpy is implemented explicitly
// because Windows does not provide that BSD interface.
__declspec(dllexport) char *_platform_strchr(char *String, int Character) {
  return ::strchr(String, Character);
}

__declspec(dllexport) int _platform_strcmp(const char *Left,
                                           const char *Right) {
  return ::strcmp(Left, Right);
}

__declspec(dllexport) size_t _platform_strlcpy(char *Destination,
                                               const char *Source,
                                               size_t DestinationSize) {
  const size_t SourceLength = ::strlen(Source);
  if (DestinationSize != 0) {
    const size_t CopyLength = std::min(SourceLength, DestinationSize - 1);
    if (CopyLength != 0)
      ::memcpy(Destination, Source, CopyLength);
    Destination[CopyLength] = '\0';
  }
  return SourceLength;
}

__declspec(dllexport) size_t _platform_strlen(const char *String) {
  return ::strlen(String);
}

__declspec(dllexport) int _platform_strncmp(const char *Left,
                                            const char *Right, size_t Count) {
  return ::strncmp(Left, Right, Count);
}

// libsystem_platform exports __platform_memmove in Mach-O. ipaSim removes the
// Mach-O symbol prefix before GetProcAddress(), so the PE export is named
// _platform_memmove. memmove has the required overlap-safe byte-copy semantics.
__declspec(dllexport) void *_platform_memmove(void *Destination,
                                              const void *Source,
                                              size_t Size) {
  return ::memmove(Destination, Source, Size);
}

// os_unfair_lock options are scheduling hints; the synchronization contract is
// mutual exclusion with ownership. Keep all ownership transitions under the
// state's SRW lock and use its condition variable for the contended slow path.
// Darwin requires unlock from the owning thread; contract violations fail fast
// instead of silently releasing another thread's lock.
__declspec(dllexport) void os_unfair_lock_lock_with_options(uint32_t *Lock,
                                                            uint32_t Options) {
  (void)Options;
  const DWORD CurrentThread = GetCurrentThreadId();
  if (!Lock)
    failFastUnfairLockContract("null lock passed to lock", Lock, 0,
                               CurrentThread);

  std::shared_ptr<UnfairLockState> State = unfairLockState(Lock);
  AcquireSRWLockExclusive(&State->StateLock);

  if (State->OwnerThreadId == CurrentThread) {
    const DWORD Owner = State->OwnerThreadId;
    ReleaseSRWLockExclusive(&State->StateLock);
    failFastUnfairLockContract("recursive acquisition of non-recursive lock",
                               Lock, Owner, CurrentThread);
  }

  while (State->OwnerThreadId != 0) {
    if (!SleepConditionVariableSRW(&State->Condition, &State->StateLock,
                                   INFINITE, 0)) {
      const DWORD Owner = State->OwnerThreadId;
      ReleaseSRWLockExclusive(&State->StateLock);
      failFastUnfairLockContract("condition wait failed", Lock, Owner,
                                 CurrentThread);
    }
  }

  State->OwnerThreadId = CurrentThread;
  const LONG Marker = static_cast<LONG>(CurrentThread == 0 ? 1 : CurrentThread);
  InterlockedExchange(reinterpret_cast<volatile LONG *>(Lock), Marker);
  ReleaseSRWLockExclusive(&State->StateLock);
}

__declspec(dllexport) void os_unfair_lock_unlock(uint32_t *Lock) {
  const DWORD CurrentThread = GetCurrentThreadId();
  if (!Lock)
    failFastUnfairLockContract("null lock passed to unlock", Lock, 0,
                               CurrentThread);

  std::shared_ptr<UnfairLockState> State = unfairLockState(Lock);
  AcquireSRWLockExclusive(&State->StateLock);
  if (State->OwnerThreadId != CurrentThread) {
    const DWORD Owner = State->OwnerThreadId;
    ReleaseSRWLockExclusive(&State->StateLock);
    failFastUnfairLockContract("unlock attempted by non-owner", Lock, Owner,
                               CurrentThread);
  }

  InterlockedExchange(reinterpret_cast<volatile LONG *>(Lock), 0);
  State->OwnerThreadId = 0;
  WakeConditionVariable(&State->Condition);
  ReleaseSRWLockExclusive(&State->StateLock);
}

} // extern "C"
