# Copies the DbgEng runtime (dbgeng/dbghelp/dbgmodel/symsrv + Visualizers + winext) into
# .\runtime\dbgeng so debugmcp.exe can load it. Sources, in order:
#   1. -Source <dir>, if given
#   2. the installed WinDbg app (newest engine; its folder under WindowsApps changes on every
#      update, hence the copy)
#   3. the Windows SDK's "Debugging Tools for Windows" (<KitsRoot10>\Debuggers\x64)
#   4. WinDbg installed with winget (skip with -NoInstall)
# Re-run after updating WinDbg.
param(
    [string]$Destination = (Join-Path $PSScriptRoot "..\runtime\dbgeng"),
    [string]$Source = "",
    [switch]$NoInstall
)
$ErrorActionPreference = "Stop"

function Test-Engine($dir) { $dir -and (Test-Path (Join-Path $dir "dbgeng.dll")) }

function Find-WinDbg {
    try {
        $pkg = Get-AppxPackage -Name Microsoft.WinDbg -ErrorAction Stop | Sort-Object Version -Descending | Select-Object -First 1
    } catch { return $null }
    if ($pkg) {
        $dir = Join-Path $pkg.InstallLocation "amd64"
        if (Test-Engine $dir) { return @{ Dir = $dir; Label = "WinDbg $($pkg.Version)" } }
    }
    return $null
}

function Find-SdkDebuggers {
    $roots = @()
    try {
        $roots += (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots" -ErrorAction Stop).KitsRoot10
    } catch {}
    $roots += Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
    foreach ($r in $roots | Select-Object -Unique) {
        $dir = Join-Path $r "Debuggers\x64"
        if (Test-Engine $dir) {
            $ver = (Get-Item (Join-Path $dir "dbgeng.dll")).VersionInfo.FileVersion
            return @{ Dir = $dir; Label = "Windows SDK Debugging Tools $ver" }
        }
    }
    return $null
}

$found = $null
if ($Source) {
    if (-not (Test-Engine $Source)) { throw "No dbgeng.dll in $Source" }
    $found = @{ Dir = $Source; Label = $Source }
}
if (-not $found) { $found = Find-WinDbg }
if (-not $found) { $found = Find-SdkDebuggers }
if (-not $found -and -not $NoInstall) {
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        Write-Host "No DbgEng found; installing WinDbg with winget..."
        winget install --id Microsoft.WinDbg -e --accept-package-agreements --accept-source-agreements --disable-interactivity | Out-Host
        $found = Find-WinDbg
    }
}
if (-not $found) {
    throw ("No DbgEng runtime found. Install WinDbg (winget install Microsoft.WinDbg, or the Microsoft Store) " +
           "or the Windows SDK's 'Debugging Tools for Windows', then re-run this script.")
}

Write-Host "Copying DbgEng runtime from $($found.Dir) ($($found.Label))"
New-Item -ItemType Directory -Force $Destination | Out-Null
robocopy $found.Dir $Destination /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with $LASTEXITCODE" }
$global:LASTEXITCODE = 0
Set-Content -Path (Join-Path $Destination "VERSION.txt") -Value $found.Label -Encoding utf8
Write-Host "Done: $((Resolve-Path $Destination).Path)"
exit 0
