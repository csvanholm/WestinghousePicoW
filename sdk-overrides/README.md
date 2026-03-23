# SDK Overrides (Pico SDK 2.2.0)

This project depends on two SDK source edits that are stored under `sdk-overrides/files`:

- `src/rp2_common/pico_lwip/altcp_tls_mbedtls.c`
- `lib/lwip/src/apps/smtp/smtp.c`

Use the scripts in this folder to apply or revert those edits on any machine using the same SDK layout/version.

## Requirements

- Pico SDK `2.2.0`
- `PICO_SDK_PATH` set, or pass `-SdkPath`
- PowerShell

## Apply overrides

From the project root:

```powershell
./sdk-overrides/apply_overrides.ps1
```

If you need to explicitly set SDK path:

```powershell
./sdk-overrides/apply_overrides.ps1 -SdkPath "C:/Users/<you>/.pico-sdk/sdk/2.2.0"
```

The script creates backups of original SDK files at `sdk-overrides/.backup/` on first apply.

## Revert overrides

```powershell
./sdk-overrides/revert_overrides.ps1
```

Or with explicit SDK path:

```powershell
./sdk-overrides/revert_overrides.ps1 -SdkPath "C:/Users/<you>/.pico-sdk/sdk/2.2.0"
```

## Team workflow recommendation

1. Clone/open this project.
2. Run `apply_overrides.ps1` once.
3. Configure/build normally.

If execution policy blocks scripts, run:

```powershell
powershell -ExecutionPolicy Bypass -File ./sdk-overrides/apply_overrides.ps1
```
