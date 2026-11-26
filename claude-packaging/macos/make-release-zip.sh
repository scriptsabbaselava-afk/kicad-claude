#!/usr/bin/env bash
# Copyright The KiCad Developers, see AUTHORS.txt for contributors.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Turn the DMG produced by kicad-mac-builder into the KiCad-Claude release files:
#   KiCad-Claude-macOS-<arch>.zip (+ .sha256)  : KiCad.app with the Claude components inside
#   KiCad-Claude-Installer-macOS.zip           : the .command installer the user runs
#
#   make-release-zip.sh <kicad.dmg> <arch: arm64|x86_64> <out-dir> <github owner/repo>
set -euo pipefail

DMG="$1"
ARCH="$2"
OUT="$3"
REPO="$4"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
STAGE="$(mktemp -d)"
MNT="$(mktemp -d)"
trap 'hdiutil detach "$MNT" -quiet 2>/dev/null || true; rm -rf "$STAGE"' EXIT

case "$ARCH" in
    arm64)  NODE_PLATFORM="darwin-arm64" ;;
    x86_64) NODE_PLATFORM="darwin-x64" ;;
    *) echo "unknown arch $ARCH" >&2; exit 1 ;;
esac

echo "== Extracting $DMG"
hdiutil attach "$DMG" -nobrowse -readonly -mountpoint "$MNT" -quiet
# The DMG holds a "KiCad" folder with KiCad.app and the other suite apps.
SRC="$MNT/KiCad"
[[ -d "$SRC/KiCad.app" ]] || SRC="$MNT"
ditto "$SRC" "$STAGE/KiCad"
hdiutil detach "$MNT" -quiet

APP="$STAGE/KiCad/KiCad.app"
[[ -d "$APP" ]] || { echo "KiCad.app not found in DMG" >&2; exit 1; }
PY="$APP/Contents/Frameworks/Python.framework/Versions/Current/bin/python3"

echo "== Bundling Claude components"
"$PY" -m pip --version >/dev/null 2>&1 || "$PY" -m ensurepip --upgrade
"$HERE/../common/bundle-components.sh" "$APP/Contents/SharedSupport/claude" "$PY" "$NODE_PLATFORM"

echo "== Ad-hoc signing"
codesign --force --deep --sign - "$APP"
codesign --verify --deep "$APP"

echo "== Zipping"
ZIP="$OUT/KiCad-Claude-macOS-${ARCH}.zip"
rm -f "$ZIP"
( cd "$STAGE/KiCad" && ditto -c -k --sequesterRsrc . "$ZIP" )
( cd "$OUT" && shasum -a 256 "$(basename "$ZIP")" > "$(basename "$ZIP").sha256" )

echo "== Installer zip"
INST="$(mktemp -d)/KiCad-Claude-Installer"
mkdir -p "$INST"
sed "s#__GITHUB_REPO__#${REPO}#" "$HERE/Installer KiCad Claude.command" > "$INST/Installer KiCad Claude.command"
chmod +x "$INST/Installer KiCad Claude.command"
cp "$HERE/LISEZMOI.txt" "$INST/"
rm -f "$OUT/KiCad-Claude-Installer-macOS.zip"
( cd "$(dirname "$INST")" && ditto -c -k --keepParent "KiCad-Claude-Installer" "$OUT/KiCad-Claude-Installer-macOS.zip" )

ls -lh "$OUT"
