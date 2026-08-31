// DarwinPthreadQosSmoke.cpp: semantic/export checks for Darwin
// pthread_priority_t QoS codecs, current-thread property updates, and direct
// explicit override lifecycle.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>

namespace {

using DarwinPriority = std::uint64_t;
using DarwinFlags = std::uint64_t;
using SetFlags = std::uint32_t;
using QosClass = std::uint32_t;
using MachPort = std::uint32_t;

constexpr QosClass QosUnspecified = 0x00;
constexpr QosClass QosMaintenance = 0x05;
constexpr QosClass QosBackground = 0x09;
constexpr QosClass QosUtility = 0x11;
constexpr QosClass QosDefault = 0x15;
constexpr QosClass QosUserInitiated = 0x19;
constexpr QosClass QosUserInteractive = 0x21;

constexpr DarwinFlags OvercommitFlag = 0x80000000ULL;
constexpr DarwinFlags InheritFlag = 0x40000000ULL;
constexpr DarwinFlags SchedPriFlag = 0x20000000ULL;
constexpr DarwinFlags CooperativeFlag = 0x08000000ULL;
constexpr DarwinFlags EventManagerFlag = 0x02000000ULL;
constexpr DarwinFlags NeedsUnbindFlag = 0x01000000ULL;

constexpr SetFlags SetSelfQosFlag = 0x01u;
constexpr SetFlags SetSelfVoucherFlag = 0x02u;
constexpr SetFlags SetSelfFixedPriorityFlag = 0x04u;
constexpr SetFlags SetSelfTimeshareFlag = 0x08u;
constexpr SetFlags SetSelfWorkqueueKeventUnbindFlag = 0x10u;
constexpr SetFlags SetSelfAlternateClusterFlag = 0x20u;
constexpr SetFlags SetSelfQosOverrideFlag = 0x40u;
constexpr int DarwinErrnoBadMessage = 94;
constexpr MachPort MachPortDead = 0xffffffffu;

int fail(const char *Message) {
  std::fprintf(stderr, "[darwin-pthread-qos-smoke] FAIL: %s\n", Message);
  return 1;
}

FARPROC requireExport(HMODULE Module, const char *Name) {
  FARPROC Proc = GetProcAddress(Module, Name);
  if (!Proc)
    std::fprintf(stderr, "[darwin-pthread-qos-smoke] missing export: %s\n", Name);
  return Proc;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return fail("expected IpaSimDarwinHost.dll path");

  HMODULE Host = LoadLibraryA(argv[1]);
  if (!Host)
    return fail("could not load IpaSimDarwinHost.dll");

  using Encode = DarwinPriority (*)(QosClass, std::int32_t, DarwinFlags);
  using Decode = QosClass (*)(DarwinPriority, std::int32_t *, DarwinFlags *);
  using OverrideEncode =
      DarwinPriority (*)(QosClass, std::int32_t, DarwinFlags, QosClass);
  using OverrideDecode = QosClass (*)(DarwinPriority, std::int32_t *,
                                      DarwinFlags *, QosClass *);
  using SchedEncode = DarwinPriority (*)(std::int32_t, DarwinFlags);
  using SchedDecode = std::int32_t (*)(DarwinPriority, DarwinFlags *);
  using SetProperties = int (*)(SetFlags, DarwinPriority, MachPort);
  using DirectStart = int (*)(MachPort, DarwinPriority, void *);
  using DirectEnd = int (*)(MachPort, void *);
  using LegacyStart = int (*)(MachPort, DarwinPriority);
  using LegacyEnd = int (*)(MachPort);

  auto QosEncode = reinterpret_cast<Encode>(
      requireExport(Host, "_pthread_qos_class_encode"));
  auto QosDecode = reinterpret_cast<Decode>(
      requireExport(Host, "_pthread_qos_class_decode"));
  auto QosOverrideEncode = reinterpret_cast<OverrideEncode>(
      requireExport(Host, "_pthread_qos_class_and_override_encode"));
  auto QosOverrideDecode = reinterpret_cast<OverrideDecode>(
      requireExport(Host, "_pthread_qos_class_and_override_decode"));
  auto PriorityEncode = reinterpret_cast<SchedEncode>(
      requireExport(Host, "_pthread_sched_pri_encode"));
  auto PriorityDecode = reinterpret_cast<SchedDecode>(
      requireExport(Host, "_pthread_sched_pri_decode"));
  auto SetSelfProperties = reinterpret_cast<SetProperties>(
      requireExport(Host, "_pthread_set_properties_self"));
  auto StartOverride = reinterpret_cast<DirectStart>(
      requireExport(Host, "_pthread_qos_override_start_direct"));
  auto EndOverride = reinterpret_cast<DirectEnd>(
      requireExport(Host, "_pthread_qos_override_end_direct"));
  auto StartLegacyOverride = reinterpret_cast<LegacyStart>(
      requireExport(Host, "_pthread_override_qos_class_start_direct"));
  auto EndLegacyOverride = reinterpret_cast<LegacyEnd>(
      requireExport(Host, "_pthread_override_qos_class_end_direct"));
  if (!QosEncode || !QosDecode || !QosOverrideEncode || !QosOverrideDecode ||
      !PriorityEncode || !PriorityDecode || !SetSelfProperties ||
      !StartOverride || !EndOverride || !StartLegacyOverride ||
      !EndLegacyOverride) {
    FreeLibrary(Host);
    return 1;
  }

  // Apple's compact encoding uses one QoS bit plus an int8 relative-priority
  // field where relpri 0 becomes 0xff and decodes back through int8_t + 1.
  const DarwinPriority DefaultPriority = QosEncode(QosDefault, 0, 0);
  if (DefaultPriority != 0x000008ffULL) {
    FreeLibrary(Host);
    return fail("default QoS encoding does not match Darwin layout");
  }

  std::int32_t Relative = 123;
  DarwinFlags Flags = ~DarwinFlags{0};
  if (QosDecode(DefaultPriority, &Relative, &Flags) != QosDefault ||
      Relative != 0 || Flags != 0) {
    FreeLibrary(Host);
    return fail("default QoS decode did not round-trip");
  }

  const DarwinPriority Interactive =
      QosEncode(QosUserInteractive, -5, OvercommitFlag);
  if (Interactive != 0x800020faULL) {
    FreeLibrary(Host);
    return fail("interactive QoS encoding does not match Darwin layout");
  }
  Relative = 123;
  Flags = 0;
  if (QosDecode(Interactive, &Relative, &Flags) != QosUserInteractive ||
      Relative != -5 || Flags != OvercommitFlag) {
    FreeLibrary(Host);
    return fail("interactive QoS decode did not preserve relpri/flags");
  }

  const DarwinPriority Utility = QosEncode(QosUtility, -15, 0);
  if (Utility != 0x000004f0ULL) {
    FreeLibrary(Host);
    return fail("minimum relative-priority encoding is incorrect");
  }
  Relative = 0;
  if (QosDecode(Utility, &Relative, nullptr) != QosUtility || Relative != -15) {
    FreeLibrary(Host);
    return fail("minimum relative priority did not decode correctly");
  }

  // Darwin pthread_priority_t is unsigned long on LP64. Upper bits therefore
  // arrive in the ABI even though Apple's current compact layout uses low 32.
  const DarwinPriority WideCarrier = Interactive | 0x1234000000000000ULL;
  Relative = 0;
  Flags = 0;
  if (QosDecode(WideCarrier, &Relative, &Flags) != QosUserInteractive ||
      Relative != -5 || Flags != OvercommitFlag) {
    FreeLibrary(Host);
    return fail("LP64 pthread_priority_t carrier was truncated or misdecoded");
  }

  if (QosDecode(Interactive, nullptr, nullptr) != QosUserInteractive) {
    FreeLibrary(Host);
    return fail("QoS decode rejected null optional outputs");
  }

  Relative = 99;
  Flags = 0;
  if (QosDecode(EventManagerFlag, &Relative, &Flags) != QosUnspecified ||
      Relative != 0 || Flags != EventManagerFlag) {
    FreeLibrary(Host);
    return fail("event-manager priority was incorrectly decoded as QoS");
  }

  const QosClass Classes[] = {QosMaintenance, QosBackground, QosUtility,
                              QosDefault, QosUserInitiated,
                              QosUserInteractive};
  for (QosClass Qos : Classes) {
    const DarwinPriority Encoded = QosEncode(Qos, 0, InheritFlag);
    Relative = 99;
    Flags = 0;
    if (QosDecode(Encoded, &Relative, &Flags) != Qos || Relative != 0 ||
        Flags != InheritFlag) {
      FreeLibrary(Host);
      return fail("QoS class round-trip mapping failed");
    }
  }

  if (QosEncode(0xffffffffu, -7, OvercommitFlag) != OvercommitFlag) {
    FreeLibrary(Host);
    return fail("unknown QoS class did not encode as unspecified");
  }

  const DarwinPriority Override = QosOverrideEncode(
      QosUtility, -3, InheritFlag, QosUserInteractive);
  if (Override != 0x408804fcULL) {
    FreeLibrary(Host);
    return fail("QoS override encoding does not match Darwin layout");
  }
  QosClass OverrideClass = QosUnspecified;
  Relative = 99;
  Flags = 0;
  if (QosOverrideDecode(Override, &Relative, &Flags, &OverrideClass) !=
          QosUtility ||
      Relative != -3 || Flags != InheritFlag ||
      OverrideClass != QosUserInteractive) {
    FreeLibrary(Host);
    return fail("QoS override decode did not round-trip");
  }
  if (QosOverrideDecode(Override, nullptr, nullptr, nullptr) != QosUtility) {
    FreeLibrary(Host);
    return fail("QoS override decode rejected null optional outputs");
  }

  const DarwinPriority Sched =
      PriorityEncode(42, OvercommitFlag | InheritFlag);
  if (Sched != 0xe000002aULL) {
    FreeLibrary(Host);
    return fail("scheduler-priority encoding does not match Darwin layout");
  }
  Flags = 0;
  if (PriorityDecode(Sched, &Flags) != 42 ||
      Flags != (OvercommitFlag | InheritFlag)) {
    FreeLibrary(Host);
    return fail("scheduler-priority decode did not strip only its marker bit");
  }
  Flags = 0;
  if (PriorityDecode(Interactive, &Flags) != 0 || Flags != OvercommitFlag) {
    FreeLibrary(Host);
    return fail("non-scheduler priority was misdecoded as scheduler priority");
  }

  // _pthread_set_properties_self returns pthread-style errno values directly.
  // It must not store those values in the Windows CRT errno slot.
  const DarwinPriority SelfUtility = QosEncode(QosUtility, -3, OvercommitFlag);
  errno = EDOM;
  if (SetSelfProperties(SetSelfQosFlag, SelfUtility, 0) != 0 || errno != EDOM) {
    FreeLibrary(Host);
    return fail("valid self QoS update failed or modified host errno");
  }
  if (SetSelfProperties(SetSelfQosFlag, 0, 0) != EINVAL || errno != EDOM) {
    FreeLibrary(Host);
    return fail("invalid self QoS did not return EINVAL directly");
  }

  const DarwinPriority CooperativeOverride = QosOverrideEncode(
      QosUtility, -3, CooperativeFlag, QosUserInitiated);
  if (SetSelfProperties(SetSelfQosOverrideFlag, CooperativeOverride, 0) !=
      EINVAL) {
    FreeLibrary(Host);
    return fail("QoS override flag was accepted without QoS flag");
  }
  const DarwinPriority NonCooperativeOverride = QosOverrideEncode(
      QosUtility, -3, 0, QosUserInitiated);
  if (SetSelfProperties(SetSelfQosFlag | SetSelfQosOverrideFlag,
                        NonCooperativeOverride, 0) != EINVAL) {
    FreeLibrary(Host);
    return fail("non-cooperative QoS override was accepted");
  }
  if (SetSelfProperties(SetSelfQosFlag | SetSelfQosOverrideFlag,
                        CooperativeOverride, 0) != 0) {
    FreeLibrary(Host);
    return fail("cooperative QoS override transition failed");
  }
  if (SetSelfProperties(SetSelfQosFlag, CooperativeOverride, 0) != EINVAL) {
    FreeLibrary(Host);
    return fail("override-encoded priority was accepted without override flag");
  }

  if (SetSelfProperties(SetSelfVoucherFlag, 0, 0) != 0 ||
      SetSelfProperties(SetSelfVoucherFlag, 0, 0x1234u) != 0) {
    FreeLibrary(Host);
    return fail("voucher clear/set transition failed");
  }
  if (SetSelfProperties(SetSelfVoucherFlag, 0, MachPortDead) != ENOENT) {
    FreeLibrary(Host);
    return fail("dead voucher name did not return ENOENT");
  }
  if (SetSelfProperties(SetSelfQosFlag | SetSelfVoucherFlag, SelfUtility,
                        MachPortDead) != ENOENT) {
    FreeLibrary(Host);
    return fail("voucher-only failure did not preserve Darwin return shape");
  }
  if (SetSelfProperties(SetSelfQosFlag | SetSelfVoucherFlag, 0,
                        MachPortDead) != DarwinErrnoBadMessage) {
    FreeLibrary(Host);
    return fail("combined QoS/voucher failure did not return Darwin EBADMSG");
  }

  const DarwinPriority NeedsUnbind =
      QosEncode(QosUtility, -3, NeedsUnbindFlag | OvercommitFlag);
  if (SetSelfProperties(SetSelfQosFlag | SetSelfWorkqueueKeventUnbindFlag,
                        NeedsUnbind, 0) != 0) {
    FreeLibrary(Host);
    return fail("workqueue unbind QoS transition failed");
  }
  if (SetSelfProperties(SetSelfFixedPriorityFlag | SetSelfTimeshareFlag, 0, 0) !=
          0 ||
      SetSelfProperties(SetSelfTimeshareFlag, 0, 0) != 0 ||
      SetSelfProperties(SetSelfAlternateClusterFlag, 0, 0) != 0) {
    FreeLibrary(Host);
    return fail("non-QoS current-thread property transition failed");
  }
  if (SetSelfProperties(0x80u, 0, 0) != EINVAL || errno != EDOM) {
    FreeLibrary(Host);
    return fail("unknown set-self flag was accepted or modified host errno");
  }

  // XNU explicit QoS overrides are keyed by target Mach thread and resource.
  // Repeated starts for one resource are reference-counted; removal of a
  // missing/underflowed resource reports EFAULT.
  constexpr MachPort Thread = 0x1234;
  int ResourceA = 0;
  int ResourceB = 0;
  const DarwinPriority DirectPriority = QosEncode(QosUtility, -3, 0);
  const DarwinPriority HigherPriority = QosEncode(QosUserInteractive, 0, 0);
  if (StartOverride(Thread, DirectPriority, &ResourceA) != 0 ||
      StartOverride(Thread, HigherPriority, &ResourceA) != 0) {
    FreeLibrary(Host);
    return fail("direct QoS override did not accept paired repeated starts");
  }
  if (EndOverride(Thread, &ResourceB) != EFAULT) {
    FreeLibrary(Host);
    return fail("direct QoS override did not reject a mismatched resource");
  }
  if (EndOverride(Thread, &ResourceA) != 0 ||
      EndOverride(Thread, &ResourceA) != 0) {
    FreeLibrary(Host);
    return fail("direct QoS override reference count did not drain cleanly");
  }
  if (EndOverride(Thread, &ResourceA) != EFAULT) {
    FreeLibrary(Host);
    return fail("direct QoS override underflow did not report EFAULT");
  }
  if (StartOverride(0, DirectPriority, &ResourceA) != ESRCH ||
      EndOverride(0, &ResourceA) != ESRCH) {
    FreeLibrary(Host);
    return fail("null Mach thread target did not report ESRCH");
  }
  if (StartOverride(Thread, Sched, &ResourceA) != EINVAL ||
      StartOverride(Thread, EventManagerFlag, &ResourceA) != EINVAL) {
    FreeLibrary(Host);
    return fail("non-QoS priority was accepted as an explicit QoS override");
  }

  // The deprecated wrappers use an implicit per-calling-thread resource. Their
  // start/end pair must therefore have the same lifecycle and underflow shape.
  if (StartLegacyOverride(Thread, DirectPriority) != 0 ||
      EndLegacyOverride(Thread) != 0 ||
      EndLegacyOverride(Thread) != EFAULT) {
    FreeLibrary(Host);
    return fail("legacy direct override wrapper did not preserve pairing");
  }

  std::printf(
      "[darwin-pthread-qos-smoke] codec/set-self/override semantics passed\n");
  FreeLibrary(Host);
  return 0;
}
