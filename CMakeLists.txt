cmake_minimum_required(VERSION 3.24)
project(blackthorne_reborn LANGUAGES CXX C)

# Link the C/C++ runtime statically on MSVC so the game runs without the
# Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
# stb_image / stb_image_write (public domain): PNGs for art packs and exports.
FetchContent_Declare(
    stb
    URL https://github.com/nothings/stb/archive/a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d.tar.gz
)
FetchContent_MakeAvailable(stb)

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

add_library(btrender STATIC src/render/renderer.cpp src/render/filters.cpp src/render/png.cpp src/render/hd.cpp src/render/effects.cpp)
target_link_libraries(btrender PUBLIC btengine)
target_include_directories(btrender PRIVATE ${stb_SOURCE_DIR})

add_library(btaudio STATIC src/audio/audio.cpp src/audio/music.cpp)
target_link_libraries(btaudio PUBLIC btengine SDL3::SDL3-static)
target_include_directories(btaudio PRIVATE ${tinysoundfont_SOURCE_DIR})
if(WIN32)
    target_link_libraries(btaudio PUBLIC winmm)
endif()

add_executable(blackthorne WIN32 src/game/main.cpp src/game/scene.cpp)
target_link_libraries(blackthorne PRIVATE btrender btaudio SDL3::SDL3-static)
target_include_directories(blackthorne PRIVATE ${CMAKE_SOURCE_DIR}/src)

# The lighting shader (src/render/shaders), compiled to DXIL for Direct3D 12
# with the Windows SDK's dxc. Without it the game runs without the effects.
if(WIN32)
    file(GLOB BT_DXC_CANDIDATES "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64/dxc.exe")
    list(SORT BT_DXC_CANDIDATES ORDER DESCENDING)
    list(GET BT_DXC_CANDIDATES 0 BT_DXC_DEFAULT)
    find_program(BT_DXC NAMES dxc HINTS ${BT_DXC_DEFAULT})
    if(NOT BT_DXC AND BT_DXC_DEFAULT)
        set(BT_DXC ${BT_DXC_DEFAULT})
    endif()
endif()
if(BT_DXC)
    set(BT_GEN ${CMAKE_BINARY_DIR}/gen)
    add_custom_command(
        OUTPUT ${BT_GEN}/light_frag_dxil.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${BT_GEN}
        COMMAND ${BT_DXC} -nologo -T ps_6_0 -E main -Fo ${BT_GEN}/light.frag.dxil ${CMAKE_SOURCE_DIR}/src/render/shaders/light.frag.hlsl
        COMMAND ${CMAKE_COMMAND} -DIN=${BT_GEN}/light.frag.dxil -DOUT=${BT_GEN}/light_frag_dxil.h -DNAME=kLightFragDxil
                -P ${CMAKE_SOURCE_DIR}/cmake/bin2h.cmake
        DEPENDS ${CMAKE_SOURCE_DIR}/src/render/shaders/light.frag.hlsl ${CMAKE_SOURCE_DIR}/cmake/bin2h.cmake
        COMMENT "Compiling the lighting shader")
    target_sources(blackthorne PRIVATE ${BT_GEN}/light_frag_dxil.h)
    target_include_directories(blackthorne PRIVATE ${BT_GEN})
    target_compile_definitions(blackthorne PRIVATE BT_HAVE_DXIL=1)
endif()

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

add_executable(bttrace tests/trace.cpp)
target_link_libraries(bttrace PRIVATE btrender)

add_executable(btexport src/export/main.cpp)
target_link_libraries(btexport PRIVATE btrender)
