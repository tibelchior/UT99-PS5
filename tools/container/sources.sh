#!/bin/bash
# Syncs the ut99dc source into the volume (build trees never touch the Mac folder).
source "$(dirname "$0")/common.sh"
log "sources"
mkdir -p "$TREE"
rsync -a --delete --exclude Thirdparty/.git /ut99src/Source /ut99src/Thirdparty "$TREE/"
ls "$TREE"
