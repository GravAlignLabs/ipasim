// IpaSimulator.cpp: Implementation of class `IpaSimulator` and
// `IpaSimLibrary`'s public API.

#include "ipasim/IpaSimulator.hpp"
#include "ipasim/Probe.hpp"

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
