# Packagers without a Git checkout (notably Nix) supply the source revision.
set(PICO_BUILD_REVISION "" CACHE STRING "Source commit hash, optionally followed by -dirty")
set(_pico_build_info_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_target(pico_build_info
    COMMAND "${CMAKE_COMMAND}"
        "-DPICO_BUILD_REVISION=${PICO_BUILD_REVISION}"
        "-DPICO_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/.."
        "-DPICO_OUTPUT_DIR=${_pico_build_info_dir}"
        -P "${CMAKE_CURRENT_LIST_DIR}/PicoGenerateBuildInfo.cmake"
    BYPRODUCTS "${_pico_build_info_dir}/pico_build_info.h"
    VERBATIM)
# Check on every build so a new commit or tracked edit needs no reconfigure.
# configure_file only touches the header when its contents actually change.
