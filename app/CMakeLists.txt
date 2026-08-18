cmake_minimum_required(VERSION 3.27)
project(pico C)
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
    theme.c
    md_view.c
    markdown.c
    richtext.c
    builtins/chat.c
    builtins/composer.c
    builtins/footer.c)

target_include_directories(pico PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(pico PUBLIC raylib md4c)

add_custom_command(
        TARGET pico POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/resources
        ${CMAKE_CURRENT_BINARY_DIR}/resources)
