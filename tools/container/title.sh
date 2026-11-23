#!/bin/bash
# Links the engine libraries, SDL2, the PS5 glue and RADV into an installed title and writes it to /work/dist.
source "$(dirname "$0")/common.sh"
install_crt
SDK=$PS5_SDK_DIR
RADV=$VK/.deps/native/radv-release
native=$VK/tooling/native
tool=$VK/build/host/ps5-native-tool
B=$OUT/cmake
W=$OUT/title-work
GLUE=/work/ps5/glue
mkdir -p "$W/stubs" "$OUT/logs"
for f in "$RADV/lib/libvulkan_radeon.ps5.a" "$tool" "$VK/runtime/libc.prx" "$VK/tools/radv-link.sh" "$OUT/sdl2/libSDL2.a"; do
    [ -e "$f" ] || { echo "missing $f: run the toolchain/sdl2 steps" >&2; exit 2; }
done
cc() { PS5_PAYLOAD_SDK="$SDK" sh "$VK/tooling/prospero-clang18" "$@"; }

log "glue"
for s in ps5_present ps5_launch ps5_io ps5_keyboard; do
    "$SDK/bin/prospero-clang++" -std=gnu++20 -O2 -g -DNDEBUG -Wall -I"$RADV/include" -I"$OUT/sdl2-inc/SDL2" -c "$GLUE/$s.cpp" -o "$W/$s.o" \
        > "$OUT/logs/$s.log" 2>&1 || { cat "$OUT/logs/$s.log"; exit 1; }
done
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$W/app_crt.o"
stub() {
    cc -std=c11 -O2 -fPIC -c "$VK/$2" -o "$W/${1}_stub.o"
    "$SDK/bin/prospero-lld" --shared -soname "${1}.prx" -o "$W/stubs/${1}.so" "$W/${1}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
(cd "$W" && "$SDK/bin/llvm-ar" x "$SDK/target/lib/libc.a" _setjmp.o)
"$SDK/bin/prospero-clang" -O2 -c /work/ps5/ps5_link_compat.c -o "$W/ps5_link_compat.o"
"$SDK/bin/prospero-clang" -O2 -Wall -c /work/ps5/ps5_title_compat.c -o "$W/ps5_title_compat.o"
printf '{\n    local:\n' > "$W/compat-local.map"
for name in fnmatch isatty mkstemp readlink link symlink pathconf setsid vfork getresuid getresgid execl execlp fork gethostbyname strcasestr strdup strndup vasprintf asprintf __wrap_pthread_mutex_lock __wrap_getcwd; do
    printf '        %s;\n' "$name" >> "$W/compat-local.map"
done
printf '};\n' >> "$W/compat-local.map"

log "link"
source "$VK/tools/radv-link.sh"
radv_link_recipe "$VK" "$SDK" "$RADV/lib/libvulkan_radeon.ps5.a" || exit 2
# Unreal's packages register themselves with static initialisers: every package archive is linked whole.
PKGS=("$B"/Core/libCore.a "$B"/Engine/libEngine.a "$B"/Render/libRender.a "$B"/Fire/libFire.a "$B"/IpDrv/libIpDrv.a
    "$B"/Audio/libAudio.a "$B"/NSDLDrv/libNSDLDrv.a "$B"/NVulkanDrv/libNVulkanDrv.a)
LAUNCH=$B/UnrealTournament/CMakeFiles/UnrealTournament.dir/Src/Launch.cpp.o
"$SDK/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --wrap=fread --wrap=ftell --wrap=pthread_mutex_lock --wrap=main --wrap=getcwd \
    --version-script "$native/app-symbols.map" --version-script "$W/compat-local.map" --exclude-libs=ALL \
    -e _start -o "$W/llvm-pie.elf" \
    "$W/app_crt.o" "$W/_setjmp.o" "$W/ps5_title_compat.o" "$W/ps5_present.o" "$W/ps5_launch.o" "$W/ps5_io.o" "$W/ps5_keyboard.o" "$W/ps5_link_compat.o" \
    "$LAUNCH" --whole-archive "${PKGS[@]}" --no-whole-archive "$OUT/sdl2/libSDL2.a" \
    "$W/stubs/libSceAgc.so" "$W/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$SDK"/target/lib/*.so > "$OUT/logs/link-title.log" 2>&1 ||
    { grep -E 'undefined|error|duplicate' "$OUT/logs/link-title.log" | sort | uniq -c | sort -rn | head -40; echo "see $OUT/logs/link-title.log" >&2; exit 1; }

log "imports no module exports"
null_imports=$(comm -23 \
    <("$SDK/bin/llvm-nm" -D --undefined-only "$W/llvm-pie.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for library in "$SDK"/target/lib/*.so "$W"/stubs/*.so; do
        case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$SDK/bin/llvm-nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [ -n "$null_imports" ]; then
    echo "imports that no module a title loads exports (null at run time): $(echo $null_imports)" >&2
    exit 1
fi

log "convert and sign"
"$tool" link --in "$W/llvm-pie.elf" --out "$W/eboot.elf" \
    --stub-dir "$SDK/target/lib" --stub "$W/stubs/libSceAgc.so" --stub "$W/stubs/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf > "$OUT/logs/tool-link.log" 2>&1 ||
    { tail -20 "$OUT/logs/tool-link.log"; exit 1; }
app=$W/$TITLE_ID
rm -rf "$app"; mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$W/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F > "$OUT/logs/tool-sign.log" 2>&1
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
cp /work/ps5/title/param.json "$app/sce_sys/param.json"
if [ -f /work/art/icon.png ] && [ -f /work/art/wallpaper.png ]; then
    (cd "$VK" && bash tools/prepare-assets.sh --icon /work/art/icon.png --background /work/art/wallpaper.png \
        --output-directory "$app/sce_sys") > "$OUT/logs/art.log" 2>&1 || { tail -20 "$OUT/logs/art.log"; exit 1; }
else
    log "placeholder artwork (add art/icon.png and art/wallpaper.png)"
    cp "$VK/sce_sys/icon0.png" "$VK/sce_sys/pic0.dds" "$VK/sce_sys/pic1.dds" "$app/sce_sys/"
fi
rm -f "$app/sce_sys/background-source.png" "$app/sce_sys/launch-background-source.png"
(cd "$VK/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$VK/runtime/libc.prx" "$app/sce_module/libc.prx"
rm -rf "/work/dist/$TITLE_ID" && mkdir -p /work/dist && cp -R "$app" /work/dist/
echo "title: /work/dist/$TITLE_ID ($(stat -c %s "$app/eboot.bin") bytes)"
