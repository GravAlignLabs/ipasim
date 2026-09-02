#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ipasim::darwinmem {

enum class Access {
  Read,
  Write,
};

inline bool protectionAllows(DWORD Protection, Access Requested) {
  if ((Protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    return false;

  const DWORD Base = Protection & 0xffu;
  if (Requested == Access::Write) {
    return Base == PAGE_READWRITE || Base == PAGE_WRITECOPY ||
           Base == PAGE_EXECUTE_READWRITE || Base == PAGE_EXECUTE_WRITECOPY;
  }

  return Base == PAGE_READONLY || Base == PAGE_READWRITE ||
         Base == PAGE_WRITECOPY || Base == PAGE_EXECUTE_READ ||
         Base == PAGE_EXECUTE_READWRITE || Base == PAGE_EXECUTE_WRITECOPY;
}

// Validate the complete host-visible span before a Windows API dereferences a
// guest pointer. Checking only the first byte is insufficient for Darwin I/O:
// a buffer may begin in a committed page and cross into a guard/unmapped page.
// A zero-length operation intentionally accepts a null pointer, matching POSIX.
inline bool validateSpan(const void *Address, std::size_t Size,
                         Access Requested) {
  if (Size == 0)
    return true;
  if (!Address)
    return false;

  const std::uintptr_t Start = reinterpret_cast<std::uintptr_t>(Address);
  if (Start > (std::numeric_limits<std::uintptr_t>::max)() - Size)
    return false;
  const std::uintptr_t End = Start + Size;

  std::uintptr_t Cursor = Start;
  while (Cursor < End) {
    MEMORY_BASIC_INFORMATION Information{};
    if (VirtualQuery(reinterpret_cast<const void *>(Cursor), &Information,
                     sizeof(Information)) == 0 ||
        Information.State != MEM_COMMIT ||
        !protectionAllows(Information.Protect, Requested))
      return false;

    const std::uintptr_t RegionStart =
        reinterpret_cast<std::uintptr_t>(Information.BaseAddress);
    if (RegionStart >
        (std::numeric_limits<std::uintptr_t>::max)() - Information.RegionSize)
      return false;
    const std::uintptr_t RegionEnd = RegionStart + Information.RegionSize;
    if (RegionEnd <= Cursor)
      return false;
    Cursor = (std::min)(End, RegionEnd);
  }
  return true;
}

inline bool readableSpan(const void *Address, std::size_t Size) {
  return validateSpan(Address, Size, Access::Read);
}

inline bool writableSpan(void *Address, std::size_t Size) {
  return validateSpan(Address, Size, Access::Write);
}

} // namespace ipasim::darwinmem
