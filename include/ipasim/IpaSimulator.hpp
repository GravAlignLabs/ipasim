// IpaSimulator.hpp: Definition of class `IpaSimulator` and declarations of
// functions that are part of `IpaSimLibrary`'s public API.

#ifndef IPASIM_IPA_SIMULATOR_HPP
#define IPASIM_IPA_SIMULATOR_HPP

#include "ipasim/Common.hpp"
#include "ipasim/RuntimeLog.hpp"
#include "ipasim/DynamicLoader.hpp"
#include "ipasim/Emulator.hpp"
#include "ipasim/SysTranslator.hpp"

#include <memory>
#include <string>
#include <unicorn/unicorn.h>

#if !defined(IPASIM_MODERN_CORE)
#include "ipasim/TextBlockStream.hpp"
#include <winrt/Windows.ApplicationModel.Activation.h>
#endif

namespace ipasim {

struct GuestExecutionContext {
  std::unique_ptr<Emulator> Emu;
  std::unique_ptr<SysTranslator> Sys;
};

class IpaSimulator {
public:
  IpaSimulator();

  // Create another ARM64 CPU context inside the current guest process. Loaded
  // images and process data remain shared through identical host-backed pages;
  // registers, execution state and SP are independent.
  std::unique_ptr<GuestExecutionContext> createExecutionContext();

  Emulator Emu;
  DynamicLoader Dyld;
  std::string MainBinary;
  SysTranslator Sys;
#if !defined(IPASIM_MODERN_CORE)
  TextBlockProvider LogText;
#endif
};

#if !defined(IPASIM_MODERN_CORE)
IPASIM_EXPORT void start(
    const winrt::hstring &Path,
    const winrt::Windows::ApplicationModel::Activation::LaunchActivatedEventArgs
        &LaunchArgs);
IPASIM_EXPORT TextBlockProvider &logText();
#endif

IPASIM_EXPORT void error(const char *Message);

extern IpaSimulator IpaSim;

} // namespace ipasim

#endif
