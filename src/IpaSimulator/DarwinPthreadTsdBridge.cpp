// DarwinPthreadTsdBridge.cpp: Darwin pthread thread-specific-data semantics for
// the iOS Simulator pthread host boundary.
//
// Apple divides the simulator pthread TSD namespace into static keys below 256
// and dynamic keys 256..767. Static keys are assigned to libSystem/frameworks
// and may be initialized with pthread_key_init_np; dynamic keys are allocated by
// pthread_key_create. Values remain per-thread while key registration is
// process-wide. Keep that guest contract explicit rather than mapping Darwin
// pthread_key_t onto Windows TLS indices, whose allocation/range semantics are
// different.
//
// Destructor pointers are guest ARM64 function addresses. ipaSim records their
// registration/lifetime exactly enough for key validity and replacement rules,
// but does not call them from native Windows thread teardown: guest-aware
// pthread exit/destructor execution is a separate runtime boundary.

#include <array>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace {

using DarwinPthreadKey = std::uint64_t; // pthread_key_t is unsigned long on LP64.
using GuestDestructor = void (*)(void *);

constexpr DarwinPthreadKey FirstManagedStaticKey = 10;
constexpr DarwinPthreadKey DynamicKeyStart = 256;
constexpr DarwinPthreadKey DynamicKeyEnd = 768; // exclusive
constexpr std::size_t TsdSlotCount = static_cast<std::size_t>(DynamicKeyEnd);

struct KeyState {
  bool Registered = false;
  std::uintptr_t Destructor = 0;
};

struct ThreadState {
  std::array<void *, TsdSlotCount> Values{};
};

struct Registry {
  std::mutex Mutex;
  std::array<KeyState, TsdSlotCount> Keys{};
  std::unordered_set<ThreadState *> Threads;
};

Registry &registry() {
  // Intentionally process-lifetime storage. The host bridge is unloadable, so
  // ordinary C++ static/TLS destructors must not retain callbacks into code that
  // Windows may already have unmapped during DLL teardown.
  static Registry *State = new Registry();
  return *State;
}

ThreadState &currentThreadState() {
  // Guest pthread teardown is not implemented yet, so keep each native backing
  // state alive for the process lifetime rather than installing a C++
  // thread_local destructor in the unloadable DLL. This also prevents Windows
  // thread-id reuse from exposing stale guest values to a later thread.
  thread_local ThreadState *State = nullptr;
  if (!State) {
    State = new ThreadState();
    Registry &R = registry();
    std::lock_guard<std::mutex> Guard(R.Mutex);
    R.Threads.insert(State);
  }
  return *State;
}

bool slotInRange(DarwinPthreadKey Key) { return Key < DynamicKeyEnd; }

bool dynamicKey(DarwinPthreadKey Key) {
  return Key >= DynamicKeyStart && Key < DynamicKeyEnd;
}

int keyInitStatic(std::int32_t Key, GuestDestructor Destructor) {
  if (Key < static_cast<std::int32_t>(FirstManagedStaticKey) ||
      Key >= static_cast<std::int32_t>(DynamicKeyStart))
    return EINVAL;

  Registry &R = registry();
  std::lock_guard<std::mutex> Guard(R.Mutex);
  KeyState &Entry = R.Keys[static_cast<std::size_t>(Key)];

  // Apple's _pthread_key_set_destructor() is first-writer-wins, and
  // pthread_key_init_np() intentionally ignores the false/already-set result.
  if (!Entry.Registered) {
    Entry.Registered = true;
    Entry.Destructor = reinterpret_cast<std::uintptr_t>(Destructor);
  }
  return 0;
}

int keyCreate(DarwinPthreadKey *Key, GuestDestructor Destructor) {
  if (!Key)
    return EINVAL;

  Registry &R = registry();
  std::lock_guard<std::mutex> Guard(R.Mutex);
  for (DarwinPthreadKey Candidate = DynamicKeyStart;
       Candidate < DynamicKeyEnd; ++Candidate) {
    KeyState &Entry = R.Keys[static_cast<std::size_t>(Candidate)];
    if (Entry.Registered)
      continue;

    Entry.Registered = true;
    Entry.Destructor = reinterpret_cast<std::uintptr_t>(Destructor);
    *Key = Candidate;
    return 0;
  }
  return EAGAIN;
}

int keyDelete(DarwinPthreadKey Key) {
  if (!dynamicKey(Key))
    return EINVAL;

  Registry &R = registry();
  std::lock_guard<std::mutex> Guard(R.Mutex);
  KeyState &Entry = R.Keys[static_cast<std::size_t>(Key)];
  if (!Entry.Registered)
    return EINVAL;

  Entry = KeyState{};
  for (ThreadState *Thread : R.Threads)
    Thread->Values[static_cast<std::size_t>(Key)] = nullptr;
  return 0;
}

int setSpecific(DarwinPthreadKey Key, const void *Value) {
  if (Key < FirstManagedStaticKey || !slotInRange(Key))
    return EINVAL;

  Registry &R = registry();
  ThreadState &Thread = currentThreadState();
  std::lock_guard<std::mutex> Guard(R.Mutex);

  KeyState &Entry = R.Keys[static_cast<std::size_t>(Key)];
  if (dynamicKey(Key)) {
    if (!Entry.Registered)
      return EINVAL;
  } else if (!Entry.Registered) {
    // pthread_setspecific() lazily marks a managed static key as initialized
    // with a NULL destructor when no explicit pthread_key_init_np occurred.
    Entry.Registered = true;
    Entry.Destructor = 0;
  }

  Thread.Values[static_cast<std::size_t>(Key)] = const_cast<void *>(Value);
  return 0;
}

int setSpecificStatic(DarwinPthreadKey Key, void *Value) {
  // Apple's simulator SPI checks only that the key is on the static side of the
  // boundary, then performs a direct TSD write. Reserved libsyscall/libplatform
  // slots 0..9 therefore remain valid for this SPI even though ordinary
  // pthread_setspecific() starts at the first libpthread-managed static key.
  if (Key >= DynamicKeyStart)
    return EINVAL;

  Registry &R = registry();
  ThreadState &Thread = currentThreadState();
  std::lock_guard<std::mutex> Guard(R.Mutex);
  Thread.Values[static_cast<std::size_t>(Key)] = Value;
  return 0;
}

void *getSpecific(DarwinPthreadKey Key) {
  if (!slotInRange(Key))
    return nullptr;

  Registry &R = registry();
  ThreadState &Thread = currentThreadState();
  std::lock_guard<std::mutex> Guard(R.Mutex);
  return Thread.Values[static_cast<std::size_t>(Key)];
}

} // namespace

extern "C" {

__declspec(dllexport) int
pthread_key_init_np(std::int32_t Key, GuestDestructor Destructor) {
  return keyInitStatic(Key, Destructor);
}

__declspec(dllexport) int
pthread_key_create(DarwinPthreadKey *Key, GuestDestructor Destructor) {
  return keyCreate(Key, Destructor);
}

__declspec(dllexport) int pthread_key_delete(DarwinPthreadKey Key) {
  return keyDelete(Key);
}

__declspec(dllexport) int
pthread_setspecific(DarwinPthreadKey Key, const void *Value) {
  return setSpecific(Key, Value);
}

__declspec(dllexport) int
_pthread_setspecific_static(DarwinPthreadKey Key, void *Value) {
  return setSpecificStatic(Key, Value);
}

__declspec(dllexport) void *pthread_getspecific(DarwinPthreadKey Key) {
  return getSpecific(Key);
}

} // extern "C"
