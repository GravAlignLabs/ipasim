// Emulator.cpp: Implementation of class `Emulator`.

#include "ipasim/Emulator.hpp"

#include "ipasim/DynamicLoader.hpp"
#include "ipasim/IpaSimulator.hpp"

#include <unicorn/unicorn.h>
#include <unicorn/arm64.h>

#include <algorithm>
#include <limits>
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

bool Emulator::readVectorReg(int RegId,
                             std::array<unsigned char, 16> &Value) {
  Value.fill(0);
  const uc_err Error = uc_reg_read(UC, RegId, Value.data());
  if (Error != UC_ERR_OK) {
    Log.error() << "couldn't read ARM64 vector register " << RegId << ": "
                << uc_strerror(Error) << Log.end();
    return false;
  }
  return true;
}

bool Emulator::writeVectorReg(int RegId,
                              const std::array<unsigned char, 16> &Value) {
  const uc_err Error = uc_reg_write(UC, RegId, Value.data());
  if (Error != UC_ERR_OK) {
    Log.error() << "couldn't write ARM64 vector register " << RegId << ": "
                << uc_strerror(Error) << Log.end();
    return false;
  }
  return true;
}

bool Emulator::readMemory(uint64_t Addr, void *Buffer, std::size_t Size) {
  if (Size == 0)
    return true;
  if (!Buffer) {
    Log.error("cannot read guest memory into a null buffer");
    return false;
  }
  const uc_err Error = uc_mem_read(UC, Addr, Buffer, Size);
  if (Error != UC_ERR_OK) {
    Log.error() << "couldn't read guest memory at 0x" << to_hex_string(Addr)
                << " of size 0x" << to_hex_string(Size) << ": "
                << uc_strerror(Error) << Log.end();
    return false;
  }
  return true;
}

bool Emulator::writeMemory(uint64_t Addr, const void *Buffer, std::size_t Size) {
  if (Size == 0)
    return true;
  if (!Buffer) {
    Log.error("cannot write guest memory from a null buffer");
    return false;
  }
  const uc_err Error = uc_mem_write(UC, Addr, Buffer, Size);
  if (Error != UC_ERR_OK) {
    Log.error() << "couldn't write guest memory at 0x" << to_hex_string(Addr)
                << " of size 0x" << to_hex_string(Size) << ": "
                << uc_strerror(Error) << Log.end();
    return false;
  }
  return true;
}

bool Emulator::mapMemoryImpl(uint64_t Addr, uint64_t Size, uc_prot Perms) {
  uc_err Error = uc_mem_map_ptr(UC, Addr, Size, Perms,
                                reinterpret_cast<void *>(Addr));
  if (Error != UC_ERR_OK) {
    Log.error() << "couldn't map memory at 0x" << to_hex_string(Addr)
                << " of size 0x" << to_hex_string(Size) << ": "
                << uc_strerror(Error) << Log.end();
    return false;
  }
  return true;
}

bool Emulator::mapMemory(uint64_t Addr, uint64_t Size, uc_prot Perms) {
  if (!mapMemoryImpl(Addr, Size, Perms))
    return false;
  Dyld.recordSharedMemory(Addr, Size, Perms);
  return true;
}

bool Emulator::mapRecordedMemory(uint64_t Addr, uint64_t Size, uc_prot Perms) {
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
  if (Address > std::numeric_limits<uint64_t>::max() - Size) {
    Log.error("shared guest mapping address overflow");
    return;
  }

  const uint64_t End = Address + Size;
  std::lock_guard<std::mutex> Guard(SharedMemoryMutex);
  for (const SharedMemoryMapping &Mapping : SharedMemoryMappings) {
    if (Mapping.Address == Address && Mapping.Size == Size &&
        Mapping.Permissions == Permissions)
      return;

    const uint64_t MappingEnd = Mapping.Address + Mapping.Size;
    if (Address < MappingEnd && End > Mapping.Address) {
      Log.error() << "conflicting shared guest mappings at 0x"
                  << to_hex_string(Address) << " and 0x"
                  << to_hex_string(Mapping.Address) << Log.end();
      return;
    }
  }

  SharedMemoryMappings.push_back(
      SharedMemoryMapping{Address, Size, Permissions});
}

bool DynamicLoader::registerEmulator(Emulator &ExecutionEmulator) {
  std::vector<SharedMemoryMapping> Snapshot;
  {
    std::lock_guard<std::mutex> Guard(SharedMemoryMutex);
    Snapshot = SharedMemoryMappings;
  }

  for (const SharedMemoryMapping &Mapping : Snapshot) {
    if (!ExecutionEmulator.mapRecordedMemory(
            Mapping.Address, Mapping.Size, Mapping.Permissions))
      return false;
  }
  return true;
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
  return ExecutionEmulator.mapRecordedMemory(Found.Address, Found.Size,
                                              Found.Permissions);
}

bool DynamicLoader::mapExternalSharedMemory(Emulator &ExecutionEmulator,
                                            uint64_t Address,
                                            uint64_t Size) {
  if (Size == 0 || Address > std::numeric_limits<uint64_t>::max() - Size)
    return false;

  const uint64_t Start = alignToPageSize(Address);
  const uint64_t End = roundToPageSize(Address + Size);
  if (End <= Start)
    return false;

  if (mapKnownSharedMemory(ExecutionEmulator, Start))
    return true;

#ifdef _WIN32
  uint64_t Cursor = Start;
  while (Cursor < End) {
    MEMORY_BASIC_INFORMATION Information{};
    if (VirtualQuery(reinterpret_cast<void *>(Cursor), &Information,
                     sizeof(Information)) == 0 ||
        Information.State != MEM_COMMIT ||
        (Information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
      return false;

    const uint64_t RegionBase =
        reinterpret_cast<uint64_t>(Information.BaseAddress);
    if (RegionBase > std::numeric_limits<uint64_t>::max() -
                         Information.RegionSize)
      return false;
    const uint64_t RegionEnd = RegionBase + Information.RegionSize;
    if (RegionEnd <= Cursor)
      return false;
    Cursor = std::min(End, RegionEnd);
  }
#endif

  return ExecutionEmulator.mapMemory(Start, End - Start,
                                     UC_PROT_READ | UC_PROT_WRITE);
}
