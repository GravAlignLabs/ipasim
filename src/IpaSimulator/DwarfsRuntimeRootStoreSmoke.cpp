#include "ipasim/DwarfsRuntimeRootStore.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int fail(const std::string &Message) {
  std::cerr << "[dwarfs-runtime-root-store-smoke] FAIL: " << Message << '\n';
  return 1;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3)
    return fail("usage: DwarfsRuntimeRootStoreSmoke <reader-bridge.dll> "
                "<fixture.dwarfs>");

  constexpr const char *DarwinOnlyPath =
      "/System/Library/Frameworks/UIKit.framework/Versions/A:/UIKit";
  const std::vector<uint8_t> Expected = {
      0x49, 0x50, 0x41, 0x53, 0x69, 0x6d, 0x00, 0xff, 0x2f, 0x3a, 0x7f};

  std::string Error;
  auto Store = ipasim::makeDwarfsRuntimeRootStore(argv[2], argv[1], Error);
  if (!Store)
    return fail("factory rejected fixture: " + Error);

  const std::string Identity = Store->identity(DarwinOnlyPath);
  if (Identity.empty() || Identity.rfind("dwarfs:", 0) != 0)
    return fail("image-backed source identity is missing or unstable");

  std::vector<uint8_t> Data;
  if (!Store->readFile(DarwinOnlyPath, Data, Error))
    return fail("cannot read Darwin-only internal pathname: " + Error);
  if (Data != Expected)
    return fail("DwarFS reader returned bytes different from fixture payload");

  if (Store->readFile("System/Library/relative", Data, Error))
    return fail("relative Darwin path was accepted");
  if (Error.empty())
    return fail("relative Darwin path rejection did not include a diagnostic");

  if (Store->readFile("/System/Library/does-not-exist", Data, Error))
    return fail("missing DwarFS entry was reported as readable");
  if (Error.empty())
    return fail("missing DwarFS entry did not include a diagnostic");

  std::cout << "[dwarfs-runtime-root-store-smoke] passed\n";
  return 0;
}
