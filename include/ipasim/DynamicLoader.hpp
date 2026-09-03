// DynamicLoader.hpp: Definition of class `DynamicLoader` and some smaller
// classes it uses.

#ifndef IPASIM_DYNAMIC_LOADER_HPP
#define IPASIM_DYNAMIC_LOADER_HPP

#include "ipasim/Common.hpp"
#include "ipasim/Emulator.hpp"
#include "ipasim/LoadedLibrary.hpp"
#include "ipasim/Logger.hpp"
#include "ipasim/RuntimeLog.hpp"
#include "ipasim/RuntimeRootStore.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stack>
#include <string>
#include <unicorn/unicorn.h>
#include <vector>

namespace ipasim {

struct BinaryPath {
  std::string Path;
  bool Relative;
  bool isFileValid() const;
};

struct LibraryInfo {
  const std::string *LibPath;
  LoadedLibrary *Lib;
};

using _dyld_objc_notify_mapped = void (*)(unsigned count,
                                          const char *const paths[],
                                          const void *const mh[]);
using _dyld_objc_notify_init = void (*)(const char *path, const void *mh);
using _dyld_objc_notify_unmapped = void (*)(const char *path, const void *mh);

class DynamicLoader {
public:
  DynamicLoader(Emulator &Emu);
  LoadedLibrary *load(const std::string &Path);
  bool setRuntimeRoot(const std::string &Path);

  LoadedLibrary *loadOrdinal(LoadedDylib &Image, int Ordinal);
  uint64_t resolveSymbol(LoadedDylib &Image, int Ordinal,
                         const std::string &Name, bool WeakImport = false);

  // Replay the process address space into a fresh Unicorn CPU context. Normal
  // Emulator::mapMemory calls register their Windows-backed pages here.
  bool registerEmulator(Emulator &ExecutionEmulator);
  // Lazily install an already-recorded process mapping into another CPU context.
  bool mapKnownSharedMemory(Emulator &ExecutionEmulator, uint64_t Address);
  // Validate and map host-backed process data first encountered by one context.
  bool mapExternalSharedMemory(Emulator &ExecutionEmulator, uint64_t Address,
                               uint64_t Size);

  void registerMachO(const void *Hdr);
  void registerHandler(_dyld_objc_notify_mapped Mapped,
                       _dyld_objc_notify_init Init,
                       _dyld_objc_notify_unmapped Unmapped);
  LibraryInfo lookup(uint64_t Addr);
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
  friend class Emulator;

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
  LoadedLibrary *loadMachO(const std::string &Path,
                           const std::vector<std::uint8_t> *Data = nullptr,
                           const std::string *Identity = nullptr);
  LoadedLibrary *loadPE(const std::string &Path);
  void handleMachOs(size_t HdrOffset, size_t HandlerOffset);
  void recordSharedMemory(uint64_t Address, uint64_t Size,
                          uc_prot Permissions);

  static constexpr int R_SCATTERED = 0x80000000;
  Emulator &Emu;
  uint64_t KernelAddr;
  LoadedDylib *MainExecutable = nullptr;
  std::unique_ptr<RuntimeRootStore> RuntimeStore;

  std::mutex SharedMemoryMutex;
  std::vector<SharedMemoryMapping> SharedMemoryMappings;

  std::map<std::string, std::unique_ptr<LoadedLibrary>> LLs;
  std::vector<LoadedLibrary *> LoadOrder;
  std::set<std::string> FailedLoads;
  std::set<std::string> ReportedFailedLoadRetries;
  bool ReportedRequiredDependencyPropagation = false;
  std::vector<const void *> Hdrs;
  std::set<uintptr_t> HdrSet;
  std::vector<MachOHandler> Handlers;
};

} // namespace ipasim

#endif
