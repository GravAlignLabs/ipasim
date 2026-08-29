// ModernMachO.cpp: Modern dyld linkedit support for ARM64 Mach-O images.

#include "ipasim/ModernMachO.hpp"

#include <LIEF/MachO/Binary.hpp>
#include <LIEF/MachO/SegmentCommand.hpp>
#include <llvm/BinaryFormat/MachO.h>

#include <cstring>
#include <functional>
#include <limits>
#include <set>
#include <utility>

using namespace ipasim;

namespace {

// These commands post-date the LLVM/LIEF snapshots vendored by ipaSim.
constexpr uint32_t LC_DYLD_EXPORTS_TRIE_VALUE = 0x80000033U;
constexpr uint32_t LC_DYLD_CHAINED_FIXUPS_VALUE = 0x80000034U;

constexpr uint32_t DYLD_CHAINED_IMPORT = 1;
constexpr uint32_t DYLD_CHAINED_IMPORT_ADDEND = 2;
constexpr uint32_t DYLD_CHAINED_IMPORT_ADDEND64 = 3;

constexpr uint16_t DYLD_CHAINED_PTR_64 = 2;
constexpr uint16_t DYLD_CHAINED_PTR_64_OFFSET = 6;
constexpr uint16_t DYLD_CHAINED_PTR_START_NONE = 0xFFFF;
constexpr uint16_t DYLD_CHAINED_PTR_START_MULTI = 0x8000;
constexpr uint16_t DYLD_CHAINED_PTR_START_LAST = 0x8000;

constexpr uint64_t EXPORT_SYMBOL_FLAGS_KIND_MASK = 0x03;
constexpr uint64_t EXPORT_SYMBOL_FLAGS_KIND_REGULAR = 0x00;
constexpr uint64_t EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL = 0x01;
constexpr uint64_t EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE = 0x02;
constexpr uint64_t EXPORT_SYMBOL_FLAGS_REEXPORT = 0x08;
constexpr uint64_t EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER = 0x10;

struct LinkEditDataCommandRaw {
  uint32_t cmd;
  uint32_t cmdsize;
  uint32_t dataoff;
  uint32_t datasize;
};

struct ChainedFixupsHeaderRaw {
  uint32_t fixups_version;
  uint32_t starts_offset;
  uint32_t imports_offset;
  uint32_t symbols_offset;
  uint32_t imports_count;
  uint32_t imports_format;
  uint32_t symbols_format;
};

template <typename T>
bool readPod(const uint8_t *Data, size_t Size, size_t Offset, T &Value) {
  if (Offset > Size || sizeof(T) > Size - Offset)
    return false;
  std::memcpy(&Value, Data + Offset, sizeof(T));
  return true;
}

int signExtendOrdinal(uint64_t Value, unsigned Bits) {
  const uint64_t Sign = uint64_t(1) << (Bits - 1);
  const uint64_t Mask = (uint64_t(1) << Bits) - 1;
  Value &= Mask;
  return static_cast<int>((Value ^ Sign) - Sign);
}

bool readCString(const uint8_t *Start, const uint8_t *End, std::string &Value) {
  const uint8_t *Cursor = Start;
  while (Cursor != End && *Cursor)
    ++Cursor;
  if (Cursor == End)
    return false;
  Value.assign(reinterpret_cast<const char *>(Start),
               reinterpret_cast<const char *>(Cursor));
  return true;
}

bool checkedAdd(uint64_t A, uint64_t B, uint64_t &Result) {
  if (B > std::numeric_limits<uint64_t>::max() - A)
    return false;
  Result = A + B;
  return true;
}

bool applySlide(uint64_t Value, uint64_t PreferredImageBase,
                uint64_t LoadedImageBase, uint64_t &Result) {
  if (LoadedImageBase >= PreferredImageBase)
    return checkedAdd(Value, LoadedImageBase - PreferredImageBase, Result);

  const uint64_t Delta = PreferredImageBase - LoadedImageBase;
  if (Value < Delta)
    return false;
  Result = Value - Delta;
  return true;
}

bool parseImports(const uint8_t *Data, size_t Size,
                  const ChainedFixupsHeaderRaw &Header,
                  std::vector<ChainedImport> &Imports, std::string &Error) {
  // Apple's format 1 is a zlib-compressed symbol pool. Do not pretend an
  // uncompressed parser can consume it.
  if (Header.symbols_format != 0) {
    Error = "compressed chained-fixup symbol pools are not supported yet";
    return false;
  }

  size_t ImportSize = 0;
  switch (Header.imports_format) {
  case DYLD_CHAINED_IMPORT:
    ImportSize = 4;
    break;
  case DYLD_CHAINED_IMPORT_ADDEND:
    ImportSize = 8;
    break;
  case DYLD_CHAINED_IMPORT_ADDEND64:
    ImportSize = 16;
    break;
  default:
    Error = "unknown chained-fixup imports format " +
            std::to_string(Header.imports_format);
    return false;
  }

  if (Header.imports_offset > Size || Header.symbols_offset > Size) {
    Error = "chained-fixup imports/symbol offset is outside payload";
    return false;
  }
  if (Header.imports_count >
      (Size - Header.imports_offset) / ImportSize) {
    Error = "chained-fixup import table extends outside payload";
    return false;
  }

  const size_t ImportsEnd =
      Header.imports_offset + size_t(Header.imports_count) * ImportSize;
  if (ImportsEnd > Header.symbols_offset) {
    Error = "chained-fixup import table overlaps symbol pool";
    return false;
  }

  Imports.clear();
  Imports.reserve(Header.imports_count);
  for (uint32_t I = 0; I != Header.imports_count; ++I) {
    const size_t Offset = Header.imports_offset + size_t(I) * ImportSize;
    ChainedImport Import;
    uint32_t NameOffset = 0;

    if (Header.imports_format == DYLD_CHAINED_IMPORT ||
        Header.imports_format == DYLD_CHAINED_IMPORT_ADDEND) {
      uint32_t Raw = 0;
      if (!readPod(Data, Size, Offset, Raw)) {
        Error = "cannot read chained-fixup import";
        return false;
      }
      Import.LibOrdinal = signExtendOrdinal(Raw & 0xFFU, 8);
      Import.WeakImport = ((Raw >> 8) & 1U) != 0;
      NameOffset = Raw >> 9;

      if (Header.imports_format == DYLD_CHAINED_IMPORT_ADDEND) {
        int32_t Addend = 0;
        if (!readPod(Data, Size, Offset + 4, Addend)) {
          Error = "cannot read chained-fixup import addend";
          return false;
        }
        // Preserve the signed 32-bit addend as pointer-width two's-complement
        // arithmetic. This matches dyld's eventual uint64_t target addition.
        Import.Addend = static_cast<uint64_t>(static_cast<int64_t>(Addend));
      }
    } else {
      uint64_t Raw = 0;
      uint64_t Addend = 0;
      if (!readPod(Data, Size, Offset, Raw) ||
          !readPod(Data, Size, Offset + 8, Addend)) {
        Error = "cannot read 64-bit chained-fixup import";
        return false;
      }
      if (((Raw >> 17) & 0x7FFFU) != 0) {
        Error = "64-bit chained-fixup import has non-zero reserved bits";
        return false;
      }
      Import.LibOrdinal = signExtendOrdinal(Raw & 0xFFFFU, 16);
      Import.WeakImport = ((Raw >> 16) & 1U) != 0;
      NameOffset = static_cast<uint32_t>(Raw >> 32);
      Import.Addend = Addend;
    }

    uint64_t SymbolOffset64 = 0;
    if (!checkedAdd(Header.symbols_offset, NameOffset, SymbolOffset64) ||
        SymbolOffset64 >= Size) {
      Error = "chained-fixup symbol name offset is outside payload";
      return false;
    }

    if (!readCString(Data + static_cast<size_t>(SymbolOffset64), Data + Size,
                     Import.Name)) {
      Error = "unterminated chained-fixup symbol name";
      return false;
    }
    Imports.push_back(std::move(Import));
  }
  return true;
}

} // namespace

bool ModernMachO::readULEB128(const uint8_t *&Cursor, const uint8_t *End,
                              uint64_t &Value) {
  Value = 0;
  for (unsigned ByteIndex = 0; ByteIndex != 10; ++ByteIndex) {
    if (Cursor == End)
      return false;
    const uint8_t Byte = *Cursor++;

    // A uint64_t ULEB128 can use ten bytes, but byte ten may carry only bit 63.
    if (ByteIndex == 9 && (Byte & 0xFEU) != 0)
      return false;

    Value |= uint64_t(Byte & 0x7FU) << (ByteIndex * 7);
    if ((Byte & 0x80U) == 0)
      return true;
  }
  return false;
}

bool ModernMachO::findLinkEditCommands(const void *MachHeader,
                                       ModernLinkEditCommands &Commands,
                                       std::string &Error) {
  using namespace llvm::MachO;

  Commands = ModernLinkEditCommands{};
  Error.clear();
  if (!MachHeader) {
    Error = "null Mach-O header";
    return false;
  }

  const auto *Header = reinterpret_cast<const mach_header_64 *>(MachHeader);
  if (Header->magic != MH_MAGIC_64) {
    Error = "modern linkedit parser requires a little-endian 64-bit Mach-O";
    return false;
  }
  if (Header->sizeofcmds > 16U * 1024U * 1024U) {
    Error = "Mach-O load-command region is unreasonably large";
    return false;
  }

  const uint8_t *Cursor = reinterpret_cast<const uint8_t *>(Header + 1);
  const uint8_t *End = Cursor + Header->sizeofcmds;
  for (uint32_t I = 0; I != Header->ncmds; ++I) {
    if (Cursor > End || sizeof(load_command) > size_t(End - Cursor)) {
      Error = "Mach-O load-command list ends early";
      return false;
    }
    const auto *Command = reinterpret_cast<const load_command *>(Cursor);
    if (Command->cmdsize < sizeof(load_command) ||
        Command->cmdsize > size_t(End - Cursor)) {
      Error = "invalid Mach-O load-command size";
      return false;
    }

    if (Command->cmd == LC_DYLD_CHAINED_FIXUPS_VALUE ||
        Command->cmd == LC_DYLD_EXPORTS_TRIE_VALUE) {
      if (Command->cmdsize < sizeof(LinkEditDataCommandRaw)) {
        Error = "modern dyld linkedit command is truncated";
        return false;
      }
      LinkEditDataCommandRaw Raw{};
      std::memcpy(&Raw, Cursor, sizeof(Raw));
      if (Raw.cmd == LC_DYLD_CHAINED_FIXUPS_VALUE) {
        if (Commands.HasChainedFixups) {
          Error = "Mach-O contains duplicate LC_DYLD_CHAINED_FIXUPS commands";
          return false;
        }
        Commands.HasChainedFixups = Raw.datasize != 0;
        Commands.ChainedFixupsOffset = Raw.dataoff;
        Commands.ChainedFixupsSize = Raw.datasize;
      } else {
        if (Commands.HasExportsTrie) {
          Error = "Mach-O contains duplicate LC_DYLD_EXPORTS_TRIE commands";
          return false;
        }
        Commands.HasExportsTrie = Raw.datasize != 0;
        Commands.ExportsTrieOffset = Raw.dataoff;
        Commands.ExportsTrieSize = Raw.datasize;
      }
    }
    Cursor += Command->cmdsize;
  }
  return true;
}

const uint8_t *ModernMachO::mappedFileRange(LIEF::MachO::Binary &Binary,
                                            uint64_t Slide,
                                            uint32_t FileOffset, uint32_t Size,
                                            std::string &Error) {
  uint64_t RequestedEnd = 0;
  if (!checkedAdd(FileOffset, Size, RequestedEnd)) {
    Error = "Mach-O linkedit file range overflows";
    return nullptr;
  }

  for (LIEF::MachO::SegmentCommand &Segment : Binary.segments()) {
    const uint64_t SegmentFileStart = Segment.file_offset();
    uint64_t SegmentFileEnd = 0;
    if (!checkedAdd(SegmentFileStart, Segment.file_size(), SegmentFileEnd))
      continue;
    if (FileOffset < SegmentFileStart || RequestedEnd > SegmentFileEnd)
      continue;

    const uint64_t Delta = uint64_t(FileOffset) - SegmentFileStart;
    uint64_t DeltaEnd = 0;
    if (!checkedAdd(Delta, Size, DeltaEnd) ||
        DeltaEnd > Segment.virtual_size()) {
      Error = "Mach-O linkedit file range exceeds mapped virtual segment";
      return nullptr;
    }

    uint64_t RuntimeAddress = 0;
    if (!checkedAdd(Segment.virtual_address(), Slide, RuntimeAddress) ||
        !checkedAdd(RuntimeAddress, Delta, RuntimeAddress)) {
      Error = "mapped Mach-O linkedit address overflows";
      return nullptr;
    }
    return reinterpret_cast<const uint8_t *>(RuntimeAddress);
  }

  Error = "Mach-O linkedit file range is not contained in a mapped segment";
  return nullptr;
}

bool ModernMachO::parseExportsTrie(
    const uint8_t *Data, size_t Size,
    std::map<std::string, ModernExport> &Exports, std::string &Error) {
  Exports.clear();
  Error.clear();
  if (!Data || Size == 0)
    return true;

  const uint8_t *End = Data + Size;
  std::set<uint64_t> ActiveNodes;

  std::function<bool(uint64_t, const std::string &, unsigned)> Visit;
  Visit = [&](uint64_t NodeOffset, const std::string &Prefix,
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

    const uint8_t *Cursor = Data + NodeOffset;
    uint64_t TerminalSize = 0;
    if (!readULEB128(Cursor, End, TerminalSize) ||
        TerminalSize > uint64_t(End - Cursor)) {
      ActiveNodes.erase(NodeOffset);
      Error = "invalid exports trie terminal size";
      return false;
    }
    const uint8_t *TerminalEnd = Cursor + TerminalSize;

    if (TerminalSize != 0) {
      uint64_t Flags = 0;
      if (!readULEB128(Cursor, TerminalEnd, Flags)) {
        ActiveNodes.erase(NodeOffset);
        Error = "invalid exports trie terminal flags";
        return false;
      }

      ModernExport Export;
      if (Flags & EXPORT_SYMBOL_FLAGS_REEXPORT) {
        uint64_t Ordinal = 0;
        if (!readULEB128(Cursor, TerminalEnd, Ordinal) ||
            Ordinal > uint64_t(std::numeric_limits<int>::max())) {
          ActiveNodes.erase(NodeOffset);
          Error = "invalid exports trie re-export ordinal";
          return false;
        }
        std::string ImportName;
        if (!readCString(Cursor, TerminalEnd, ImportName)) {
          ActiveNodes.erase(NodeOffset);
          Error = "invalid exports trie re-export name";
          return false;
        }
        Export.Type = ModernExport::Kind::Reexport;
        Export.LibOrdinal = static_cast<int>(Ordinal);
        Export.ImportName = std::move(ImportName);
      } else {
        uint64_t Address = 0;
        if (!readULEB128(Cursor, TerminalEnd, Address)) {
          ActiveNodes.erase(NodeOffset);
          Error = "invalid exports trie symbol address";
          return false;
        }
        Export.Value = Address;

        switch (Flags & EXPORT_SYMBOL_FLAGS_KIND_MASK) {
        case EXPORT_SYMBOL_FLAGS_KIND_REGULAR:
          Export.Type = ModernExport::Kind::Regular;
          break;
        case EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL:
          Export.Type = ModernExport::Kind::ThreadLocal;
          break;
        case EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE:
          Export.Type = ModernExport::Kind::Absolute;
          break;
        default:
          ActiveNodes.erase(NodeOffset);
          Error = "unknown exports trie symbol kind";
          return false;
        }

        if (Flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER) {
          uint64_t Resolver = 0;
          if (!readULEB128(Cursor, TerminalEnd, Resolver)) {
            ActiveNodes.erase(NodeOffset);
            Error = "invalid exports trie resolver address";
            return false;
          }
          Export.Type = ModernExport::Kind::Resolver;
          Export.Other = Resolver;
        }
      }
      Exports[Prefix] = std::move(Export);
    }

    Cursor = TerminalEnd;
    if (Cursor == End) {
      ActiveNodes.erase(NodeOffset);
      Error = "exports trie node is missing child count";
      return false;
    }
    const uint8_t ChildCount = *Cursor++;
    for (uint8_t Child = 0; Child != ChildCount; ++Child) {
      const uint8_t *EdgeStart = Cursor;
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

      uint64_t ChildOffset = 0;
      if (!readULEB128(Cursor, End, ChildOffset)) {
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

bool ModernMachO::applyChainedFixups(const uint8_t *Data, size_t Size,
                                     uint64_t PreferredImageBase,
                                     uint64_t LoadedImageBase,
                                     const BindResolver &Resolve,
                                     std::string &Error) {
  Error.clear();
  if (!Data || Size < sizeof(ChainedFixupsHeaderRaw)) {
    Error = "chained-fixup payload is missing or truncated";
    return false;
  }

  ChainedFixupsHeaderRaw Header{};
  std::memcpy(&Header, Data, sizeof(Header));
  if (Header.fixups_version != 0) {
    Error = "unsupported chained-fixup version " +
            std::to_string(Header.fixups_version);
    return false;
  }

  std::vector<ChainedImport> Imports;
  if (!parseImports(Data, Size, Header, Imports, Error))
    return false;

  if (Header.starts_offset > Size ||
      sizeof(uint32_t) > Size - Header.starts_offset) {
    Error = "chained-fixup starts table is outside payload";
    return false;
  }

  uint32_t SegmentCount = 0;
  if (!readPod(Data, Size, Header.starts_offset, SegmentCount)) {
    Error = "cannot read chained-fixup segment count";
    return false;
  }
  const size_t SegmentOffsetsStart = Header.starts_offset + sizeof(uint32_t);
  if (SegmentCount > (Size - SegmentOffsetsStart) / sizeof(uint32_t)) {
    Error = "chained-fixup segment-offset table extends outside payload";
    return false;
  }

  for (uint32_t SegmentIndex = 0; SegmentIndex != SegmentCount;
       ++SegmentIndex) {
    uint32_t SegmentInfoRelativeOffset = 0;
    if (!readPod(Data, Size,
                 SegmentOffsetsStart + size_t(SegmentIndex) * sizeof(uint32_t),
                 SegmentInfoRelativeOffset)) {
      Error = "cannot read chained-fixup segment-info offset";
      return false;
    }
    if (SegmentInfoRelativeOffset == 0)
      continue;

    uint64_t SegmentInfoOffset64 = 0;
    if (!checkedAdd(Header.starts_offset, SegmentInfoRelativeOffset,
                    SegmentInfoOffset64) ||
        SegmentInfoOffset64 > Size) {
      Error = "chained-fixup segment-info offset is outside payload";
      return false;
    }
    const size_t SegmentInfoOffset = static_cast<size_t>(SegmentInfoOffset64);

    uint32_t SegmentInfoSize = 0;
    uint16_t PageSize = 0;
    uint16_t PointerFormat = 0;
    uint64_t SegmentOffset = 0;
    uint16_t PageCount = 0;
    if (!readPod(Data, Size, SegmentInfoOffset + 0, SegmentInfoSize) ||
        !readPod(Data, Size, SegmentInfoOffset + 4, PageSize) ||
        !readPod(Data, Size, SegmentInfoOffset + 6, PointerFormat) ||
        !readPod(Data, Size, SegmentInfoOffset + 8, SegmentOffset) ||
        !readPod(Data, Size, SegmentInfoOffset + 20, PageCount)) {
      Error = "truncated chained-fixup segment-info structure";
      return false;
    }

    constexpr size_t PageStartOffset = 22;
    if (SegmentInfoSize < PageStartOffset ||
        ((SegmentInfoSize - PageStartOffset) % sizeof(uint16_t)) != 0 ||
        SegmentInfoOffset > Size || SegmentInfoSize > Size - SegmentInfoOffset) {
      Error = "invalid chained-fixup segment-info size";
      return false;
    }
    const size_t StartSlotCount =
        (SegmentInfoSize - PageStartOffset) / sizeof(uint16_t);
    if (PageCount > StartSlotCount) {
      Error = "chained-fixup page-start table is truncated";
      return false;
    }
    if (PageSize == 0) {
      Error = "chained-fixup segment has zero page size";
      return false;
    }
    if (PointerFormat != DYLD_CHAINED_PTR_64 &&
        PointerFormat != DYLD_CHAINED_PTR_64_OFFSET) {
      Error = "unsupported chained-fixup pointer format " +
              std::to_string(PointerFormat) +
              " (arm64e/PAC formats require their real implementation)";
      return false;
    }

    auto ReadStartSlot = [&](size_t Index, uint16_t &Value) -> bool {
      if (Index >= StartSlotCount)
        return false;
      return readPod(Data, Size,
                     SegmentInfoOffset + PageStartOffset +
                         Index * sizeof(uint16_t),
                     Value);
    };

    auto ApplyChain = [&](uint32_t PageIndex,
                          uint16_t StartOffset) -> bool {
      if (StartOffset >= PageSize) {
        Error = "chained-fixup chain starts outside its page";
        return false;
      }

      uint64_t PageDelta = 0;
      if (uint64_t(PageIndex) >
          std::numeric_limits<uint64_t>::max() / uint64_t(PageSize)) {
        Error = "chained-fixup page offset overflows";
        return false;
      }
      if (!checkedAdd(SegmentOffset,
                      uint64_t(PageIndex) * uint64_t(PageSize), PageDelta)) {
        Error = "chained-fixup page address overflows";
        return false;
      }
      uint64_t PageAddress = 0;
      if (!checkedAdd(LoadedImageBase, PageDelta, PageAddress)) {
        Error = "chained-fixup runtime page address overflows";
        return false;
      }

      uint32_t OffsetInPage = StartOffset;
      for (;;) {
        if (OffsetInPage > PageSize ||
            sizeof(uint64_t) > size_t(PageSize - OffsetInPage)) {
          Error = "chained-fixup pointer extends outside its page";
          return false;
        }

        uint64_t Location = 0;
        if (!checkedAdd(PageAddress, OffsetInPage, Location)) {
          Error = "chained-fixup pointer address overflows";
          return false;
        }
        uint64_t Raw = 0;
        std::memcpy(&Raw, reinterpret_cast<const void *>(Location), sizeof(Raw));

        const bool IsBind = ((Raw >> 63) & 1U) != 0;
        const uint32_t Next = static_cast<uint32_t>((Raw >> 51) & 0xFFFU);
        uint64_t FixedValue = 0;

        if (IsBind) {
          const uint32_t ImportIndex = static_cast<uint32_t>(Raw & 0xFFFFFFU);
          const uint64_t InlineAddend = (Raw >> 24) & 0xFFU;
          if (ImportIndex >= Imports.size()) {
            Error = "chained-fixup bind references an out-of-range import";
            return false;
          }

          const ChainedImport &Import = Imports[ImportIndex];
          const uint64_t Target =
              Resolve(Import.LibOrdinal, Import.Name, Import.WeakImport);
          if (Target == 0 && !Import.WeakImport) {
            Error = "cannot resolve chained-fixup import " + Import.Name +
                    " from library ordinal " +
                    std::to_string(Import.LibOrdinal);
            return false;
          }

          // Apple's generic64 bind semantics add both sources of addend.
          FixedValue = Target + Import.Addend + InlineAddend;
        } else {
          const uint64_t Target = Raw & 0xFFFFFFFFFULL;
          const uint64_t High8 = (Raw >> 36) & 0xFFU;

          if (PointerFormat == DYLD_CHAINED_PTR_64_OFFSET) {
            // target is a runtime offset. high8 is restored before the image
            // relocation is applied, so using the loaded image base directly
            // avoids unsigned slide underflow.
            uint64_t EncodedOffset = Target | (High8 << 56);
            if (!checkedAdd(LoadedImageBase, EncodedOffset, FixedValue)) {
              Error = "chained-fixup 64-offset rebase overflows";
              return false;
            }
          } else {
            // target is a preferred vmAddr. Slide its low address first, then
            // restore TBI high8 as specified for DYLD_CHAINED_PTR_64.
            uint64_t SlidTarget = 0;
            if (!applySlide(Target, PreferredImageBase, LoadedImageBase,
                            SlidTarget)) {
              Error = "chained-fixup 64 rebase cannot apply image slide";
              return false;
            }
            FixedValue = (SlidTarget & 0x00FFFFFFFFFFFFFFULL) | (High8 << 56);
          }
        }

        std::memcpy(reinterpret_cast<void *>(Location), &FixedValue,
                    sizeof(FixedValue));

        if (Next == 0)
          break;
        if (Next > (PageSize - OffsetInPage) / 4U) {
          Error = "chained-fixup next pointer leaves its page";
          return false;
        }
        OffsetInPage += Next * 4U;
      }
      return true;
    };

    for (uint32_t PageIndex = 0; PageIndex != PageCount; ++PageIndex) {
      uint16_t PageStart = 0;
      if (!ReadStartSlot(PageIndex, PageStart)) {
        Error = "cannot read chained-fixup page start";
        return false;
      }
      if (PageStart == DYLD_CHAINED_PTR_START_NONE)
        continue;

      if (PageStart & DYLD_CHAINED_PTR_START_MULTI) {
        size_t OverflowIndex = PageStart & ~DYLD_CHAINED_PTR_START_MULTI;
        if (OverflowIndex < PageCount) {
          Error = "chained-fixup multi-start points into page-start entries";
          return false;
        }
        for (;;) {
          uint16_t OverflowStart = 0;
          if (!ReadStartSlot(OverflowIndex, OverflowStart)) {
            Error = "chained-fixup multi-start index is outside table";
            return false;
          }
          const bool Last = (OverflowStart & DYLD_CHAINED_PTR_START_LAST) != 0;
          const uint16_t ActualStart =
              OverflowStart & ~DYLD_CHAINED_PTR_START_LAST;
          if (!ApplyChain(PageIndex, ActualStart))
            return false;
          ++OverflowIndex;
          if (Last)
            break;
        }
      } else if (!ApplyChain(PageIndex, PageStart)) {
        return false;
      }
    }
  }

  return true;
}
