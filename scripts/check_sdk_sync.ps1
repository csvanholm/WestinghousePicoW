param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot/..").Path
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-NormalizedFileText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        throw "Missing file: $Path"
    }

    $text = Get-Content -Path $Path -Raw
    return $text.Replace("`r`n", "`n").Replace("`r", "`n")
}

Push-Location $RepoRoot
try {
    $filePairs = @(
        @(
            "third_party/pico-sdk/src/rp2_common/pico_lwip/altcp_tls_mbedtls.c",
            "sdk-overrides/files/src/rp2_common/pico_lwip/altcp_tls_mbedtls.c"
        ),
        @(
            "third_party/pico-sdk/lib/lwip/src/apps/smtp/smtp.c",
            "sdk-overrides/files/lib/lwip/src/apps/smtp/smtp.c"
        )
    )

    $hadDrift = $false
    foreach ($pair in $filePairs) {
        $canonical = Join-Path $RepoRoot $pair[0]
        $mirror = Join-Path $RepoRoot $pair[1]

        $canonicalText = Get-NormalizedFileText -Path $canonical
        $mirrorText = Get-NormalizedFileText -Path $mirror

        if ($canonicalText -ne $mirrorText) {
            Write-Host "[sdk-sync] DRIFT:" -ForegroundColor Red
            Write-Host "  canonical: $($pair[0])"
            Write-Host "  mirror:    $($pair[1])"
            $hadDrift = $true
        } else {
            Write-Host "[sdk-sync] OK: $($pair[0])"
        }
    }

    if ($hadDrift) {
        Write-Error "SDK mirror drift detected. Sync sdk-overrides/files from third_party/pico-sdk before committing."
        exit 1
    }

    Write-Host "[sdk-sync] All checked SDK files match their mirrors."
}
finally {
    Pop-Location
}