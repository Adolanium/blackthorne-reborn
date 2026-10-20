cmake_minimum_required(VERSION 3.24)
project(blackthorne_reborn LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
)
FetchContent_MakeAvailable(SDL3)

add_library(btdata STATIC
    src/data/actors.cpp
    src/data/archive.cpp
    src/data/level.cpp
    src/data/sprites.cpp
)
target_include_directories(btdata PUBLIC src)

add_executable(btviewer src/viewer/main.cpp)
target_link_libraries(btviewer PRIVATE btdata SDL3::SDL3-static)

if(MSVC)
    target_compile_options(btdata PRIVATE /W4 /permissive-)
    target_compile_options(btviewer PRIVATE /W4 /permissive-)
endif()

enable_testing()
add_executable(bttests tests/data_tests.cpp)
target_link_libraries(bttests PRIVATE btdata)
add_test(NAME data COMMAND bttests)
