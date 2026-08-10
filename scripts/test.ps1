$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    & .\scripts\build.ps1
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
    & .\build\orbitops_tests.exe
    if ($LASTEXITCODE -ne 0) { throw "Unit tests failed with exit code $LASTEXITCODE" }
    python .\tests\e2e_test.py
    if ($LASTEXITCODE -ne 0) { throw "E2E tests failed with exit code $LASTEXITCODE" }
    python .\tests\ollama_integration_test.py
    if ($LASTEXITCODE -ne 0) { throw "Ollama integration failed with exit code $LASTEXITCODE" }
    & .\build\orbitops_dev_review_test.exe
    if ($LASTEXITCODE -ne 0) { throw "Developer review integration failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
