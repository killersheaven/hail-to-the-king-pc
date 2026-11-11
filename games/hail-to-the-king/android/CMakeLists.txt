# Native part of the Android build: SDL2 (shared) + the game as libmain.so.
# Configured by build-android.ps1 with the NDK's toolchain file:
#   -DSDL_SRC=<SDL2 source folder> -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
cmake_minimum_required(VERSION 3.16)
project(hail_to_the_king_android C CXX)
set(SDL_SRC "" CACHE PATH "SDL2 source folder")
if(NOT EXISTS "${SDL_SRC}/CMakeLists.txt")
  message(FATAL_ERROR "SDL_SRC must point at the SDL2 source folder")
endif()
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(${SDL_SRC} SDL)

set(GAME ${CMAKE_CURRENT_SOURCE_DIR}/..)
# our sources include <SDL2/SDL.h>: give them SDL's headers under an SDL2/ folder
file(COPY ${SDL_SRC}/include/ DESTINATION ${CMAKE_BINARY_DIR}/sdlinc/SDL2)

set(RT runtime.c hw.c bios_hle.c disc.c pvr.c dma.c maple.c aica.c aica_synth.c arm7.c coro.c interp.c
       render_gl.c upscale.c png.c textpatch.c iso9660.c sfd.c models.c hdfont.c platform_sdl.c)
list(TRANSFORM RT PREPEND ${GAME}/runtime/)
file(GLOB GEN_SRC ${GAME}/build/gen/*.c)
add_library(main SHARED ${RT} ${GEN_SRC})
target_include_directories(main PRIVATE ${GAME}/runtime ${GAME}/build/gen ${GAME}/third_party/khronos ${CMAKE_BINARY_DIR}/sdlinc)
# wrapping 32-bit arithmetic and type punning, as on the PC; no fused multiply-add, so the
# game's floating-point results match the PC build bit for bit
target_compile_options(main PRIVATE -O2 -fwrapv -fno-strict-aliasing -ffp-contract=off
  -Wno-unused-variable -Wno-unused-label -Wno-tautological-compare)
target_link_libraries(main PRIVATE SDL2 GLESv3 log android dl m)
