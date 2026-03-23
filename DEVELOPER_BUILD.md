# PicoMail Developer Build (Forked SDK)

This project supports a pinned forked Pico SDK for reproducible builds across developers.

## 1) Create and publish your SDK fork (one-time, maintainer)

1. Fork `raspberrypi/pico-sdk`.
2. Create branch from `2.2.0`.
3. Commit your SDK-side fix in:
   - `src/rp2_common/pico_lwip/altcp_tls_mbedtls.c`
4. Fork the lwIP repo used by pico-sdk and commit your lwIP fix in:
   - `lib/lwip/src/apps/smtp/smtp.c`
5. In your SDK fork, update `lib/lwip` submodule pointer to your lwIP commit.
6. Tag the SDK fork commit, for example: `v2.2.0-picomail.1`.

## 2) Teammate setup (per machine)

From project root in PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File ./sdk-overrides/setup_forked_sdk.ps1 \
  -SdkForkUrl "https://github.com/<your-user>/pico-sdk.git" \
  -SdkTag "v2.2.0-picomail.1"
```

This clones the pinned SDK to `third_party/pico-sdk`.

## 3) Build

After setup, use normal VS Code tasks:

- `Apply + Compile Project` (or `Compile Project`)
- `Flash`

Wi-Fi and SMTP credentials are provisioned at runtime through the setup portal (AP mode), not through CMake build variables.

`CMakeLists.txt` automatically prefers `third_party/pico-sdk` when present.

## 4) Distribution model

- Distribute this app repository.
- Do not copy full SDK sources into app source folders.
- Keep SDK changes in your SDK fork + tag.
- Bump tag (`v2.2.0-picomail.2`, etc.) when SDK-side fixes change.

## 5) Existing override fallback

If a teammate cannot use the fork immediately, these scripts still work against a compatible SDK path:

- `sdk-overrides/apply_overrides.ps1`
- `sdk-overrides/revert_overrides.ps1`

## 6) Maintainer release flow

For publishing future SDK tags (`v2.2.0-picomail.N`) and update validation, follow:

- `RELEASE_CHECKLIST.md`

### Source redistribution with submodules

If you release source bundles and any repository in your dependency chain uses git submodules, create release assets from a recursive checkout.

```powershell
git clone --recurse-submodules <repo-url> release-src
cd release-src
git submodule update --init --recursive
git submodule status --recursive
Compress-Archive -Path * -DestinationPath ..\WestinghouseRTOSDev-src-with-submodules.zip -Force
```

Why this step exists:

- Provider-generated source archives are often missing materialized submodule files.
- A zip created from the recursive working tree ensures third parties receive complete build inputs.

## 7) Debug launch notes (RP2040 and RP2350)

The Cortex-Debug launch profiles in `.vscode/launch.json` are tuned for stable OpenOCD attach behavior:

- RP2350 profile (`Pico Debug RP2350 (Cortex-M33)`):
   - `openOCDLaunchCommands` includes:
      - `adapter speed 5000`
      - `targets rp2350.cm0`
      - `rp2350.cm0 configure -event gdb-attach { reset init }`
- RP2040 profile (`Pico Debug (Cortex-Debug)`):
   - `openOCDLaunchCommands` includes:
      - `adapter speed 500`
      - `rp2040.core0 configure -event gdb-attach { reset init }`

Why this matters:

- The `gdb-attach` reset/init handler prepares the target right as GDB connects, avoiding OpenOCD auto-probe failures.
- RP2040 with CYW43 activity may require a lower SWD clock to avoid `Too long SWD WAIT` / multidrop connect failures.

If RP2040 debug attach still fails intermittently, press RESET on the board immediately before launching the debug profile.
