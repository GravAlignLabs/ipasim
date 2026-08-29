#pragma once

#include "HostImportInventory.hpp"

#include <map>
#include <tuple>

namespace ipasim::probe {
namespace host_inventory_v2_detail {

using namespace host_inventory_detail;

inline bool isSimulatorHostProxyInstallName(const std::string &Name) {
  const std::string FileName = std::filesystem::path(Name).filename().string();
  return FileName == "libsystem_sim_kernel_host.dylib" ||
         FileName == "libsystem_sim_platform_host.dylib" ||
         FileName == "libsystem_sim_pthread_host.dylib";
}

inline bool isHostFacingDependency(const std::string &Name) {
  return isDarwinHostInstallName(Name) || isSimulatorHostProxyInstallName(Name);
}

struct HostImportKey {
  std::string Image;
  std::string Library;
  std::string Symbol;

  bool operator<(const HostImportKey &Other) const {
    return std::tie(Image, Library, Symbol) <
           std::tie(Other.Image, Other.Library, Other.Symbol);
  }
};

struct PositiveImportKey {
  std::string Image;
  int Ordinal = 0;
  std::string Library;
  std::string Symbol;
  bool Weak = false;

  bool operator<(const PositiveImportKey &Other) const {
    return std::tie(Image, Ordinal, Library, Symbol, Weak) <
           std::tie(Other.Image, Other.Ordinal, Other.Library, Other.Symbol,
                    Other.Weak);
  }
};

inline void collectImageHostImports(
    const std::filesystem::path &ImagePath,
    std::set<HostImportKey> &RequiredHostImports,
    std::set<HostImportKey> &WeakHostImports,
    std::set<PositiveImportKey> &AllPositiveImports) {
  std::vector<std::uint8_t> Data;
  if (!loadFile(ImagePath, Data)) {
    std::printf("[host-import-inventory] image unavailable, skipped: %s\n",
                ImagePath.string().c_str());
    return;
  }

  std::size_t SliceOffset = 0;
  std::size_t SliceSize = 0;
  std::string Error;
  if (!selectArm64Slice(Data, SliceOffset, SliceSize, Error)) {
    std::printf("[host-import-inventory] %s: %s; skipped\n",
                ImagePath.filename().string().c_str(), Error.c_str());
    return;
  }

  std::vector<std::string> Libraries;
  std::vector<Import> Imports;
  if (!parseImage(Data, SliceOffset, SliceSize, Libraries, Imports, Error)) {
    std::printf("[host-import-inventory] %s: %s; skipped\n",
                ImagePath.filename().string().c_str(), Error.c_str());
    return;
  }

  std::set<int> HostOrdinals;
  for (std::size_t I = 0; I != Libraries.size(); ++I) {
    if (!isHostFacingDependency(Libraries[I]))
      continue;
    const int Ordinal = static_cast<int>(I + 1);
    HostOrdinals.insert(Ordinal);
    std::printf("[host-import-inventory] %s host ordinal %d -> %s\n",
                ImagePath.filename().string().c_str(), Ordinal,
                Libraries[I].c_str());
  }

  std::size_t PositiveImports = 0;
  std::size_t MatchedHostImports = 0;
  for (const Import &ImportEntry : Imports) {
    if (ImportEntry.Ordinal > 0 &&
        static_cast<std::size_t>(ImportEntry.Ordinal) <= Libraries.size()) {
      ++PositiveImports;
      AllPositiveImports.insert(PositiveImportKey{
          ImagePath.filename().string(), ImportEntry.Ordinal,
          Libraries[ImportEntry.Ordinal - 1], ImportEntry.Name,
          ImportEntry.Weak});
    }

    if (ImportEntry.Ordinal <= 0 ||
        static_cast<std::size_t>(ImportEntry.Ordinal) > Libraries.size() ||
        HostOrdinals.count(ImportEntry.Ordinal) == 0)
      continue;

    ++MatchedHostImports;
    const std::string &Library = Libraries[ImportEntry.Ordinal - 1];
    HostImportKey Key{ImagePath.filename().string(), Library,
                      ImportEntry.Name};
    (ImportEntry.Weak ? WeakHostImports : RequiredHostImports)
        .insert(std::move(Key));
  }

  std::printf("[host-import-inventory] %s chained imports: %zu total, %zu positive-ordinal, %zu host-facing\n",
              ImagePath.filename().string().c_str(), Imports.size(),
              PositiveImports, MatchedHostImports);

  if (PositiveImports != 0 && HostOrdinals.empty()) {
    std::printf("[host-import-inventory] WARNING: %s has positive-ordinal imports but no recognized host dependency; dependency list follows\n",
                ImagePath.filename().string().c_str());
    for (std::size_t I = 0; I != Libraries.size(); ++I)
      std::printf("[host-import-inventory]   ordinal %zu -> %s\n", I + 1,
                  Libraries[I].c_str());
  }
}

inline void reportPositiveImportSurface(
    const std::set<PositiveImportKey> &AllPositiveImports) {
  std::printf("[host-import-inventory] full simulator positive-ordinal import surface: %zu unique bindings\n",
              AllPositiveImports.size());

  std::string LastImage;
  int LastOrdinal = -1;
  std::string LastLibrary;
  for (const PositiveImportKey &Entry : AllPositiveImports) {
    if (Entry.Image != LastImage || Entry.Ordinal != LastOrdinal ||
        Entry.Library != LastLibrary) {
      std::printf("[host-import-inventory] IMPORT GROUP %s ordinal %d -> %s\n",
                  Entry.Image.c_str(), Entry.Ordinal, Entry.Library.c_str());
      LastImage = Entry.Image;
      LastOrdinal = Entry.Ordinal;
      LastLibrary = Entry.Library;
    }
    std::printf("[host-import-inventory]   %s%s\n", Entry.Symbol.c_str(),
                Entry.Weak ? " [weak]" : "");
  }
}

} // namespace host_inventory_v2_detail

inline void reportDarwinHostImportInventoryV2(const char *RuntimeRoot) {
  using namespace host_inventory_detail;
  using namespace host_inventory_v2_detail;

  if (!RuntimeRoot || !*RuntimeRoot)
    return;

  const std::filesystem::path SystemDir =
      std::filesystem::path(RuntimeRoot) / L"usr" / L"lib" / L"system";

  std::set<HostImportKey> RequiredHostImports;
  std::set<HostImportKey> WeakHostImports;
  std::set<PositiveImportKey> AllPositiveImports;
  collectImageHostImports(SystemDir / L"libsystem_sim_kernel.dylib",
                          RequiredHostImports, WeakHostImports,
                          AllPositiveImports);
  collectImageHostImports(SystemDir / L"libsystem_sim_platform.dylib",
                          RequiredHostImports, WeakHostImports,
                          AllPositiveImports);
  collectImageHostImports(SystemDir / L"libsystem_sim_pthread.dylib",
                          RequiredHostImports, WeakHostImports,
                          AllPositiveImports);

  // Print the complete positive-ordinal surface before the loader runs. This is
  // diagnostic only: no import is patched or marked successful here. The goal
  // is to discover the next several concrete boundaries in one target run.
  reportPositiveImportSurface(AllPositiveImports);

  const std::filesystem::path BridgePath = bridgePath();
  HMODULE Bridge = BridgePath.empty() ? nullptr : LoadLibraryW(BridgePath.c_str());
  if (!Bridge) {
    std::fprintf(stderr,
                 "[host-import-inventory] could not load IpaSimDarwinHost.dll for export comparison; continuing with normal loader\n");
    return;
  }

  const auto HasExport = [&](const std::string &MachSymbol) {
    std::string ExportName = MachSymbol;
    if (!ExportName.empty() && ExportName.front() == '_')
      ExportName.erase(ExportName.begin());
    return GetProcAddress(Bridge, ExportName.c_str()) != nullptr;
  };

  std::size_t RequiredPresent = 0;
  std::vector<HostImportKey> MissingRequired;
  std::vector<HostImportKey> MissingWeak;

  for (const HostImportKey &Entry : RequiredHostImports) {
    if (HasExport(Entry.Symbol))
      ++RequiredPresent;
    else
      MissingRequired.push_back(Entry);
  }
  for (const HostImportKey &Entry : WeakHostImports) {
    if (!HasExport(Entry.Symbol))
      MissingWeak.push_back(Entry);
  }

  std::printf("[host-import-inventory] simulator host ABI: %zu required imports, %zu already bridged, %zu missing; %zu weak imports missing\n",
              RequiredHostImports.size(), RequiredPresent,
              MissingRequired.size(), MissingWeak.size());

  for (const HostImportKey &Entry : MissingRequired)
    std::printf("[host-import-inventory] MISSING required %s via %s :: %s\n",
                Entry.Image.c_str(), Entry.Library.c_str(),
                Entry.Symbol.c_str());
  for (const HostImportKey &Entry : MissingWeak)
    std::printf("[host-import-inventory] MISSING weak %s via %s :: %s\n",
                Entry.Image.c_str(), Entry.Library.c_str(),
                Entry.Symbol.c_str());

  if (RequiredHostImports.empty() && WeakHostImports.empty())
    std::printf("[host-import-inventory] WARNING: no host-facing imports matched; this is diagnostic failure, not evidence that the host ABI is complete\n");

  FreeLibrary(Bridge);
}

} // namespace ipasim::probe
