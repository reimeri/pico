if(NOT DEFINED PICO_BUILD_DIR OR NOT DEFINED PICO_SOURCE_DIR OR
   NOT DEFINED PICO_STAGE_DIR OR NOT DEFINED PICO_CC OR NOT DEFINED PICO_NM OR
   NOT DEFINED PICO_PREFIX OR NOT DEFINED PICO_BINDIR OR NOT DEFINED PICO_DATADIR)
    message(FATAL_ERROR "install smoke test is missing required -D arguments")
endif()

file(REMOVE_RECURSE "${PICO_STAGE_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${PICO_STAGE_DIR}"
            "${CMAKE_COMMAND}" --install "${PICO_BUILD_DIR}"
    RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed: ${install_result}")
endif()

function(pico_staged_path out directory)
    if(IS_ABSOLUTE "${directory}")
        set("${out}" "${PICO_STAGE_DIR}${directory}" PARENT_SCOPE)
    else()
        set("${out}" "${PICO_STAGE_DIR}${PICO_PREFIX}/${directory}" PARENT_SCOPE)
    endif()
endfunction()

pico_staged_path(staged_bindir "${PICO_BINDIR}")
pico_staged_path(staged_datadir "${PICO_DATADIR}")
set(pico_exe "${staged_bindir}/pico")
set(data_dir "${staged_datadir}/pico")
set(sdk_dir "${data_dir}/sdk/include")
foreach(required
        "${pico_exe}"
        "${data_dir}/resources/Roboto-Regular.ttf"
        "${data_dir}/docs/extend/README.md"
        "${data_dir}/examples/settings.json"
        "${data_dir}/examples/hello.c"
        "${data_dir}/builtins/shell.c"
        "${sdk_dir}/pico/plugin.h"
        "${sdk_dir}/clay/clay.h"
        "${sdk_dir}/raylib.h"
        "${staged_datadir}/applications/io.github.reimeri.pico.desktop"
        "${staged_datadir}/metainfo/io.github.reimeri.pico.appdata.xml"
        "${staged_datadir}/pixmaps/pico.png")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "installed file is missing: ${required}")
    endif()
endforeach()

set(desktop_file "${staged_datadir}/applications/io.github.reimeri.pico.desktop")
file(READ "${desktop_file}" desktop_contents)
if(NOT desktop_contents MATCHES "Exec=pico --no-workspace")
    message(FATAL_ERROR "installed desktop entry does not request workspace-less startup")
endif()

set(extension "${PICO_STAGE_DIR}/install-smoke.so")
set(loader "${PICO_STAGE_DIR}/install-loader")
execute_process(
    COMMAND "${PICO_CC}" -shared -fPIC -std=c99 "-I${sdk_dir}"
            -o "${extension}" "${PICO_SOURCE_DIR}/tests/install_extension.c"
    RESULT_VARIABLE extension_result
    OUTPUT_VARIABLE extension_stdout
    ERROR_VARIABLE extension_stderr)
if(NOT extension_result EQUAL 0)
    message(FATAL_ERROR "installed SDK extension compile failed (${extension_result}):\n${extension_stdout}${extension_stderr}")
endif()

execute_process(
    COMMAND "${PICO_CC}" -std=c99 -rdynamic "-I${sdk_dir}"
            -o "${loader}" "${PICO_SOURCE_DIR}/tests/install_loader.c" -ldl
    RESULT_VARIABLE loader_result
    OUTPUT_VARIABLE loader_stdout
    ERROR_VARIABLE loader_stderr)
if(NOT loader_result EQUAL 0)
    message(FATAL_ERROR "installed SDK loader compile failed (${loader_result}):\n${loader_stdout}${loader_stderr}")
endif()

execute_process(
    COMMAND "${PICO_NM}" -D "${pico_exe}"
    RESULT_VARIABLE symbols_result
    OUTPUT_VARIABLE symbols_stdout
    ERROR_VARIABLE symbols_stderr)
if(NOT symbols_result EQUAL 0 OR NOT symbols_stdout MATCHES "pico_host_add_view")
    message(FATAL_ERROR "installed Pico does not export pico_host_add_view (${symbols_result}):\n${symbols_stdout}${symbols_stderr}")
endif()

execute_process(
    COMMAND "${loader}" "${extension}"
    RESULT_VARIABLE load_result
    OUTPUT_VARIABLE load_stdout
    ERROR_VARIABLE load_stderr)
if(NOT load_result EQUAL 0)
    message(FATAL_ERROR "installed extension load failed (${load_result}):\n${load_stdout}${load_stderr}")
endif()

execute_process(
    COMMAND "${pico_exe}" --help
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_stdout
    ERROR_VARIABLE help_stderr)
if(NOT help_result EQUAL 0 OR NOT "${help_stdout}${help_stderr}" MATCHES "[Uu]sage:")
    message(FATAL_ERROR "installed Pico --help failed (${help_result}):\n${help_stdout}${help_stderr}")
endif()
