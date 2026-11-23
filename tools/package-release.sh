#!/bin/bash
# Packs the built title into dist/UT99-PS5-v<version>.zip: the title only, with none of the game's files.
#   tools/package-release.sh 1.0.0
# Run ./build.sh title and ./build.sh package first. The zip holds one folder, PPSA99781:
#   eboot.bin, sce_sys/, sce_module/, port/System/UnrealTournament.ini
# The player adds the game's folders (System, Maps, Textures, Sounds, Music) and the two menu packages; see README.md.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
source "$HERE/tools/pins.env"
VERSION=${1:?usage: tools/package-release.sh <version>}
SRC=$HERE/dist/$TITLE_ID
[ -f "$SRC/eboot.bin" ] || { echo "error: no title: run ./build.sh title first" >&2; exit 1; }
[ -f "$SRC/port/System/UnrealTournament.ini" ] || { echo "error: no port ini: run ./build.sh package first" >&2; exit 1; }
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/$TITLE_ID/port/System" "$STAGE/$TITLE_ID/UnrealTournament"
cp "$SRC/eboot.bin" "$STAGE/$TITLE_ID/"
cp -R "$SRC/sce_sys" "$SRC/sce_module" "$STAGE/$TITLE_ID/"
cp "$SRC/port/System/UnrealTournament.ini" "$STAGE/$TITLE_ID/port/System/"
# The two patched menu packages ship with the title; the player adds the game's own files on top of them.
PATCHES=${UT99_PATCHES:-$HERE/../../pc-source/UT99/UT99-Android/app/src/main/assets/ut99_patches/System}
mkdir -p "$STAGE/$TITLE_ID/UnrealTournament/System"
for f in UMenu.u UTMenu.u; do
    [ -f "$PATCHES/$f" ] || { echo "error: $PATCHES/$f missing (set UT99_PATCHES)" >&2; exit 1; }
    cp "$PATCHES/$f" "$STAGE/$TITLE_ID/UnrealTournament/System/"
done
cat > "$STAGE/$TITLE_ID/UnrealTournament/PUT_THE_GAME_FILES_HERE.txt" <<'TXT'
Copy System, Maps, Textures, Sounds and Music from your own Unreal Tournament (v400) into this folder.
When asked about System/UMenu.u and System/UTMenu.u, keep the ones that are already here: they are the patched menus.
TXT
find "$STAGE" -name .DS_Store -delete
OUT=$HERE/dist/UT99-PS5-v$VERSION.zip
rm -f "$OUT"
(cd "$STAGE" && zip -qr "$OUT" "$TITLE_ID")
echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"
unzip -l "$OUT" | tail -n +4 | head -30
