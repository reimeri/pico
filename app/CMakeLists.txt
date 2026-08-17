cmake_minimum_required(VERSION 3.27)
project(clay_markdown_reader C)
set(CMAKE_C_STANDARD 99)

# Adding Raylib
include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE) # don't build the supplied examples
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE) # don't build the supplied example games

FetchContent_Declare(
    raylib
    GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
    GIT_TAG "5.5"
    GIT_PROGRESS TRUE
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(raylib)

# Vendored md4c markdown parser (https://github.com/mity/md4c, MIT license)
add_library(md4c STATIC
    md4c/md4c.c
    md4c/entity.c)
target_include_directories(md4c PUBLIC md4c)

add_executable(clay_markdown_reader
    main.c
    markdown.c
    richtext.c)

target_compile_options(clay_markdown_reader PUBLIC)
target_include_directories(clay_markdown_reader PUBLIC .)

target_link_libraries(clay_markdown_reader PUBLIC raylib md4c)
if(MSVC)
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
else()
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
  set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
endif()

add_custom_command(
        TARGET clay_markdown_reader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/resources
        ${CMAKE_CURRENT_BINARY_DIR}/resources)
