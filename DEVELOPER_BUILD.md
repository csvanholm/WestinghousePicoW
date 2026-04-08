# PicoMail Developer Build

This project is meant to build from a normal repository clone with the vendored SDK already included under `third_party/pico-sdk`.

## 1) Canonical SDK source

- The authoritative SDK source for this repo is `third_party/pico-sdk`.
- `CMakeLists.txt` prefers that vendored tree automatically when it exists.
- The files under `sdk-overrides/files/` are mirror copies and optional fallback patch sources.
- Editing only the mirror files does not change the normal firmware build.

## 2) Teammate setup (per machine)

Expected user workflow:

- Clone the repository normally.
- Import it with the Raspberry Pi Pico VS Code extension.
- Let the Pico extension manage or install the required toolchain components if they are missing.

Important:
- The active build compiles against `third_party/pico-sdk`.
- The Pico extension can manage the toolchain, but it does not replace the vendored SDK source inside this repository.
- If SMTP/TLS behavior regresses, verify the compiled SDK sources under `third_party/pico-sdk` first.

## 3) Build

After setup, use normal VS Code tasks:

- `Compile Project` for the RP2040 `build/` tree
- `Configure Project (RP2040 Preset)` and `Configure Project (RP2350 Preset)` to create the target-specific build folders
- `Compile Project (RP2350)` for the RP2350 `build-rp2350/` tree
- `Run Project` or `Run Project (RP2350)` to load the matching UF2
- `Flash` for the OpenOCD flow

For side-by-side target builds from the same code base, use the repo presets:

```powershell
cmake --preset rp2040-release
cmake --build --preset build-rp2040-release

cmake --preset rp2350-release
cmake --build --preset build-rp2350-release
```

This keeps RP2040 output in `build/` and RP2350 output in `build-rp2350/`.

Wi-Fi and SMTP credentials are provisioned at runtime through the setup portal (AP mode), not through CMake build variables.

The bootstrap script configures both build directories without requiring local credential files.

`CMakeLists.txt` automatically prefers `third_party/pico-sdk` when present.

Postmortem note:
- A real SMTP/TLS failure was traced to the bundled SDK being built without the effective `ALTCP_MBEDTLS_CLIENT_PROFILE` implementation in its compiled `altcp_tls_mbedtls.c`.
- The project config in `lwipopts.h` still enabled that profile, so the build looked correct at a glance while the active SDK source was not.
- The lasting fix was to restore the TLS profile logic in the compiled bundled SDK and keep the compiled SMTP source aligned with the TLS hostname/SNI fix.

## 4) Distribution model

- Distribute this app repository.
- Keep `third_party/pico-sdk` committed in the repository.
- Update SDK-side fixes in that vendored tree first.
- Keep `sdk-overrides/files` only as fallback mirrors when needed.
- Run the SDK drift check before commit when touching SDK-side files.

Drift check command:

```powershell
powershell -ExecutionPolicy Bypass -File ./scripts/check_sdk_sync.ps1
```

## 5) Existing override fallback

If someone intentionally builds against another compatible SDK path, these scripts still work as a fallback:

- `sdk-overrides/apply_overrides.ps1`
- `sdk-overrides/revert_overrides.ps1`

## 6) Maintainer release flow

For publishing future SDK tags (`v2.2.0-picomail.N`) and update validation, follow:

- `RELEASE_CHECKLIST.md`

### Release archives

If you publish source releases, create the archive from a checked-out working tree so consumers receive the vendored SDK and other tracked build inputs exactly as tested.

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
