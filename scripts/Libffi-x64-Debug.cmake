list (APPEND CMAKE_MODULE_PATH "${SOURCE_DIR}/scripts")
include (CommonVariables)

set (LIBFFI_X64_DIR "${BINARY_DIR}/Libffi-x64-Debug")
file (MAKE_DIRECTORY "${LIBFFI_X64_DIR}")
execute_process (
    COMMAND "${CMAKE_COMMAND}" -G Ninja
        -DCMAKE_BUILD_TYPE=Debug
        "-DCMAKE_C_FLAGS=/FS"
        # Match the Release boundary: the pinned libffi build's compiler side
        # outputs are not safe to route through sccache.
        "-DCMAKE_C_COMPILER_LAUNCHER="
        "-DCMAKE_CXX_COMPILER_LAUNCHER="
        -DCMAKE_EXPORT_COMPILE_COMMANDS=On
        "${SOURCE_DIR}/deps/Libffi"
    WORKING_DIRECTORY "${LIBFFI_X64_DIR}"
    RESULT_VARIABLE LIBFFI_CONFIGURE_RESULT)
if (NOT LIBFFI_CONFIGURE_RESULT EQUAL 0)
    message (FATAL_ERROR
        "libffi x64 Debug configure failed with ${LIBFFI_CONFIGURE_RESULT}")
endif ()

# The pinned libffi port correctly selects X86_WIN64 in ffi.h/ffitarget.h, but
# its fficonfig.h template unconditionally emits `#undef FFI_NO_RAW_API`.
# ffi_common.h includes that generated header after ffi.h, erasing the Win64
# target contract and exposing the unsupported FFI_SYSV raw-API path. Correct
# the generated configuration rather than modifying the pinned submodule or
# inventing compatibility symbols.
set (LIBFFI_CONFIG_HEADER "${LIBFFI_X64_DIR}/build/include/fficonfig.h")
if (NOT EXISTS "${LIBFFI_CONFIG_HEADER}")
    message (FATAL_ERROR
        "libffi did not generate expected config header: ${LIBFFI_CONFIG_HEADER}")
endif ()
file (READ "${LIBFFI_CONFIG_HEADER}" LIBFFI_CONFIG_CONTENT)
string (FIND "${LIBFFI_CONFIG_CONTENT}" "#undef FFI_NO_RAW_API"
    LIBFFI_RAW_API_UNDEF_OFFSET)
if (LIBFFI_RAW_API_UNDEF_OFFSET EQUAL -1)
    message (FATAL_ERROR
        "Pinned libffi config no longer has the expected FFI_NO_RAW_API defect")
endif ()
string (REPLACE "#undef FFI_NO_RAW_API" "#define FFI_NO_RAW_API 1"
    LIBFFI_CONFIG_CONTENT "${LIBFFI_CONFIG_CONTENT}")
file (WRITE "${LIBFFI_CONFIG_HEADER}" "${LIBFFI_CONFIG_CONTENT}")
