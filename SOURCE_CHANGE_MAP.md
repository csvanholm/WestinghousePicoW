# Source Code Change Map

Date: 2026-02-25

This report lists the concrete source-code edits made for the SMTP/TLS stabilization work and shows exactly where they are in code.

## 1) SDK TLS integration changes

Project mirror file:
- `sdk-overrides/files/src/rp2_common/pico_lwip/altcp_tls_mbedtls.c`

Upstream SDK target path (when applied):
- `src/rp2_common/pico_lwip/altcp_tls_mbedtls.c`

### 1.1 Added compile-time TLS profile + verbosity controls
- Lines 109-115
- Added:
  - `ALTCP_MBEDTLS_CLIENT_PROFILE`
  - `ALTCP_MBEDTLS_VERBOSE_TLS_ERRORS`

Purpose:
- Control deterministic TLS client behavior.
- Enable detailed diagnostics only when explicitly requested.

### 1.2 Added handshake failure diagnostics (gated)
- Around lines 321-343
- Added guarded logging block under:
  - `#if ALTCP_MBEDTLS_VERBOSE_TLS_ERRORS`
- Uses `mbedtls_strerror(...)` for readable TLS error output.

Purpose:
- Improve root-cause visibility for handshake failures during bring-up.

### 1.3 Added TLS read failure diagnostics (gated)
- Around lines 460-466
- Added guarded error detail path for `mbedtls_ssl_read` failures.

Purpose:
- Distinguish benign close/reset cases from actionable TLS read errors.

### 1.4 Added stable client ciphersuite profile
- Around lines 901-911
- Added profile-controlled ciphersuite list and call to:
  - `mbedtls_ssl_conf_ciphersuites(&conf->conf, client_ciphersuites)`

Purpose:
- Restrict client negotiation to known-good RSA-CBC ciphers in this target environment.

## 2) lwIP SMTP client changes

Project mirror file:
- `sdk-overrides/files/lib/lwip/src/apps/smtp/smtp.c`

Upstream SDK target path (when applied):
- `lib/lwip/src/apps/smtp/smtp.c`

### 2.1 Added mbedTLS SSL include for TLS hostname setup
- Line 69
- Added include:
  - `#include "mbedtls/ssl.h"`

Purpose:
- Access TLS context API needed to set SMTP TLS hostname.

### 2.2 Set TLS hostname on SMTP TLS context
- Around lines 474-476 (inside SMTP PCB setup)
- Added:
  - `mbedtls_ssl_set_hostname(ssl, smtp_server)`
  - warning log if it fails

Purpose:
- Ensure TLS host context is set from configured SMTP server, improving interop/reliability.

### 2.3 Added direct-target SMTP diagnostic
- Line 560
- Added diagnostic:
  - `[SMTP] direct target ...`

Purpose:
- Make non-DNS connection target/port visible in runtime logs.

### 2.4 Added DNS resolution SMTP diagnostic
- Line 893
- Added diagnostic:
  - `[SMTP] DNS <host> -> <ip>:<port>`

Purpose:
- Confirm DNS resolution result and actual endpoint used for connect.

## 3) Project-level configuration changes

File:
- `lwipopts.h`

### 3.1 Enabled stable TLS profile
- Line 21
- `#define ALTCP_MBEDTLS_CLIENT_PROFILE 1`

### 3.2 Disabled verbose TLS errors by default
- Line 22
- `#define ALTCP_MBEDTLS_VERBOSE_TLS_ERRORS 0`

Purpose:
- Keep stable runtime behavior by default while allowing opt-in diagnostics.

## 4) How these changes are applied

The two SDK/lwIP source patches are carried in project mirrors and applied by script:
- `sdk-overrides/files/...` (patched source copies)
- `sdk-overrides/apply_overrides.ps1`
- `sdk-overrides/revert_overrides.ps1`

This allows reproducible builds for other developers without manual SDK editing.
