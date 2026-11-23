#!/bin/bash
# Builds SDL2 (static, dummy video/audio, virtual joystick) for Prospero, with the PS5 patches.
source "$(dirname "$0")/common.sh"
install_crt
S=$TREE/SDL2
B=$OUT/sdl2
log "sdl2 sources"
rsync -a --delete /ut99sdl/ "$S/"
python3 /work/ps5/patches/sdl2/apply.py "$S"
log "sdl2 configure"
cmake -S "$S" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$PS5_SDK_DIR/toolchain/prospero.cmake" -DCMAKE_VERBOSE_MAKEFILE=OFF \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF \
    -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF -DSDL_RENDER_D3D=OFF -DSDL_METAL=OFF \
    -DSDL_DUMMYVIDEO=ON -DSDL_DUMMYAUDIO=ON -DSDL_DISKAUDIO=OFF -DSDL_OSS=OFF -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF \
    -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_NAS=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF \
    -DSDL_HIDAPI=OFF -DSDL_HAPTIC=OFF -DSDL_SENSOR=OFF -DSDL_VIRTUAL_JOYSTICK=ON -DSDL_LIBSAMPLERATE=OFF -DSDL_CLOCK_GETTIME=ON \
    > "$OUT/sdl2-configure.log" 2>&1 || { tail -40 "$OUT/sdl2-configure.log"; exit 1; }
log "sdl2 build"
cmake --build "$B" -- -k 0 > "$OUT/sdl2-build.log" 2>&1 || { grep -E 'error:|FAILED' "$OUT/sdl2-build.log" | head -30; echo "see $OUT/sdl2-build.log"; exit 1; }
find "$B" -name '*.a'
