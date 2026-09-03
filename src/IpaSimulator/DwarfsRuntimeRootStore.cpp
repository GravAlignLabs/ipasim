#include "ipasim/DwarfsRuntimeRootStore.hpp"
#include "ipasim/DwarfsReaderBridgeApi.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ipasim {
namespace {

bool validDarwinPath(std::string_view DarwinPath) {
  return !DarwinPath.empty() && DarwinPath.front() == '/' &&
         DarwinPath.find('\0') == std::string_view::npos;
}

std::string wideToUtf8(const std::wstring &Value) {
  if (Value.empty())
    return {};

  const int Required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                            Value.data(),
                                            static_cast<int>(Value.size()),
                                            nullptr, 0, nullptr, nullptr);
  if (Required <= 0)
    return {};

  std::string Result(static_cast<size_t>(Required), '\0');
  const int Written = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(),
      static_cast<int>(Value.size()), Result.data(), Required, nullptr, nullptr);
  if (Written != Required)
    return {};
  return Result;
}

template <typename T>
T loadFunction(HMODULE Module, const char *Name) {
  return reinterpret_cast<T>(GetProcAddress(Module, Name));
}

class DwarfsRuntimeRootStore final : public RuntimeRootStore {
public:
  static std::unique_ptr<DwarfsRuntimeRootStore>
  create(const std::string &ImagePath, const std::string &ReaderBridgePath,
         std::string &Error) {
    Error.clear();

    try {
      const std::filesystem::path Image =
          std::filesystem::absolute(std::filesystem::u8path(ImagePath))
              .lexically_normal();
      const std::filesystem::path Bridge =
          std::filesystem::absolute(std::filesystem::u8path(ReaderBridgePath))
              .lexically_normal();

      if (!std::filesystem::is_regular_file(Image)) {
        Error = "DwarFS image is unavailable: " + ImagePath;
        return nullptr;
      }
      if (!std::filesystem::is_regular_file(Bridge)) {
        Error = "DwarFS reader bridge is unavailable: " + ReaderBridgePath;
        return nullptr;
      }

      HMODULE Module = LoadLibraryW(Bridge.c_str());
      if (Module == nullptr) {
        Error = "cannot load DwarFS reader bridge (Win32 error " +
                std::to_string(GetLastError()) + "): " + ReaderBridgePath;
        return nullptr;
      }

      const auto Abi =
          loadFunction<IpaSimDwarfsReaderAbiFn>(Module,
                                                "ipasim_dwarfs_reader_abi");
      const auto Open = loadFunction<IpaSimDwarfsReaderOpenFn>(
          Module, "ipasim_dwarfs_reader_open");
      const auto Read = loadFunction<IpaSimDwarfsReaderReadFn>(
          Module, "ipasim_dwarfs_reader_read");
      const auto Free = loadFunction<IpaSimDwarfsReaderFreeFn>(
          Module, "ipasim_dwarfs_reader_free");
      const auto Close = loadFunction<IpaSimDwarfsReaderCloseFn>(
          Module, "ipasim_dwarfs_reader_close");

      if (Abi == nullptr || Open == nullptr || Read == nullptr ||
          Free == nullptr || Close == nullptr) {
        FreeLibrary(Module);
        Error = "DwarFS reader bridge is missing required ABI exports";
        return nullptr;
      }
      if (Abi() != IPASIM_DWARFS_READER_ABI_VERSION) {
        const uint32_t Actual = Abi();
        FreeLibrary(Module);
        Error = "DwarFS reader bridge ABI mismatch: expected " +
                std::to_string(IPASIM_DWARFS_READER_ABI_VERSION) + ", got " +
                std::to_string(Actual);
        return nullptr;
      }

      std::array<char, 1024> BridgeError{};
      IpaSimDwarfsReaderHandle Handle =
          Open(Image.c_str(), BridgeError.data(), BridgeError.size());
      if (Handle == nullptr) {
        FreeLibrary(Module);
        Error = BridgeError[0] != '\0'
                    ? std::string("cannot open DwarFS image: ") +
                          BridgeError.data()
                    : "cannot open DwarFS image";
        return nullptr;
      }

      std::string ImageIdentity = wideToUtf8(Image.native());
      if (ImageIdentity.empty())
        ImageIdentity = ImagePath;

      return std::unique_ptr<DwarfsRuntimeRootStore>(new DwarfsRuntimeRootStore(
          Module, Handle, Open, Read, Free, Close, std::move(ImageIdentity)));
    } catch (const std::exception &Ex) {
      Error = std::string("cannot initialize DwarFS RuntimeRoot store: ") +
              Ex.what();
      return nullptr;
    }
  }

  ~DwarfsRuntimeRootStore() override {
    if (Handle != nullptr && Close != nullptr)
      Close(Handle);
    if (Module != nullptr)
      FreeLibrary(Module);
  }

  std::string identity(std::string_view DarwinPath) const override {
    if (!validDarwinPath(DarwinPath))
      return {};

    return "dwarfs:" + std::to_string(ImageIdentity.size()) + ":" +
           ImageIdentity + ":" + std::string(DarwinPath);
  }

  bool readFile(std::string_view DarwinPath, std::vector<uint8_t> &Data,
                std::string &Error) const override {
    Data.clear();
    Error.clear();

    if (!validDarwinPath(DarwinPath)) {
      Error = "RuntimeRoot path is not an absolute Darwin path";
      return false;
    }

    std::string Path(DarwinPath);
    std::array<char, 1024> BridgeError{};
    uint8_t *RawData = nullptr;
    size_t RawSize = 0;

    const int Result = Read(Handle, Path.c_str(), &RawData, &RawSize,
                            BridgeError.data(), BridgeError.size());
    if (Result == 0) {
      if (RawData != nullptr)
        Free(RawData);
      Error = BridgeError[0] != '\0'
                  ? std::string(BridgeError.data())
                  : "DwarFS reader bridge could not read RuntimeRoot entry";
      return false;
    }

    if (RawSize != 0 && RawData == nullptr) {
      Error = "DwarFS reader bridge returned a null buffer for non-empty data";
      return false;
    }
    if (RawSize > Data.max_size()) {
      if (RawData != nullptr)
        Free(RawData);
      Error = "DwarFS RuntimeRoot entry is too large for host memory";
      return false;
    }

    try {
      Data.assign(RawData, RawData + RawSize);
    } catch (const std::exception &Ex) {
      if (RawData != nullptr)
        Free(RawData);
      Error = std::string("cannot copy DwarFS RuntimeRoot entry: ") + Ex.what();
      return false;
    }

    if (RawData != nullptr)
      Free(RawData);
    return true;
  }

private:
  DwarfsRuntimeRootStore(HMODULE Module, IpaSimDwarfsReaderHandle Handle,
                         IpaSimDwarfsReaderOpenFn Open,
                         IpaSimDwarfsReaderReadFn Read,
                         IpaSimDwarfsReaderFreeFn Free,
                         IpaSimDwarfsReaderCloseFn Close,
                         std::string ImageIdentity)
      : Module(Module), Handle(Handle), Open(Open), Read(Read), Free(Free),
        Close(Close), ImageIdentity(std::move(ImageIdentity)) {}

  HMODULE Module = nullptr;
  IpaSimDwarfsReaderHandle Handle = nullptr;
  IpaSimDwarfsReaderOpenFn Open = nullptr;
  IpaSimDwarfsReaderReadFn Read = nullptr;
  IpaSimDwarfsReaderFreeFn Free = nullptr;
  IpaSimDwarfsReaderCloseFn Close = nullptr;
  std::string ImageIdentity;
};

} // namespace

std::unique_ptr<RuntimeRootStore>
makeDwarfsRuntimeRootStore(const std::string &ImagePath,
                           const std::string &ReaderBridgePath,
                           std::string &Error) {
  return DwarfsRuntimeRootStore::create(ImagePath, ReaderBridgePath, Error);
}

} // namespace ipasim
