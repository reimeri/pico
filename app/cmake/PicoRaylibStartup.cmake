# Raylib 5.5 queries the monitor before checking glfwCreateWindow's result,
# and InitWindow ignores InitPlatform's failure. Patch build-local copies so
# failed display/context creation returns without invoking GLFW or GL on NULL.
# Keep the fetched source immutable (in particular for Nix store sources).
function(pico_patch_raylib_startup)
    set(_src "${raylib_SOURCE_DIR}/src")
    set(_dst "${CMAKE_CURRENT_BINARY_DIR}/pico_raylib")
    file(READ "${_src}/rcore.c" _core)
    file(READ "${_src}/platforms/rcore_desktop_glfw.c" _desktop)

    set(_old "    InitPlatform();")
    string(FIND "${_core}" "${_old}" _match)
    if(_match EQUAL -1)
        message(FATAL_ERROR "Pico failed to patch raylib InitPlatform failure handling")
    endif()
    string(REPLACE "${_old}" [=[    if (InitPlatform() != 0)
    {
        CORE.Window.ready = false;
        TRACELOG(LOG_WARNING, "PLATFORM: Failed to initialize graphics device");
        return;
    }]=] _core "${_core}")

    set(_old "        // After the window was created, determine the monitor that the window manager assigned.")
    string(FIND "${_desktop}" "${_old}" _match)
    if(_match EQUAL -1)
        message(FATAL_ERROR "Pico failed to patch raylib window creation failure handling")
    endif()
    string(REPLACE "${_old}" [=[        if (!platform.handle)
        {
            glfwTerminate();
            TRACELOG(LOG_WARNING, "GLFW: Failed to initialize Window");
            return -1;
        }

        // After the window was created, determine the monitor that the window manager assigned.]=] _desktop "${_desktop}")

    # Preserve the backend's Windows-only relative include after relocation.
    string(REPLACE "\"../external/win32_clipboard.h\""
        "\"${_src}/external/win32_clipboard.h\"" _desktop "${_desktop}")
    string(REPLACE "\"platforms/rcore_desktop_glfw.c\""
        "\"${_dst}/rcore_desktop_glfw.c\"" _core "${_core}")
    file(CONFIGURE OUTPUT "${_dst}/rcore.c" CONTENT "${_core}" @ONLY)
    file(CONFIGURE OUTPUT "${_dst}/rcore_desktop_glfw.c" CONTENT "${_desktop}" @ONLY)
    set_source_files_properties("${_src}/rcore.c"
        TARGET_DIRECTORY raylib PROPERTIES HEADER_FILE_ONLY TRUE)
    target_sources(raylib PRIVATE "${_dst}/rcore.c")
    set_property(SOURCE "${_dst}/rcore.c" TARGET_DIRECTORY raylib
        PROPERTY COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/pico_glfw_scale.h")
endfunction()

pico_patch_raylib_startup()
