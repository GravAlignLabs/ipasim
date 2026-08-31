#include "FifoAdapter.hpp"

#include "DarwinCredentialAdapter.hpp"

#include <cstddef>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <windows.h>

namespace ipasim::darwinfs {
namespace {

constexpr DWORD PipeBufferSize = 64 * 1024;
constexpr std::int64_t WindowsToUnixEpoch100ns = 116444736000000000LL;
constexpr std::int64_t DarwinStatBlockBytes = 512;

// Darwin arm64 uses the LP64 __DARWIN_STRUCT_STAT64 layout from <sys/stat.h>.
// Keep every ABI width explicit; Windows' struct stat is neither layout- nor
// width-compatible with this guest structure.
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
static_assert(offsetof(DarwinStat64, DeviceId) == 0);
static_assert(offsetof(DarwinStat64, ModeBits) == 4);
static_assert(offsetof(DarwinStat64, Inode) == 8);
static_assert(offsetof(DarwinStat64, UserId) == 16);
static_assert(offsetof(DarwinStat64, SpecialDeviceId) == 24);
static_assert(offsetof(DarwinStat64, AccessTime) == 32);
static_assert(offsetof(DarwinStat64, Size) == 96);
static_assert(offsetof(DarwinStat64, Blocks) == 104);
static_assert(offsetof(DarwinStat64, BlockSize) == 112);
static_assert(offsetof(DarwinStat64, QuadSpare) == 128);
static_assert(sizeof(DarwinStat64) == 144);

struct NodeEntry {
  NodeInfo Info;
  HANDLE Keeper = INVALID_HANDLE_VALUE;

  ~NodeEntry() {
    if (Keeper != INVALID_HANDLE_VALUE)
      CloseHandle(Keeper);
    if (!Info.BackingPath.empty())
      DeleteFileW(Info.BackingPath.c_str());
  }
};

struct NodeNamespace {
  std::mutex Mutex;
  std::unordered_map<std::string, std::unique_ptr<NodeEntry>> Entries;
  std::unordered_map<int, NodeInfo> OpenDescriptors;
  std::uint64_t NextObjectId = 1;
};

NodeNamespace &nodeNamespace() {
  static NodeNamespace Namespace;
  return Namespace;
}

bool normalizeGuestPath(const char *Path, std::string &Normalized) {
  if (!Path) {
    errno = EFAULT;
    return false;
  }

  const std::string Input(Path);
  if (Input.empty()) {
    errno = ENOENT;
    return false;
  }

  const bool Absolute = Input.front() == '/';
  const bool TrailingSlash = Input.size() > 1 && Input.back() == '/';
  std::vector<std::string> Components;

  std::size_t Begin = Absolute ? 1 : 0;
  while (Begin <= Input.size()) {
    const std::size_t End = Input.find('/', Begin);
    const std::size_t Count =
        End == std::string::npos ? Input.size() - Begin : End - Begin;
    const std::string Component = Input.substr(Begin, Count);

    if (!Component.empty() && Component != ".") {
      if (Component == "..") {
        if (!Components.empty() && Components.back() != "..")
          Components.pop_back();
        else if (!Absolute)
          Components.push_back(Component);
      } else {
        Components.push_back(Component);
      }
    }

    if (End == std::string::npos)
      break;
    Begin = End + 1;
  }

  Normalized = Absolute ? "/" : "";
  for (std::size_t I = 0; I < Components.size(); ++I) {
    if (!Normalized.empty() && Normalized.back() != '/')
      Normalized.push_back('/');
    Normalized += Components[I];
  }

  if (Normalized.empty())
    Normalized = Absolute ? "/" : ".";

  // mknod/mkfifo create non-directory namespace objects. A trailing slash
  // therefore cannot be silently accepted as the same pathname.
  if (TrailingSlash) {
    errno = ENOENT;
    return false;
  }

  return true;
}

std::wstring makePipeName(std::uint64_t Id) {
  return L"\\\\.\\pipe\\ipasim-fifo-" +
         std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId())) +
         L"-" + std::to_wstring(Id);
}

void setErrnoFromWinError(DWORD Error) {
  switch (Error) {
  case ERROR_ACCESS_DENIED:
    errno = EACCES;
    break;
  case ERROR_ALREADY_EXISTS:
  case ERROR_FILE_EXISTS:
    errno = EEXIST;
    break;
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
    errno = ENOENT;
    break;
  case ERROR_INVALID_HANDLE:
    errno = EBADF;
    break;
  case ERROR_INVALID_NAME:
  case ERROR_INVALID_PARAMETER:
    errno = EINVAL;
    break;
  case ERROR_NOT_SUPPORTED:
  case ERROR_INVALID_FUNCTION:
    errno = ENOTSUP;
    break;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:
    errno = ENOMEM;
    break;
  case ERROR_PIPE_BUSY:
    errno = EBUSY;
    break;
  default:
    errno = EIO;
    break;
  }
}

bool createRegularBacking(std::uint64_t Id, std::wstring &BackingPath) {
  wchar_t TempPath[MAX_PATH + 1] = {};
  const DWORD Length = GetTempPathW(MAX_PATH, TempPath);
  if (Length == 0 || Length >= MAX_PATH) {
    setErrnoFromWinError(Length == 0 ? GetLastError() : ERROR_INVALID_NAME);
    return false;
  }

  std::wstring Directory(TempPath, Length);
  if (!Directory.empty() && Directory.back() != L'\\')
    Directory.push_back(L'\\');
  Directory += L"ipasim-darwinfs-" +
               std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId()));

  if (!CreateDirectoryW(Directory.c_str(), nullptr) &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    setErrnoFromWinError(GetLastError());
    return false;
  }

  BackingPath = Directory + L"\\node-" + std::to_wstring(Id) + L".dat";
  HANDLE File = CreateFileW(
      BackingPath.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (File == INVALID_HANDLE_VALUE) {
    setErrnoFromWinError(GetLastError());
    BackingPath.clear();
    return false;
  }
  CloseHandle(File);
  return true;
}

bool decodeNodeType(Mode ModeBits, NodeType &Type) {
  switch (static_cast<Mode>(ModeBits & DarwinTypeMask)) {
  case DarwinFifo:
    Type = NodeType::Fifo;
    return true;
  case DarwinCharacter:
    Type = NodeType::CharacterDevice;
    return true;
  case DarwinBlock:
    Type = NodeType::BlockDevice;
    return true;
  case DarwinRegular:
    Type = NodeType::Regular;
    return true;
  case DarwinSocket:
    Type = NodeType::Socket;
    return true;
  default:
    // Directories and symlinks have their own Darwin creation syscalls. A
    // missing/unknown type is not reinterpreted as a regular file.
    errno = EINVAL;
    return false;
  }
}

Mode nodeTypeMode(NodeType Type) {
  switch (Type) {
  case NodeType::Fifo:
    return DarwinFifo;
  case NodeType::CharacterDevice:
    return DarwinCharacter;
  case NodeType::BlockDevice:
    return DarwinBlock;
  case NodeType::Regular:
    return DarwinRegular;
  case NodeType::Socket:
    return DarwinSocket;
  }
  return 0;
}

bool convertWindowsTime(std::int64_t Ticks, DarwinTimespec64 &Out) {
  if (Ticks < WindowsToUnixEpoch100ns) {
    errno = EOVERFLOW;
    return false;
  }
  const std::int64_t UnixTicks = Ticks - WindowsToUnixEpoch100ns;
  Out.Seconds = UnixTicks / 10000000LL;
  Out.Nanoseconds = (UnixTicks % 10000000LL) * 100LL;
  return true;
}

bool fillRegularStat(HANDLE Handle, const NodeInfo &Node, void *Buffer) {
  if (!Buffer) {
    errno = EFAULT;
    return false;
  }
  if (Node.Type != NodeType::Regular) {
    errno = ENOTSUP;
    return false;
  }

  FILE_BASIC_INFO Basic{};
  FILE_STANDARD_INFO Standard{};
  FILE_STORAGE_INFO Storage{};
  BY_HANDLE_FILE_INFORMATION Identity{};
  if (!GetFileInformationByHandleEx(Handle, FileBasicInfo, &Basic,
                                    sizeof(Basic)) ||
      !GetFileInformationByHandleEx(Handle, FileStandardInfo, &Standard,
                                    sizeof(Standard)) ||
      !GetFileInformationByHandleEx(Handle, FileStorageInfo, &Storage,
                                    sizeof(Storage)) ||
      !GetFileInformationByHandle(Handle, &Identity)) {
    setErrnoFromWinError(GetLastError());
    return false;
  }

  if (Standard.EndOfFile.QuadPart < 0 || Standard.AllocationSize.QuadPart < 0 ||
      Standard.NumberOfLinks >
          static_cast<DWORD>((std::numeric_limits<std::uint16_t>::max)())) {
    errno = EOVERFLOW;
    return false;
  }

  const DWORD HostBlockSize =
      Storage.PhysicalBytesPerSectorForPerformance != 0
          ? Storage.PhysicalBytesPerSectorForPerformance
          : Storage.LogicalBytesPerSector;
  if (HostBlockSize == 0 ||
      HostBlockSize >
          static_cast<DWORD>((std::numeric_limits<std::int32_t>::max)())) {
    errno = EOVERFLOW;
    return false;
  }

  std::uint32_t Uid = 0;
  std::uint32_t Gid = 0;
  if (!darwincred::queryUserRid(false, Uid) ||
      !darwincred::queryPrimaryGroupRid(false, Gid)) {
    errno = EIO;
    return false;
  }

  DarwinStat64 Result{};
  Result.DeviceId = static_cast<std::int32_t>(Identity.dwVolumeSerialNumber);
  Result.ModeBits = static_cast<std::uint16_t>(
      nodeTypeMode(Node.Type) | (Node.Permissions & DarwinPermissionMask));
  Result.LinkCount = static_cast<std::uint16_t>(Standard.NumberOfLinks);
  Result.Inode = (static_cast<std::uint64_t>(Identity.nFileIndexHigh) << 32) |
                 static_cast<std::uint64_t>(Identity.nFileIndexLow);
  Result.UserId = Uid;
  Result.GroupId = Gid;
  Result.SpecialDeviceId = 0;
  if (!convertWindowsTime(Basic.LastAccessTime.QuadPart, Result.AccessTime) ||
      !convertWindowsTime(Basic.LastWriteTime.QuadPart,
                          Result.ModificationTime) ||
      !convertWindowsTime(Basic.ChangeTime.QuadPart, Result.StatusChangeTime) ||
      !convertWindowsTime(Basic.CreationTime.QuadPart, Result.BirthTime))
    return false;

  Result.Size = Standard.EndOfFile.QuadPart;
  const std::int64_t Allocation = Standard.AllocationSize.QuadPart;
  Result.Blocks = Allocation / DarwinStatBlockBytes +
                  (Allocation % DarwinStatBlockBytes != 0 ? 1 : 0);
  Result.BlockSize = static_cast<std::int32_t>(HostBlockSize);
  // The virtual namespace does not implement BSD file flags or generation
  // numbers. Zero therefore means no flags/generation are present, not an
  // invented Windows attribute translation.
  Result.Flags = 0;
  Result.Generation = 0;

  std::memcpy(Buffer, &Result, sizeof(Result));
  return true;
}

int openRegularBacking(const NodeInfo &Node, int Flags) {
  int NativeFlags = _O_BINARY;
  switch (Flags & DarwinOpenAccessMask) {
  case DarwinOpenReadOnly:
    NativeFlags |= _O_RDONLY;
    break;
  case DarwinOpenWriteOnly:
    NativeFlags |= _O_WRONLY;
    break;
  case DarwinOpenReadWrite:
    NativeFlags |= _O_RDWR;
    break;
  default:
    errno = EINVAL;
    return -1;
  }

  if (Flags & DarwinOpenAppend)
    NativeFlags |= _O_APPEND;
  if (Flags & DarwinOpenTruncate)
    NativeFlags |= _O_TRUNC;
  if (Flags & DarwinOpenCloseOnExec)
    NativeFlags |= _O_NOINHERIT;

  // The namespace already owns creation/existence. _wopen only materializes a
  // CRT descriptor over the private backing file so close/fcntl/lseek share the
  // same descriptor model as the rest of the Darwin host bridge.
  return _wopen(Node.BackingPath.c_str(), NativeFlags);
}

void rememberOpenDescriptor(int Descriptor, const NodeInfo &Node) {
  if (Descriptor < 0)
    return;
  NodeNamespace &Namespace = nodeNamespace();
  std::lock_guard<std::mutex> Guard(Namespace.Mutex);
  Namespace.OpenDescriptors[Descriptor] = Node;
}

} // namespace

int createNode(const char *Path, Mode ModeBits, Device DeviceNumber) {
  std::string GuestPath;
  if (!normalizeGuestPath(Path, GuestPath))
    return -1;

  if (GuestPath == "/" || GuestPath == ".") {
    errno = EEXIST;
    return -1;
  }

  NodeType Type;
  if (!decodeNodeType(ModeBits, Type))
    return -1;

  NodeNamespace &Namespace = nodeNamespace();
  std::lock_guard<std::mutex> Guard(Namespace.Mutex);

  if (Namespace.Entries.count(GuestPath) != 0) {
    errno = EEXIST;
    return -1;
  }

  const std::uint64_t ObjectId = Namespace.NextObjectId++;
  auto Entry = std::make_unique<NodeEntry>();
  Entry->Info.GuestPath = GuestPath;
  Entry->Info.Type = Type;
  Entry->Info.Permissions = static_cast<Mode>(ModeBits & DarwinPermissionMask);
  Entry->Info.DeviceNumber =
      (Type == NodeType::CharacterDevice || Type == NodeType::BlockDevice)
          ? DeviceNumber
          : 0;

  if (Type == NodeType::Fifo) {
    Entry->Info.PipeName = makePipeName(ObjectId);
    Entry->Keeper = CreateNamedPipeW(
        Entry->Info.PipeName.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, PipeBufferSize, PipeBufferSize, 0, nullptr);
    if (Entry->Keeper == INVALID_HANDLE_VALUE) {
      setErrnoFromWinError(GetLastError());
      return -1;
    }
  } else if (Type == NodeType::Regular) {
    if (!createRegularBacking(ObjectId, Entry->Info.BackingPath))
      return -1;
  }

  // Character/block devices and socket nodes are real entries in the Darwin
  // namespace even though their I/O dispatch is not a filesystem byte stream.
  // Their type and dev_t identity are preserved here for future open/ioctl or
  // socket dispatch instead of fabricating an NTFS file with the wrong type.
  Namespace.Entries.emplace(GuestPath, std::move(Entry));
  return 0;
}

bool lookupNode(const char *Path, NodeInfo &Out) {
  std::string GuestPath;
  if (!normalizeGuestPath(Path, GuestPath))
    return false;

  NodeNamespace &Namespace = nodeNamespace();
  std::lock_guard<std::mutex> Guard(Namespace.Mutex);
  const auto It = Namespace.Entries.find(GuestPath);
  if (It == Namespace.Entries.end()) {
    errno = ENOENT;
    return false;
  }
  Out = It->second->Info;
  return true;
}

bool removeNode(const char *Path) {
  std::string GuestPath;
  if (!normalizeGuestPath(Path, GuestPath))
    return false;

  NodeNamespace &Namespace = nodeNamespace();
  std::lock_guard<std::mutex> Guard(Namespace.Mutex);
  return Namespace.Entries.erase(GuestPath) != 0;
}

int openNode(const char *Path, int Flags, Mode CreateMode) {
  constexpr int SupportedFlags =
      DarwinOpenAccessMask | DarwinOpenNonBlock | DarwinOpenAppend |
      DarwinOpenNoFollow | DarwinOpenCreate | DarwinOpenTruncate |
      DarwinOpenExclusive | DarwinOpenNoControllingTty |
      DarwinOpenDirectory | DarwinOpenCloseOnExec | DarwinOpenNoFollowAny;

  if ((Flags & ~SupportedFlags) != 0) {
    errno = ENOTSUP;
    return -1;
  }
  if ((Flags & DarwinOpenAccessMask) == DarwinOpenAccessMask) {
    errno = EINVAL;
    return -1;
  }

  NodeInfo Node;
  bool Exists = lookupNode(Path, Node);
  if (Exists && (Flags & DarwinOpenCreate) &&
      (Flags & DarwinOpenExclusive)) {
    errno = EEXIST;
    return -1;
  }

  if (!Exists) {
    if ((Flags & DarwinOpenCreate) == 0) {
      errno = ENOENT;
      return -1;
    }

    const Mode ModeBits = static_cast<Mode>(
        DarwinRegular | (CreateMode & DarwinPermissionMask));
    if (createNode(Path, ModeBits, 0) != 0) {
      // Another thread may have won the create race. O_EXCL must retain the
      // EEXIST result; a normal O_CREAT retries the lookup and opens the winner.
      if (errno != EEXIST || (Flags & DarwinOpenExclusive))
        return -1;
    }
    if (!lookupNode(Path, Node)) {
      if (errno == 0)
        errno = ENOENT;
      return -1;
    }
  }

  if (Flags & DarwinOpenDirectory) {
    // Directory nodes are deliberately not synthesized by this special-node
    // registry. Do not convert a non-directory object into a successful open.
    errno = ENOTDIR;
    return -1;
  }

  switch (Node.Type) {
  case NodeType::Regular: {
    const int Descriptor = openRegularBacking(Node, Flags);
    rememberOpenDescriptor(Descriptor, Node);
    return Descriptor;
  }
  case NodeType::Fifo:
    // The namespace and real named-pipe transport exist, but POSIX FIFO open
    // requires reader/writer rendezvous plus blocking/O_NONBLOCK descriptor
    // state. Fail explicitly until the target proves that execution boundary.
    errno = ENOTSUP;
    return -1;
  case NodeType::CharacterDevice:
  case NodeType::BlockDevice:
  case NodeType::Socket:
    // These nodes have preserved type/dev identity but no byte-stream dispatch
    // yet. ENXIO exposes that missing device/socket endpoint without pretending
    // the node is an ordinary file.
    errno = ENXIO;
    return -1;
  }

  errno = ENOTSUP;
  return -1;
}

std::vector<int> listOpenNodeDescriptors() {
  NodeNamespace &Namespace = nodeNamespace();
  std::lock_guard<std::mutex> Guard(Namespace.Mutex);
  std::vector<int> Result;
  Result.reserve(Namespace.OpenDescriptors.size());
  for (const auto &Entry : Namespace.OpenDescriptors)
    Result.push_back(Entry.first);
  return Result;
}

bool isOpenNodeDescriptor(int Descriptor) {
  NodeNamespace &Namespace = nodeNamespace();
  std::lock_guard<std::mutex> Guard(Namespace.Mutex);
  return Namespace.OpenDescriptors.count(Descriptor) != 0;
}

bool lookupOpenNodeDescriptor(int Descriptor, NodeInfo &Out) {
  NodeNamespace &Namespace = nodeNamespace();
  std::lock_guard<std::mutex> Guard(Namespace.Mutex);
  const auto It = Namespace.OpenDescriptors.find(Descriptor);
  if (It == Namespace.OpenDescriptors.end()) {
    errno = EBADF;
    return false;
  }
  Out = It->second;
  return true;
}

void forgetOpenNodeDescriptor(int Descriptor) {
  NodeNamespace &Namespace = nodeNamespace();
  std::lock_guard<std::mutex> Guard(Namespace.Mutex);
  Namespace.OpenDescriptors.erase(Descriptor);
}

int createFifo(const char *Path, Mode Permissions) {
  return createNode(Path,
                    static_cast<Mode>(DarwinFifo |
                                      (Permissions & DarwinPermissionMask)),
                    0);
}

bool lookupFifo(const char *Path, FifoInfo &Out) {
  NodeInfo Node;
  if (!lookupNode(Path, Node) || Node.Type != NodeType::Fifo)
    return false;
  Out.GuestPath = Node.GuestPath;
  Out.PipeName = Node.PipeName;
  Out.Permissions = Node.Permissions;
  return true;
}

bool removeFifo(const char *Path) {
  NodeInfo Node;
  if (!lookupNode(Path, Node) || Node.Type != NodeType::Fifo)
    return false;
  return removeNode(Path);
}

int statDescriptor(int Descriptor, void *Buffer) {
  NodeInfo Node;
  if (!lookupOpenNodeDescriptor(Descriptor, Node))
    return -1;

  const std::intptr_t Native = _get_osfhandle(Descriptor);
  if (Native == -1) {
    errno = EBADF;
    return -1;
  }
  return fillRegularStat(reinterpret_cast<HANDLE>(Native), Node, Buffer) ? 0
                                                                         : -1;
}

int statPath(const char *Path, void *Buffer) {
  if (!Buffer) {
    errno = EFAULT;
    return -1;
  }

  NodeInfo Node;
  if (!lookupNode(Path, Node))
    return -1;
  if (Node.Type != NodeType::Regular || Node.BackingPath.empty()) {
    errno = ENOTSUP;
    return -1;
  }

  HANDLE Handle = CreateFileW(
      Node.BackingPath.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (Handle == INVALID_HANDLE_VALUE) {
    setErrnoFromWinError(GetLastError());
    return -1;
  }

  const bool Ok = fillRegularStat(Handle, Node, Buffer);
  const DWORD CloseError = CloseHandle(Handle) ? ERROR_SUCCESS : GetLastError();
  if (!Ok)
    return -1;
  if (CloseError != ERROR_SUCCESS) {
    setErrnoFromWinError(CloseError);
    return -1;
  }
  return 0;
}

} // namespace ipasim::darwinfs

extern "C" {

// Darwin fstat/stat/lstat use the 144-byte LP64 stat ABI. The current guest
// namespace has no symlink node type, so stat and lstat are intentionally
// identical for the regular nodes it can represent. When symlink creation is
// implemented, lstat must stop following that new node type while stat follows
// it; until then there is no symlink behavior to fabricate.
int darwin_fstat(int Descriptor, void *Buffer) {
  return ipasim::darwinfs::statDescriptor(Descriptor, Buffer);
}

int darwin_stat(const char *Path, void *Buffer) {
  return ipasim::darwinfs::statPath(Path, Buffer);
}

int darwin_lstat(const char *Path, void *Buffer) {
  return ipasim::darwinfs::statPath(Path, Buffer);
}

} // extern "C"
