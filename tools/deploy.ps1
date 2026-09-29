# Developer deploy: copies the built mod DLLs (and the OpenXR loader) into the
# Thief 2 folder. Players use the release zip's install.bat instead (see
# tools\package.ps1), which also sets up the game's config files.
# Usage: tools\deploy.ps1 [-Configuration Release|Debug] [-GameDir <path>] [-Remove]
param(
    [string]$Configuration = 'Release',
    [string]$GameDir = 'D:\Program Files\Steam\steamapps\common\thief_2',
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build\$Configuration"
$files = @('d3d9.dll', 'thief2vr.dll')

if (-not (Test-Path (Join-Path $GameDir 'Thief2.exe'))) {
    throw "Thief2.exe not found in $GameDir"
}

if ($Remove) {
    foreach ($f in $files + @('thief2vr.pdb')) {
        $p = Join-Path $GameDir $f
        if (Test-Path $p) { Remove-Item $p; "removed $p" }
    }
    return
}

foreach ($f in $files) {
    Copy-Item (Join-Path $build $f) $GameDir -Force
    "deployed $f"
}
$pdb = Join-Path $build 'thief2vr.pdb'
if (Test-Path $pdb) { Copy-Item $pdb $GameDir -Force }
$loader = Join-Path $root 'third_party\openxr\Win32\bin\openxr_loader.dll'
if (Test-Path $loader) { Copy-Item $loader $GameDir -Force; "deployed openxr_loader.dll" }
