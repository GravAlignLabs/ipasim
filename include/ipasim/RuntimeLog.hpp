// RuntimeLog.hpp: Select the logging sink for the active ipaSim runtime.
// The modern console/core build is deliberately independent of XAML; the
// historical application build retains its TextBlock sink.

#ifndef IPASIM_RUNTIME_LOG_HPP
#define IPASIM_RUNTIME_LOG_HPP

#if defined(IPASIM_MODERN_CORE)
#include "ipasim/Logger.hpp"

namespace ipasim {
using LogStream = AggregateStream<DebugStream, StdStream>;
using RuntimeLogger = Logger<LogStream>;
extern RuntimeLogger Log;
} // namespace ipasim

#else
#include "ipasim/TextBlockStream.hpp"

namespace ipasim {
using RuntimeLogger = Logger<LogStream>;
extern RuntimeLogger Log;
} // namespace ipasim
#endif

#endif // IPASIM_RUNTIME_LOG_HPP
