# Builds a release and packs it for players: dist\Thief2VR-<version>.zip with
# the mod DLLs, the OpenXR loader, install/uninstall scripts, the player README
# and third-party licences.
# Usage: tools\package.ps1 [-NoBuild]
param(
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$versionLine = Select-String -Path (Join-Path $root 'core\version.h') -Pattern '#define THIEF2VR_VERSION "(.+)"'
if (-not $versionLine) { throw 'THIEF2VR_VERSION not found in core\version.h' }
$version = $versionLine.Matches[0].Groups[1].Value

if (-not $NoBuild) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $msbuild = & $vswhere -latest -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw 'MSBuild not found (install VS 2022 with the C++ workload)' }
    & $msbuild (Join-Path $root 'Thief2VR.sln') /p:Configuration=Release /p:Platform=x86 /m /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
}

# The zip holds one folder, Thief2VR, which players extract into their Thief 2
# folder; install.bat finds the game in its parent folder.
$name = "Thief2VR-$version"
$dist = Join-Path $root 'dist'
$stageRoot = Join-Path $dist $name
$stage = Join-Path $stageRoot 'Thief2VR'
if (Test-Path $stageRoot) { Remove-Item $stageRoot -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'licenses') | Out-Null

$build = Join-Path $root 'build\Release'
Copy-Item (Join-Path $build 'd3d9.dll') $stage
Copy-Item (Join-Path $build 'thief2vr.dll') $stage
Copy-Item (Join-Path $root 'third_party\openxr\Win32\bin\openxr_loader.dll') $stage

# Scripts and README with Windows line endings (for Notepad and cmd).
foreach ($f in @('install.ps1', 'uninstall.ps1', 'install.bat', 'uninstall.bat', 'README.txt')) {
    $text = [System.IO.File]::ReadAllText((Join-Path $root "package\$f"))
    $text = $text -replace "`r?`n", "`r`n"
    [System.IO.File]::WriteAllText((Join-Path $stage $f), $text, (New-Object System.Text.UTF8Encoding($false)))
}
Set-Content -Path (Join-Path $stage 'VERSION.txt') -Value $version -Encoding ASCII

$license = [System.IO.File]::ReadAllText((Join-Path $root 'LICENSE')) -replace "`r?`n", "`r`n"
[System.IO.File]::WriteAllText((Join-Path $stage 'LICENSE.txt'), $license, (New-Object System.Text.UTF8Encoding($false)))
Copy-Item (Join-Path $root 'third_party\minhook\LICENSE.txt') (Join-Path $stage 'licenses\MinHook.txt')
Copy-Item (Join-Path $root 'third_party\imgui\LICENSE.txt') (Join-Path $stage 'licenses\DearImGui.txt')
Copy-Item (Join-Path $root 'third_party\openxr\share\doc\openxr\LICENSE') (Join-Path $stage 'licenses\OpenXR-loader.txt')

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip
"packaged $zip"
Get-ChildItem $stage -Recurse | Select-Object -ExpandProperty FullName | ForEach-Object { "  " + $_.Substring($stageRoot.Length + 1) }
