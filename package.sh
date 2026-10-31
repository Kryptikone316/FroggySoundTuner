#!/bin/bash
# Makes dist/FroggySoundTuner-<version>.zip, to be unzipped at the root of the SD card.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
VERSION=$(sed -n 's/^#define ST_VERSION "\(.*\)"/\1/p' "$ROOT/src/froggysoundtuner.c")
[ -f "$ROOT/froggysoundtuner.mipsel" ] || { echo "build it first: SF3500_TC=... ./build.sh"; exit 1; }
STAGE="$ROOT/dist/stage"
rm -rf "$STAGE"; mkdir -p "$STAGE/roms/ports/froggysoundtuner/lang"
cp "$ROOT/pack/FroggySoundTuner.sh" "$STAGE/roms/ports/FroggySoundTuner.sh"
cp "$ROOT/froggysoundtuner.mipsel" "$ROOT/pack/run.sh" "$ROOT/LICENSE" "$ROOT/NOTICE.md" "$STAGE/roms/ports/froggysoundtuner/"
cp "$ROOT/pack/README.txt" "$STAGE/roms/ports/froggysoundtuner/"
cp "$ROOT"/lang/*.json "$STAGE/roms/ports/froggysoundtuner/lang/"
chmod +x "$STAGE/roms/ports/FroggySoundTuner.sh" "$STAGE/roms/ports/froggysoundtuner/run.sh" "$STAGE/roms/ports/froggysoundtuner/froggysoundtuner.mipsel"
(cd "$STAGE" && zip -qr "../FroggySoundTuner-$VERSION.zip" roms)
echo "wrote dist/FroggySoundTuner-$VERSION.zip"
