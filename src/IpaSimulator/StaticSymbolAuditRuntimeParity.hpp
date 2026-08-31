#pragma once

#include "StaticSymbolAudit.hpp"

#include <set>
#include <string>

namespace ipasim::probe {
namespace static_symbol_audit_runtime_parity_detail {

using namespace static_symbol_audit_detail;

inline bool runtimeRejectsExportFlags(std::uint64_t Flags) {
  const std::uint64_t Kind = Flags & ExportKindMask;
  // LoadedDylib::findSymbol currently rejects modern thread-local exports and
  // stub/resolver exports rather than returning a usable address. The static
  // audit must make the same distinction or it could hide a future runtime
  // boundary as already provided.
  return Kind == ExportKindThreadLocal ||
         (Flags & ExportStubAndResolver) != 0;
}

inline bool collectRuntimeUnsupportedExportsFromTrie(
    const std::uint8_t *Data, std::size_t Size, std::set<std::string> &Names,
    std::string &Error) {
  if (!Data || Size == 0)
    return true;

  const std::uint8_t *End = Data + Size;
  std::set<std::uint64_t> ActiveNodes;
  std::function<bool(std::uint64_t, const std::string &, unsigned)> Visit;
  Visit = [&](std::uint64_t NodeOffset, const std::string &Prefix,
              unsigned Depth) -> bool {
    if (Depth > 1024) {
      Error = "runtime-parity exports trie exceeds maximum depth";
      return false;
    }
    if (NodeOffset >= Size) {
      Error = "runtime-parity exports trie node is outside payload";
      return false;
    }
    if (!ActiveNodes.insert(NodeOffset).second) {
      Error = "runtime-parity exports trie contains a cycle";
      return false;
    }

    const std::uint8_t *Cursor = Data + NodeOffset;
    std::uint64_t TerminalSize = 0;
    if (!readUleb128(Cursor, End, TerminalSize) ||
        TerminalSize > std::uint64_t(End - Cursor)) {
      ActiveNodes.erase(NodeOffset);
      Error = "invalid runtime-parity exports trie terminal size";
      return false;
    }
    const std::uint8_t *TerminalEnd = Cursor + TerminalSize;

    if (TerminalSize != 0) {
      std::uint64_t Flags = 0;
      if (!readUleb128(Cursor, TerminalEnd, Flags)) {
        ActiveNodes.erase(NodeOffset);
        Error = "invalid runtime-parity exports trie terminal flags";
        return false;
      }
      if ((Flags & ExportReexport) == 0 && runtimeRejectsExportFlags(Flags))
        Names.insert(Prefix);
    }

    Cursor = TerminalEnd;
    if (Cursor == End) {
      ActiveNodes.erase(NodeOffset);
      Error = "runtime-parity exports trie node is missing child count";
      return false;
    }
    const std::uint8_t ChildCount = *Cursor++;
    for (std::uint8_t Child = 0; Child != ChildCount; ++Child) {
      const std::uint8_t *EdgeStart = Cursor;
      while (Cursor != End && *Cursor)
        ++Cursor;
      if (Cursor == End) {
        ActiveNodes.erase(NodeOffset);
        Error = "unterminated runtime-parity exports trie edge";
        return false;
      }
      std::string Edge(reinterpret_cast<const char *>(EdgeStart),
                       reinterpret_cast<const char *>(Cursor));
      ++Cursor;
      std::uint64_t ChildOffset = 0;
      if (!readUleb128(Cursor, End, ChildOffset)) {
        ActiveNodes.erase(NodeOffset);
        Error = "invalid runtime-parity exports trie child offset";
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

inline bool collectRuntimeUnsupportedExports(
    const std::filesystem::path &Path, std::set<std::string> &Names,
    std::string &Error) {
  std::vector<std::uint8_t> Data;
  if (!loadFile(Path, Data)) {
    Error = "cannot read image for runtime export parity";
    return false;
  }

  std::size_t SliceOffset = 0;
  std::size_t SliceSize = 0;
  if (!selectArm64Slice(Data, SliceOffset, SliceSize, Error))
    return false;
  if (!rangeValid(SliceOffset, 32, Data.size()) || SliceSize < 32) {
    Error = "runtime-parity Mach-O header is truncated";
    return false;
  }

  const std::uint32_t CommandCount = readLe32(Data, SliceOffset + 16);
  const std::uint32_t CommandBytes = readLe32(Data, SliceOffset + 20);
  const std::size_t CommandsBegin = SliceOffset + 32;
  if (!rangeValid(CommandsBegin, CommandBytes, Data.size()) ||
      CommandBytes > SliceSize - 32) {
    Error = "runtime-parity load commands are outside ARM64 slice";
    return false;
  }

  std::uint32_t ExportsOffset = 0;
  std::uint32_t ExportsSize = 0;
  std::size_t Cursor = CommandsBegin;
  const std::size_t CommandsEnd = CommandsBegin + CommandBytes;
  for (std::uint32_t Index = 0; Index != CommandCount; ++Index) {
    if (!rangeValid(Cursor, 8, CommandsEnd)) {
      Error = "runtime-parity load command list ends early";
      return false;
    }
    const std::uint32_t Command = readLe32(Data, Cursor);
    const std::uint32_t CommandSize = readLe32(Data, Cursor + 4);
    if (CommandSize < 8 || !rangeValid(Cursor, CommandSize, CommandsEnd)) {
      Error = "invalid runtime-parity load command size";
      return false;
    }
    if (Command == LcDyldExportsTrie) {
      if (CommandSize < 16) {
        Error = "truncated runtime-parity LC_DYLD_EXPORTS_TRIE";
        return false;
      }
      ExportsOffset = readLe32(Data, Cursor + 8);
      ExportsSize = readLe32(Data, Cursor + 12);
      break;
    }
    Cursor += CommandSize;
  }

  if (ExportsSize == 0)
    return true;
  if (ExportsOffset > SliceSize || ExportsSize > SliceSize - ExportsOffset ||
      !rangeValid(SliceOffset + ExportsOffset, ExportsSize, Data.size())) {
    Error = "runtime-parity exports trie is outside ARM64 slice";
    return false;
  }

  return collectRuntimeUnsupportedExportsFromTrie(
      Data.data() + SliceOffset + ExportsOffset, ExportsSize, Names, Error);
}

inline void applyRuntimeExportParity(Context &Ctx) {
  for (auto &Pair : Ctx.Images) {
    Image &ImageRef = Pair.second;
    std::set<std::string> Unsupported;
    std::string Error;
    if (!collectRuntimeUnsupportedExports(ImageRef.Path, Unsupported, Error)) {
      Ctx.ParseWarnings.push_back(
          ImageRef.Path.string() + ": runtime export parity check failed: " +
          Error);
      continue;
    }
    for (const std::string &Name : Unsupported) {
      auto It = ImageRef.Exports.find(Name);
      if (It != ImageRef.Exports.end())
        It->second.Type = ExportEntry::Kind::Unsupported;
    }
  }
}

} // namespace static_symbol_audit_runtime_parity_detail

inline void reportRuntimeParityStaticClosureSymbolAudit(
    const char *ImagePath, const char *RuntimeRoot) {
  using namespace static_symbol_audit_detail;
  using namespace static_symbol_audit_runtime_parity_detail;
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
  applyRuntimeExportParity(Ctx);

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
