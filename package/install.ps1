# Thief2VR installer. The Thief2VR folder goes inside the Thief 2 folder (the
# one with Thief2.exe); run install.bat from there. Copies the mod into the
# game folder and applies the game settings it needs; every change is recorded
# in thief2vr_install.json so uninstall.ps1 can undo it.
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
$extKeys = @('force_windowed', 'vsync_mode', 'framerate_cap', 'bob_factor', 'phys_freq', 'use_hi_res_timer', 'snd3d')
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

# --- Find the game: the folder this Thief2VR folder was placed in ------------
if (-not $GameDir) {
    $parent = Split-Path -Parent $here
    if (Test-Path (Join-Path $parent 'Thief2.exe')) { $GameDir = $parent }
    elseif (Test-Path (Join-Path $here 'Thief2.exe')) { $GameDir = $here }
    else {
        Fail ("The Thief2VR folder isn't inside your Thief 2 folder.`n" +
              "Move the whole Thief2VR folder into the Thief 2 folder (the one with Thief2.exe),`n" +
              "for example ...\steamapps\common\thief_2\Thief2VR, then run install.bat again.")
    }
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
# Thief 2 is 32-bit, so the OpenXR loader uses the 32-bit runtime setting
# (WOW6432Node), not the one 64-bit apps use. If it's missing or broken, offer
# to set it to a 32-bit runtime found on this PC (needs administrator rights).
$runtimeKey = 'HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenXR\1'
$runtimeHint = ("Thief 2 is a 32-bit game, so it uses this 32-bit setting, not the one 64-bit apps use. " +
                "Point it to your headset's 32-bit runtime (e.g. SteamVR: steamxr_win32.json; " +
                "Virtual Desktop: virtualdesktop-openxr-32.json). Without it the game runs flat.")
$runtimeSet = $null  # what the installer set (recorded for the uninstaller's note)

# A runtime manifest's problem, or $null if it and its library are there.
function RuntimeProblem($manifest) {
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { return "the file doesn't exist: $manifest" }
    try {
        $library = (Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json).runtime.library_path
    } catch {
        return "its manifest can't be read: $manifest"
    }
    if ($library -and -not [System.IO.Path]::IsPathRooted($library)) {
        $library = Join-Path (Split-Path -Parent $manifest) $library
    }
    if ($library -and -not (Test-Path -LiteralPath $library -PathType Leaf)) { return "its library is missing: $library" }
    return $null
}

# 32-bit runtimes on this PC, the likeliest first: the 32-bit partner of the
# runtime 64-bit apps use, then SteamVR (any Steam library), Virtual Desktop.
function Find32BitRuntimes {
    $found = New-Object System.Collections.Generic.List[string]
    $active64 = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime
    if ($active64) {
        $partners = @{ 'steamxr_win64.json' = 'steamxr_win32.json'; 'virtualdesktop-openxr.json' = 'virtualdesktop-openxr-32.json' }
        $leaf = Split-Path -Leaf $active64
        if ($partners.ContainsKey($leaf)) { $found.Add((Join-Path (Split-Path -Parent $active64) $partners[$leaf])) }
    }
    $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
    $libraries = @()
    if ($steam) {
        $libraries += $steam
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libraries += $m.Groups[1].Value -replace '\\\\', '\'
            }
        }
    }
    foreach ($lib in $libraries) { $found.Add((Join-Path $lib 'steamapps\common\SteamVR\steamxr_win32.json')) }
    $found.Add((Join-Path $env:ProgramFiles 'Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr-32.json'))
    $seen = @{}
    $usable = @()
    foreach ($f in $found) {
        $full = [System.IO.Path]::GetFullPath($f)
        if (-not $seen.ContainsKey($full.ToLower()) -and -not (RuntimeProblem $full)) {
            $seen[$full.ToLower()] = $true
            $usable += $full
        }
    }
    return ,$usable
}

$runtime = (Get-ItemProperty -Path $runtimeKey -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime
$problem = if ($runtime) { RuntimeProblem $runtime } else { "none is set ($runtimeKey, ActiveRuntime)" }
if (-not $problem) {
    Write-Host "32-bit OpenXR runtime: $runtime"
} else {
    Write-Host "WARNING: no usable 32-bit OpenXR runtime: $problem" -ForegroundColor Yellow
    Write-Host $runtimeHint -ForegroundColor Yellow
    $candidates = Find32BitRuntimes
    if ($candidates.Count -eq 0) {
        Write-Host 'No 32-bit OpenXR runtime was found on this PC to set it to (SteamVR and Virtual Desktop provide one).' -ForegroundColor Yellow
    } else {
        Write-Host ''
        Write-Host 'Found 32-bit OpenXR runtimes:'
        for ($i = 0; $i -lt $candidates.Count; $i++) { Write-Host "  $($i + 1)) $($candidates[$i])" }
        $answer = Read-Host "Set Thief 2's (32-bit) OpenXR runtime to one of these? Enter its number, or just Enter to skip"
        $pick = 0
        if ([int]::TryParse($answer, [ref]$pick) -and $pick -ge 1 -and $pick -le $candidates.Count) {
            $chosen = $candidates[$pick - 1]
            # HKLM needs administrator rights: reg.exe through a UAC prompt (the
            # 64-bit registry view, so WOW6432Node is the real 32-bit key).
            $regArgs = @('add', 'HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1', '/v', 'ActiveRuntime', '/t', 'REG_SZ',
                      '/d', "`"$chosen`"", '/f', '/reg:64')
            try {
                Start-Process -FilePath "$env:SystemRoot\System32\reg.exe" -ArgumentList $regArgs -Verb RunAs -Wait -WindowStyle Hidden
            } catch {
                Write-Host 'The runtime setting was not changed (administrator permission was declined).' -ForegroundColor Yellow
            }
            $now = (Get-ItemProperty -Path $runtimeKey -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime
            if ($now -eq $chosen) {
                Write-Host "32-bit OpenXR runtime set: $chosen" -ForegroundColor Green
                $runtimeSet = [pscustomobject]@{ set = $chosen; before = $runtime }
            } else {
                Write-Host 'WARNING: the 32-bit OpenXR runtime setting is unchanged.' -ForegroundColor Yellow
            }
        } else {
            Write-Host 'Skipped: the 32-bit OpenXR runtime setting is unchanged.'
        }
    }
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
# 3D sound through a provider, not the game's own software mixing (which the
# mod can't keep at your head): OpenAL if it's installed, else DirectSound3D.
$systemX86 = [Environment]::GetFolderPath('SystemX86')
$haveOpenAL = (Test-Path (Join-Path $systemX86 'OpenAL32.dll')) -or (Test-Path (Join-Path $GameDir 'OpenAL32.dll'))
$snd3d = if ($haveOpenAL) { 'openal' } else { 'a3d' }
foreach ($l in $block) {
    if ($l -eq $blockEnd) {
        $out.Add('; 3D sound through a provider (the mod keeps it at your head, not the aiming hand)')
        $out.Add("snd3d $snd3d")
    }
    $out.Add($l)
}
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
if ($runtimeSet) {
    if ($state.PSObject.Properties.Name -contains 'openxr_runtime') { $state.openxr_runtime = $runtimeSet }
    else { $state | Add-Member -NotePropertyName openxr_runtime -NotePropertyValue $runtimeSet }
}
$state | ConvertTo-Json -Depth 4 | Set-Content -Path $statePath -Encoding UTF8
Write-Host 'Mod files copied.'

Write-Host ''
Write-Host 'Thief2VR is installed.' -ForegroundColor Green
Write-Host ' - Start your headset streaming (e.g. Virtual Desktop), then launch Thief 2 as usual.'
Write-Host ' - Hold the LEFT MENU button for half a second for the VR menu (settings, controls, recenter).'
Write-Host ' - F8 recenters the view. See README.txt for controls and troubleshooting.'
Write-Host ' - To remove the mod, run uninstall.bat.'
