#pragma once

#include <string_view>

namespace ipasim {

// Apple ships the iOS Simulator libSystem kernel/platform/pthread layers as
// overlays over matching *_host.dylib companions. Preserve that public
// namespace relationship explicitly instead of flattening arbitrary dependency
// graphs.
inline constexpr bool
isSimulatorLibSystemHostReexport(std::string_view InstallName) {
  return InstallName ==
             "/usr/lib/system/libsystem_sim_kernel_host.dylib" ||
         InstallName ==
             "/usr/lib/system/libsystem_sim_platform_host.dylib" ||
         InstallName ==
             "/usr/lib/system/libsystem_sim_pthread_host.dylib";
}

} // namespace ipasim
