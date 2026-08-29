// DarwinSocketBridge.cpp: target-proven Darwin socket ABI entry points plus
// small normalized libc host calls that share the native bridge DLL.
//
// Mach-O symbol normalization removes one leading underscore before PE lookup.
// ___sendto therefore resolves as __sendto, while _socket/_connect resolve as
// socket/connect. Those names are exported through DarwinHostBridge.def aliases
// so they never collide with Winsock's source-level declarations.

#include "DarwinCredentialAdapter.hpp"
#include "DarwinSocketAdapter.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int darwin_socket(int Domain, int Type, int Protocol) {
  return ipasim::darwinsock::createSocket(Domain, Type, Protocol);
}

extern "C" int darwin_connect(int SocketDescriptor, const void *Address,
                              std::uint32_t AddressLength) {
  return ipasim::darwinsock::connectSocket(SocketDescriptor, Address,
                                           AddressLength);
}

extern "C" std::uint32_t darwin_getuid() {
  return ipasim::darwincred::getuid();
}

extern "C" std::uint32_t darwin_geteuid() {
  return ipasim::darwincred::geteuid();
}

extern "C" std::uint32_t darwin_getgid() {
  return ipasim::darwincred::getgid();
}

extern "C" std::uint32_t darwin_getegid() {
  return ipasim::darwincred::getegid();
}

extern "C" __declspec(dllexport) std::intptr_t
__sendto(int SocketDescriptor, const void *Buffer, std::size_t Length, int Flags,
         const void *Address, std::uint32_t AddressLength) {
  return ipasim::darwinsock::sendTo(SocketDescriptor, Buffer, Length, Flags,
                                    Address, AddressLength);
}
