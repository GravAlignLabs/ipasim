// Arm64SharedMemorySmoke.cpp: prove that independent Unicorn ARM64 engines can
// execute concurrently over ipaSim's host-pointer-backed guest address space.
//
// Real pthreads need independent CPU/register contexts but one shared process
// memory. ipaSim already maps Mach-O pages with uc_mem_map_ptr() at their host
// addresses. This smoke establishes that the same backing pages can be mapped
// into multiple Unicorn engines, written concurrently, and observed across
// engines before the loader/thread runtime is refactored around that property.

#include <unicorn/arm64.h>
#include <unicorn/unicorn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <thread>

namespace {

constexpr std::size_t PageSize = 0x1000;
constexpr std::uint64_t ValueA = 0x1122334455667788ULL;
constexpr std::uint64_t ValueB = 0x8877665544332211ULL;

[[noreturn]] void fail(const char *Message) {
  std::fprintf(stderr, "[arm64-shared-memory-smoke] FAIL: %s\n", Message);
  std::exit(1);
}

void require(bool Condition, const char *Message) {
  if (!Condition)
    fail(Message);
}

void requireUc(uc_err Error, const char *Operation) {
  if (Error == UC_ERR_OK)
    return;
  std::fprintf(stderr, "[arm64-shared-memory-smoke] FAIL: %s: %s\n",
               Operation, uc_strerror(Error));
  std::exit(1);
}

uc_engine *openEngine() {
  uc_engine *Engine = nullptr;
  requireUc(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &Engine), "uc_open");
  require(Engine != nullptr, "uc_open returned a null engine");
  return Engine;
}

void writeReg(uc_engine *Engine, int Register, std::uint64_t Value) {
  requireUc(uc_reg_write(Engine, Register, &Value), "uc_reg_write");
}

std::uint64_t readReg(uc_engine *Engine, int Register) {
  std::uint64_t Value = 0;
  requireUc(uc_reg_read(Engine, Register, &Value), "uc_reg_read");
  return Value;
}

} // namespace

int main() {
  void *DataPage = _aligned_malloc(PageSize, PageSize);
  void *CodePage = _aligned_malloc(PageSize, PageSize);
  require(DataPage != nullptr && CodePage != nullptr,
          "could not allocate aligned host backing pages");
  std::memset(DataPage, 0, PageSize);
  std::memset(CodePage, 0, PageSize);

  const std::uint64_t DataAddress =
      reinterpret_cast<std::uint64_t>(DataPage);
  const std::uint64_t CodeAddress =
      reinterpret_cast<std::uint64_t>(CodePage);

  require((DataAddress & (PageSize - 1)) == 0,
          "data host page is not page aligned");
  require((CodeAddress & (PageSize - 1)) == 0,
          "code host page is not page aligned");

  // str x1, [x0]
  // ldr x2, [x0]
  // ret
  constexpr std::uint32_t Program[] = {
      0xF9000001u,
      0xF9400002u,
      0xD65F03C0u,
  };
  std::memcpy(CodePage, Program, sizeof(Program));

  uc_engine *EngineA = openEngine();
  uc_engine *EngineB = openEngine();

  for (uc_engine *Engine : {EngineA, EngineB}) {
    requireUc(uc_mem_map_ptr(Engine, DataAddress, PageSize,
                             UC_PROT_READ | UC_PROT_WRITE, DataPage),
              "map shared data page");
    requireUc(uc_mem_map_ptr(Engine, CodeAddress, PageSize,
                             UC_PROT_READ | UC_PROT_EXEC, CodePage),
              "map shared code page");
  }

  // Both engines execute at the same virtual code address while writing
  // distinct words in the same shared guest page. This is the shape needed by
  // independent guest pthread CPU contexts.
  std::thread ThreadA([&]() {
    writeReg(EngineA, UC_ARM64_REG_X0, DataAddress);
    writeReg(EngineA, UC_ARM64_REG_X1, ValueA);
    requireUc(uc_emu_start(EngineA, CodeAddress, 0, 0, 1),
              "engine A STR execution");
  });
  std::thread ThreadB([&]() {
    writeReg(EngineB, UC_ARM64_REG_X0, DataAddress + sizeof(std::uint64_t));
    writeReg(EngineB, UC_ARM64_REG_X1, ValueB);
    requireUc(uc_emu_start(EngineB, CodeAddress, 0, 0, 1),
              "engine B STR execution");
  });
  ThreadA.join();
  ThreadB.join();

  const auto *Words = reinterpret_cast<const std::uint64_t *>(DataPage);
  require(Words[0] == ValueA, "engine A write did not reach shared host page");
  require(Words[1] == ValueB, "engine B write did not reach shared host page");

  // Engine B must observe engine A's write through its own memory/TLB context.
  writeReg(EngineB, UC_ARM64_REG_X0, DataAddress);
  requireUc(uc_emu_start(EngineB, CodeAddress + sizeof(std::uint32_t), 0, 0, 1),
            "engine B LDR execution");
  require(readReg(EngineB, UC_ARM64_REG_X2) == ValueA,
          "engine B did not observe engine A shared-memory write");

  // And the visibility must be symmetric.
  writeReg(EngineA, UC_ARM64_REG_X0, DataAddress + sizeof(std::uint64_t));
  requireUc(uc_emu_start(EngineA, CodeAddress + sizeof(std::uint32_t), 0, 0, 1),
            "engine A LDR execution");
  require(readReg(EngineA, UC_ARM64_REG_X2) == ValueB,
          "engine A did not observe engine B shared-memory write");

  requireUc(uc_close(EngineA), "close engine A");
  requireUc(uc_close(EngineB), "close engine B");
  _aligned_free(CodePage);
  _aligned_free(DataPage);

  std::printf("[arm64-shared-memory-smoke] passed\n");
  return 0;
}
