// DarwinPthreadTsdSmoke.cpp: executable export and semantic checks for the
// Darwin pthread TSD host bridge used by simulator libpthread/libdispatch.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using DarwinPthreadKey = std::uint64_t;
using GuestDestructor = void (*)(void *);

constexpr DarwinPthreadKey DynamicKeyStart = 256;
constexpr DarwinPthreadKey DynamicKeyEnd = 768;

int fail(const char *Message) {
  std::fprintf(stderr, "[darwin-pthread-tsd-smoke] FAIL: %s\n", Message);
  return 1;
}

FARPROC requireExport(HMODULE Module, const char *Name) {
  FARPROC Proc = GetProcAddress(Module, Name);
  if (!Proc)
    std::fprintf(stderr, "[darwin-pthread-tsd-smoke] missing export: %s\n",
                 Name);
  return Proc;
}

void destructorA(void *) {}
void destructorB(void *) {}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return fail("expected IpaSimDarwinHost.dll path");

  HMODULE Host = LoadLibraryA(argv[1]);
  if (!Host)
    return fail("could not load IpaSimDarwinHost.dll");

  using KeyInit = int (*)(std::int32_t, GuestDestructor);
  using KeyCreate = int (*)(DarwinPthreadKey *, GuestDestructor);
  using KeyDelete = int (*)(DarwinPthreadKey);
  using SetSpecific = int (*)(DarwinPthreadKey, const void *);
  using StaticSetter = int (*)(DarwinPthreadKey, void *);
  using GetSpecific = void *(*)(DarwinPthreadKey);

  auto InitStaticKey = reinterpret_cast<KeyInit>(
      requireExport(Host, "pthread_key_init_np"));
  auto CreateKey = reinterpret_cast<KeyCreate>(
      requireExport(Host, "pthread_key_create"));
  auto DeleteKey = reinterpret_cast<KeyDelete>(
      requireExport(Host, "pthread_key_delete"));
  auto Set = reinterpret_cast<SetSpecific>(
      requireExport(Host, "pthread_setspecific"));
  auto SetStatic = reinterpret_cast<StaticSetter>(
      requireExport(Host, "_pthread_setspecific_static"));
  auto Get = reinterpret_cast<GetSpecific>(
      requireExport(Host, "pthread_getspecific"));

  if (!InitStaticKey || !CreateKey || !DeleteKey || !Set || !SetStatic || !Get) {
    FreeLibrary(Host);
    return 1;
  }

  errno = EDOM;

  // pthread_key_init_np owns libpthread-managed static keys 10..255. Repeating
  // initialization is accepted and is first-writer-wins for the destructor.
  if (InitStaticKey(9, destructorA) != EINVAL ||
      InitStaticKey(10, destructorA) != 0 ||
      InitStaticKey(10, destructorB) != 0 ||
      InitStaticKey(255, nullptr) != 0 ||
      InitStaticKey(256, destructorA) != EINVAL || errno != EDOM) {
    FreeLibrary(Host);
    return fail("static key initialization range/return contract failed");
  }

  int MainStaticValue = 1;
  int OtherStaticValue = 2;
  if (SetStatic(20, &MainStaticValue) != 0 || Get(20) != &MainStaticValue ||
      SetStatic(255, &OtherStaticValue) != 0 || Get(255) != &OtherStaticValue ||
      SetStatic(256, &MainStaticValue) != EINVAL) {
    FreeLibrary(Host);
    return fail("static setter did not preserve the 256-key boundary");
  }

  // The simulator static SPI permits reserved direct slots 0..9, while the
  // ordinary pthread_setspecific API begins at libpthread-managed key 10.
  int ReservedValue = 3;
  if (SetStatic(0, &ReservedValue) != 0 || Get(0) != &ReservedValue ||
      Set(9, &ReservedValue) != EINVAL) {
    FreeLibrary(Host);
    return fail("reserved/static TSD distinction is incorrect");
  }

  // Ordinary setspecific lazily initializes managed static keys with a NULL
  // destructor, matching libpthread's static-key behavior.
  int LazyStaticValue = 4;
  if (Set(30, &LazyStaticValue) != 0 || Get(30) != &LazyStaticValue ||
      Set(DynamicKeyEnd, &LazyStaticValue) != EINVAL) {
    FreeLibrary(Host);
    return fail("ordinary static TSD set/get contract failed");
  }

  DarwinPthreadKey DynamicA = 0;
  DarwinPthreadKey DynamicB = 0;
  if (CreateKey(&DynamicA, destructorA) != 0 ||
      CreateKey(&DynamicB, nullptr) != 0 || DynamicA != DynamicKeyStart ||
      DynamicB != DynamicKeyStart + 1) {
    FreeLibrary(Host);
    return fail("dynamic key allocation did not begin at key 256");
  }

  int MainDynamicValue = 5;
  if (Set(DynamicA, &MainDynamicValue) != 0 || Get(DynamicA) != &MainDynamicValue) {
    FreeLibrary(Host);
    return fail("dynamic key set/get failed");
  }

  // TSD values are per-thread even though key registration is process-wide.
  int WorkerStaticValue = 6;
  int WorkerDynamicValue = 7;
  std::atomic<bool> WorkerReady{false};
  std::atomic<bool> KeyDeleted{false};
  std::atomic<bool> WorkerPassed{true};
  std::thread Worker([&] {
    if (Get(20) != nullptr || Get(DynamicA) != nullptr)
      WorkerPassed = false;
    if (SetStatic(20, &WorkerStaticValue) != 0 ||
        Set(DynamicA, &WorkerDynamicValue) != 0 ||
        Get(20) != &WorkerStaticValue || Get(DynamicA) != &WorkerDynamicValue)
      WorkerPassed = false;
    WorkerReady = true;
    while (!KeyDeleted.load())
      std::this_thread::yield();
    if (Get(DynamicA) != nullptr)
      WorkerPassed = false;
  });

  while (!WorkerReady.load())
    std::this_thread::yield();

  // Apple clears a deleted dynamic key from all live pthread TSD arrays.
  if (DeleteKey(DynamicA) != 0 || Get(DynamicA) != nullptr)
    WorkerPassed = false;
  KeyDeleted = true;
  Worker.join();

  if (!WorkerPassed.load() || Get(20) != &MainStaticValue ||
      Set(DynamicA, &MainDynamicValue) != EINVAL ||
      DeleteKey(DynamicA) != EINVAL) {
    FreeLibrary(Host);
    return fail("thread isolation or dynamic-key deletion semantics failed");
  }

  // Deleted dynamic keys are reusable, and exhaustion returns EAGAIN only
  // after all 512 simulator dynamic slots (256..767) are registered.
  DarwinPthreadKey Recycled = 0;
  if (CreateKey(&Recycled, destructorB) != 0 || Recycled != DynamicKeyStart) {
    FreeLibrary(Host);
    return fail("deleted dynamic key was not reused");
  }

  std::vector<DarwinPthreadKey> ExtraKeys;
  ExtraKeys.reserve(static_cast<std::size_t>(DynamicKeyEnd - DynamicKeyStart));
  for (;;) {
    DarwinPthreadKey Key = 0;
    const int Result = CreateKey(&Key, nullptr);
    if (Result == EAGAIN)
      break;
    if (Result != 0 || Key < DynamicKeyStart || Key >= DynamicKeyEnd) {
      FreeLibrary(Host);
      return fail("dynamic key allocator returned invalid key/result");
    }
    ExtraKeys.push_back(Key);
  }

  // DynamicB and Recycled are already live, so exactly 510 further keys fit.
  if (ExtraKeys.size() != 510) {
    FreeLibrary(Host);
    return fail("dynamic key capacity does not match simulator range");
  }

  if (DeleteKey(DynamicB) != 0 || DeleteKey(Recycled) != 0) {
    FreeLibrary(Host);
    return fail("dynamic key cleanup failed");
  }
  for (DarwinPthreadKey Key : ExtraKeys) {
    if (DeleteKey(Key) != 0) {
      FreeLibrary(Host);
      return fail("bulk dynamic key cleanup failed");
    }
  }

  if (errno != EDOM) {
    FreeLibrary(Host);
    return fail("pthread TSD bridge modified host errno");
  }

  std::printf(
      "[darwin-pthread-tsd-smoke] static/dynamic key and per-thread TSD semantics passed\n");
  FreeLibrary(Host);
  return 0;
}
