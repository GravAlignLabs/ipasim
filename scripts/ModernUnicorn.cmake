# Modern x64 Unicorn integration for ipaSim.
#
# The vendored Unicorn CMakeLists.txt is an ipaSim-era Win32 port that builds
# only arm-softmmu/armeb-softmmu and hard-depends on ipaSim's old patched x86
# Clang. The pinned Unicorn source already contains the real AArch64 backends;
# build both endian variants directly without modifying the submodule.

set (UNICORN_DIR "${SOURCE_DIR}/deps/unicorn")
set (UNICORN_WINDOWS_SETJMP_SHIM
    "${CMAKE_CURRENT_LIST_DIR}/UnicornWindowsSetjmp.h")
set (UNICORN_WINDOWS_SETJMP_WRAPPER
    "${CMAKE_CURRENT_LIST_DIR}/UnicornWindowsSetjmp.S")

set (UNICORN_CORE_SOURCES
    ${UNICORN_DIR}/list.c
    ${UNICORN_DIR}/qemu/accel.c
    ${UNICORN_DIR}/qemu/glib_compat.c
    ${UNICORN_DIR}/qemu/hw/core/machine.c
    ${UNICORN_DIR}/qemu/hw/core/qdev.c
    ${UNICORN_DIR}/qemu/qapi/qapi-dealloc-visitor.c
    ${UNICORN_DIR}/qemu/qapi/qapi-visit-core.c
    ${UNICORN_DIR}/qemu/qapi/qmp-input-visitor.c
    ${UNICORN_DIR}/qemu/qapi/qmp-output-visitor.c
    ${UNICORN_DIR}/qemu/qapi/string-input-visitor.c
    ${UNICORN_DIR}/qemu/qemu-log.c
    ${UNICORN_DIR}/qemu/qemu-timer.c
    ${UNICORN_DIR}/qemu/qobject/qbool.c
    ${UNICORN_DIR}/qemu/qobject/qdict.c
    ${UNICORN_DIR}/qemu/qobject/qerror.c
    ${UNICORN_DIR}/qemu/qobject/qfloat.c
    ${UNICORN_DIR}/qemu/qobject/qint.c
    ${UNICORN_DIR}/qemu/qobject/qlist.c
    ${UNICORN_DIR}/qemu/qobject/qstring.c
    ${UNICORN_DIR}/qemu/qom/container.c
    ${UNICORN_DIR}/qemu/qom/cpu.c
    ${UNICORN_DIR}/qemu/qom/object.c
    ${UNICORN_DIR}/qemu/qom/qom-qobject.c
    ${UNICORN_DIR}/qemu/tcg-runtime.c
    ${UNICORN_DIR}/qemu/util/aes.c
    ${UNICORN_DIR}/qemu/util/bitmap.c
    ${UNICORN_DIR}/qemu/util/bitops.c
    ${UNICORN_DIR}/qemu/util/crc32c.c
    ${UNICORN_DIR}/qemu/util/cutils.c
    ${UNICORN_DIR}/qemu/util/error.c
    ${UNICORN_DIR}/qemu/util/getauxval.c
    ${UNICORN_DIR}/qemu/util/host-utils.c
    ${UNICORN_DIR}/qemu/util/module.c
    ${UNICORN_DIR}/qemu/util/oslib-win32.c
    ${UNICORN_DIR}/qemu/util/qemu-error.c
    ${UNICORN_DIR}/qemu/util/qemu-thread-win32.c
    ${UNICORN_DIR}/qemu/util/qemu-timer-common.c
    ${UNICORN_DIR}/qemu/vl.c
    ${UNICORN_DIR}/uc.c)

# These sources are intentionally compiled twice. Unicorn's generated
# aarch64.h/aarch64eb.h headers namespace each backend and select target
# endianness, matching the pinned upstream MSVC projects and Makefile.
set (UNICORN_AARCH64_SOURCES
    ${UNICORN_DIR}/qemu/cpu-exec.c
    ${UNICORN_DIR}/qemu/cpus.c
    ${UNICORN_DIR}/qemu/cputlb.c
    ${UNICORN_DIR}/qemu/exec.c
    ${UNICORN_DIR}/qemu/fpu/softfloat.c
    ${UNICORN_DIR}/qemu/hw/arm/tosa.c
    ${UNICORN_DIR}/qemu/hw/arm/virt.c
    ${UNICORN_DIR}/qemu/ioport.c
    ${UNICORN_DIR}/qemu/memory.c
    ${UNICORN_DIR}/qemu/memory_mapping.c
    ${UNICORN_DIR}/qemu/target-arm/cpu.c
    ${UNICORN_DIR}/qemu/target-arm/cpu64.c
    ${UNICORN_DIR}/qemu/target-arm/crypto_helper.c
    ${UNICORN_DIR}/qemu/target-arm/helper-a64.c
    ${UNICORN_DIR}/qemu/target-arm/helper.c
    ${UNICORN_DIR}/qemu/target-arm/iwmmxt_helper.c
    ${UNICORN_DIR}/qemu/target-arm/neon_helper.c
    ${UNICORN_DIR}/qemu/target-arm/op_helper.c
    ${UNICORN_DIR}/qemu/target-arm/psci.c
    ${UNICORN_DIR}/qemu/target-arm/translate-a64.c
    ${UNICORN_DIR}/qemu/target-arm/translate.c
    ${UNICORN_DIR}/qemu/target-arm/unicorn_aarch64.c
    ${UNICORN_DIR}/qemu/tcg/optimize.c
    ${UNICORN_DIR}/qemu/tcg/tcg.c
    ${UNICORN_DIR}/qemu/translate-all.c)

function (add_unicorn_aarch64_backend target_name target_dir forced_header)
    add_library (${target_name} STATIC ${UNICORN_AARCH64_SOURCES})
    target_include_directories (${target_name} PRIVATE
        ${UNICORN_DIR}/msvc/unicorn/${target_dir}
        ${UNICORN_DIR}/msvc/unicorn
        ${UNICORN_DIR}/qemu
        ${UNICORN_DIR}/qemu/include
        ${UNICORN_DIR}/qemu/tcg
        ${UNICORN_DIR}/qemu/tcg/i386
        ${UNICORN_DIR}/qemu/target-arm
        ${UNICORN_DIR}/include)
    target_compile_definitions (${target_name} PRIVATE
        WIN32
        $<IF:$<CONFIG:Debug>,_DEBUG,NDEBUG>
        _LIB
        __x86_64__
        _CRT_SECURE_NO_WARNINGS
        inline=__inline
        __func__=__FUNCTION__
        NEED_CPU_H
        WIN32_LEAN_AND_MEAN)
    target_compile_options (${target_name} PRIVATE
        -Wno-macro-redefined
        "-include${UNICORN_WINDOWS_SETJMP_SHIM}"
        -include${forced_header})
endfunction ()

add_unicorn_aarch64_backend (
    unicorn-aarch64-softmmu
    aarch64-softmmu
    aarch64.h)
add_unicorn_aarch64_backend (
    unicorn-aarch64eb-softmmu
    aarch64eb-softmmu
    aarch64eb.h)

# The wrapper is linked once into the shared Unicorn DLL. Both endian backend
# archives reference it through the forced compatibility header. The x64 Clang
# assembler emits a native COFF object, avoiding any mutation of the submodule.
add_library (unicorn SHARED
    ${UNICORN_CORE_SOURCES}
    ${UNICORN_WINDOWS_SETJMP_WRAPPER})
target_include_directories (unicorn PRIVATE
    ${UNICORN_DIR}/msvc/unicorn
    ${UNICORN_DIR}/qemu
    ${UNICORN_DIR}/qemu/include
    ${UNICORN_DIR}/qemu/tcg)
target_include_directories (unicorn PUBLIC ${UNICORN_DIR}/include)
target_compile_definitions (unicorn PRIVATE
    WIN32
    $<IF:$<CONFIG:Debug>,_DEBUG,NDEBUG>
    _WINDOWS
    _USRDLL
    UNICORN_DLL_EXPORTS
    UNICORN_SHARED
    _CRT_SECURE_NO_WARNINGS
    inline=__inline
    __func__=__FUNCTION__
    __x86_64__
    UNICORN_HAS_ARM64
    UNICORN_HAS_ARM64EB
    WIN32_LEAN_AND_MEAN)
target_link_libraries (unicorn PRIVATE
    unicorn-aarch64-softmmu
    unicorn-aarch64eb-softmmu)

if (NOT EXISTS "${UNICORN_WINDOWS_SETJMP_SHIM}")
    message (FATAL_ERROR
        "Modern ipaSim requires Unicorn Windows setjmp shim: ${UNICORN_WINDOWS_SETJMP_SHIM}")
endif ()
if (NOT EXISTS "${UNICORN_WINDOWS_SETJMP_WRAPPER}")
    message (FATAL_ERROR
        "Modern ipaSim requires Unicorn Windows setjmp wrapper: ${UNICORN_WINDOWS_SETJMP_WRAPPER}")
endif ()

# Fail at configuration time if the vendored source loses either backend we
# rely on. These are real source requirements, not generated markers/fallbacks.
foreach (required_source
        qemu/aarch64.h
        qemu/aarch64eb.h
        qemu/target-arm/cpu64.c
        qemu/target-arm/helper-a64.c
        qemu/target-arm/translate-a64.c
        qemu/target-arm/unicorn_aarch64.c
        msvc/unicorn/aarch64-softmmu/config-target.h
        msvc/unicorn/aarch64eb-softmmu/config-target.h)
    if (NOT EXISTS "${UNICORN_DIR}/${required_source}")
        message (FATAL_ERROR "Modern ipaSim requires Unicorn ARM64 source: ${required_source}")
    endif ()
endforeach ()
