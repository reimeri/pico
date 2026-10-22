# Build and execute a tiny consumer to exercise the same metadata dependency
# graph as Pico, including incremental builds that do not reconfigure CMake.
find_package(Git REQUIRED)
set(ENV{GIT_CONFIG_NOSYSTEM} 1)
set(ENV{GIT_CONFIG_GLOBAL} /dev/null)

function(run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${ARGV} failed (${result}):\n${stdout}\n${stderr}")
    endif()
    set(command_output "${stdout}" PARENT_SCOPE)
endfunction()

function(configure revision)
    run("${CMAKE_COMMAND}" -S "${source}/app" -B "${build}"
        -G "${PICO_GENERATOR}" "-DCMAKE_C_COMPILER=${PICO_CC}"
        "-DPICO_BUILD_REVISION=${revision}")
endfunction()

function(expect_version expected)
    run("${CMAKE_COMMAND}" --build "${build}")
    run("${build}/version")
    if(NOT command_output STREQUAL expected)
        message(FATAL_ERROR "Expected '${expected}', got '${command_output}'")
    endif()
endfunction()

set(source "${PICO_TEST_DIR}/source")
set(build "${PICO_TEST_DIR}/build")
file(REMOVE_RECURSE "${PICO_TEST_DIR}")
file(MAKE_DIRECTORY "${source}/app")
set(project [=[
cmake_minimum_required(VERSION 3.27)
project(build_info_test LANGUAGES C)
include("@PICO_CMAKE_DIR@/PicoBuildInfo.cmake")
add_executable(version main.c)
add_dependencies(version pico_build_info)
target_include_directories(version PRIVATE "${_pico_build_info_dir}")
]=])
string(CONFIGURE "${project}" project @ONLY)
file(WRITE "${source}/app/CMakeLists.txt" "${project}")
file(WRITE "${source}/app/main.c" [=[
#include "pico_build_info.h"
#include <stdio.h>
int main(void) { puts("v1.2.3" PICO_BUILD_VERSION_SUFFIX); return 0; }
]=])

# Source archive fallback, and explicit clean/dirty revisions supplied by Nix.
configure("")
expect_version("v1.2.3")
set(supplied_revision "abcdef0123456789abcdef0123456789abcdef01")
configure("${supplied_revision}")
expect_version("v1.2.3 (abcdef0)")
configure("${supplied_revision}-dirty")
expect_version("v1.2.3 (abcdef0-dirty)")

# A normal checkout tracks HEAD and both unstaged and staged changes without
# needing to configure again. Keep the build directory outside the checkout.
run("${GIT_EXECUTABLE}" -C "${source}" init -q)
run("${GIT_EXECUTABLE}" -C "${source}" config user.name "Pico Tests")
run("${GIT_EXECUTABLE}" -C "${source}" config user.email "pico@example.invalid")
run("${GIT_EXECUTABLE}" -C "${source}" config commit.gpgsign false)
run("${GIT_EXECUTABLE}" -C "${source}" add .)
run("${GIT_EXECUTABLE}" -C "${source}" commit -qm "Initial source")
run("${GIT_EXECUTABLE}" -C "${source}" rev-parse HEAD)
string(SUBSTRING "${command_output}" 0 7 short_revision)
configure("")
expect_version("v1.2.3 (${short_revision})")
file(APPEND "${source}/app/main.c" "\n/* Modified source. */\n")
expect_version("v1.2.3 (${short_revision}-dirty)")
run("${GIT_EXECUTABLE}" -C "${source}" add .)
expect_version("v1.2.3 (${short_revision}-dirty)")
run("${GIT_EXECUTABLE}" -C "${source}" commit -qm "Updated source")
run("${GIT_EXECUTABLE}" -C "${source}" rev-parse HEAD)
string(SUBSTRING "${command_output}" 0 7 short_revision)
expect_version("v1.2.3 (${short_revision})")

# Supplied provenance takes precedence over any incidental checkout metadata.
configure("${supplied_revision}")
expect_version("v1.2.3 (abcdef0)")
