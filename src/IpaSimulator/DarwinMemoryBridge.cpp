// DarwinMemoryBridge.cpp: Apple libplatform memory/string primitives exposed
// through the native Windows host bridge used by the iOS Simulator runtime.
//
// These functions preserve the public Darwin C contracts. They are not loader
// success stubs: each operation performs the corresponding byte/string work on
// the guest-visible host-backed memory mapped by ipaSim.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

template <std::size_t PatternSize>
void fillPattern(void *Destination, const void *Pattern, std::size_t Length) {
  auto *Out = static_cast<unsigned char *>(Destination);
  const auto *Bytes = static_cast<const unsigned char *>(Pattern);

  std::size_t Offset = 0;
  while (Length - Offset >= PatternSize) {
    std::memmove(Out + Offset, Bytes, PatternSize);
    Offset += PatternSize;
  }
  if (Offset != Length)
    std::memmove(Out + Offset, Bytes, Length - Offset);
}

std::size_t boundedStringLength(const char *String, std::size_t Maximum) {
  std::size_t Length = 0;
  while (Length < Maximum && String[Length] != '\0')
    ++Length;
  return Length;
}

} // namespace

extern "C" {

// Apple libplatform's generic implementation defines _platform_bzero exactly
// as _platform_memset(s, 0, n). Keep the direct zero-fill contract here.
__declspec(dllexport) void _platform_bzero(void *Storage, std::size_t Size) {
  std::memset(Storage, 0, Size);
}

// libsystem_platform also exports the C __bzero entry point (Mach-O
// ___bzero). libc's public bzero re-export can alias to this provider symbol.
__declspec(dllexport) void __bzero(void *Storage, std::size_t Size) {
  _platform_bzero(Storage, Size);
}

__declspec(dllexport) void *_platform_memset(void *Storage, int Value,
                                             std::size_t Size) {
  return std::memset(Storage, Value, Size);
}

__declspec(dllexport) void *_platform_memchr(const void *Storage, int Value,
                                             std::size_t Size) {
  return const_cast<void *>(std::memchr(Storage, Value, Size));
}

__declspec(dllexport) int _platform_memcmp(const void *Left, const void *Right,
                                           std::size_t Size) {
  return std::memcmp(Left, Right, Size);
}

// Darwin LP64 returns unsigned long. The observable contract is strictly 0 for
// an all-zero aligned range and 1 otherwise; zero length is also zero.
__declspec(dllexport) std::uint64_t
_platform_memcmp_zero_aligned8(const void *Storage, std::size_t Size) {
  const auto *Bytes = static_cast<const unsigned char *>(Storage);
  for (std::size_t Index = 0; Index < Size; ++Index) {
    if (Bytes[Index] != 0)
      return 1;
  }
  return 0;
}

__declspec(dllexport) void *_platform_memccpy(void *Destination,
                                              const void *Source, int Value,
                                              std::size_t Size) {
  if (Size == 0)
    return nullptr;

  const void *Last = std::memchr(Source, Value, Size);
  if (!Last) {
    std::memmove(Destination, Source, Size);
    return nullptr;
  }

  const auto Bytes = static_cast<std::size_t>(
      static_cast<const unsigned char *>(Last) -
      static_cast<const unsigned char *>(Source) + 1);
  std::memmove(Destination, Source, Bytes);
  return static_cast<unsigned char *>(Destination) + Bytes;
}

__declspec(dllexport) void
_platform_memset_pattern4(void *Destination, const void *Pattern,
                          std::size_t Size) {
  fillPattern<4>(Destination, Pattern, Size);
}

__declspec(dllexport) void
_platform_memset_pattern8(void *Destination, const void *Pattern,
                          std::size_t Size) {
  fillPattern<8>(Destination, Pattern, Size);
}

__declspec(dllexport) void
_platform_memset_pattern16(void *Destination, const void *Pattern,
                           std::size_t Size) {
  fillPattern<16>(Destination, Pattern, Size);
}

__declspec(dllexport) char *_platform_strcpy(char *Destination,
                                             const char *Source) {
  return std::strcpy(Destination, Source);
}

__declspec(dllexport) std::size_t _platform_strnlen(const char *String,
                                                    std::size_t Maximum) {
  return boundedStringLength(String, Maximum);
}

__declspec(dllexport) std::size_t _platform_strlcat(char *Destination,
                                                    const char *Source,
                                                    std::size_t Maximum) {
  const std::size_t SourceLength = std::strlen(Source);
  const std::size_t DestinationLength =
      boundedStringLength(Destination, Maximum);

  if (DestinationLength == Maximum)
    return Maximum + SourceLength;

  const std::size_t Remaining = Maximum - DestinationLength;
  if (SourceLength < Remaining) {
    std::memmove(Destination + DestinationLength, Source, SourceLength + 1);
  } else if (Remaining != 0) {
    if (Remaining > 1)
      std::memmove(Destination + DestinationLength, Source, Remaining - 1);
    Destination[Maximum - 1] = '\0';
  }

  return DestinationLength + SourceLength;
}

__declspec(dllexport) char *_platform_strncpy(char *Destination,
                                              const char *Source,
                                              std::size_t Maximum) {
  return std::strncpy(Destination, Source, Maximum);
}

__declspec(dllexport) char *_platform_strstr(const char *Haystack,
                                             const char *Needle) {
  return const_cast<char *>(std::strstr(Haystack, Needle));
}

} // extern "C"
