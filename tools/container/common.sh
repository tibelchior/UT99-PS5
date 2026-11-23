# Shared by the steps that run inside the build container.
set -euo pipefail
source /work/tools/pins.env
VOL=${VOL:-/build}                       # the Docker volume: toolchain, sources, build trees
VK=$VOL/PS5_Vulkan
TREE=${TREE:-$VOL/tree-ut99}   # UT99 source (ut99dc fork) and build scripts
OUT=${OUT:-$VOL/game-ut99}        # build trees and the finished title
export LLVM_CONFIG=/usr/bin/llvm-config
export PS5_OUT=$OUT PS5_SDK_DIR=$VK/.deps/native/ps5-payload-sdk
log() { printf '\n== %s\n' "$*"; }
clone_at() {  # url dir commit
    [ -d "$2/.git" ] || git clone -q "$1" "$2"
    git -C "$2" cat-file -e "$3^{commit}" 2>/dev/null || git -C "$2" fetch -q origin
    git -C "$2" checkout -q --detach "$3"
}
# Clang's x86-64 builtins library, kept in the volume. It has to sit in the compiler's own
# folder, which is part of the container (and is recreated each run), so it is copied in here.
install_crt() {
    [ "$(uname -m)" = x86_64 ] && return 0
    local lib=$VOL/crt-x86/libclang_rt.builtins-x86_64.a d
    [ -f "$lib" ] || { echo "missing $lib: run the toolchain step first" >&2; exit 1; }
    d=$(clang --print-resource-dir)/lib/linux
    mkdir -p "$d" && cp "$lib" "$d/"
}
