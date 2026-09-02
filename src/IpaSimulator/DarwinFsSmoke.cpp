#include "FifoAdapter.hpp"

#include <errno.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <io.h>
#include <windows.h>

namespace {

struct DarwinTimespec64 {
  std::int64_t Seconds;
  std::int64_t Nanoseconds;
};
static_assert(sizeof(DarwinTimespec64) == 16);

struct DarwinStat64 {
  std::int32_t DeviceId;
  std::uint16_t ModeBits;
  std::uint16_t LinkCount;
  std::uint64_t Inode;
  std::uint32_t UserId;
  std::uint32_t GroupId;
  std::int32_t SpecialDeviceId;
  DarwinTimespec64 AccessTime;
  DarwinTimespec64 ModificationTime;
  DarwinTimespec64 StatusChangeTime;
  DarwinTimespec64 BirthTime;
  std::int64_t Size;
  std::int64_t Blocks;
  std::int32_t BlockSize;
  std::uint32_t Flags;
  std::uint32_t Generation;
  std::int32_t Spare;
  std::int64_t QuadSpare[2];
};
static_assert(sizeof(DarwinStat64) == 144);

bool plausibleTimespec(const DarwinTimespec64 &Value) {
  return Value.Seconds > 0 && Value.Nanoseconds >= 0 &&
         Value.Nanoseconds < 1000000000LL;
}

int fail(const char *Message, int Code) {
  std::fprintf(stderr, "[darwin-fs-smoke] %s\n", Message);
  return Code;
}

} // namespace

int main(int ArgC, char **ArgV) {
  using namespace ipasim::darwinfs;

  constexpr const char *DirectPath = "/tmp/ipasim-fifo-smoke/../fifo";
  if (createFifo(DirectPath, 0660) != 0)
    return fail("could not create adapter FIFO", 1);

  FifoInfo Info;
  if (!lookupFifo("/tmp/fifo", Info))
    return fail("normalized FIFO path was not registered", 2);
  if (Info.Permissions != 0660 || Info.PipeName.empty())
    return fail("FIFO metadata was not preserved", 3);

  if (!WaitNamedPipeW(Info.PipeName.c_str(), 0))
    return fail("Windows named-pipe backing endpoint was not available", 4);

  errno = 0;
  if (createFifo("/tmp/fifo", 0600) != -1 || errno != EEXIST)
    return fail("duplicate FIFO did not report EEXIST", 5);

  if (!removeFifo("/tmp/fifo") || lookupFifo("/tmp/fifo", Info))
    return fail("FIFO registry removal failed", 6);

  constexpr Device CharacterDevice = static_cast<Device>(0x12000034u);
  if (createNode("/tmp/ipasim-character", DarwinCharacter | 0620,
                 CharacterDevice) != 0)
    return fail("could not create character-device node", 7);

  NodeInfo Node;
  if (!lookupNode("/tmp/ipasim-character", Node) ||
      Node.Type != NodeType::CharacterDevice || Node.Permissions != 0620 ||
      Node.DeviceNumber != CharacterDevice)
    return fail("mknod character-device metadata was not preserved", 8);
  if (!removeNode("/tmp/ipasim-character"))
    return fail("character-device node removal failed", 9);

  if (createNode("/tmp/ipasim-regular", DarwinRegular | 0640, 123) != 0)
    return fail("could not create regular mknod backing", 10);
  if (!lookupNode("/tmp/ipasim-regular", Node) ||
      Node.Type != NodeType::Regular || Node.Permissions != 0640 ||
      Node.DeviceNumber != 0 || Node.BackingPath.empty() ||
      GetFileAttributesW(Node.BackingPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    return fail("regular-node backing or metadata was not preserved", 11);

  const int DirectFd = openNode(
      "/tmp/ipasim-regular",
      DarwinOpenReadWrite | DarwinOpenTruncate | DarwinOpenCloseOnExec, 0);
  if (DirectFd < 0)
    return fail("regular Darwin node did not open as a CRT descriptor", 12);

  const intptr_t DirectNative = _get_osfhandle(DirectFd);
  DWORD DirectHandleFlags = 0;
  if (DirectNative == -1 ||
      !GetHandleInformation(reinterpret_cast<HANDLE>(DirectNative),
                            &DirectHandleFlags) ||
      (DirectHandleFlags & HANDLE_FLAG_INHERIT) != 0) {
    _close(DirectFd);
    forgetOpenNodeDescriptor(DirectFd);
    return fail("O_CLOEXEC did not produce a non-inheritable descriptor", 13);
  }

  constexpr char Payload[] = "darwin-open";
  if (_write(DirectFd, Payload, sizeof(Payload)) != sizeof(Payload) ||
      _lseeki64(DirectFd, 0, SEEK_SET) != 0) {
    _close(DirectFd);
    forgetOpenNodeDescriptor(DirectFd);
    return fail("regular Darwin descriptor write/seek failed", 14);
  }
  char Readback[sizeof(Payload)] = {};
  if (_read(DirectFd, Readback, sizeof(Readback)) != sizeof(Readback) ||
      std::memcmp(Payload, Readback, sizeof(Payload)) != 0) {
    _close(DirectFd);
    forgetOpenNodeDescriptor(DirectFd);
    return fail("regular Darwin descriptor readback failed", 15);
  }
  _close(DirectFd);
  forgetOpenNodeDescriptor(DirectFd);

  const std::wstring RegularBacking = Node.BackingPath;
  if (!removeNode("/tmp/ipasim-regular") ||
      GetFileAttributesW(RegularBacking.c_str()) != INVALID_FILE_ATTRIBUTES)
    return fail("regular-node backing was not removed with the namespace node",
                16);

  constexpr const char *OpenCreatePath = "/tmp/ipasim-open-create";
  const int CreatedFd = openNode(
      OpenCreatePath,
      DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenExclusive, 0604);
  if (CreatedFd < 0)
    return fail("O_CREAT did not create and open a regular Darwin node", 17);
  if (!lookupNode(OpenCreatePath, Node) || Node.Type != NodeType::Regular ||
      Node.Permissions != 0604) {
    _close(CreatedFd);
    forgetOpenNodeDescriptor(CreatedFd);
    return fail("O_CREAT did not preserve regular-node mode metadata", 18);
  }
  _close(CreatedFd);
  forgetOpenNodeDescriptor(CreatedFd);

  errno = 0;
  if (openNode(OpenCreatePath,
               DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenExclusive,
               0600) != -1 ||
      errno != EEXIST)
    return fail("O_CREAT|O_EXCL did not report EEXIST", 19);
  if (!removeNode(OpenCreatePath))
    return fail("open-created node removal failed", 20);

  errno = 0;
  if (createNode("/tmp/ipasim-directory", DarwinDirectory | 0755, 0) != -1 ||
      errno != EINVAL)
    return fail("mknod incorrectly accepted a directory node type", 21);

  if (ArgC != 2)
    return fail("expected IpaSimDarwinHost.dll path", 22);

  HMODULE DarwinHost = LoadLibraryA(ArgV[1]);
  if (!DarwinHost)
    return fail("could not load Darwin host bridge", 23);

  using Mkfifo = int (*)(const char *, std::uint16_t);
  using Mknod = int (*)(const char *, std::uint16_t, std::int32_t);
  using Open = int (*)(const char *, int, std::uint16_t);
  using GuardedOpen = int (*)(const char *, const std::uint64_t *,
                              std::uint32_t, int, int);
  using GuardedClose = int (*)(int, const std::uint64_t *);
  using Close = int (*)(int);
  using Fcntl = int (*)(int, int, std::intptr_t);
  using Write = std::intptr_t (*)(int, const void *, std::size_t);
  using Fstat = int (*)(int, void *);
  using PathStat = int (*)(const char *, void *);
  using Error = int *(*)();
  auto HostMkfifo = reinterpret_cast<Mkfifo>(GetProcAddress(DarwinHost, "mkfifo"));
  auto HostMknod = reinterpret_cast<Mknod>(GetProcAddress(DarwinHost, "mknod"));
  auto HostOpen = reinterpret_cast<Open>(GetProcAddress(DarwinHost, "open"));
  auto HostGuardedOpen = reinterpret_cast<GuardedOpen>(
      GetProcAddress(DarwinHost, "guarded_open_np"));
  auto HostGuardedClose = reinterpret_cast<GuardedClose>(
      GetProcAddress(DarwinHost, "guarded_close_np"));
  auto HostClose = reinterpret_cast<Close>(GetProcAddress(DarwinHost, "close"));
  auto HostFcntl = reinterpret_cast<Fcntl>(GetProcAddress(DarwinHost, "fcntl"));
  auto HostWrite = reinterpret_cast<Write>(GetProcAddress(DarwinHost, "write"));
  auto HostFstat = reinterpret_cast<Fstat>(GetProcAddress(DarwinHost, "fstat"));
  auto HostStat = reinterpret_cast<PathStat>(GetProcAddress(DarwinHost, "stat"));
  auto HostLstat = reinterpret_cast<PathStat>(GetProcAddress(DarwinHost, "lstat"));
  auto HostError = reinterpret_cast<Error>(GetProcAddress(DarwinHost, "__error"));
  if (!HostMkfifo || !HostMknod || !HostOpen || !HostGuardedOpen ||
      !HostGuardedClose || !HostClose || !HostFcntl || !HostWrite ||
      !HostFstat || !HostStat || !HostLstat || !HostError) {
    FreeLibrary(DarwinHost);
    return fail(!HostMkfifo        ? "mkfifo export was missing"
                : !HostMknod      ? "mknod export was missing"
                : !HostOpen       ? "open export was missing"
                : !HostGuardedOpen ? "guarded_open_np export was missing"
                : !HostGuardedClose ? "guarded_close_np export was missing"
                : !HostClose      ? "close export was missing"
                : !HostFcntl      ? "fcntl export was missing"
                : !HostWrite      ? "write export was missing"
                : !HostFstat      ? "fstat export was missing"
                : !HostStat       ? "stat export was missing"
                : !HostLstat      ? "lstat export was missing"
                                  : "__error export was missing",
                24);
  }

  constexpr const char *HostPath = "/tmp/ipasim-host-fifo";
  *HostError() = 0;
  if (HostMkfifo(HostPath, 0640) != 0) {
    FreeLibrary(DarwinHost);
    return fail("mkfifo export did not create a FIFO", 25);
  }

  // IpaSimDarwinHost.dll owns the guest-visible CRT errno storage. The smoke
  // executable has a separate CRT module/TLS errno slot, so inspect the same
  // Darwin __error() export that libsystem uses instead of reading this EXE's
  // unrelated errno value.
  *HostError() = 0;
  if (HostMkfifo(HostPath, 0640) != -1 || *HostError() != EEXIST) {
    FreeLibrary(DarwinHost);
    return fail("mkfifo export did not preserve guest-visible EEXIST semantics",
                26);
  }

  constexpr const char *HostNodePath = "/tmp/ipasim-host-character";
  *HostError() = 0;
  if (HostMknod(HostNodePath, DarwinCharacter | 0600, CharacterDevice) != 0) {
    FreeLibrary(DarwinHost);
    return fail("mknod export did not create a typed Darwin node", 27);
  }
  *HostError() = 0;
  if (HostMknod(HostNodePath, DarwinCharacter | 0600, CharacterDevice) != -1 ||
      *HostError() != EEXIST) {
    FreeLibrary(DarwinHost);
    return fail("mknod export did not preserve guest-visible EEXIST semantics",
                28);
  }

  constexpr const char *HostOpenPath = "/tmp/ipasim-host-open";
  *HostError() = 0;
  const int HostFd = HostOpen(
      HostOpenPath,
      DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenExclusive |
          DarwinOpenCloseOnExec,
      0600);
  if (HostFd < 0) {
    FreeLibrary(DarwinHost);
    return fail("open export did not create a usable CRT-backed descriptor", 29);
  }

  constexpr char StatPayload[] = "darwin-stat-metadata";
  if (HostWrite(HostFd, StatPayload, sizeof(StatPayload) - 1) !=
      static_cast<std::intptr_t>(sizeof(StatPayload) - 1)) {
    HostClose(HostFd);
    FreeLibrary(DarwinHost);
    return fail("write export did not populate the stat test file", 30);
  }

  DarwinStat64 ByFd{};
  DarwinStat64 ByPath{};
  DarwinStat64 ByLinkPath{};
  if (HostFstat(HostFd, &ByFd) != 0 || HostStat(HostOpenPath, &ByPath) != 0 ||
      HostLstat(HostOpenPath, &ByLinkPath) != 0) {
    HostClose(HostFd);
    FreeLibrary(DarwinHost);
    return fail("fstat/stat/lstat did not describe the guest regular file", 31);
  }

  const std::uint16_t ExpectedMode =
      static_cast<std::uint16_t>(DarwinRegular | 0600);
  if (ByFd.ModeBits != ExpectedMode || ByFd.LinkCount == 0 || ByFd.Inode == 0 ||
      ByFd.Size != static_cast<std::int64_t>(sizeof(StatPayload) - 1) ||
      ByFd.Blocks < 0 || ByFd.BlockSize <= 0 ||
      !plausibleTimespec(ByFd.AccessTime) ||
      !plausibleTimespec(ByFd.ModificationTime) ||
      !plausibleTimespec(ByFd.StatusChangeTime) ||
      !plausibleTimespec(ByFd.BirthTime)) {
    HostClose(HostFd);
    FreeLibrary(DarwinHost);
    return fail("fstat returned invalid Darwin LP64 metadata", 32);
  }

  if (ByPath.DeviceId != ByFd.DeviceId || ByPath.ModeBits != ByFd.ModeBits ||
      ByPath.LinkCount != ByFd.LinkCount || ByPath.Inode != ByFd.Inode ||
      ByPath.UserId != ByFd.UserId || ByPath.GroupId != ByFd.GroupId ||
      ByPath.Size != ByFd.Size || ByPath.Blocks != ByFd.Blocks ||
      ByPath.BlockSize != ByFd.BlockSize ||
      std::memcmp(&ByPath, &ByLinkPath, sizeof(ByPath)) != 0) {
    HostClose(HostFd);
    FreeLibrary(DarwinHost);
    return fail("stat/lstat metadata disagreed with fstat for one guest node", 33);
  }

  *HostError() = 0;
  DarwinStat64 Missing{};
  if (HostStat("/tmp/ipasim-stat-missing", &Missing) != -1 ||
      *HostError() != ENOENT) {
    HostClose(HostFd);
    FreeLibrary(DarwinHost);
    return fail("stat missing-path behavior did not report ENOENT", 34);
  }

  if (HostClose(HostFd) != 0) {
    FreeLibrary(DarwinHost);
    return fail("close export did not release the stat test descriptor", 35);
  }
  *HostError() = 0;
  if (HostFstat(HostFd, &ByFd) != -1 || *HostError() != EBADF) {
    FreeLibrary(DarwinHost);
    return fail("fstat did not observe descriptor lifetime after close", 36);
  }

  *HostError() = 0;
  if (HostOpen(HostOpenPath,
               DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenExclusive,
               0600) != -1 ||
      *HostError() != EEXIST) {
    FreeLibrary(DarwinHost);
    return fail("open export did not preserve guest-visible O_EXCL/EEXIST", 37);
  }

  // Apple libdispatch opens its internal dispatch_io descriptors with exactly
  // this guard set, then closes them through guarded_close_np with the same
  // 64-bit identity value. Prove the XNU contract rather than aliasing guarded
  // close to ordinary close.
  constexpr std::uint32_t GuardClose = 1u << 0;
  constexpr std::uint32_t GuardDup = 1u << 1;
  constexpr std::uint32_t GuardSocketIpc = 1u << 2;
  constexpr std::uint32_t GuardFileport = 1u << 3;
  constexpr std::uint32_t GuardWrite = 1u << 4;
  constexpr std::uint32_t DispatchGuardFlags =
      GuardClose | GuardDup | GuardSocketIpc | GuardFileport;
  constexpr std::uint64_t GuardValue = 0x1122334455667788ULL;
  constexpr const char *GuardedPath = "/tmp/ipasim-host-guarded-open";

  *HostError() = 0;
  if (HostGuardedOpen(
          GuardedPath, &GuardValue, DispatchGuardFlags,
          DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenExclusive, 0600) !=
          -1 ||
      *HostError() != EINVAL) {
    FreeLibrary(DarwinHost);
    return fail("guarded_open_np accepted a descriptor without O_CLOEXEC", 38);
  }

  constexpr std::uint64_t ZeroGuard = 0;
  *HostError() = 0;
  if (HostGuardedOpen(
          GuardedPath, &ZeroGuard, DispatchGuardFlags,
          DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenExclusive |
              DarwinOpenCloseOnExec,
          0600) != -1 ||
      *HostError() != EINVAL) {
    FreeLibrary(DarwinHost);
    return fail("guarded_open_np accepted a zero guard identity", 39);
  }

  *HostError() = 0;
  if (HostGuardedOpen(
          GuardedPath, nullptr, DispatchGuardFlags,
          DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenExclusive |
              DarwinOpenCloseOnExec,
          0600) != -1 ||
      *HostError() != EFAULT) {
    FreeLibrary(DarwinHost);
    return fail("guarded_open_np did not validate the guard pointer", 40);
  }

  *HostError() = 0;
  if (HostGuardedOpen(
          GuardedPath, &GuardValue, GuardClose,
          DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenExclusive |
              DarwinOpenCloseOnExec,
          0600) != -1 ||
      *HostError() != EINVAL) {
    FreeLibrary(DarwinHost);
    return fail("guarded_open_np accepted a guard set without GUARD_DUP", 41);
  }

  *HostError() = 0;
  if (HostGuardedOpen(
          GuardedPath, &GuardValue, DispatchGuardFlags | GuardWrite,
          DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenExclusive |
              DarwinOpenCloseOnExec,
          0600) != -1 ||
      *HostError() != ENOTSUP) {
    FreeLibrary(DarwinHost);
    return fail("guarded_open_np silently accepted unsupported GUARD_WRITE", 42);
  }

  *HostError() = 0;
  const int GuardedFd = HostGuardedOpen(
      GuardedPath, &GuardValue, DispatchGuardFlags,
      DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenExclusive |
          DarwinOpenCloseOnExec,
      0600);
  if (GuardedFd < 0) {
    FreeLibrary(DarwinHost);
    return fail("guarded_open_np could not create the libdispatch guard set", 43);
  }

  constexpr int DarwinFGetFd = 1;
  constexpr int DarwinFSetFd = 2;
  constexpr std::intptr_t DarwinFdCloExec = 1;
  *HostError() = 0;
  if (HostFcntl(GuardedFd, DarwinFGetFd, 0) != DarwinFdCloExec) {
    HostGuardedClose(GuardedFd, &GuardValue);
    FreeLibrary(DarwinHost);
    return fail("guarded descriptor was not forced close-on-exec", 44);
  }

  *HostError() = 0;
  if (HostFcntl(GuardedFd, DarwinFSetFd, 0) != -1 ||
      *HostError() != EPERM ||
      HostFcntl(GuardedFd, DarwinFGetFd, 0) != DarwinFdCloExec) {
    HostGuardedClose(GuardedFd, &GuardValue);
    FreeLibrary(DarwinHost);
    return fail("F_SETFD cleared close-on-exec on a guarded descriptor", 45);
  }

  constexpr char GuardedPayload[] = "darwin-guarded-fd";
  if (HostWrite(GuardedFd, GuardedPayload, sizeof(GuardedPayload) - 1) !=
      static_cast<std::intptr_t>(sizeof(GuardedPayload) - 1)) {
    HostGuardedClose(GuardedFd, &GuardValue);
    FreeLibrary(DarwinHost);
    return fail("guarded descriptor stopped normal file I/O", 46);
  }

  *HostError() = 0;
  if (HostClose(GuardedFd) != -1 || *HostError() != EPERM) {
    HostGuardedClose(GuardedFd, &GuardValue);
    FreeLibrary(DarwinHost);
    return fail("ordinary close bypassed GUARD_CLOSE", 47);
  }
  DarwinStat64 GuardedStat{};
  if (HostFstat(GuardedFd, &GuardedStat) != 0) {
    HostGuardedClose(GuardedFd, &GuardValue);
    FreeLibrary(DarwinHost);
    return fail("failed ordinary close destroyed guarded descriptor state", 48);
  }

  constexpr std::uint64_t WrongGuard = GuardValue ^ 0x55ULL;
  *HostError() = 0;
  if (HostGuardedClose(GuardedFd, &WrongGuard) != -1 ||
      *HostError() != EPERM || HostFstat(GuardedFd, &GuardedStat) != 0) {
    HostGuardedClose(GuardedFd, &GuardValue);
    FreeLibrary(DarwinHost);
    return fail("guarded_close_np accepted a mismatched guard identity", 49);
  }

  *HostError() = 0;
  if (HostGuardedClose(GuardedFd, nullptr) != -1 ||
      *HostError() != EFAULT || HostFstat(GuardedFd, &GuardedStat) != 0) {
    HostGuardedClose(GuardedFd, &GuardValue);
    FreeLibrary(DarwinHost);
    return fail("guarded_close_np did not validate the guard pointer", 50);
  }

  if (HostGuardedClose(GuardedFd, &GuardValue) != 0) {
    FreeLibrary(DarwinHost);
    return fail("matching guarded_close_np did not close the descriptor", 51);
  }
  *HostError() = 0;
  if (HostFstat(GuardedFd, &GuardedStat) != -1 || *HostError() != EBADF) {
    FreeLibrary(DarwinHost);
    return fail("guarded descriptor remained live after matching close", 52);
  }

  constexpr const char *UnguardedPath = "/tmp/ipasim-host-unguarded-control";
  const int UnguardedFd = HostOpen(
      UnguardedPath,
      DarwinOpenReadWrite | DarwinOpenCreate | DarwinOpenExclusive |
          DarwinOpenCloseOnExec,
      0600);
  if (UnguardedFd < 0) {
    FreeLibrary(DarwinHost);
    return fail("could not create unguarded control descriptor", 53);
  }
  *HostError() = 0;
  if (HostGuardedClose(UnguardedFd, &GuardValue) != -1 ||
      *HostError() != EINVAL || HostFstat(UnguardedFd, &GuardedStat) != 0) {
    HostClose(UnguardedFd);
    FreeLibrary(DarwinHost);
    return fail("guarded_close_np did not distinguish an unguarded descriptor",
                54);
  }
  if (HostClose(UnguardedFd) != 0) {
    FreeLibrary(DarwinHost);
    return fail("ordinary close stopped working for an unguarded descriptor",
                55);
  }

  *HostError() = 0;
  if (HostGuardedClose(0x7ffffffe, &GuardValue) != -1 ||
      *HostError() != EBADF) {
    FreeLibrary(DarwinHost);
    return fail("guarded_close_np invalid descriptor did not report EBADF", 56);
  }

  FreeLibrary(DarwinHost);
  std::printf("Darwin filesystem smoke passed: shared typed node registry, FIFO named-pipe transport, regular-file backing, device metadata, real CRT open descriptors, O_CREAT/O_EXCL/O_TRUNC/O_CLOEXEC translation, Darwin LP64 fstat/stat/lstat metadata, descriptor-lifetime cleanup, XNU guarded-open/guarded-close identity and close-policy enforcement, mkfifo/mknod/open bridge exports, removal, and guest-visible errno.\n");
  return 0;
}
