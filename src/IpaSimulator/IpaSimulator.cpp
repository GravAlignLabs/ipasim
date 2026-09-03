// IpaSimulator.cpp: Implementation of class `IpaSimulator` and
// `IpaSimLibrary`'s public API.

#include "ipasim/IpaSimulator.hpp"
#include "ipasim/Probe.hpp"

#include "ipasim/DwarfsRuntimeRootStore.hpp"
#include "ipasim/DynamicLoader.hpp"
#include "ipasim/LoadedLibrary.hpp"

#include <memory>
#include <string>
#include <system_error>
#include <thread>

using namespace ipasim;
using namespace std;

#if !defined(IPASIM_MODERN_CORE)
using namespace winrt;
using namespace Windows::ApplicationModel::Activation;
#endif

namespace {
thread_local SysTranslator *ActiveSysTranslator = nullptr;
} // namespace

// TODO: This Emu-Dyld circular reference is not very cool.
IpaSimulator::IpaSimulator() : Emu(Dyld), Dyld(Emu), Sys(Dyld, Emu) {}

ScopedSysTranslatorActivation::ScopedSysTranslatorActivation(SysTranslator &Sys)
    : Previous(ActiveSysTranslator) {
  ActiveSysTranslator = &Sys;
}

ScopedSysTranslatorActivation::~ScopedSysTranslatorActivation() {
  ActiveSysTranslator = Previous;
}

SysTranslator &ipasim::currentSysTranslator() {
  return ActiveSysTranslator ? *ActiveSysTranslator : IpaSim.Sys;
}

unique_ptr<GuestExecutionContext> IpaSimulator::createExecutionContext() {
  auto Context = make_unique<GuestExecutionContext>();
  Context->Emu = make_unique<Emulator>(Dyld);
  if (!Dyld.registerEmulator(*Context->Emu)) {
    Log.error("could not replay guest process mappings into worker CPU context");
    return nullptr;
  }

  Context->Sys = make_unique<SysTranslator>(Dyld, *Context->Emu);
  if (!Context->Sys->initializeExecutionContext()) {
    Log.error("could not initialize worker ARM64 execution context");
    return nullptr;
  }
  return Context;
}

#if !defined(IPASIM_MODERN_CORE)
void ipasim::start(const hstring &Path,
                   const LaunchActivatedEventArgs &LaunchArgs) {
  IpaSim.MainBinary = to_string(Path);
  LoadedLibrary *App = IpaSim.Dyld.load(IpaSim.MainBinary);
  if (!App)
    return;

  IpaSim.Sys.execute(App);
  IpaSim.Sys.call("UIKit.dll", "UIApplicationLaunched", get_abi(LaunchArgs));
}
TextBlockProvider &ipasim::logText() { return IpaSim.LogText; }
#endif

void ipasim::error(const char *Message) { Log.error(Message); }

IpaSimulator ipasim::IpaSim;

#if defined(IPASIM_MODERN_CORE)
RuntimeLogger ipasim::Log = RuntimeLogger(
    LogStream(DebugStream(), StdStream::out()),
    LogStream(DebugStream(), StdStream::err()));
#else
RuntimeLogger ipasim::Log = RuntimeLogger(
    LogStream(DebugStream(), TextBlockStream(false, IpaSim.LogText)),
    LogStream(DebugStream(), TextBlockStream(true, IpaSim.LogText)));
#endif

IPASIM_API int ipaSim_setRuntimeRoot(const char *Path) {
  if (!Path)
    return 64;

  if (!IpaSim.Dyld.setRuntimeRoot(Path)) {
    Log.error() << "invalid iOS runtime root: " << Path << Log.end();
    return 65;
  }
  return 0;
}

IPASIM_API int ipaSim_setDwarfsRuntimeRoot(const char *ImagePath,
                                           const char *ReaderBridgePath) {
#if defined(IPASIM_MODERN_CORE)
  if (!ImagePath || !*ImagePath || !ReaderBridgePath || !*ReaderBridgePath)
    return 64;

  string Error;
  auto Store = makeDwarfsRuntimeRootStore(ImagePath, ReaderBridgePath, Error);
  if (!Store) {
    // Match the directory setter's fail-closed behavior: an invalid replacement
    // cannot leave a previously configured RuntimeRoot active.
    IpaSim.Dyld.setRuntimeRootStore(nullptr);
    Log.error() << "invalid DwarFS iOS runtime root: " << Error << Log.end();
    return 65;
  }

  if (!IpaSim.Dyld.setRuntimeRootStore(std::move(Store)))
    return 65;
  return 0;
#else
  (void)ImagePath;
  (void)ReaderBridgePath;
  return 65;
#endif
}

IPASIM_API int ipaSim_probeImage(const char *Path) {
  if (!Path || !*Path)
    return 64;

  IpaSim.MainBinary = Path;
  LoadedLibrary *App = IpaSim.Dyld.load(IpaSim.MainBinary);
  return App ? 0 : 2;
}

IPASIM_API int ipaSim_executeImage(const char *Path, uint64_t *ReturnValue) {
  if (!Path || !*Path || !ReturnValue)
    return 64;

  IpaSim.MainBinary = Path;
  LoadedLibrary *App = IpaSim.Dyld.load(IpaSim.MainBinary);
  if (!App)
    return 2;

  if (!dynamic_cast<LoadedDylib *>(App)) {
    Log.error("loaded image is not an executable Mach-O dylib image");
    return 3;
  }

  IpaSim.Sys.execute(App);
  *ReturnValue = IpaSim.Emu.readReg(UC_ARM64_REG_X0);
  return 0;
}

IPASIM_API int ipaSim_executeImageThreaded(const char *Path,
                                           uint64_t *ReturnValue) {
  if (!Path || !*Path || !ReturnValue)
    return 64;

  IpaSim.MainBinary = Path;
  LoadedLibrary *App = IpaSim.Dyld.load(IpaSim.MainBinary);
  auto *Dylib = dynamic_cast<LoadedDylib *>(App);
  if (!Dylib)
    return App ? 3 : 2;

  const uint64_t Entry = Dylib->Bin.entrypoint() + Dylib->StartAddress;
  uint64_t WorkerReturn = 0;
  bool WorkerSucceeded = false;

  try {
    thread Worker([&]() {
      auto Context = IpaSim.createExecutionContext();
      if (!Context)
        return;
      ScopedSysTranslatorActivation Active(*Context->Sys);
      WorkerReturn = reinterpret_cast<uint64_t>(
          Context->Sys->callBackR(reinterpret_cast<void *>(Entry)));
      WorkerSucceeded = true;
    });
    Worker.join();
  } catch (const system_error &Error) {
    Log.error() << "could not create Windows host thread for threaded probe: "
                << Error.what() << Log.end();
    return 71;
  }

  if (!WorkerSucceeded)
    return 72;
  *ReturnValue = WorkerReturn;
  return 0;
}

// Run one guest pthread start routine on a fresh ARM64 CPU/register/stack
// context in the calling Windows thread. DarwinPthreadCoreBridge owns the
// Windows pthread lifetime; this routine is deliberately synchronous so the
// host pthread identity/TSD state and its Unicorn context stay on one thread.
IPASIM_API int ipaSim_runGuestPthread(void *FP, void *Arg0,
                                     void **ReturnValue) {
  if (!FP || !ReturnValue)
    return 64;

  auto Context = IpaSim.createExecutionContext();
  if (!Context)
    return 72;

  ScopedSysTranslatorActivation Active(*Context->Sys);
  *ReturnValue = Context->Sys->callBackR(FP, Arg0);
  return 0;
}

// Darwin pthread_get_stackaddr_np reports the actual top of the current
// downward-growing guest stack. Return no value when there is no active ARM64
// execution context rather than leaking a native Windows stack or fabricating a
// guest address.
IPASIM_API void *ipaSim_currentGuestStackTop() {
  if (!ActiveSysTranslator)
    return nullptr;

  void *Base = nullptr;
  size_t Size = 0;
  if (!ActiveSysTranslator->getExecutionStackBounds(Base, Size) || !Base ||
      Size == 0)
    return nullptr;
  return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(Base) + Size);
}

// Return 1 only for the process's primary SysTranslator, 0 for an active worker
// context, and -1 when called outside guest execution. The Darwin host bridge
// uses this to avoid equating DLL-load thread identity with guest main-thread
// identity.
IPASIM_API int ipaSim_isMainGuestContext() {
  if (!ActiveSysTranslator)
    return -1;
  return ActiveSysTranslator == &IpaSim.Sys ? 1 : 0;
}

// Request a non-local Darwin pthread exit without terminating the backing
// Windows thread. SysTranslator stops resumption after the current native bridge
// call and lets ipaSim_runGuestPthread unwind its C++ ownership normally.
IPASIM_API int ipaSim_requestGuestPthreadExit(void *ExitValue) {
  if (!ActiveSysTranslator)
    return 3;
  ActiveSysTranslator->requestGuestThreadExit(ExitValue);
  return 0;
}

IPASIM_API void *ipaSim_translate(void *FP) {
  return currentSysTranslator().translate(FP);
}
IPASIM_API void ipaSim_translate4(uint64_t *Addr) {
  Addr[1] = reinterpret_cast<uint64_t>(currentSysTranslator().translate(
      reinterpret_cast<void *>(Addr[1])));
}
IPASIM_API void *ipaSim_translateC(void *FP, size_t ArgC) {
  return currentSysTranslator().translate(FP, ArgC);
}
IPASIM_API const char *ipaSim_processPath() {
  return IpaSim.MainBinary.c_str();
}
IPASIM_API void ipaSim_callBack1(void *FP, void *Arg0) {
  currentSysTranslator().callBack(FP, Arg0);
}

// Workqueue workers execute on independent ARM64 CPU/register/stack contexts.
// The host worker is detached because Darwin workqueue requests are asynchronous;
// GuestExecutionContext owns the Unicorn CPU and stack until the callback exits.
IPASIM_API void ipaSim_callBack1Threaded(void *FP, void *Arg0) {
  try {
    thread([FP, Arg0]() {
      auto Context = IpaSim.createExecutionContext();
      if (!Context)
        return;
      ScopedSysTranslatorActivation Active(*Context->Sys);
      Context->Sys->callBack(FP, Arg0);
    }).detach();
  } catch (const system_error &Error) {
    Log.error() << "could not create Windows host thread for guest callback: "
                << Error.what() << Log.end();
  }
}

IPASIM_API void ipaSim_callBack2(void *FP, void *Arg0, void *Arg1) {
  currentSysTranslator().callBack(FP, Arg0, Arg1);
}
IPASIM_API void *ipaSim_callBack1r(void *FP, void *Arg0) {
  return currentSysTranslator().callBackR(FP, Arg0);
}
IPASIM_API void *ipaSim_callBack2r(void *FP, void *Arg0, void *Arg1) {
  return currentSysTranslator().callBackR(FP, Arg0, Arg1);
}
IPASIM_API void *ipaSim_callBack3r(void *FP, void *Arg0, void *Arg1,
                                   void *Arg2) {
  return currentSysTranslator().callBackR(FP, Arg0, Arg1, Arg2);
}
IPASIM_API void ipaSim_register(void *Hdr) { IpaSim.Dyld.registerMachO(Hdr); }
IPASIM_API void
_dyld_objc_notify_register(_dyld_objc_notify_mapped Mapped,
                           _dyld_objc_notify_init Init,
                           _dyld_objc_notify_unmapped Unmapped) {
  IpaSim.Dyld.registerHandler(Mapped, Init, Unmapped);
}
