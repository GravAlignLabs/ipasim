if (IPASIM_MODERN_CORE)
    # The modern ARM64 path builds the emulator core as x64. The historical
    # HeadersAnalyzer/WinObjC Objective-C bridge is a separate legacy toolchain
    # and cannot be linked into the same 64-bit runtime process.
    add_subdirectory (IpaSimulator)

    # This follow-on proof consumes targets defined by IpaSimulator, so include
    # it only after that subdirectory is complete. The separate file also keeps
    # this narrow generated-bridge increment out of the active pthread CMake
    # scope while still participating in IpaSimLibrary's normal build checks.
    include ("${CMAKE_CURRENT_SOURCE_DIR}/IpaSimulator/GeneratedSemanticProvider.cmake")
else ()
    add_subdirectory (crt)
    add_subdirectory (HeadersAnalyzer)
    add_subdirectory (IpaSimulator)
    add_subdirectory (objc)
    add_subdirectory (pthread)
    add_subdirectory (RTObjCInterop)
endif ()
