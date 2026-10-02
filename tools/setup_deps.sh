#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# IMPORTANT:
# Do NOT build a private copy of libnx here. The devkitpro/devkita64 image
# already provides a matching libnx + devkitA64 toolchain. Rebuilding an older
# libnx release against the container's current runtime headers/toolchain can
# fail before our project is even compiled.
#
# libtesla is fetched from its current upstream master so its HID/service code
# matches the current libnx ABI in the container.

mkdir -p "$ROOT/libs" "$ROOT/include/switch"

# Always refresh libtesla. This prevents an old v1.3.3 checkout from surviving
# between CI revisions.
rm -rf "$ROOT/libs/libtesla"
git clone --depth 1 --branch master \
  https://github.com/WerWolv/libtesla.git \
  "$ROOT/libs/libtesla"

echo "libtesla commit: $(git -C "$ROOT/libs/libtesla" rev-parse HEAD)"

# dmnt:cht client is consumed as a small prebuilt static library + header.
# Keep this dependency isolated from libnx: the final link uses the libnx that
# ships with the devkitPro container.
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

git clone --depth 1 \
  https://github.com/Insektaure/Shiny-Stash-Live-Map.git \
  "$TMP/dmnt"

echo "dmnt source commit: $(git -C "$TMP/dmnt" rev-parse HEAD)"

install -m 0644 "$TMP/dmnt/lib/libdmntcht.a" \
  "$ROOT/libs/libdmntcht.a"
install -m 0644 "$TMP/dmnt/include/switch/dmntcht.h" \
  "$ROOT/include/switch/dmntcht.h"

test -s "$ROOT/libs/libdmntcht.a"
test -s "$ROOT/include/switch/dmntcht.h"
test -s "$ROOT/libs/libtesla/include/tesla.hpp"

echo "Installed libnx source: $DEVKITPRO/libnx"
ls -ld "$DEVKITPRO/libnx" "$DEVKITPRO/devkitA64"
echo "Dependencies ready."
