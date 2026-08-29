// LoadedLibrary.cpp: Implementation of class `LoadedLibrary` and its
// descendants.

#include "ipasim/LoadedLibrary.hpp"

#include "ipasim/DynamicLoader.hpp"
#include "ipasim/IpaSimulator.hpp"

using namespace ipasim;
using namespace std;

DylibSymbolIterator DylibSymbolIterator::begin() {
  return DylibSymbolIterator(RVA, Symbols.begin()).next();
}

DylibSymbolIterator &DylibSymbolIterator::next() {
  while (Symbols != Symbols.end() && Symbols->value() != RVA)
    ++Symbols;
  return *this;
}

DylibSymbolIterator &DylibSymbolIterator::operator++() {
  ++Symbols;
  return next();
}

bool DylibSymbolIterator::operator!=(const DylibSymbolIterator &Other) {
  return Symbols != Other.Symbols;
}

LIEF::MachO::Symbol &DylibSymbolIterator::operator*() { return *Symbols; }

bool LoadedLibrary::isInRange(uint64_t Addr) {
  const uint64_t Base = MappedStartAddress ? MappedStartAddress : StartAddress;
  return Addr >= Base && Addr - Base < Size;
}

void LoadedLibrary::checkInRange(uint64_t Addr) {
  if (!isInRange(Addr))
    Log.error() << "address " << Addr << " out of range" << Log.end();
}

uint64_t LoadedDylib::findSymbol(DynamicLoader &DL, const string &Name) {
  using namespace LIEF::MachO;

  // LC_DYLD_EXPORTS_TRIE is authoritative for modern images and is parsed by
  // ipaSim directly because the vendored LIEF version predates that command.
  if (const ModernExport *Export = modernExport(Name)) {
    switch (Export->Type) {
    case ModernExport::Kind::Regular:
      // Regular export-trie addresses are offsets from the Mach-O header.
      return loadedImageBase() + Export->Value;
    case ModernExport::Kind::Absolute:
      return Export->Value;
    case ModernExport::Kind::Reexport: {
      const string &ImportName =
          Export->ImportName.empty() ? Name : Export->ImportName;
      return DL.resolveSymbol(*this, Export->LibOrdinal, ImportName);
    }
    case ModernExport::Kind::ThreadLocal:
      Log.error() << "thread-local export " << Name
                  << " requires TLS runtime support" << Log.end();
      return 0;
    case ModernExport::Kind::Resolver:
      Log.error() << "resolver export " << Name
                  << " requires resolver invocation support" << Log.end();
      return 0;
    }
  }

  if (!Bin.has_symbol(Name)) {
    // Try also classic re-exported libraries.
    for (DylibCommand &Lib : Bin.libraries()) {
      if (Lib.command() != LOAD_COMMAND_TYPES::LC_REEXPORT_DYLIB)
        continue;

      LoadedLibrary *LL = DL.load(Lib.name());
      if (!LL)
        continue;

      // Windows DLL exports do not carry Mach-O's leading underscore.
      uint64_t SymAddr;
      if (!LL->hasUnderscorePrefix() && !Name.empty() && Name[0] == '_')
        SymAddr = LL->findSymbol(DL, Name.substr(1));
      else
        SymAddr = LL->findSymbol(DL, Name);

      if (SymAddr)
        return SymAddr;
    }
    return 0;
  }
  return StartAddress + Bin.get_symbol(Name).value();
}

uint64_t LoadedDll::findSymbol(DynamicLoader &DL, const string &Name) {
  return (uint64_t)GetProcAddress(Ptr, Name.c_str());
}

DylibSymbolIterator LoadedDylib::lookup(uint64_t Addr) {
  uint64_t RVA = Addr - StartAddress;
  return DylibSymbolIterator(RVA, Bin.exported_symbols());
}
