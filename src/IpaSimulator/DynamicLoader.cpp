// DynamicLoader.cpp: Implementation of class `DynamicLoader`

#include "ipasim/DynamicLoader.hpp"

#include "ipasim/Common.hpp"
#include "ipasim/IpaSimulator.hpp"
#include "ipasim/IpaSimulator/Config.hpp"
#include "ipasim/ModernMachO.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <psapi.h> // For `GetModuleInformation`
#if !defined(IPASIM_MODERN_CORE)
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#endif

using namespace ipasim;
using namespace std;
#if !defined(IPASIM_MODERN_CORE)
using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::Storage;
#endif

namespace {

constexpr int BIND_SPECIAL_DYLIB_SELF = 0;
constexpr int BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE = -1;
constexpr int BIND_SPECIAL_DYLIB_FLAT_LOOKUP = -2;
constexpr int BIND_SPECIAL_DYLIB_WEAK_LOOKUP = -3;

#if defined(IPASIM_MODERN_CORE)
constexpr const char *DarwinHostBridgeName = "IpaSimDarwinHost.dll";
// Mach-O LC_LOAD_WEAK_DYLIB and LC_LAZY_LOAD_DYLIB are intentionally not
// required during eager dependency loading. Use their ABI values directly so
// this remains compatible with ipaSim's pinned 2018 LIEF enum surface.
constexpr uint32_t LoadWeakDylibCommand = 0x80000018u;
constexpr uint32_t LazyLoadDylibCommand = 0x00000020u;

bool isDarwinHostInstallName(const string &Path) {
  // These are macOS-host dependencies of the iOS Simulator libsystem_sim_*
  // layer. They are not images from the iOS RuntimeRoot. Windows supplies the
  // target-proven host semantics through one explicit native bridge instead of
  // aliasing the simulator dylibs or fabricating Mach-O files.
  return Path == "/usr/lib/system/libsystem_kernel.dylib" ||
         Path == "/usr/lib/system/libsystem_platform.dylib" ||
         Path == "/usr/lib/system/libsystem_pthread.dylib";
}

bool isOptionalDependency(const LIEF::MachO::DylibCommand &Lib) {
  const uint32_t Command = static_cast<uint32_t>(Lib.command());
  return Command == LoadWeakDylibCommand || Command == LazyLoadDylibCommand;
}

filesystem::path executableDirectory() {
  wchar_t Buffer[32768];
  constexpr DWORD BufferCount =
      static_cast<DWORD>(sizeof(Buffer) / sizeof(Buffer[0]));
  const DWORD Length = GetModuleFileNameW(nullptr, Buffer, BufferCount);
  if (Length == 0 || Length >= BufferCount)
    return {};
  return filesystem::path(wstring(Buffer, Length)).parent_path();
}
#endif

} // namespace

bool BinaryPath::isFileValid() const {
  // The modern core runs unpackaged and resolves files through the host
  // filesystem. The legacy application retains its AppX package lookup below.
  error_code EC;
  if (filesystem::exists(filesystem::path(Path), EC) && !EC)
    return true;

#if defined(IPASIM_MODERN_CORE)
  return false;
#else
  if (Relative) {
    try {
      return Package::Current()
                 .InstalledLocation()
                 .TryGetItemAsync(to_hstring(Path))
                 .get() != nullptr;
    } catch (...) {
      return false;
    }
  }

  try {
    StorageFile File =
        StorageFile::GetFileFromPathAsync(to_hstring(Path)).get();
    return true;
  } catch (...) {
    return false;
  }
#endif
}

DynamicLoader::DynamicLoader(Emulator &Emu) : Emu(Emu) {
  // Map "kernel" page.
  void *KernelPtr =
      _aligned_malloc(DynamicLoader::PageSize, DynamicLoader::PageSize);
  KernelAddr = reinterpret_cast<uint64_t>(KernelPtr);
  Emu.mapMemory(KernelAddr, DynamicLoader::PageSize, UC_PROT_NONE);
}

bool DynamicLoader::setRuntimeRoot(const string &Path) {
#if defined(IPASIM_MODERN_CORE)
  RuntimeRoot.clear();
  // A different RuntimeRoot can legitimately make a previously missing image
  // available. Reset only failure memoization; successfully loaded images keep
  // their normal loader lifetime.
  FailedLoads.clear();
  ReportedFailedLoadRetries.clear();
  ReportedRequiredDependencyPropagation = false;
  if (Path.empty())
    return true;

  error_code EC;
  filesystem::path Root = filesystem::absolute(filesystem::path(Path), EC);
  if (EC || !filesystem::is_directory(Root, EC) || EC)
    return false;

  RuntimeRoot = Root.lexically_normal().make_preferred().string();
  return true;
#else
  (void)Path;
  return false;
#endif
}

LoadedLibrary *DynamicLoader::load(const string &Path) {
#if defined(IPASIM_MODERN_CORE)
  const bool IsSystemInstallName = !Path.empty() && Path[0] == '/';
  const bool IsDarwinHostInstallName = isDarwinHostInstallName(Path);
  if (IsSystemInstallName && !IsDarwinHostInstallName && RuntimeRoot.empty()) {
    Log.error() << "iOS runtime root is not configured for dependency " << Path
                << Log.end();
    return nullptr;
  }
#endif

  BinaryPath BP(resolvePath(Path));

  auto I = LLs.find(BP.Path);
  if (I != LLs.end())
    return I->second.get();

#if defined(IPASIM_MODERN_CORE)
  // A failed image cannot become valid later in the same loader pass: its file
  // and native bridge exports are immutable during that run. Remember the
  // failure so every dependent image does not parse/map it again and reproduce
  // the same root diagnostic dozens of times.
  if (FailedLoads.count(BP.Path) != 0) {
    if (ReportedFailedLoadRetries.insert(BP.Path).second)
      Log.info() << "skipping repeated load of previously failed library "
                 << BP.Path << "; original failure is reported above"
                 << Log.end();
    return nullptr;
  }

  auto RememberFailure = [this, &BP]() -> LoadedLibrary * {
    FailedLoads.insert(BP.Path);
    return nullptr;
  };
#endif

  // Check that file exists.
  if (!BP.isFileValid()) {
#if defined(IPASIM_MODERN_CORE)
    if (IsDarwinHostInstallName) {
      Log.error() << "Darwin host bridge missing for " << Path << ": "
                  << BP.Path << Log.end();
      return RememberFailure();
    }
    if (IsSystemInstallName) {
      Log.error() << "iOS runtime image missing for " << Path << ": " << BP.Path
                  << Log.end();
      return RememberFailure();
    }
#endif
    Log.error() << "invalid file: " << BP.Path << Log.end();
#if defined(IPASIM_MODERN_CORE)
    return RememberFailure();
#else
    return nullptr;
#endif
  }

  Log.info() << "loading library " << BP.Path << "...\n";

  LoadedLibrary *L;
  if (LIEF::MachO::is_macho(BP.Path))
    L = loadMachO(BP.Path);
  else if (LIEF::PE::is_pe(BP.Path))
    L = loadPE(BP.Path);
  else {
    Log.error() << "invalid binary type: " << BP.Path << Log.end();
#if defined(IPASIM_MODERN_CORE)
    return RememberFailure();
#else
    return nullptr;
#endif
  }

#if defined(IPASIM_MODERN_CORE)
  if (!L)
    return RememberFailure();
#endif

  // Recognize wrapper libraries.
  if (L)
    L->IsWrapper = BP.Relative && startsWith(BP.Path, "gen\\");

  return L;
}

LoadedLibrary *DynamicLoader::loadOrdinal(LoadedDylib &Image, int Ordinal) {
  using namespace LIEF::MachO;

  if (Ordinal == BIND_SPECIAL_DYLIB_SELF)
    return &Image;
  if (Ordinal == BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE)
    return MainExecutable;
  if (Ordinal <= BIND_SPECIAL_DYLIB_FLAT_LOOKUP)
    return nullptr; // Resolved by resolveSymbol(), which needs a symbol name.

  int CurrentOrdinal = 1;
  for (DylibCommand &Lib : Image.Bin.libraries()) {
    if (CurrentOrdinal == Ordinal)
      return load(Lib.name());
    ++CurrentOrdinal;
  }
  return nullptr;
}

uint64_t DynamicLoader::resolveSymbol(LoadedDylib &Image, int Ordinal,
                                      const string &Name, bool WeakImport) {
  auto FindIn = [&](LoadedLibrary *Lib) -> uint64_t {
    if (!Lib)
      return 0;
    if (!Lib->hasUnderscorePrefix() && !Name.empty() && Name[0] == '_')
      return Lib->findSymbol(*this, Name.substr(1));
    return Lib->findSymbol(*this, Name);
  };

  if (Ordinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP ||
      Ordinal == BIND_SPECIAL_DYLIB_WEAK_LOOKUP) {
    for (LoadedLibrary *Lib : LoadOrder) {
      // A flat import is looking outward. Skipping the requesting image also
      // avoids recursive self-reexports through the flat namespace.
      if (!Lib || Lib == &Image)
        continue;
      if (uint64_t Address = FindIn(Lib))
        return Address;
    }
    if (!WeakImport && Ordinal != BIND_SPECIAL_DYLIB_WEAK_LOOKUP)
      Log.error() << "flat-namespace symbol " << Name << " was not found"
                  << Log.end();
    return 0;
  }

  LoadedLibrary *Lib = loadOrdinal(Image, Ordinal);
  if (!Lib) {
    if (!WeakImport)
      Log.error() << "cannot resolve library ordinal " << Ordinal
                  << " for symbol " << Name << Log.end();
    return 0;
  }

  uint64_t Address = FindIn(Lib);
  if (!Address && !WeakImport)
    Log.error() << "symbol " << Name << " was not found for library ordinal "
                << Ordinal << Log.end();
  return Address;
}

void DynamicLoader::registerMachO(const void *Hdr) {
  auto HdrPtr = reinterpret_cast<uintptr_t>(Hdr);

  // Do nothing if already registered.
  if (!HdrSet.insert(HdrPtr).second)
    return;
  Hdrs.push_back(Hdr);

  // Fix some bindings generated by ipaSim's legacy wrapper toolchain.
  size_t Count;
  if (auto *FB = MachO(Hdr).getSectionData<uintptr_t **>(MachO::DataSegment,
                                                         "__fixbind", &Count))
    for (auto *EndFB = FB + Count; FB != EndFB; ++FB)
      if (*FB)
        **FB = reinterpret_cast<uintptr_t *>(***FB);

  // Call registered handlers.
  handleMachOs(Hdrs.size() - 1, 0);
}

void DynamicLoader::handleMachOs(size_t HdrOffset, size_t HandlerOffset) {
  // Handle Dylibs in reverse order, so that dependencies are resolved first,
  // before libraries that depend on them.
  vector<const char *> Paths;
  Paths.reserve(Hdrs.size() - HdrOffset);
  vector<const void *> Headers;
  Headers.reserve(Hdrs.size() - HdrOffset);
  for (ptrdiff_t I = Hdrs.size() - 1, End = HdrOffset - 1; I != End; --I) {
    // TODO: Find out paths from `LLs`.
    Paths.push_back(nullptr);
    Headers.push_back(Hdrs[I]);
  }

  for (auto I = Handlers.begin() + HandlerOffset, End = Handlers.end();
       I != End; ++I) {
    MachOHandler &Handler = *I;
    Handler.Mapped(Headers.size(), Paths.data(), Headers.data());
    for (ptrdiff_t I = Hdrs.size() - 1, End = HdrOffset - 1; I != End; --I)
      // TODO: Find out path from `LLs`.
      Handler.Init(nullptr, Hdrs[I]);
  }
}

void DynamicLoader::registerHandler(_dyld_objc_notify_mapped Mapped,
                                    _dyld_objc_notify_init Init,
                                    _dyld_objc_notify_unmapped Unmapped) {
  Handlers.push_back(MachOHandler{Mapped, Init, Unmapped});
  handleMachOs(0, Handlers.size() - 1);
}

// Inspired by `ImageLoaderMachO::segmentsCanSlide`.
bool DynamicLoader::canSegmentsSlide(LIEF::MachO::Binary &Bin) {
  using namespace LIEF::MachO;

  auto FType = Bin.header().file_type();
  return FType == FILE_TYPES::MH_DYLIB || FType == FILE_TYPES::MH_BUNDLE ||
         (FType == FILE_TYPES::MH_EXECUTE && Bin.is_pie());
}

BinaryPath DynamicLoader::resolvePath(const string &Path) {
  if (!Path.empty() && Path[0] == '/') {
    // Mach-O system install names normally root at the iOS runtime. Simulator
    // libsystem_sim_* also imports a small macOS-host libSystem surface; those
    // explicit install names instead resolve to our native Windows host bridge.
#if defined(IPASIM_MODERN_CORE)
    if (isDarwinHostInstallName(Path)) {
      filesystem::path Base = executableDirectory();
      if (Base.empty())
        return BinaryPath{"", /* Relative */ false};
      filesystem::path Bridge = Base / DarwinHostBridgeName;
      return BinaryPath{Bridge.lexically_normal().make_preferred().string(),
                        /* Relative */ false};
    }

    filesystem::path Relative(Path.substr(1));
    filesystem::path Resolved =
        (filesystem::path(RuntimeRoot) / Relative).lexically_normal();
    return BinaryPath{Resolved.make_preferred().string(), /* Relative */ false};
#else
    return BinaryPath{filesystem::path("gen" + Path).make_preferred().string(),
                      /* Relative */ true};
#endif
  }

  // TODO: Handle also `.ipa`-relative paths.
  return BinaryPath{Path, filesystem::path(Path).is_relative()};
}

LoadedLibrary *DynamicLoader::loadMachO(const string &Path) {
  using namespace LIEF::MachO;
  if (sizeof(void *) != 8) {
    Log.error("ARM64 Mach-O loading requires a 64-bit Windows host");
    return nullptr;
  }

  unique_ptr<FatBinary> Parsed;
  try {
    Parsed = Parser::parse(Path);
  } catch (const exception &E) {
    Log.error() << "cannot parse Mach-O " << Path << ": " << E.what()
                << Log.end();
    return nullptr;
  }
  if (!Parsed) {
    Log.error() << "cannot parse Mach-O " << Path << Log.end();
    return nullptr;
  }

  unique_ptr<LoadedDylib> LL;
  try {
    LL = make_unique<LoadedDylib>(move(Parsed));
  } catch (const exception &E) {
    Log.error() << "cannot select ARM64 Mach-O slice from " << Path << ": "
                << E.what() << Log.end();
    return nullptr;
  }

  LoadedDylib *LLP = LL.get();
  Binary &Bin = LL->Bin;
  LLs[Path] = move(LL);
  LoadOrder.push_back(LLP);

  auto ForgetCurrent = [&]() -> LoadedLibrary * {
    LoadOrder.erase(remove(LoadOrder.begin(), LoadOrder.end(), LLP),
                    LoadOrder.end());
    if (MainExecutable == LLP)
      MainExecutable = nullptr;
    LLs.erase(Path);
    return nullptr;
  };

  Header &Hdr = Bin.header();
  if (Hdr.cpu_type() != CPU_TYPES::CPU_TYPE_ARM64) {
    Log.error("expected ARM64 binary");
    return ForgetCurrent();
  }
  if (Hdr.has(HEADER_FLAGS::MH_SPLIT_SEGS)) {
    Log.error("MH_SPLIT_SEGS is not supported by the ARM64 loader");
    return ForgetCurrent();
  }
  if (!canSegmentsSlide(Bin)) {
    Log.error("the ARM64 Mach-O image is not slideable");
    return ForgetCurrent();
  }
  if (Hdr.file_type() == FILE_TYPES::MH_EXECUTE && !MainExecutable)
    MainExecutable = LLP;

  auto IsPageZero = [](SegmentCommand &Seg) {
    return Seg.name() == "__PAGEZERO";
  };

  // Do not reserve __PAGEZERO in host memory. Modern 64-bit executables often
  // make it several GiB specifically as a null-pointer guard region.
  uint64_t LowAddr = numeric_limits<uint64_t>::max();
  uint64_t HighAddr = 0;
  vector<pair<uint64_t, uint64_t>> Ranges;
  for (SegmentCommand &Seg : Bin.segments()) {
    if (IsPageZero(Seg) || Seg.virtual_size() == 0)
      continue;
    if (Seg.virtual_size() > numeric_limits<uint64_t>::max() -
                                 Seg.virtual_address()) {
      Log.error("Mach-O segment address overflows");
      return ForgetCurrent();
    }
    uint64_t SegLow = Seg.virtual_address();
    uint64_t SegHigh = roundToPageSize(SegLow + Seg.virtual_size());
    for (const auto &Range : Ranges) {
      if (SegLow < Range.second && SegHigh > Range.first) {
        Log.error("overlapping Mach-O segments after page rounding");
        return ForgetCurrent();
      }
    }
    Ranges.emplace_back(SegLow, SegHigh);
    LowAddr = min(LowAddr, SegLow);
    HighAddr = max(HighAddr, SegHigh);
  }

  if (LowAddr == numeric_limits<uint64_t>::max() || HighAddr <= LowAddr) {
    Log.error("Mach-O image contains no mappable segments");
    return ForgetCurrent();
  }

  uint64_t Size = HighAddr - LowAddr;
  uintptr_t Addr = reinterpret_cast<uintptr_t>(_aligned_malloc(Size, PageSize));
  if (!Addr) {
    Log.error("couldn't allocate memory for Mach-O segments");
    return ForgetCurrent();
  }
  uint64_t Slide = Addr - LowAddr;
  LLP->StartAddress = Slide;
  LLP->MappedStartAddress = Addr;
  LLP->Size = Size;

  // Copy and map segments at one coherent slide.
  for (SegmentCommand &Seg : Bin.segments()) {
    if (IsPageZero(Seg) || Seg.virtual_size() == 0)
      continue;

    uint32_t VMProt = Seg.init_protection();
    uc_prot Perms = UC_PROT_NONE;
    if (VMProt & (uint32_t)VM_PROTECTIONS::VM_PROT_READ)
      Perms |= UC_PROT_READ;
    if (VMProt & (uint32_t)VM_PROTECTIONS::VM_PROT_WRITE)
      Perms |= UC_PROT_WRITE;
    if (VMProt & (uint32_t)VM_PROTECTIONS::VM_PROT_EXECUTE)
      Perms |= UC_PROT_EXEC;

    uint64_t VAddr = Seg.virtual_address() + Slide;
    uint64_t VSize = Seg.virtual_size();
    uint64_t MapSize = roundToPageSize(VSize);
    if ((VAddr & (PageSize - 1)) != 0) {
      Log.error("Mach-O segment is not page aligned");
      return ForgetCurrent();
    }

    auto &Buff = Seg.content();
    if (Buff.size() > VSize) {
      Log.error("Mach-O segment file content exceeds virtual size");
      return ForgetCurrent();
    }

    uint8_t *Mem = reinterpret_cast<uint8_t *>(VAddr);
    memset(Mem, 0, MapSize);
    if (!Buff.empty())
      memcpy(Mem, Buff.data(), Buff.size());
    Emu.mapMemory(VAddr, MapSize, Perms);

    // Classic dyld-info rebases remain supported independently of chained
    // fixups. Do not reinterpret one format as the other.
    if (Slide > 0) {
      for (Relocation &Rel : Seg.relocations()) {
        if (Rel.is_pc_relative() ||
            Rel.origin() != RELOCATION_ORIGINS::ORIGIN_DYLDINFO ||
            Rel.size() != 64 || (Rel.address() & R_SCATTERED) != 0) {
          Log.error("unsupported ARM64 relocation");
          continue;
        }

        uint64_t RelBase = LowAddr + Slide;
        uint64_t RelAddr = RelBase + Rel.address();
        if (RelAddr < VAddr || RelAddr + sizeof(uint64_t) > VAddr + MapSize) {
          Log.error("relocation target out of range");
          continue;
        }

        uint64_t *Val = reinterpret_cast<uint64_t *>(RelAddr);
        if (*Val != 0)
          *Val += Slide;
      }
    }
  }

  const uint64_t LoadedImageBase = LLP->loadedImageBase();
  ModernLinkEditCommands ModernCommands;
  string ModernError;
  if (!ModernMachO::findLinkEditCommands(
          reinterpret_cast<const void *>(LoadedImageBase), ModernCommands,
          ModernError)) {
    Log.error() << "cannot inspect modern dyld commands in " << Path << ": "
                << ModernError << Log.end();
    return ForgetCurrent();
  }

  // Parse modern exports before resolving imports so self-binds and dependency
  // re-exports can use the same symbol path as classic Mach-O images.
  if (ModernCommands.HasExportsTrie) {
    const uint8_t *ExportsData = ModernMachO::mappedFileRange(
        Bin, Slide, ModernCommands.ExportsTrieOffset,
        ModernCommands.ExportsTrieSize, ModernError);
    if (!ExportsData) {
      Log.error() << "cannot map exports trie for " << Path << ": "
                  << ModernError << Log.end();
      return ForgetCurrent();
    }
    map<string, ModernExport> Exports;
    if (!ModernMachO::parseExportsTrie(ExportsData,
                                       ModernCommands.ExportsTrieSize, Exports,
                                       ModernError)) {
      Log.error() << "cannot parse exports trie for " << Path << ": "
                  << ModernError << Log.end();
      return ForgetCurrent();
    }
    LLP->modernExports(move(Exports));
  }

  // Make dependencies available before chained binds are applied. A required
  // dependency failure is already the root cause; do not continue into this
  // image's fixups and manufacture secondary unresolved-symbol diagnostics.
  // Weak and lazy dylib commands preserve their normal non-fatal eager behavior.
#if defined(IPASIM_MODERN_CORE)
  for (DylibCommand &Lib : Bin.libraries()) {
    if (load(Lib.name()) || isOptionalDependency(Lib))
      continue;
    if (!ReportedRequiredDependencyPropagation) {
      Log.error() << "required dependency " << Lib.name()
                  << " failed while loading " << Path
                  << "; original dependency failure is reported above; further "
                     "upstream propagation is suppressed"
                  << Log.end();
      ReportedRequiredDependencyPropagation = true;
    }
    return ForgetCurrent();
  }
#else
  for (DylibCommand &Lib : Bin.libraries())
    load(Lib.name());
#endif

  if (ModernCommands.HasChainedFixups) {
    const uint8_t *FixupsData = ModernMachO::mappedFileRange(
        Bin, Slide, ModernCommands.ChainedFixupsOffset,
        ModernCommands.ChainedFixupsSize, ModernError);
    if (!FixupsData) {
      Log.error() << "cannot map chained fixups for " << Path << ": "
                  << ModernError << Log.end();
      return ForgetCurrent();
    }

    auto Resolver = [this, LLP](int Ordinal, const string &Name,
                                bool WeakImport) -> uint64_t {
      return resolveSymbol(*LLP, Ordinal, Name, WeakImport);
    };
    if (!ModernMachO::applyChainedFixups(
            FixupsData, ModernCommands.ChainedFixupsSize, Bin.imagebase(),
            LoadedImageBase, Resolver, ModernError)) {
      Log.error() << "cannot apply chained fixups for " << Path << ": "
                  << ModernError << Log.end();
      return ForgetCurrent();
    }
  }

  // Older binaries may still use dyld opcode bindings. Crucially, do not call
  // dyld_info() when the command is absent: chained-fixup-only binaries are a
  // normal modern case.
  if (Bin.has_dyld_info()) {
    for (BindingInfo &BInfo : Bin.dyld_info().bindings()) {
      if ((BInfo.binding_class() != BINDING_CLASS::BIND_CLASS_STANDARD &&
           BInfo.binding_class() != BINDING_CLASS::BIND_CLASS_LAZY) ||
          BInfo.binding_type() != BIND_TYPES::BIND_TYPE_POINTER) {
        Log.error("unsupported classic binding info");
        continue;
      }
      if (!BInfo.has_library()) {
        Log.error("flat-namespace classic bindings are not supported yet");
        continue;
      }

      string LibName(BInfo.library().name());
      LoadedLibrary *Lib = load(LibName);
      if (!Lib) {
        Log.error("classic binding library couldn't be loaded");
        continue;
      }

      string SymName(BInfo.symbol().name());
      uint64_t SymAddr;
      if (!Lib->hasUnderscorePrefix() && !SymName.empty() && SymName[0] == '_')
        SymAddr = Lib->findSymbol(*this, SymName.substr(1));
      else
        SymAddr = Lib->findSymbol(*this, SymName);
      if (!SymAddr) {
        Log.error() << "external symbol " << SymName << " from library "
                    << LibName << " couldn't be resolved" << Log.end();
        continue;
      }

      uint64_t TargetAddr = BInfo.address() + Slide;
      LLP->checkInRange(TargetAddr);
      *reinterpret_cast<uint64_t *>(TargetAddr) =
          SymAddr + static_cast<uint64_t>(BInfo.addend());
    }
  }

  return LLP;
}

LoadedLibrary *DynamicLoader::loadPE(const string &Path) {
  using namespace LIEF::PE;

  auto LL = make_unique<LoadedDll>();
  LoadedDll *LLP = LL.get();
  LLs[Path] = move(LL);
  LoadOrder.push_back(LLP);

  auto ForgetCurrent = [&]() -> LoadedLibrary * {
    LoadOrder.erase(remove(LoadOrder.begin(), LoadOrder.end(), LLP),
                    LoadOrder.end());
    LLs.erase(Path);
    return nullptr;
  };

  // The modern core uses normal Win32 loading. The legacy app may additionally
  // resolve a packaged DLL through LoadPackagedLibrary.
  const wstring WidePath = filesystem::path(Path).wstring();
  HMODULE Lib = LoadLibraryExW(WidePath.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
#if !defined(IPASIM_MODERN_CORE)
  if (!Lib)
    Lib = LoadPackagedLibrary(WidePath.c_str(), 0);
#endif
  if (!Lib) {
    Log.error() << "couldn't load DLL: " << Path << Log.appendWinError();
    return ForgetCurrent();
  }
  LLP->Ptr = Lib;

  MODULEINFO Info;
  if (!GetModuleInformation(GetCurrentProcess(), Lib, &Info, sizeof(Info))) {
    Log.winError("couldn't load module information");
    FreeLibrary(Lib);
    return ForgetCurrent();
  }
  if (uint64_t Hdr = LLP->findSymbol(*this, "_mh_dylib_header")) {
    LLP->StartAddress = Hdr;
    LLP->Size =
        Info.SizeOfImage - (Hdr - reinterpret_cast<uint64_t>(Info.lpBaseOfDll));
    LLP->MachOPoser = true;
  } else {
    LLP->StartAddress = reinterpret_cast<uint64_t>(Info.lpBaseOfDll);
    LLP->Size = Info.SizeOfImage;
    LLP->MachOPoser = false;
  }
  LLP->MappedStartAddress = LLP->StartAddress;

  // Keep native DLL code non-executable inside Unicorn. A fetch from one of
  // these pages is the deliberate cross-platform call boundary handled by
  // SysTranslator::handleFetchProtMem().
  uint64_t StartAddr = alignToPageSize(LLP->StartAddress);
  uint64_t MapSize = roundToPageSize(LLP->Size);
  Emu.mapMemory(StartAddr, MapSize, UC_PROT_READ | UC_PROT_WRITE);

  return LLP;
}

LibraryInfo DynamicLoader::lookup(uint64_t Addr) {
  for (auto &Pair : LLs) {
    LoadedLibrary *LL = Pair.second.get();
    if (LL->isInRange(Addr))
      return {&Pair.first, LL};
  }
  return {nullptr, nullptr};
}

LogStream::Handler DynamicLoader::dumpAddr(uint64_t Addr) {
  return [this, Addr](LogStream &S) {
    if (Addr == KernelAddr)
      S << "kernel!0x" << to_hex_string(Addr);
    else {
      LibraryInfo LI(lookup(Addr));
      S << dumpAddr(Addr, LI);
    }
  };
}

static LogStream::Handler dumpAddrImpl(uint64_t Addr, const LibraryInfo &LI) {
  return [Addr, &LI](LogStream &S) {
    uint64_t RVA = Addr - LI.Lib->StartAddress;
    S << *LI.LibPath << "+0x" << to_hex_string(RVA);
  };
}

LogStream::Handler DynamicLoader::dumpAddr(uint64_t Addr,
                                           const LibraryInfo &LI) {
  return [this, Addr, &LI](LogStream &S) {
    if (!LI.Lib) {
      S << "0x" << to_hex_string(Addr);
      return;
    }
    if (LI.Lib->hasMachO())
      if (ObjCMethod M = LI.Lib->getMachO().findMethod(Addr)) {
        S << dumpAddr(Addr, LI, M);
        return;
      }
    S << dumpAddrImpl(Addr, LI);
  };
}

LogStream::Handler DynamicLoader::dumpAddr(uint64_t Addr, const LibraryInfo &LI,
                                           ObjCMethod M) {
  return
      [Addr, &LI, M](LogStream &S) { S << M << "!" << dumpAddrImpl(Addr, LI); };
}
