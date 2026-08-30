// DarwinTelemetryBridge.cpp: Darwin telemetry(2) syscall semantics for the
// Windows host bridge.
//
// XNU exposes __telemetry as a six-argument POSIX syscall wrapper. Commands 1
// (timer-event sampling) and 3 (PMI sampling) exist only when kernel telemetry
// is configured; XNU otherwise reports EINVAL. Windows has no equivalent XNU
// microstackshot/PMI facility, so ipaSim preserves that valid Darwin failure
// shape instead of claiming telemetry was armed.
//
// Command 2 sets the current thread's Mach voucher name. ipaSim can faithfully
// represent the null voucher (clear) now. Non-null Mach voucher names require a
// real voucher-right namespace, which is not implemented yet, so they fail
// explicitly with EINVAL rather than manufacturing a voucher relationship.

#include <cerrno>
#include <cstdint>
#include <limits>

namespace {

constexpr std::uint64_t TelemetryCmdTimerEvent = 1;
constexpr std::uint64_t TelemetryCmdVoucherName = 2;
constexpr std::uint64_t TelemetryCmdPmiSetup = 3;
constexpr std::uint32_t MachPortNull = 0;
constexpr std::uint32_t MachPortDead =
    (std::numeric_limits<std::uint32_t>::max)();

thread_local std::uint32_t CurrentVoucherName = MachPortNull;

int failInvalidArgument() {
  errno = EINVAL;
  return -1;
}

} // namespace

extern "C" __declspec(dllexport) int
__telemetry(std::uint64_t Command, std::uint64_t Deadline,
            std::uint64_t Interval, std::uint64_t Leeway,
            std::uint64_t Argument4, std::uint64_t Argument5) {
  // Keep the complete syscall ABI even for commands whose trailing arguments
  // are not interpreted. This matters because libsystem/libdispatch bind the
  // six-register entry point directly on arm64.
  (void)Interval;
  (void)Leeway;
  (void)Argument4;
  (void)Argument5;

  switch (Command) {
  case TelemetryCmdTimerEvent:
  case TelemetryCmdPmiSetup:
    // This is the same externally visible result XNU produces when
    // CONFIG_TELEMETRY does not provide these command handlers.
    return failInvalidArgument();

  case TelemetryCmdVoucherName: {
    if (Deadline > (std::numeric_limits<std::uint32_t>::max)())
      return failInvalidArgument();

    const std::uint32_t VoucherName = static_cast<std::uint32_t>(Deadline);
    if (VoucherName == MachPortDead)
      return failInvalidArgument();

    if (VoucherName != MachPortNull) {
      // A non-null name must resolve to a real Mach voucher right. ipaSim does
      // not yet have that namespace, so do not accept an unverifiable name.
      return failInvalidArgument();
    }

    CurrentVoucherName = MachPortNull;
    return 0;
  }

  default:
    return failInvalidArgument();
  }
}
