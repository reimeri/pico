if(PICO_BUILD_REVISION STREQUAL "" AND EXISTS "${PICO_SOURCE_DIR}/.git")
    find_program(_pico_git git)
    if(_pico_git)
        execute_process(COMMAND "${_pico_git}" -C "${PICO_SOURCE_DIR}" rev-parse --verify HEAD
            OUTPUT_VARIABLE PICO_BUILD_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _pico_git_result ERROR_QUIET)
        if(_pico_git_result EQUAL 0)
            execute_process(COMMAND "${_pico_git}" -C "${PICO_SOURCE_DIR}" diff --quiet HEAD --
                RESULT_VARIABLE _pico_dirty ERROR_QUIET)
            if(_pico_dirty EQUAL 1)
                string(APPEND PICO_BUILD_REVISION "-dirty")
            endif()
        else()
            set(PICO_BUILD_REVISION "")
        endif()
    endif()
endif()

set(PICO_BUILD_VERSION_SUFFIX "")
if(NOT PICO_BUILD_REVISION STREQUAL "")
    if(NOT PICO_BUILD_REVISION MATCHES "^([0-9a-fA-F]+)(-dirty)?$")
        message(FATAL_ERROR "PICO_BUILD_REVISION must be a commit hash, optionally followed by -dirty")
    endif()
    string(SUBSTRING "${CMAKE_MATCH_1}" 0 7 _pico_short_revision)
    if(PICO_BUILD_REVISION MATCHES "-dirty$")
        string(APPEND _pico_short_revision "-dirty")
    endif()
    set(PICO_BUILD_VERSION_SUFFIX " (${_pico_short_revision})")
endif()

configure_file("${CMAKE_CURRENT_LIST_DIR}/pico_build_info.h.in"
    "${PICO_OUTPUT_DIR}/pico_build_info.h" @ONLY)
