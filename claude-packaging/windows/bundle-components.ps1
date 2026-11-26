# Copyright The KiCad Developers, see AUTHORS.txt for contributors.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Windows counterpart of common/bundle-components.sh: put Node.js, the KiCad MCP server and
# its Python dependencies into <KiCadRoot>\claude, and copy the MSVC runtime next to the
# KiCad binaries so that the MSI does not need vc_redist.
#
#   bundle-components.ps1 -KiCadRoot <staged install root containing bin\>

param(
    [Parameter(Mandatory = $true)][string]$KiCadRoot
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$versions = @{}
Get-Content (Join-Path $PSScriptRoot '..\common\versions.env') | ForEach-Object {
    if ($_ -match '^\s*([A-Z_]+)=(.*)$') { $versions[$Matches[1]] = $Matches[2].Trim() }
}

$out = Join-Path $KiCadRoot 'claude'
$bin = Join-Path $KiCadRoot 'bin'
$py = Join-Path $bin 'python.exe'
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("kicad-claude-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force -Path $out, $work | Out-Null

try {
    # ---- Node.js ----
    $nodeVer = $versions['NODE_VERSION']
    $nodeZip = "node-v$nodeVer-win-x64.zip"
    Write-Host "== Node.js $nodeVer"
    Invoke-WebRequest "https://nodejs.org/dist/v$nodeVer/$nodeZip" -OutFile (Join-Path $work $nodeZip)
    Invoke-WebRequest "https://nodejs.org/dist/v$nodeVer/SHASUMS256.txt" -OutFile (Join-Path $work 'SHASUMS256.txt')
    $expected = (Select-String -Path (Join-Path $work 'SHASUMS256.txt') -Pattern " $([regex]::Escape($nodeZip))$").Line.Split(' ')[0]
    $actual = (Get-FileHash (Join-Path $work $nodeZip) -Algorithm SHA256).Hash.ToLower()
    if ($expected -ne $actual) { throw "Node.js checksum mismatch" }
    Expand-Archive (Join-Path $work $nodeZip) -DestinationPath $work
    $nodeDir = Join-Path $out 'node'
    if (Test-Path $nodeDir) { Remove-Item -Recurse -Force $nodeDir }
    Move-Item (Join-Path $work "node-v$nodeVer-win-x64") $nodeDir
    $node = Join-Path $nodeDir 'node.exe'
    $npm = Join-Path $nodeDir 'npm.cmd'

    # ---- MCP server ----
    $commit = $versions['MCP_SERVER_COMMIT']
    $repo = $versions['MCP_SERVER_REPO']
    Write-Host "== KiCad MCP server $($commit.Substring(0,7))"
    $srv = Join-Path $out 'mcp-server'
    if (Test-Path $srv) { Remove-Item -Recurse -Force $srv }
    git clone -q $repo $srv
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
    git -C $srv -c advice.detachedHead=false checkout -q $commit
    if ($LASTEXITCODE -ne 0) { throw "git checkout failed" }
    Remove-Item -Recurse -Force (Join-Path $srv '.git')

    $env:Path = "$nodeDir;$env:Path"
    Push-Location $srv
    & $npm ci --no-audit --no-fund
    if ($LASTEXITCODE -ne 0) { throw "npm ci failed" }
    & $npm run build
    if ($LASTEXITCODE -ne 0) { throw "npm run build failed" }
    & $npm prune --omit=dev --no-audit --no-fund
    if ($LASTEXITCODE -ne 0) { throw "npm prune failed" }
    foreach ($d in 'tests', 'tests-ts', 'docs', '.github') {
        if (Test-Path $d) { Remove-Item -Recurse -Force $d }
    }
    Pop-Location

    # ---- Python dependencies, built for KiCad's own python ----
    Write-Host "== Python dependencies for $(& $py --version)"
    $pydeps = Join-Path $out 'pydeps'
    if (Test-Path $pydeps) { Remove-Item -Recurse -Force $pydeps }
    & $py -m pip install --disable-pip-version-check --no-warn-script-location `
        --target $pydeps -r (Join-Path $srv 'requirements.txt')
    if ($LASTEXITCODE -ne 0) { throw "pip install failed" }

    # ---- MSVC runtime, app-local ----
    if ($env:VCToolsRedistDir) {
        $crt = Get-ChildItem (Join-Path $env:VCToolsRedistDir 'x64') -Directory |
               Where-Object { $_.Name -like 'Microsoft.VC*.CRT' } | Select-Object -First 1
        if ($crt) {
            Write-Host "== MSVC runtime from $($crt.FullName)"
            Copy-Item (Join-Path $crt.FullName '*.dll') -Destination $bin -Force
        }
    }

    Write-Host "== Bundled components ready in $out"
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
