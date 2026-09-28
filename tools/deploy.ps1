# Copies the built mod DLLs and the resource mod (assets\mods\Thief2VR, e.g. the
# invisible first-person arm) into the Thief 2 folder. The resource mod is found
# through "mod_path mods\Thief2VR" in cam_mod.ini.
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
$assets = Join-Path $root 'assets\mods\Thief2VR'
$game_assets = Join-Path $GameDir 'mods\Thief2VR'

if (-not (Test-Path (Join-Path $GameDir 'Thief2.exe'))) {
    throw "Thief2.exe not found in $GameDir"
}

if ($Remove) {
    foreach ($f in $files) {
        $p = Join-Path $GameDir $f
        if (Test-Path $p) { Remove-Item $p; "removed $p" }
    }
    if (Test-Path $game_assets) { Remove-Item $game_assets -Recurse; "removed $game_assets" }
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
if (Test-Path $assets) {
    if (Test-Path $game_assets) { Remove-Item $game_assets -Recurse }
    New-Item -ItemType Directory -Force (Split-Path $game_assets) | Out-Null
    Copy-Item $assets $game_assets -Recurse
    "deployed mods\Thief2VR"
}
