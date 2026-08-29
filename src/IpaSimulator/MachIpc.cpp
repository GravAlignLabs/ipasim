// MachIpc.cpp: In-process Mach port/message core for ipaSim's Windows host.

#include "MachIpc.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <process.h>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace ipasim::mach {
namespace {

constexpr std::size_t DefaultQueueLimit = 5;
constexpr MessageSize MaximumInlineMessageSize = 16U * 1024U * 1024U;
constexpr MessageOption SupportedOptions = MachSendMsg | MachReceiveMsg |
                                           MachReceiveLarge | MachSendTimeout |
                                           MachReceiveTimeout;

struct Port {
  std::mutex Mutex;
  std::condition_variable MessageAvailable;
  std::condition_variable SpaceAvailable;
  std::deque<std::vector<std::uint8_t>> Queue;
  std::uint32_t SendRights = 0;
  bool ReceiveRight = true;
  bool UserOwnsReceiveRight = true;
  // A non-negative value marks a task port and preserves the task -> BSD
  // process identity required by pid_for_task. Ordinary message ports remain -1.
  std::int32_t ProcessId = -1;
};

struct PortNamespace {
  std::mutex Mutex;
  std::unordered_map<PortName, std::shared_ptr<Port>> Ports;
  PortName NextName = 0x100;
};

PortNamespace &portNamespace() {
  static PortNamespace Namespace;
  return Namespace;
}

void diagnostic(const char *Message) {
  std::fprintf(stderr, "[mach-ipc] %s\n", Message);
  std::fflush(stderr);

  char Debug[512] = {};
  std::snprintf(Debug, sizeof(Debug), "[mach-ipc] %s\n", Message);
  OutputDebugStringA(Debug);
}

std::shared_ptr<Port> findPort(PortName Name) {
  PortNamespace &Namespace = portNamespace();
  std::lock_guard<std::mutex> Guard(Namespace.Mutex);
  const auto It = Namespace.Ports.find(Name);
  return It == Namespace.Ports.end() ? nullptr : It->second;
}

PortName allocatePort(bool UserOwnsReceiveRight,
                      std::uint32_t InitialSendRights,
                      std::int32_t ProcessId = -1) {
  PortNamespace &Namespace = portNamespace();
  std::lock_guard<std::mutex> Guard(Namespace.Mutex);

  for (;;) {
    const PortName Candidate = Namespace.NextName++;
    if (Candidate == PortNull || Namespace.Ports.count(Candidate) != 0)
      continue;

    auto NewPort = std::make_shared<Port>();
    NewPort->SendRights = InitialSendRights;
    NewPort->UserOwnsReceiveRight = UserOwnsReceiveRight;
    NewPort->ProcessId = ProcessId;
    Namespace.Ports.emplace(Candidate, std::move(NewPort));
    return Candidate;
  }
}

MessageReturn sendInline(MessageHeader *Message, MessageOption Option,
                         MessageSize SendSize, MessageTimeout Timeout,
                         PortName Notify) {
  if (!Message) {
    diagnostic("send requested with a null message buffer");
    return SendInvalidData;
  }
  if (SendSize < sizeof(MessageHeader)) {
    diagnostic("send buffer is smaller than mach_msg_header_t");
    return SendMessageTooSmall;
  }
  if (SendSize > MaximumInlineMessageSize) {
    diagnostic("inline send exceeds ipaSim Mach IPC safety limit");
    return SendInvalidData;
  }
  if (Message->Size != SendSize) {
    diagnostic("mach_msg_header_t.msgh_size does not match send_size");
    return SendInvalidData;
  }
  if ((Message->Bits & MessageBitsComplex) != 0) {
    diagnostic("complex Mach messages/descriptors are not implemented yet");
    return SendInvalidData;
  }
  if (Message->LocalPort != PortNull) {
    diagnostic("Mach reply-right transfer is not implemented yet");
    return SendInvalidReply;
  }
  if (Message->VoucherPort != PortNull) {
    diagnostic("Mach voucher transfer is not implemented yet");
    return SendInvalidVoucher;
  }
  if (Message->RemotePort == PortNull) {
    return SendInvalidDestination;
  }

  if (Notify != PortNull) {
    // On current LP64 libsyscall the historical `notify` argument is interpreted
    // as mach_msg_priority_t for sends. ipaSim does not model Mach QoS/priority
    // scheduling yet, so do not silently downgrade a prioritized send to FIFO.
    diagnostic("non-zero Mach send priority is not implemented yet");
    return SendInvalidOptions;
  }

  const std::uint32_t RemoteDisposition =
      Message->Bits & MessageBitsRemoteMask;
  if (RemoteDisposition != MessageTypeCopySend &&
      RemoteDisposition != MessageTypeMoveSend &&
      RemoteDisposition != MessageTypeMakeSend) {
    diagnostic("unsupported destination right disposition in Mach send");
    return SendInvalidRight;
  }

  std::shared_ptr<Port> Destination = findPort(Message->RemotePort);
  if (!Destination)
    return SendInvalidDestination;

  std::unique_lock<std::mutex> Lock(Destination->Mutex);
  if (!Destination->ReceiveRight)
    return SendInvalidDestination;

  if (RemoteDisposition == MessageTypeCopySend ||
      RemoteDisposition == MessageTypeMoveSend) {
    if (Destination->SendRights == 0)
      return SendInvalidRight;
  } else if (RemoteDisposition == MessageTypeMakeSend &&
             !Destination->ReceiveRight) {
    return SendInvalidRight;
  }

  const auto HasSpace = [&]() {
    return Destination->Queue.size() < DefaultQueueLimit ||
           !Destination->ReceiveRight;
  };

  if (!HasSpace()) {
    if ((Option & MachSendTimeout) != 0) {
      if (Timeout == 0)
        return SendTimedOut;
      if (!Destination->SpaceAvailable.wait_for(
              Lock, std::chrono::milliseconds(Timeout), HasSpace))
        return SendTimedOut;
    } else {
      Destination->SpaceAvailable.wait(Lock, HasSpace);
    }
  }

  if (!Destination->ReceiveRight)
    return SendInvalidDestination;

  std::vector<std::uint8_t> Bytes(SendSize);
  std::memcpy(Bytes.data(), Message, SendSize);
  Destination->Queue.push_back(std::move(Bytes));

  if (RemoteDisposition == MessageTypeMoveSend)
    --Destination->SendRights;

  Lock.unlock();
  Destination->MessageAvailable.notify_one();
  return MessageSuccess;
}

MessageReturn receiveInline(MessageHeader *Message,
                            MessageHeader *ReceiveMessageBuffer,
                            MessageOption Option, MessageSize ReceiveLimit,
                            PortName ReceiveName, MessageTimeout Timeout,
                            MessageSize ReceiveScatterSize) {
  if (ReceiveName == PortNull)
    return ReceiveInvalidName;
  if (ReceiveScatterSize != 0) {
    diagnostic("Mach scatter receive descriptors are not implemented yet");
    return ReceiveInvalidType;
  }

  MessageHeader *Output = ReceiveMessageBuffer ? ReceiveMessageBuffer : Message;
  if (!Output) {
    diagnostic("receive requested without a receive buffer");
    return ReceiveInvalidData;
  }
  if (ReceiveLimit < sizeof(MessageHeader))
    return ReceiveInvalidData;

  std::shared_ptr<Port> Source = findPort(ReceiveName);
  if (!Source)
    return ReceiveInvalidName;

  std::unique_lock<std::mutex> Lock(Source->Mutex);
  if (!Source->ReceiveRight || !Source->UserOwnsReceiveRight)
    return ReceiveInvalidName;

  const auto HasMessage = [&]() {
    return !Source->Queue.empty() || !Source->ReceiveRight;
  };

  if (!HasMessage()) {
    if ((Option & MachReceiveTimeout) != 0) {
      if (Timeout == 0)
        return ReceiveTimedOut;
      if (!Source->MessageAvailable.wait_for(
              Lock, std::chrono::milliseconds(Timeout), HasMessage))
        return ReceiveTimedOut;
    } else {
      Source->MessageAvailable.wait(Lock, HasMessage);
    }
  }

  if (!Source->ReceiveRight)
    return ReceiveInvalidName;
  if (Source->Queue.empty())
    return ReceiveTimedOut;

  const std::vector<std::uint8_t> &Bytes = Source->Queue.front();
  if (Bytes.size() > ReceiveLimit) {
    if ((Option & MachReceiveLarge) != 0) {
      std::memset(Output, 0, sizeof(MessageHeader));
      Output->Size = static_cast<MessageSize>(Bytes.size());
      Output->LocalPort = ReceiveName;
    }
    return ReceiveTooLarge;
  }

  std::memcpy(Output, Bytes.data(), Bytes.size());
  Source->Queue.pop_front();

  // This first real subset deliberately permits only one-way inline messages,
  // so there are no transferred reply/voucher rights to expose on receive.
  // Preserve all non-port header bits while making the receive port explicit.
  Output->Bits &= ~MessageBitsPortsMask;
  Output->RemotePort = PortNull;
  Output->LocalPort = ReceiveName;
  Output->VoucherPort = PortNull;

  Lock.unlock();
  Source->SpaceAvailable.notify_one();
  return MessageSuccess;
}

} // namespace

PortName allocateReceivePort() { return allocatePort(true, 0); }

bool insertSendRight(PortName Name) {
  std::shared_ptr<Port> Target = findPort(Name);
  if (!Target)
    return false;
  std::lock_guard<std::mutex> Guard(Target->Mutex);
  if (!Target->ReceiveRight)
    return false;
  ++Target->SendRights;
  return true;
}

bool deallocateReceiveRight(PortName Name) {
  std::shared_ptr<Port> Target = findPort(Name);
  if (!Target)
    return false;

  {
    std::lock_guard<std::mutex> Guard(Target->Mutex);
    if (!Target->ReceiveRight || !Target->UserOwnsReceiveRight)
      return false;
    Target->ReceiveRight = false;
  }
  Target->MessageAvailable.notify_all();
  Target->SpaceAvailable.notify_all();
  return true;
}

PortName taskSelfPort() {
  // XNU/libsyscall exposes mach_task_self_ as a cached send right. Model that
  // with one stable port-name entry whose receive right is owned by the
  // emulated kernel side, not by the user process. Preserve the real process id
  // on that port so pid_for_task observes the same task/process relationship.
  static const PortName TaskSelf = allocatePort(false, 1, _getpid());
  return TaskSelf;
}

KernelReturn pidForTask(PortName Task, std::int32_t *ProcessId) {
  if (!ProcessId)
    return KernelInvalidArgument;

  // XNU initializes the returned pid to -1 and copies that value out even when
  // the task lookup fails. Preserve that failure shape rather than leaving the
  // caller's output untouched.
  *ProcessId = -1;

  std::shared_ptr<Port> Target = findPort(Task);
  if (!Target)
    return KernelFailure;

  std::lock_guard<std::mutex> Guard(Target->Mutex);
  if (!Target->ReceiveRight || Target->ProcessId < 0)
    return KernelFailure;

  *ProcessId = Target->ProcessId;
  return KernelSuccess;
}

MessageReturn messageOverwrite(MessageHeader *Message, MessageOption Option,
                               MessageSize SendSize, MessageSize ReceiveLimit,
                               PortName ReceiveName, MessageTimeout Timeout,
                               PortName Notify,
                               MessageHeader *ReceiveMessageBuffer,
                               MessageSize ReceiveScatterSize) {
  const bool DoSend = (Option & MachSendMsg) != 0;
  const bool DoReceive = (Option & MachReceiveMsg) != 0;

  if (!DoSend && !DoReceive) {
    diagnostic("mach_msg_overwrite called without MACH_SEND_MSG or MACH_RCV_MSG");
    return SendInvalidOptions;
  }

  const MessageOption UnknownOptions = Option & ~SupportedOptions;
  if (UnknownOptions != 0) {
    diagnostic("mach_msg_overwrite received unsupported option bits");
    return DoSend ? SendInvalidOptions : ReceiveInvalidArguments;
  }

  if (DoSend) {
    const MessageReturn SendResult =
        sendInline(Message, Option, SendSize, Timeout, Notify);
    if (SendResult != MessageSuccess)
      return SendResult;
  }

  if (DoReceive) {
    return receiveInline(Message, ReceiveMessageBuffer, Option, ReceiveLimit,
                         ReceiveName, Timeout, ReceiveScatterSize);
  }

  return MessageSuccess;
}

} // namespace ipasim::mach
