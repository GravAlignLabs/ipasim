# Generated semantic-provider execution proof.
#
# This file is included after the IpaSimulator subdirectory has defined the
# generated bridge runtime, the Darwin host DLL, the Win64 libffi machine object,
# and IpaSimLibrary. Keeping this incremental target wiring outside the main
# IpaSimulator CMake file avoids overlapping the active pthread-core work while
# still making the proof part of the normal modern-core dependency graph.

if (NOT IPASIM_MODERN_CORE)
    return ()
endif ()

if (NOT TARGET IpaSimGeneratedBridgeAdapter OR
    NOT TARGET IpaSimDarwinHost OR
    NOT TARGET ffi-win64-machdep OR
    NOT TARGET IpaSimLibrary)
    message (FATAL_ERROR
        "GeneratedSemanticProvider.cmake requires the modern IpaSimulator targets")
endif ()

add_executable (GeneratedSemanticProviderSmoke
    "${CMAKE_CURRENT_LIST_DIR}/GeneratedSemanticProvider.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/GeneratedSemanticProviderSmoke.cpp")
target_compile_options (GeneratedSemanticProviderSmoke PRIVATE -std=c++17)
target_include_directories (GeneratedSemanticProviderSmoke PRIVATE
    "${SOURCE_DIR}/tools/compat_surface/fixtures")
target_sources (GeneratedSemanticProviderSmoke PRIVATE
    $<TARGET_OBJECTS:ffi-win64-machdep>)
# IpaSimGeneratedBridgeAdapter publishes its child-scope imported libffi target
# through its PUBLIC link interface. Do not add a bare `ffi` name from this
# parent directory: imported targets are directory scoped and that would turn
# into an unresolved ffi.lib linker input instead of the configured artifact.
target_link_libraries (GeneratedSemanticProviderSmoke PRIVATE
    IpaSimGeneratedBridgeAdapter)

add_custom_target (GeneratedSemanticProviderCheck
    COMMAND "$<TARGET_FILE:GeneratedSemanticProviderSmoke>"
            "$<TARGET_FILE:IpaSimDarwinHost>"
    DEPENDS GeneratedSemanticProviderSmoke IpaSimDarwinHost
    USES_TERMINAL)

# The proof must execute before the core is accepted. A build/link/runtime
# failure therefore flows through the existing Windows core diagnostic capture
# and fails normally; there is no suppress/fake-green path here.
add_dependencies (IpaSimLibrary GeneratedSemanticProviderCheck)
