// IpaSimulator.cpp: Implementation of class `IpaSimulator` and
// `IpaSimLibrary`'s public API.

#include "ipasim/IpaSimulator.hpp"
#include "ipasim/Probe.hpp"

#include "ipasim/DynamicLoader.hpp"
#include "ipasim/LoadedLibrary.hpp"

#include <string>

using namespace ipasim;
using namespace std;

#if !defined(IPASIM_MODERN_CORE)
using namespace winrt;
using namespace Windows::ApplicationModel::Activation;
#endif

// TODO: This Emu-Dyld circular reference is not very cool.
IpaSimulator::IpaSimulator() : Emu(Dyld), Dyld(Emu), Sys(Dyld, Emu) {}

#if !defined(IPASIM_MODERN_CORE)
void ipasim::start(const hstring &Path,
                   const LaunchActivatedEventArgs &LaunchArgs) {
  // Load the binary.
  IpaSim.MainBinary = to_string(Path);
  LoadedLibrary *App = IpaSim.Dyld.load(IpaSim.MainBinary);
  if (!App)
    return;

  // Execute it.
  IpaSim.Sys.execute(App);

  // Call `UIApplicationLaunched`. `get_abi` converts C++/WinRT object to its
  // C++/CX equivalent. The modern framework boundary will replace this legacy
  // WinObjC launch hook; it is left explicit until that replacement lands.
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

  // This is intentionally a loader-only checkpoint for tester builds. It uses
  // the same DynamicLoader and modern Mach-O path as normal ipaSim startup but
  // does not execute guest code or enter the incomplete UIKit/SwiftUI launch
  // boundary.
  IpaSim.MainBinary = Path;
  LoadedLibrary *App = IpaSim.Dyld.load(IpaSim.MainBinary);
  return App ? 0 : 2;
}

IPASIM_API int ipaSim_executeImage(const char *Path, uint64_t *ReturnValue) {
  if (!Path || !*Path || !ReturnValue)
    return 64;

  // This deliberately uses the same loader and SysTranslator execution path as
  // normal ipaSim startup. The synthetic CI image is intentionally framework-
  // free, so a successful X0 return proves a GitHub-built iOS ARM64 Mach-O was
  // loaded and executed on the Windows host rather than merely parsed.
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

IPASIM_API void *ipaSim_translate(void *FP) { return IpaSim.Sys.translate(FP); }
IPASIM_API void ipaSim_translate4(uint64_t *Addr) {
  // Historical export name retained for generated-wrapper compatibility; the
  // slot itself is now pointer-width for ARM64 guests.
  Addr[1] = reinterpret_cast<uint64_t>(
      IpaSim.Sys.translate(reinterpret_cast<void *>(Addr[1])));
}
IPASIM_API void *ipaSim_translateC(void *FP, size_t ArgC) {
  return IpaSim.Sys.translate(FP, ArgC);
}
IPASIM_API const char *ipaSim_processPath() {
  return IpaSim.MainBinary.c_str();
}
IPASIM_API void ipaSim_callBack1(void *FP, void *Arg0) {
  IpaSim.Sys.callBack(FP, Arg0);
}
IPASIM_API void ipaSim_callBack2(void *FP, void *Arg0, void *Arg1) {
  IpaSim.Sys.callBack(FP, Arg0, Arg1);
}
IPASIM_API void *ipaSim_callBack1r(void *FP, void *Arg0) {
  return IpaSim.Sys.callBackR(FP, Arg0);
}
IPASIM_API void *ipaSim_callBack2r(void *FP, void *Arg0, void *Arg1) {
  return IpaSim.Sys.callBackR(FP, Arg0, Arg1);
}
IPASIM_API void *ipaSim_callBack3r(void *FP, void *Arg0, void *Arg1,
                                   void *Arg2) {
  return IpaSim.Sys.callBackR(FP, Arg0, Arg1, Arg2);
}
IPASIM_API void ipaSim_register(void *Hdr) { IpaSim.Dyld.registerMachO(Hdr); }
IPASIM_API void
_dyld_objc_notify_register(_dyld_objc_notify_mapped Mapped,
                           _dyld_objc_notify_init Init,
                           _dyld_objc_notify_unmapped Unmapped) {
  IpaSim.Dyld.registerHandler(Mapped, Init, Unmapped);
}
