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

- `Compile Project (RP2350)` is the default build task (`Ctrl+Shift+B`), for Pico 2 W Release.
- `Compile Project` builds Pico W Release.
- `Compile Project (RP2040 Debug)` and `Compile Project (RP2350 Debug)` build the corresponding Debug firmware.
- Every compile task configures its matching preset before building. No manual cache deletion or board editing is needed when switching.
- `Run Project` or `Run Project (RP2350)` builds and loads the matching Release UF2 with picotool. The board must be accessible to picotool (use BOOTSEL if necessary).
- `Flash` or `Flash (RP2350)` builds and programs the matching Release firmware over a CMSIS-DAP SWD probe.

For the CMake Tools UI, select one of the four configure presets with
`CMake: Select Configure Preset`. IntelliSense follows that preset through
the CMake Tools configuration provider. Fixed-name tasks and debug launches
always use their named target, independently of the selected UI preset.
Use CMake Tools rather than reimporting the project to change boards.
The Pico extension's independent auto-configure is disabled to avoid competing
build configurations.

For side-by-side target builds from the same code base, use the repo presets:

```powershell
cmake --preset rp2040-release
cmake --build --preset build-rp2040-release

cmake --preset rp2350-release
cmake --build --preset build-rp2350-release
```

This keeps RP2040 output in `build/` and RP2350 output in `build-rp2350/`.

Wi-Fi and SMTP credentials are provisioned at runtime through the setup portal (AP mode), not through CMake build variables.

The bootstrap script configures both Debug presets by default; use
`-BuildType Release` to configure both Release presets. It does not require
local credential files.

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

Select the matching profile in Run and Debug, then press F5:

- `Pico 2 W RP2350 Release`
- `Pico 2 W RP2350 Debug`
- `Pico W RP2040 Release`
- `Pico W RP2040 Debug`

Each profile builds its own preset first and uses an explicit ELF, SVD and
OpenOCD target, rather than the Pico extension's separately selected board.
Release firmware is optimized, so stepping and inspecting variables can be
limited. Use the corresponding Debug profile for normal source debugging.
A CMSIS-DAP-compatible SWD probe and a powered, correctly wired target are required.

The profiles in `.vscode/launch.json` are tuned for stable OpenOCD attach behavior:

- RP2350 profiles:
   - `openOCDLaunchCommands` includes:
      - `adapter speed 5000`
      - `targets rp2350.cm0`
      - `rp2350.cm0 configure -event gdb-attach { reset init }`
- RP2040 profiles:
   - `openOCDLaunchCommands` includes:
      - `adapter speed 500`
      - `rp2040.core0 configure -event gdb-attach { reset init }`

Why this matters:

- The `gdb-attach` reset/init handler prepares the target right as GDB connects, avoiding OpenOCD auto-probe failures.
- RP2040 with CYW43 activity may require a lower SWD clock to avoid `Too long SWD WAIT` / multidrop connect failures.

If RP2040 debug attach still fails intermittently, press RESET on the board immediately before launching the debug profile.

Shared VS Code configuration is version-controlled. Tool paths currently target
the Windows Pico extension installation: CMake 3.31.5, Ninja 1.12.1,
ARM toolchain 15_2_Rel1, picotool 2.3.1 and OpenOCD 0.12.0+dev.
Build and debugger tool paths use `${env:USERPROFILE}` with Windows separators.
If a task reports a malformed path such as `\c:\Users\...`, verify it is using
the checked-in configuration rather than a `${userHome}`-based tool path.
