// DarwinPthreadQosBridge.cpp: pure Darwin pthread_priority_t QoS codecs used
// by libdispatch and libpthread clients.
//
// Darwin LP64 typedefs pthread_priority_t and unsigned long as 64-bit values,
// while Windows uses LLP64 where unsigned long is only 32 bits. Keep the guest
// ABI explicit with fixed-width types and reproduce Apple's compact low-32-bit
// priority layout without mapping these values onto Windows scheduler state.

#include <cstdint>

namespace {

using DarwinPriority = std::uint64_t;
using DarwinFlags = std::uint64_t;
using QosClass = std::uint32_t;
using ThreadQos = std::uint8_t;

constexpr DarwinPriority PriorityFlagsMask = 0xff000000ULL;
constexpr DarwinPriority SchedPriMask = 0x0000ffffULL;
constexpr DarwinPriority ValidQosMask = 0x00003f00ULL;
constexpr unsigned QosShift = 8;
constexpr DarwinPriority ValidOverrideQosMask = 0x003fc000ULL;
constexpr unsigned OverrideQosShift = 14;
constexpr DarwinPriority RelativePriorityMask = 0x000000ffULL;

constexpr DarwinPriority SchedPriFlag = 0x20000000ULL;
constexpr DarwinPriority EventManagerFlag = 0x02000000ULL;
constexpr DarwinPriority OverrideQosFlag = 0x00800000ULL;

constexpr ThreadQos ThreadQosUnspecified = 0;
constexpr ThreadQos ThreadQosMaintenance = 1;
constexpr ThreadQos ThreadQosBackground = 2;
constexpr ThreadQos ThreadQosUtility = 3;
constexpr ThreadQos ThreadQosLegacy = 4;
constexpr ThreadQos ThreadQosUserInitiated = 5;
constexpr ThreadQos ThreadQosUserInteractive = 6;
constexpr ThreadQos ThreadQosLast = 7;

constexpr QosClass QosClassUnspecified = 0x00;
constexpr QosClass QosClassMaintenance = 0x05;
constexpr QosClass QosClassBackground = 0x09;
constexpr QosClass QosClassUtility = 0x11;
constexpr QosClass QosClassDefault = 0x15;
constexpr QosClass QosClassUserInitiated = 0x19;
constexpr QosClass QosClassUserInteractive = 0x21;

ThreadQos qosClassToThreadQos(QosClass Qos) {
  switch (Qos) {
  case QosClassUserInteractive:
    return ThreadQosUserInteractive;
  case QosClassUserInitiated:
    return ThreadQosUserInitiated;
  case QosClassDefault:
    return ThreadQosLegacy;
  case QosClassUtility:
    return ThreadQosUtility;
  case QosClassBackground:
    return ThreadQosBackground;
  case QosClassMaintenance:
    return ThreadQosMaintenance;
  default:
    return ThreadQosUnspecified;
  }
}

QosClass qosClassFromThreadQos(ThreadQos Qos) {
  switch (Qos) {
  case ThreadQosMaintenance:
    return QosClassMaintenance;
  case ThreadQosBackground:
    return QosClassBackground;
  case ThreadQosUtility:
    return QosClassUtility;
  case ThreadQosLegacy:
    return QosClassDefault;
  case ThreadQosUserInitiated:
    return QosClassUserInitiated;
  case ThreadQosUserInteractive:
    return QosClassUserInteractive;
  default:
    return QosClassUnspecified;
  }
}

ThreadQos firstSetQos(DarwinPriority Bits) {
  for (ThreadQos Index = 1; Index < ThreadQosLast; ++Index) {
    if (Bits & (DarwinPriority{1} << (Index - 1)))
      return Index;
  }
  return ThreadQosUnspecified;
}

bool priorityHasQos(DarwinPriority Priority) {
  return (Priority & (SchedPriFlag | EventManagerFlag)) == 0 &&
         (Priority & ValidQosMask) != 0;
}

bool priorityHasOverrideQos(DarwinPriority Priority) {
  return (Priority & (SchedPriFlag | EventManagerFlag)) == 0 &&
         (Priority & OverrideQosFlag) != 0 &&
         (Priority & ValidOverrideQosMask) != 0;
}

ThreadQos priorityThreadQos(DarwinPriority Priority) {
  if (!priorityHasQos(Priority))
    return ThreadQosUnspecified;
  return firstSetQos((Priority & ValidQosMask) >> QosShift);
}

ThreadQos priorityOverrideThreadQos(DarwinPriority Priority) {
  if (!priorityHasOverrideQos(Priority))
    return ThreadQosUnspecified;
  return firstSetQos((Priority & ValidOverrideQosMask) >> OverrideQosShift);
}

std::int32_t priorityRelativePriority(DarwinPriority Priority) {
  if (!priorityHasQos(Priority))
    return 0;
  const auto Encoded = static_cast<std::uint8_t>(Priority & RelativePriorityMask);
  return static_cast<std::int8_t>(Encoded) + 1;
}

DarwinPriority makePriority(ThreadQos Qos, std::int32_t RelativePriority,
                            DarwinFlags Flags) {
  std::uint32_t Compact = static_cast<std::uint32_t>(Flags & PriorityFlagsMask);
  if (Qos != ThreadQosUnspecified && Qos < ThreadQosLast) {
    Compact |= std::uint32_t{1} << (QosShift + Qos - 1);
    Compact |= (static_cast<std::uint8_t>(RelativePriority) - 1u) & 0xffu;
  }
  return Compact;
}

DarwinPriority makePriorityWithOverride(ThreadQos Requested,
                                        std::int32_t RelativePriority,
                                        ThreadQos Override,
                                        DarwinFlags Flags) {
  DarwinPriority Priority = makePriority(Requested, RelativePriority, Flags);
  if (Override != ThreadQosUnspecified && Override < ThreadQosLast) {
    Priority |= DarwinPriority{1} << (OverrideQosShift + Override - 1);
    Priority |= OverrideQosFlag;
  }
  return Priority;
}

DarwinPriority makeSchedPriority(std::int32_t SchedPriority, DarwinFlags Flags) {
  std::uint32_t Compact = static_cast<std::uint32_t>(Flags & PriorityFlagsMask);
  Compact |= static_cast<std::uint32_t>(SchedPriFlag);
  Compact |= static_cast<std::uint32_t>(SchedPriority);
  return Compact;
}

std::int32_t decodeSchedPriority(DarwinPriority Priority) {
  if ((Priority & SchedPriFlag) == 0)
    return 0;
  return static_cast<std::int32_t>(Priority & SchedPriMask);
}

} // namespace

extern "C" {

__declspec(dllexport) DarwinPriority
_pthread_qos_class_encode(QosClass Qos, std::int32_t RelativePriority,
                          DarwinFlags Flags) {
  return makePriority(qosClassToThreadQos(Qos), RelativePriority, Flags);
}

__declspec(dllexport) QosClass
_pthread_qos_class_decode(DarwinPriority Priority, std::int32_t *RelativePriority,
                          DarwinFlags *Flags) {
  if (RelativePriority)
    *RelativePriority = priorityRelativePriority(Priority);
  if (Flags)
    *Flags = Priority & PriorityFlagsMask;
  return qosClassFromThreadQos(priorityThreadQos(Priority));
}

__declspec(dllexport) DarwinPriority _pthread_qos_class_and_override_encode(
    QosClass Qos, std::int32_t RelativePriority, DarwinFlags Flags,
    QosClass OverrideQos) {
  return makePriorityWithOverride(qosClassToThreadQos(Qos), RelativePriority,
                                  qosClassToThreadQos(OverrideQos), Flags);
}

__declspec(dllexport) QosClass _pthread_qos_class_and_override_decode(
    DarwinPriority Priority, std::int32_t *RelativePriority, DarwinFlags *Flags,
    QosClass *OverrideQos) {
  if (RelativePriority)
    *RelativePriority = priorityRelativePriority(Priority);
  if (Flags)
    *Flags = Priority & PriorityFlagsMask;
  if (OverrideQos)
    *OverrideQos = qosClassFromThreadQos(priorityOverrideThreadQos(Priority));
  return qosClassFromThreadQos(priorityThreadQos(Priority));
}

__declspec(dllexport) DarwinPriority
_pthread_sched_pri_encode(std::int32_t SchedPriority, DarwinFlags Flags) {
  return makeSchedPriority(SchedPriority, Flags);
}

__declspec(dllexport) std::int32_t
_pthread_sched_pri_decode(DarwinPriority Priority, DarwinFlags *Flags) {
  if (Flags)
    *Flags = Priority & (PriorityFlagsMask & ~SchedPriFlag);
  return decodeSchedPriority(Priority);
}

} // extern "C"
