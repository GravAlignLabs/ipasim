list (APPEND CMAKE_MODULE_PATH "${SOURCE_DIR}/scripts")
include (CommonVariables)

set (IPASIM_X64_DIR "${BINARY_DIR}/ipaSim-x64-Debug")
file (MAKE_DIRECTORY "${IPASIM_X64_DIR}")
execute_process (
    COMMAND "${CMAKE_COMMAND}" -G Ninja
        -DSUPERBUILD=Off
        -DIPASIM_RUNTIME_ARCH=x64
        -DIPASIM_MODERN_CORE=On
        -DUSE_ORIG_CLANG=On
        "-DCMAKE_C_COMPILER=${CLANG_EXE}"
        -DCMAKE_C_COMPILER_ID=Clang
        "-DCMAKE_CXX_COMPILER=${CLANG_EXE}"
        -DCMAKE_CXX_COMPILER_ID=Clang
        "-DCMAKE_LINKER=${LLD_LINK_EXE}"
        "-DCMAKE_AR=${LLVM_BIN_DIR}/llvm-ar.exe"
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
        "-DSOURCE_DIR=${SOURCE_DIR}"
        "-DBINARY_DIR=${BINARY_DIR}"
        -DCMAKE_EXPORT_COMPILE_COMMANDS=On
        -DCMAKE_BUILD_TYPE=Debug
        "-DCMAKE_C_FLAGS=-m64 -gcodeview"
        "-DCMAKE_CXX_FLAGS=-m64 -gcodeview"
        "${SOURCE_DIR}"
    WORKING_DIRECTORY "${IPASIM_X64_DIR}")
