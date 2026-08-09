$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if (Get-Command cmake -ErrorAction SilentlyContinue) {
        cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
        cmake --build build --config Release --parallel
    } elseif (Get-Command mingw32-make -ErrorAction SilentlyContinue) {
        mingw32-make -j ([Environment]::ProcessorCount)
    } else {
        throw 'CMake or MinGW Make is required to build OrbitOps.'
    }
} finally {
    Pop-Location
}
