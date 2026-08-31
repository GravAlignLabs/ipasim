// Emulator.cpp: Implementation of class `Emulator`.

#include "ipasim/Emulator.hpp"

#include "ipasim/DynamicLoader.hpp"
#include "ipasim/IpaSimulator.hpp"

#include <unicorn/unicorn.h>
#include <unicorn/arm64.h>

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
