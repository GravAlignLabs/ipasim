// Probe.hpp: Minimal C ABI for exercising ipaSim's real loader from a console
// test host without entering the incomplete modern SwiftUI launch boundary.

#ifndef IPASIM_PROBE_HPP
#define IPASIM_PROBE_HPP

#include "ipasim/Common.hpp"

#include <cstdint>

namespace ipasim {
class RuntimeRootStore;
}

// Configure a filesystem root that mirrors iOS absolute install names. Passing
// an empty string clears the root. Returns 0 on success or a non-zero value for
// an invalid root. No Apple runtime binaries are supplied by ipaSim.
IPASIM_API int ipaSim_setRuntimeRoot(const char *Path);

// Configure one immutable DwarFS image as the RuntimeRoot source. The reader
// bridge is supplied explicitly; ipaSim never guesses between a directory and
// an image and never falls back to extraction. Returns 0 on success or a
// non-zero value when the image/bridge cannot be opened or validated.
IPASIM_API int ipaSim_setDwarfsRuntimeRoot(const char *ImagePath,
                                           const char *ReaderBridgePath);

// Borrow the configured immutable RuntimeRoot source for read-only diagnostic
// preflights. Ownership remains with the emulator and the pointer is invalidated
// by the next RuntimeRoot configuration call.
IPASIM_API const ipasim::RuntimeRootStore *ipaSim_getRuntimeRootStore();

// Returns 0 when ipaSim's DynamicLoader successfully loads the supplied Mach-O
// image and its currently available dependencies. A non-zero result means the
// loader stopped before execution; no UIKit/SwiftUI application launch occurs.
IPASIM_API int ipaSim_probeImage(const char *Path);

// Load and execute a Mach-O through ipaSim's existing SysTranslator/Unicorn
// path. This is intended for deterministic synthetic compatibility fixtures;
// it does not claim UIKit/SwiftUI application launch support. On a normal guest
// return, ReturnValue receives AArch64 X0 so CI can prove the loaded image
// actually executed rather than merely parsed successfully.
IPASIM_API int ipaSim_executeImage(const char *Path, uint64_t *ReturnValue);

// Load a Mach-O, create an independent ARM64 execution context sharing the
// loaded guest-process pages, and execute the image entry point on a separate
// Windows thread. The call joins that worker only for deterministic validation;
// normal workqueue delivery uses the asynchronous threaded callback export.
IPASIM_API int ipaSim_executeImageThreaded(const char *Path,
                                           uint64_t *ReturnValue);

// Internal runtime C ABI used by the Darwin pthread semantic provider. These
// exports do not create application-specific behavior: they expose one fresh
// guest CPU context, its real stack identity, and a controlled non-local guest
// pthread exit while preserving C++ ownership of the Unicorn context.
IPASIM_API int ipaSim_runGuestPthread(void *FP, void *Arg0,
                                     void **ReturnValue);
IPASIM_API void *ipaSim_currentGuestStackTop();
IPASIM_API int ipaSim_isMainGuestContext();
IPASIM_API int ipaSim_requestGuestPthreadExit(void *ExitValue);

#endif // IPASIM_PROBE_HPP
