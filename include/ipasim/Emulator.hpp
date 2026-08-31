// Emulator.hpp: Definition of class `Emulator`.

#ifndef IPASIM_EMULATOR_HPP
#define IPASIM_EMULATOR_HPP

#include <cstdint>
#include <unicorn/unicorn.h>
#include <unicorn/arm64.h>
#include <utility>

namespace ipasim {

class DynamicLoader;

namespace hooks {

template <typename T, typename F> struct FunctionHelper;

// Template helper used in `Emulator::hook`.
template <typename T, typename RetTy, typename... ArgTys>
struct FunctionHelper<T, RetTy(ArgTys...)> {
  using F = RetTy(ArgTys...);

  struct DataTy {
    T *Instance;
    F T::*Handler;
  };

  static RetTy hook(uc_engine *, ArgTys... Args, void *Data) {
    auto *D = reinterpret_cast<DataTy *>(Data);
    return (D->Instance->*(D->Handler))(Args...);
  }
};

} // namespace hooks

// Wraps one ARM64 Unicorn CPU context. Guest process memory is backed directly
// by Windows pages at the same addresses; DynamicLoader records normal mappings
// so additional CPU contexts can replay the same address space.
class Emulator {
public:
  Emulator(DynamicLoader &Dyld)
      : UC(initUC()), Dyld(Dyld), IgnoreError(false) {}
  Emulator(const Emulator &) = delete;
  Emulator(Emulator &&E)
      : UC(nullptr), Dyld(E.Dyld), IgnoreError(E.IgnoreError) {
    std::swap(UC, E.UC);
  }
  ~Emulator();

  uint64_t readReg(int RegId);
  void writeReg(int RegId, uint64_t Value);

  // Map a shared guest-process region and record it for future CPU contexts.
  bool mapMemory(uint64_t Addr, uint64_t Size, uc_prot Perms);
  // Replay an already-recorded region into this CPU context without recording
  // it again.
  bool mapRecordedMemory(uint64_t Addr, uint64_t Size, uc_prot Perms);

  void start(uint64_t Addr);
  void stop();
  template <typename F>
  void hook(uc_hook_type Type, F *Handler, void *Instance) {
    hook(Type, reinterpret_cast<void *>(Handler), Instance);
  }
  void hook(uc_hook_type Type, void *Handler, void *Instance);
  template <typename T, typename F>
  void hook(uc_hook_type Type, F T::*Handler, T *Instance) {
    using Helper = hooks::FunctionHelper<T, F>;
    hook(Type, Helper::hook, new typename Helper::DataTy{Instance, Handler});
  }
  void ignoreNextError();

private:
  bool mapMemoryImpl(uint64_t Addr, uint64_t Size, uc_prot Perms);

  uc_engine *UC;
  DynamicLoader &Dyld;
  bool IgnoreError;

  static uc_engine *initUC();
  static void callUCStatic(uc_err Err);
  void callUC(uc_err Err);
};

} // namespace ipasim

#endif
