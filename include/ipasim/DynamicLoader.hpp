// DynamicLoader.hpp: Definition of class `DynamicLoader` and some smaller
// classes it uses.

#ifndef IPASIM_DYNAMIC_LOADER_HPP
#define IPASIM_DYNAMIC_LOADER_HPP

#include "ipasim/Common.hpp"
#include "ipasim/Emulator.hpp"
#include "ipasim/LoadedLibrary.hpp"
#include "ipasim/Logger.hpp"
#include "ipasim/RuntimeLog.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stack>
#include <string>
#include <unicorn/unicorn.h>
#include <vector>

namespace ipasim {

// Represents a path to a binary file. It can be both `.dll` and `.dylib`. It
// can also be both user and "our system" binary.
struct BinaryPath {
  std::string Path;
  bool Relative; // `true` iff `Path` is relative to install dir

  // Checks whether the binary exists.
  bool isFileValid() const;
};

// Pair of a `LoadedLibrary` and its path.
struct LibraryInfo {
  const std::string *LibPath;
  LoadedLibrary *Lib;
};

// Used for dyld-objc integration.
using _dyld_objc_notify_mapped = void (*)(unsigned count,
                                          const char *const paths[],
                                          const void *const mh[]);
using _dyld_objc_notify_init = void (*)(const char *path, const void *mh);
using _dyld_objc_notify_unmapped = void (*)(const char *path, const void *mh);

// Represents our dynamic loader. It tries to resemble the behavior of iOS's
// `dyld`. The dynamic loader retains information about loaded libraries.
class DynamicLoader {
public:
  DynamicLoader(Emulator &Emu);
  LoadedLibrary *load(const std::string &Path);

  // Configure the root that mirrors iOS absolute install names, for example
  // <root>/System/Library/Frameworks/Foundation.framework/Foundation and
  // <root>/usr/lib/libSystem.B.dylib. The modern core never falls back to the
  // legacy generated-wrapper `gen` tree for these system images.
  bool setRuntimeRoot(const std::string &Path);

  // Resolve Mach-O library ordinals used by export re-exports and chained
  // imports. Positive ordinals index the image's dependent dylibs; 0 is self;
  // -1 is the main executable; -2 and -3 use flat/weak lookup semantics.
  LoadedLibrary *loadOrdinal(LoadedDylib &Image, int Ordinal);
  uint64_t resolveSymbol(LoadedDylib &Image, int Ordinal,
                         const std::string &Name, bool WeakImport = false);

  // ipaSim maps guest images directly onto Windows host backing pages with
  // uc_mem_map_ptr(). Additional Unicorn CPU contexts can therefore share the
  // same guest process memory. New execution contexts replay all loader-known
  // mappings; contexts that encounter a mapping added later can install it
  // lazily without remapping a concurrently executing engine.
  void registerEmulator(Emulator &ExecutionEmulator);
  bool mapKnownSharedMemory(Emulator &ExecutionEmulator, uint64_t Address);
  bool mapExternalSharedMemory(Emulator &ExecutionEmulator, uint64_t Address,
                               uint64_t Size);

  // Used for dyld-objc integration. Notifies registered listeners that a new
  // library was loaded into memory. Objective-C runtime uses this to initialize
  // the library's classes.
  void registerMachO(const void *Hdr);
  // Registers listeners for dyld-objc integration. See also `registerMachO`.
  void registerHandler(_dyld_objc_notify_mapped Mapped,
                       _dyld_objc_notify_init Init,
                       _dyld_objc_notify_unmapped Unmapped);
  // Finds a library that `Addr` is mapped inside.
  LibraryInfo lookup(uint64_t Addr);
  // Logging helpers
  LogStream::Handler dumpAddr(uint64_t Addr);
  LogStream::Handler dumpAddr(uint64_t Addr, const LibraryInfo &LI);
  LogStream::Handler dumpAddr(uint64_t Addr, const LibraryInfo &LI,
                              ObjCMethod M);
  uint64_t getKernelAddr() { return KernelAddr; }
  static constexpr uint64_t alignToPageSize(uint64_t Addr) {
    return Addr & (-PageSize);
  }
  static constexpr uint64_t roundToPageSize(uint64_t Addr) {
    return alignToPageSize(Addr + PageSize - 1);
  }

  static constexpr int PageSize = 4096;

private:
  struct MachOHandler {
    _dyld_objc_notify_mapped Mapped;
    _dyld_objc_notify_init Init;
    _dyld_objc_notify_unmapped Unmapped;
  };

  struct SharedMemoryMapping {
    uint64_t Address;
    uint64_t Size;
    uc_prot Permissions;
  };

  bool canSegmentsSlide(LIEF::MachO::Binary &Bin);
  BinaryPath resolvePath(const std::string &Path);
  LoadedLibrary *loadMachO(const std::string &Path);
  LoadedLibrary *loadPE(const std::string &Path);
  void handleMachOs(size_t HdrOffset, size_t HandlerOffset);
  void mapAndRecordSharedMemory(Emulator &ExecutionEmulator, uint64_t Address,
                                uint64_t Size, uc_prot Permissions);

  static constexpr int R_SCATTERED = 0x80000000; // From `<mach-o/reloc.h>`
  Emulator &Emu;
  uint64_t KernelAddr;
  LoadedDylib *MainExecutable = nullptr;
  std::string RuntimeRoot;
  // Guest image/external pages are backed by the Windows process at identical
  // addresses. The registry is the process-wide mapping description replayed
  // into each independent Unicorn CPU context.
  std::mutex SharedMemoryMutex;
  std::vector<SharedMemoryMapping> SharedMemoryMappings;
  // Loaded libraries and their paths. LoadOrder preserves dyld-style flat
  // lookup order; LLs remains the path-indexed ownership map.
  std::map<std::string, std::unique_ptr<LoadedLibrary>> LLs;
  std::vector<LoadedLibrary *> LoadOrder;
  // Failed modern-core images are stable failures for the lifetime of a loader
  // pass. Remember them so one missing dependency is not reparsed/remapped and
  // re-reported dozens of times through downstream callers.
  std::set<std::string> FailedLoads;
  std::set<std::string> ReportedFailedLoadRetries;
  // A root image failure already carries the actionable symbol/path diagnostic.
  // Print one immediate parent propagation line, then suppress the recursive
  // unwind so the same dependency chain does not dominate the target trace.
  bool ReportedRequiredDependencyPropagation = false;
  // These are used for dyld-objc integration:
  std::vector<const void *> Hdrs; // Registered headers
  std::set<uintptr_t> HdrSet;     // Set of registered headers for faster lookup
  std::vector<MachOHandler> Handlers; // Registered handlers
};

} // namespace ipasim

// !defined(IPASIM_DYNAMIC_LOADER_HPP)
#endif
