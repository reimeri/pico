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

FetchContent_MakeAvailable(raylib)

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
    llm.c
    agent.c
    chat_sel.c
    builtins/chat.c
    builtins/composer.c
    builtins/footer.c
    builtins/overlay.c
    builtins/shell.c)

set_target_properties(pico PROPERTIES ENABLE_EXPORTS ON)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

target_include_directories(pico PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${raylib_SOURCE_DIR}/src/external/glfw/include)

target_compile_definitions(pico PRIVATE
    "PICO_VERSION=\"${PROJECT_VERSION}\""
    "PICO_CC=\"${CMAKE_C_COMPILER}\""
    "PICO_INCLUDE_APP=\"${CMAKE_CURRENT_SOURCE_DIR}\""
    "PICO_INCLUDE_HEADERS=\"${CMAKE_CURRENT_SOURCE_DIR}/include\""
    "PICO_INCLUDE_CLAY=\"${CMAKE_CURRENT_SOURCE_DIR}/..\""
    "PICO_INCLUDE_RAYLIB=\"${raylib_SOURCE_DIR}/src\"")

target_link_libraries(pico PUBLIC raylib md4c CURL::libcurl Threads::Threads ${CMAKE_DL_LIBS})
target_link_options(pico PRIVATE "-rdynamic")

add_custom_command(
        TARGET pico POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/resources
        ${CMAKE_CURRENT_BINARY_DIR}/resources)

add_custom_command(
        TARGET pico POST_BUILD
        COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -std=c99
            -I${CMAKE_CURRENT_SOURCE_DIR}/include
            -I${CMAKE_CURRENT_SOURCE_DIR}
            -I${CMAKE_CURRENT_SOURCE_DIR}/..
            -I${raylib_SOURCE_DIR}/src
            -o ${CMAKE_CURRENT_BINARY_DIR}/hello_ext.so
            ${CMAKE_CURRENT_SOURCE_DIR}/../examples/hello.c
        COMMENT "Compile examples/hello.c as a plugin smoke test")
