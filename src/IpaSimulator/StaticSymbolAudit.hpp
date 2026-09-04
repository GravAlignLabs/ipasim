#pragma once

#include "HostImportInventory.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <windows.h>

namespace ipasim::probe {
namespace static_symbol_audit_detail {

using namespace host_inventory_detail;

constexpr std::uint32_t LcSymtab = 0x00000002U;
constexpr std::uint32_t LcDyldExportsTrie = 0x80000033U;
constexpr std::uint8_t NStab = 0xe0U;
constexpr std::uint8_t NType = 0x0eU;
constexpr std::uint8_t NExt = 0x01U;
constexpr std::uint8_t NUndef = 0x00U;
constexpr std::uint16_t NWeakRef = 0x0040U;

constexpr std::uint64_t ExportKindMask = 0x03U;
constexpr std::uint64_t ExportKindRegular = 0x00U;
constexpr std::uint64_t ExportKindThreadLocal = 0x01U;
constexpr std::uint64_t ExportKindAbsolute = 0x02U;
constexpr std::uint64_t ExportReexport = 0x08U;
constexpr std::uint64_t ExportStubAndResolver = 0x10U;

constexpr int BindSpecialDylibSelf = 0;
constexpr int BindSpecialDylibMainExecutable = -1;
constexpr int BindSpecialDylibFlatLookup = -2;
constexpr int BindSpecialDylibWeakLookup = -3;

struct Dependency {
  std::uint32_t Command = 0;
  std::string Name;

  bool optional() const {
    return Command == LcLoadWeakDylib || Command == LcLazyLoadDylib;
  }
  bool reexport() const { return Command == LcReexportDylib; }
};

struct ExportEntry {
  enum class Kind { Direct, Reexport, Unsupported };
  Kind Type = Kind::Direct;
  int LibOrdinal = 0;
  std::string ImportName;
};

struct Image {
  image_source_detail::ImageSource Source;
  std::string DisplayName;
  std::vector<Dependency> Dependencies;
  std::vector<Import> Imports;
  std::map<std::string, ExportEntry> Exports;
  bool ImportCoverageComplete = true;
};

struct Binding {
  std::string Importer;
  int Ordinal = 0;
  std::string ExpectedLibrary;
  std::string Symbol;
  bool Weak = false;
  std::string Reason;
};

struct Context {
  const RuntimeRootStore *RuntimeStore = nullptr;
  std::filesystem::path MainExecutable;
  std::filesystem::path BridgePath;
  HMODULE Bridge = nullptr;
  std::map<std::string, Image> Images;
  std::set<std::string> MissingImageKeys;
  std::map<std::string, std::string> MissingImageDetails;
  std::vector<std::string> ParseWarnings;
};

inline std::string pathKey(const std::filesystem::path &Path) {
  return image_source_detail::hostImageKey(Path);
}

inline std::string displayName(const std::filesystem::path &Path) {
  return image_source_detail::hostDisplayName(Path);
}

inline bool isHostDependency(const std::string &Name) {
  return Name == "IpaSimDarwinHost.dll" || isDarwinHostInstallName(Name);
}

inline bool dependencySource(const Context &Ctx, const Dependency &Dep,
                             image_source_detail::ImageSource &Source,
                             std::string &Error) {
  if (isHostDependency(Dep.Name)) {
    Source = image_source_detail::makeHostImageSource(Ctx.BridgePath);
    Error.clear();
    return true;
  }
  if (!Dep.Name.empty() && Dep.Name.front() == '/') {
    if (!Ctx.RuntimeStore) {
      Error = "RuntimeRoot store is not configured for " + Dep.Name;
      return false;
    }
    return image_source_detail::makeRuntimeImageSource(
        *Ctx.RuntimeStore, Dep.Name, Source, Error);
  }

  // This intentionally mirrors the modern DynamicLoader. It does not invent
  // @rpath/@loader_path behavior that the runtime does not implement yet.
  Source = image_source_detail::makeHostImageSource(
      std::filesystem::path(Dep.Name).lexically_normal());
  Error.clear();
  return true;
}

inline bool readUleb128(const std::uint8_t *&Cursor, const std::uint8_t *End,
                        std::uint64_t &Value) {
  Value = 0;
  for (unsigned ByteIndex = 0; ByteIndex != 10; ++ByteIndex) {
    if (Cursor == End)
      return false;
    const std::uint8_t Byte = *Cursor++;
    if (ByteIndex == 9 && (Byte & 0xfeU) != 0)
      return false;
    Value |= std::uint64_t(Byte & 0x7fU) << (ByteIndex * 7);
    if ((Byte & 0x80U) == 0)
      return true;
  }
  return false;
}

inline bool readCStringPtr(const std::uint8_t *Start, const std::uint8_t *End,
                           std::string &Value) {
  const std::uint8_t *Cursor = Start;
  while (Cursor != End && *Cursor)
    ++Cursor;
  if (Cursor == End)
    return false;
  Value.assign(reinterpret_cast<const char *>(Start),
               reinterpret_cast<const char *>(Cursor));
  return true;
}

inline bool parseExportsTrie(const std::uint8_t *Data, std::size_t Size,
                             std::map<std::string, ExportEntry> &Exports,
                             std::string &Error) {
  if (!Data || Size == 0)
    return true;

  const std::uint8_t *End = Data + Size;
  std::set<std::uint64_t> ActiveNodes;
  std::function<bool(std::uint64_t, const std::string &, unsigned)> Visit;
  Visit = [&](std::uint64_t NodeOffset, const std::string &Prefix,
              unsigned Depth) -> bool {
    if (Depth > 1024) {
      Error = "exports trie exceeds maximum depth";
      return false;
    }
    if (NodeOffset >= Size) {
      Error = "exports trie node offset is outside payload";
      return false;
    }
    if (!ActiveNodes.insert(NodeOffset).second) {
      Error = "exports trie contains a cycle";
      return false;
    }

    const std::uint8_t *Cursor = Data + NodeOffset;
    std::uint64_t TerminalSize = 0;
    if (!readUleb128(Cursor, End, TerminalSize) ||
        TerminalSize > std::uint64_t(End - Cursor)) {
      ActiveNodes.erase(NodeOffset);
      Error = "invalid exports trie terminal size";
      return false;
    }
    const std::uint8_t *TerminalEnd = Cursor + TerminalSize;

    if (TerminalSize != 0) {
      std::uint64_t Flags = 0;
      if (!readUleb128(Cursor, TerminalEnd, Flags)) {
        ActiveNodes.erase(NodeOffset);
        Error = "invalid exports trie terminal flags";
        return false;
      }

      ExportEntry Entry;
      if (Flags & ExportReexport) {
        std::uint64_t Ordinal = 0;
        if (!readUleb128(Cursor, TerminalEnd, Ordinal) ||
            Ordinal > 0x7fffffffU) {
          ActiveNodes.erase(NodeOffset);
          Error = "invalid exports trie reexport ordinal";
          return false;
        }
        std::string ImportName;
        if (!readCStringPtr(Cursor, TerminalEnd, ImportName)) {
          ActiveNodes.erase(NodeOffset);
          Error = "invalid exports trie reexport name";
          return false;
        }
        Entry.Type = ExportEntry::Kind::Reexport;
        Entry.LibOrdinal = static_cast<int>(Ordinal);
        Entry.ImportName = std::move(ImportName);
      } else {
        std::uint64_t Address = 0;
        if (!readUleb128(Cursor, TerminalEnd, Address)) {
          ActiveNodes.erase(NodeOffset);
          Error = "invalid exports trie symbol address";
          return false;
        }
        (void)Address;
        const std::uint64_t Kind = Flags & ExportKindMask;
        if (Kind != ExportKindRegular && Kind != ExportKindThreadLocal &&
            Kind != ExportKindAbsolute) {
          Entry.Type = ExportEntry::Kind::Unsupported;
        }
        if (Flags & ExportStubAndResolver) {
          std::uint64_t Resolver = 0;
          if (!readUleb128(Cursor, TerminalEnd, Resolver)) {
            ActiveNodes.erase(NodeOffset);
            Error = "invalid exports trie resolver";
            return false;
          }
          (void)Resolver;
          Entry.Type = ExportEntry::Kind::Unsupported;
        }
      }
      Exports[Prefix] = std::move(Entry);
    }

    Cursor = TerminalEnd;
    if (Cursor == End) {
      ActiveNodes.erase(NodeOffset);
      Error = "exports trie node is missing child count";
      return false;
    }
    const std::uint8_t ChildCount = *Cursor++;
    for (std::uint8_t Child = 0; Child != ChildCount; ++Child) {
      const std::uint8_t *EdgeStart = Cursor;
      while (Cursor != End && *Cursor)
        ++Cursor;
      if (Cursor == End) {
        ActiveNodes.erase(NodeOffset);
        Error = "unterminated exports trie edge";
        return false;
      }
      std::string Edge(reinterpret_cast<const char *>(EdgeStart),
                       reinterpret_cast<const char *>(Cursor));
      ++Cursor;
      std::uint64_t ChildOffset = 0;
      if (!readUleb128(Cursor, End, ChildOffset)) {
        ActiveNodes.erase(NodeOffset);
        Error = "invalid exports trie child offset";
        return false;
      }
      if (!Visit(ChildOffset, Prefix + Edge, Depth + 1)) {
        ActiveNodes.erase(NodeOffset);
        return false;
      }
    }
    ActiveNodes.erase(NodeOffset);
    return true;
  };

  return Visit(0, std::string(), 0);
}

inline bool parseChainedImports(const std::vector<std::uint8_t> &Data,
                                std::size_t SliceOffset,
                                std::size_t SliceSize,
                                std::uint32_t FixupsOffset,
                                std::uint32_t FixupsSize,
                                std::vector<Import> &Imports,
                                std::string &Error) {
  if (FixupsSize == 0)
    return true;
  const std::size_t Begin = SliceOffset + FixupsOffset;
  if (FixupsOffset > SliceSize || FixupsSize > SliceSize - FixupsOffset ||
      !rangeValid(Begin, FixupsSize, Data.size()) || FixupsSize < 28) {
    Error = "chained-fixup payload is outside ARM64 slice";
    return false;
  }

  const std::uint32_t ImportsOffset = readLe32(Data, Begin + 8);
  const std::uint32_t SymbolsOffset = readLe32(Data, Begin + 12);
  const std::uint32_t ImportsCount = readLe32(Data, Begin + 16);
  const std::uint32_t ImportsFormat = readLe32(Data, Begin + 20);
  const std::uint32_t SymbolsFormat = readLe32(Data, Begin + 24);
  if (SymbolsFormat != 0) {
    Error = "compressed chained-fixup symbol pool is unsupported by audit";
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

  const std::size_t SymbolsBegin = Begin + SymbolsOffset;
  const std::size_t FixupsEnd = Begin + FixupsSize;
  Imports.clear();
  Imports.reserve(ImportsCount);
  for (std::uint32_t Index = 0; Index != ImportsCount; ++Index) {
    const std::size_t Entry =
        Begin + ImportsOffset + std::size_t(Index) * ImportSize;
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

inline bool parseSymtab(const std::vector<std::uint8_t> &Data,
                        std::size_t SliceOffset, std::size_t SliceSize,
                        std::uint32_t SymOffset, std::uint32_t SymbolCount,
                        std::uint32_t StringOffset, std::uint32_t StringSize,
                        bool CollectImports, std::vector<Import> &Imports,
                        std::map<std::string, ExportEntry> &Exports,
                        std::string &Error) {
  constexpr std::size_t Nlist64Size = 16;
  if (SymOffset > SliceSize ||
      SymbolCount > (SliceSize - SymOffset) / Nlist64Size ||
      StringOffset > SliceSize || StringSize > SliceSize - StringOffset) {
    Error = "symbol/string table is outside ARM64 slice";
    return false;
  }
  const std::size_t SymBegin = SliceOffset + SymOffset;
  const std::size_t StrBegin = SliceOffset + StringOffset;
  const std::size_t StrEnd = StrBegin + StringSize;
  if (!rangeValid(SymBegin, std::size_t(SymbolCount) * Nlist64Size,
                  Data.size()) ||
      !rangeValid(StrBegin, StringSize, Data.size())) {
    Error = "symbol/string table is outside file";
    return false;
  }

  if (CollectImports)
    Imports.clear();
  for (std::uint32_t Index = 0; Index != SymbolCount; ++Index) {
    const std::size_t Entry = SymBegin + std::size_t(Index) * Nlist64Size;
    const std::uint32_t StringIndex = readLe32(Data, Entry);
    const std::uint8_t TypeByte = Data[Entry + 4];
    const std::uint16_t Desc = static_cast<std::uint16_t>(Data[Entry + 6]) |
                               (static_cast<std::uint16_t>(Data[Entry + 7]) << 8);
    const std::uint64_t Value = readLe64(Data, Entry + 8);
    if ((TypeByte & NStab) != 0 || (TypeByte & NExt) == 0 ||
        StringIndex == 0 || StringIndex >= StringSize)
      continue;

    std::string Name;
    if (!readCString(Data, StrBegin + StringIndex, StrEnd, Name)) {
      Error = "invalid nlist string index";
      return false;
    }
    const std::uint8_t Type = TypeByte & NType;
    if (Type == NUndef && Value == 0) {
      if (!CollectImports)
        continue;
      Import Result;
      Result.Name = std::move(Name);
      Result.Ordinal = static_cast<std::int8_t>((Desc >> 8) & 0xffU);
      Result.Weak = (Desc & NWeakRef) != 0;
      Imports.push_back(std::move(Result));
    } else if (Type != NUndef) {
      // The runtime's classic fallback asks LIEF whether a symbol exists before
      // using its value. Presence is therefore the relevant static property.
      Exports.emplace(std::move(Name), ExportEntry{});
    }
  }
  return true;
}

inline bool parseImage(const image_source_detail::ImageSource &Source,
                       Image &Result, std::string &Error,
                       bool *ReadFailure = nullptr) {
  if (ReadFailure)
    *ReadFailure = false;
  std::vector<std::uint8_t> Data;
  if (!image_source_detail::readImageSource(Source, Data, Error)) {
    if (ReadFailure)
      *ReadFailure = true;
    return false;
  }
  std::size_t SliceOffset = 0;
  std::size_t SliceSize = 0;
  if (!selectArm64Slice(Data, SliceOffset, SliceSize, Error))
    return false;

  if (!rangeValid(SliceOffset, 32, Data.size()) || SliceSize < 32) {
    Error = "Mach-O header is truncated";
    return false;
  }
  const std::uint32_t CommandCount = readLe32(Data, SliceOffset + 16);
  const std::uint32_t CommandBytes = readLe32(Data, SliceOffset + 20);
  const std::size_t CommandsBegin = SliceOffset + 32;
  if (!rangeValid(CommandsBegin, CommandBytes, Data.size()) ||
      CommandBytes > SliceSize - 32) {
    Error = "Mach-O load commands are outside ARM64 slice";
    return false;
  }

  std::uint32_t FixupsOffset = 0, FixupsSize = 0;
  std::uint32_t ExportsOffset = 0, ExportsSize = 0;
  std::uint32_t SymOffset = 0, SymbolCount = 0, StringOffset = 0,
                StringSize = 0;
  std::size_t Cursor = CommandsBegin;
  const std::size_t CommandsEnd = CommandsBegin + CommandBytes;
  Result = Image{};
  Result.Source = Source;
  Result.DisplayName = Source.DisplayName;

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
      Result.Dependencies.push_back(Dependency{Command, std::move(Name)});
    } else if (Command == LcDyldChainedFixups) {
      if (CommandSize < 16) {
        Error = "truncated LC_DYLD_CHAINED_FIXUPS";
        return false;
      }
      FixupsOffset = readLe32(Data, Cursor + 8);
      FixupsSize = readLe32(Data, Cursor + 12);
    } else if (Command == LcDyldExportsTrie) {
      if (CommandSize < 16) {
        Error = "truncated LC_DYLD_EXPORTS_TRIE";
        return false;
      }
      ExportsOffset = readLe32(Data, Cursor + 8);
      ExportsSize = readLe32(Data, Cursor + 12);
    } else if (Command == LcSymtab) {
      if (CommandSize < 24) {
        Error = "truncated LC_SYMTAB";
        return false;
      }
      SymOffset = readLe32(Data, Cursor + 8);
      SymbolCount = readLe32(Data, Cursor + 12);
      StringOffset = readLe32(Data, Cursor + 16);
      StringSize = readLe32(Data, Cursor + 20);
    }
    Cursor += CommandSize;
  }

  bool HaveChainedImports = FixupsSize != 0;
  if (HaveChainedImports) {
    if (!parseChainedImports(Data, SliceOffset, SliceSize, FixupsOffset,
                             FixupsSize, Result.Imports, Error))
      return false;
  }

  if (ExportsSize != 0) {
    if (ExportsOffset > SliceSize || ExportsSize > SliceSize - ExportsOffset ||
        !rangeValid(SliceOffset + ExportsOffset, ExportsSize, Data.size())) {
      Error = "exports trie is outside ARM64 slice";
      return false;
    }
    if (!parseExportsTrie(Data.data() + SliceOffset + ExportsOffset, ExportsSize,
                          Result.Exports, Error))
      return false;
  }

  if (SymbolCount != 0 || StringSize != 0) {
    if (!parseSymtab(Data, SliceOffset, SliceSize, SymOffset, SymbolCount,
                     StringOffset, StringSize, !HaveChainedImports,
                     Result.Imports, Result.Exports, Error))
      return false;
  } else if (!HaveChainedImports) {
    Result.ImportCoverageComplete = false;
  }

  return true;
}

inline bool bridgeHasSymbol(HMODULE Bridge, const std::string &MachSymbol) {
  if (!Bridge)
    return false;
  std::string ExportName = MachSymbol;
  if (!ExportName.empty() && ExportName.front() == '_')
    ExportName.erase(ExportName.begin());
  return GetProcAddress(Bridge, ExportName.c_str()) != nullptr;
}

inline std::string dependencyKey(const Context &Ctx, const Dependency &Dep) {
  image_source_detail::ImageSource Source;
  std::string Error;
  if (!dependencySource(Ctx, Dep, Source, Error))
    return "unresolved-runtime-source:" + Dep.Name;
  return Source.Key;
}

inline bool providerHasSymbol(Context &Ctx, const std::string &ProviderKey,
                              const std::string &Symbol,
                              std::set<std::pair<std::string, std::string>> &Seen,
                              std::string &Reason) {
  if (!Seen.insert({ProviderKey, Symbol}).second) {
    Reason = "reexport cycle";
    return false;
  }

  if (pathKey(Ctx.BridgePath) == ProviderKey) {
    if (bridgeHasSymbol(Ctx.Bridge, Symbol))
      return true;
    Reason = "native bridge export missing";
    return false;
  }

  auto It = Ctx.Images.find(ProviderKey);
  if (It == Ctx.Images.end()) {
    Reason = Ctx.MissingImageKeys.count(ProviderKey) != 0
                 ? "provider image missing"
                 : "provider image was not parsed";
    return false;
  }
  const Image &Provider = It->second;
  auto ExportIt = Provider.Exports.find(Symbol);
  if (ExportIt != Provider.Exports.end()) {
    const ExportEntry &Export = ExportIt->second;
    if (Export.Type == ExportEntry::Kind::Direct)
      return true;
    if (Export.Type == ExportEntry::Kind::Unsupported) {
      Reason = "provider export kind is unsupported by ipaSim";
      return false;
    }
    if (Export.LibOrdinal <= 0 ||
        static_cast<std::size_t>(Export.LibOrdinal) >
            Provider.Dependencies.size()) {
      Reason = "reexport ordinal is invalid";
      return false;
    }
    const Dependency &Dep = Provider.Dependencies[Export.LibOrdinal - 1];
    const std::string ImportName =
        Export.ImportName.empty() ? Symbol : Export.ImportName;
    return providerHasSymbol(Ctx, dependencyKey(Ctx, Dep), ImportName, Seen,
                             Reason);
  }

  // Classic LC_REEXPORT_DYLIB makes the dependency's whole namespace visible.
  for (const Dependency &Dep : Provider.Dependencies) {
    if (!Dep.reexport())
      continue;
    std::set<std::pair<std::string, std::string>> BranchSeen = Seen;
    std::string BranchReason;
    if (providerHasSymbol(Ctx, dependencyKey(Ctx, Dep), Symbol, BranchSeen,
                          BranchReason))
      return true;
  }

  Reason = "symbol absent from expected provider";
  return false;
}

inline bool providerHasSymbol(Context &Ctx, const std::string &ProviderKey,
                              const std::string &Symbol,
                              std::string &Reason) {
  std::set<std::pair<std::string, std::string>> Seen;
  return providerHasSymbol(Ctx, ProviderKey, Symbol, Seen, Reason);
}

inline bool flatLookupHasSymbol(Context &Ctx, const std::string &Symbol) {
  std::string Reason;
  if (bridgeHasSymbol(Ctx.Bridge, Symbol))
    return true;
  for (const auto &Pair : Ctx.Images) {
    if (providerHasSymbol(Ctx, Pair.first, Symbol, Reason))
      return true;
  }
  return false;
}

inline void collectClosure(Context &Ctx, const std::filesystem::path &RootImage) {
  using namespace image_source_detail;

  ImageSource RootSource = makeHostImageSource(RootImage);
  std::vector<ImageSource> Queue{RootSource};
  std::set<std::string> Queued{RootSource.Key};

  while (!Queue.empty()) {
    ImageSource Source = std::move(Queue.back());
    Queue.pop_back();
    const std::string Key = Source.Key;
    if (Ctx.Images.count(Key) != 0 || Ctx.MissingImageKeys.count(Key) != 0)
      continue;

    Image Parsed;
    std::string Error;
    bool ReadFailure = false;
    if (!parseImage(Source, Parsed, Error, &ReadFailure)) {
      if (ReadFailure) {
        Ctx.MissingImageKeys.insert(Key);
        Ctx.MissingImageDetails[Key] = Source.isRuntimeImage()
                                                ? Source.DarwinPath + ": " + Error
                                                : Error;
      } else {
        Ctx.ParseWarnings.push_back(Source.DisplayName + ": " + Error);
      }
      // A parse failure is not equivalent to a missing provider. Keep the key
      // distinct so the report does not overstate certainty.
      continue;
    }
    auto Inserted = Ctx.Images.emplace(Key, std::move(Parsed));
    const Image &ImageRef = Inserted.first->second;
    if (!ImageRef.ImportCoverageComplete)
      Ctx.ParseWarnings.push_back(Source.DisplayName +
                                  ": no chained imports or classic undefined-symbol table; import coverage incomplete");

    for (const Dependency &Dep : ImageRef.Dependencies) {
      if (isHostDependency(Dep.Name))
        continue;
      ImageSource DependencySource;
      if (!dependencySource(Ctx, Dep, DependencySource, Error)) {
        Ctx.ParseWarnings.push_back(Source.DisplayName + " dependency " +
                                    Dep.Name + ": " + Error);
        continue;
      }
      if (Queued.insert(DependencySource.Key).second)
        Queue.push_back(std::move(DependencySource));
    }
  }
}

inline std::vector<Binding> findMissingBindings(Context &Ctx,
                                                std::size_t &RequiredCount,
                                                std::size_t &WeakMissingCount) {
  std::vector<Binding> Missing;
  RequiredCount = 0;
  WeakMissingCount = 0;
  const std::string MainKey = pathKey(Ctx.MainExecutable);

  for (const auto &Pair : Ctx.Images) {
    const std::string &ImporterKey = Pair.first;
    const Image &Importer = Pair.second;
    for (const Import &Entry : Importer.Imports) {
      if (!Entry.Weak)
        ++RequiredCount;

      bool Present = false;
      std::string Expected;
      std::string Reason;
      if (Entry.Ordinal > 0) {
        if (static_cast<std::size_t>(Entry.Ordinal) >
            Importer.Dependencies.size()) {
          Expected = "<invalid-ordinal>";
          Reason = "library ordinal is outside dependency table";
        } else {
          const Dependency &Dep = Importer.Dependencies[Entry.Ordinal - 1];
          Expected = Dep.Name;
          Present = providerHasSymbol(Ctx, dependencyKey(Ctx, Dep), Entry.Name,
                                      Reason);
        }
      } else if (Entry.Ordinal == BindSpecialDylibSelf) {
        Expected = "<self>";
        Present = providerHasSymbol(Ctx, ImporterKey, Entry.Name, Reason);
      } else if (Entry.Ordinal == BindSpecialDylibMainExecutable) {
        Expected = "<main-executable>";
        Present = providerHasSymbol(Ctx, MainKey, Entry.Name, Reason);
      } else if (Entry.Ordinal == BindSpecialDylibFlatLookup ||
                 Entry.Ordinal == BindSpecialDylibWeakLookup) {
        Expected = Entry.Ordinal == BindSpecialDylibFlatLookup
                       ? "<flat-lookup>"
                       : "<weak-lookup>";
        Present = flatLookupHasSymbol(Ctx, Entry.Name);
        if (!Present)
          Reason = "symbol absent from closure-wide flat namespace";
      } else {
        Expected = "<special-ordinal>";
        Reason = "unsupported special library ordinal";
      }

      if (Present)
        continue;
      Binding MissingEntry{Importer.DisplayName, Entry.Ordinal, Expected,
                           Entry.Name, Entry.Weak, Reason};
      Missing.push_back(std::move(MissingEntry));
      if (Entry.Weak)
        ++WeakMissingCount;
    }
  }
  return Missing;
}

inline std::string csvEscape(const std::string &Value) {
  if (Value.find_first_of(",\"\r\n") == std::string::npos)
    return Value;
  std::string Out = "\"";
  for (char C : Value) {
    if (C == '"')
      Out += "\"\"";
    else
      Out += C;
  }
  Out += '"';
  return Out;
}

inline void writeCsv(const std::vector<Binding> &Missing,
                     const std::filesystem::path &Path) {
  std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
  if (!Out) {
    std::fprintf(stderr, "[static-symbol-audit] cannot write CSV: %s\n",
                 Path.string().c_str());
    return;
  }
  Out << "symbol,expected_library,importing_image,ordinal,weak,reason\r\n";
  for (const Binding &Entry : Missing) {
    Out << csvEscape(Entry.Symbol) << ',' << csvEscape(Entry.ExpectedLibrary)
        << ',' << csvEscape(Entry.Importer) << ',' << Entry.Ordinal << ','
        << (Entry.Weak ? "true" : "false") << ',' << csvEscape(Entry.Reason)
        << "\r\n";
  }
}

inline void report(Context &Ctx, const std::vector<Binding> &Missing,
                   std::size_t RequiredCount, std::size_t WeakMissingCount) {
  std::size_t RequiredMissing = 0;
  for (const Binding &Entry : Missing)
    if (!Entry.Weak)
      ++RequiredMissing;

  std::printf("[static-symbol-audit] closure: %zu Mach-O image(s), %zu missing image path(s), %zu parse/coverage warning(s)\n",
              Ctx.Images.size(), Ctx.MissingImageKeys.size(),
              Ctx.ParseWarnings.size());
  std::printf("[static-symbol-audit] bindings: %zu required, %zu required missing, %zu weak missing\n",
              RequiredCount, RequiredMissing, WeakMissingCount);

  std::map<std::string, std::vector<const Binding *>> Groups;
  for (const Binding &Entry : Missing)
    Groups[Entry.ExpectedLibrary].push_back(&Entry);

  for (const auto &Group : Groups) {
    std::size_t GroupRequired = 0;
    for (const Binding *Entry : Group.second)
      if (!Entry->Weak)
        ++GroupRequired;
    std::printf("[static-symbol-audit] MISSING GROUP %s: %zu binding(s), %zu required\n",
                Group.first.c_str(), Group.second.size(), GroupRequired);
    for (const Binding *Entry : Group.second) {
      std::printf("[static-symbol-audit]   %s ordinal %d :: %s%s (%s)\n",
                  Entry->Importer.c_str(), Entry->Ordinal,
                  Entry->Symbol.c_str(), Entry->Weak ? " [weak]" : "",
                  Entry->Reason.c_str());
    }
  }

  for (const std::string &Warning : Ctx.ParseWarnings)
    std::printf("[static-symbol-audit] WARNING %s\n", Warning.c_str());
  for (const std::string &MissingKey : Ctx.MissingImageKeys) {
    const auto Detail = Ctx.MissingImageDetails.find(MissingKey);
    std::printf("[static-symbol-audit] MISSING IMAGE %s\n",
                Detail == Ctx.MissingImageDetails.end()
                    ? MissingKey.c_str()
                    : Detail->second.c_str());
  }
}

} // namespace static_symbol_audit_detail

inline void reportStaticClosureSymbolAudit(const char *ImagePath,
                                           const RuntimeRootStore &Store) {
  using namespace static_symbol_audit_detail;
  if (!ImagePath || !*ImagePath)
    return;

  Context Ctx;
  Ctx.RuntimeStore = &Store;
  Ctx.MainExecutable = std::filesystem::path(ImagePath).lexically_normal();
  Ctx.BridgePath = bridgePath();
  if (!Ctx.BridgePath.empty())
    Ctx.Bridge = LoadLibraryW(Ctx.BridgePath.c_str());
  if (!Ctx.Bridge)
    std::fprintf(stderr,
                 "[static-symbol-audit] native bridge could not be loaded; host-bound imports will be reported unresolved\n");

  collectClosure(Ctx, Ctx.MainExecutable);
  std::size_t RequiredCount = 0;
  std::size_t WeakMissingCount = 0;
  std::vector<Binding> Missing =
      findMissingBindings(Ctx, RequiredCount, WeakMissingCount);
  std::sort(Missing.begin(), Missing.end(),
            [](const Binding &A, const Binding &B) {
              return std::tie(A.ExpectedLibrary, A.Importer, A.Symbol, A.Ordinal) <
                     std::tie(B.ExpectedLibrary, B.Importer, B.Symbol, B.Ordinal);
            });

  report(Ctx, Missing, RequiredCount, WeakMissingCount);

  const char *CsvEnv = std::getenv("IPASIM_SYMBOL_AUDIT_CSV");
  std::filesystem::path CsvPath =
      CsvEnv && *CsvEnv ? std::filesystem::path(CsvEnv)
                        : std::filesystem::path("missing_symbols.csv");
  writeCsv(Missing, CsvPath);
  std::printf("[static-symbol-audit] CSV: %s\n", CsvPath.string().c_str());

  if (Ctx.Bridge)
    FreeLibrary(Ctx.Bridge);
}

} // namespace ipasim::probe
