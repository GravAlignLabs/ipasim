#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ipasim {

// Immutable byte-source boundary for Apple RuntimeRoot objects. Darwin install
// names stay in Darwin form here; callers do not need to materialize them as
// Windows filesystem paths. The directory backend preserves the current
// extracted-RuntimeRoot behavior while allowing a later compressed-image
// backend to satisfy the same contract without mounting or extraction.
class RuntimeRootStore {
public:
  virtual ~RuntimeRootStore() = default;

  virtual bool readFile(std::string_view DarwinPath,
                        std::vector<std::uint8_t> &Data,
                        std::string &Error) const = 0;
};

namespace runtime_root_detail {

class DirectoryRuntimeRootStore final : public RuntimeRootStore {
public:
  explicit DirectoryRuntimeRootStore(std::filesystem::path Root)
      : Root(std::move(Root)) {}

  bool readFile(std::string_view DarwinPath, std::vector<std::uint8_t> &Data,
                std::string &Error) const override {
    Data.clear();
    Error.clear();

    if (DarwinPath.empty() || DarwinPath.front() != '/') {
      Error = "RuntimeRoot path is not an absolute Darwin install name";
      return false;
    }

    const std::filesystem::path Relative(
        std::string(DarwinPath.substr(1)));
    const std::filesystem::path Resolved =
        (Root / Relative).lexically_normal();

    std::error_code EC;
    if (!std::filesystem::is_regular_file(Resolved, EC) || EC) {
      Error = "RuntimeRoot file is unavailable: " + Resolved.string();
      return false;
    }

    const std::uintmax_t Size = std::filesystem::file_size(Resolved, EC);
    if (EC) {
      Error = "cannot determine RuntimeRoot file size: " + Resolved.string();
      return false;
    }
    if (Size > std::numeric_limits<std::size_t>::max() ||
        Size > static_cast<std::uintmax_t>(
                   std::numeric_limits<std::streamsize>::max())) {
      Error = "RuntimeRoot file is too large to read: " + Resolved.string();
      return false;
    }

    std::ifstream Input(Resolved, std::ios::binary);
    if (!Input) {
      Error = "cannot open RuntimeRoot file: " + Resolved.string();
      return false;
    }

    Data.resize(static_cast<std::size_t>(Size));
    if (!Data.empty() &&
        !Input.read(reinterpret_cast<char *>(Data.data()),
                    static_cast<std::streamsize>(Data.size()))) {
      Data.clear();
      Error = "cannot read complete RuntimeRoot file: " + Resolved.string();
      return false;
    }

    return true;
  }

private:
  std::filesystem::path Root;
};

} // namespace runtime_root_detail

inline std::unique_ptr<RuntimeRootStore>
makeDirectoryRuntimeRootStore(const std::string &Path, std::string &Error) {
  Error.clear();

  std::error_code EC;
  std::filesystem::path Root =
      std::filesystem::absolute(std::filesystem::path(Path), EC);
  if (EC || !std::filesystem::is_directory(Root, EC) || EC) {
    Error = "RuntimeRoot directory is unavailable: " + Path;
    return nullptr;
  }

  return std::make_unique<runtime_root_detail::DirectoryRuntimeRootStore>(
      Root.lexically_normal().make_preferred());
}

} // namespace ipasim
