#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ipasim::darwinsock {

// Darwin address-family values are ABI values, not Windows AF_* constants.
constexpr int DarwinAfInet = 2;
constexpr int DarwinAfInet6 = 30;

constexpr int DarwinSockStream = 1;
constexpr int DarwinSockDgram = 2;
constexpr int DarwinSockRaw = 3;

// Allocate a Darwin integer descriptor backed by a real Winsock socket. This is
// kept internal until the target reaches the socket(2) boundary; it exists now
// so socket operations have a real descriptor subsystem rather than treating a
// Win64 SOCKET (pointer-width) as a Darwin int.
int createSocket(int Domain, int Type, int Protocol);

// Close a descriptor owned by this socket namespace. Returns 0 on success and
// -1 with errno set on failure, matching Darwin socket conventions.
int closeSocket(int Descriptor);

// Query descriptor ownership without mutating the registry. Generic close/write
// dispatch uses this so a Darwin socket fd never falls through to UCRT _close or
// _write, whose descriptor namespace is unrelated to Win64 SOCKET handles.
bool isSocketDescriptor(int Descriptor);

// Return a stable snapshot of the Darwin integer descriptors currently owned by
// the Winsock-backed registry. proc_pidinfo(PROC_PIDLISTFDS) consumes this same
// namespace instead of inventing unrelated Windows HANDLE values.
std::vector<int> listDescriptors();

// Darwin connect(2) ABI. Address points to a Darwin sockaddr layout (one-byte
// length followed by one-byte family), which is translated before Winsock sees
// it. The descriptor remains a Darwin 32-bit fd backed by the adapter registry.
int connectSocket(int Descriptor, const void *Address,
                  std::uint32_t AddressLength);

// Darwin sendto(2) ABI. Address points to a Darwin sockaddr layout (one-byte
// length followed by one-byte family), so implementations must translate it
// before calling Winsock rather than reinterpret_casting it as sockaddr.
std::intptr_t sendTo(int Descriptor, const void *Buffer, std::size_t Length,
                     int Flags, const void *Address,
                     std::uint32_t AddressLength);

} // namespace ipasim::darwinsock
