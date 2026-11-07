#!/bin/sh
# Cross-build the Windows test build (run after ./rebuild.sh has generated build/gen).
# Needs: llvm-mingw (github.com/mstorsjo/llvm-mingw releases), SDL2 mingw dev package,
# Khronos EGL/GLES headers + SDL2 headers in $EXT_INC_WIN.
set -e
cd "$(dirname "$0")/../.."
export LLVM_MINGW="${LLVM_MINGW:-/root/ext/llvm-mingw}"; : "${SDL2_MINGW:=/root/ext/SDL2-2.30.9/x86_64-w64-mingw32}" "${EXT_INC_WIN:=/root/ext/inc-win}"
cmake -S . -B build/win -G Ninja -DCMAKE_TOOLCHAIN_FILE=tools/win/win.cmake -DCMAKE_BUILD_TYPE=Release \
  -DPLATFORM=sdl -DEXT_INC="$EXT_INC_WIN" -DSDL2_LIB="$SDL2_MINGW/lib/libSDL2.dll.a" -DCMAKE_EXE_LINKER_FLAGS=-static >/dev/null
ninja -C build/win -j"$(nproc)" | grep -E "error|FAILED" || true
mkdir -p build/win/package
cp build/win/hail-to-the-king.exe "$SDL2_MINGW/bin/SDL2.dll" build/win/package/
