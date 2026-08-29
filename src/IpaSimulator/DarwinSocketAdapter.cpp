#include "DarwinSocketAdapter.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace ipasim::darwinsock {
namespace {

constexpr int DarwinMsgOob = 0x0001;
constexpr int DarwinMsgDontRoute = 0x0004;
constexpr int DarwinMsgNoSignal = 0x00080000;
constexpr int SupportedDarwinSendFlags =
    DarwinMsgOob | DarwinMsgDontRoute | DarwinMsgNoSignal;

constexpr int FirstSocketDescriptor = 0x40000000;

#pragma pack(push, 1)
struct DarwinSockaddrHeader {
  std::uint8_t Length;
  std::uint8_t Family;
};

struct DarwinSockaddrIn {
  std::uint8_t Length;
  std::uint8_t Family;
  std::uint16_t Port;
  std::uint32_t Address;
  std::uint8_t Zero[8];
};

struct DarwinSockaddrIn6 {
  std::uint8_t Length;
  std::uint8_t Family;
  std::uint16_t Port;
  std::uint32_t FlowInfo;
  std::uint8_t Address[16];
  std::uint32_t ScopeId;
};
#pragma pack(pop)

static_assert(sizeof(DarwinSockaddrIn) == 16,
              "Darwin sockaddr_in ABI layout changed unexpectedly");
static_assert(sizeof(DarwinSockaddrIn6) == 28,
              "Darwin sockaddr_in6 ABI layout changed unexpectedly");

struct SocketState {
  explicit SocketState(SOCKET Value) : Handle(Value) {}

  std::mutex OperationMutex;
  SOCKET Handle = INVALID_SOCKET;
};

std::once_flag WinsockOnce;
int WinsockStartupResult = WSANOTINITIALISED;
std::mutex RegistryMutex;
std::unordered_map<int, std::shared_ptr<SocketState>> Registry;
int NextSocketDescriptor = FirstSocketDescriptor;

void setErrnoFromWinsock(int Error) {
  switch (Error) {
  case WSAEACCES:
    errno = EACCES;
    break;
  case WSAEADDRINUSE:
    errno = EADDRINUSE;
    break;
  case WSAEADDRNOTAVAIL:
    errno = EADDRNOTAVAIL;
    break;
  case WSAEAFNOSUPPORT:
    errno = EAFNOSUPPORT;
    break;
  case WSAECONNABORTED:
    errno = ECONNABORTED;
    break;
  case WSAECONNREFUSED:
    errno = ECONNREFUSED;
    break;
  case WSAECONNRESET:
    errno = ECONNRESET;
    break;
  case WSAEDESTADDRREQ:
    errno = EDESTADDRREQ;
    break;
  case WSAEFAULT:
    errno = EFAULT;
    break;
  case WSAEHOSTUNREACH:
    errno = EHOSTUNREACH;
    break;
  case WSAEINPROGRESS:
    errno = EINPROGRESS;
    break;
  case WSAEINTR:
    errno = EINTR;
    break;
  case WSAEINVAL:
    errno = EINVAL;
    break;
  case WSAEISCONN:
    errno = EISCONN;
    break;
  case WSAEMSGSIZE:
    errno = EMSGSIZE;
    break;
  case WSAENETDOWN:
    errno = ENETDOWN;
    break;
  case WSAENETRESET:
    errno = ENETRESET;
    break;
  case WSAENETUNREACH:
    errno = ENETUNREACH;
    break;
  case WSAENOBUFS:
    errno = ENOBUFS;
    break;
  case WSAENOTCONN:
    errno = ENOTCONN;
    break;
  case WSAENOTSOCK:
    errno = ENOTSOCK;
    break;
  case WSAEOPNOTSUPP:
    errno = EOPNOTSUPP;
    break;
  case WSAEPROTONOSUPPORT:
    errno = EPROTONOSUPPORT;
    break;
  case WSAEPROTOTYPE:
    errno = EPROTOTYPE;
    break;
  case WSAETIMEDOUT:
    errno = ETIMEDOUT;
    break;
  case WSAEWOULDBLOCK:
    errno = EWOULDBLOCK;
    break;
  default:
    errno = EIO;
    break;
  }
}

bool ensureWinsock() {
  std::call_once(WinsockOnce, []() {
    WSADATA Data{};
    WinsockStartupResult = WSAStartup(MAKEWORD(2, 2), &Data);
  });
  if (WinsockStartupResult == 0)
    return true;
  setErrnoFromWinsock(WinsockStartupResult);
  return false;
}

int allocateDescriptorLocked() {
  // Socket descriptors live in a high positive range so they cannot alias the
  // CRT descriptors used by the Darwin filesystem adapter. Wrap safely if a
  // very long process ever consumes the range.
  for (std::uint64_t Attempt = 0;
       Attempt <= static_cast<std::uint64_t>(INT_MAX - FirstSocketDescriptor);
       ++Attempt) {
    const int Candidate = NextSocketDescriptor;
    if (NextSocketDescriptor == INT_MAX)
      NextSocketDescriptor = FirstSocketDescriptor;
    else
      ++NextSocketDescriptor;
    if (Registry.find(Candidate) == Registry.end())
      return Candidate;
  }
  errno = EMFILE;
  return -1;
}

std::shared_ptr<SocketState> lookupSocket(int Descriptor) {
  std::lock_guard<std::mutex> Guard(RegistryMutex);
  const auto It = Registry.find(Descriptor);
  return It == Registry.end() ? nullptr : It->second;
}

int translateDomain(int Domain) {
  switch (Domain) {
  case DarwinAfInet:
    return AF_INET;
  case DarwinAfInet6:
    return AF_INET6;
  default:
    errno = EAFNOSUPPORT;
    return -1;
  }
}

int translateType(int Type) {
  switch (Type) {
  case DarwinSockStream:
    return SOCK_STREAM;
  case DarwinSockDgram:
    return SOCK_DGRAM;
  case DarwinSockRaw:
    return SOCK_RAW;
  default:
    errno = EPROTOTYPE;
    return -1;
  }
}

bool translateSendFlags(int DarwinFlags, int &WindowsFlags) {
  if ((DarwinFlags & ~SupportedDarwinSendFlags) != 0) {
    errno = EOPNOTSUPP;
    return false;
  }

  WindowsFlags = 0;
  if ((DarwinFlags & DarwinMsgOob) != 0)
    WindowsFlags |= MSG_OOB;
  if ((DarwinFlags & DarwinMsgDontRoute) != 0)
    WindowsFlags |= MSG_DONTROUTE;
  // Windows sockets do not generate POSIX SIGPIPE, so Darwin MSG_NOSIGNAL is
  // already satisfied and intentionally contributes no Winsock flag.
  return true;
}

bool translateAddress(const void *Address, std::uint32_t AddressLength,
                      sockaddr_storage &Storage, int &StorageLength) {
  if (!Address) {
    if (AddressLength != 0) {
      errno = EFAULT;
      return false;
    }
    StorageLength = 0;
    return true;
  }
  if (AddressLength < sizeof(DarwinSockaddrHeader)) {
    errno = EINVAL;
    return false;
  }

  DarwinSockaddrHeader Header{};
  std::memcpy(&Header, Address, sizeof(Header));
  if (Header.Length != 0 && Header.Length > AddressLength) {
    errno = EINVAL;
    return false;
  }

  std::memset(&Storage, 0, sizeof(Storage));
  switch (Header.Family) {
  case DarwinAfInet: {
    if (AddressLength < sizeof(DarwinSockaddrIn) ||
        (Header.Length != 0 && Header.Length < sizeof(DarwinSockaddrIn))) {
      errno = EINVAL;
      return false;
    }
    DarwinSockaddrIn Darwin{};
    std::memcpy(&Darwin, Address, sizeof(Darwin));
    auto *Windows = reinterpret_cast<sockaddr_in *>(&Storage);
    Windows->sin_family = AF_INET;
    Windows->sin_port = Darwin.Port;
    Windows->sin_addr.s_addr = Darwin.Address;
    StorageLength = sizeof(sockaddr_in);
    return true;
  }

  case DarwinAfInet6: {
    if (AddressLength < sizeof(DarwinSockaddrIn6) ||
        (Header.Length != 0 && Header.Length < sizeof(DarwinSockaddrIn6))) {
      errno = EINVAL;
      return false;
    }
    DarwinSockaddrIn6 Darwin{};
    std::memcpy(&Darwin, Address, sizeof(Darwin));
    auto *Windows = reinterpret_cast<sockaddr_in6 *>(&Storage);
    Windows->sin6_family = AF_INET6;
    Windows->sin6_port = Darwin.Port;
    Windows->sin6_flowinfo = Darwin.FlowInfo;
    std::memcpy(&Windows->sin6_addr, Darwin.Address, sizeof(Darwin.Address));
    Windows->sin6_scope_id = Darwin.ScopeId;
    StorageLength = sizeof(sockaddr_in6);
    return true;
  }

  default:
    errno = EAFNOSUPPORT;
    return false;
  }
}

} // namespace

int createSocket(int Domain, int Type, int Protocol) {
  if (!ensureWinsock())
    return -1;

  const int WindowsDomain = translateDomain(Domain);
  if (WindowsDomain == -1)
    return -1;
  const int WindowsType = translateType(Type);
  if (WindowsType == -1)
    return -1;

  const SOCKET Handle = ::socket(WindowsDomain, WindowsType, Protocol);
  if (Handle == INVALID_SOCKET) {
    setErrnoFromWinsock(WSAGetLastError());
    return -1;
  }

  std::lock_guard<std::mutex> Guard(RegistryMutex);
  const int Descriptor = allocateDescriptorLocked();
  if (Descriptor == -1) {
    ::closesocket(Handle);
    return -1;
  }
  Registry.emplace(Descriptor, std::make_shared<SocketState>(Handle));
  return Descriptor;
}

int closeSocket(int Descriptor) {
  std::shared_ptr<SocketState> State;
  {
    std::lock_guard<std::mutex> Guard(RegistryMutex);
    const auto It = Registry.find(Descriptor);
    if (It == Registry.end()) {
      errno = EBADF;
      return -1;
    }
    State = It->second;
    Registry.erase(It);
  }

  std::lock_guard<std::mutex> OperationGuard(State->OperationMutex);
  const SOCKET Handle = State->Handle;
  State->Handle = INVALID_SOCKET;
  if (Handle == INVALID_SOCKET) {
    errno = EBADF;
    return -1;
  }
  if (::closesocket(Handle) == SOCKET_ERROR) {
    setErrnoFromWinsock(WSAGetLastError());
    return -1;
  }
  return 0;
}

bool isSocketDescriptor(int Descriptor) {
  std::lock_guard<std::mutex> Guard(RegistryMutex);
  return Registry.find(Descriptor) != Registry.end();
}

std::vector<int> listDescriptors() {
  std::lock_guard<std::mutex> Guard(RegistryMutex);
  std::vector<int> Result;
  Result.reserve(Registry.size());
  for (const auto &Entry : Registry)
    Result.push_back(Entry.first);
  std::sort(Result.begin(), Result.end());
  return Result;
}

int connectSocket(int Descriptor, const void *Address,
                  std::uint32_t AddressLength) {
  const std::shared_ptr<SocketState> State = lookupSocket(Descriptor);
  if (!State) {
    errno = EBADF;
    return -1;
  }
  if (!Address) {
    errno = EFAULT;
    return -1;
  }

  sockaddr_storage NativeAddress{};
  int NativeAddressLength = 0;
  if (!translateAddress(Address, AddressLength, NativeAddress,
                        NativeAddressLength))
    return -1;
  if (NativeAddressLength == 0) {
    errno = EINVAL;
    return -1;
  }

  std::lock_guard<std::mutex> OperationGuard(State->OperationMutex);
  if (State->Handle == INVALID_SOCKET) {
    errno = EBADF;
    return -1;
  }

  if (::connect(State->Handle,
                reinterpret_cast<const sockaddr *>(&NativeAddress),
                NativeAddressLength) == SOCKET_ERROR) {
    setErrnoFromWinsock(WSAGetLastError());
    return -1;
  }
  return 0;
}

std::intptr_t sendTo(int Descriptor, const void *Buffer, std::size_t Length,
                     int Flags, const void *Address,
                     std::uint32_t AddressLength) {
  if (!Buffer && Length != 0) {
    errno = EFAULT;
    return -1;
  }

  const std::shared_ptr<SocketState> State = lookupSocket(Descriptor);
  if (!State) {
    errno = EBADF;
    return -1;
  }

  int WindowsFlags = 0;
  if (!translateSendFlags(Flags, WindowsFlags))
    return -1;

  sockaddr_storage NativeAddress{};
  int NativeAddressLength = 0;
  if (!translateAddress(Address, AddressLength, NativeAddress,
                        NativeAddressLength))
    return -1;

  const int HostLength = static_cast<int>(
      (std::min)(Length, static_cast<std::size_t>(INT_MAX)));

  std::lock_guard<std::mutex> OperationGuard(State->OperationMutex);
  if (State->Handle == INVALID_SOCKET) {
    errno = EBADF;
    return -1;
  }

  const int Result = NativeAddressLength == 0
                         ? ::send(State->Handle,
                                  static_cast<const char *>(Buffer), HostLength,
                                  WindowsFlags)
                         : ::sendto(
                               State->Handle,
                               static_cast<const char *>(Buffer), HostLength,
                               WindowsFlags,
                               reinterpret_cast<const sockaddr *>(&NativeAddress),
                               NativeAddressLength);
  if (Result == SOCKET_ERROR) {
    setErrnoFromWinsock(WSAGetLastError());
    return -1;
  }
  return static_cast<std::intptr_t>(Result);
}

} // namespace ipasim::darwinsock
