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
    src/data/xmidi.cpp
)
target_include_directories(btdata PUBLIC src)

add_library(btengine STATIC
    src/engine/data_segment.cpp
    src/engine/vm.cpp
    src/engine/vm_world.cpp
    src/engine/vm_anim.cpp
    src/engine/vm_actors.cpp
    src/engine/vm_misc.cpp
    src/engine/vm_tick.cpp
    src/engine/game.cpp
)
target_link_libraries(btengine PUBLIC btdata)

add_library(btrender STATIC src/render/renderer.cpp)
target_link_libraries(btrender PUBLIC btengine)

add_library(btaudio STATIC src/audio/audio.cpp src/audio/music.cpp)
target_link_libraries(btaudio PUBLIC btengine SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(btaudio PUBLIC winmm)
endif()

add_executable(blackthorne WIN32 src/game/main.cpp)
target_link_libraries(blackthorne PRIVATE btrender btaudio SDL3::SDL3-static)

add_executable(btviewer src/viewer/main.cpp)
target_link_libraries(btviewer PRIVATE btrender SDL3::SDL3-static)

if(MSVC)
    # /w44456-/w44459: warn when a declaration shadows another one.
    foreach(t btdata btengine btrender btaudio btviewer blackthorne)
        target_compile_options(${t} PRIVATE /W4 /permissive- /w44456 /w44457 /w44458 /w44459)
    endforeach()
endif()

enable_testing()
add_executable(bttests tests/data_tests.cpp)
target_link_libraries(bttests PRIVATE btengine)
add_test(NAME data COMMAND bttests)

add_executable(btvmtests tests/vm_tests.cpp)
target_link_libraries(btvmtests PRIVATE btengine)
add_test(NAME vm COMMAND btvmtests)

add_executable(btprobe tests/probe.cpp)
target_link_libraries(btprobe PRIVATE btengine)
