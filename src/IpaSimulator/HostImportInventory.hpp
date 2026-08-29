#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace ipasim::probe {
namespace host_inventory_detail {

constexpr std::uint32_t CpuTypeArm64 = 0x0100000cU;
constexpr std::uint32_t MhMagic64 = 0xfeedfacfU;
constexpr std::uint32_t FatMagic = 0xcafebabeU;
constexpr std::uint32_t FatMagic64 = 0xcafebabfU;

constexpr std::uint32_t LcLoadDylib = 0x0000000cU;
constexpr std::uint32_t LcLoadWeakDylib = 0x80000018U;
constexpr std::uint32_t LcReexportDylib = 0x8000001fU;
constexpr std::uint32_t LcLazyLoadDylib = 0x00000020U;
constexpr std::uint32_t LcLoadUpwardDylib = 0x80000023U;
constexpr std::uint32_t LcDyldChainedFixups = 0x80000034U;

constexpr std::uint32_t DyldChainedImport = 1;
constexpr std::uint32_t DyldChainedImportAddend = 2;
constexpr std::uint32_t DyldChainedImportAddend64 = 3;

struct Import {
  int Ordinal = 0;
  bool Weak = false;
  std::string Name;
};

inline bool rangeValid(std::size_t Offset, std::size_t Size,
                       std::size_t Total) {
  return Offset <= Total && Size <= Total - Offset;
}

inline std::uint32_t readLe32(const std::vector<std::uint8_t> &Data,
                              std::size_t Offset) {
  return std::uint32_t(Data[Offset]) |
         (std::uint32_t(Data[Offset + 1]) << 8) |
         (std::uint32_t(Data[Offset + 2]) << 16) |
         (std::uint32_t(Data[Offset + 3]) << 24);
}

inline std::uint64_t readLe64(const std::vector<std::uint8_t> &Data,
                              std::size_t Offset) {
  return std::uint64_t(readLe32(Data, Offset)) |
         (std::uint64_t(readLe32(Data, Offset + 4)) << 32);
}

inline std::uint32_t readBe32(const std::vector<std::uint8_t> &Data,
                              std::size_t Offset) {
  return (std::uint32_t(Data[Offset]) << 24) |
         (std::uint32_t(Data[Offset + 1]) << 16) |
         (std::uint32_t(Data[Offset + 2]) << 8) |
         std::uint32_t(Data[Offset + 3]);
}

inline std::uint64_t readBe64(const std::vector<std::uint8_t> &Data,
                              std::size_t Offset) {
  return (std::uint64_t(readBe32(Data, Offset)) << 32) |
         std::uint64_t(readBe32(Data, Offset + 4));
}

inline bool readCString(const std::vector<std::uint8_t> &Data,
                        std::size_t Begin, std::size_t End,
                        std::string &Value) {
  if (Begin >= End || End > Data.size())
    return false;
  std::size_t Cursor = Begin;
  while (Cursor < End && Data[Cursor] != 0)
    ++Cursor;
  if (Cursor == End)
    return false;
  Value.assign(reinterpret_cast<const char *>(Data.data() + Begin),
               Cursor - Begin);
  return true;
}

inline bool isDylibLoadCommand(std::uint32_t Command) {
  return Command == LcLoadDylib || Command == LcLoadWeakDylib ||
         Command == LcReexportDylib || Command == LcLazyLoadDylib ||
         Command == LcLoadUpwardDylib;
}

inline bool isDarwinHostInstallName(const std::string &Name) {
  return Name == "/usr/lib/system/libsystem_kernel.dylib" ||
         Name == "/usr/lib/system/libsystem_platform.dylib" ||
         Name == "/usr/lib/system/libsystem_pthread.dylib";
}

inline bool loadFile(const std::filesystem::path &Path,
                     std::vector<std::uint8_t> &Data) {
  std::ifstream Input(Path, std::ios::binary | std::ios::ate);
  if (!Input)
    return false;
  const std::streamoff Length = Input.tellg();
  if (Length <= 0)
    return false;
  Data.resize(static_cast<std::size_t>(Length));
  Input.seekg(0, std::ios::beg);
  return static_cast<bool>(Input.read(
      reinterpret_cast<char *>(Data.data()),
      static_cast<std::streamsize>(Data.size())));
}

inline bool selectArm64Slice(const std::vector<std::uint8_t> &Data,
                             std::size_t &SliceOffset,
                             std::size_t &SliceSize,
                             std::string &Error) {
  if (!rangeValid(0, 8, Data.size())) {
    Error = "file is too small for a Mach-O header";
    return false;
  }

  const std::uint32_t ThinMagic = readLe32(Data, 0);
  if (ThinMagic == MhMagic64) {
    if (!rangeValid(0, 32, Data.size()) || readLe32(Data, 4) != CpuTypeArm64) {
      Error = "thin Mach-O is not ARM64";
      return false;
    }
    SliceOffset = 0;
    SliceSize = Data.size();
    return true;
  }

  const std::uint32_t Magic = readBe32(Data, 0);
  if (Magic != FatMagic && Magic != FatMagic64) {
    Error = "unsupported Mach-O/fat magic";
    return false;
  }

  const std::uint32_t Count = readBe32(Data, 4);
  const std::size_t EntrySize = Magic == FatMagic64 ? 32 : 20;
  if (Count > (Data.size() - 8) / EntrySize) {
    Error = "fat architecture table is truncated";
    return false;
  }

  for (std::uint32_t Index = 0; Index != Count; ++Index) {
    const std::size_t Entry = 8 + std::size_t(Index) * EntrySize;
    if (readBe32(Data, Entry) != CpuTypeArm64)
      continue;

    const std::uint64_t Offset = Magic == FatMagic64
                                     ? readBe64(Data, Entry + 8)
                                     : readBe32(Data, Entry + 8);
    const std::uint64_t Size = Magic == FatMagic64
                                   ? readBe64(Data, Entry + 16)
                                   : readBe32(Data, Entry + 12);
    if (Offset > Data.size() || Size > Data.size() - std::size_t(Offset)) {
      Error = "ARM64 fat slice is outside the file";
      return false;
    }
    SliceOffset = static_cast<std::size_t>(Offset);
    SliceSize = static_cast<std::size_t>(Size);
    if (!rangeValid(SliceOffset, 32, Data.size()) ||
        readLe32(Data, SliceOffset) != MhMagic64) {
      Error = "ARM64 fat slice is not a little-endian Mach-O 64 image";
      return false;
    }
    return true;
  }

  Error = "fat Mach-O contains no ARM64 slice";
  return false;
}

inline bool parseImage(const std::vector<std::uint8_t> &Data,
                       std::size_t SliceOffset, std::size_t SliceSize,
                       std::vector<std::string> &Libraries,
                       std::vector<Import> &Imports,
                       std::string &Error) {
  if (!rangeValid(SliceOffset, 32, Data.size()) || SliceSize < 32) {
    Error = "Mach-O header is truncated";
    return false;
  }

  const std::uint32_t CommandCount = readLe32(Data, SliceOffset + 16);
  const std::uint32_t CommandBytes = readLe32(Data, SliceOffset + 20);
  const std::size_t CommandsBegin = SliceOffset + 32;
  if (!rangeValid(CommandsBegin, CommandBytes, Data.size()) ||
      CommandBytes > SliceSize - 32) {
    Error = "Mach-O load commands are outside the ARM64 slice";
    return false;
  }

  std::uint32_t FixupsOffset = 0;
  std::uint32_t FixupsSize = 0;
  std::size_t Cursor = CommandsBegin;
  const std::size_t CommandsEnd = CommandsBegin + CommandBytes;
  Libraries.clear();

  for (std::uint32_t Index = 0; Index != CommandCount; ++Index) {
    if (!rangeValid(Cursor, 8, CommandsEnd)) {
      Error = "Mach-O load command list ends early";
      return false;
    }
    const std::uint32_t Command = readLe32(Data, Cursor);
    const std::uint32_t CommandSize = readLe32(Data, Cursor + 4);
    if (CommandSize < 8 || !rangeValid(Cursor, CommandSize, CommandsEnd)) {
      Error = "invalid Mach-O load command size";
      return false;
    }

    if (isDylibLoadCommand(Command)) {
      if (CommandSize < 24) {
        Error = "truncated dylib load command";
        return false;
      }
      const std::uint32_t NameOffset = readLe32(Data, Cursor + 8);
      std::string Name;
      if (NameOffset >= CommandSize ||
          !readCString(Data, Cursor + NameOffset, Cursor + CommandSize, Name)) {
        Error = "invalid dylib install name";
        return false;
      }
      Libraries.push_back(std::move(Name));
    } else if (Command == LcDyldChainedFixups) {
      if (CommandSize < 16) {
        Error = "truncated LC_DYLD_CHAINED_FIXUPS command";
        return false;
      }
      FixupsOffset = readLe32(Data, Cursor + 8);
      FixupsSize = readLe32(Data, Cursor + 12);
    }

    Cursor += CommandSize;
  }

  if (FixupsSize == 0) {
    Error = "ARM64 image has no chained-fixup payload";
    return false;
  }

  const std::size_t FixupsBegin = SliceOffset + FixupsOffset;
  if (FixupsOffset > SliceSize || FixupsSize > SliceSize - FixupsOffset ||
      !rangeValid(FixupsBegin, FixupsSize, Data.size()) || FixupsSize < 28) {
    Error = "chained-fixup payload is outside the ARM64 slice";
    return false;
  }

  const std::uint32_t ImportsOffset = readLe32(Data, FixupsBegin + 8);
  const std::uint32_t SymbolsOffset = readLe32(Data, FixupsBegin + 12);
  const std::uint32_t ImportsCount = readLe32(Data, FixupsBegin + 16);
  const std::uint32_t ImportsFormat = readLe32(Data, FixupsBegin + 20);
  const std::uint32_t SymbolsFormat = readLe32(Data, FixupsBegin + 24);
  if (SymbolsFormat != 0) {
    Error = "compressed chained-fixup symbol pool is not supported by inventory";
    return false;
  }

  std::size_t ImportSize = 0;
  if (ImportsFormat == DyldChainedImport)
    ImportSize = 4;
  else if (ImportsFormat == DyldChainedImportAddend)
    ImportSize = 8;
  else if (ImportsFormat == DyldChainedImportAddend64)
    ImportSize = 16;
  else {
    Error = "unknown chained-fixup import format";
    return false;
  }

  if (ImportsOffset > FixupsSize || SymbolsOffset > FixupsSize ||
      ImportsCount > (FixupsSize - ImportsOffset) / ImportSize) {
    Error = "chained-fixup import table is outside payload";
    return false;
  }

  Imports.clear();
  Imports.reserve(ImportsCount);
  const std::size_t SymbolsBegin = FixupsBegin + SymbolsOffset;
  const std::size_t FixupsEnd = FixupsBegin + FixupsSize;

  for (std::uint32_t Index = 0; Index != ImportsCount; ++Index) {
    const std::size_t Entry = FixupsBegin + ImportsOffset +
                              std::size_t(Index) * ImportSize;
    Import Result;
    std::uint32_t NameOffset = 0;

    if (ImportsFormat == DyldChainedImport ||
        ImportsFormat == DyldChainedImportAddend) {
      const std::uint32_t Raw = readLe32(Data, Entry);
      Result.Ordinal = static_cast<std::int8_t>(Raw & 0xffU);
      Result.Weak = ((Raw >> 8) & 1U) != 0;
      NameOffset = Raw >> 9;
    } else {
      const std::uint64_t Raw = readLe64(Data, Entry);
      Result.Ordinal = static_cast<std::int16_t>(Raw & 0xffffU);
      Result.Weak = ((Raw >> 16) & 1U) != 0;
      NameOffset = static_cast<std::uint32_t>(Raw >> 32);
    }

    if (NameOffset >= FixupsSize - SymbolsOffset ||
        !readCString(Data, SymbolsBegin + NameOffset, FixupsEnd, Result.Name)) {
      Error = "invalid chained-fixup symbol name";
      return false;
    }
    Imports.push_back(std::move(Result));
  }

  return true;
}

inline std::filesystem::path bridgePath() {
  wchar_t Buffer[32768] = {};
  const DWORD Length = GetModuleFileNameW(nullptr, Buffer,
                                          static_cast<DWORD>(std::size(Buffer)));
  if (Length == 0 || Length >= std::size(Buffer))
    return {};
  return std::filesystem::path(std::wstring(Buffer, Length)).parent_path() /
         L"IpaSimDarwinHost.dll";
}

} // namespace host_inventory_detail

inline void reportDarwinHostImportInventory(const char *RuntimeRoot) {
  using namespace host_inventory_detail;
  if (!RuntimeRoot || !*RuntimeRoot)
    return;

  const std::filesystem::path KernelImage =
      std::filesystem::path(RuntimeRoot) / L"usr" / L"lib" / L"system" /
      L"libsystem_sim_kernel.dylib";

  std::vector<std::uint8_t> Data;
  if (!loadFile(KernelImage, Data)) {
    std::fprintf(stderr,
                 "[host-import-inventory] could not read %s; continuing with normal loader\n",
                 KernelImage.string().c_str());
    return;
  }

  std::size_t SliceOffset = 0;
  std::size_t SliceSize = 0;
  std::string Error;
  if (!selectArm64Slice(Data, SliceOffset, SliceSize, Error)) {
    std::fprintf(stderr,
                 "[host-import-inventory] %s; continuing with normal loader\n",
                 Error.c_str());
    return;
  }

  std::vector<std::string> Libraries;
  std::vector<Import> Imports;
  if (!parseImage(Data, SliceOffset, SliceSize, Libraries, Imports, Error)) {
    std::fprintf(stderr,
                 "[host-import-inventory] %s; continuing with normal loader\n",
                 Error.c_str());
    return;
  }

  const std::filesystem::path BridgePath = bridgePath();
  HMODULE Bridge = BridgePath.empty()
                       ? nullptr
                       : LoadLibraryW(BridgePath.c_str());
  if (!Bridge) {
    std::fprintf(stderr,
                 "[host-import-inventory] could not load IpaSimDarwinHost.dll for export comparison; continuing with normal loader\n");
    return;
  }

  using Key = std::pair<std::string, std::string>;
  std::set<Key> RequiredHostImports;
  std::set<Key> WeakHostImports;
  for (const Import &ImportEntry : Imports) {
    if (ImportEntry.Ordinal <= 0 ||
        static_cast<std::size_t>(ImportEntry.Ordinal) > Libraries.size())
      continue;
    const std::string &Library = Libraries[ImportEntry.Ordinal - 1];
    if (!isDarwinHostInstallName(Library))
      continue;
    (ImportEntry.Weak ? WeakHostImports : RequiredHostImports)
        .emplace(Library, ImportEntry.Name);
  }

  std::size_t RequiredPresent = 0;
  std::size_t RequiredMissing = 0;
  std::size_t WeakMissing = 0;
  std::vector<Key> MissingRequired;
  std::vector<Key> MissingWeak;

  const auto HasExport = [&](const std::string &MachSymbol) {
    std::string ExportName = MachSymbol;
    if (!ExportName.empty() && ExportName.front() == '_')
      ExportName.erase(ExportName.begin());
    return GetProcAddress(Bridge, ExportName.c_str()) != nullptr;
  };

  for (const Key &Entry : RequiredHostImports) {
    if (HasExport(Entry.second))
      ++RequiredPresent;
    else {
      ++RequiredMissing;
      MissingRequired.push_back(Entry);
    }
  }
  for (const Key &Entry : WeakHostImports) {
    if (!HasExport(Entry.second)) {
      ++WeakMissing;
      MissingWeak.push_back(Entry);
    }
  }

  std::printf("[host-import-inventory] libsystem_sim_kernel ARM64 host ABI: %zu required imports, %zu already bridged, %zu missing; %zu weak imports missing\n",
              RequiredHostImports.size(), RequiredPresent, RequiredMissing,
              WeakMissing);

  for (const Key &Entry : MissingRequired)
    std::printf("[host-import-inventory] MISSING required %s :: %s\n",
                Entry.first.c_str(), Entry.second.c_str());
  for (const Key &Entry : MissingWeak)
    std::printf("[host-import-inventory] MISSING weak %s :: %s\n",
                Entry.first.c_str(), Entry.second.c_str());

  FreeLibrary(Bridge);
}

} // namespace ipasim::probe
