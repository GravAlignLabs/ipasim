// Emulator.cpp: Implementation of class `Emulator`.

#include "ipasim/Emulator.hpp"

#include "ipasim/DynamicLoader.hpp"
#include "ipasim/IpaSimulator.hpp"

#include <unicorn/unicorn.h>
#include <unicorn/arm64.h>

#include <algorithm>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace ipasim;

Emulator::~Emulator() {
  if (UC)
    callUCStatic(uc_close(UC));
}

uint64_t Emulator::readReg(int RegId) {
  uint64_t Result = 0;
  callUC(uc_reg_read(UC, RegId, &Result));
  return Result;
}
void Emulator::writeReg(int RegId, uint64_t Value) {
  callUC(uc_reg_write(UC, RegId, &Value));
}

bool Emulator::mapMemoryImpl(uint64_t Addr, uint64_t Size, uc_prot Perms) {
  if (uc_err Error = uc_mem_map_ptr(UC, Addr, Size, Perms,
                                    reinterpret_cast<void *>(Addr))) {
    Log.error() << "couldn't map memory at 0x" << to_hex_string(Addr)
                << " of size 0x" << to_hex_string(Size) << ": "
                << uc_strerror(Error) << Log.end();
    return false;
  }
  return true;
}

// Shared guest mappings are backed by the Windows process at the identical
// address. Record only successful mappings so a later Unicorn execution context
// can replay exactly the process memory that this engine can see.
bool Emulator::mapMemory(uint64_t Addr, uint64_t Size, uc_prot Perms) {
  if (!mapMemoryImpl(Addr, Size, Perms))
    return false;
  Dyld.recordSharedMemory(Addr, Size, Perms);
  return true;
}

// Per-execution-context mappings (currently guest stacks) must never be
// replayed into another Unicorn engine.
bool Emulator::mapPrivateMemory(uint64_t Addr, uint64_t Size, uc_prot Perms) {
  return mapMemoryImpl(Addr, Size, Perms);
}

void Emulator::start(uint64_t Addr) { callUC(uc_emu_start(UC, Addr, 0, 0, 0)); }

void Emulator::stop() { callUC(uc_emu_stop(UC)); }

void Emulator::hook(uc_hook_type Type, void *Handler, void *Instance) {
  uc_hook Hook;
  callUC(uc_hook_add(UC, &Hook, Type, Handler, Instance, 1, 0));
}

void Emulator::ignoreNextError() {
  assert(!IgnoreError && "Only one next error can be ignored.");
  IgnoreError = true;
}

uc_engine *Emulator::initUC() {
  uc_engine *UC;
  // Modern iOS device binaries are AArch64. The previous unconditional
  // UC_ARCH_ARM engine made every 64-bit IPA impossible to execute.
  callUCStatic(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &UC));
  return UC;
}

void Emulator::callUCStatic(uc_err Err) {
  if (Err != UC_ERR_OK)
    Log.error() << "unicorn failed: " << uc_strerror(Err) << Log.end();
}

void Emulator::callUC(uc_err Err) {
  if (Err != UC_ERR_OK) {
    if (IgnoreError)
      IgnoreError = false;
    else
      Log.error() << "unicorn failed at "
                  << Dyld.dumpAddr(readReg(UC_ARM64_REG_PC)) << ": "
                  << uc_strerror(Err) << Log.end();
  }
}

// =============================================================================
// Shared guest process address space
// =============================================================================

void DynamicLoader::recordSharedMemory(uint64_t Address, uint64_t Size,
                                       uc_prot Permissions) {
  if (Size == 0 || (Address & (PageSize - 1)) != 0 ||
      (Size & (PageSize - 1)) != 0) {
    Log.error() << "invalid shared guest mapping 0x" << to_hex_string(Address)
                << "+0x" << to_hex_string(Size) << Log.end();
    return;
  }

  const uint64_t End = Address + Size;
  if (End < Address) {
    Log.error("shared guest mapping address overflow");
    return;
  }

  std::lock_guard<std::mutex> Guard(SharedMemoryMutex);
  for (const SharedMemoryMapping &Mapping : SharedMemoryMappings) {
    if (Mapping.Address == Address && Mapping.Size == Size &&
        Mapping.Permissions == Permissions)
      return;

    const uint64_t MappingEnd = Mapping.Address + Mapping.Size;
    if (Address < MappingEnd && End > Mapping.Address) {
      // A second successful uc_mem_map_ptr() cannot overlap an existing map in
      // the same engine. Seeing an overlap here therefore indicates that two
      // execution contexts discovered inconsistent descriptions of one guest
      // region. Keep the first authoritative description and fail visibly.
      Log.error() << "conflicting shared guest mappings at 0x"
                  << to_hex_string(Address) << " and 0x"
                  << to_hex_string(Mapping.Address) << Log.end();
      return;
    }
  }

  SharedMemoryMappings.push_back(
      SharedMemoryMapping{Address, Size, Permissions});
}

void DynamicLoader::registerEmulator(Emulator &ExecutionEmulator) {
  std::vector<SharedMemoryMapping> Snapshot;
  {
    std::lock_guard<std::mutex> Guard(SharedMemoryMutex);
    Snapshot = SharedMemoryMappings;
  }

  for (const SharedMemoryMapping &Mapping : Snapshot)
    ExecutionEmulator.mapPrivateMemory(Mapping.Address, Mapping.Size,
                                       Mapping.Permissions);
}

bool DynamicLoader::mapKnownSharedMemory(Emulator &ExecutionEmulator,
                                         uint64_t Address) {
  SharedMemoryMapping Found{};
  bool HasMapping = false;
  {
    std::lock_guard<std::mutex> Guard(SharedMemoryMutex);
    for (const SharedMemoryMapping &Mapping : SharedMemoryMappings) {
      if (Address >= Mapping.Address &&
          Address - Mapping.Address < Mapping.Size) {
        Found = Mapping;
        HasMapping = true;
        break;
      }
    }
  }

  if (!HasMapping)
    return false;
  return ExecutionEmulator.mapPrivateMemory(Found.Address, Found.Size,
                                            Found.Permissions);
}

bool DynamicLoader::mapExternalSharedMemory(Emulator &ExecutionEmulator,
                                            uint64_t Address,
                                            uint64_t Size) {
  if (Size == 0)
    return false;

  uint64_t Start = alignToPageSize(Address);
  uint64_t End = roundToPageSize(Address + Size);
  if (End <= Start)
    return false;

  if (mapKnownSharedMemory(ExecutionEmulator, Start))
    return true;

#ifdef _WIN32
  // uc_mem_map_ptr() dereferences the supplied Windows backing memory. Fail
  // closed for genuinely invalid guest pointers instead of registering a host
  // address that would later fault outside Unicorn.
  uint64_t Cursor = Start;
  while (Cursor < End) {
    MEMORY_BASIC_INFORMATION Information{};
    if (VirtualQuery(reinterpret_cast<void *>(Cursor), &Information,
                     sizeof(Information)) == 0 ||
        Information.State != MEM_COMMIT ||
        (Information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
      return false;
    }

    const uint64_t RegionBase =
        reinterpret_cast<uint64_t>(Information.BaseAddress);
    const uint64_t RegionEnd = RegionBase + Information.RegionSize;
    if (RegionEnd <= Cursor)
      return false;
    Cursor = std::min(End, RegionEnd);
  }
#endif

  return ExecutionEmulator.mapMemory(Start, End - Start,
                                     UC_PROT_READ | UC_PROT_WRITE);
}
