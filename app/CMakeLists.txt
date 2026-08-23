cmake_minimum_required(VERSION 3.27)
project(pico VERSION 0.1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 99)

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    raylib
    GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
    GIT_TAG "5.5"
    GIT_PROGRESS TRUE
    GIT_SHALLOW TRUE
)

set(_pico_policy_version_minimum_defined FALSE)
if(DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(_pico_policy_version_minimum_defined TRUE)
    set(_pico_policy_version_minimum "${CMAKE_POLICY_VERSION_MINIMUM}")
endif()
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_MakeAvailable(raylib)
if(_pico_policy_version_minimum_defined)
    set(CMAKE_POLICY_VERSION_MINIMUM "${_pico_policy_version_minimum}")
else()
    unset(CMAKE_POLICY_VERSION_MINIMUM)
endif()
unset(_pico_policy_version_minimum)
unset(_pico_policy_version_minimum_defined)

target_compile_options(raylib PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-Wno-unused-result>)

if(MSVC)
    set(PICO_WARNINGS_AS_ERRORS_OPTION /WX)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(PICO_WARNINGS_AS_ERRORS_OPTION -Werror)
else()
    set(PICO_WARNINGS_AS_ERRORS_OPTION)
endif()

function(pico_warnings_are_errors target)
    if(PICO_WARNINGS_AS_ERRORS_OPTION)
        target_compile_options(${target} PRIVATE ${PICO_WARNINGS_AS_ERRORS_OPTION})
    endif()
endfunction()

add_library(md4c STATIC
    md4c/md4c.c
    md4c/entity.c)
target_include_directories(md4c PUBLIC md4c)

add_executable(pico
    main.c
    app.c
    plugin.c
    theme.c
    md_view.c
    markdown.c
    richtext.c
    json.c
    settings.c
    auth.c
    http.c
    agent.c
    agent_manager.c
    subagent_config.c
    session.c
    usage.c
    chat_sel.c
    builtins/chat.c
    builtins/composer.c
    builtins/footer.c
    builtins/overlay.c
    builtins/ask_user.c
    builtins/todo.c
    builtins/todo_model.c
    builtins/shell.c
    builtins/subagent.c
    builtins/complete.c
    builtins/commands.c
    builtins/files.c
    builtins/openai.c
    builtins/hyper.c
    builtins/hyper_auth.c
    builtins/responses.c
    builtins/extensions.c
    builtins/prompt.c
    third_party/tinyfiledialogs/tinyfiledialogs.c)

set_target_properties(pico PROPERTIES ENABLE_EXPORTS ON)
pico_warnings_are_errors(pico)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

get_filename_component(PICO_DOCS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../docs/extend" ABSOLUTE)
get_filename_component(PICO_USER_DOCS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../docs" ABSOLUTE)

target_include_directories(pico PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyfiledialogs
    ${raylib_SOURCE_DIR}/src/external/glfw/include)

target_compile_definitions(pico PRIVATE
    "PICO_VERSION=\"${PROJECT_VERSION}\""
    "PICO_CC=\"${CMAKE_C_COMPILER}\""
    "PICO_INCLUDE_APP=\"${CMAKE_CURRENT_SOURCE_DIR}\""
    "PICO_INCLUDE_HEADERS=\"${CMAKE_CURRENT_SOURCE_DIR}/include\""
    "PICO_INCLUDE_CLAY=\"${CMAKE_CURRENT_SOURCE_DIR}/..\""
    "PICO_INCLUDE_RAYLIB=\"${raylib_SOURCE_DIR}/src\""
    "PICO_DOCS=\"${PICO_DOCS_DIR}\""
    "PICO_USER_DOCS=\"${PICO_USER_DOCS_DIR}\"")

target_link_libraries(pico PUBLIC raylib md4c CURL::libcurl Threads::Threads ${CMAKE_DL_LIBS})
target_link_options(pico PRIVATE "-rdynamic")

include(CTest)
if(BUILD_TESTING)
    add_executable(pico_json_tests
        tests/json_test.c
        json.c)
    target_include_directories(pico_json_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    pico_warnings_are_errors(pico_json_tests)
    add_test(NAME pico_json_tests COMMAND pico_json_tests)

    add_executable(pico_responses_tests
        tests/responses_test.c
        builtins/responses.c
        builtins/hyper_auth.c
        json.c)
    target_include_directories(pico_responses_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${raylib_SOURCE_DIR}/src
        ${raylib_SOURCE_DIR}/src/external/glfw/include)
    pico_warnings_are_errors(pico_responses_tests)
    target_compile_options(pico_responses_tests PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(pico_responses_tests PRIVATE -Wl,--gc-sections)
    add_test(NAME pico_responses_tests COMMAND pico_responses_tests)

    add_executable(pico_todo_tests
        tests/todo_test.c
        builtins/todo_model.c
        json.c)
    target_include_directories(pico_todo_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    pico_warnings_are_errors(pico_todo_tests)
    add_test(NAME pico_todo_tests COMMAND pico_todo_tests)

    add_executable(pico_ask_user_tests
        tests/ask_user_test.c
        builtins/ask_user.c
        json.c)
    target_include_directories(pico_ask_user_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${raylib_SOURCE_DIR}/src)
    pico_warnings_are_errors(pico_ask_user_tests)
    target_compile_options(pico_ask_user_tests PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(pico_ask_user_tests PRIVATE -Wl,--gc-sections)
    add_test(NAME pico_ask_user_tests COMMAND pico_ask_user_tests)

    set_source_files_properties(tests/agent_manager_test.c PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(tests/subagent_config_test.c PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties(tests/subagent_test.c PROPERTIES HEADER_FILE_ONLY TRUE)
    add_executable(pico_agent_behavior_tests
        tests/agent_behavior_test.c
        tests/agent_manager_test.c
        tests/subagent_config_test.c
        tests/subagent_test.c
        tests/clay_impl.c
        app.c
        agent.c
        agent_manager.c
        subagent_config.c
        builtins/ask_user.c
        builtins/subagent.c
        builtins/todo.c
        builtins/todo_model.c
        json.c
        usage.c)
    target_include_directories(pico_agent_behavior_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${raylib_SOURCE_DIR}/src/external/glfw/include)
    target_compile_definitions(pico_agent_behavior_tests PRIVATE
        "PICO_SOURCE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/..\"")
    pico_warnings_are_errors(pico_agent_behavior_tests)
    target_compile_options(pico_agent_behavior_tests PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(pico_agent_behavior_tests PRIVATE -Wl,--gc-sections)
    target_link_libraries(pico_agent_behavior_tests PRIVATE raylib CURL::libcurl Threads::Threads)
    add_test(NAME pico_agent_behavior_tests COMMAND pico_agent_behavior_tests)

    add_executable(pico_session_usage_tests
        tests/session_usage_test.c
        session.c
        json.c
        usage.c)
    target_include_directories(pico_session_usage_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${raylib_SOURCE_DIR}/src)
    pico_warnings_are_errors(pico_session_usage_tests)
    target_compile_options(pico_session_usage_tests PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(pico_session_usage_tests PRIVATE -Wl,--gc-sections)
    add_test(NAME pico_session_usage_tests COMMAND pico_session_usage_tests)

    add_executable(pico_settings_agent_tests
        tests/settings_agent_test.c
        settings.c
        json.c)
    target_include_directories(pico_settings_agent_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${raylib_SOURCE_DIR}/src)
    pico_warnings_are_errors(pico_settings_agent_tests)
    target_compile_options(pico_settings_agent_tests PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(pico_settings_agent_tests PRIVATE -Wl,--gc-sections)
    add_test(NAME pico_settings_agent_tests COMMAND pico_settings_agent_tests)
endif()

add_custom_command(
        TARGET pico POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/resources
        ${CMAKE_CURRENT_BINARY_DIR}/resources)

foreach(ext hello empty_banner echo_tool ask_tool permit_tool extra_instructions ephemeral_context time_cmd)
    add_custom_command(
            TARGET pico POST_BUILD
            COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -std=c99 ${PICO_WARNINGS_AS_ERRORS_OPTION}
                -I${CMAKE_CURRENT_SOURCE_DIR}/include
                -I${CMAKE_CURRENT_SOURCE_DIR}
                -I${CMAKE_CURRENT_SOURCE_DIR}/..
                -I${raylib_SOURCE_DIR}/src
                -o ${CMAKE_CURRENT_BINARY_DIR}/${ext}_ext.so
                ${CMAKE_CURRENT_SOURCE_DIR}/../examples/${ext}.c
            COMMENT "Compile examples/${ext}.c as a plugin smoke test")
endforeach()
