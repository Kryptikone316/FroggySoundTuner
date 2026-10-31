#!/bin/bash
# Builds froggysoundtuner.mipsel for the Datafrog SF3500 (MIPS32r2, little endian).
#   SF3500_TC=/path/to/toolchain/host ./build.sh
# The toolchain is a MIPS32r2 GCC (tested with Codescape MIPS MTI GCC 6.3.0) whose sysroot has
# FreeType 2 (headers in usr/include/freetype2).
set -e
: "${SF3500_TC:?set SF3500_TC to the toolchain host directory}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
SYSROOT="$SF3500_TC/mipsel-buildroot-linux-gnu/sysroot"
"$SF3500_TC/bin/mips-mti-linux-gnu-gcc" --sysroot="$SYSROOT" -mips32r2 -mtune=74kc -mhard-float \
    -I"$ROOT/src" -I"$SYSROOT/usr/include/freetype2" -std=gnu99 -O2 -Wall \
    -o "$ROOT/froggysoundtuner.mipsel" \
    "$ROOT/src/froggysoundtuner.c" "$ROOT/src/hwdisp.c" "$ROOT/src/hwaudio.c" "$ROOT/src/joykey.c" \
    -lfreetype -lpthread -ldl -lm
echo "built $ROOT/froggysoundtuner.mipsel"
