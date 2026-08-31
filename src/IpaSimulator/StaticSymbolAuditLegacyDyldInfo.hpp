#pragma once

#include "StaticSymbolAudit.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ipasim::probe {
namespace static_symbol_audit_detail {

// Older and extracted Mach-O images can carry their export trie inside the
// 48-byte dyld_info_command rather than LC_DYLD_EXPORTS_TRIE. Apple dyld treats
// LC_DYLD_INFO and LC_DYLD_INFO_ONLY as first-class sources of export_off /
// export_size. The base audit already has the correct trie parser; this layer
// supplies the missing load-command coverage without changing namespace rules.
constexpr std::uint32_t LcDyldInfo = 0x00000022U;
constexpr std::uint32_t LcDyldInfoOnly = 0x80000022U;
constexpr std::uint32_t DyldInfoCommandSize = 48U;

struct LegacyDyldInfoStats {
  std::size_t ImagesWithExportTrie = 0;
  std::size_t ExportEntriesMerged = 0;
};

inline bool parseLegacyDyldInfoExports(
    const std::vector<std::uint8_t> &Data, std::size_t SliceOffset,
    std::size_t SliceSize, std::map<std::string, ExportEntry> &Exports,
    bool &FoundDyldInfo, std::string &Error) {
  FoundDyldInfo = false;
  Exports.clear();

  if (!rangeValid(SliceOffset, 32, Data.size()) || SliceSize < 32) {
    Error = "Mach-O header is truncated while scanning legacy dyld info";
    return false;
  }

  const std::uint32_t CommandCount = readLe32(Data, SliceOffset + 16);
  const std::uint32_t CommandBytes = readLe32(Data, SliceOffset + 20);
  const std::size_t CommandsBegin = SliceOffset + 32;
  if (!rangeValid(CommandsBegin, CommandBytes, Data.size()) ||
      CommandBytes > SliceSize - 32) {
    Error = "Mach-O load commands are outside ARM64 slice while scanning legacy dyld info";
    return false;
  }

  std::uint32_t ExportOffset = 0;
  std::uint32_t ExportSize = 0;
  std::size_t Cursor = CommandsBegin;
  const std::size_t CommandsEnd = CommandsBegin + CommandBytes;

  for (std::uint32_t Index = 0; Index != CommandCount; ++Index) {
    if (!rangeValid(Cursor, 8, CommandsEnd)) {
      Error = "Mach-O load command list ends early while scanning legacy dyld info";
      return false;
    }

    const std::uint32_t Command = readLe32(Data, Cursor);
    const std::uint32_t CommandSize = readLe32(Data, Cursor + 4);
    if (CommandSize < 8 || !rangeValid(Cursor, CommandSize, CommandsEnd)) {
      Error = "invalid Mach-O load command size while scanning legacy dyld info";
      return false;
    }

    if (Command == LcDyldInfo || Command == LcDyldInfoOnly) {
      // dyld itself requires sizeof(dyld_info_command), not merely enough bytes
      // to reach export_off/export_size. Match that format validation.
      if (CommandSize != DyldInfoCommandSize) {
        Error = "LC_DYLD_INFO load command size is not 48 bytes";
        return false;
      }
      if (FoundDyldInfo) {
        Error = "multiple LC_DYLD_INFO load commands";
        return false;
      }
      FoundDyldInfo = true;
      ExportOffset = readLe32(Data, Cursor + 40);
      ExportSize = readLe32(Data, Cursor + 44);
    }

    Cursor += CommandSize;
  }

  if (!FoundDyldInfo || ExportSize == 0)
    return true;

  if (ExportOffset > SliceSize || ExportSize > SliceSize - ExportOffset ||
      !rangeValid(SliceOffset + ExportOffset, ExportSize, Data.size())) {
    Error = "legacy dyld export trie is outside ARM64 slice";
    return false;
  }

  return parseExportsTrie(Data.data() + SliceOffset + ExportOffset, ExportSize,
                          Exports, Error);
}

inline LegacyDyldInfoStats augmentLegacyDyldInfoExports(Context &Ctx) {
  LegacyDyldInfoStats Stats;

  for (auto &Pair : Ctx.Images) {
    Image &ImageRef = Pair.second;
    std::vector<std::uint8_t> Data;
    if (!loadFile(ImageRef.Path, Data)) {
      Ctx.ParseWarnings.push_back(
          ImageRef.Path.string() +
          ": cannot re-read image for LC_DYLD_INFO export coverage");
      continue;
    }

    std::size_t SliceOffset = 0;
    std::size_t SliceSize = 0;
    std::string Error;
    if (!selectArm64Slice(Data, SliceOffset, SliceSize, Error)) {
      Ctx.ParseWarnings.push_back(ImageRef.Path.string() + ": " + Error);
      continue;
    }

    std::map<std::string, ExportEntry> LegacyExports;
    bool FoundDyldInfo = false;
    if (!parseLegacyDyldInfoExports(Data, SliceOffset, SliceSize, LegacyExports,
                                    FoundDyldInfo, Error)) {
      Ctx.ParseWarnings.push_back(ImageRef.Path.string() + ": " + Error);
      continue;
    }
    if (!FoundDyldInfo || LegacyExports.empty())
      continue;

    ++Stats.ImagesWithExportTrie;
    Stats.ExportEntriesMerged += LegacyExports.size();

    // The dyld export trie is authoritative over LC_SYMTAB symbol presence.
    // Overlay it after parseImage() so reexport/resolver kinds are preserved.
    for (auto &Export : LegacyExports)
      ImageRef.Exports[Export.first] = std::move(Export.second);
  }

  return Stats;
}

struct RankedMissing {
  std::string ExpectedLibrary;
  std::string Symbol;
  std::size_t RequiredBindings = 0;
  std::size_t WeakBindings = 0;
  std::set<std::string> RequiredImporters;
  std::set<std::string> WeakImporters;
  std::set<std::string> Reasons;
};

inline std::vector<RankedMissing>
buildMissingRanking(const std::vector<Binding> &Missing) {
  using Key = std::pair<std::string, std::string>;
  std::map<Key, RankedMissing> Rows;

  for (const Binding &Entry : Missing) {
    const Key K{Entry.ExpectedLibrary, Entry.Symbol};
    RankedMissing &Row = Rows[K];
    Row.ExpectedLibrary = Entry.ExpectedLibrary;
    Row.Symbol = Entry.Symbol;
    Row.Reasons.insert(Entry.Reason);
    if (Entry.Weak) {
      ++Row.WeakBindings;
      Row.WeakImporters.insert(Entry.Importer);
    } else {
      ++Row.RequiredBindings;
      Row.RequiredImporters.insert(Entry.Importer);
    }
  }

  std::vector<RankedMissing> Ranked;
  Ranked.reserve(Rows.size());
  for (auto &Pair : Rows)
    Ranked.push_back(std::move(Pair.second));

  std::sort(Ranked.begin(), Ranked.end(),
            [](const RankedMissing &A, const RankedMissing &B) {
              if ((A.RequiredBindings != 0) != (B.RequiredBindings != 0))
                return A.RequiredBindings != 0;
              if (A.RequiredBindings != B.RequiredBindings)
                return A.RequiredBindings > B.RequiredBindings;
              if (A.RequiredImporters.size() != B.RequiredImporters.size())
                return A.RequiredImporters.size() > B.RequiredImporters.size();
              if (A.WeakBindings != B.WeakBindings)
                return A.WeakBindings > B.WeakBindings;
              return std::tie(A.ExpectedLibrary, A.Symbol) <
                     std::tie(B.ExpectedLibrary, B.Symbol);
            });
  return Ranked;
}

inline std::string joinReasons(const std::set<std::string> &Reasons) {
  std::string Result;
  for (const std::string &Reason : Reasons) {
    if (!Result.empty())
      Result += " | ";
    Result += Reason;
  }
  return Result;
}

inline std::filesystem::path
rankedCsvPath(const std::filesystem::path &DetailedPath) {
  const std::filesystem::path Parent = DetailedPath.parent_path();
  const std::string Extension = DetailedPath.extension().string();
  if (Extension.empty())
    return Parent / (DetailedPath.filename().string() + "_ranked.csv");
  return Parent /
         (DetailedPath.stem().string() + "_ranked" + Extension);
}

inline void writeRankedCsv(const std::vector<RankedMissing> &Ranked,
                           const std::filesystem::path &Path) {
  std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
  if (!Out) {
    std::fprintf(stderr,
                 "[static-symbol-audit] cannot write ranked CSV: %s\n",
                 Path.string().c_str());
    return;
  }

  Out << "expected_library,symbol,required_bindings,required_importers,"
         "weak_bindings,weak_importers,reasons\r\n";
  for (const RankedMissing &Row : Ranked) {
    Out << csvEscape(Row.ExpectedLibrary) << ',' << csvEscape(Row.Symbol) << ','
        << Row.RequiredBindings << ',' << Row.RequiredImporters.size() << ','
        << Row.WeakBindings << ',' << Row.WeakImporters.size() << ','
        << csvEscape(joinReasons(Row.Reasons)) << "\r\n";
  }
}

inline void reportRankedMissing(const std::vector<RankedMissing> &Ranked) {
  std::size_t RequiredPairs = 0;
  for (const RankedMissing &Row : Ranked)
    if (Row.RequiredBindings != 0)
      ++RequiredPairs;

  std::printf(
      "[static-symbol-audit] unique required provider/symbol pairs: %zu\n",
      RequiredPairs);

  constexpr std::size_t MaxConsoleRows = 50;
  std::size_t Printed = 0;
  for (const RankedMissing &Row : Ranked) {
    if (Row.RequiredBindings == 0 || Printed == MaxConsoleRows)
      break;
    std::printf(
        "[static-symbol-audit] PRIORITY %zu binding(s), %zu importer(s) :: %s :: %s\n",
        Row.RequiredBindings, Row.RequiredImporters.size(),
        Row.ExpectedLibrary.c_str(), Row.Symbol.c_str());
    ++Printed;
  }
  if (RequiredPairs > Printed)
    std::printf(
        "[static-symbol-audit] priority console view truncated to %zu of %zu required provider/symbol pairs; ranked CSV contains all rows\n",
        Printed, RequiredPairs);
}

inline bool runLegacyDyldInfoAuditSelfTest() {
  auto Fail = [](const char *Message) {
    std::fprintf(stderr,
                 "[static-symbol-audit-self-test] FAIL legacy dyld info: %s\n",
                 Message);
    return false;
  };

  auto Write32 = [](std::vector<std::uint8_t> &Data, std::size_t Offset,
                    std::uint32_t Value) {
    Data[Offset] = static_cast<std::uint8_t>(Value);
    Data[Offset + 1] = static_cast<std::uint8_t>(Value >> 8);
    Data[Offset + 2] = static_cast<std::uint8_t>(Value >> 16);
    Data[Offset + 3] = static_cast<std::uint8_t>(Value >> 24);
  };

  auto CheckCommand = [&](std::uint32_t Command) {
    // One 48-byte dyld_info_command followed by a minimal export trie for
    // regular symbol `_legacy` at offset 80.
    const std::uint8_t Trie[] = {
        0x00, 0x01, '_', 'l', 'e', 'g', 'a', 'c', 'y', 0x00, 0x0b,
        0x02, 0x00, 0x00, 0x00,
    };
    std::vector<std::uint8_t> Data(80 + sizeof(Trie), 0);
    Write32(Data, 0, MhMagic64);
    Write32(Data, 4, CpuTypeArm64);
    Write32(Data, 16, 1);
    Write32(Data, 20, DyldInfoCommandSize);
    Write32(Data, 32, Command);
    Write32(Data, 36, DyldInfoCommandSize);
    Write32(Data, 72, 80);
    Write32(Data, 76, static_cast<std::uint32_t>(sizeof(Trie)));
    std::copy(std::begin(Trie), std::end(Trie), Data.begin() + 80);

    std::map<std::string, ExportEntry> Exports;
    bool Found = false;
    std::string Error;
    if (!parseLegacyDyldInfoExports(Data, 0, Data.size(), Exports, Found,
                                    Error))
      return false;
    auto It = Exports.find("_legacy");
    return Found && It != Exports.end() &&
           It->second.Type == ExportEntry::Kind::Direct;
  };

  if (!CheckCommand(LcDyldInfo))
    return Fail("LC_DYLD_INFO export trie was not recognized");
  if (!CheckCommand(LcDyldInfoOnly))
    return Fail("LC_DYLD_INFO_ONLY export trie was not recognized");

  // Ranking is allowed to deduplicate repeated bindings only within the same
  // two-level provider namespace. The same symbol expected from another dylib
  // is a distinct compatibility item.
  std::vector<Binding> Missing{
      {"importer-a", 2, "provider-a", "_same", false, "missing"},
      {"importer-b", 3, "provider-a", "_same", false, "missing"},
      {"importer-c", 4, "provider-b", "_same", false, "missing"},
  };
  const std::vector<RankedMissing> Ranked = buildMissingRanking(Missing);
  if (Ranked.size() != 2 || Ranked[0].ExpectedLibrary != "provider-a" ||
      Ranked[0].Symbol != "_same" || Ranked[0].RequiredBindings != 2 ||
      Ranked[0].RequiredImporters.size() != 2 ||
      Ranked[1].ExpectedLibrary != "provider-b" ||
      Ranked[1].RequiredBindings != 1) {
    return Fail("provider-aware ranking flattened two-level namespaces");
  }

  return true;
}

inline void reportStaticClosureSymbolAuditComplete(const char *ImagePath,
                                                   const char *RuntimeRoot) {
  if (!ImagePath || !*ImagePath || !RuntimeRoot || !*RuntimeRoot)
    return;

  Context Ctx;
  Ctx.RuntimeRoot = std::filesystem::path(RuntimeRoot).lexically_normal();
  Ctx.MainExecutable = std::filesystem::path(ImagePath).lexically_normal();
  Ctx.BridgePath = bridgePath();
  if (!Ctx.BridgePath.empty())
    Ctx.Bridge = LoadLibraryW(Ctx.BridgePath.c_str());
  if (!Ctx.Bridge)
    std::fprintf(stderr,
                 "[static-symbol-audit] native bridge could not be loaded; host-bound imports will be reported unresolved\n");

  collectClosure(Ctx, Ctx.MainExecutable);
  const LegacyDyldInfoStats LegacyStats = augmentLegacyDyldInfoExports(Ctx);

  std::size_t RequiredCount = 0;
  std::size_t WeakMissingCount = 0;
  std::vector<Binding> Missing =
      findMissingBindings(Ctx, RequiredCount, WeakMissingCount);
  std::sort(Missing.begin(), Missing.end(),
            [](const Binding &A, const Binding &B) {
              return std::tie(A.ExpectedLibrary, A.Importer, A.Symbol, A.Ordinal) <
                     std::tie(B.ExpectedLibrary, B.Importer, B.Symbol, B.Ordinal);
            });

  std::printf(
      "[static-symbol-audit] legacy dyld-info export coverage: %zu image(s), %zu export entry/entries merged\n",
      LegacyStats.ImagesWithExportTrie, LegacyStats.ExportEntriesMerged);
  report(Ctx, Missing, RequiredCount, WeakMissingCount);

  const std::vector<RankedMissing> Ranked = buildMissingRanking(Missing);
  reportRankedMissing(Ranked);

  const char *CsvEnv = std::getenv("IPASIM_SYMBOL_AUDIT_CSV");
  const std::filesystem::path CsvPath =
      CsvEnv && *CsvEnv ? std::filesystem::path(CsvEnv)
                        : std::filesystem::path("missing_symbols.csv");
  writeCsv(Missing, CsvPath);
  std::printf("[static-symbol-audit] CSV: %s\n", CsvPath.string().c_str());

  const std::filesystem::path RankedPath = rankedCsvPath(CsvPath);
  writeRankedCsv(Ranked, RankedPath);
  std::printf("[static-symbol-audit] ranked CSV: %s\n",
              RankedPath.string().c_str());

  if (Ctx.Bridge)
    FreeLibrary(Ctx.Bridge);
}

} // namespace static_symbol_audit_detail

using static_symbol_audit_detail::reportStaticClosureSymbolAuditComplete;
using static_symbol_audit_detail::runLegacyDyldInfoAuditSelfTest;

} // namespace ipasim::probe
