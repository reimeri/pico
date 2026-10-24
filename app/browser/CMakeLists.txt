# CEF's compiler policy stays in this subdirectory, not in Pico's C99 targets.
enable_language(CXX)
set(CEF_ROOT "$ENV{CEF_ROOT}" CACHE PATH "Pinned CEF binary distribution")
if(NOT EXISTS "${CEF_ROOT}/cmake/FindCEF.cmake")
    message(FATAL_ERROR "CEF_ROOT must point to a CEF binary distribution; use nix develop")
endif()
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
if(NOT CMAKE_STRIP)
    message(FATAL_ERROR "A strip tool is required to install the CEF runtime")
endif()
if(NOT EXISTS "${CEF_ROOT}/Resources/locales/en-US.pak")
    message(FATAL_ERROR "The CEF runtime must include the en-US locale")
endif()
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/wrapper")
add_executable(pico_browser_helper helper.cc)
pico_warnings_are_errors(pico_browser_helper)
set_target_properties(pico_browser_helper PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES
    BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/browser/runtime")
target_include_directories(pico_browser_helper SYSTEM PRIVATE "${CEF_ROOT}")
target_compile_definitions(pico_browser_helper PRIVATE
    ${CEF_COMPILER_DEFINES})
target_compile_options(pico_browser_helper PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(pico_browser_helper PRIVATE pico_browser_gpu libcef_dll_wrapper
    "${CEF_ROOT}/Release/libcef.so" Threads::Threads ${CMAKE_DL_LIBS})
# Chromium needs ICU/V8 data beside the executable before CefInitialize.
file(GLOB _cef_runtime "${CEF_ROOT}/Release/*" "${CEF_ROOT}/Resources/*")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/browser/runtime")
foreach(_asset IN LISTS _cef_runtime)
    get_filename_component(_name "${_asset}" NAME)
    file(CREATE_LINK "${_asset}" "${CMAKE_BINARY_DIR}/browser/runtime/${_name}" SYMBOLIC)
endforeach()
install(TARGETS pico_browser_helper RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/pico/browser)
install(DIRECTORY "${CEF_ROOT}/Release/" "${CEF_ROOT}/Resources/"
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/pico/browser
    USE_SOURCE_PERMISSIONS
    DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    PATTERN "locales" EXCLUDE)
# Transform only the installed copy, never the SDK or development symlinks.
configure_file(install_runtime.cmake.in install_runtime.cmake @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/install_runtime.cmake")
install(DIRECTORY "${CEF_ROOT}/Resources/locales/"
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/pico/browser/locales
    USE_SOURCE_PERMISSIONS
    DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    FILES_MATCHING PATTERN "en-US.pak" PATTERN "en-US_*.pak")
install(FILES "${CEF_ROOT}/LICENSE.txt" "${CEF_ROOT}/CREDITS.html"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/pico/cef)
