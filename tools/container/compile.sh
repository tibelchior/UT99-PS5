#!/bin/bash
# Cross-compiles the engine (static, no GL driver) for Prospero.
source "$(dirname "$0")/common.sh"
install_crt
B=$OUT/cmake
RADV=$VK/.deps/native/radv-release
mkdir -p "$B"
SDL_INC=$OUT/sdl2-inc   # NSDLDrv includes <SDL2/SDL.h>
mkdir -p "$SDL_INC/SDL2" && cp /ut99sdl/include/*.h "$SDL_INC/SDL2/"
[ -f "$OUT/sdl2/include-config-release/SDL2/SDL_config.h" ] && cp "$OUT/sdl2/include-config-release/SDL2/SDL_config.h" "$SDL_INC/SDL2/SDL_config.h"   # the build's own config
# clang packs the engine headers' GCC_PACK(4) bit-fields tightly (GCC and the game's scripts start a new 4-byte
# unit); -mms-bitfields gives the layout the packages expect (Actor = 664 bytes).
log "configure"
cmake -S "$TREE/Source" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$PS5_SDK_DIR/toolchain/prospero.cmake" \
    -DCMAKE_VERBOSE_MAKEFILE=OFF \
    -DBUILD_STATIC=ON -DBUILD_EDITOR=OFF -DBUILD_DCUTIL=OFF -DBUILD_NULLSOUNDDRV=OFF -DBUILD_NOPENGLESDRV=OFF -DBUILD_NVULKANDRV=ON -DVULKAN_INCLUDE_DIR="$RADV/include" \
    -DSDL2_INCLUDE_DIR="$SDL_INC" -DCMAKE_CXX_FLAGS="-I$SDL_INC/SDL2 -mms-bitfields -DUT99_PS5_PAD" \
    > "$OUT/configure.log" 2>&1 || { tail -40 "$OUT/configure.log"; exit 1; }
log "build"
cmake --build "$B" -- -k 0 > "$OUT/build.log" 2>&1 || { grep -E 'error:|FAILED' "$OUT/build.log" | sort | uniq -c | sort -rn | head -40; echo "see $OUT/build.log (in the volume)"; exit 1; }
find "$B" -name '*.a' | sed "s|$B/||"
