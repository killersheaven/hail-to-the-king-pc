#!/bin/sh
# Builds Evil Dead: Hail to the King for Linux from your own disc image.
#   ./build.sh "/path/to/Evil Dead - Hail to the King (USA).cue"
# Needs: python3, cmake, ninja (or make), a C compiler (clang or gcc), SDL2 development files.
# The result goes to pc-build-linux/. No game data is downloaded or included.
set -e
[ -n "$1" ] || { echo "usage: $0 <your disc .cue>"; exit 1; }
CUE=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
cd "$(dirname "$0")"; ROOT=$(pwd); G=$ROOT/games/hail-to-the-king
echo "== 1/4 reading the program from your disc"
case "$CUE" in
  *.BIN|*.bin) mkdir -p "$G/orig"; cp "$CUE" "$G/orig/1ST_READ.BIN";;    # developers: an extracted 1ST_READ.BIN
  *) python3 "$G/tools/extract_disc.py" "$CUE" "$G/orig";;
esac
python3 "$G/tools/make_overlay.py" "$G/orig/1ST_READ.BIN"
echo "== 2/4 translating it to C (a few minutes)"
(cd "$G/tools" && python3 recomp.py ../orig/1ST_READ.BIN ../build/gen >/dev/null)
echo "== 3/4 compiling"
GEN=Ninja; command -v ninja >/dev/null || GEN="Unix Makefiles"
SDLI=$( (sdl2-config --cflags 2>/dev/null || echo) | tr ' ' '\n' | sed -n 's/^-I\(.*\)\/SDL2$/\1/p' | head -1)
cmake -S "$G" -B "$G/build/linux" -G "$GEN" -DCMAKE_BUILD_TYPE=Release -DPLATFORM=sdl \
  -DSDL2_LIB="${SDL2_LIB:-SDL2}" ${SDLI:+-DEXT_INC="$SDLI"} ${EXT_INC:+-DEXT_INC="$EXT_INC"} >/dev/null
cmake --build "$G/build/linux" -j"$(nproc 2>/dev/null || echo 4)"
echo "== 4/4 packaging"
OUT=$ROOT/pc-build-linux; mkdir -p "$OUT"
cp "$G/build/linux/hail-to-the-king" "$OUT/"
cp "$G/dist/README.txt" "$OUT/" 2>/dev/null || true
mkdir -p "$OUT/textures"; cp -r "$G/textures/." "$OUT/textures/"
for m in play:"" options:" --options" launcher:" --launcher"; do
  n=${m%%:*}; a=${m#*:}
  printf '#!/bin/sh\ncd "$(dirname "$0")"\nexec ./hail-to-the-king "%s"%s 2> log.txt\n' "$CUE" "$a" > "$OUT/$n.sh"; chmod +x "$OUT/$n.sh"
done
echo "Done: $OUT  (start launcher.sh)"
