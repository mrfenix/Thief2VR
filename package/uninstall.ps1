# Thief2VR uninstaller: removes the mod and undoes the game-setting changes the
# installer recorded in thief2vr_install.json. Leaves thief2vr.ini (your VR
# settings) and thief2vr.log.
# Usage: uninstall.ps1 [-GameDir <Thief 2 folder>]
param(
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$blockStart = '; >>> Thief2VR (added by the Thief2VR installer; removed by its uninstaller)'
$blockEnd = '; <<< Thief2VR'

function Fail($msg) {
    Write-Host ""
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

if (-not $GameDir) {
    $parent = Split-Path -Parent $here
    if (Test-Path (Join-Path $parent 'Thief2.exe')) { $GameDir = $parent }
    elseif (Test-Path (Join-Path $here 'Thief2.exe')) { $GameDir = $here }
    else { Fail "Run uninstall.bat from the Thief2VR folder inside your Thief 2 folder (the one with Thief2.exe)." }
}
$GameDir = $GameDir.Trim('"')
if (-not (Test-Path (Join-Path $GameDir 'Thief2.exe'))) { Fail "Thief2.exe not found in '$GameDir'." }

$statePath = Join-Path $GameDir 'thief2vr_install.json'
$state = $null
if (Test-Path $statePath) {
    $state = Get-Content $statePath -Raw | ConvertFrom-Json
} else {
    Write-Host 'WARNING: no install record (thief2vr_install.json); removing the mod files and its cam_ext.cfg block only.' -ForegroundColor Yellow
}

# --- Mod files ------------------------------------------------------------------
$d3d9 = Join-Path $GameDir 'd3d9.dll'
if (Test-Path $d3d9) {
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($d3d9))
    if ($text.Contains('thief2vr.dll')) { Remove-Item $d3d9 }
}
if (Test-Path "$d3d9.pre_vr") {
    Move-Item "$d3d9.pre_vr" $d3d9 -Force
    Write-Host 'Restored the previous d3d9.dll.'
}
Remove-Item (Join-Path $GameDir 'thief2vr.dll') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $GameDir 'thief2vr.pdb') -ErrorAction SilentlyContinue
if (-not $state -or -not $state.loader_was_present) {
    Remove-Item (Join-Path $GameDir 'openxr_loader.dll') -ErrorAction SilentlyContinue
}
Write-Host 'Mod files removed.'

# --- cam_ext.cfg: our block out, commented lines back ----------------------------
$extPath = Join-Path $GameDir 'cam_ext.cfg'
if (Test-Path $extPath) {
    $out = New-Object System.Collections.Generic.List[string]
    $skip = $false
    foreach ($line in @(Get-Content $extPath -Encoding Default)) {
        if ($line -eq $blockStart) { $skip = $true; continue }
        if ($skip) { if ($line -eq $blockEnd) { $skip = $false }; continue }
        if ($line.StartsWith(';Thief2VR: ')) { $out.Add($line.Substring(11)) } else { $out.Add($line) }
    }
    while ($out.Count -gt 0 -and -not $out[$out.Count - 1].Trim()) { $out.RemoveAt($out.Count - 1) }
    Set-Content -Path $extPath -Value $out -Encoding Default
    Write-Host 'cam_ext.cfg restored.'
}

# --- cam.cfg: original values ----------------------------------------------------
$camPath = Join-Path $GameDir 'cam.cfg'
if ($state -and (Test-Path $camPath)) {
    $cam = New-Object System.Collections.Generic.List[string]
    foreach ($line in @(Get-Content $camPath -Encoding Default)) {
        $key = ($line.Trim() -split '\s+', 2)[0]
        $prop = $state.cam_cfg.PSObject.Properties[$key]
        if ($prop) {
            if ($null -ne $prop.Value) { $cam.Add(("$key $($prop.Value)").TrimEnd()) }  # else: we added it; drop it
        } else {
            $cam.Add($line)
        }
    }
    Set-Content -Path $camPath -Value $cam -Encoding Default
    Write-Host 'cam.cfg restored.'
}

Remove-Item $statePath -ErrorAction SilentlyContinue
Write-Host ''
Write-Host 'Thief2VR is uninstalled. Your VR settings (thief2vr.ini) and log were left in the game folder.' -ForegroundColor Green
Write-Host 'You can now delete the Thief2VR folder.'
