#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung
# SPDX-FileContributor: Jan Fuchs <jf [at] simonwunderlich . de>
#
# Builds the XIAO firmware, for the Seeed Studio XIAO ESP32-C5 (8 MB
# flash, 8 MB PSRAM, USB only), into BUILDDIR:
#
#     tools/build_xiao.sh BUILDDIR
#
# The sketch is copied aside with the XIAO values from the comment in
# config.h written in front of its config.h, and partitions_xiao.csv
# as its partitions.csv (Arduino takes the one next to the sketch):
# no Ethernet, two firmware slots, updates from the page over USB.
#
# XIAO_ONE_SLOT=1 builds the option with one firmware slot instead:
# ENABLE_OTA 0 and partitions_xiao_1slot.csv, 4.625 MB LittleFS
# instead of 1.4 MB, new firmware through the chip's download mode
# (flash.html).
#
# USB CDC On Boot stays off: the USB port carries the API, and Serial
# would share it.
# ARDUINO_CLI names arduino-cli if it is not on the PATH; WLS_FLAGS
# adds defines.

set -e

[ $# -eq 1 ] || { echo "usage: $0 BUILDDIR" >&2; exit 1; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$1"
BUILD=$(cd "$1" && pwd)
CLI=${ARDUINO_CLI:-arduino-cli}

SKETCH="$BUILD/sketch/wlanscout"
rm -rf "$BUILD/sketch"
mkdir -p "$SKETCH"

cp "$ROOT"/source/wlanscout/*.ino "$ROOT"/source/wlanscout/*.h "$SKETCH/"

{
    cat <<'EOF'
// XIAO build, written by tools/build_xiao.sh
#define BOARD_NAME      "Seeed Studio XIAO ESP32-C5"
#define BOARD_ID        "xiao"
#define ENABLE_ETHERNET 0
#define STATUS_LED_TYPE LED_PLAIN_LOW
EOF
    if [ "${XIAO_ONE_SLOT:-0}" = 1 ]; then
        echo "#define ENABLE_OTA      0"
    fi
    echo
    cat "$ROOT/source/wlanscout/config.h"
} > "$SKETCH/config.h"

if [ "${XIAO_ONE_SLOT:-0}" = 1 ]; then
    PARTITIONS=partitions_xiao_1slot.csv
else
    PARTITIONS=partitions_xiao.csv
fi

cp "$ROOT/source/wlanscout/$PARTITIONS" "$SKETCH/partitions.csv"

# The builder's home directory stays out of the images
FLAGS="-ffile-prefix-map=$HOME/.arduino15=~/.arduino15"
FLAGS="$FLAGS -ffile-prefix-map=$SKETCH=. -ffile-prefix-map=$BUILD=build"
FLAGS="$FLAGS ${WLS_FLAGS:-}"

# upload.maximum_size: app0 of both XIAO tables, 0x340000
"$CLI" compile \
    --fqbn esp32:esp32:XIAO_ESP32C5:FlashSize=8M,PSRAM=enabled,CDCOnBoot=default \
    --build-property "upload.maximum_size=3407872" \
    --build-property "compiler.c.extra_flags=$FLAGS" \
    --build-property "compiler.cpp.extra_flags=$FLAGS" \
    --build-property "compiler.S.extra_flags=$FLAGS" \
    --build-path "$BUILD" "$SKETCH"
