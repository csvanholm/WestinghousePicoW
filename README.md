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
- `sdk-overrides/`: scripts and override files for SDK customization
- `docs/Hardware/`: hardware reference documents
- `CMakeLists.txt`: project build definition

## Prerequisites

- VS Code with Raspberry Pi Pico extension/toolchain installed
- import it as a raspberry pico project and select your pico type
- Pico SDK/toolchain configured in your environment
- `cmake plugin` is recomended to select build type debug,release compiler ect.
- `ninja` available via the Pico toolchain path
- `perl` must be installed in order to rebuild the html content
-  can be downloaded here  https://strawberryperl.com/
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

1. `Compile Project`
2. `Run Project` (UF2 load via `picotool`)
3. `Flash` (OpenOCD-based programming)

## One-Step Bootstrap

Run the bootstrap script once per checkout:

```powershell
powershell -ExecutionPolicy Bypass -File ./scripts/bootstrap.ps1
```

This performs:

1. Recursive submodule init/update
2. RP2040 configure in `build/`
3. RP2350 configure in `build-rp2350/`

## Build And Flash (Command Line)

From repository root:

```powershell
# Build
$env:USERPROFILE/.pico-sdk/ninja/v1.12.1/ninja.exe -C .\build

# Load UF2 (same action as "Run Project" task)
$env:USERPROFILE/.pico-sdk/picotool/2.2.0-a4/picotool/picotool.exe load .\build\westinghouse_RTOS_wifi.uf2 -fx
```

OpenOCD flash command (same action as `Flash` task) is configured in `.vscode/tasks.json`.

## SDK Overrides

Use scripts in `sdk-overrides/`:

- `setup_forked_sdk.ps1`
- `apply_overrides.ps1`
- `revert_overrides.ps1`

See `sdk-overrides/README.md` for details.

## Documentation

- Build and development notes: `DEVELOPER_BUILD.md`
- Release checklist: `RELEASE_CHECKLIST.md`
- Patch reports: `PATCH_REPORT.md`, `PATCH_REPORT_EXEC_SUMMARY.md`

## Redistribution With Submodules

If this repository (or a dependency repository you publish) uses git submodules, distribute it with submodule-safe steps so downstream users get complete sources.

For source users:

```powershell
git clone --recurse-submodules <repo-url>
cd <repo-folder>
git submodule update --init --recursive
```

For maintainers creating release archives:

```powershell
# 1) Create a clean recursive checkout
git clone --recurse-submodules <repo-url> release-src
cd release-src

# 2) Optional sanity check
git submodule status --recursive

# 3) Create a zip that includes submodule contents
Compress-Archive -Path * -DestinationPath ..\WestinghouseRTOSDev-src-with-submodules.zip -Force
```

Notes:

- Git hosting provider auto-generated "Source code (zip/tar.gz)" archives may omit populated submodule content.
- Upload your own generated zip as a release asset when you need a complete, buildable source drop.

## Publish A Complete GitHub Repository

If you want this repository to be self-contained and reproducible for collaborators:

1. Add the Pico SDK fork as a pinned submodule at `third_party/pico-sdk`.
2. Commit tracked VS Code workspace files under `.vscode/`.
3. Keep machine-local debug or tooling settings untracked.
4. Push with submodule metadata.

Example maintainer commands (fresh repo):

```powershell
git submodule add -b v2.2.0-picomail.1 https://github.com/<your-user>/pico-sdk.git third_party/pico-sdk
git add .gitmodules third_party/pico-sdk .vscode .github/workflows/ci.yml scripts/bootstrap.ps1
git commit -m "Make repository reproducible with pinned SDK and bootstrap flow"
git push -u origin main
```

If `third_party/pico-sdk` already exists and you later create your own fork, switch URL with:

```powershell
git submodule set-url third_party/pico-sdk https://github.com/<your-user>/pico-sdk.git
git submodule sync --recursive
```

## License

This project is licensed under the MIT License. See `LICENSE` for details.
