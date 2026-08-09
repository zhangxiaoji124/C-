$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    & .\scripts\build.ps1
    & .\build\orbitops_tests.exe
    python .\tests\e2e_test.py
    python .\tests\ollama_integration_test.py
} finally {
    Pop-Location
}
