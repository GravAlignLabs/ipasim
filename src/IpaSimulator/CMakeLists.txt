set (SOURCE_FILES
    DynamicLoader.cpp
    Emulator.cpp
    IpaSimulator.cpp
    LoadedLibrary.cpp
    MachO.cpp
    ModernMachO.cpp
    SysTranslator.cpp)

if (NOT IPASIM_MODERN_CORE)
    list (APPEND SOURCE_FILES TextBlockStream.cpp)
endif ()

if (NOT DEFINED IPASIM_RUNTIME_ARCH)
    set (IPASIM_RUNTIME_ARCH "x64")
endif ()
if (NOT IPASIM_RUNTIME_ARCH STREQUAL "x64")
    message (FATAL_ERROR "The ARM64 ipaSim runtime requires a 64-bit Windows host")
endif ()

# Runtime dependencies share the ipaSim process address space and therefore
# must be built for x64.
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set (CURRENT_LIEF_RUNTIME_DIR "${BINARY_DIR}/lief-x64-Debug")
    set (CURRENT_LIBFFI_RUNTIME_DIR "${BINARY_DIR}/Libffi-x64-Debug")
else ()
    set (CURRENT_LIEF_RUNTIME_DIR "${BINARY_DIR}/lief-x64-Release")
    set (CURRENT_LIBFFI_RUNTIME_DIR "${BINARY_DIR}/Libffi-x64-Release")
endif ()

# The iOS Simulator runtime links a small set of libSystem dependencies against
# its macOS host. Windows has no such dylibs, so expose only target-proven host
# semantics through explicit native adapters. These are intentionally separate
# from the historical WinObjC wrapper stack and must not accumulate placeholder
# exports: each boundary has a real subsystem behind it.
if (IPASIM_MODERN_CORE)
    # Mach IPC is a real subsystem shared by the Darwin bridge and a semantic
    # smoke executable. Keeping it in its own target prevents mach_msg_overwrite
    # from becoming a one-off host stub and gives later mach_port_* boundaries a
    # single implementation to extend.
    add_library (IpaSimMachIpc STATIC
        MachIpc.cpp)
    target_compile_options (IpaSimMachIpc PRIVATE -std=c++17)

    # Darwin filesystem special files do not map directly onto NTFS. Keep the
    # guest pathname/type semantics in one adapter and use Win32 named pipes as
    # the FIFO transport. Future open/read/write/stat/unlink boundaries extend
    # this same registry instead of inventing per-symbol translations.
    add_library (IpaSimDarwinFs STATIC
        FifoAdapter.cpp)
    target_compile_options (IpaSimDarwinFs PRIVATE -std=c++17)

    # Darwin socket descriptors are 32-bit integers while Win64 SOCKET is
    # pointer-width. Keep a real descriptor registry and translate Darwin
    # sockaddr layouts before crossing into Winsock. This backs the target-
    # proven __sendto boundary and is the single subsystem future socket calls
    # will extend.
    add_library (IpaSimDarwinSockets STATIC
        DarwinSocketAdapter.cpp)
    target_compile_options (IpaSimDarwinSockets PRIVATE -std=c++17)
    target_link_libraries (IpaSimDarwinSockets PUBLIC ws2_32)

    add_library (IpaSimDarwinHost SHARED
        DarwinHostBridge.cpp
        DarwinTimeBridge.cpp
        DarwinSocketBridge.cpp
        DarwinKernelBridge.cpp
        DarwinHostCompatBridge.cpp
        DarwinVoucherBridge.cpp
        DarwinTelemetryBridge.cpp
        DarwinUlockBridge.cpp)
    target_compile_options (IpaSimDarwinHost PRIVATE -std=c++17)
    target_link_libraries (IpaSimDarwinHost PRIVATE
        IpaSimMachIpc
        IpaSimDarwinFs
        IpaSimDarwinSockets
        psapi
        Mincore.lib)

    add_executable (MachIpcSmoke
        MachIpcSmoke.cpp)
    target_compile_options (MachIpcSmoke PRIVATE -std=c++17)
    target_link_libraries (MachIpcSmoke PRIVATE IpaSimMachIpc)

    add_executable (DarwinFsSmoke
        DarwinFsSmoke.cpp)
    target_compile_options (DarwinFsSmoke PRIVATE -std=c++17)
    target_link_libraries (DarwinFsSmoke PRIVATE IpaSimDarwinFs)

    add_executable (DarwinTimeSmoke
        DarwinTimeSmoke.cpp)
    target_compile_options (DarwinTimeSmoke PRIVATE -std=c++17)
    target_link_libraries (DarwinTimeSmoke PRIVATE Mincore.lib)

    add_executable (DarwinSocketSmoke
        DarwinSocketSmoke.cpp)
    target_compile_options (DarwinSocketSmoke PRIVATE -std=c++17)
    target_link_libraries (DarwinSocketSmoke PRIVATE IpaSimDarwinSockets)

    add_executable (DarwinKernelSmoke
        DarwinKernelSmoke.cpp)
    target_compile_options (DarwinKernelSmoke PRIVATE -std=c++17)

    add_executable (DarwinVoucherSmoke
        DarwinVoucherSmoke.cpp)
    target_compile_options (DarwinVoucherSmoke PRIVATE -std=c++17)

    add_executable (DarwinTelemetrySmoke
        DarwinTelemetrySmoke.cpp)
    target_compile_options (DarwinTelemetrySmoke PRIVATE -std=c++17)

    add_executable (DarwinUlockSmoke
        DarwinUlockSmoke.cpp)
    target_compile_options (DarwinUlockSmoke PRIVATE -std=c++17)

    # Run the semantic checks as part of the normal core dependency graph. A
    # compile/link/runtime failure therefore flows through the existing captured
    # core-build log and the single self-updating PR diagnostic comment before
    # CI fails normally. The smokes also inspect/call the built host bridge so a
    # semantic subsystem cannot pass while its PE export is absent.
    add_custom_target (MachIpcCheck
        COMMAND "$<TARGET_FILE:MachIpcSmoke>" "$<TARGET_FILE:IpaSimDarwinHost>"
        DEPENDS MachIpcSmoke IpaSimDarwinHost
        USES_TERMINAL)
    add_custom_target (DarwinFsCheck
        COMMAND "$<TARGET_FILE:DarwinFsSmoke>" "$<TARGET_FILE:IpaSimDarwinHost>"
        DEPENDS DarwinFsSmoke IpaSimDarwinHost
        USES_TERMINAL)
    add_custom_target (DarwinTimeCheck
        COMMAND "$<TARGET_FILE:DarwinTimeSmoke>" "$<TARGET_FILE:IpaSimDarwinHost>"
        DEPENDS DarwinTimeSmoke IpaSimDarwinHost
        USES_TERMINAL)
    add_custom_target (DarwinSocketCheck
        COMMAND "$<TARGET_FILE:DarwinSocketSmoke>" "$<TARGET_FILE:IpaSimDarwinHost>"
        DEPENDS DarwinSocketSmoke IpaSimDarwinHost
        USES_TERMINAL)
    add_custom_target (DarwinKernelCheck
        COMMAND "$<TARGET_FILE:DarwinKernelSmoke>" "$<TARGET_FILE:IpaSimDarwinHost>"
        DEPENDS DarwinKernelSmoke IpaSimDarwinHost
        USES_TERMINAL)
    add_custom_target (DarwinVoucherCheck
        COMMAND "$<TARGET_FILE:DarwinVoucherSmoke>" "$<TARGET_FILE:IpaSimDarwinHost>"
        DEPENDS DarwinVoucherSmoke IpaSimDarwinHost
        USES_TERMINAL)
    add_custom_target (DarwinTelemetryCheck
        COMMAND "$<TARGET_FILE:DarwinTelemetrySmoke>" "$<TARGET_FILE:IpaSimDarwinHost>"
        DEPENDS DarwinTelemetrySmoke IpaSimDarwinHost
        USES_TERMINAL)
    add_custom_target (DarwinUlockCheck
        COMMAND "$<TARGET_FILE:DarwinUlockSmoke>" "$<TARGET_FILE:IpaSimDarwinHost>"
        DEPENDS DarwinUlockSmoke IpaSimDarwinHost
        USES_TERMINAL)

    # A module-definition file is linker input, not an object/source input.
    # The modern core deliberately uses clang.exe as the compiler driver and
    # lld-link.exe as the PE linker. Because this superbuild forces the Clang
    # compiler ID during configure, CMake can select the generic Windows-GNU
    # definition-file flag (empty), which would pass the .def path to lld-link
    # as an untyped input. Route it through Clang's explicit linker wrapper so
    # lld-link receives /DEF:<file> and applies the documented export alias.
    set (DARWIN_HOST_DEF "${CMAKE_CURRENT_SOURCE_DIR}/DarwinHostBridge.def")
    set_target_properties (IpaSimDarwinHost PROPERTIES
        PREFIX ""
        OUTPUT_NAME "IpaSimDarwinHost"
        LINK_FLAGS "-Xlinker /DEF:\"${DARWIN_HOST_DEF}\""
        LINK_DEPENDS "${DARWIN_HOST_DEF}")
endif ()

add_library (IpaSimLibrary SHARED ${SOURCE_FILES})
if (NOT IPASIM_MODERN_CORE)
    add_prep_dep (IpaSimLibrary)
else ()
    # Build and execute the target-proven semantic checkpoints before accepting
    # the bridge/core build. Diagnostics are captured by the parent CI step.
    add_dependencies (IpaSimLibrary
        IpaSimDarwinHost
        MachIpcCheck
        DarwinFsCheck
        DarwinTimeCheck
        DarwinSocketCheck
        DarwinKernelCheck
        DarwinVoucherCheck
        DarwinTelemetryCheck
        DarwinUlockCheck)
endif ()

# The legacy path used a patched Clang plus the Coroutines TS. Current Clang
# and the current Windows SDK use standard C++20 coroutines instead. Delayed
# template parsing matches the MSVC model used by C++/WinRT's generated headers
# that remain in the AppX file-discovery boundary.
if (IPASIM_MODERN_CORE)
    target_compile_options (IpaSimLibrary PRIVATE
        -std=c++20
        -fdelayed-template-parsing)
else ()
    target_compile_options (IpaSimLibrary PRIVATE -std=c++17 -fcoroutines-ts)
endif ()

# MachO.cpp and ModernMachO.cpp consume LLVM's public Mach-O ABI declarations.
# The modern core does not build the historical patched compiler; its generated
# LLVM configuration header is provided by include/llvm/Config/llvm-config.h.
if (IPASIM_MODERN_CORE)
    set (LLVM_INCLUDE_DIRS "${SOURCE_DIR}/deps/llvm/include")
else ()
    set (LLVM_INCLUDE_DIRS
        "${CURRENT_CLANG_CMAKE_DIR}/include"
        "${SOURCE_DIR}/deps/llvm/include")
endif ()
file (MAKE_DIRECTORY ${LLVM_INCLUDE_DIRS})
if (NOT IPASIM_MODERN_CORE AND TARGET HeadersAnalyzer)
    add_dependencies (IpaSimLibrary HeadersAnalyzer)
endif ()

target_include_directories (IpaSimLibrary PRIVATE
    "${SOURCE_DIR}/include"
    ${LLVM_INCLUDE_DIRS})

target_compile_definitions (IpaSimLibrary PRIVATE
    # Don't include `winsock.h`, it interferes with Unicorn's `winsock2.h`. See
    # <https://stackoverflow.com/a/1372836>.
    _WINSOCKAPI_
    NOMINMAX
    $<$<BOOL:${IPASIM_MODERN_CORE}>:IPASIM_MODERN_CORE>
    $<$<CONFIG:Debug>:IPASIM_PRINT_EMU_INFO>
    IPASIM_DISABLE_FATAL_ERRORS)

# Only the historical WinObjC path requires the old resumable-functions switch.
if (NOT IPASIM_MODERN_CORE)
    target_compile_definitions (IpaSimLibrary PRIVATE
        _RESUMABLE_FUNCTIONS_SUPPORTED)
endif ()

# LIEF must match the x64 host process.
add_custom_target (lief-x64
    COMMENT "LIEF x64"
    COMMAND ninja "lief-x64-${CMAKE_BUILD_TYPE}"
    BYPRODUCTS "${CURRENT_LIEF_RUNTIME_DIR}/libLIEF.a"
    WORKING_DIRECTORY "${BINARY_DIR}"
    USES_TERMINAL)
add_library (LIEF STATIC IMPORTED)
set_target_properties (LIEF PROPERTIES
    IMPORTED_LOCATION "${CURRENT_LIEF_RUNTIME_DIR}/libLIEF.a")
set (LIEF_INCLUDE_DIRS
    "${SOURCE_DIR}/deps/LIEF/include"
    "${SOURCE_DIR}/deps/LIEF/api/c/include"
    "${CURRENT_LIEF_RUNTIME_DIR}/include")
file (MAKE_DIRECTORY ${LIEF_INCLUDE_DIRS})
target_include_directories (LIEF SYSTEM INTERFACE ${LIEF_INCLUDE_DIRS})
target_compile_options (LIEF INTERFACE
    "-include${CMAKE_CURRENT_SOURCE_DIR}/my_iso646.h")
target_compile_definitions (LIEF INTERFACE NOMINMAX)
target_link_libraries (LIEF INTERFACE $<IF:$<CONFIG:Debug>,msvcrtd,msvcrt>)
add_dependencies (LIEF lief-x64)

# libffi must also be x64 because trampolines contain host pointers. This port
# generates ffi.h under <build>/build/include, not directly under <build>/include.
add_custom_target (Libffi-x64
    COMMENT "Libffi x64"
    COMMAND ninja "Libffi-x64-${CMAKE_BUILD_TYPE}"
    BYPRODUCTS "${CURRENT_LIBFFI_RUNTIME_DIR}/lib/libffi.lib"
    WORKING_DIRECTORY "${BINARY_DIR}"
    USES_TERMINAL)
add_library (ffi STATIC IMPORTED)
set_target_properties (ffi PROPERTIES
    IMPORTED_LOCATION "${CURRENT_LIBFFI_RUNTIME_DIR}/lib/libffi.lib")
add_dependencies (ffi Libffi-x64)
set (LIBFFI_INCLUDE_DIRS
    "${CURRENT_LIBFFI_RUNTIME_DIR}/build/include")
file (MAKE_DIRECTORY ${LIBFFI_INCLUDE_DIRS})
target_include_directories (ffi SYSTEM INTERFACE ${LIBFFI_INCLUDE_DIRS})
target_compile_definitions (ffi INTERFACE FFI_BUILDING)

# The pinned libffi CMake port selects source/ffi64.c whenever the host pointer
# size is 64 bits. That source is the Unix/System-V x86-64 implementation and
# is compiled out by MSVC, while the same repository's source/ffi.c contains
# the real X86_WIN64 implementation paired with win64.S. Build that existing
# machine-dependent source explicitly for the modern x64 core instead of
# defining Unix ABI macros or modifying the pinned submodule.
if (IPASIM_MODERN_CORE)
    add_library (ffi-win64-machdep OBJECT
        "${SOURCE_DIR}/deps/Libffi/source/ffi.c")
    target_include_directories (ffi-win64-machdep PRIVATE ${LIBFFI_INCLUDE_DIRS})
    # source/ffi.c is a multi-target implementation. The Win64 branch is the
    # one paired with the vendored win64.S assembly and deliberately disables
    # libffi's unsupported x86 raw API. State those target facts explicitly so
    # this object cannot fall through into FFI_SYSV code under clang-cl.
    target_compile_definitions (ffi-win64-machdep PRIVATE
        FFI_BUILDING
        X86_WIN64
        FFI_NO_RAW_API=1)
    add_dependencies (ffi-win64-machdep Libffi-x64)
    target_sources (IpaSimLibrary PRIVATE $<TARGET_OBJECTS:ffi-win64-machdep>)
endif ()

# ANGLE in the historical WinObjC package is Win32-only and cannot be loaded
# into an x64 process. The modern framework renderer will replace that boundary;
# the core emulator library therefore has no dependency on those binaries.

target_link_libraries (IpaSimLibrary PRIVATE WindowsApp.lib unicorn LIEF ffi)

# This executable is deliberately independent of the ipaSim core. It proves
# that the vendored Unicorn DLL exposes both AArch64 endian backends and can
# execute real little-endian AArch64 instructions before a core build is trusted.
if (IPASIM_MODERN_CORE)
    add_executable (Arm64Smoke Arm64Smoke.cpp)
    target_compile_options (Arm64Smoke PRIVATE -std=c++17)
    target_link_libraries (Arm64Smoke PRIVATE unicorn)

    # First user-testable checkpoint: run the real ipaSim DynamicLoader against
    # an extracted app Mach-O without crossing into the incomplete SwiftUI/UIKit
    # application launch boundary.
    add_executable (IpaProbe IpaProbe.cpp)
    target_compile_options (IpaProbe PRIVATE -std=c++17)
    target_include_directories (IpaProbe PRIVATE "${SOURCE_DIR}/include")
    target_link_libraries (IpaProbe PRIVATE IpaSimLibrary)
endif ()

# The current milestone is a real x64 emulator core. The Windows application
# shell will be reconnected after the modern framework boundary replaces the
# Win32-only WinObjC/ANGLE stack.
add_custom_target (IpaSimApp)
add_dependencies (IpaSimApp IpaSimLibrary)
if (TARGET CodeGen)
    add_dependencies (IpaSimApp CodeGen)
endif ()
if (TARGET Frameworks)
    add_dependencies (IpaSimApp Frameworks)
endif ()
