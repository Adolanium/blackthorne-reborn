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
# TinySoundFont (MIT, header-only) renders the music through a SoundFont.
FetchContent_Declare(
    tinysoundfont
    URL https://github.com/schellingb/TinySoundFont/archive/853a0a171759f1ddba0de1442133a75912bbeffa.tar.gz
)
FetchContent_MakeAvailable(tinysoundfont)

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

add_library(btrender STATIC src/render/renderer.cpp src/render/filters.cpp)
target_link_libraries(btrender PUBLIC btengine)

add_library(btaudio STATIC src/audio/audio.cpp src/audio/music.cpp)
target_link_libraries(btaudio PUBLIC btengine SDL3::SDL3-static)
target_include_directories(btaudio PRIVATE ${tinysoundfont_SOURCE_DIR})
if(WIN32)
    target_link_libraries(btaudio PUBLIC winmm)
endif()

add_executable(blackthorne WIN32 src/game/main.cpp)
target_link_libraries(blackthorne PRIVATE btrender btaudio SDL3::SDL3-static)

# GeneralUser GS (free SoundFont by S. Christian Collins, see its LICENSE.txt),
# placed next to the game so the music uses it out of the box.
option(BT_FETCH_SOUNDFONT "Download the GeneralUser GS SoundFont" ON)
if(BT_FETCH_SOUNDFONT)
    set(GUGS_URL https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/684543d5e5efaef08d02be50dcda8d552478fa60)
    set(GUGS_DIR ${CMAKE_BINARY_DIR}/soundfont)
    if(NOT EXISTS ${GUGS_DIR}/GeneralUser-GS.sf2)
        file(DOWNLOAD ${GUGS_URL}/GeneralUser-GS.sf2 ${GUGS_DIR}/GeneralUser-GS.sf2
             EXPECTED_HASH SHA256=9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe)
        file(DOWNLOAD ${GUGS_URL}/documentation/LICENSE.txt ${GUGS_DIR}/GeneralUser-GS-LICENSE.txt)
    endif()
    add_custom_command(TARGET blackthorne POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GUGS_DIR}/GeneralUser-GS.sf2 ${GUGS_DIR}/GeneralUser-GS-LICENSE.txt
                $<TARGET_FILE_DIR:blackthorne>)
endif()

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

add_executable(btsoak tests/soak.cpp)
target_link_libraries(btsoak PRIVATE btengine)

add_executable(btprobe tests/probe.cpp)
target_link_libraries(btprobe PRIVATE btengine)

add_executable(btmusicwav tests/musicwav.cpp)
target_link_libraries(btmusicwav PRIVATE btaudio)
