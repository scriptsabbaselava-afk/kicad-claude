#!/usr/bin/env bash
# Copyright The KiCad Developers, see AUTHORS.txt for contributors.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Put Node.js, the KiCad MCP server and its Python dependencies into a folder that ships
# inside the KiCad-Claude package (macOS / Linux).
#
#   bundle-components.sh <output-dir> <kicad-python> <node-platform>
#
#   output-dir     e.g. KiCad.app/Contents/SharedSupport/claude
#   kicad-python   the python3 that ships with this KiCad build (wheels must match it)
#   node-platform  darwin-arm64 | darwin-x64 | linux-x64
set -euo pipefail

OUT="$1"
KICAD_PY="$2"
NODE_PLATFORM="$3"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=versions.env
source "$HERE/versions.env"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "== Node.js $NODE_VERSION ($NODE_PLATFORM)"
NODE_TGZ="node-v${NODE_VERSION}-${NODE_PLATFORM}.tar.gz"
curl -fsSL --retry 4 -o "$WORK/$NODE_TGZ" "https://nodejs.org/dist/v${NODE_VERSION}/${NODE_TGZ}"
curl -fsSL --retry 4 -o "$WORK/SHASUMS256.txt" "https://nodejs.org/dist/v${NODE_VERSION}/SHASUMS256.txt"
( cd "$WORK" && grep " ${NODE_TGZ}\$" SHASUMS256.txt | shasum -a 256 -c - )
rm -rf "$OUT/node"
mkdir -p "$OUT/node"
tar -xzf "$WORK/$NODE_TGZ" -C "$OUT/node" --strip-components 1
rm -rf "$OUT/node/include" "$OUT/node/share"
NODE="$OUT/node/bin/node"
NPM="$OUT/node/bin/npm"

echo "== KiCad MCP server ${MCP_SERVER_COMMIT:0:7}"
rm -rf "$OUT/mcp-server"
mkdir -p "$OUT/mcp-server"
if curl -fsSL --retry 4 -o "$WORK/mcp.tar.gz" "${MCP_SERVER_REPO}/archive/${MCP_SERVER_COMMIT}.tar.gz"; then
    tar -xzf "$WORK/mcp.tar.gz" -C "$OUT/mcp-server" --strip-components 1
else
    echo "   archive download failed, falling back to git"
    git clone -q "$MCP_SERVER_REPO" "$WORK/mcp-git"
    git -C "$WORK/mcp-git" -c advice.detachedHead=false checkout -q "$MCP_SERVER_COMMIT"
    rm -rf "$WORK/mcp-git/.git"
    cp -R "$WORK/mcp-git/." "$OUT/mcp-server/"
fi
(
    cd "$OUT/mcp-server"
    PATH="$OUT/node/bin:$PATH" "$NPM" ci --no-audit --no-fund
    PATH="$OUT/node/bin:$PATH" "$NPM" run build
    PATH="$OUT/node/bin:$PATH" "$NPM" prune --omit=dev --no-audit --no-fund
    rm -rf tests tests-ts docs .github
)

echo "== Python dependencies for $("$KICAD_PY" --version)"
rm -rf "$OUT/pydeps"
"$KICAD_PY" -m pip install --disable-pip-version-check --no-warn-script-location \
    --target "$OUT/pydeps" -r "$OUT/mcp-server/requirements.txt"

echo "== Bundled components ready in $OUT"
du -sh "$OUT"/* || true
