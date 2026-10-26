cmake_minimum_required(VERSION 3.20)
project(RotkPcLab LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GBARECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/gbarecomp" CACHE PATH "Pinned GBARecomp checkout")
set(ROTK_CART_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/generated/cart" CACHE PATH "Private generated cartridge code")
set(GBARECOMP_GENERATED_BIOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/generated/bios" CACHE PATH "Private generated BIOS code")
# Target identity, set by tools/lab.py per recompiled ROM (stock or Expanded).
set(ROTK_EXECUTABLE "RotkPcLab" CACHE STRING "Game executable name")
set(ROTK_GAME_NAME "ROTK PC Lab" CACHE STRING "Window title and game name")
set(ROTK_ROM_SHA1 "ec3d126d37ea6e2ad7030d8a99cf87c6e46c03fd" CACHE STRING "Expected ROM SHA-1")
set(ROTK_ROM_CRC32 "0x7590ebfa" CACHE STRING "Expected ROM CRC32")
set(ROTK_DEBUG_PORT "19852" CACHE STRING "TCP debug port")
# recomp-ui: the pre-boot launcher and its Mods page, where the PC enhancements are chosen.
set(GBARECOMP_RUNTIME_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/recomp-ui" CACHE PATH "recomp-ui checkout")
set(RECOMP_UI_ROOT "${GBARECOMP_RUNTIME_UI_ROOT}" CACHE PATH "recomp-ui checkout")
set(GBARECOMP_ENABLE_MODS ON CACHE BOOL "PC enhancements are a built-in mod package" FORCE)
if(NOT EXISTS "${ROTK_CART_DIR}/dispatch_table.cpp")
    message(FATAL_ERROR "Generate private code first: python tools/lab.py prepare")
endif()
add_subdirectory("${GBARECOMP_ROOT}" framework EXCLUDE_FROM_ALL)
get_target_property(RUNTIME_DEFINITIONS gbarecomp_runtime INTERFACE_COMPILE_DEFINITIONS)
if(NOT "GBARECOMP_HAVE_SDL2" IN_LIST RUNTIME_DEFINITIONS)
    message(FATAL_ERROR "SDL2 was not found; a headless-only stub is not a playable port")
endif()
file(GLOB CART_SOURCES CONFIGURE_DEPENDS "${ROTK_CART_DIR}/*.cpp")
set(ROTK_HOST_INCLUDES
    "${GBARECOMP_ROOT}/src/armv4t"
    "${GBARECOMP_ROOT}/src/gba"
    "${GBARECOMP_ROOT}/src/runtime"
    "${GBARECOMP_ROOT}/src/debug"
    "${ROTK_CART_DIR}")
# Generated cartridge code, shared by the game and the host checks.
add_library(RotkCart OBJECT ${CART_SOURCES})
target_include_directories(RotkCart PRIVATE ${ROTK_HOST_INCLUDES})
target_link_gbarecomp_runtime_stack(RotkCart)

add_executable(${ROTK_EXECUTABLE} src/main.cpp src/widescreen.cpp $<TARGET_OBJECTS:RotkCart>)
target_include_directories(${ROTK_EXECUTABLE} PRIVATE ${ROTK_HOST_INCLUDES})
target_link_gbarecomp_runtime_stack(${ROTK_EXECUTABLE})
gbarecomp_target_link_host_stack(${ROTK_EXECUTABLE} PRIVATE)
target_compile_definitions(${ROTK_EXECUTABLE} PRIVATE
    GBARECOMP_DEFAULT_DEBUG_PORT=${ROTK_DEBUG_PORT}
    GBARECOMP_WINDOW_TITLE="${ROTK_GAME_NAME}"
    ROTK_GAME_NAME="${ROTK_GAME_NAME}"
    ROTK_ROM_SHA1="${ROTK_ROM_SHA1}"
    ROTK_ROM_CRC32=${ROTK_ROM_CRC32}u)

# Launcher in its own library so only launcher_boot.cpp sees the recomp-ui flags.
option(ROTK_LAUNCHER "Build the recomp-ui pre-boot launcher" ON)
if(ROTK_LAUNCHER)
    include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
    add_library(RotkLauncher STATIC src/launcher_boot.cpp)
    target_include_directories(RotkLauncher PRIVATE
        "${GBARECOMP_ROOT}/src/runtime" "${GBARECOMP_ROOT}/src/gba" "${GBARECOMP_ROOT}/src/armv4t" src)
    target_compile_definitions(RotkLauncher PRIVATE GBARECOMP_ENABLE_MODS=1)
    recomp_target_launcher_ui(RotkLauncher CONSOLE gba)
    target_include_directories(RotkLauncher PRIVATE "${SDL2_INCLUDE_DIR}")
    target_link_libraries(RotkLauncher PUBLIC "${SDL2_LIBRARY}")
    if(WIN32)
        target_link_libraries(RotkLauncher PUBLIC opengl32)
    endif()
    target_link_libraries(${ROTK_EXECUTABLE} PRIVATE RotkLauncher)
    target_compile_definitions(${ROTK_EXECUTABLE} PRIVATE ROTK_LAUNCHER=1)
endif()

# Built-in package for the PC enhancements, preloaded next to the executable for this target's ROM.
set(ROTK_MOD_GAME_ID "rotk")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mods/rotk.pc.manifest.toml.in"
    "${CMAKE_CURRENT_BINARY_DIR}/mods/packages/rotk.pc/1.0.0/manifest.toml" @ONLY)
target_compile_definitions(${ROTK_EXECUTABLE} PRIVATE ROTK_MOD_GAME_ID="${ROTK_MOD_GAME_ID}")

add_executable(RotkHostRamCheck EXCLUDE_FROM_ALL tests/host_ram_test.cpp)
target_sources(RotkHostRamCheck PRIVATE $<TARGET_OBJECTS:RotkCart>)
target_include_directories(RotkHostRamCheck PRIVATE ${ROTK_HOST_INCLUDES})
target_link_gbarecomp_runtime_stack(RotkHostRamCheck)
gbarecomp_target_link_host_stack(RotkHostRamCheck PRIVATE)

add_executable(RotkAudioRateCheck EXCLUDE_FROM_ALL tests/audio_rate_test.cpp)
target_include_directories(RotkAudioRateCheck PRIVATE "${SDL2_INCLUDE_DIR}")
target_link_libraries(RotkAudioRateCheck PRIVATE "${SDL2_LIBRARY}")
