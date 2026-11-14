param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot/..").Path,
    [ValidateSet("Debug", "Release")]
    [string]$BuildType = "Debug"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

Write-Host "[bootstrap] Repo root: $RepoRoot"
Push-Location $RepoRoot
try {
    Write-Host "[bootstrap] Initializing submodules..."
    git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) {
        throw "Submodule initialization failed."
    }

    if (-not (Test-Path "$RepoRoot/third_party/pico-sdk/pico_sdk_init.cmake")) {
        Write-Warning "third_party/pico-sdk was not found. Add the pinned SDK submodule for fully reproducible builds."
    }

    Write-Host "[bootstrap] Runtime Wi-Fi/SMTP credentials are provisioned through the setup portal."

    $PresetSuffix = $BuildType.ToLowerInvariant()
    foreach ($Target in @("rp2040", "rp2350")) {
        $Preset = "$Target-$PresetSuffix"
        Write-Host "[bootstrap] Configuring $Preset..."
        cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configure failed for preset '$Preset'."
        }
    }

    Write-Host "[bootstrap] Done. Next commands:"
    Write-Host "  cmake --build --preset build-rp2040-$PresetSuffix"
    Write-Host "  cmake --build --preset build-rp2350-$PresetSuffix"
}
finally {
    Pop-Location
}
