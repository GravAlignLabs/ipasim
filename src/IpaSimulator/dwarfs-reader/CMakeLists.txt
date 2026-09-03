cmake_minimum_required(VERSION 3.28)

cmake_policy(SET CMP0091 NEW)
project(IpaSimDwarfsReaderProof LANGUAGES CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(dwarfs 0.15.7 EXACT CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

get_filename_component(IPASIM_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

add_library(IpaSimDwarfsReader SHARED
  "${CMAKE_CURRENT_LIST_DIR}/../DwarfsReaderBridge.cpp"
)
target_compile_features(IpaSimDwarfsReader PRIVATE cxx_std_23)
target_compile_definitions(IpaSimDwarfsReader PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(IpaSimDwarfsReader PRIVATE "${IPASIM_REPO_ROOT}/include")
target_link_libraries(IpaSimDwarfsReader PRIVATE dwarfs::dwarfs_reader nlohmann_json::nlohmann_json)
set_target_properties(IpaSimDwarfsReader PROPERTIES
  OUTPUT_NAME "IpaSimDwarfsReader"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

add_library(IpaSimDwarfsRuntimeRootStore STATIC
  "${CMAKE_CURRENT_LIST_DIR}/../DwarfsRuntimeRootStore.cpp"
)
target_compile_features(IpaSimDwarfsRuntimeRootStore PRIVATE cxx_std_17)
target_compile_definitions(IpaSimDwarfsRuntimeRootStore PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(IpaSimDwarfsRuntimeRootStore PRIVATE "${IPASIM_REPO_ROOT}/include")

add_executable(DwarfsRuntimeRootStoreSmoke
  "${CMAKE_CURRENT_LIST_DIR}/../DwarfsRuntimeRootStoreSmoke.cpp"
)
target_compile_features(DwarfsRuntimeRootStoreSmoke PRIVATE cxx_std_17)
target_include_directories(DwarfsRuntimeRootStoreSmoke PRIVATE "${IPASIM_REPO_ROOT}/include")
target_link_libraries(DwarfsRuntimeRootStoreSmoke PRIVATE IpaSimDwarfsRuntimeRootStore)
add_dependencies(DwarfsRuntimeRootStoreSmoke IpaSimDwarfsReader)
set_target_properties(DwarfsRuntimeRootStoreSmoke PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
