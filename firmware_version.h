/**
 * @file firmware_version.h
 * @brief Centralized firmware version definition for UI, status output, and diagnostics.
 *
 * Purpose:
 * - exposes a single firmware version string for status pages and logs
 * - allows runtime code and the config portal to report the build version consistently
 *
 * Relationships:
 * - http_server.c includes this header to render the running firmware version in the setup portal
 * - any status/reporting code can depend on this value without duplicating version strings
 */
#ifndef FIRMWARE_VERSION_H
#define FIRMWARE_VERSION_H

#define APP_FIRMWARE_VERSION "1.0.12"

#endif // FIRMWARE_VERSION_H
