# PicoMail SMTP/TLS Patch Report

Date: 2026-02-25

## 1) Executive summary

This project required SDK-level patches because the failing behavior occurred inside lwIP + mbedTLS integration code (Pico SDK/lwIP), not only in application code.

Postmortem note from 2026-04-07:
- The decisive root cause was not bad SMTP credentials or a generic network problem.
- The real build was using the bundled SDK under `third_party/pico-sdk`, while some expected fixes existed only in the override mirror or historical notes.
- That meant the firmware was being built without the effective TLS client-profile fix in the compiled `altcp_tls_mbedtls.c`, even though `lwipopts.h` still enabled `ALTCP_MBEDTLS_CLIENT_PROFILE 1`.
- Once the compiled SDK copy was corrected, and the compiled SMTP source also set TLS hostname/SNI, Gmail SMTPS worked again.

Separate from that root cause, RP2350 also needed runtime fixes for safe CYW43/LED ownership and task stability. Those issues were real, but they were not the underlying reason Gmail SMTPS was failing.

After patching:
- TLS handshake and SMTP transaction became stable with Gmail SMTPS (`smtp.gmail.com:465`).
- Build/flash path is reproducible.
- Team distribution workflow now supports either a forked SDK tag or scripted overrides.

For exact source locations of each edit, see section 8 (Source Code Change Map).

## 2) Why SDK patching was necessary

Application code can configure SMTP host/auth and call into lwIP SMTP APIs, but the failures were caused by lower layers:

1. TLS client profile negotiation behavior in Pico SDK TLS glue.
2. SMTP TLS session setup details in lwIP SMTP implementation.

Those are implemented in SDK/lwIP source, so fixing only app-level code would not fully solve the failures.

Important build-path clarification:
- In this repository, the files under `sdk-overrides/files/...` are mirrors/fallback inputs, not proof that the active firmware build contains the same logic.
- The actual build path currently prefers `third_party/pico-sdk` via `CMakeLists.txt`.
- If a fix is applied only to the override mirror and not to the bundled SDK source tree, the firmware can still be built with the broken behavior.

## 3) Files changed and what each change does

### A) SDK TLS glue patch
- File: `src/rp2_common/pico_lwip/altcp_tls_mbedtls.c`
- Project mirror: sdk-overrides/files/src/rp2_common/pico_lwip/altcp_tls_mbedtls.c

Changes:
- Added `ALTCP_MBEDTLS_CLIENT_PROFILE` compile-time selector.
- Added `ALTCP_MBEDTLS_VERBOSE_TLS_ERRORS` compile-time selector.
- Added optional verbose diagnostics for handshake/read failures.
- Added client ciphersuite profile for stable RSA-CBC operation when profile=1.
- Kept debug callback behavior aligned with lwIP debug macros.

Why:
- Handshake interoperability was unstable with broader/default profile in this embedded configuration.
- Restricting client ciphersuites to the known-good RSA-CBC set produced stable connection behavior in your environment.
- Verbose error telemetry was needed during diagnosis, then made optional to reduce noise.

### B) lwIP SMTP patch
- File: `lib/lwip/src/apps/smtp/smtp.c`
- Project mirror: sdk-overrides/files/lib/lwip/src/apps/smtp/smtp.c

Changes:
- Included mbedTLS SSL API when TLS is enabled.
- Set TLS hostname (`mbedtls_ssl_set_hostname`) on SMTP TLS PCB creation.
- Added SMTP connection diagnostics for direct target and DNS resolution.

Why:
- Correct hostname propagation/SNI behavior improves TLS server-side matching and reliability.
- Additional SMTP diagnostics made DNS/connection-stage failures observable and debuggable.

### C) Project configuration
- File: lwipopts.h

Changes:
- `ALTCP_MBEDTLS_CLIENT_PROFILE 1`
- `ALTCP_MBEDTLS_VERBOSE_TLS_ERRORS 0`

Why:
- Centralized control of SDK patch behavior from project config.
- Safe defaults: stable profile enabled, verbose TLS diagnostics off.

### D) Portability/distribution workflow

Added files:
- sdk-overrides/apply_overrides.ps1
- sdk-overrides/revert_overrides.ps1
- sdk-overrides/setup_forked_sdk.ps1
- DEVELOPER_BUILD.md
- RELEASE_CHECKLIST.md

Why:
- Allows other developers to reproduce the exact SDK behavior reliably.
- Supports two distribution models:
  - preferred: forked/pinned SDK tag,
  - fallback: scripted local overrides.

## 4) Failure types observed (root-cause classification)

### 4.1 TLS interoperability/handshake failures
Symptoms:
- TLS connection attempts could fail during handshake in pre-stable configurations.

Contributing factors:
- Embedded stack interoperability with server-side TLS policy/ciphersuite negotiation.
- Need for deterministic client profile and TLS hostname handling.
- The compiled SDK copy had drifted from the intended patched state, so the active firmware was missing the TLS profile logic even though project config still expected it.

Resolution:
- SDK TLS glue ciphersuite/profile control + SMTP TLS hostname setup.

Most direct root cause summary:
- `lwipopts.h` requested `ALTCP_MBEDTLS_CLIENT_PROFILE 1`.
- The compiled bundled SDK copy of `altcp_tls_mbedtls.c` did not contain the corresponding client-profile implementation.
- Therefore the setting was effectively ignored at runtime, and TLS negotiation failed before SMTP banner/auth flow.

### 4.2 Runtime observability gaps
Symptoms:
- Limited diagnostics for why TLS read/handshake failed.

Resolution:
- Added optional detailed TLS error logs guarded by compile-time macro.
- Added SMTP DNS/target diagnostics.

### 4.3 Deployment mode mismatch (not code bug)
Symptoms:
- `Run Project` task (`picotool load ... -fx`) returned exit code `-7`.

Likely cause:
- Device not in BOOTSEL-compatible state when using picotool path.

Evidence:
- `Flash` task via OpenOCD completed with exit code `0` and verify/reset.

Resolution:
- Use OpenOCD flash path as primary workflow when debug probe is connected.

### 4.4 Security constraint accepted by design
- Current project notes indicate certificate chain verification is not fully implemented due memory constraints in this target stack profile.
- This means encrypted transport is used, but full peer identity verification is limited.

## 5) Current known-good operating baseline

- Pico SDK: 2.2.0-based with the two source patches above.
- SMTP target: Gmail SMTPS on port 465.
- Build: succeeds.
- Flash: OpenOCD path succeeds reliably in your environment.
- Config baseline: `ALTCP_MBEDTLS_CLIENT_PROFILE=1`, `ALTCP_MBEDTLS_VERBOSE_TLS_ERRORS=0`.

## 6) Recommendations for other developers

1. Use forked SDK tag distribution as source of truth.
2. Keep override scripts only as fallback/emergency compatibility path.
3. Follow DEVELOPER_BUILD.md for setup and RELEASE_CHECKLIST.md for publishing updates.
4. Keep secrets out of committed project config before broad sharing.

## 7) References in this repository

- Project TLS/runtime config: lwipopts.h
- SDK override copy (TLS glue): sdk-overrides/files/src/rp2_common/pico_lwip/altcp_tls_mbedtls.c
- SDK override copy (SMTP): sdk-overrides/files/lib/lwip/src/apps/smtp/smtp.c
- Fork setup guide: DEVELOPER_BUILD.md
- Release process: RELEASE_CHECKLIST.md

## 8) Source Code Change Map (exact locations)

### 8.1 SDK TLS integration file

File:
- `sdk-overrides/files/src/rp2_common/pico_lwip/altcp_tls_mbedtls.c`

Upstream target when applied:
- `src/rp2_common/pico_lwip/altcp_tls_mbedtls.c`

Code points:
- Lines 109-115: added compile-time controls
  - `ALTCP_MBEDTLS_CLIENT_PROFILE`
  - `ALTCP_MBEDTLS_VERBOSE_TLS_ERRORS`
- Around line 321: handshake failure diagnostics gate
  - `#if ALTCP_MBEDTLS_VERBOSE_TLS_ERRORS`
- Around line 328: readable handshake error string
  - `mbedtls_strerror(ret, errbuf, sizeof(errbuf));`
- Around line 460: TLS read failure diagnostics gate
  - `#if ALTCP_MBEDTLS_VERBOSE_TLS_ERRORS`
- Around line 463: readable read-error string
  - `mbedtls_strerror(ret, errbuf, sizeof(errbuf));`
- Around line 901: profile-controlled client ciphersuite block
  - `#if ALTCP_MBEDTLS_CLIENT_PROFILE == 1`
- Around line 911: ciphersuite assignment call
  - `mbedtls_ssl_conf_ciphersuites(&conf->conf, client_ciphersuites);`

### 8.2 lwIP SMTP client file

File:
- `sdk-overrides/files/lib/lwip/src/apps/smtp/smtp.c`

Upstream target when applied:
- `lib/lwip/src/apps/smtp/smtp.c`

Code points:
- Line 69: mbedTLS SSL include for TLS-hostname support
  - `#include "mbedtls/ssl.h"`
- Around line 474: set TLS hostname on SMTP TLS context
  - `tls_ret = mbedtls_ssl_set_hostname(ssl, smtp_server);`
- Around line 476: log if hostname set fails
  - `LWIP_DEBUGF(... "mbedtls_ssl_set_hostname failed: %d\\n", tls_ret);`
- Around line 560: direct-target connection diagnostic
  - `LWIP_PLATFORM_DIAG(("[SMTP] direct target ..."));`
- Around line 893: DNS-resolution connection diagnostic
  - `LWIP_PLATFORM_DIAG(("[SMTP] DNS ..."));`

### 8.3 Project configuration file

File:
- `lwipopts.h`

Code points:
- Line 21:
  - `#define ALTCP_MBEDTLS_CLIENT_PROFILE 1`
- Line 22:
  - `#define ALTCP_MBEDTLS_VERBOSE_TLS_ERRORS 0`

### 8.4 Portable application of the source edits

Scripts used to apply/revert these exact files into SDK trees:
- `sdk-overrides/apply_overrides.ps1`
- `sdk-overrides/revert_overrides.ps1`
