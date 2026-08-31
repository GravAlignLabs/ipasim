// Probe.hpp: Minimal C ABI for exercising ipaSim's real loader from a console
// test host without entering the incomplete modern SwiftUI launch boundary.

#ifndef IPASIM_PROBE_HPP
#define IPASIM_PROBE_HPP

#include "ipasim/Common.hpp"

#include <cstdint>

// Configure a filesystem root that mirrors iOS absolute install names. Passing
// an empty string clears the root. Returns 0 on success or a non-zero value for
// an invalid root. No Apple runtime binaries are supplied by ipaSim.
IPASIM_API int ipaSim_setRuntimeRoot(const char *Path);

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

#endif // IPASIM_PROBE_HPP
