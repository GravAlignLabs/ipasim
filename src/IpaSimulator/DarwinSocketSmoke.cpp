#include "DarwinSocketAdapter.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

#pragma pack(push, 1)
struct DarwinSockaddrIn {
  std::uint8_t Length;
  std::uint8_t Family;
  std::uint16_t Port;
  std::uint32_t Address;
  std::uint8_t Zero[8];
};
#pragma pack(pop)

static_assert(sizeof(DarwinSockaddrIn) == 16,
              "Darwin sockaddr_in ABI layout changed unexpectedly");

int fail(const char *Message) {
  std::fprintf(stderr, "[darwin-socket-smoke] error: %s\n", Message);
  return 1;
}

DarwinSockaddrIn makeDarwinLoopback(std::uint16_t NetworkPort) {
  DarwinSockaddrIn Address{};
  Address.Length = sizeof(Address);
  Address.Family = ipasim::darwinsock::DarwinAfInet;
  Address.Port = NetworkPort;
  Address.Address = htonl(INADDR_LOOPBACK);
  return Address;
}

bool sidTerminalRid(PSID Sid, std::uint32_t &Rid) {
  if (!Sid || !IsValidSid(Sid))
    return false;
  PUCHAR Count = GetSidSubAuthorityCount(Sid);
  if (!Count || *Count == 0)
    return false;
  PDWORD Value = GetSidSubAuthority(Sid, static_cast<DWORD>(*Count - 1));
  if (!Value)
    return false;
  Rid = static_cast<std::uint32_t>(*Value);
  return true;
}

bool queryProcessTokenRid(TOKEN_INFORMATION_CLASS Class,
                          std::uint32_t &Rid) {
  HANDLE Token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    return false;

  DWORD Required = 0;
  GetTokenInformation(Token, Class, nullptr, 0, &Required);
  if (Required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    CloseHandle(Token);
    return false;
  }

  std::vector<std::uint8_t> Storage(Required);
  if (!GetTokenInformation(Token, Class, Storage.data(), Required, &Required)) {
    CloseHandle(Token);
    return false;
  }
  CloseHandle(Token);

  if (Class == TokenUser) {
    auto *User = reinterpret_cast<TOKEN_USER *>(Storage.data());
    return sidTerminalRid(User->User.Sid, Rid);
  }
  if (Class == TokenPrimaryGroup) {
    auto *Group = reinterpret_cast<TOKEN_PRIMARY_GROUP *>(Storage.data());
    return sidTerminalRid(Group->PrimaryGroup, Rid);
  }
  return false;
}

} // namespace

int main(int ArgC, char **ArgV) {
  using namespace ipasim::darwinsock;

  if (ArgC != 2)
    return fail("expected IpaSimDarwinHost.dll path");

  // First prove connect(2) against a real Winsock TCP listener. createSocket
  // initializes Winsock and gives the Darwin side a real 32-bit descriptor
  // backed by a pointer-width Windows SOCKET in the adapter registry.
  const int Client = createSocket(DarwinAfInet, DarwinSockStream, IPPROTO_TCP);
  if (Client == -1)
    return fail("could not create Darwin TCP client");

  SOCKET Listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (Listener == INVALID_SOCKET) {
    closeSocket(Client);
    return fail("could not create Winsock TCP listener");
  }

  sockaddr_in ListenerAddress{};
  ListenerAddress.sin_family = AF_INET;
  ListenerAddress.sin_port = 0;
  ListenerAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(Listener, reinterpret_cast<const sockaddr *>(&ListenerAddress),
             sizeof(ListenerAddress)) == SOCKET_ERROR ||
      ::listen(Listener, 1) == SOCKET_ERROR) {
    ::closesocket(Listener);
    closeSocket(Client);
    return fail("could not bind/listen on loopback TCP socket");
  }

  int ListenerAddressLength = sizeof(ListenerAddress);
  if (::getsockname(Listener, reinterpret_cast<sockaddr *>(&ListenerAddress),
                    &ListenerAddressLength) == SOCKET_ERROR) {
    ::closesocket(Listener);
    closeSocket(Client);
    return fail("could not query loopback TCP listener address");
  }

  const DarwinSockaddrIn DarwinTcpAddress =
      makeDarwinLoopback(ListenerAddress.sin_port);
  if (connectSocket(Client, &DarwinTcpAddress, sizeof(DarwinTcpAddress)) != 0) {
    ::closesocket(Listener);
    closeSocket(Client);
    return fail("Darwin connect did not reach the loopback TCP listener");
  }

  SOCKET Accepted = ::accept(Listener, nullptr, nullptr);
  if (Accepted == INVALID_SOCKET) {
    ::closesocket(Listener);
    closeSocket(Client);
    return fail("loopback TCP listener did not accept Darwin connection");
  }

  constexpr char TcpPayload[] = "ipasim-darwin-connect";
  const std::intptr_t TcpSent =
      sendTo(Client, TcpPayload, sizeof(TcpPayload) - 1, 0, nullptr, 0);
  char TcpReceived[64] = {};
  const int TcpReceivedBytes =
      ::recv(Accepted, TcpReceived, sizeof(TcpReceived), 0);
  if (TcpSent != static_cast<std::intptr_t>(sizeof(TcpPayload) - 1) ||
      TcpReceivedBytes != static_cast<int>(sizeof(TcpPayload) - 1) ||
      std::memcmp(TcpReceived, TcpPayload, sizeof(TcpPayload) - 1) != 0) {
    ::closesocket(Accepted);
    ::closesocket(Listener);
    closeSocket(Client);
    return fail("connected Darwin TCP socket did not carry payload");
  }

  // Prove the receive side over the same Darwin descriptor. A real Winsock peer
  // sends bytes in the opposite direction; receive() must return the exact short
  // read result and then report orderly stream shutdown as EOF (0).
  constexpr char ReversePayload[] = "ipasim-darwin-receive";
  const int ReverseSent = ::send(
      Accepted, ReversePayload, static_cast<int>(sizeof(ReversePayload) - 1), 0);
  if (ReverseSent != static_cast<int>(sizeof(ReversePayload) - 1)) {
    ::closesocket(Accepted);
    ::closesocket(Listener);
    closeSocket(Client);
    return fail("Winsock peer could not send reverse-direction TCP payload");
  }

  char ReverseReceived[64] = {};
  const std::intptr_t ReverseReceivedBytes =
      receive(Client, ReverseReceived, sizeof(ReverseReceived));
  if (ReverseReceivedBytes !=
          static_cast<std::intptr_t>(sizeof(ReversePayload) - 1) ||
      std::memcmp(ReverseReceived, ReversePayload,
                  sizeof(ReversePayload) - 1) != 0) {
    ::closesocket(Accepted);
    ::closesocket(Listener);
    closeSocket(Client);
    return fail("Darwin receive did not return the reverse-direction TCP payload");
  }

  errno = 0;
  if (receive(Client, nullptr, 0) != 0) {
    ::closesocket(Accepted);
    ::closesocket(Listener);
    closeSocket(Client);
    return fail("zero-length Darwin receive did not preserve null-buffer success");
  }

  errno = 0;
  if (receive(Client, nullptr, 1) != -1 || errno != EFAULT) {
    ::closesocket(Accepted);
    ::closesocket(Listener);
    closeSocket(Client);
    return fail("Darwin receive null/nonzero buffer did not report EFAULT");
  }

  char InvalidBuffer = 0;
  errno = 0;
  if (receive(0x7ffffffe, &InvalidBuffer, 1) != -1 || errno != EBADF) {
    ::closesocket(Accepted);
    ::closesocket(Listener);
    closeSocket(Client);
    return fail("Darwin receive invalid descriptor did not report EBADF");
  }

  if (::shutdown(Accepted, SD_SEND) == SOCKET_ERROR) {
    ::closesocket(Accepted);
    ::closesocket(Listener);
    closeSocket(Client);
    return fail("could not orderly-shutdown Winsock peer send side");
  }
  char EofSentinel = 'Z';
  if (receive(Client, &EofSentinel, 1) != 0 || EofSentinel != 'Z') {
    ::closesocket(Accepted);
    ::closesocket(Listener);
    closeSocket(Client);
    return fail("Darwin receive did not report orderly TCP shutdown as EOF");
  }

  ::closesocket(Accepted);
  ::closesocket(Listener);
  if (closeSocket(Client) != 0)
    return fail("Darwin TCP descriptor did not close cleanly");

  // Preserve the existing real IPv4 UDP sendto coverage.
  const int Sender = createSocket(DarwinAfInet, DarwinSockDgram, IPPROTO_UDP);
  if (Sender == -1)
    return fail("could not create Darwin UDP sender");

  SOCKET Receiver = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (Receiver == INVALID_SOCKET) {
    closeSocket(Sender);
    return fail("could not create Winsock UDP receiver");
  }

  DWORD TimeoutMs = 2000;
  if (::setsockopt(Receiver, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&TimeoutMs),
                   sizeof(TimeoutMs)) == SOCKET_ERROR) {
    ::closesocket(Receiver);
    closeSocket(Sender);
    return fail("could not set receiver timeout");
  }

  sockaddr_in ReceiverAddress{};
  ReceiverAddress.sin_family = AF_INET;
  ReceiverAddress.sin_port = 0;
  ReceiverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(Receiver, reinterpret_cast<const sockaddr *>(&ReceiverAddress),
             sizeof(ReceiverAddress)) == SOCKET_ERROR) {
    ::closesocket(Receiver);
    closeSocket(Sender);
    return fail("could not bind loopback UDP receiver");
  }

  int ReceiverAddressLength = sizeof(ReceiverAddress);
  if (::getsockname(Receiver, reinterpret_cast<sockaddr *>(&ReceiverAddress),
                    &ReceiverAddressLength) == SOCKET_ERROR) {
    ::closesocket(Receiver);
    closeSocket(Sender);
    return fail("could not query loopback UDP receiver address");
  }

  const DarwinSockaddrIn DarwinUdpAddress =
      makeDarwinLoopback(ReceiverAddress.sin_port);

  constexpr char Payload[] = "ipasim-darwin-sendto";
  const std::intptr_t Sent = sendTo(Sender, Payload, sizeof(Payload) - 1, 0,
                                    &DarwinUdpAddress,
                                    sizeof(DarwinUdpAddress));
  if (Sent != static_cast<std::intptr_t>(sizeof(Payload) - 1)) {
    ::closesocket(Receiver);
    closeSocket(Sender);
    return fail("Darwin sendto did not transmit the complete UDP datagram");
  }

  char Received[64] = {};
  const int ReceivedBytes =
      ::recvfrom(Receiver, Received, sizeof(Received), 0, nullptr, nullptr);
  if (ReceivedBytes != static_cast<int>(sizeof(Payload) - 1) ||
      std::memcmp(Received, Payload, sizeof(Payload) - 1) != 0) {
    ::closesocket(Receiver);
    closeSocket(Sender);
    return fail("loopback receiver did not receive the Darwin sendto payload");
  }

  if (closeSocket(Sender) != 0) {
    ::closesocket(Receiver);
    return fail("Darwin socket descriptor did not close cleanly");
  }
  ::closesocket(Receiver);

  // Validate the exact PE names DynamicLoader sees after removing one Mach-O
  // underscore. In particular, Mach-O _connect must bind to PE connect, not to
  // an incorrectly preserved _connect export.
  HMODULE DarwinHost = LoadLibraryA(ArgV[1]);
  if (!DarwinHost)
    return fail("could not load Darwin host bridge");

  using SocketFunction = int (*)(int, int, int);
  using Connect = int (*)(int, const void *, std::uint32_t);
  using CloseFunction = int (*)(int);
  using ReadFunction = std::intptr_t (*)(int, void *, std::size_t);
  using Identity = std::uint32_t (*)();
  using SendTo = std::intptr_t (*)(int, const void *, std::size_t, int,
                                   const void *, std::uint32_t);
  using DarwinError = int *(*)();
  auto HostSocket =
      reinterpret_cast<SocketFunction>(GetProcAddress(DarwinHost, "socket"));
  auto HostConnect =
      reinterpret_cast<Connect>(GetProcAddress(DarwinHost, "connect"));
  auto HostClose =
      reinterpret_cast<CloseFunction>(GetProcAddress(DarwinHost, "close"));
  auto HostRead =
      reinterpret_cast<ReadFunction>(GetProcAddress(DarwinHost, "read"));
  auto WrongHostConnect = GetProcAddress(DarwinHost, "_connect");
  auto HostGetUid = reinterpret_cast<Identity>(GetProcAddress(DarwinHost, "getuid"));
  auto HostGetEuid = reinterpret_cast<Identity>(GetProcAddress(DarwinHost, "geteuid"));
  auto HostGetGid = reinterpret_cast<Identity>(GetProcAddress(DarwinHost, "getgid"));
  auto HostGetEgid = reinterpret_cast<Identity>(GetProcAddress(DarwinHost, "getegid"));
  auto HostSendTo = reinterpret_cast<SendTo>(
      GetProcAddress(DarwinHost, "__sendto"));
  auto HostError =
      reinterpret_cast<DarwinError>(GetProcAddress(DarwinHost, "__error"));
  if (!HostSocket || !HostConnect || !HostClose || !HostRead ||
      WrongHostConnect || !HostGetUid || !HostGetEuid || !HostGetGid ||
      !HostGetEgid || !HostSendTo || !HostError) {
    FreeLibrary(DarwinHost);
    return fail("normalized socket/credential PE exports were incorrect");
  }

  std::uint32_t ExpectedUid = 0;
  std::uint32_t ExpectedGid = 0;
  if (!queryProcessTokenRid(TokenUser, ExpectedUid) ||
      !queryProcessTokenRid(TokenPrimaryGroup, ExpectedGid)) {
    FreeLibrary(DarwinHost);
    return fail("could not independently derive Windows token identity");
  }
  if (HostGetUid() != ExpectedUid || HostGetEuid() != ExpectedUid ||
      HostGetGid() != ExpectedGid || HostGetEgid() != ExpectedGid) {
    FreeLibrary(DarwinHost);
    return fail("Darwin credential exports did not match Windows token SIDs");
  }

  // The DLL has its own descriptor registry, so prove the exported read provider
  // through DLL-owned socket/connect/close entry points rather than reusing the
  // smoke executable's adapter registry. This is the exact provider generated
  // routing will invoke after semantic approval.
  int HostReadClient = HostSocket(DarwinAfInet, DarwinSockStream, IPPROTO_TCP);
  if (HostReadClient == -1) {
    FreeLibrary(DarwinHost);
    return fail("Darwin host bridge could not create read-provider socket");
  }

  SOCKET HostReadListener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (HostReadListener == INVALID_SOCKET) {
    HostClose(HostReadClient);
    FreeLibrary(DarwinHost);
    return fail("could not create read-provider loopback listener");
  }

  sockaddr_in HostReadListenerAddress{};
  HostReadListenerAddress.sin_family = AF_INET;
  HostReadListenerAddress.sin_port = 0;
  HostReadListenerAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(HostReadListener,
             reinterpret_cast<const sockaddr *>(&HostReadListenerAddress),
             sizeof(HostReadListenerAddress)) == SOCKET_ERROR ||
      ::listen(HostReadListener, 1) == SOCKET_ERROR) {
    ::closesocket(HostReadListener);
    HostClose(HostReadClient);
    FreeLibrary(DarwinHost);
    return fail("could not bind/listen for exported Darwin read provider");
  }

  int HostReadListenerAddressLength = sizeof(HostReadListenerAddress);
  if (::getsockname(
          HostReadListener,
          reinterpret_cast<sockaddr *>(&HostReadListenerAddress),
          &HostReadListenerAddressLength) == SOCKET_ERROR) {
    ::closesocket(HostReadListener);
    HostClose(HostReadClient);
    FreeLibrary(DarwinHost);
    return fail("could not query exported read-provider listener address");
  }

  const DarwinSockaddrIn HostReadDarwinAddress =
      makeDarwinLoopback(HostReadListenerAddress.sin_port);
  if (HostConnect(HostReadClient, &HostReadDarwinAddress,
                  sizeof(HostReadDarwinAddress)) != 0) {
    ::closesocket(HostReadListener);
    HostClose(HostReadClient);
    FreeLibrary(DarwinHost);
    return fail("exported Darwin socket/connect could not establish read fixture");
  }

  SOCKET HostReadPeer = ::accept(HostReadListener, nullptr, nullptr);
  if (HostReadPeer == INVALID_SOCKET) {
    ::closesocket(HostReadListener);
    HostClose(HostReadClient);
    FreeLibrary(DarwinHost);
    return fail("read-provider loopback listener did not accept connection");
  }

  constexpr char HostReadPayload[] = "ipasim-host-read-provider";
  if (::send(HostReadPeer, HostReadPayload,
             static_cast<int>(sizeof(HostReadPayload) - 1), 0) !=
      static_cast<int>(sizeof(HostReadPayload) - 1)) {
    ::closesocket(HostReadPeer);
    ::closesocket(HostReadListener);
    HostClose(HostReadClient);
    FreeLibrary(DarwinHost);
    return fail("Winsock peer could not feed exported Darwin read provider");
  }

  if (HostRead(HostReadClient, nullptr, 0) != 0) {
    ::closesocket(HostReadPeer);
    ::closesocket(HostReadListener);
    HostClose(HostReadClient);
    FreeLibrary(DarwinHost);
    return fail("exported Darwin read changed zero-length/null behavior");
  }

  char HostReadBuffer[64] = {};
  const std::intptr_t HostReadBytes =
      HostRead(HostReadClient, HostReadBuffer, sizeof(HostReadBuffer));
  if (HostReadBytes !=
          static_cast<std::intptr_t>(sizeof(HostReadPayload) - 1) ||
      std::memcmp(HostReadBuffer, HostReadPayload,
                  sizeof(HostReadPayload) - 1) != 0) {
    ::closesocket(HostReadPeer);
    ::closesocket(HostReadListener);
    HostClose(HostReadClient);
    FreeLibrary(DarwinHost);
    return fail("exported Darwin read did not receive real TCP payload");
  }

  if (::shutdown(HostReadPeer, SD_SEND) == SOCKET_ERROR) {
    ::closesocket(HostReadPeer);
    ::closesocket(HostReadListener);
    HostClose(HostReadClient);
    FreeLibrary(DarwinHost);
    return fail("could not orderly-shutdown exported read-provider peer");
  }
  char HostReadEofSentinel = 'R';
  if (HostRead(HostReadClient, &HostReadEofSentinel, 1) != 0 ||
      HostReadEofSentinel != 'R') {
    ::closesocket(HostReadPeer);
    ::closesocket(HostReadListener);
    HostClose(HostReadClient);
    FreeLibrary(DarwinHost);
    return fail("exported Darwin read did not preserve socket EOF semantics");
  }

  ::closesocket(HostReadPeer);
  ::closesocket(HostReadListener);
  if (HostClose(HostReadClient) != 0) {
    FreeLibrary(DarwinHost);
    return fail("exported Darwin close did not release read-provider socket");
  }

  char HostInvalidReadBuffer = 0;
  *HostError() = 0;
  if (HostRead(0x7ffffffe, &HostInvalidReadBuffer, 1) != -1 ||
      *HostError() != EBADF) {
    FreeLibrary(DarwinHost);
    return fail("exported Darwin read invalid-descriptor errno contract changed");
  }

  *HostError() = 0;
  if (HostConnect(0x7ffffffe, &DarwinTcpAddress,
                  sizeof(DarwinTcpAddress)) != -1 ||
      *HostError() != EBADF) {
    FreeLibrary(DarwinHost);
    return fail("connect invalid-descriptor errno contract changed");
  }

  *HostError() = 0;
  if (HostSendTo(0x7ffffffe, nullptr, 0, 0, nullptr, 0) != -1 ||
      *HostError() != EBADF) {
    FreeLibrary(DarwinHost);
    return fail("__sendto invalid-descriptor errno contract changed");
  }

  FreeLibrary(DarwinHost);
  std::printf("Darwin socket + credential smoke passed: normalized PE exports, Windows token identity, real loopback TCP send/receive, exported read semantics, and UDP traffic validated.\n");
  return 0;
}
