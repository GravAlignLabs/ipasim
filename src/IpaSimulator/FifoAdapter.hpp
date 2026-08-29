#ifndef IPASIM_FIFO_ADAPTER_HPP
#define IPASIM_FIFO_ADAPTER_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace ipasim::darwinfs {

// Darwin ABI widths on modern Apple targets.
using Mode = std::uint16_t;
using Device = std::int32_t;

// Darwin <sys/stat.h> file-type bits. Keep these target values explicit rather
// than depending on the Windows CRT's stat constants, which are not ABI-equal.
constexpr Mode DarwinTypeMask = 0170000;
constexpr Mode DarwinFifo = 0010000;
constexpr Mode DarwinCharacter = 0020000;
constexpr Mode DarwinDirectory = 0040000;
constexpr Mode DarwinBlock = 0060000;
constexpr Mode DarwinRegular = 0100000;
constexpr Mode DarwinSymlink = 0120000;
constexpr Mode DarwinSocket = 0140000;
constexpr Mode DarwinPermissionMask = 07777;

// Darwin open(2) flags from <sys/fcntl.h>. These are intentionally not mapped
// by numeric passthrough because Windows CRT flag values are different.
constexpr int DarwinOpenReadOnly = 0x00000000;
constexpr int DarwinOpenWriteOnly = 0x00000001;
constexpr int DarwinOpenReadWrite = 0x00000002;
constexpr int DarwinOpenAccessMask = 0x00000003;
constexpr int DarwinOpenNonBlock = 0x00000004;
constexpr int DarwinOpenAppend = 0x00000008;
constexpr int DarwinOpenNoFollow = 0x00000100;
constexpr int DarwinOpenCreate = 0x00000200;
constexpr int DarwinOpenTruncate = 0x00000400;
constexpr int DarwinOpenExclusive = 0x00000800;
constexpr int DarwinOpenNoControllingTty = 0x00020000;
constexpr int DarwinOpenDirectory = 0x00100000;
constexpr int DarwinOpenCloseOnExec = 0x01000000;
constexpr int DarwinOpenNoFollowAny = 0x20000000;

enum class NodeType {
  Fifo,
  CharacterDevice,
  BlockDevice,
  Regular,
  Socket,
};

struct NodeInfo {
  std::string GuestPath;
  NodeType Type = NodeType::Regular;
  Mode Permissions = 0;
  Device DeviceNumber = 0;
  // FIFO transport and regular-file storage remain host-private implementation
  // details. Guest code sees only the Darwin pathname/type/metadata.
  std::wstring PipeName;
  std::wstring BackingPath;
};

struct FifoInfo {
  std::string GuestPath;
  std::wstring PipeName;
  Mode Permissions = 0;
};

// Create one Darwin namespace node with mknod(2) semantics for the node classes
// represented by the adapter. Character/block device numbers are retained for
// later open/ioctl dispatch; FIFO and regular nodes receive concrete Windows
// backing immediately. Returns 0 on success and -1 with thread-local errno on
// failure.
int createNode(const char *Path, Mode ModeBits, Device DeviceNumber);
bool lookupNode(const char *Path, NodeInfo &Out);
bool removeNode(const char *Path);

// Open a node from the same Darwin namespace. Regular nodes return real CRT
// descriptors so the existing close/fcntl/lseek bridge functions operate on
// the same descriptor lifetime. O_CREAT creates a regular node in this
// namespace. Unsupported special-node I/O fails explicitly until target-proven
// semantics are added rather than being silently represented as a normal file.
int openNode(const char *Path, int Flags, Mode CreateMode);

// Only descriptors returned through the Darwin filesystem boundary belong in
// proc_pidinfo's guest-visible descriptor list. Do not discover descriptors by
// probing every possible UCRT fd: invalid _get_osfhandle inputs enter the UCRT
// invalid-parameter path and can fast-fail the process. Keep explicit lifetime
// state instead, which also moves this adapter toward one coherent guest FD
// namespace for later file/socket unification.
std::vector<int> listOpenNodeDescriptors();
bool isOpenNodeDescriptor(int Descriptor);
void forgetOpenNodeDescriptor(int Descriptor);

// mkfifo(2) is the FIFO-specific frontend over the same node registry.
int createFifo(const char *Path, Mode Permissions);
bool lookupFifo(const char *Path, FifoInfo &Out);
bool removeFifo(const char *Path);

} // namespace ipasim::darwinfs

#endif // IPASIM_FIFO_ADAPTER_HPP
