cmake_minimum_required (VERSION 3.12.2)

# TODO: When calling ninja, hide message "no work to do".

option (SUPERBUILD "Configure Clang and then the project itself." ON)

set (CL_COMPILER OFF)

if (SUPERBUILD)

    project (ipaSim-superbuild LANGUAGES NONE)

    set (SOURCE_DIR "${CMAKE_SOURCE_DIR}")
    set (BINARY_DIR "${CMAKE_BINARY_DIR}")

    list (APPEND CMAKE_MODULE_PATH "${SOURCE_DIR}/scripts")
    include (CommonVariables)

    # The compiler/tablegen executables remain build tools in their historical
    # x86 directories. The emulator process and libraries that live inside its
    # address space are built in x64 directories.
    set (DEBUG_IPASIM_X64_CMAKE_DIR "${BINARY_DIR}/ipaSim-x64-Debug")
    set (RELEASE_IPASIM_X64_CMAKE_DIR "${BINARY_DIR}/ipaSim-x64-Release")
    set (DEBUG_LIEF_X64_CMAKE_DIR "${BINARY_DIR}/lief-x64-Debug")
    set (RELEASE_LIEF_X64_CMAKE_DIR "${BINARY_DIR}/lief-x64-Release")
    set (DEBUG_LIBFFI_X64_CMAKE_DIR "${BINARY_DIR}/Libffi-x64-Debug")
    set (RELEASE_LIBFFI_X64_CMAKE_DIR "${BINARY_DIR}/Libffi-x64-Release")

    function (add_superbuild_target name cmakeDir)
        add_custom_command (
            OUTPUT "${cmakeDir}/build.ninja"
            COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIR=${SOURCE_DIR}"
                "-DBINARY_DIR=${BINARY_DIR}"
                -P "${SOURCE_DIR}/scripts/${name}.cmake"
            USES_TERMINAL
            DEPENDS "${SOURCE_DIR}/scripts/${name}.cmake"
                "${SOURCE_DIR}/scripts/CommonVariables.cmake")
        add_custom_target ("config-${name}"
            DEPENDS "${cmakeDir}/build.ninja")
    endfunction (add_superbuild_target)

    # Patched compiler/tool generation remains a host-tool concern.
    add_superbuild_target (clang-x86-Debug "${DEBUG_CLANG_CMAKE_DIR}")
    add_superbuild_target (clang-x86-Release "${RELEASE_CLANG_CMAKE_DIR}")

    # Primary emulator runtime is now x64.
    add_superbuild_target (ipaSim-x64-Debug "${DEBUG_IPASIM_X64_CMAKE_DIR}")
    add_superbuild_target (ipaSim-x64-Release "${RELEASE_IPASIM_X64_CMAKE_DIR}")
    add_superbuild_target (lief-x64-Debug "${DEBUG_LIEF_X64_CMAKE_DIR}")
    add_superbuild_target (lief-x64-Release "${RELEASE_LIEF_X64_CMAKE_DIR}")
    add_superbuild_target (Libffi-x64-Debug "${DEBUG_LIBFFI_X64_CMAKE_DIR}")
    add_superbuild_target (Libffi-x64-Release "${RELEASE_LIBFFI_X64_CMAKE_DIR}")

    # Keep the historical WinObjC configure targets available only as source
    # reference while its framework boundary is being replaced. They are not a
    # dependency of the modern x64 emulator runtime.
    add_superbuild_target (winobjc-x86-Debug "${DEBUG_WINOBJC_CMAKE_DIR}")
    add_superbuild_target (winobjc-x86-Release "${RELEASE_WINOBJC_CMAKE_DIR}")

    add_custom_target (tblgens-x86-Release
        COMMENT "Tablegens"
        COMMAND ninja llvm-tblgen clang-tblgen
        WORKING_DIRECTORY "${RELEASE_CLANG_CMAKE_DIR}"
        USES_TERMINAL
        DEPENDS config-clang-x86-Release)

    add_dependencies (config-clang-x86-Debug tblgens-x86-Release)

    add_custom_target (clang-x86-Release
        COMMENT "Clang build tools"
        COMMAND ninja clang lld
        WORKING_DIRECTORY "${RELEASE_CLANG_CMAKE_DIR}"
        USES_TERMINAL
        DEPENDS config-clang-x86-Release)

    add_custom_target (clang-x86-Debug
        COMMENT "Clang build tools"
        COMMAND ninja clang lld
        WORKING_DIRECTORY "${DEBUG_CLANG_CMAKE_DIR}"
        USES_TERMINAL
        DEPENDS config-clang-x86-Debug)

    add_custom_target (prep-ipaSim-x64-Debug
        COMMENT "Prepare x64 ipaSim"
        DEPENDS config-ipaSim-x64-Debug clang-x86-Release)

    add_custom_target (prep-ipaSim-x64-Release
        COMMENT "Prepare x64 ipaSim"
        DEPENDS config-ipaSim-x64-Release clang-x86-Release)

    add_custom_target (ipaSim-x64-Debug
        COMMENT "ipaSim x64 core"
        COMMAND ninja IpaSimLibrary
        WORKING_DIRECTORY "${DEBUG_IPASIM_X64_CMAKE_DIR}"
        USES_TERMINAL
        DEPENDS config-ipaSim-x64-Debug)

    add_custom_target (ipaSim-x64-Release
        COMMENT "ipaSim x64 core"
        COMMAND ninja IpaSimLibrary
        WORKING_DIRECTORY "${RELEASE_IPASIM_X64_CMAKE_DIR}"
        USES_TERMINAL
        DEPENDS config-ipaSim-x64-Release)

    add_custom_target (lief-x64-Debug
        COMMENT "LIEF x64"
        COMMAND ninja LIB_LIEF_STATIC
        WORKING_DIRECTORY "${DEBUG_LIEF_X64_CMAKE_DIR}"
        USES_TERMINAL
        DEPENDS config-lief-x64-Debug)

    add_custom_target (lief-x64-Release
        COMMENT "LIEF x64"
        COMMAND ninja LIB_LIEF_STATIC
        WORKING_DIRECTORY "${RELEASE_LIEF_X64_CMAKE_DIR}"
        USES_TERMINAL
        DEPENDS config-lief-x64-Release)

    # The pinned libffi build hard-codes /Zi for Release, which sends every
    # parallel cl.exe process to one ffi.pdb. MSVC appends the _CL_ environment
    # options after generated command-line options, so /Z7 wins over /Zi and
    # embeds debug information per object. /FS remains as a defensive guard.
    # Keeping this at the superbuild invocation avoids modifying the submodule.
    add_custom_target (Libffi-x64-Debug
        COMMENT "Libffi x64"
        COMMAND "${CMAKE_COMMAND}" -E env "_CL_=/Z7 /FS" ninja ffi
        WORKING_DIRECTORY "${DEBUG_LIBFFI_X64_CMAKE_DIR}"
        USES_TERMINAL
        DEPENDS config-Libffi-x64-Debug)

    add_custom_target (Libffi-x64-Release
        COMMENT "Libffi x64"
        COMMAND "${CMAKE_COMMAND}" -E env "_CL_=/Z7 /FS" ninja ffi
        WORKING_DIRECTORY "${RELEASE_LIBFFI_X64_CMAKE_DIR}"
        USES_TERMINAL
        DEPENDS config-Libffi-x64-Release)

else (SUPERBUILD)

    option (USE_ORIG_CLANG "Use the original instead of our patched Clang. For testing purposes only." OFF)
    option (USE_DEBUG_CLANG "Use Debug version of our patched Clang." OFF)
    option (IPASIM_MODERN_CORE "Build the x64 ARM64-capable emulator core without the legacy Win32 framework bridge." OFF)
    set (IPASIM_RUNTIME_ARCH "x64" CACHE STRING "ipaSim runtime host architecture")

    project (ipaSim LANGUAGES C CXX ASM)

    list (APPEND CMAKE_MODULE_PATH "${SOURCE_DIR}/scripts")
    include (CommonVariables)

    if (IPASIM_MODERN_CORE AND NOT IPASIM_RUNTIME_ARCH STREQUAL "x64")
        message (FATAL_ERROR "IPASIM_MODERN_CORE requires IPASIM_RUNTIME_ARCH=x64")
    endif ()

    # Now, the compiler was tested, so we can change it to our patched Clang.
    if (NOT USE_ORIG_CLANG)
        if (USE_DEBUG_CLANG)
            set (CLANG_EXE "${BUILT_DEBUG_CLANG_EXE}")
            set (LLD_LINK_EXE "${BUILT_DEBUG_LLD_LINK_EXE}")
        else (USE_DEBUG_CLANG)
            set (CLANG_EXE "${BUILT_CLANG_EXE}")
            set (LLD_LINK_EXE "${BUILT_LLD_LINK_EXE}")
        endif (USE_DEBUG_CLANG)
    endif (NOT USE_ORIG_CLANG)
    set (CMAKE_C_COMPILER "${CLANG_EXE}")
    set (CMAKE_CXX_COMPILER "${CLANG_EXE}")
    set (CMAKE_LINKER "${LLD_LINK_EXE}")

    # Simplify linker commands so lld-link is used directly.
    macro (update_commands lang)
        set ("CMAKE_${lang}_LINK_EXECUTABLE"
            "<CMAKE_CXX_COMPILER> -fuse-ld=lld-link -g <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> -Wl,/implib:<TARGET_IMPLIB> <LINK_LIBRARIES>")
        set ("CMAKE_${lang}_CREATE_SHARED_LIBRARY"
            "<CMAKE_CXX_COMPILER> -fuse-ld=lld-link -g <CMAKE_SHARED_LIBRARY_CXX_FLAGS> <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> <CMAKE_SHARED_LIBRARY_CREATE_CXX_FLAGS> <SONAME_FLAG><TARGET_SONAME> -o <TARGET> -Wl,/implib:<TARGET_IMPLIB> <OBJECTS> <LINK_LIBRARIES>")
    endmacro ()
    update_commands (C)
    update_commands (CXX)

    if (IPASIM_MODERN_CORE)
        # `add_prep_target()` in the legacy helper also copies WinObjC headers.
        # The modern runtime deliberately has no WinObjC dependency, so define
        # the real preparation target directly and keep its dependency visible.
        add_custom_target (prep
            COMMENT "Prepare x64 ipaSim runtime"
            COMMAND ninja "prep-ipaSim-x64-${CMAKE_BUILD_TYPE}"
            WORKING_DIRECTORY "${BINARY_DIR}"
            USES_TERMINAL)
        option (DEPEND_ON_COMPILER "Rebuild runtime sources when the patched compiler changes." ON)
    else ()
        add_prep_target ("prep-ipaSim-x86-${CMAKE_BUILD_TYPE}")
    endif ()

    if (NOT IPASIM_MODERN_CORE)
        add_custom_target (config-winobjc
            COMMENT "Configure WinObjC"
            COMMAND ninja "config-winobjc-x86-${CMAKE_BUILD_TYPE}"
            WORKING_DIRECTORY "${BINARY_DIR}"
            USES_TERMINAL)
        function (add_woc_lib name)
            add_custom_target ("woc-${name}-build"
                BYPRODUCTS "${CURRENT_WINOBJC_CMAKE_DIR}/bin/${name}.dll"
                    "${CURRENT_WINOBJC_CMAKE_DIR}/bin/${name}.lib"
                COMMENT "WinObjC: ${name}"
                COMMAND ninja "${name}"
                WORKING_DIRECTORY "${CURRENT_WINOBJC_CMAKE_DIR}"
                USES_TERMINAL
                DEPENDS config-winobjc)
            add_library ("woc-${name}" SHARED IMPORTED)
            set_target_properties ("woc-${name}" PROPERTIES
                IMPORTED_LOCATION "${CURRENT_WINOBJC_CMAKE_DIR}/bin/${name}.dll"
                IMPORTED_IMPLIB "${CURRENT_WINOBJC_CMAKE_DIR}/bin/${name}.lib")
        endfunction (add_woc_lib)
        add_woc_lib (WOCStdLib)
        add_woc_lib (Logging)
        add_woc_lib (Starboard)
        add_woc_lib (WinObjCRT)
        add_woc_lib (Foundation)
        add_woc_lib (Accelerate)
        add_woc_lib (UIKit)
        target_include_directories (woc-Logging INTERFACE
            "${SOURCE_DIR}/deps/WinObjC/tools/Logging/include")

        function (add_prebuilt_lib_core name dllname)
            add_library ("${name}" SHARED IMPORTED)
            set_target_properties ("${name}" PROPERTIES
                IMPORTED_LOCATION "${SOURCE_DIR}/deps/WinObjC/deps/prebuilt/Universal Windows/x86/${dllname}.dll"
                IMPORTED_IMPLIB "${SOURCE_DIR}/deps/WinObjC/deps/prebuilt/Universal Windows/x86/${name}.lib")
        endfunction (add_prebuilt_lib_core)
        function (add_prebuilt_lib name)
            add_prebuilt_lib_core ("${name}" "${name}")
        endfunction (add_prebuilt_lib)
        function (add_icu_lib name)
            add_prebuilt_lib_core ("icu${name}" "icu${name}57")
        endfunction (add_icu_lib)
        add_icu_lib (dt)
        add_icu_lib (in)
        add_icu_lib (uc)
        add_prebuilt_lib (libxml2)

        add_library (EGL SHARED IMPORTED)
        set_target_properties (EGL PROPERTIES
            IMPORTED_LOCATION "${ANGLE_DIR}/bin/UAP/Win32/libEGL.dll"
            IMPORTED_IMPLIB "${ANGLE_DIR}/bin/UAP/Win32/libEGL.lib")
        target_include_directories (EGL INTERFACE "${ANGLE_DIR}/Include")
        add_library (GLESv2 SHARED IMPORTED)
        set_target_properties (GLESv2 PROPERTIES
            IMPORTED_LOCATION "${ANGLE_DIR}/bin/UAP/Win32/libGLESv2.dll"
            IMPORTED_IMPLIB "${ANGLE_DIR}/bin/UAP/Win32/libGLESv2.lib")
        target_include_directories (GLESv2 INTERFACE "${ANGLE_DIR}/Include")
    endif ()

    option (FORCE_COLORED_OUTPUT "Always produce ANSI-colored output." OFF)
    if (FORCE_COLORED_OUTPUT)
        add_compile_options (-fcolor-diagnostics -fansi-escape-codes)
    endif (FORCE_COLORED_OUTPUT)

    if (NOT IPASIM_MODERN_CORE)
        add_custom_target (Frameworks)
    endif ()

    # The vendored Unicorn CMake port only builds 32-bit ARM. The modern path
    # consumes the same vendored source tree through an explicit AArch64 build
    # integration in the parent repository; the submodule itself stays intact.
    if (IPASIM_MODERN_CORE)
        include ("${SOURCE_DIR}/scripts/ModernUnicorn.cmake")
    else ()
        add_subdirectory (deps/unicorn)
    endif ()

    add_subdirectory (src)

    if (NOT IPASIM_MODERN_CORE)
        add_subdirectory (deps/WinObjC/deps/3rdparty/cassowary-0.60)
        add_subdirectory (deps/WinObjC/deps/3rdparty/libdispatch)
        add_subdirectory (deps/WinObjC/Frameworks)
    endif ()

endif (SUPERBUILD)
