# Exercise the real executable's early GLFW initialization failure. It must
# report an unavailable window and exit normally before callbacks/fonts/plugins.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "DISPLAY="
        "WAYLAND_DISPLAY=${CMAKE_CURRENT_BINARY_DIR}/pico-nonexistent-display/wayland"
        "${PICO}" --safe --no-workspace --no-session
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
if(NOT "${result}" STREQUAL "1")
    message(FATAL_ERROR "Expected exit 1 without a display, got ${result}\n${stdout}\n${stderr}")
endif()
if(NOT stderr MATCHES "Pico could not initialize its window")
    message(FATAL_ERROR "Missing window initialization diagnostic\n${stdout}\n${stderr}")
endif()
