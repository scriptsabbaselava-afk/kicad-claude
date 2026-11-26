# KiCad-Claude

Version modifiée de KiCad 10.0.8 qui ajoute un **panneau de chat Claude** ancré à droite
de l'éditeur de schéma et de l'éditeur de PCB. Claude agit sur KiCad grâce aux outils du
serveur MCP [mixelpixx/KiCAD-MCP-Server](https://github.com/mixelpixx/KiCAD-MCP-Server)
(licence MIT), que le panneau lance lui-même en arrière-plan. Chaque action de Claude
(nom de l'outil et résumé du résultat) s'affiche dans le chat.

Licence : GPL, comme KiCad. Tout le code ajouté est dans la branche `claude-chat`.

## Installer

**macOS** : téléchargez `KiCad-Claude-Installer-macOS.zip` depuis les *Releases*,
décompressez-le, puis dans le Terminal tapez `bash `, glissez
`Installer KiCad Claude.command` dans la fenêtre et appuyez sur Entrée. Le script
télécharge l'application (Node.js, serveur MCP et dépendances Python inclus) et l'installe
dans `/Applications/KiCad-Claude`.

**Windows** : téléchargez et lancez `KiCad-Claude-Windows-x64.msi`.

## Configurer la clé API

1. Ouvrez un projet, puis l'éditeur de schéma ou de PCB.
2. Dans le panneau **Claude**, cliquez sur **Réglages** (ou *Préférences > Claude (IA)*).
3. Collez votre clé API Anthropic (`sk-ant-…`) et validez.

La clé est rangée dans le trousseau du système (Trousseau macOS, Gestionnaire
d'identification Windows, libsecret sous Linux). Elle n'est jamais écrite dans un fichier
ni dans le dépôt git. La même page permet de choisir le modèle (par défaut
`claude-opus-5-5`) et de corriger les chemins de Node.js, du serveur MCP et de Python.

## Utiliser

Écrivez dans le panneau, par exemple :
« ajoute une résistance de 330 ohms entre la broche 1 de J1 et la masse ».

* **Entrée** envoie, **Maj+Entrée** va à la ligne. **Arrêter** interrompt Claude.
* Avant que Claude ne modifie un fichier, le document ouvert est enregistré. Il est
  rechargé automatiquement quand le fichier change sur le disque.
* Claude lance l'ERC (schéma) ou le DRC (PCB) après les modifications importantes et
  résume le résultat.
* L'API IPC de KiCad (*Préférences > Plugins*) est activée automatiquement au premier
  lancement.

## Compiler

### Paquets macOS et Windows (recommandé)

Les workflows GitHub Actions `KiCad-Claude macOS` et `KiCad-Claude Windows` compilent
KiCad avec les outils d'emballage officiels de KiCad (`kicad-mac-builder`,
`kicad-win-builder`), ajoutent les composants Claude (`claude-packaging/`) et produisent
les zips Mac et le MSI. Lancez-les depuis l'onglet *Actions*, ou poussez une étiquette
`v*` pour publier une *Release*.

### À la main (Linux)

```bash
sudo ./install-deps.sh            # dépendances (Ubuntu/Debian)
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DKICAD_BUILD_QA_TESTS=OFF ..
ninja && sudo ninja install
```

Installez ensuite le serveur MCP dans `~/kicad-claude/mcp-server` (`npm ci`) et ses
dépendances Python (`pip install -r requirements.txt`), ou utilisez
`claude-packaging/common/bundle-components.sh`.

## Fichiers ajoutés ou modifiés

| Fichier | Rôle |
|---|---|
| `common/claude_chat/claude_chat_panel.cpp` | Panneau, boucle de dialogue avec l'API Claude (tool use) |
| `common/claude_chat/mcp_client.cpp` | Client MCP (JSON-RPC sur stdio du serveur lancé en sous-processus) |
| `common/claude_chat/claude_chat_config.cpp` | Réglages, chemins par défaut, clé dans le trousseau |
| `common/claude_chat/panel_claude_settings.cpp` | Page *Préférences > Claude (IA)* |
| `eeschema/sch_edit_frame.cpp`, `pcbnew/pcb_edit_frame.cpp` | Ajout du panneau aux éditeurs |
| `common/eda_base_frame.cpp` | Ajout de la page de préférences |
| `claude-packaging/`, `.github/workflows/kicad-claude-*.yml` | Emballage macOS / Windows |
