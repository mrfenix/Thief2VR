# Thief2VR installer. Run install.bat (or this script) from the extracted mod
# folder. Copies the mod into the Thief 2 folder and applies the game settings
# it needs; every change is recorded in thief2vr_install.json so uninstall.ps1
# can undo it.
# Usage: install.ps1 [-GameDir <Thief 2 folder>] [-Force]
param(
    [string]$GameDir,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

# NewDark builds the mod supports (Thief2.exe SHA-256). See core/engine/engine.cpp.
$supported = @{
    'd26342c34624a08a0e5fc8d8c9daf14c676642c328f41c01f548170774aafe8f' = 'NewDark 1.29 (T2Fix 2026-09-16)'
}
$modFiles = @('d3d9.dll', 'thief2vr.dll', 'openxr_loader.dll')
$extKeys = @('force_windowed', 'vsync_mode', 'framerate_cap', 'bob_factor', 'phys_freq', 'use_hi_res_timer')
$blockStart = '; >>> Thief2VR (added by the Thief2VR installer; removed by its uninstaller)'
$blockEnd = '; <<< Thief2VR'
$block = @(
    $blockStart,
    '; windowed: the headset renders at its own resolution, whatever the window size',
    'force_windowed',
    '; the headset paces the frames, not the monitor or a frame cap',
    'vsync_mode 0',
    'framerate_cap 240.0',
    '; comfort and steady movement at VR frame rates',
    'bob_factor 0',
    'phys_freq 60',
    'use_hi_res_timer',
    $blockEnd
)

function Fail($msg) {
    Write-Host ""
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# --- Find the game ---------------------------------------------------------
if (-not $GameDir) {
    if (Test-Path (Join-Path $here 'Thief2.exe')) { $GameDir = $here }
    else { $GameDir = Read-Host 'Path to your Thief 2 folder (the one with Thief2.exe)' }
}
$GameDir = $GameDir.Trim('"')
$exe = Join-Path $GameDir 'Thief2.exe'
if (-not (Test-Path $exe)) { Fail "Thief2.exe not found in '$GameDir'." }
Write-Host "Thief 2 folder: $GameDir"

$hash = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLower()
if ($supported.ContainsKey($hash)) {
    Write-Host "Game build: $($supported[$hash])"
} elseif ($Force) {
    Write-Host "WARNING: unsupported Thief2.exe ($hash). Installing anyway (-Force); VR will stay off in game." -ForegroundColor Yellow
} else {
    Fail ("This Thief2.exe isn't a supported build. Install T2Fix (NewDark 1.29) first: " +
          "https://github.com/Xanfre/T2Fix/releases - then run the installer again.")
}

# --- OpenXR runtime (32-bit) ------------------------------------------------
$runtimeKey = 'HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenXR\1'
$runtime = (Get-ItemProperty -Path $runtimeKey -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime
if ($runtime) {
    Write-Host "32-bit OpenXR runtime: $runtime"
} else {
    Write-Host ("WARNING: no 32-bit OpenXR runtime is registered. Thief 2 is a 32-bit game and needs one; " +
                "Virtual Desktop (VDXR) provides it. Without it the game runs flat.") -ForegroundColor Yellow
}

# --- Install state (originals, for the uninstaller) ------------------------
$statePath = Join-Path $GameDir 'thief2vr_install.json'
if (Test-Path $statePath) {
    $state = Get-Content $statePath -Raw | ConvertFrom-Json
    Write-Host 'Updating an existing install (original settings are kept for uninstall).'
} else {
    $state = [pscustomobject]@{
        version            = ''
        d3d9_backed_up     = $false
        loader_was_present = (Test-Path (Join-Path $GameDir 'openxr_loader.dll'))
        cam_cfg            = [pscustomobject]@{}
    }
}

# --- d3d9.dll: back up another wrapper ---------------------------------------
$d3d9 = Join-Path $GameDir 'd3d9.dll'
if ((Test-Path $d3d9) -and ((Resolve-Path $d3d9).Path -ne (Join-Path $here 'd3d9.dll'))) {
    $bytes = [System.IO.File]::ReadAllBytes($d3d9)
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    if (-not $text.Contains('thief2vr.dll')) {
        $backup = "$d3d9.pre_vr"
        if (-not (Test-Path $backup)) { Copy-Item $d3d9 $backup }
        $state.d3d9_backed_up = $true
        Write-Host "WARNING: another d3d9.dll (e.g. ReShade or dgVoodoo) was in the game folder. It was saved as d3d9.dll.pre_vr and is restored on uninstall." -ForegroundColor Yellow
    }
}

# --- cam_ext.cfg: our block; conflicting lines commented out ----------------
$extPath = Join-Path $GameDir 'cam_ext.cfg'
$ext = @()
if (Test-Path $extPath) { $ext = @(Get-Content $extPath -Encoding Default) }
$out = New-Object System.Collections.Generic.List[string]
$skip = $false
for ($i = 0; $i -lt $ext.Count; $i++) {
    $line = $ext[$i]
    if ($line -eq $blockStart) { $skip = $true; continue }
    if ($skip) { if ($line -eq $blockEnd) { $skip = $false }; continue }
    $key = ($line.Trim() -split '\s+')[0]
    if ($line.Trim() -and -not $line.TrimStart().StartsWith(';') -and $extKeys -contains $key) {
        $out.Add(";Thief2VR: $line")
    } else {
        $out.Add($line)
    }
}
if ($out.Count -gt 0 -and $out[$out.Count - 1].Trim()) { $out.Add('') }
foreach ($l in $block) { $out.Add($l) }
Set-Content -Path $extPath -Value $out -Encoding Default
Write-Host 'cam_ext.cfg: Thief2VR settings applied.'

# --- cam.cfg: windowed, at a size that fits the desktop ---------------------
Add-Type -AssemblyName System.Windows.Forms
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$size = @(@(1600, 900), @(1280, 720), @(1024, 576)) | Where-Object { $_[0] -le $screen.Width -and $_[1] -le $screen.Height } | Select-Object -First 1
if (-not $size) { $size = @(1024, 576) }
$camPath = Join-Path $GameDir 'cam.cfg'
$cam = @()
if (Test-Path $camPath) { $cam = @(Get-Content $camPath -Encoding Default) }
$want = [ordered]@{ game_full_screen = '0'; game_screen_size = "$($size[0]) $($size[1])" }
foreach ($k in $want.Keys) {
    $found = $false
    for ($i = 0; $i -lt $cam.Count; $i++) {
        $parts = $cam[$i].Trim() -split '\s+', 2
        if ($parts[0] -eq $k) {
            if (-not ($state.cam_cfg.PSObject.Properties.Name -contains $k)) {
                $state.cam_cfg | Add-Member -NotePropertyName $k -NotePropertyValue ($(if ($parts.Count -gt 1) { $parts[1] } else { '' }))
            }
            $cam[$i] = "$k $($want[$k])"
            $found = $true
        }
    }
    if (-not $found) {
        if (-not ($state.cam_cfg.PSObject.Properties.Name -contains $k)) {
            $state.cam_cfg | Add-Member -NotePropertyName $k -NotePropertyValue $null
        }
        $cam += "$k $($want[$k])"
    }
}
Set-Content -Path $camPath -Value $cam -Encoding Default
Write-Host "cam.cfg: windowed, $($size[0])x$($size[1]) (the headset renders at its own resolution)."

# --- Copy the mod ------------------------------------------------------------
foreach ($f in $modFiles) {
    $src = Join-Path $here $f
    $dst = Join-Path $GameDir $f
    if (-not (Test-Path $src)) { Fail "$f is missing from the mod folder." }
    if ((Resolve-Path $src).Path -ne [System.IO.Path]::GetFullPath($dst)) { Copy-Item $src $dst -Force }
}
$versionFile = Join-Path $here 'VERSION.txt'
if (Test-Path $versionFile) { $state.version = (Get-Content $versionFile -Raw).Trim() }
$state | ConvertTo-Json -Depth 4 | Set-Content -Path $statePath -Encoding UTF8
Write-Host 'Mod files copied.'

Write-Host ''
Write-Host 'Thief2VR is installed.' -ForegroundColor Green
Write-Host ' - Start your headset streaming (e.g. Virtual Desktop), then launch Thief 2 as usual.'
Write-Host ' - Hold the LEFT MENU button for half a second for the VR menu (settings, controls, recenter).'
Write-Host ' - F8 recenters the view. See README.txt for controls and troubleshooting.'
Write-Host ' - To remove the mod, run uninstall.bat.'
