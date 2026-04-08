# SDK Overrides (Pico SDK 2.2.0)

Important:

- The canonical SDK source for normal builds in this repository is `third_party/pico-sdk`.
- `CMakeLists.txt` prefers that vendored SDK tree when it exists.
- The files under `sdk-overrides/files` are fallback mirror copies and patch sources for applying the same edits onto some other compatible SDK tree.
- Editing only `sdk-overrides/files` does not change the firmware built from `third_party/pico-sdk`.

This folder contains mirror copies of two SDK source edits:

- `src/rp2_common/pico_lwip/altcp_tls_mbedtls.c`
- `lib/lwip/src/apps/smtp/smtp.c`

Use the scripts in this folder only when you intentionally want to apply or revert those edits on another SDK tree using the same layout/version.

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

Preferred reproducible workflow:

1. Clone this repository.
2. Import it with the Raspberry Pi Pico VS Code extension.
3. Let the extension manage or install the Pico toolchain if needed.
4. Build against the vendored `third_party/pico-sdk` tree.
5. Treat that vendored SDK as the source of truth for SDK-side fixes.

When maintaining SDK-side files, run:

```powershell
powershell -ExecutionPolicy Bypass -File ./scripts/check_sdk_sync.ps1
```

That script fails if the vendored SDK copies and the mirror files in `sdk-overrides/files` have drifted apart.

Override workflow for external SDK users only:

1. Clone/open this project.
2. Run `apply_overrides.ps1` once.
3. Configure/build normally.

If execution policy blocks scripts, run:

```powershell
powershell -ExecutionPolicy Bypass -File ./sdk-overrides/apply_overrides.ps1
```
