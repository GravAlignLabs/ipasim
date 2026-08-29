// MachIpc.hpp: Minimal real Mach message/port core for the Windows host bridge.
//
// This is not a success-returning compatibility stub. It implements an
// in-process Mach port namespace with receive rights, send rights, FIFO message
// queues, timeouts, overwrite receive buffers, and real Mach error codes for the
// subset currently implemented. Unsupported right-transfer/complex-message
// behavior is rejected explicitly so the next required semantic can be added
// without hiding it.

#pragma once

// Every native Mach bridge translation unit eventually includes windows.h.
// Keep its legacy min/max macros disabled so C++ standard-library calls remain
// usable in the semantic adapters rather than being macro-rewritten.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>

namespace ipasim::mach {

using KernelReturn = std::int32_t;
using MessageBits = std::uint32_t;
using MessageSize = std::uint32_t;
using MessageId = std::int32_t;
using MessageOption = std::uint32_t;
using MessageReturn = std::int32_t;
using MessageTimeout = std::uint32_t;
using PortName = std::uint32_t;

struct MessageHeader {
  MessageBits Bits;
  MessageSize Size;
  PortName RemotePort;
  PortName LocalPort;
  PortName VoucherPort;
  MessageId Id;
};
static_assert(sizeof(MessageHeader) == 24,
              "Darwin mach_msg_header_t must remain 24 bytes");

constexpr PortName PortNull = 0;

// XNU kern_return.h values used by target-proven task operations.
constexpr KernelReturn KernelSuccess = 0;
constexpr KernelReturn KernelInvalidArgument = 4;
constexpr KernelReturn KernelFailure = 5;

constexpr MessageBits MessageBitsRemoteMask = 0x0000001fU;
constexpr MessageBits MessageBitsLocalMask = 0x00001f00U;
constexpr MessageBits MessageBitsVoucherMask = 0x001f0000U;
constexpr MessageBits MessageBitsPortsMask =
    MessageBitsRemoteMask | MessageBitsLocalMask | MessageBitsVoucherMask;
constexpr MessageBits MessageBitsComplex = 0x80000000U;

constexpr std::uint32_t MessageTypeMoveReceive = 16;
constexpr std::uint32_t MessageTypeMoveSend = 17;
constexpr std::uint32_t MessageTypeMoveSendOnce = 18;
constexpr std::uint32_t MessageTypeCopySend = 19;
constexpr std::uint32_t MessageTypeMakeSend = 20;
constexpr std::uint32_t MessageTypeMakeSendOnce = 21;

// Prefix Mach option constants so Win32 APIs/macros such as SendMessage cannot
// collide with the Darwin ABI names when windows.h is present.
constexpr MessageOption MachSendMsg = 0x00000001U;
constexpr MessageOption MachReceiveMsg = 0x00000002U;
constexpr MessageOption MachReceiveLarge = 0x00000004U;
constexpr MessageOption MachSendTimeout = 0x00000010U;
constexpr MessageOption MachReceiveTimeout = 0x00000100U;

constexpr MessageReturn MessageSuccess = 0;
constexpr MessageReturn SendInvalidData = 0x10000002;
constexpr MessageReturn SendInvalidDestination = 0x10000003;
constexpr MessageReturn SendTimedOut = 0x10000004;
constexpr MessageReturn SendInvalidVoucher = 0x10000005;
constexpr MessageReturn SendMessageTooSmall = 0x10000008;
constexpr MessageReturn SendInvalidReply = 0x10000009;
constexpr MessageReturn SendInvalidRight = 0x1000000a;
constexpr MessageReturn SendInvalidOptions = 0x10000013;
constexpr MessageReturn ReceiveInvalidName = 0x10004002;
constexpr MessageReturn ReceiveTimedOut = 0x10004003;
constexpr MessageReturn ReceiveTooLarge = 0x10004004;
constexpr MessageReturn ReceiveInvalidData = 0x10004008;
constexpr MessageReturn ReceiveInvalidType = 0x1000400d;
constexpr MessageReturn ReceiveInvalidArguments = 0x10004013;

// Internal port-namespace primitives. They are intentionally not Darwin exports
// yet. Future mach_port_* host boundaries can delegate to these same operations
// when the target proves those APIs are required.
PortName allocateReceivePort();
bool insertSendRight(PortName Name);
bool deallocateReceiveRight(PortName Name);

// Darwin's mach_task_self_ is a cached send right to the current task's kernel
// port. Return one stable non-null name backed by this namespace. The receive
// right is kernel-owned and therefore cannot be deallocated or received from
// through the user-owned receive-right primitives above.
PortName taskSelfPort();

// Resolve a Mach task port to its BSD process identifier. The current task-self
// port is backed by the real Windows process id. Unknown/non-task ports return
// KERN_FAILURE and write -1, matching XNU pid_for_task's failure result shape.
KernelReturn pidForTask(PortName Task, std::int32_t *ProcessId);

// Implements the public mach_msg_overwrite contract for the currently supported
// inline-message subset. Timeout units are milliseconds, matching XNU.
MessageReturn messageOverwrite(MessageHeader *Message, MessageOption Option,
                               MessageSize SendSize, MessageSize ReceiveLimit,
                               PortName ReceiveName, MessageTimeout Timeout,
                               PortName Notify, MessageHeader *ReceiveMessageBuffer,
                               MessageSize ReceiveScatterSize);

} // namespace ipasim::mach
