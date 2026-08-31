# Production generated semantic-import routing.
#
# This file is included by src/CMakeLists.txt only after the IpaSimulator
# subdirectory has finished defining IpaSimLibrary, IpaSimGeneratedBridgeAdapter,
# and the generated semantic-provider proof. Keeping this narrow follow-on wiring
# outside src/IpaSimulator/CMakeLists.txt avoids overlapping the active pthread
# implementation scope.

if (NOT IPASIM_MODERN_CORE)
    return ()
endif ()

if (NOT TARGET IpaSimLibrary OR NOT TARGET IpaSimGeneratedBridgeAdapter)
    message (FATAL_ERROR
        "GeneratedSemanticImportRouter.cmake requires the modern ipaSim runtime targets")
endif ()

target_sources (IpaSimLibrary PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/GeneratedSemanticProvider.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/GeneratedSemanticImportRouter.cpp")

target_link_libraries (IpaSimLibrary PRIVATE IpaSimGeneratedBridgeAdapter)
