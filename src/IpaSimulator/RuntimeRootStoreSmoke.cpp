#include "ipasim/RuntimeRootStore.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <windows.h>

namespace {

int fail(const char *Message) {
  std::fprintf(stderr, "[runtime-root-store-smoke] FAIL: %s\n", Message);
  return 1;
}

bool writeBytes(const std::filesystem::path &Path,
                const std::vector<std::uint8_t> &Data) {
  std::filesystem::create_directories(Path.parent_path());
  std::ofstream Output(Path, std::ios::binary);
  if (!Output)
    return false;
  if (!Data.empty())
    Output.write(reinterpret_cast<const char *>(Data.data()),
                 static_cast<std::streamsize>(Data.size()));
  return static_cast<bool>(Output);
}

} // namespace

int main() {
  std::printf("[runtime-root-store-smoke] begin\n");

  wchar_t TempPath[MAX_PATH];
  const DWORD TempLength = GetTempPathW(MAX_PATH, TempPath);
  if (TempLength == 0 || TempLength >= MAX_PATH)
    return fail("GetTempPathW failed");

  const std::filesystem::path Base =
      std::filesystem::path(TempPath) /
      (L"ipasim-runtime-root-store-" + std::to_wstring(GetCurrentProcessId()));
  const std::filesystem::path RootA = Base / L"root-a";
  const std::filesystem::path RootB = Base / L"root-b";
  std::error_code EC;
  std::filesystem::remove_all(Base, EC);

  const std::vector<std::uint8_t> Expected{0xcf, 0xfa, 0xed, 0xfe, 0x42, 0x00};
  const std::filesystem::path Relative =
      std::filesystem::path(L"System") / L"Library" / L"Test.dylib";
  if (!writeBytes(RootA / Relative, Expected) ||
      !writeBytes(RootB / Relative, Expected)) {
    std::filesystem::remove_all(Base, EC);
    return fail("could not create fixture");
  }

  std::string Error;
  auto StoreA = ipasim::makeDirectoryRuntimeRootStore(RootA.string(), Error);
  if (!StoreA) {
    std::fprintf(stderr, "[runtime-root-store-smoke] store A error: %s\n",
                 Error.c_str());
    std::filesystem::remove_all(Base, EC);
    return 1;
  }
  auto StoreB = ipasim::makeDirectoryRuntimeRootStore(RootB.string(), Error);
  if (!StoreB) {
    std::fprintf(stderr, "[runtime-root-store-smoke] store B error: %s\n",
                 Error.c_str());
    std::filesystem::remove_all(Base, EC);
    return 1;
  }

  constexpr const char *DarwinPath = "/System/Library/Test.dylib";
  const std::string IdentityA = StoreA->identity(DarwinPath);
  const std::string IdentityB = StoreB->identity(DarwinPath);
  if (IdentityA.empty() || IdentityB.empty() || IdentityA == IdentityB) {
    std::filesystem::remove_all(Base, EC);
    return fail("store identities do not distinguish RuntimeRoot sources");
  }

  std::vector<std::uint8_t> Data;
  if (!StoreA->readFile(DarwinPath, Data, Error) || Data != Expected) {
    std::fprintf(stderr, "[runtime-root-store-smoke] read error: %s\n",
                 Error.c_str());
    std::filesystem::remove_all(Base, EC);
    return fail("absolute Darwin path did not return exact bytes");
  }

  Data.assign(1, 0xff);
  if (StoreA->readFile("System/Library/Test.dylib", Data, Error) ||
      !Data.empty() || Error.empty()) {
    std::filesystem::remove_all(Base, EC);
    return fail("relative Darwin path was not rejected explicitly");
  }

  if (StoreA->readFile("/System/Library/Missing.dylib", Data, Error) ||
      Error.empty()) {
    std::filesystem::remove_all(Base, EC);
    return fail("missing RuntimeRoot file was not reported");
  }

  std::filesystem::remove_all(Base, EC);
  std::printf("[runtime-root-store-smoke] passed\n");
  return 0;
}
