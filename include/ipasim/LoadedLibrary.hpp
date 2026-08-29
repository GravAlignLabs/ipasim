// LoadedLibrary.hpp: Definition of class `LoadedLibrary` and its descendants.

#ifndef IPASIM_LOADED_LIBRARY_HPP
#define IPASIM_LOADED_LIBRARY_HPP

#include "ipasim/Common.hpp"
#include "ipasim/Logger.hpp"
#include "ipasim/MachO.hpp"
#include "ipasim/ModernMachO.hpp"

#include <LIEF/LIEF.hpp>
#include <Windows.h>
#include <cassert>
#include <map>
#include <stdexcept>

namespace ipasim {

class DynamicLoader;

// Iterator over symbols of LIEF's Mach-O binary filtered by a RVA.
class DylibSymbolIterator {
public:
  DylibSymbolIterator(uint64_t RVA, LIEF::MachO::it_exported_symbols Symbols)
      : RVA(RVA), Symbols(std::move(Symbols)) {}

  DylibSymbolIterator begin();
  DylibSymbolIterator end() { return DylibSymbolIterator(RVA, Symbols.end()); }
  DylibSymbolIterator &next();
  DylibSymbolIterator IPASIM_PREFIX(++);
  bool operator!=(const DylibSymbolIterator &Other);
  LIEF::MachO::Symbol &operator*();

private:
  uint64_t RVA;
  LIEF::MachO::it_exported_symbols Symbols;
};

// Represents a dynamic library (or executable) loaded by `DynamicLoader`.
class LoadedLibrary {
public:
  LoadedLibrary()
      : StartAddress(0), MappedStartAddress(0), Size(0), IsWrapper(false) {}
  virtual ~LoadedLibrary() = default;

  // StartAddress retains ipaSim's historical meaning. For Mach-O images it is
  // the dyld slide; for PE images it is the mapped module base. Modern Mach-O
  // images also track their real lowest mapped address so address lookup does
  // not confuse a non-zero preferred image base with the runtime range.
  uint64_t StartAddress, MappedStartAddress, Size;
  bool IsWrapper;

  virtual bool isDylib() = 0;
  bool isDLL() { return !isDylib(); }
  virtual uint64_t findSymbol(DynamicLoader &DL, const std::string &Name) = 0;
  virtual bool hasUnderscorePrefix() = 0;
  bool isInRange(uint64_t Addr);
  void checkInRange(uint64_t Addr);
  virtual bool hasMachO() = 0;
  virtual MachO getMachO() = 0;
};

// A Mach-O image loaded via LIEF. Modern iOS device applications are ARM64;
// selecting the first fat-binary slice is therefore no longer acceptable.
class LoadedDylib : public LoadedLibrary {
private:
  std::unique_ptr<LIEF::MachO::FatBinary> Fat;
  uint64_t Header;
  std::map<std::string, ModernExport> ModernExports;

  static LIEF::MachO::Binary &selectArm64(LIEF::MachO::FatBinary &Fat) {
    using namespace LIEF::MachO;
    for (size_t I = 0; I != Fat.size(); ++I) {
      Binary &Candidate = Fat.at(I);
      if (Candidate.header().cpu_type() == CPU_TYPES::CPU_TYPE_ARM64)
        return Candidate;
    }
    throw std::runtime_error("Mach-O image contains no ARM64 slice");
  }

public:
  LIEF::MachO::Binary &Bin;

  LoadedDylib(std::unique_ptr<LIEF::MachO::FatBinary> &&ParsedFat)
      : Fat(move(ParsedFat)), Header(0), Bin(selectArm64(*Fat)) {}

  bool isDylib() override { return true; }
  uint64_t findSymbol(DynamicLoader &DL, const std::string &Name) override;
  // TODO: Use this function to implement `src/objc/dladdr.mm`.
  DylibSymbolIterator lookup(uint64_t Addr);
  bool hasUnderscorePrefix() override { return true; }
  bool hasMachO() override { return true; }
  uint64_t loadedImageBase() const { return StartAddress + Bin.imagebase(); }
  MachO getMachO() override {
    if (!Header)
      Header = loadedImageBase();
    return MachO(reinterpret_cast<const void *>(Header));
  }

  void modernExports(std::map<std::string, ModernExport> &&Exports) {
    ModernExports = std::move(Exports);
  }
  const ModernExport *modernExport(const std::string &Name) const {
    auto I = ModernExports.find(Name);
    return I == ModernExports.end() ? nullptr : &I->second;
  }
};

// A `.dll` loaded via Windows API.
class LoadedDll : public LoadedLibrary {
public:
  HMODULE Ptr;
  bool MachOPoser;

  bool isDylib() override { return false; }
  uint64_t findSymbol(DynamicLoader &DL, const std::string &Name) override;
  bool hasUnderscorePrefix() override { return false; }
  bool hasMachO() override { return MachOPoser; }
  MachO getMachO() override {
    assert(hasMachO());
    return MachO(reinterpret_cast<const void *>(StartAddress));
  }
};

} // namespace ipasim

// !defined(IPASIM_LOADED_LIBRARY_HPP)
#endif
