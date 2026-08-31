// SysTranslator.hpp: Definition of classes `SysTranslator`, `DynamicCaller`,
// `DynamicBackCaller` and `TypeDecoder`.

#ifndef IPASIM_SYS_TRANSLATOR_HPP
#define IPASIM_SYS_TRANSLATOR_HPP

#include "ipasim/DynamicLoader.hpp"
#include "ipasim/Emulator.hpp"
#include "ipasim/LoadedLibrary.hpp"

#include <ffi.h>
#include <stack>

namespace ipasim {

class SysTranslator {
public:
  SysTranslator(DynamicLoader &Dyld, Emulator &Emu)
      : Dyld(Dyld), Emu(Emu), Restart(false), Continue(false),
        RestartFromLRs(false) {}
  void execute(LoadedLibrary *Lib);
  void execute(uint64_t Addr);

  // Prepare an additional CPU context for code inside the already-loaded guest
  // process. This installs its own stack/register state and translation hooks
  // without repeating process-wide image/Objective-C initialization.
  bool initializeExecutionContext();

  // Host bridge callbacks use the translator belonging to the ARM64 context
  // currently executing on this Windows thread. Nested guest->host->guest calls
  // therefore return to the correct CPU context instead of always using main.
  static SysTranslator *current();

  void *translate(void *FP);
  void *translate(void *FP, size_t ArgC, bool Returns = false);

  template <typename... Args>
  void call(const std::string &Lib, const std::string &Func,
            Args &&... Params) {
    LoadedLibrary *L = Dyld.load(Lib);
    if (!L)
      return;

    uint64_t Addr = L->findSymbol(Dyld, Func);
    if (!Addr) {
      Log.error() << "cannot find function " << Func << " in " << Lib
                  << Log.end();
      return;
    }

    auto *Ptr = reinterpret_cast<void (*)(Args...)>(Addr);
    Ptr(std::forward<Args>(Params)...);
  }

  template <typename... ArgTys> void callBack(void *FP, ArgTys... Args);
  template <typename... ArgTys> void *callBackR(void *FP, ArgTys... Args);

private:
  bool handleFetchProtMem(uc_mem_type Type, uint64_t Addr, int Size,
                          int64_t Value);
  void handleCode(uint64_t Addr, uint32_t Size);
  bool handleMemWrite(uc_mem_type Type, uint64_t Addr, int Size, int64_t Value);
  bool handleMemUnmapped(uc_mem_type Type, uint64_t Addr, int Size,
                         int64_t Value);
  void *createTrampoline(void *Addr, size_t ArgC, bool Returns);
  void handleTrampoline(void *Ret, void **Args, void *Data);
  static void handleTrampolineStatic(ffi_cif *, void *Ret, void **Args,
                                     void *Data);
  void returnToKernel();
  void returnToEmulation();
  void continueOutsideEmulation(std::function<void()> &&Cont);

  static constexpr ConstexprString WrapsPrefix = "$__ipaSim_wraps_";
  static constexpr uint64_t DLLBase = 0x1000;
  static thread_local SysTranslator *Current;

  DynamicLoader &Dyld;
  Emulator &Emu;
  std::stack<uint64_t> LRs;
  bool Restart, Continue, RestartFromLRs;
  std::function<void()> Continuation;
  bool ExecutionContextInitialized = false;
  void *ExecutionStack = nullptr;
};

class DynamicCaller {
public:
  DynamicCaller(Emulator &Emu)
      : Emu(Emu), RegId(UC_ARM64_REG_X0),
        SP(Emu.readReg(UC_ARM64_REG_SP)) {}
  void loadArg(size_t Size);
  bool call(bool Returns, uint64_t Addr);

private:
  template <size_t N> void call(bool Returns, uint64_t Addr) {
    if (Returns)
      call<N, N, true>(Addr);
    else
      call<N, N, false>(Addr);
  }
  template <size_t N, size_t C, bool Returns, typename... ArgTys>
  void call(uint64_t Addr, ArgTys... Params) {
    if constexpr (N > 0)
      call<N - 1, C, Returns, ArgTys..., uint64_t>(Addr, Params...,
                                                   Args[C - N]);
    else
      call<Returns, ArgTys...>(Addr, Params...);
  }
  template <bool Returns, typename... ArgTys>
  void call(uint64_t Addr, ArgTys... Params) {
    if constexpr (Returns) {
      uint64_t RetVal =
          reinterpret_cast<uint64_t (*)(ArgTys...)>(Addr)(Params...);
      Emu.writeReg(UC_ARM64_REG_X0, RetVal);
    } else
      reinterpret_cast<void (*)(ArgTys...)>(Addr)(Params...);
  }

  Emulator &Emu;
  int RegId;
  uint64_t SP;
  std::vector<uint64_t> Args;
};

class DynamicBackCaller {
public:
  DynamicBackCaller(DynamicLoader &Dyld, Emulator &Emu, SysTranslator &Sys)
      : Dyld(Dyld), Emu(Emu), Sys(Sys) {}

  template <typename RetTy, typename... ArgTys>
  RetTy callBack(void *FP, ArgTys... Args) {
    uint64_t Addr = reinterpret_cast<uint64_t>(FP);
    LibraryInfo LI(Dyld.lookup(Addr));
    if (!LI.Lib || LI.Lib->isDLL()) {
      return reinterpret_cast<RetTy (*)(ArgTys...)>(FP)(Args...);
    } else {
      pushArgs<UC_ARM64_REG_X0>(Args...);
      Sys.execute(Addr);

      if constexpr (!std::is_same_v<RetTy, void>)
        return reinterpret_cast<RetTy>(Emu.readReg(UC_ARM64_REG_X0));
    }
  }

private:
  template <int RegId> void pushArgs() {}
  template <int RegId, typename... ArgTys>
  void pushArgs(void *Arg, ArgTys... Args) {
    static_assert(UC_ARM64_REG_X0 <= RegId && RegId <= UC_ARM64_REG_X7,
                  "Callback has too many arguments.");
    Emu.writeReg(RegId, reinterpret_cast<uint64_t>(Arg));
    pushArgs<RegId + 1>(Args...);
  }

  DynamicLoader &Dyld;
  Emulator &Emu;
  SysTranslator &Sys;
};

class TypeDecoder {
public:
  TypeDecoder(const char *Encoding) : T(Encoding) {
    if (Encoding && !isIntegerPointerSignature(Encoding)) {
      Log.error("Objective-C signature requires unsupported AAPCS64 FP/SIMD/aggregate ABI handling");
      T = InvalidEncoding;
    }
  }
  size_t getNextTypeSize();
  bool hasNext() { return *T; }

  static const size_t InvalidSize = static_cast<size_t>(-1);

private:
  const char *T;
  static constexpr const char *InvalidEncoding = "!";

  static void skipQuoted(const char *&P) {
    if (*P != '"')
      return;
    ++P;
    while (*P && *P != '"')
      ++P;
    if (*P == '"')
      ++P;
  }

  static bool consumeType(const char *&P, bool TopLevel) {
    while (*P == 'r' || *P == 'n' || *P == 'N' || *P == 'o' ||
           *P == 'O' || *P == 'R' || *P == 'V')
      ++P;
    if (!*P)
      return false;

    const char C = *P++;
    switch (C) {
    case 'v':
    case 'c':
    case 'C':
    case 's':
    case 'S':
    case 'i':
    case 'I':
    case 'l':
    case 'L':
    case 'q':
    case 'Q':
    case 'B':
    case '#':
    case ':':
    case '*':
      return true;
    case '@':
      if (*P == '?')
        ++P;
      else if (*P == '"')
        skipQuoted(P);
      return true;
    case '^':
      return consumeType(P, false);
    case 'f':
    case 'd':
    case 'D':
      return !TopLevel;
    case 'b':
      while (*P >= '0' && *P <= '9')
        ++P;
      return !TopLevel;
    case '[': {
      while (*P >= '0' && *P <= '9')
        ++P;
      bool SyntaxOK = consumeType(P, false);
      if (*P != ']')
        return false;
      ++P;
      return SyntaxOK && !TopLevel;
    }
    case '{':
    case '(': {
      const char Close = C == '{' ? '}' : ')';
      while (*P && *P != '=' && *P != Close)
        ++P;
      if (!*P)
        return false;
      if (*P == '=') {
        ++P;
        while (*P && *P != Close) {
          if (*P == '"') {
            skipQuoted(P);
            continue;
          }
          if (!consumeType(P, false))
            return false;
        }
      }
      if (*P != Close)
        return false;
      ++P;
      return !TopLevel;
    }
    case '?':
      return !TopLevel;
    default:
      return false;
    }
  }

  static bool isIntegerPointerSignature(const char *P) {
    if (!P)
      return false;
    while (*P) {
      if (!consumeType(P, true))
        return false;
      while (*P >= '0' && *P <= '9')
        ++P;
    }
    return true;
  }

  size_t getNextTypeSizeImpl();
};

template <typename... ArgTys>
inline void SysTranslator::callBack(void *FP, ArgTys... Args) {
  DynamicBackCaller(Dyld, Emu, *this).callBack<void, ArgTys...>(FP, Args...);
}
template <typename... ArgTys>
inline void *SysTranslator::callBackR(void *FP, ArgTys... Args) {
  return DynamicBackCaller(Dyld, Emu, *this)
      .callBack<void *, ArgTys...>(FP, Args...);
}

} // namespace ipasim

#endif
