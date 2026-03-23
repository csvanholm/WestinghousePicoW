param(
    [string]$SdkPath = $env:PICO_SDK_PATH
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($SdkPath)) {
    throw "PICO_SDK_PATH is not set. Pass -SdkPath or set environment variable PICO_SDK_PATH."
}

$resolvedSdkPath = (Resolve-Path -Path $SdkPath).Path
$backupRoot = Join-Path $PSScriptRoot '.backup'

$files = @(
    'src/rp2_common/pico_lwip/altcp_tls_mbedtls.c',
    'lib/lwip/src/apps/smtp/smtp.c'
)

foreach ($relativePath in $files) {
    $backupFile = Join-Path $backupRoot $relativePath
    $targetFile = Join-Path $resolvedSdkPath $relativePath

    if (-not (Test-Path -Path $backupFile -PathType Leaf)) {
        Write-Warning "No backup found for: $relativePath (skipping)"
        continue
    }
    if (-not (Test-Path -Path $targetFile -PathType Leaf)) {
        throw "Target SDK file not found: $targetFile"
    }

    Copy-Item -Path $backupFile -Destination $targetFile -Force
    Write-Host "Restored: $relativePath"
}

Write-Host "Done. SDK files restored from backups where available."
