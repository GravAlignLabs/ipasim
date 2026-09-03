#pragma once

#include "ipasim/RuntimeRootStore.hpp"

#include <memory>
#include <string>

namespace ipasim {

// Creates an immutable RuntimeRoot byte source backed by a DwarFS image. The
// reader implementation lives behind a C ABI bridge DLL so DwarFS's C++23 ABI
// and dependency graph do not leak into the ipaSim core.
std::unique_ptr<RuntimeRootStore>
makeDwarfsRuntimeRootStore(const std::string &ImagePath,
                           const std::string &ReaderBridgePath,
                           std::string &Error);

} // namespace ipasim
