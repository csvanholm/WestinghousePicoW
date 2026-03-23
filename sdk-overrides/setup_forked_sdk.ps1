param(
    [Parameter(Mandatory = $false)]
    [string]$SdkForkUrl = "https://github.com/<your-user>/pico-sdk.git",

    [Parameter(Mandatory = $false)]
    [string]$SdkTag = "v2.2.0-picomail.1",

    [Parameter(Mandatory = $false)]
    [string]$Destination = "third_party/pico-sdk",

    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($SdkForkUrl -like "*<your-user>*") {
    throw "Set -SdkForkUrl to your real SDK fork URL before running this script."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$destPath = Join-Path $repoRoot $Destination

if (Test-Path $destPath) {
    if (-not $Force) {
        throw "Destination already exists: $destPath. Use -Force to replace it."
    }
    Remove-Item -Path $destPath -Recurse -Force
}

New-Item -ItemType Directory -Path (Split-Path -Parent $destPath) -Force | Out-Null

Write-Host "Cloning SDK fork..."
git clone --recursive --branch $SdkTag $SdkForkUrl $destPath

Write-Host "Ensuring nested submodules are pinned..."
git -C $destPath submodule update --init --recursive

Write-Host "Forked SDK ready at: $destPath"
Write-Host "Next: configure/build; CMakeLists prefers third_party/pico-sdk automatically."
