param(
    [string]$SdkPath = $env:PICO_SDK_PATH,
    [switch]$ForceBackupRefresh
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($SdkPath)) {
    throw "PICO_SDK_PATH is not set. Pass -SdkPath or set environment variable PICO_SDK_PATH."
}

$resolvedSdkPath = (Resolve-Path -Path $SdkPath).Path
$overridesRoot = Join-Path $PSScriptRoot 'files'
$backupRoot = Join-Path $PSScriptRoot '.backup'

$files = @(
    'src/rp2_common/pico_lwip/altcp_tls_mbedtls.c',
    'lib/lwip/src/apps/smtp/smtp.c'
)

foreach ($relativePath in $files) {
    $sourceOverride = Join-Path $overridesRoot $relativePath
    $targetFile = Join-Path $resolvedSdkPath $relativePath
    $backupFile = Join-Path $backupRoot $relativePath

    if (-not (Test-Path -Path $sourceOverride -PathType Leaf)) {
        throw "Missing override file: $sourceOverride"
    }
    if (-not (Test-Path -Path $targetFile -PathType Leaf)) {
        throw "Target SDK file not found: $targetFile"
    }

    $backupDir = Split-Path -Path $backupFile -Parent
    if (-not (Test-Path -Path $backupDir -PathType Container)) {
        New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    }

    if ($ForceBackupRefresh -or -not (Test-Path -Path $backupFile -PathType Leaf)) {
        Copy-Item -Path $targetFile -Destination $backupFile -Force
        Write-Host "Backed up: $relativePath"
    }

    Copy-Item -Path $sourceOverride -Destination $targetFile -Force
    Write-Host "Applied override: $relativePath"
}

Write-Host "Done. SDK overrides applied to: $resolvedSdkPath"
