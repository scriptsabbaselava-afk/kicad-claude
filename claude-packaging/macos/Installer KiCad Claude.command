#!/bin/bash
# Copyright The KiCad Developers, see AUTHORS.txt for contributors.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Installe KiCad-Claude (KiCad + panneau de chat Claude + serveur MCP KiCad) sur macOS.
#
# Utilisation : dans le Terminal, tapez « bash » puis une espace, glissez ce fichier dans
# la fenêtre et appuyez sur Entrée. (Ou clic droit > Ouvrir.)
#
# Tout est téléchargé depuis les « Releases » GitHub du projet : une seule archive qui
# contient KiCad.app avec Node.js, le serveur MCP et ses dépendances Python déjà inclus.
set -euo pipefail

REPO="__GITHUB_REPO__"          # remplacé lors de la création du zip, ex. "moncompte/kicad-claude"
RELEASE="${KICAD_CLAUDE_RELEASE:-latest}"

bold=$'\033[1m'; green=$'\033[32m'; red=$'\033[31m'; dim=$'\033[2m'; reset=$'\033[0m'
step() { echo; echo "${bold}==> $*${reset}"; }
fail() { echo; echo "${red}${bold}Erreur :${reset} $*"; echo; read -r -p "Appuyez sur Entrée pour fermer." _ || true; exit 1; }

echo "${bold}Installation de KiCad-Claude${reset}"
echo "${dim}KiCad 10 avec un panneau de chat Claude intégré (logiciel libre GPL).${reset}"

[[ "$(uname -s)" == "Darwin" ]] || fail "ce script est pour macOS."

case "$(uname -m)" in
    arm64)  ARCH="arm64" ;;
    x86_64) ARCH="x86_64" ;;
    *)      fail "processeur non pris en charge : $(uname -m)" ;;
esac

# Rosetta: a Terminal running under translation reports x86_64 on Apple silicon.
if [[ "$ARCH" == "x86_64" ]] && [[ "$(sysctl -in sysctl.proc_translated 2>/dev/null || echo 0)" == "1" ]]; then
    ARCH="arm64"
fi

ASSET="KiCad-Claude-macOS-${ARCH}.zip"

if [[ "$RELEASE" == "latest" ]]; then
    BASE_URL="https://github.com/${REPO}/releases/latest/download"
else
    BASE_URL="https://github.com/${REPO}/releases/download/${RELEASE}"
fi

# /Applications is writable by administrators; otherwise use ~/Applications.
if [[ -w /Applications ]]; then
    DEST="/Applications/KiCad-Claude"
else
    DEST="$HOME/Applications/KiCad-Claude"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

step "Téléchargement de ${ASSET} (environ 1,5 Go, cela peut prendre quelques minutes)"
curl -fL --retry 4 --progress-bar -o "$TMP/$ASSET" "$BASE_URL/$ASSET" \
    || fail "téléchargement impossible depuis $BASE_URL/$ASSET
Vérifiez votre connexion Internet et que la release existe sur GitHub."

step "Vérification de l'intégrité"
if curl -fsSL --retry 4 -o "$TMP/$ASSET.sha256" "$BASE_URL/$ASSET.sha256"; then
    expected="$(awk '{print $1}' "$TMP/$ASSET.sha256")"
    actual="$(shasum -a 256 "$TMP/$ASSET" | awk '{print $1}')"
    [[ "$expected" == "$actual" ]] || fail "l'archive téléchargée est corrompue (somme SHA-256 différente)."
    echo "${green}OK${reset}"
else
    echo "${dim}(pas de somme de contrôle publiée, vérification ignorée)${reset}"
fi

step "Installation dans ${DEST}"
if [[ -d "$DEST" ]]; then
    if pgrep -f "$DEST/KiCad.app" >/dev/null 2>&1; then
        fail "KiCad-Claude est ouvert. Quittez-le puis relancez ce script."
    fi
    rm -rf "$DEST.old"
    mv "$DEST" "$DEST.old"
fi
mkdir -p "$DEST"
ditto -x -k "$TMP/$ASSET" "$DEST" || fail "décompression impossible."
rm -rf "$DEST.old"

# The bundle is ad-hoc signed, not notarized: remove the download quarantine so that
# Gatekeeper lets it start.
xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true

APP="$DEST/KiCad.app"
[[ -d "$APP" ]] || fail "KiCad.app est introuvable dans l'archive."

step "Vérification des composants inclus"
CLAUDE_DIR="$APP/Contents/SharedSupport/claude"
for f in "$CLAUDE_DIR/node/bin/node" "$CLAUDE_DIR/mcp-server/dist/index.js"; do
    [[ -e "$f" ]] || fail "composant manquant : $f"
done
"$CLAUDE_DIR/node/bin/node" --version >/dev/null || fail "Node.js inclus ne démarre pas."
echo "${green}OK${reset}"

echo
echo "${green}${bold}KiCad-Claude est installé.${reset}"
echo
echo "Ensuite :"
echo "  1. KiCad va s'ouvrir. Ouvrez un projet puis l'éditeur de schéma ou de PCB."
echo "  2. Dans le panneau « Claude » à droite, cliquez sur « Réglages » et collez votre clé"
echo "     API Anthropic (sk-ant-…). Elle est rangée dans le trousseau macOS."
echo "  3. Écrivez votre demande dans le panneau, par exemple :"
echo "     « ajoute une résistance de 330 ohms entre la broche 1 de J1 et la masse »"
echo
open "$APP" || true
read -r -p "Appuyez sur Entrée pour fermer cette fenêtre." _ || true
