// DarwinPthreadQosSmoke.cpp: semantic/export checks for Darwin's pure
// pthread_priority_t QoS codecs used by libdispatch.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace {

using DarwinPriority = std::uint64_t;
using DarwinFlags = std::uint64_t;
using QosClass = std::uint32_t;

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
constexpr DarwinFlags EventManagerFlag = 0x02000000ULL;

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
  if (!QosEncode || !QosDecode || !QosOverrideEncode || !QosOverrideDecode ||
      !PriorityEncode || !PriorityDecode) {
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
  // Prove the bridge accepts the 64-bit carrier and ignores unrelated high bits.
  const DarwinPriority WideCarrier = Interactive | 0x1234000000000000ULL;
  Relative = 0;
  Flags = 0;
  if (QosDecode(WideCarrier, &Relative, &Flags) != QosUserInteractive ||
      Relative != -5 || Flags != OvercommitFlag) {
    FreeLibrary(Host);
    return fail("LP64 pthread_priority_t carrier was truncated or misdecoded");
  }

  // Optional output pointers are part of the SPI contract.
  if (QosDecode(Interactive, nullptr, nullptr) != QosUserInteractive) {
    FreeLibrary(Host);
    return fail("QoS decode rejected null optional outputs");
  }

  // Event-manager/scheduler encodings intentionally do not report a QoS class.
  Relative = 99;
  Flags = 0;
  if (QosDecode(EventManagerFlag, &Relative, &Flags) != QosUnspecified ||
      Relative != 0 || Flags != EventManagerFlag) {
    FreeLibrary(Host);
    return fail("event-manager priority was incorrectly decoded as QoS");
  }

  // Cover every class mapping, including the private maintenance class.
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

  // Unknown classes map to THREAD_QOS_UNSPECIFIED exactly as libpthread does.
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

  std::printf("[darwin-pthread-qos-smoke] priority codec semantics passed\n");
  FreeLibrary(Host);
  return 0;
}
