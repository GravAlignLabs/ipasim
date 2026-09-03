#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPASIM_DWARFS_READER_ABI_VERSION 1u

typedef void *IpaSimDwarfsReaderHandle;

typedef uint32_t (*IpaSimDwarfsReaderAbiFn)(void);
typedef IpaSimDwarfsReaderHandle (*IpaSimDwarfsReaderOpenFn)(
    const wchar_t *ImagePath, char *Error, size_t ErrorCapacity);
typedef int (*IpaSimDwarfsReaderReadFn)(IpaSimDwarfsReaderHandle Handle,
                                        const char *DarwinPath,
                                        uint8_t **Data, size_t *Size,
                                        char *Error, size_t ErrorCapacity);
typedef void (*IpaSimDwarfsReaderFreeFn)(void *Data);
typedef void (*IpaSimDwarfsReaderCloseFn)(IpaSimDwarfsReaderHandle Handle);

#ifdef __cplusplus
}
#endif
