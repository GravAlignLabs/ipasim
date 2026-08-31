#include "ipasim/Probe.hpp"

#include <cstdint>
#include <cstdio>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr,
                 "[threaded-guest-callback-smoke] expected extracted ARM64 Mach-O path\n");
    return 64;
  }

  // The synthetic HelloBootstrap entry point returns 42. Requiring that same
  // value through ipaSim_executeImageThreaded proves the image was loaded once,
  // its host-backed guest mappings were replayed into a second Unicorn engine,
  // and ARM64 guest instructions executed on a separate Windows thread with an
  // independent register/stack context.
  std::uint64_t ReturnValue = 0;
  const int Result = ipaSim_executeImageThreaded(argv[1], &ReturnValue);
  if (Result != 0) {
    std::fprintf(stderr,
                 "[threaded-guest-callback-smoke] threaded execution stopped with code %d\n",
                 Result);
    return Result;
  }

  if (ReturnValue != 42) {
    std::fprintf(stderr,
                 "[threaded-guest-callback-smoke] expected X0=42, got %llu\n",
                 static_cast<unsigned long long>(ReturnValue));
    return 73;
  }

  std::printf(
      "[threaded-guest-callback-smoke] independent ARM64 guest context returned X0=42\n");
  return 0;
}
