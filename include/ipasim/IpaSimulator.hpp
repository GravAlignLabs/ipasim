// IpaSimulator.hpp: Definition of class `IpaSimulator` and declarations of
// functions that are part of `IpaSimLibrary`'s public API.

#ifndef IPASIM_IPA_SIMULATOR_HPP
#define IPASIM_IPA_SIMULATOR_HPP

#include "ipasim/Common.hpp"
#include "ipasim/RuntimeLog.hpp"
#include "ipasim/DynamicLoader.hpp"
#include "ipasim/Emulator.hpp"
#include "ipasim/SysTranslator.hpp"

#include <string>
#include <unicorn/unicorn.h>

#if !defined(IPASIM_MODERN_CORE)
#include "ipasim/TextBlockStream.hpp"
#include <winrt/Windows.ApplicationModel.Activation.h>
#endif

namespace ipasim {

class IpaSimulator {
public:
  IpaSimulator();

  Emulator Emu;
  DynamicLoader Dyld;
  std::string MainBinary;
  SysTranslator Sys;
#if !defined(IPASIM_MODERN_CORE)
  TextBlockProvider LogText;
#endif
};

#if !defined(IPASIM_MODERN_CORE)
// Starts the historical WinObjC application shell. The modern core deliberately
// does not expose this UI boundary until its replacement is implemented.
IPASIM_EXPORT void start(
    const winrt::hstring &Path,
    const winrt::Windows::ApplicationModel::Activation::LaunchActivatedEventArgs
        &LaunchArgs);
IPASIM_EXPORT TextBlockProvider &logText();
#endif

// TODO: This is just a workaround, because MSVC cannot compile `Log.error`
// calls.
IPASIM_EXPORT void error(const char *Message);

extern IpaSimulator IpaSim;

} // namespace ipasim

// !defined(IPASIM_IPA_SIMULATOR_HPP)
#endif
