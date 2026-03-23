# PicoMail SDK Release Checklist

Use this checklist each time you publish a new forked SDK release (example: `v2.2.0-picomail.2`).

Related setup guide for developers:

- `DEVELOPER_BUILD.md`

## 1) Prepare lwIP fork update

- In your lwIP fork, branch from the currently used commit/tag.
- Apply and review changes in:
  - `src/apps/smtp/smtp.c`
- Run a local build using Pico SDK 2.2.0 + your branch.
- Commit and push lwIP changes.
- Record the lwIP commit SHA.

## 2) Prepare pico-sdk fork update

- In your pico-sdk fork, branch from the currently used SDK tag/branch.
- Apply and review SDK-side changes in:
  - `src/rp2_common/pico_lwip/altcp_tls_mbedtls.c`
- Update `lib/lwip` submodule pointer to the new lwIP commit SHA.
- Run:
  - `git submodule update --init --recursive`
- Commit and push pico-sdk changes.

## 3) Tag and publish SDK fork

- Create annotated tag in pico-sdk fork:
  - `v2.2.0-picomail.N`
- Push branch and tag.
- Confirm tag resolves to the intended commit and submodule pointer.

## 4) Update app repository

- Update documentation tag references in:
  - `DEVELOPER_BUILD.md`
  - `sdk-overrides/setup_forked_sdk.ps1` (default `SdkTag` if you want)
- If override files changed, refresh:
  - `sdk-overrides/files/src/rp2_common/pico_lwip/altcp_tls_mbedtls.c`
  - `sdk-overrides/files/lib/lwip/src/apps/smtp/smtp.c`
- Run:
  - `powershell -ExecutionPolicy Bypass -File ./sdk-overrides/apply_overrides.ps1`
  - build task `Apply + Compile Project`
  - flash task `Flash`

## 5) Validation before announcing

- Compile succeeds from clean build directory.
- Device flash succeeds via OpenOCD.
- Runtime setup portal works end-to-end (AP mode, save Wi-Fi/SMTP settings, reboot with persisted config).
- SMTP send path works end-to-end (connect, TLS handshake, AUTH, DATA, QUIT).
- Confirm no credentials are committed in shared files.

## 6) Source bundle integrity (if distributing source archives)

- Create a fresh recursive checkout:
  - `git clone --recurse-submodules <repo-url> release-src`
  - `cd release-src`
  - `git submodule update --init --recursive`
  - `git submodule status --recursive`
- Build at least once from this checkout to confirm all required source inputs are present.
- Create and publish your own release asset archive from this checkout:
  - `Compress-Archive -Path * -DestinationPath ..\WestinghouseRTOSDev-src-with-submodules.zip -Force`
- Do not rely on provider auto-generated source archives when complete submodule materialization is required.

## 7) Team announcement template

- New SDK tag: `<tag>`
- Why: `<summary of fixes>`
- Action for developers:
  - Run `setup_forked_sdk.ps1` with new tag
  - Reconfigure/build
- Rollback option:
  - Use previous known-good tag `<previous-tag>`

## 8) Quick pre-release gate (3 commands)

Run these from a clean checkout before you attach a source zip to a GitHub release:

```powershell
git submodule sync --recursive; git submodule update --init --recursive
powershell -ExecutionPolicy Bypass -File ./scripts/bootstrap.ps1
git submodule status --recursive
```

Expected result:

- Submodules are initialized and synchronized.
- Bootstrap configures both `build/` and `build-rp2350/`.
- `git submodule status --recursive` prints pinned SHAs with no missing entries.
