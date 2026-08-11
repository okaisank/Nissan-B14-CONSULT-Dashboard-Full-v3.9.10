$ErrorActionPreference = "Stop"
Write-Host "=== Nissan B14 v3.9.10 CONFIG-GUARD FIX FULL ==="
Write-Host "Project       : C:\B14_v310"
Write-Host "PIO workspace : C:\pio\b14v310"
Write-Host "Protected cfg : config\b14_sdkconfig.defaults"
Write-Host "IMPORTANT     : This script NEVER deletes config\b14_sdkconfig.defaults"

Set-Location $PSScriptRoot

if (-not (Test-Path ".\config\b14_sdkconfig.defaults")) {
    throw "Missing protected config: .\config\b14_sdkconfig.defaults"
}

Remove-Item ".pio" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "managed_components" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "dependencies.lock" -Force -ErrorAction SilentlyContinue

# Delete only generated sdkconfig files. Do NOT use Remove-Item sdkconfig.*
Remove-Item "sdkconfig" -Force -ErrorAction SilentlyContinue
Remove-Item "sdkconfig.old" -Force -ErrorAction SilentlyContinue
Remove-Item "sdkconfig.b14v310" -Force -ErrorAction SilentlyContinue
Remove-Item "sdkconfig.b14v310.old" -Force -ErrorAction SilentlyContinue

Remove-Item "C:\pio\b14v310" -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path "C:\pio" -Force | Out-Null

Write-Host "Protected config OK."
Write-Host "Building with one job..."
pio run -e b14v310 -j 1
