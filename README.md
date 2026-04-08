# WestinghousePicoW

This controller was designed using a pico-w or pico2-w (rp2040 or rp2350) using RTOS with just a few extra external components to 
start, stop and detect power failure and engine run status of the Westinghouse WGen9500DF generator but it will work with most westinghouse models via the built-in smart switch port from a transfer-switch relay contact that will close when utility power fails and open when power is present.
The schematic for this controller can be found in the doc/hardware folder

The generator is normally started manually using either the start push button or the remote key fob, however it cannot be hooked 
up directly to a transfer-switch relay since the generator needs a pulse of a specific length to start and longer pulse to stop. 

The controller was designed with the following features.

1) After powering up it will wait for the generator controller to become active before 
   sending a start pulse, this is necessary because the generator controller does not
   accept commands right after power-up.

2) The Controller will try to start the generator up to 3 times if it fails after that the       controller will
   "lockup" and blink all LEDs this is done to prevent draining the battery with infinite   restart attempts. 
   if the generator runs out of gas or fails for some other mechanical reason (such as forgetting to open the gas spigot) 
   (Lockup is reset by turning the power off and on again)

3) When power returns the controller will run the generator in "cool down" mode for 40 seconds before stopping it,
   if power fails during cool-down, it will just keep running and exit cool-down mode.

4) The generator controller supports a weekly exerciser function that is enabled by shorting 
   GPIO5 to GND If enabled it will run the generator for 5 minutes every 7 days. (since last   run)

5) Email notification using your Wi-Fi, the controller will queue up notifications in case of network failure and send them when your network comes online again.

6) The controller has a web-based setup of your network credentials and email server info.


Good Luck!

Carsten Svanholm
csvanholm@comcast.net

RP2040/RP2350 Pico-W or Pico2-W firmware project with SMTP-related networking updates and SDK override support.

## Repository Layout

- `main.cpp`      : main application source
- `third_party/pico-sdk/`: vendored Pico SDK source used by normal builds
- `sdk-overrides/`: scripts and override files for SDK customization
- `docs/Hardware/`: hardware reference documents
- `CMakeLists.txt`: project build definition

## Prerequisites

- VS Code with the Raspberry Pi Pico extension
- import it as a Raspberry Pi Pico project and select your target board
- the Pico extension should install or assign the required toolchain components if they are not already present on the machine
- `cmake plugin` is recomended to select build type debug,release compiler ect.
- `ninja` available via the Pico toolchain path
- `perl` must be installed in order to rebuild the html content
-  can be downloaded here  https://strawberryperl.com/

## Canonical SDK Source

Normal builds in this repository use the vendored SDK tree in `third_party/pico-sdk`.

- This repository is intended to be self-contained at the source level after a normal `git clone`.
- The files under `sdk-overrides/files` are mirror copies and fallback patch sources, not the primary build input.
- If you change SDK-side TLS or SMTP behavior, update `third_party/pico-sdk` first.
## Runtime Configuration

Wi-Fi and SMTP credentials are configured at runtime through the Pico W setup portal.

1. Boot into setup mode (short GPIO22 to Ground at boot for minimum of 3 seconds).
   The onboard LED will blink fast when you are in setup mode.
2. Connect to SSID `WESTINGHOUSE_CONFIG`.
3. Browse to `http://192.168.0.1` and save Wi-Fi/SMTP settings.

Credentials are stored in device flash and are no longer configured via CMake.

## Known-Good Tool Versions

Based on current workspace task configuration:

- Ninja: `1.12.1`
- Picotool: `2.2.0-a4`
- OpenOCD: `0.12.0+dev`

Windows task paths resolve under `%USERPROFILE%\.pico-sdk\...`.

## Build And Flash (VS Code Tasks)

Use the built-in tasks in this workspace:

1. `Compile Project` builds the RP2040 Pico W binary from `build/`
2. `Run Project` loads the active launch target with `picotool`
3. `Configure Project (RP2040 Preset)` configures `build/`
4. `Configure Project (RP2350 Preset)` configures `build-rp2350/`
5. `Compile Project (RP2350)` builds the Pico 2 W binary from `build-rp2350/`
6. `Run Project (RP2350)` loads `build-rp2350/westinghouse_wifi_rtos.uf2`
7. `Flash` runs the OpenOCD-based programming flow

The Pico extension's generated [CMakeLists.txt](/home/pi/pico/dummy/WestinghousePicoW/CMakeLists.txt) currently defaults `PICO_BOARD` to `pico2_w`, so presets or the explicit configure tasks are the safest way to select the target build directory you want.

## CMake Presets

The repository now includes target-specific presets so RP2040 and RP2350 can be built from the same source tree without reusing the same build directory.

Configure or build with CMake directly:

```powershell
cmake --preset rp2040-release
cmake --build --preset build-rp2040-release

cmake --preset rp2350-release
cmake --build --preset build-rp2350-release
```

Preset layout:

- `rp2040-release` uses `PICO_BOARD=pico_w` and `build/`
- `rp2040-debug` uses `PICO_BOARD=pico_w` and `build-debug/`
- `rp2350-release` uses `PICO_BOARD=pico2_w` and `build-rp2350/`
- `rp2350-debug` uses `PICO_BOARD=pico2_w` and `build-rp2350-debug/`

## One-Step Bootstrap

Run the bootstrap script once per checkout:

```powershell
powershell -ExecutionPolicy Bypass -File ./scripts/bootstrap.ps1
```

This performs:

1. Recursive submodule init/update
2. RP2040 configure in `build/`
3. RP2350 configure in `build-rp2350/`

It does not require Wi-Fi or SMTP credentials up front. Those are provisioned later through the setup portal and stored in device flash.

## Build And Flash (Command Line)

From repository root:

```powershell
# Build RP2040
$env:USERPROFILE/.pico-sdk/ninja/v1.12.1/ninja.exe -C .\build

# Build RP2350
$env:USERPROFILE/.pico-sdk/ninja/v1.12.1/ninja.exe -C .\build-rp2350

# Load RP2040 UF2
$env:USERPROFILE/.pico-sdk/picotool/2.2.0-a4/picotool/picotool.exe load .\build\westinghouse_wifi_rtos.uf2 -fx

# Load RP2350 UF2
$env:USERPROFILE/.pico-sdk/picotool/2.2.0-a4/picotool/picotool.exe load .\build-rp2350\westinghouse_wifi_rtos.uf2 -fx
```

OpenOCD flash command (same action as `Flash` task) is configured in `.vscode/tasks.json`.

## SDK Overrides

Use scripts in `sdk-overrides/`:

- `apply_overrides.ps1`
- `revert_overrides.ps1`

For maintainers, verify the mirror copies have not drifted from the vendored SDK with:

```powershell
powershell -ExecutionPolicy Bypass -File ./scripts/check_sdk_sync.ps1
```

The override scripts are only for intentionally patching some other compatible SDK tree. They are not part of the normal clone/import/build workflow for this repository.

See `sdk-overrides/README.md` for details.

## Documentation

- Build and development notes: `DEVELOPER_BUILD.md`
- Release checklist: `RELEASE_CHECKLIST.md`
- Patch reports: `PATCH_REPORT.md`, `PATCH_REPORT_EXEC_SUMMARY.md`

## Distribution Model

This repository is intended to be consumed as a normal clone with complete source inputs already present.

- Keep `third_party/pico-sdk` committed and in sync with any SDK-side fixes.
- Keep `.vscode/`, presets, and build scripts tracked when they are part of the intended developer workflow.
- Treat `sdk-overrides/files` as fallback mirrors only.
- When making SDK-side edits, run `scripts/check_sdk_sync.ps1` before committing.

For release archives, create a zip from the checked-out working tree instead of relying on generated hosting-provider source archives.

## License

This project is licensed under the MIT License. See `LICENSE` for details.
