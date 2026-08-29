// ModernMachO.hpp: Parser for modern dyld linkedit data that predates the
// vendored LIEF version.

#ifndef IPASIM_MODERN_MACHO_HPP
#define IPASIM_MODERN_MACHO_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace LIEF {
namespace MachO {
class Binary;
}
}

namespace ipasim {

struct ModernExport {
  enum class Kind {
    Regular,
    Absolute,
    ThreadLocal,
    Reexport,
    Resolver,
  };

  Kind Type = Kind::Regular;
  uint64_t Value = 0;
  uint64_t Other = 0;
  int LibOrdinal = 0;
  std::string ImportName;
};

struct ModernLinkEditCommands {
  bool HasChainedFixups = false;
  uint32_t ChainedFixupsOffset = 0;
  uint32_t ChainedFixupsSize = 0;

  bool HasExportsTrie = false;
  uint32_t ExportsTrieOffset = 0;
  uint32_t ExportsTrieSize = 0;
};

struct ChainedImport {
  int LibOrdinal = 0;
  bool WeakImport = false;
  std::string Name;
  // Stored as dyld ultimately applies it to an unsigned pointer value. The
  // 32-bit ADDEND form is sign-extended into this field; ADDEND64 is natively
  // uint64_t in Apple's ABI.
  uint64_t Addend = 0;
};

class ModernMachO {
public:
  using BindResolver =
      std::function<uint64_t(int LibOrdinal, const std::string &Name,
                             bool WeakImport)>;

  // Inspect an already-mapped 64-bit Mach-O header for modern dyld commands.
  static bool findLinkEditCommands(const void *MachHeader,
                                   ModernLinkEditCommands &Commands,
                                   std::string &Error);

  // Convert a file range inside a Mach-O segment to the corresponding mapped
  // runtime address using the image slide.
  static const uint8_t *mappedFileRange(LIEF::MachO::Binary &Binary,
                                        uint64_t Slide, uint32_t FileOffset,
                                        uint32_t Size, std::string &Error);

  // Decode LC_DYLD_EXPORTS_TRIE payload into a symbol table. No method is
  // substituted when a terminal kind is unsupported; the kind is retained so
  // lookup can report it explicitly.
  static bool parseExportsTrie(const uint8_t *Data, size_t Size,
                               std::map<std::string, ModernExport> &Exports,
                               std::string &Error);

  // Decode and apply LC_DYLD_CHAINED_FIXUPS for generic 64-bit pointers. This
  // implements DYLD_CHAINED_PTR_64 and DYLD_CHAINED_PTR_64_OFFSET. arm64e/PAC
  // formats are deliberately rejected until their real semantics are added.
  static bool applyChainedFixups(const uint8_t *Data, size_t Size,
                                 uint64_t PreferredImageBase,
                                 uint64_t LoadedImageBase,
                                 const BindResolver &Resolve,
                                 std::string &Error);

private:
  static bool readULEB128(const uint8_t *&Cursor, const uint8_t *End,
                          uint64_t &Value);
};

} // namespace ipasim

#endif // IPASIM_MODERN_MACHO_HPP
