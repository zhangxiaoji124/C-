$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if (-not (Test-Path .\build\orbitops.exe)) { & .\scripts\build.ps1 }
    Write-Host 'OrbitOps is starting at http://127.0.0.1:8080'
    & .\build\orbitops.exe --role all
} finally {
    Pop-Location
}
