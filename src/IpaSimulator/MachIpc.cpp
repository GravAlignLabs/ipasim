// MachIpc.cpp: In-process Mach port/message core for ipaSim's Windows host bridge.

#include "MachIpc.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <process.h>
#include <unordered_map>
#include <utility>
#include <vector>
#include <windows.h>

namespace ipasim::mach {
namespace {

constexpr std::size_t DefaultQueueLimit = 5;
constexpr MessageSize MaximumInlineMessageSize = 16U * 1024U * 1024U;
constexpr MessageOption SupportedOptions = MachSendMsg | MachReceiveMsg |
                                           MachReceiveLarge | MachSendTimeout |
                                           MachReceiveTimeout;

// Public mach_voucher_types.h defines a packed 16-byte recipe prefix followed
// by content_size raw bytes. Keep the parser here, in the Mach object namespace,
// so later extract/deallocate/transport boundaries can reuse the same voucher
// identity instead of inventing another registry.
#pragma pack(push, 1)
struct VoucherRecipeHeader {
  std::uint32_t Key;
  std::uint32_t Command;
  PortName PreviousVoucher;
  std::uint32_t ContentSize;
};
#pragma pack(pop)
static_assert(sizeof(VoucherRecipeHeader) == 16,
              "Darwin mach_voucher_attr_recipe_data_t prefix must be 16 bytes");

constexpr std::uint32_t VoucherKeyAll = 0xffffffffU;
constexpr std::uint32_t VoucherKeyAtm = 1;
constexpr std::uint32_t VoucherKeyImportance = 2;
constexpr std::uint32_t VoucherKeyBank = 3;
constexpr std::uint32_t VoucherKeyPthreadPriority = 4;
constexpr std::uint32_t VoucherKeyUserData = 7;
constexpr std::uint32_t VoucherKeyTest = 8;
constexpr std::uint32_t VoucherCommandCopy = 1;
constexpr std::uint32_t VoucherCommandRemove = 2;
constexpr std::uint32_t MaximumRawVoucherRecipeSize = 5120;

using VoucherAttributes =
    std::unordered_map<std::uint32_t, std::vector<std::uint8_t>>;

enum class PortKind {
  Message,
  Task,
  Host,
  Voucher,
};

struct Port {
  std::mutex Mutex;
  std::condition_variable MessageAvailable;
  std::condition_variable SpaceAvailable;
  std::deque<std::vector<std::uint8_t>> Queue;
  std::uint32_t SendRights = 0;
  bool ReceiveRight = true;
  bool UserOwnsReceiveRight = true;
  PortKind Kind = PortKind::Message;
  // A non-negative value marks a task port and preserves the task -> BSD
  // process identity required by pid_for_task. Ordinary objects remain -1.
  std::int32_t ProcessId = -1;
  // Voucher attributes are immutable after publication. The first supported
  // subset is intentionally manager-independent: COPY and REMOVE can preserve
  // existing attribute state without pretending to implement an XNU resource
  // manager. Manager-specific construction remains fail-closed.
  VoucherAttributes Attributes;
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
                      std::int32_t ProcessId = -1,
                      PortKind Kind = PortKind::Message,
                      VoucherAttributes Attributes = {}) {
  PortNamespace &Namespace = portNamespace();
  std::lock_guard<std::mutex> Guard(Namespace.Mutex);

  for (;;) {
    const PortName Candidate = Namespace.NextName++;
    if (Candidate == PortNull || Namespace.Ports.count(Candidate) != 0)
      continue;

    auto NewPort = std::make_shared<Port>();
    NewPort->SendRights = InitialSendRights;
    NewPort->UserOwnsReceiveRight = UserOwnsReceiveRight;
    NewPort->Kind = Kind;
    NewPort->ProcessId = ProcessId;
    NewPort->Attributes = std::move(Attributes);
    Namespace.Ports.emplace(Candidate, std::move(NewPort));
    return Candidate;
  }
}

bool isLivePortOfKind(PortName Name, PortKind Kind) {
  std::shared_ptr<Port> Target = findPort(Name);
  if (!Target)
    return false;
  std::lock_guard<std::mutex> Guard(Target->Mutex);
  return Target->ReceiveRight && Target->SendRights != 0 &&
         Target->Kind == Kind;
}

KernelReturn snapshotVoucher(PortName Name, VoucherAttributes &Attributes) {
  Attributes.clear();
  if (Name == PortNull)
    return KernelSuccess;

  std::shared_ptr<Port> Target = findPort(Name);
  if (!Target)
    return KernelInvalidCapability;

  std::lock_guard<std::mutex> Guard(Target->Mutex);
  if (!Target->ReceiveRight || Target->SendRights == 0 ||
      Target->Kind != PortKind::Voucher)
    return KernelInvalidCapability;
  Attributes = Target->Attributes;
  return KernelSuccess;
}

bool isKnownVoucherKey(std::uint32_t Key) {
  switch (Key) {
  case VoucherKeyAll:
  case VoucherKeyAtm:
  case VoucherKeyImportance:
  case VoucherKeyBank:
  case VoucherKeyPthreadPriority:
  case VoucherKeyUserData:
  case VoucherKeyTest:
    return true;
  default:
    return false;
  }
}

KernelReturn applyVoucherRecipes(const std::uint8_t *Recipes,
                                 std::uint32_t RecipeSize,
                                 VoucherAttributes &Attributes) {
  std::size_t Used = 0;
  while (Used < RecipeSize) {
    const std::size_t Remaining = static_cast<std::size_t>(RecipeSize) - Used;
    if (Remaining < sizeof(VoucherRecipeHeader))
      return KernelInvalidArgument;

    VoucherRecipeHeader Recipe{};
    std::memcpy(&Recipe, Recipes + Used, sizeof(Recipe));
    if (Recipe.ContentSize > Remaining - sizeof(VoucherRecipeHeader))
      return KernelInvalidArgument;
    if (!isKnownVoucherKey(Recipe.Key))
      return KernelInvalidArgument;

    VoucherAttributes Previous;
    const KernelReturn PreviousResult =
        snapshotVoucher(Recipe.PreviousVoucher, Previous);
    if (PreviousResult != KernelSuccess)
      return PreviousResult;

    switch (Recipe.Command) {
    case VoucherCommandCopy:
      if (Recipe.ContentSize != 0)
        return KernelInvalidArgument;
      // XNU treats a null previous voucher as an empty source and leaves the
      // forming voucher unchanged for COPY.
      if (Recipe.PreviousVoucher != PortNull) {
        if (Recipe.Key == VoucherKeyAll) {
          Attributes = Previous;
        } else {
          const auto It = Previous.find(Recipe.Key);
          if (It == Previous.end())
            Attributes.erase(Recipe.Key);
          else
            Attributes[Recipe.Key] = It->second;
        }
      }
      break;

    case VoucherCommandRemove:
      if (Recipe.ContentSize != 0)
        return KernelInvalidArgument;
      if (Recipe.Key == VoucherKeyAll) {
        if (Recipe.PreviousVoucher == PortNull) {
          Attributes.clear();
        } else {
          for (auto It = Attributes.begin(); It != Attributes.end();) {
            const auto PreviousIt = Previous.find(It->first);
            if (PreviousIt != Previous.end() && PreviousIt->second == It->second)
              It = Attributes.erase(It);
            else
              ++It;
          }
        }
      } else if (Recipe.PreviousVoucher == PortNull) {
        Attributes.erase(Recipe.Key);
      } else {
        const auto CurrentIt = Attributes.find(Recipe.Key);
        const auto PreviousIt = Previous.find(Recipe.Key);
        if (CurrentIt != Attributes.end() && PreviousIt != Previous.end() &&
            CurrentIt->second == PreviousIt->second)
          Attributes.erase(CurrentIt);
      }
      break;

    default:
      // AUTO_REDEEM, manager-specific BANK/IMPORTANCE/ATM commands, direct
      // value-handle installation, and arbitrary manager commands require the
      // corresponding XNU attribute-manager semantics. Do not manufacture an
      // opaque attribute and report success: that would make subsequent
      // voucher extraction/importance/accounting behavior observably false.
      diagnostic("Mach voucher recipe requires an unsupported attribute manager");
      return KernelNotSupported;
    }

    Used += sizeof(VoucherRecipeHeader) + Recipe.ContentSize;
  }
  return KernelSuccess;
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
  static const PortName TaskSelf =
      allocatePort(false, 1, _getpid(), PortKind::Task);
  return TaskSelf;
}

PortName hostSelfPort() {
  // mach_host_self() names a kernel host object, not a task or ordinary message
  // receive right. Keep one stable send right in the same namespace so host_t
  // consumers can validate capability type instead of accepting arbitrary names.
  static const PortName HostSelf =
      allocatePort(false, 1, -1, PortKind::Host);
  return HostSelf;
}

KernelReturn createVoucher(PortName Host, const void *Recipes,
                           std::uint32_t RecipeSize, PortName *Voucher) {
  if (!Voucher)
    return KernelInvalidArgument;
  *Voucher = PortNull;

  if (!isLivePortOfKind(Host, PortKind::Host))
    return KernelInvalidHost;
  if (RecipeSize > MaximumRawVoucherRecipeSize)
    return KernelInvalidArgument;

  // XNU's ipc_create_mach_voucher returns a null voucher with KERN_SUCCESS for
  // an empty recipe list. Preserve that observable special case exactly.
  if (RecipeSize == 0)
    return KernelSuccess;
  if (!Recipes)
    return KernelInvalidArgument;

  try {
    VoucherAttributes Attributes;
    const auto *Bytes = static_cast<const std::uint8_t *>(Recipes);
    const KernelReturn Result = applyVoucherRecipes(Bytes, RecipeSize, Attributes);
    if (Result != KernelSuccess)
      return Result;

    *Voucher = allocatePort(false, 1, -1, PortKind::Voucher,
                            std::move(Attributes));
    return *Voucher == PortNull ? KernelResourceShortage : KernelSuccess;
  } catch (const std::bad_alloc &) {
    return KernelResourceShortage;
  }
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
  if (!Target->ReceiveRight || Target->Kind != PortKind::Task ||
      Target->ProcessId < 0)
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
