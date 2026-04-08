// SPDX-License-Identifier: MIT

#pragma once

#include "mail_service.h"

#include <atomic>
#include <cstdio>
#include <cstdint>

#include "lwip/err.h"

inline constexpr bool kVerboseMailLogs = false;

#define MAIL_LOG_DEBUG(...)            \
  do                                  \
  {                                   \
    if (kVerboseMailLogs)             \
    {                                 \
      printf(__VA_ARGS__);            \
    }                                 \
  } while (0)

void service_onboard_led_pulse();

enum class DnsQueryState
{
  Idle,
  Waiting,
  Completed,
  Failed,
  TimedOut
};

extern std::atomic<DnsQueryState> g_dnsQueryState;
extern uint32_t g_dnsQueryStartMs;
extern uint32_t g_dnsQueryTimeoutMs;
extern std::atomic<bool> g_dnsResultPending;
extern std::atomic<uint32_t> g_dnsResolvedIpv4Raw;
extern std::atomic<bool> g_dnsResolvedIpv4Valid;
extern std::atomic<bool> g_smtpClosedDiagnosticsPrinted;

void MailDelayMs(uint32_t delayMs);
const char *LwipErrToString(err_t err);
const char *DnsQueryStateToString(DnsQueryState state);
void stop_ntp_client();
void reset_ntp_sync_state();
int get_local_time(char *time, size_t timeSize, char *date, size_t dateSize);
bool get_local_time_for_ms(uint32_t timestampMs,
                           char *time,
                           size_t timeSize,
                           char *date,
                           size_t dateSize);

void PrintDnsServerDiagnostics(const char *context);
void EnsureDnsServersConfigured(const char *reason);
void print_memory_usage();
