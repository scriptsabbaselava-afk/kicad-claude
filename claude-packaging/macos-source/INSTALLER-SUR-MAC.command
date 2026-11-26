#!/bin/bash
# KiCad-Claude - installation automatique sur Mac (Apple silicon ou Intel)
# Compile KiCad avec le panneau de chat Claude intégré et l'installe dans
# /Applications/KiCad-Claude.  Logiciel libre (GPL).
set -e
cd "$(dirname "$0")"
clear
echo "=== Installation de KiCad-Claude ==="
echo "La première fois, cela prend 1 à 3 heures et environ 30 Go sur le disque."
echo "Laissez cette fenêtre ouverte et le Mac branché sur secteur."
echo

bash scripts/build-mac.sh 2>&1 | tee "$HOME/Desktop/kicad-claude-installation.log"
[ "${PIPESTATUS[0]}" -eq 0 ] || {
  echo
  echo "!! L'installation a échoué. Le détail est dans le fichier"
  echo "   kicad-claude-installation.log sur votre Bureau."
  exit 1
}

echo
echo "=== Terminé ! KiCad-Claude va s'ouvrir. ==="
echo "Dans l'éditeur de schéma ou de PCB, panneau « Claude » à droite :"
echo "cliquez sur « Réglages » et collez votre clé API Anthropic (sk-ant-...)."
open "/Applications/KiCad-Claude/KiCad.app"
