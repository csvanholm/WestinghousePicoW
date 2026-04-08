param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot/..").Path,
    [string]$BuildType = "Debug"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

Write-Host "[bootstrap] Repo root: $RepoRoot"
Push-Location $RepoRoot
try {
    Write-Host "[bootstrap] Initializing submodules..."
    git submodule update --init --recursive

    if (-not (Test-Path "$RepoRoot/third_party/pico-sdk/pico_sdk_init.cmake")) {
        Write-Warning "third_party/pico-sdk was not found. Add the pinned SDK submodule for fully reproducible builds."
    }

    Write-Host "[bootstrap] Runtime Wi-Fi/SMTP credentials are provisioned through the setup portal."

    Write-Host "[bootstrap] Configuring RP2040 build folder..."
    cmake -S . -B build -G Ninja -DPICO_BOARD=pico_w -DCMAKE_BUILD_TYPE=$BuildType

    Write-Host "[bootstrap] Configuring RP2350 build folder..."
    cmake -S . -B build-rp2350 -G Ninja -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=$BuildType

    Write-Host "[bootstrap] Done. Next commands:"
    Write-Host "  ninja -C build"
    Write-Host "  ninja -C build-rp2350"
}
finally {
    Pop-Location
}
