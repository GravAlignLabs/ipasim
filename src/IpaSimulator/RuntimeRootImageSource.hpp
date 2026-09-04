#pragma once

#include "ipasim/RuntimeRootStore.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ipasim::probe::image_source_detail {

// Static preflights inspect both the host-resident application image and
// RuntimeRoot dependencies. Keep that distinction explicit so a Darwin install
// name is never accidentally projected back through the Windows filesystem.
struct ImageSource {
  std::string Key;
  std::string DisplayName;
  std::filesystem::path HostPath;
  std::string DarwinPath;
  const RuntimeRootStore *Store = nullptr;

  bool isRuntimeImage() const { return Store != nullptr; }
};

inline std::string hostImageKey(const std::filesystem::path &Path) {
  std::error_code EC;
  std::filesystem::path Absolute = std::filesystem::absolute(Path, EC);
  if (EC)
    Absolute = Path;
  return Absolute.lexically_normal().make_preferred().string();
}

inline std::string hostDisplayName(const std::filesystem::path &Path) {
  const std::string Name = Path.filename().string();
  return Name.empty() ? Path.string() : Name;
}

inline std::string darwinDisplayName(std::string_view Path) {
  const std::size_t Separator = Path.find_last_of('/');
  if (Separator == std::string_view::npos || Separator + 1 == Path.size())
    return std::string(Path);
  return std::string(Path.substr(Separator + 1));
}

inline ImageSource makeHostImageSource(std::filesystem::path Path) {
  ImageSource Source;
  Source.Key = hostImageKey(Path);
  Source.DisplayName = hostDisplayName(Path);
  Source.HostPath = std::move(Path);
  return Source;
}

inline bool makeRuntimeImageSource(const RuntimeRootStore &Store,
                                   std::string_view DarwinPath,
                                   ImageSource &Source, std::string &Error) {
  Source = ImageSource{};
  Error.clear();
  if (DarwinPath.empty() || DarwinPath.front() != '/') {
    Error = "RuntimeRoot image source is not an absolute Darwin install name";
    return false;
  }

  const std::string Key = Store.identity(DarwinPath);
  if (Key.empty()) {
    Error = "RuntimeRoot store could not identify " + std::string(DarwinPath);
    return false;
  }

  Source.Key = Key;
  Source.DisplayName = darwinDisplayName(DarwinPath);
  Source.DarwinPath = std::string(DarwinPath);
  Source.Store = &Store;
  return true;
}

inline bool readImageSource(const ImageSource &Source,
                            std::vector<std::uint8_t> &Data,
                            std::string &Error) {
  Data.clear();
  Error.clear();

  if (Source.isRuntimeImage())
    return Source.Store->readFile(Source.DarwinPath, Data, Error);

  std::error_code EC;
  if (!std::filesystem::is_regular_file(Source.HostPath, EC) || EC) {
    Error = "host image is unavailable: " + Source.HostPath.string();
    return false;
  }

  const std::uintmax_t Size = std::filesystem::file_size(Source.HostPath, EC);
  if (EC) {
    Error = "cannot determine host image size: " + Source.HostPath.string();
    return false;
  }
  if (Size > std::numeric_limits<std::size_t>::max() ||
      Size > static_cast<std::uintmax_t>(
                 std::numeric_limits<std::streamsize>::max())) {
    Error = "host image is too large to read: " + Source.HostPath.string();
    return false;
  }

  std::ifstream Input(Source.HostPath, std::ios::binary);
  if (!Input) {
    Error = "cannot open host image: " + Source.HostPath.string();
    return false;
  }

  Data.resize(static_cast<std::size_t>(Size));
  if (!Data.empty() &&
      !Input.read(reinterpret_cast<char *>(Data.data()),
                  static_cast<std::streamsize>(Data.size()))) {
    Data.clear();
    Error = "cannot read complete host image: " + Source.HostPath.string();
    return false;
  }
  return true;
}

} // namespace ipasim::probe::image_source_detail
