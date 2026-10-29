#!/bin/bash
# Regenerates the README screenshots in docs/screenshots/ (shred-gui-*.png).
# Runs the real application offscreen in the Breeze style (nothing appears on the screen, no Plasma
# notifications) with demo files in /tmp/shred-gui-demo - which are really shredded - and a temporary
# configuration, so the remembered options and presets of the user are neither shown nor changed. The Dolphin menu screenshot
# (context-menu.png) is taken by hand.
set -euo pipefail
# The times in the log and the dialog do not depend on the time zone of the machine.
export TZ=UTC

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
# Large enough for a few seconds of shredding, so that the speed graph has something to show.
head -c 400M /dev/urandom > "$demo/old-documents/backup-2019.tar"

# Names that are not valid UTF-8 (Latin-1) for the built-in file dialog; not used by the job itself.
names="$demo/Old disk"
mkdir -p "$names/"$'Fr\xfchling' "$names/scans"
for entry in $'caf\xe9.jpg':2310000 $'caf\xe8.jpg':2290000 $'M\xfcnchen station.jpg':3104000 \
             $'Stra\xdfe 12.jpg':1876000 harbour.jpg:2650000 $'na\xefve sketch.png':512000 \
             'résumé.pdf':88000 notes.txt:2400; do
    head -c "${entry##*:}" /dev/zero > "$names/${entry%:*}"
    touch -d '2004-07-14 18:22' "$names/${entry%:*}"
done
touch -d '2004-07-11 09:05' "$names/"$'Fr\xfchling' "$names/scans"

mkdir -p "$repo/docs/screenshots"
# Only the presets of the repository (next to the executable), not the user's own ones; English number
# and date formats, whatever the locale of the user.
SCREENSHOT_DIR="$repo/docs/screenshots" SCREENSHOT_NAMES_DIR="$names" \
    XDG_CONFIG_HOME="$config" XDG_DATA_HOME="$config/data" \
    QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME=kde QT_STYLE_OVERRIDE=Breeze \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
    LC_ALL=en_US.UTF-8 \
    "$build/shred-gui-screenshots" --preset shred-delete "$demo/old-documents" "$demo/passwords-backup.kdbx"

rm -rf -- "$demo"
