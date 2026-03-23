# PicoMail SMTP/TLS Patch Executive Summary

Date: 2026-02-25

## Objective

Restore reliable SMTP email delivery from Pico W using Gmail SMTPS (`smtp.gmail.com:465`) and make the solution reproducible for other developers.

## Outcome

- Email send path is stable with current patched baseline.
- Build and OpenOCD flash flows are successful.
- Team distribution workflow is in place (forked SDK tag + fallback override scripts).

## Why SDK patching was required

The critical failures were below application level, in Pico SDK/lwIP TLS-SMTP integration.

- App-layer changes alone could not control TLS negotiation behavior in the SDK TLS glue.
- SMTP TLS session setup needed lwIP SMTP source-level adjustment.

## What changed (high level)

1. SDK TLS glue patch (`altcp_tls_mbedtls.c`)
   - Added compile-time TLS client profile selection.
   - Added optional detailed TLS failure diagnostics.
   - Stabilized client ciphersuite profile used by this target.

2. lwIP SMTP patch (`smtp.c`)
   - Set TLS hostname on SMTP TLS context.
   - Added SMTP DNS/target diagnostics for troubleshooting.

3. Project config (`lwipopts.h`)
   - Enabled stable client profile.
   - Left verbose TLS diagnostics disabled by default.

4. Portability workflow
   - Added scripts/docs for applying overrides and for consuming a pinned forked SDK tag.

## Failure modes addressed

1. TLS interoperability/handshake instability
   - Fixed by SDK TLS profile controls and SMTP TLS hostname setup.

2. Limited failure visibility during debugging
   - Fixed by optional verbose TLS diagnostics and added SMTP connection diagnostics.

3. Deployment method mismatch (not firmware logic defect)
   - `Run Project` (picotool) can fail when device is not in BOOTSEL mode (observed exit `-7`).
   - `Flash` via OpenOCD succeeds consistently in current setup.

## Current known-good baseline

- Pico SDK line: 2.2.0 with two source patches.
- SMTP mode: Gmail SMTPS 465.
- Build: successful.
- Flash: OpenOCD verified/reset success.
- Config defaults: stable TLS profile on, verbose TLS errors off.

## Distribution recommendation

- Preferred: publish and pin a forked SDK tag (`v2.2.0-picomail.N`) and have developers consume that tag.
- Fallback: use project override scripts when immediate fork adoption is not possible.

## Risks / constraints

- Full certificate-chain verification remains limited by current memory/stack constraints in this profile.
- Shared repo currently contains credential values in configuration and should be sanitized before broad distribution.

## Reference documents

- Full technical detail: PATCH_REPORT.md
- Developer setup: DEVELOPER_BUILD.md
- Release process: RELEASE_CHECKLIST.md
