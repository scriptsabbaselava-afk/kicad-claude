# Compiler, installer et lancer KiCad-Claude sur ce PC Windows

Instructions pour Claude Code. Parle français à l'utilisateur. Demande avant d'installer un logiciel.

Ce dépôt = KiCad 10.0.8 + panneau de chat Claude (`common/claude_chat/`). La recette est celle de
`.github/workflows/kicad-claude-windows.yml` ; à reproduire en local :

1. Prérequis : Visual Studio 2022 Build Tools avec « Développement Desktop en C++ »
   (`winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`),
   Git, SDK .NET 8 (`winget install Microsoft.DotNet.SDK.8`). Environ 40 Go libres.
2. À côté de ce dépôt : `git clone --depth 1 https://gitlab.com/kicad/packaging/kicad-win-builder.git win-builder`
3. Créer `win-builder/build-configs/kicad-claude.json` à partir de `kicad-10.0.8.json`, avec
   `sources.kicad.repo = https://github.com/scriptsabbaselava-afk/kicad-claude.git` et `sources.kicad.ref = branch/main`.
4. Dans `win-builder`, avec **Windows PowerShell 5.1 (`powershell.exe`, pas `pwsh`)**, dans cet ordre exact :
   - `./build.ps1 -Vcpkg -Latest -Arch x64 -BuildConfigName kicad-claude`  (avant -Init, sinon CMake est perdu)
   - `./build.ps1 -Init`
   - `./build.ps1 -Build -Arch x64 -BuildConfigName kicad-claude`  (plusieurs heures la 1re fois)
   - `./build.ps1 -PreparePackage -Arch x64 -BuildConfigName kicad-claude -Lite`
5. `claude-packaging/windows/bundle-components.ps1 -KiCadRoot <win-builder>\.out\x64-windows-Release`
6. `dotnet tool install --global wix --version 5.0.2` ; `wix extension add -g WixToolset.UI.wixext/5.0.2` ;
   `wix build claude-packaging/windows/kicad-claude.wxs -arch x64 -ext WixToolset.UI.wixext -d SourceDir=<racine .out> -d Version=10.0.8.1 -d LicenseRtf=claude-packaging/windows/license.rtf -o KiCad-Claude-Windows-x64.msi`
7. Installer (`msiexec /i KiCad-Claude-Windows-x64.msi`), lancer `C:\Program Files\KiCad-Claude\bin\kicad.exe`,
   ouvrir l'éditeur de schéma, vérifier le panneau « Claude » à droite. Réglages : mode « Mon compte Claude »,
   « Vérifier » doit afficher « Connecté ».

Si une étape échoue : lire le log, corriger, réessayer. Après 2-3 échecs sur la même étape, s'arrêter et expliquer.
