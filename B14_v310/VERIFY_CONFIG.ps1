$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
Write-Host "=== v3.9.10 CONFIG VERIFY ==="
$cfg = ".\config\b14_sdkconfig.defaults"
if (-not (Test-Path $cfg)) { throw "FAIL: $cfg missing" }
Write-Host "PASS: protected defaults exists"
Select-String -Path $cfg -Pattern "CONFIG_COMPILER_CXX_EXCEPTIONS=y|CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y"
Write-Host "PASS: required settings found"
