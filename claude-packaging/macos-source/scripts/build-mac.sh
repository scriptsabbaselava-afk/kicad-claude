#!/bin/bash
# Copyright The KiCad Developers, see AUTHORS.txt for contributors.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Compile KiCad-Claude on this Mac and install it in /Applications/KiCad-Claude.
# Called by INSTALLER-SUR-MAC.command.  Everything happens in ~/kicad-claude-build;
# a second run reuses what was already built and is much faster.
set -euo pipefail

REPO="scriptsabbaselava-afk/kicad-claude"
REPO_URL="https://github.com/${REPO}.git"
REF="${KICAD_CLAUDE_REF:-main}"
KICAD_LIBS_REF="10.0.8"
WORK="$HOME/kicad-claude-build"
DEST="/Applications/KiCad-Claude"

step() { echo; echo ">> $*"; }
die()  { echo; echo "!! $*"; exit 1; }

[[ "$(uname -s)" == "Darwin" ]] || die "Ce script est pour macOS."

# ---- Architecture ----------------------------------------------------------
if [[ "$(sysctl -in hw.optional.arm64 2>/dev/null || echo 0)" == "1" ]]; then
    ARCH="arm64"
    BREW_PREFIX="/opt/homebrew"
else
    ARCH="x86_64"
    BREW_PREFIX="/usr/local"
fi
echo "Processeur : $ARCH"

# ---- Disk space (the build needs about 30 GB) --------------------------------
FREE_GB=$(df -g "$HOME" | awk 'NR==2 {print $4}')
[[ "$FREE_GB" -ge 35 ]] || die "Il faut au moins 35 Go libres sur le disque (il y en a ${FREE_GB})."

# ---- Apple command line tools ------------------------------------------------
step "Vérification des outils de développement Apple"
if ! xcode-select -p >/dev/null 2>&1; then
    xcode-select --install || true
    die "macOS va installer les « outils de ligne de commande ». Acceptez, attendez la fin,
   puis relancez INSTALLER-SUR-MAC.command."
fi

# ---- Homebrew ------------------------------------------------------------------
step "Vérification de Homebrew"
if [[ ! -x "$BREW_PREFIX/bin/brew" ]]; then
    echo "Installation de Homebrew (votre mot de passe Mac peut être demandé)…"
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi
eval "$("$BREW_PREFIX/bin/brew" shellenv)"
export HOMEBREW_NO_ANALYTICS=1 HOMEBREW_NO_AUTO_UPDATE=1

mkdir -p "$WORK"
cd "$WORK"

# ---- Sources ---------------------------------------------------------------
step "Téléchargement de l'outil de compilation officiel de KiCad (kicad-mac-builder)"
if [[ -d mac-builder/.git ]]; then
    git -C mac-builder fetch -q --depth 1 origin 10.0
    git -C mac-builder reset -q --hard FETCH_HEAD
else
    git clone -q --depth 1 --branch 10.0 https://gitlab.com/kicad/packaging/kicad-mac-builder.git mac-builder
fi

step "Téléchargement des scripts KiCad-Claude"
if [[ -d kicad-claude/.git ]]; then
    git -C kicad-claude fetch -q --depth 1 origin "$REF"
    git -C kicad-claude reset -q --hard FETCH_HEAD
else
    git clone -q --depth 1 --branch "$REF" "$REPO_URL" kicad-claude
fi

# ---- Build dependencies --------------------------------------------------------
step "Installation des dépendances (Homebrew)"
# shellcheck source=/dev/null
source mac-builder/ci/src/brew_deps.sh
brew install "${BREW_DEPS[@]}" || brew install "${BREW_DEPS[@]}"

# ---- Build -------------------------------------------------------------------
step "Compilation de KiCad-Claude (la première fois : 1 à 3 heures)"
cd mac-builder
# caffeinate keeps the Mac awake during the build
caffeinate -i ./build.py --arch="$ARCH" \
    --kicad-git-url "$REPO_URL" \
    --kicad-ref "$REF" \
    --symbols-ref "$KICAD_LIBS_REF" \
    --footprints-ref "$KICAD_LIBS_REF" \
    --templates-ref "$KICAD_LIBS_REF" \
    --packages3d-ref "$KICAD_LIBS_REF" \
    --build-type Release \
    --extra-version "claude" \
    --target package-kicad-nightly
cd "$WORK"

DMG=$(ls -t mac-builder/build/dmg/*.dmg 2>/dev/null | head -n 1 || true)
[[ -n "$DMG" ]] || die "La compilation n'a pas produit de DMG."

# ---- Claude components + zip ----------------------------------------------------
step "Ajout du panneau Claude : Node.js, serveur MCP KiCad, dépendances Python"
rm -rf out
kicad-claude/claude-packaging/macos/make-release-zip.sh "$DMG" "$ARCH" "$WORK/out" "$REPO"
ZIP="$WORK/out/KiCad-Claude-macOS-${ARCH}.zip"
[[ -f "$ZIP" ]] || die "Le paquet final n'a pas été créé."

# ---- Install ---------------------------------------------------------------------
step "Installation dans $DEST"
osascript -e 'quit app "KiCad"' 2>/dev/null || true
rm -rf "$DEST"
mkdir -p "$DEST"
ditto -x -k "$ZIP" "$DEST"
xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true
[[ -d "$DEST/KiCad.app" ]] || die "KiCad.app est introuvable après l'installation."

echo
echo "KiCad-Claude est installé dans $DEST"
