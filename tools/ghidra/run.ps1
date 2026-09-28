# Runs one of our Ghidra scripts headless against the analyzed exe.
# Usage: tools\ghidra\run.ps1 <Script.java> <out_file> <args...>
param(
    [Parameter(Mandatory)] [string]$Script,
    [Parameter(Mandatory)] [string]$Out,
    [Parameter(ValueFromRemainingArguments)] [string[]]$ScriptArgs
)
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$headless = 'D:\Projects\Tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat'
if (Test-Path $Out) { Remove-Item $Out }
& $headless (Join-Path $root 're') T2VR -process Thief2_129.exe -noanalysis -readOnly `
    -scriptPath $PSScriptRoot -postScript $Script $Out @ScriptArgs 2>&1 |
    Select-String -Pattern 'ERROR|Exception' | ForEach-Object Line
