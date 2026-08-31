// DarwinMemorySmoke.cpp: semantic checks for the native libplatform memory and
// string primitives exposed by IpaSimDarwinHost.dll.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace {

int fail(const char *Message) {
  std::fprintf(stderr, "[darwin-memory-smoke] FAIL: %s\n", Message);
  return 1;
}

template <typename Function>
Function load(HMODULE Module, const char *Name) {
  return reinterpret_cast<Function>(GetProcAddress(Module, Name));
}

bool bytesEqual(const unsigned char *Left, const unsigned char *Right,
                std::size_t Size) {
  return std::memcmp(Left, Right, Size) == 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return fail("expected path to IpaSimDarwinHost.dll");

  HMODULE Host = LoadLibraryA(argv[1]);
  if (!Host)
    return fail("could not load IpaSimDarwinHost.dll");

  using Bzero = void (*)(void *, std::size_t);
  using Memset = void *(*)(void *, int, std::size_t);
  using Memchr = void *(*)(const void *, int, std::size_t);
  using Memcmp = int (*)(const void *, const void *, std::size_t);
  using MemcmpZero = std::uint64_t (*)(const void *, std::size_t);
  using Memccpy = void *(*)(void *, const void *, int, std::size_t);
  using Pattern = void (*)(void *, const void *, std::size_t);
  using Strcpy = char *(*)(char *, const char *);
  using Strlcat = std::size_t (*)(char *, const char *, std::size_t);
  using Strncpy = char *(*)(char *, const char *, std::size_t);
  using Strnlen = std::size_t (*)(const char *, std::size_t);
  using Strstr = char *(*)(const char *, const char *);

  const Bzero PlatformBzero = load<Bzero>(Host, "_platform_bzero");
  const Bzero DoubleBzero = load<Bzero>(Host, "__bzero");
  const Memset PlatformMemset = load<Memset>(Host, "_platform_memset");
  const Memchr PlatformMemchr = load<Memchr>(Host, "_platform_memchr");
  const Memcmp PlatformMemcmp = load<Memcmp>(Host, "_platform_memcmp");
  const MemcmpZero PlatformMemcmpZero =
      load<MemcmpZero>(Host, "_platform_memcmp_zero_aligned8");
  const MemcmpZero InterposedMemcmpZero = load<MemcmpZero>(
      Host, "__interposition_sim_system__platform_memcmp_zero_aligned8");
  const Memccpy PlatformMemccpy = load<Memccpy>(Host, "_platform_memccpy");
  const Pattern Pattern4 = load<Pattern>(Host, "_platform_memset_pattern4");
  const Pattern Pattern8 = load<Pattern>(Host, "_platform_memset_pattern8");
  const Pattern Pattern16 = load<Pattern>(Host, "_platform_memset_pattern16");
  const Strcpy PlatformStrcpy = load<Strcpy>(Host, "_platform_strcpy");
  const Strlcat PlatformStrlcat = load<Strlcat>(Host, "_platform_strlcat");
  const Strncpy PlatformStrncpy = load<Strncpy>(Host, "_platform_strncpy");
  const Strnlen PlatformStrnlen = load<Strnlen>(Host, "_platform_strnlen");
  const Strstr PlatformStrstr = load<Strstr>(Host, "_platform_strstr");

  if (!PlatformBzero || !DoubleBzero || !PlatformMemset || !PlatformMemchr ||
      !PlatformMemcmp || !PlatformMemcmpZero || !InterposedMemcmpZero ||
      !PlatformMemccpy || !Pattern4 || !Pattern8 || !Pattern16 ||
      !PlatformStrcpy || !PlatformStrlcat || !PlatformStrncpy ||
      !PlatformStrnlen || !PlatformStrstr) {
    FreeLibrary(Host);
    return fail("one or more required memory exports are missing");
  }

  std::array<unsigned char, 10> ZeroTarget =
      {0xA5, 1, 2, 3, 4, 5, 6, 7, 8, 0x5A};
  PlatformBzero(ZeroTarget.data() + 1, 8);
  if (ZeroTarget.front() != 0xA5 || ZeroTarget.back() != 0x5A)
    return fail("_platform_bzero crossed its requested range");
  for (std::size_t I = 1; I != 9; ++I) {
    if (ZeroTarget[I] != 0)
      return fail("_platform_bzero did not zero the requested range");
  }

  ZeroTarget.fill(0xCC);
  DoubleBzero(ZeroTarget.data() + 2, 4);
  for (std::size_t I = 2; I != 6; ++I) {
    if (ZeroTarget[I] != 0)
      return fail("__bzero did not delegate to zero-fill semantics");
  }

  std::array<unsigned char, 8> Fill = {};
  if (PlatformMemset(Fill.data(), 0x7B, Fill.size()) != Fill.data())
    return fail("_platform_memset returned the wrong destination");
  for (unsigned char Byte : Fill) {
    if (Byte != 0x7B)
      return fail("_platform_memset filled an incorrect byte");
  }

  const std::array<unsigned char, 6> Search = {1, 2, 3, 4, 3, 5};
  if (PlatformMemchr(Search.data(), 3, Search.size()) != Search.data() + 2)
    return fail("_platform_memchr did not return the first matching byte");
  if (PlatformMemchr(Search.data(), 9, Search.size()) != nullptr)
    return fail("_platform_memchr should return null when no byte matches");

  const std::array<unsigned char, 4> EqualA = {1, 2, 3, 4};
  const std::array<unsigned char, 4> EqualB = {1, 2, 3, 4};
  const std::array<unsigned char, 4> Greater = {1, 2, 4, 4};
  if (PlatformMemcmp(EqualA.data(), EqualB.data(), EqualA.size()) != 0)
    return fail("_platform_memcmp equality contract failed");
  if (PlatformMemcmp(EqualA.data(), Greater.data(), EqualA.size()) >= 0)
    return fail("_platform_memcmp ordering contract failed");

  alignas(8) std::array<std::uint64_t, 4> ZeroWords = {};
  if (PlatformMemcmpZero(ZeroWords.data(), sizeof(ZeroWords)) != 0 ||
      InterposedMemcmpZero(ZeroWords.data(), sizeof(ZeroWords)) != 0 ||
      PlatformMemcmpZero(ZeroWords.data(), 0) != 0)
    return fail("memcmp_zero_aligned8 zero contract failed");
  reinterpret_cast<unsigned char *>(ZeroWords.data())[9] = 0x80;
  if (PlatformMemcmpZero(ZeroWords.data(), sizeof(ZeroWords)) != 1 ||
      InterposedMemcmpZero(ZeroWords.data(), sizeof(ZeroWords)) != 1)
    return fail("memcmp_zero_aligned8 must return exactly one for nonzero data");

  const std::array<unsigned char, 6> CopySource = {'a', 'b', 'c', 'd', 'e', 'f'};
  std::array<unsigned char, 8> CopyTarget = {};
  void *After = PlatformMemccpy(CopyTarget.data(), CopySource.data(), 'c',
                                CopySource.size());
  if (After != CopyTarget.data() + 3 || CopyTarget[0] != 'a' ||
      CopyTarget[1] != 'b' || CopyTarget[2] != 'c')
    return fail("_platform_memccpy stop-byte contract failed");
  CopyTarget.fill(0);
  if (PlatformMemccpy(CopyTarget.data(), CopySource.data(), 'z',
                      CopySource.size()) != nullptr ||
      !bytesEqual(CopyTarget.data(), CopySource.data(), CopySource.size()))
    return fail("_platform_memccpy full-copy contract failed");
  if (PlatformMemccpy(CopyTarget.data(), CopySource.data(), 'a', 0) != nullptr)
    return fail("_platform_memccpy zero-length contract failed");

  const std::array<unsigned char, 4> P4 = {1, 2, 3, 4};
  std::array<unsigned char, 10> PatternTarget4 = {};
  Pattern4(PatternTarget4.data(), P4.data(), PatternTarget4.size());
  const std::array<unsigned char, 10> Expected4 = {1, 2, 3, 4, 1, 2, 3, 4, 1, 2};
  if (!bytesEqual(PatternTarget4.data(), Expected4.data(), Expected4.size()))
    return fail("_platform_memset_pattern4 partial-tail contract failed");

  const std::array<unsigned char, 8> P8 = {0, 1, 2, 3, 4, 5, 6, 7};
  std::array<unsigned char, 18> PatternTarget8 = {};
  Pattern8(PatternTarget8.data(), P8.data(), PatternTarget8.size());
  for (std::size_t I = 0; I != PatternTarget8.size(); ++I) {
    if (PatternTarget8[I] != P8[I % P8.size()])
      return fail("_platform_memset_pattern8 repetition failed");
  }

  std::array<unsigned char, 16> P16 = {};
  for (std::size_t I = 0; I != P16.size(); ++I)
    P16[I] = static_cast<unsigned char>(0xA0 + I);
  std::array<unsigned char, 35> PatternTarget16 = {};
  Pattern16(PatternTarget16.data(), P16.data(), PatternTarget16.size());
  for (std::size_t I = 0; I != PatternTarget16.size(); ++I) {
    if (PatternTarget16[I] != P16[I % P16.size()])
      return fail("_platform_memset_pattern16 repetition failed");
  }

  char StringBuffer[32] = {};
  if (PlatformStrcpy(StringBuffer, "alpha") != StringBuffer ||
      std::strcmp(StringBuffer, "alpha") != 0)
    return fail("_platform_strcpy contract failed");

  std::strcpy(StringBuffer, "abc");
  if (PlatformStrlcat(StringBuffer, "defgh", sizeof(StringBuffer)) != 8 ||
      std::strcmp(StringBuffer, "abcdefgh") != 0)
    return fail("_platform_strlcat append contract failed");

  char Small[6] = "abc";
  if (PlatformStrlcat(Small, "defgh", sizeof(Small)) != 8 ||
      std::strcmp(Small, "abcde") != 0)
    return fail("_platform_strlcat truncation contract failed");

  char Unterminated[4] = {'a', 'b', 'c', 'd'};
  if (PlatformStrlcat(Unterminated, "xy", sizeof(Unterminated)) != 6)
    return fail("_platform_strlcat unterminated-destination length failed");

  char Padded[8];
  std::memset(Padded, 0x7F, sizeof(Padded));
  if (PlatformStrncpy(Padded, "hi", 5) != Padded || Padded[0] != 'h' ||
      Padded[1] != 'i' || Padded[2] != 0 || Padded[3] != 0 || Padded[4] != 0)
    return fail("_platform_strncpy padding contract failed");

  if (PlatformStrnlen("abcdef", 3) != 3 ||
      PlatformStrnlen("abc", 20) != 3)
    return fail("_platform_strnlen bound contract failed");

  const char *Haystack = "one two three";
  if (PlatformStrstr(Haystack, "two") != Haystack + 4 ||
      PlatformStrstr(Haystack, "four") != nullptr)
    return fail("_platform_strstr contract failed");

  FreeLibrary(Host);
  std::puts("Darwin memory primitive smoke passed.");
  return 0;
}
