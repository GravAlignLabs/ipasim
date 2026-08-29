## See [docker-script].

# Run CMake. See i3.
# TODO: When we use CMake v3.13, rewrite this to
# `cmake -G Ninja -S <source_dir> -B <build_dir>`.
mkdir -Force "C:/ipaSim/build" >$null
pushd "C:/ipaSim/build"
cmake -G Ninja "C:/ipaSim/src"

if ($env:BUILD_TABLEGENS_ONLY -eq "1") {
    # Tablegen remains a host build-tool concern and can use the historical
    # compiler-tool target.
    ninja tblgens-x86-Release
} else {
    # ARM64 iOS guests require a 64-bit Windows emulator process. Build the
    # x64 runtime configurations by default; do not silently fall back to the
    # historical Win32 emulator targets.
    ninja ipaSim-x64-Debug ipaSim-x64-Release
}
$ExitCode = $LastExitCode

popd
exit $ExitCode
