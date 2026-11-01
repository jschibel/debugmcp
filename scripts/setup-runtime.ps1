# Copies the DbgEng runtime (dbgeng/dbghelp/dbgmodel/symsrv + Visualizers + winext)
# from the installed WinDbg package into .\runtime\dbgeng so debugmcp.exe can load it.
# WinDbg lives under C:\Program Files\WindowsApps, whose path changes on every update,
# so we copy it to a stable location. Re-run after updating WinDbg.
param(
    [string]$Destination = (Join-Path $PSScriptRoot "..\runtime\dbgeng")
)
$ErrorActionPreference = "Stop"

$pkg = Get-AppxPackage -Name Microsoft.WinDbg | Sort-Object Version -Descending | Select-Object -First 1
if (-not $pkg) {
    Write-Host "WinDbg not installed; installing via winget..."
    winget install --id Microsoft.WinDbg -e --accept-package-agreements --accept-source-agreements --disable-interactivity
    $pkg = Get-AppxPackage -Name Microsoft.WinDbg | Sort-Object Version -Descending | Select-Object -First 1
    if (-not $pkg) { throw "WinDbg install failed" }
}
$src = Join-Path $pkg.InstallLocation "amd64"
Write-Host "Copying DbgEng runtime from $src (WinDbg $($pkg.Version))"
New-Item -ItemType Directory -Force $Destination | Out-Null
robocopy $src $Destination /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with $LASTEXITCODE" }
Set-Content -Path (Join-Path $Destination "VERSION.txt") -Value "WinDbg $($pkg.Version)" -Encoding utf8
Write-Host "Done: $Destination"
