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

if (NOT TARGET IpaSimLibrary OR
    NOT TARGET IpaSimGeneratedBridgeAdapter OR
    NOT TARGET IpaSimDarwinHost OR
    NOT TARGET ffi-win64-machdep)
    message (FATAL_ERROR
        "GeneratedSemanticImportRouter.cmake requires the modern ipaSim runtime targets")
endif ()

# IpaSimLibrary is created in the IpaSimulator directory. Do not opt around that
# ownership with CMP0079. Compile the same generated bridge implementation into
# the production library instead: IpaSimLibrary already owns its configured
# libffi dependency and Win64 machine object in the directory where it is
# defined. There is still one bridge implementation source and no second ABI
# table to maintain.
target_sources (IpaSimLibrary PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/GeneratedBridgeAdapter.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/GeneratedSemanticProvider.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/GeneratedSemanticImportRouter.cpp")

add_executable (GeneratedSemanticImportRouterSmoke
    "${CMAKE_CURRENT_LIST_DIR}/GeneratedSemanticProvider.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/GeneratedSemanticImportRouter.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/GeneratedSemanticImportRouterSmoke.cpp")
target_compile_options (GeneratedSemanticImportRouterSmoke PRIVATE -std=c++17)
target_sources (GeneratedSemanticImportRouterSmoke PRIVATE
    $<TARGET_OBJECTS:ffi-win64-machdep>)
target_link_libraries (GeneratedSemanticImportRouterSmoke PRIVATE
    IpaSimGeneratedBridgeAdapter)

add_custom_target (GeneratedSemanticImportRouterCheck
    COMMAND "$<TARGET_FILE:GeneratedSemanticImportRouterSmoke>"
            "$<TARGET_FILE:IpaSimDarwinHost>"
    DEPENDS GeneratedSemanticImportRouterSmoke IpaSimDarwinHost
    USES_TERMINAL)

# A real selection/provider/execution failure is part of the core acceptance
# boundary. Let it print its diagnostic and fail the normal build; CI captures
# that output in the existing single self-updating PR diagnostic comment.
add_dependencies (IpaSimLibrary GeneratedSemanticImportRouterCheck)
