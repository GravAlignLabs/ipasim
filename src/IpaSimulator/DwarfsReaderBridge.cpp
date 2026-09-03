#include "ipasim/DwarfsReaderBridgeApi.h"

#include <dwarfs/logger.h>
#include <dwarfs/os_access_generic.h>
#include <dwarfs/reader/filesystem_v2.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <system_error>

namespace {

void writeError(char *Error, size_t ErrorCapacity, std::string_view Message) {
  if (Error == nullptr || ErrorCapacity == 0)
    return;

  const size_t Count = std::min(ErrorCapacity - 1, Message.size());
  if (Count != 0)
    std::memcpy(Error, Message.data(), Count);
  Error[Count] = '\0';
}

void clearError(char *Error, size_t ErrorCapacity) {
  if (Error != nullptr && ErrorCapacity != 0)
    Error[0] = '\0';
}

struct ReaderContext {
  explicit ReaderContext(const std::filesystem::path &ImagePath)
      : FileSystem(Logger, OsAccess, ImagePath) {}

  dwarfs::os_access_generic OsAccess;
  dwarfs::null_logger Logger;
  dwarfs::reader::filesystem_v2 FileSystem;
};

} // namespace

extern "C" __declspec(dllexport) uint32_t ipasim_dwarfs_reader_abi(void) {
  return IPASIM_DWARFS_READER_ABI_VERSION;
}

extern "C" __declspec(dllexport) IpaSimDwarfsReaderHandle
ipasim_dwarfs_reader_open(const wchar_t *ImagePath, char *Error,
                          size_t ErrorCapacity) {
  clearError(Error, ErrorCapacity);

  if (ImagePath == nullptr || ImagePath[0] == L'\0') {
    writeError(Error, ErrorCapacity, "DwarFS image path is empty");
    return nullptr;
  }

  try {
    return new ReaderContext(std::filesystem::path(ImagePath));
  } catch (const std::exception &Ex) {
    writeError(Error, ErrorCapacity, Ex.what());
  } catch (...) {
    writeError(Error, ErrorCapacity,
               "unknown exception while opening DwarFS image");
  }

  return nullptr;
}

extern "C" __declspec(dllexport) int ipasim_dwarfs_reader_read(
    IpaSimDwarfsReaderHandle Handle, const char *DarwinPath, uint8_t **Data,
    size_t *Size, char *Error, size_t ErrorCapacity) {
  clearError(Error, ErrorCapacity);

  if (Data == nullptr || Size == nullptr) {
    writeError(Error, ErrorCapacity, "DwarFS read output pointers are null");
    return 0;
  }

  *Data = nullptr;
  *Size = 0;

  if (Handle == nullptr) {
    writeError(Error, ErrorCapacity, "DwarFS reader handle is null");
    return 0;
  }
  if (DarwinPath == nullptr || DarwinPath[0] != '/') {
    writeError(Error, ErrorCapacity,
               "DwarFS lookup path is not an absolute Darwin path");
    return 0;
  }

  try {
    auto &Context = *static_cast<ReaderContext *>(Handle);
    const auto Entry = Context.FileSystem.find(DarwinPath);
    if (!Entry) {
      writeError(Error, ErrorCapacity,
                 std::string("DwarFS entry is unavailable: ") + DarwinPath);
      return 0;
    }

    std::error_code EC;
    const int Inode = Context.FileSystem.open(Entry->inode(), EC);
    if (EC) {
      writeError(Error, ErrorCapacity,
                 std::string("cannot open DwarFS entry: ") + EC.message());
      return 0;
    }

    const std::string Bytes = Context.FileSystem.read_string(Inode, EC);
    if (EC) {
      writeError(Error, ErrorCapacity,
                 std::string("cannot read DwarFS entry: ") + EC.message());
      return 0;
    }

    if (!Bytes.empty()) {
      auto Buffer = std::make_unique<uint8_t[]>(Bytes.size());
      std::memcpy(Buffer.get(), Bytes.data(), Bytes.size());
      *Data = Buffer.release();
    }
    *Size = Bytes.size();
    return 1;
  } catch (const std::bad_alloc &) {
    writeError(Error, ErrorCapacity,
               "memory allocation failed while reading DwarFS entry");
  } catch (const std::exception &Ex) {
    writeError(Error, ErrorCapacity, Ex.what());
  } catch (...) {
    writeError(Error, ErrorCapacity,
               "unknown exception while reading DwarFS entry");
  }

  return 0;
}

extern "C" __declspec(dllexport) void ipasim_dwarfs_reader_free(void *Data) {
  delete[] static_cast<uint8_t *>(Data);
}

extern "C" __declspec(dllexport) void
ipasim_dwarfs_reader_close(IpaSimDwarfsReaderHandle Handle) {
  delete static_cast<ReaderContext *>(Handle);
}
