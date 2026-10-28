#!/bin/bash
# Regenerates the README screenshots in docs/screenshots/ (shred-gui-*.png).
# Runs the real application on the current desktop (windows appear for a few seconds) with demo
# files in /tmp/shred-gui-demo - which are really shredded - and a temporary configuration, so the
# remembered options and presets of the user are neither shown nor changed. The Dolphin menu screenshot
# (context-menu.png) is taken by hand.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
build="$repo/build-screenshots"
demo=/tmp/shred-gui-demo
config=$(mktemp -d)
trap 'rm -rf -- "$config"' EXIT

cmake -S "$repo/tools/screenshots" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build" > /dev/null

rm -rf -- "$demo"
mkdir -p "$demo/old-documents/2019" "$demo/old-documents/scans"
for name in 2019/invoice-03.pdf 2019/invoice-04.pdf scans/id-card.png scans/contract.pdf notes.txt; do
    head -c $((20000 + RANDOM)) /dev/urandom > "$demo/old-documents/$name"
done
head -c 4096 /dev/urandom > "$demo/passwords-backup.kdbx"

mkdir -p "$repo/docs/screenshots"
# Only the presets of the repository (next to the executable), not the user's own ones.
SCREENSHOT_DIR="$repo/docs/screenshots" XDG_CONFIG_HOME="$config" XDG_DATA_HOME="$config/data" \
    "$build/shred-gui-screenshots" --preset shred-delete "$demo/old-documents" "$demo/passwords-backup.kdbx"

rm -rf -- "$demo"
