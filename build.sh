#!/bin/bash
# Builds Unreal Tournament (UT99 v400) for the PS5 from the local ut99dc source and your own game files.
#   ./build.sh sources     sync the engine source into the build volume
#   ./build.sh compile     cross-compile the engine as static libraries (Prospero toolchain)
# Options: BUILD_VOLUME=<name> (default dirt2ps5-build-data, shared with the DiRT/Motorhead ports)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/tools/pins.env"
STEP=${1:-compile}
IMAGE=dirt2ps5-build VOLUME=${BUILD_VOLUME:-dirt2ps5-build-data} NAME=ut99ps5
SRC=$HERE/../../$UT99_SRC_REL
die() { echo "error: $*" >&2; exit 1; }
docker info > /dev/null 2>&1 || die "Docker is not running."
docker image inspect "$IMAGE" > /dev/null 2>&1 || docker build -q -t "$IMAGE" "$HERE/docker" > /dev/null
docker rm -f "$NAME" > /dev/null 2>&1 || true
mkdir -p "$HERE/dist"
docker run -d --name "$NAME" -v "$VOLUME":/build -v "$HERE":/work -v "$SRC":/ut99src:ro -v "$SRC/../SDL2":/ut99sdl:ro "$IMAGE" sleep infinity > /dev/null
trap 'docker rm -f "$NAME" > /dev/null 2>&1 || true' EXIT
run() { docker exec -e VOL=/build "$NAME" bash "/work/tools/container/$1.sh"; }
case $STEP in
    sources) run sources ;;
    sdl2) run sdl2 ;;
    compile) run sources; run sdl2; run compile ;;
    title) run sources; run sdl2; run compile || true; run title ;;
    package)
        OUT=$HERE/dist/$TITLE_ID; GAME=${GAME_DIR:-$HERE/../../pc-source/UT99/iso-extract}
        [ -f "$OUT/eboot.bin" ] || die "no title yet: run ./build.sh title first"
        echo "== game files (about 0.7 GB) into $OUT/UnrealTournament"
        mkdir -p "$OUT/UnrealTournament" "$OUT/port/System" "$OUT/data/System" "$OUT/data/Save" "$OUT/data/Cache"
        for d in System Maps Textures Sounds Music; do rsync -a --exclude '.DS_Store' --exclude '*.dll' --exclude '*.exe' --exclude '*.EXE' --exclude '*.ICD' "$GAME/$d/" "$OUT/UnrealTournament/$d/"; done
        # The fork's patched menu packages (UMenu/UTMenu) replace the CD's.
        cp "$HERE/../../pc-source/UT99/UT99-Android/app/src/main/assets/ut99_patches/System/"*.u "$OUT/UnrealTournament/System/"
        python3 "$HERE/tools/make_ini.py" "$HERE/../../$UT99_SRC_REL/Config/System/DCUtil.ini" /app0/UnrealTournament "$OUT/port/System/UnrealTournament.ini"
        echo "Done: $OUT  ->  copy to /data/homebrew/$TITLE_ID on the console" ;;
    *) die "unknown step '$STEP' (sources, sdl2, compile, title, package)" ;;
esac
