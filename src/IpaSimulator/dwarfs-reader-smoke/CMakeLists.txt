cmake_minimum_required(VERSION 3.20)
project(IpaSimPrebuiltDwarfsReaderSmoke LANGUAGES CXX)

# Compile only the current consumer and its assertions. The reader DLL is an
# independently pinned C ABI dependency, not a CMake build dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_EXTENSIONS OFF)
add_executable(DwarfsRuntimeRootStoreSmoke
  ../DwarfsRuntimeRootStore.cpp
  ../DwarfsRuntimeRootStoreSmoke.cpp
)
target_compile_features(DwarfsRuntimeRootStoreSmoke PRIVATE cxx_std_17)
target_compile_definitions(DwarfsRuntimeRootStoreSmoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(DwarfsRuntimeRootStoreSmoke PRIVATE ../../../include)
set_target_properties(DwarfsRuntimeRootStoreSmoke PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
