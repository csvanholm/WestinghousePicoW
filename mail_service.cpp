// SPDX-License-Identifier: MIT

#include "lwip/dns.h"
#include <stdlib.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/smtp.h"
#include "lwipopts.h"
#include "hardware/regs/rosc.h"
#include "hardware/structs/rosc.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "mail_service.h"

#include <malloc.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>

extern "C" {
#include "FreeRTOS.h"
#include "task.h"
}

#include "pico/sync.h"

#include "lwip/apps/sntp.h"
#if PICO_RP2040
#include "hardware/rtc.h"
#else
#include "pico/aon_timer.h"
#endif
#include "pico/util/datetime.h"
#include <time.h>

//#define _PICO_W
//must be defined for Pico W but is now set by board type in the CMakeFiles.txt

// Your timezone offset in seconds (e.g., -5 hours for EST = -18000)
#define UTC_OFFSET_SECONDS (-4 * 3600) // 5hr-1hr for EST daylight saving time

// remember to update the to email !!!
const char RECIPIENT_EMAIL[] = ""; // recipient (to) email
const char EMAIL_SUBJECT[] = "Hello world";
const char EMAIL_BODY[] = "Pico SDK LwIP SMTP client.";


PicoMail::QueuedEmail PicoMail::m_outbox[PicoMail::OUTBOX_CAPACITY] = {};
uint8_t PicoMail::m_outboxHead = 0;
uint8_t PicoMail::m_outboxTail = 0;
uint8_t PicoMail::m_outboxCount = 0;
critical_section_t PicoMail::m_outboxLock;
bool PicoMail::m_outboxLockInitialized = false;
uint32_t PicoMail::m_sendTimeoutMs = 30000;
static std::atomic<bool> g_ntpSynced(false);
static volatile bool g_sntpRunning = false;
static volatile bool g_cyw43ArchInitialized = false;
static const char *LwipErrToString(err_t err);
static int get_local_time(char *time, size_t timeSize, char *date, size_t dateSize);

static void MailDelayMs(uint32_t delayMs)
{
  if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
  {
    vTaskDelay(pdMS_TO_TICKS(delayMs));
  } else
  {
    sleep_ms(delayMs);
  }
}

enum class NtpSyncState
{
  Idle,
  Waiting,
  Synced,
  TimedOut,
  Failed
};

static std::atomic<NtpSyncState> g_ntpSyncState(NtpSyncState::Idle);
static uint32_t g_ntpSyncStartMs = 0;
static uint32_t g_ntpSyncTimeoutMs = 0;
static uint32_t g_ntpRetryDelayMs = 15000;

enum class DnsQueryState
{
  Idle,
  Waiting,
  Completed,
  Failed,
  TimedOut
};

static DnsQueryState g_dnsQueryState = DnsQueryState::Idle;
static uint32_t g_dnsQueryStartMs = 0;
static uint32_t g_dnsQueryTimeoutMs = 10000;
static char g_lastConfiguredSmtpHost[64] = {};
static char g_lastConfiguredSender[64] = {};

static bool ends_with_case_insensitive(const char *text, const char *suffix)
{
  if ((text == NULL) || (suffix == NULL))
  {
    return false;
  }

  size_t textLen = strlen(text);
  size_t suffixLen = strlen(suffix);
  if ((suffixLen == 0) || (suffixLen > textLen))
  {
    return false;
  }

  const char *textTail = text + (textLen - suffixLen);
  for (size_t i = 0; i < suffixLen; i++)
  {
    char a = textTail[i];
    char b = suffix[i];
    if ((a >= 'A') && (a <= 'Z'))
    {
      a = (char)(a - 'A' + 'a');
    }
    if ((b >= 'A') && (b <= 'Z'))
    {
      b = (char)(b - 'A' + 'a');
    }
    if (a != b)
    {
      return false;
    }
  }

  return true;
}

static bool contains_case_insensitive(const char *text, const char *needle)
{
  if ((text == NULL) || (needle == NULL) || (needle[0] == '\0'))
  {
    return false;
  }

  size_t textLen = strlen(text);
  size_t needleLen = strlen(needle);
  if (needleLen > textLen)
  {
    return false;
  }

  for (size_t i = 0; i <= (textLen - needleLen); i++)
  {
    bool match = true;
    for (size_t j = 0; j < needleLen; j++)
    {
      char a = text[i + j];
      char b = needle[j];
      if ((a >= 'A') && (a <= 'Z'))
      {
        a = (char)(a - 'A' + 'a');
      }
      if ((b >= 'A') && (b <= 'Z'))
      {
        b = (char)(b - 'A' + 'a');
      }
      if (a != b)
      {
        match = false;
        break;
      }
    }
    if (match)
    {
      return true;
    }
  }

  return false;
}

static bool IsDnsServerUsable(const ip_addr_t *dns)
{
  if (dns == NULL)
  {
    return false;
  }

  char addrText[IPADDR_STRLEN_MAX] = {0};
  if (ipaddr_ntoa_r(dns, addrText, sizeof(addrText)) == NULL)
  {
    return false;
  }
  if ((strcmp(addrText, "0.0.0.0") == 0) || (strcmp(addrText, "::") == 0))
  {
    return false;
  }
  return true;
}

static const char *DnsQueryStateToString(DnsQueryState state)
{
  switch (state)
  {
    case DnsQueryState::Idle:
      return "Idle";
    case DnsQueryState::Waiting:
      return "Waiting";
    case DnsQueryState::Completed:
      return "Completed";
    case DnsQueryState::Failed:
      return "Failed";
    case DnsQueryState::TimedOut:
      return "TimedOut";
    default:
      return "Unknown";
  }
}

static void PrintDnsServerDiagnostics(const char *context)
{
  cyw43_arch_lwip_begin();
  const ip_addr_t *dns1 = dns_getserver(0);
  const ip_addr_t *dns2 = dns_getserver(1);
  char dns1Text[IPADDR_STRLEN_MAX] = {0};
  char dns2Text[IPADDR_STRLEN_MAX] = {0};
  ipaddr_ntoa_r(dns1, dns1Text, sizeof(dns1Text));
  ipaddr_ntoa_r(dns2, dns2Text, sizeof(dns2Text));
  printf("[SMTP] %s host='%s' dns1=%s dns2=%s state=%s\n",
         context != NULL ? context : "DNS diagnostics",
         g_lastConfiguredSmtpHost[0] != '\0' ? g_lastConfiguredSmtpHost : "<unset>",
         dns1Text,
         dns2Text,
         DnsQueryStateToString(g_dnsQueryState));
  cyw43_arch_lwip_end();
}

static void EnsureDnsServersConfigured(const char *reason)
{
  cyw43_arch_lwip_begin();
  const ip_addr_t *dns1 = dns_getserver(0);
  const ip_addr_t *dns2 = dns_getserver(1);
  const bool dns1Unset = !IsDnsServerUsable(dns1);
  const bool dns2Unset = !IsDnsServerUsable(dns2);

  char dns1Text[IPADDR_STRLEN_MAX] = {0};
  char dns2Text[IPADDR_STRLEN_MAX] = {0};
  ipaddr_ntoa_r(dns1, dns1Text, sizeof(dns1Text));
  ipaddr_ntoa_r(dns2, dns2Text, sizeof(dns2Text));

  if (dns1Unset && dns2Unset)
  {
    printf("[SMTP] DNS fallback (%s): dns1=%s dns2=%s. Applying 8.8.8.8/1.1.1.1\n",
           (reason != NULL) ? reason : "unspecified",
           dns1Text,
           dns2Text);

    ip_addr_t publicDns1;
    ip_addr_t publicDns2;
    IP4_ADDR(&publicDns1, 8, 8, 8, 8);
    IP4_ADDR(&publicDns2, 1, 1, 1, 1);
    dns_setserver(0, &publicDns1);
    dns_setserver(1, &publicDns2);
    dns_init();

    dns1 = dns_getserver(0);
    dns2 = dns_getserver(1);
        ipaddr_ntoa_r(dns1, dns1Text, sizeof(dns1Text));
        ipaddr_ntoa_r(dns2, dns2Text, sizeof(dns2Text));
    printf("[SMTP] DNS fallback applied: dns1=%s dns2=%s\n",
          dns1Text,
          dns2Text);
  } else
  if (dns1Unset || dns2Unset)
  {
    printf("[SMTP] Single DHCP DNS server detected (%s): dns1=%s dns2=%s. Keeping DHCP DNS.\n",
           (reason != NULL) ? reason : "unspecified",
           dns1Text,
           dns2Text);
  }

  cyw43_arch_lwip_end();
}

static bool WaitForDhcpDetails(uint32_t timeoutMs)
{
  const uint32_t startMs = to_ms_since_boot(get_absolute_time());
  while ((to_ms_since_boot(get_absolute_time()) - startMs) < timeoutMs)
  {
    cyw43_arch_poll();
    cyw43_arch_lwip_begin();
    const ip4_addr_t *gateway = netif_ip4_gw(netif_default);
    const ip_addr_t *dns1 = dns_getserver(0);
    const ip_addr_t *dns2 = dns_getserver(1);
    const bool hasGateway = !ip4_addr_isany(gateway);
    const bool hasDns = IsDnsServerUsable(dns1) || IsDnsServerUsable(dns2);
    cyw43_arch_lwip_end();

    if (hasGateway && hasDns)
    {
      printf("[DHCP] Gateway/DNS available after %u ms.\n",
             (unsigned)(to_ms_since_boot(get_absolute_time()) - startMs));
      return true;
    }

    MailDelayMs(100);
  }

  PrintDnsServerDiagnostics("DHCP details wait timeout");
  return false;
}


/**
 * @brief Prints field diagnostics used to debug SMTP payload formatting.
 * @param name Field label printed in diagnostics.
 * @param value Field value to inspect for CR/LF/high-bit characters.
 */
static void PrintSmtpFieldDiagnostics(const char *name, const char *value)
{
  if (value == NULL)
  {
    printf("[SMTP] %s: <null>\n", name);
    return;
  }

  size_t len = strlen(value);
  bool hasCr = false;
  bool hasLf = false;
  bool hasHighBit = false;
  size_t firstCr = 0;
  size_t firstLf = 0;
  size_t firstHighBit = 0;

  for (size_t i = 0; i < len; i++)
  {
    unsigned char current = (unsigned char)value[i];
    if ((!hasCr) && (current == '\r'))
    {
      hasCr = true;
      firstCr = i;
    }
    if ((!hasLf) && (current == '\n'))
    {
      hasLf = true;
      firstLf = i;
    }
    if ((!hasHighBit) && ((current & 0x80u) != 0u))
    {
      hasHighBit = true;
      firstHighBit = i;
    }
  }
  printf("[SMTP] %s: len=%u CR=%d LF=%d highBit=%d", name, (unsigned)len, hasCr ? 1 : 0, hasLf ? 1 : 0, hasHighBit ? 1 : 0);
  if (hasCr)
  {
    printf(" firstCR=%u", (unsigned)firstCr);
  }
  if (hasLf)
  {
    printf(" firstLF=%u", (unsigned)firstLf);
  }
  if (hasHighBit)
  {
    printf(" firstHighBit=%u", (unsigned)firstHighBit);
  }
  printf("\n");
}

/**
 * @brief Stops SNTP client if currently running.
 */
static void stop_ntp_client()
{
  if (!g_sntpRunning)
  {
    return;
  }
  cyw43_arch_lwip_begin();
  sntp_stop();
  cyw43_arch_lwip_end();
  g_sntpRunning = false;
}

// Static member definitions
std::atomic<uint8_t> PicoMail::m_emailSent(0);
std::atomic<bool> PicoMail::m_isDnsResolved(false);
std::atomic<bool> PicoMail::m_isConnected(false);
std::atomic<uint8_t> PicoMail::m_lastSmtpResult(0);
std::atomic<uint16_t> PicoMail::m_lastSmtpSrvErr(0);
std::atomic<int16_t> PicoMail::m_lastSmtpErr((int16_t)ERR_OK);

PicoMail::PicoMail() : m_tlsConfig(NULL)
{
  if (!m_outboxLockInitialized)
  {
    critical_section_init(&m_outboxLock);
    m_outboxLockInitialized = true;
  }
  PicoMail::m_emailSent.store(0);
  PicoMail::m_isDnsResolved.store(false);
  PicoMail::m_isConnected.store(false);
  PicoMail::m_lastSmtpResult.store(0);
  PicoMail::m_lastSmtpSrvErr.store(0);
  PicoMail::m_lastSmtpErr.store((int16_t)ERR_OK);
  /*
  SafeCopy(m_smtpServer, sizeof(m_smtpServer), SMTP_SERVER);
  SafeCopy(m_senderEmail, sizeof(m_senderEmail), SENDER_EMAIL);
  SafeCopy(m_senderPassword, sizeof(m_senderPassword), SENDER_PASSWORD);
  SafeCopy(m_recipientEmail, sizeof(m_recipientEmail), RECIPIENT_EMAIL);
  m_smtpPort = (uint16_t)SMTP_PORT;
  */
  }

PicoMail::~PicoMail()
{
  Disconnect(true);
}

bool PicoMail::IsBusySmtpError(err_t err)
{
  return (err == ERR_ISCONN) || (err == ERR_ALREADY) || (err == ERR_INPROGRESS);
}

uint32_t PicoMail::GetNowMs() const
{
  return to_ms_since_boot(get_absolute_time());
}

uint8_t PicoMail::GetOutboxCount()
{
  critical_section_enter_blocking(&m_outboxLock);
  uint8_t count = m_outboxCount;
  critical_section_exit(&m_outboxLock);
  return count;
}

bool PicoMail::IsConnected() const
{
  return m_isConnected.load();
}

const char *PicoMail::GetSenderEmail() const
{
  return m_senderEmail;
}

const char *PicoMail::GetRecipientEmail() const
{
  return m_recipientEmail;
}

void PicoMail::ConfigureRuntimeSmtp(const char *server,
                                    const uint16_t port,
                                    const char *senderEmail,
                                    const char *senderPassword,
                                    const char *recipientEmail)
{
  bool changed = false;

  auto is_ascii_space = [](char c) -> bool 
  {
    return (c == ' ') || (c == '\t') || (c == '\r') || (c == '\n');
  };

  auto copy_trimmed = [&](char *destination, size_t destinationSize, const char *source) -> bool 
  {
    if ((destination == NULL) || (destinationSize == 0) || (source == NULL))
    {
      return false;
    }

    while (is_ascii_space(*source))
    {
      source++;
    }

    size_t len = strlen(source);
    while ((len > 0) && is_ascii_space(source[len - 1]))
    {
      len--;
    }

    if (len == 0)
    {
      return false;
    }

    if (len >= destinationSize)
    {
      len = destinationSize - 1;
    }

    memcpy(destination, source, len);
    destination[len] = '\0';
    return true;
  };

  // Apply each field independently so one invalid/missing value does not block all updates.
  if ((server != NULL) && copy_trimmed(m_smtpServer, sizeof(m_smtpServer), server))
  {
    changed = true;
  }
  if ((senderEmail != NULL) && copy_trimmed(m_senderEmail, sizeof(m_senderEmail), senderEmail))
  {
    changed = true;
  }
  if ((senderPassword != NULL) && (senderPassword[0] != '\0'))
  {
    SafeCopy(m_senderPassword, sizeof(m_senderPassword), senderPassword);
    changed = true;
  }
  if ((recipientEmail != NULL) && copy_trimmed(m_recipientEmail, sizeof(m_recipientEmail), recipientEmail))
  {
    changed = true;
  }
  if (port > 0)
  {
    m_smtpPort = port;
    changed = true;
  }

  printf("[SMTP] Runtime config %s. host='%s' port=%u sender='%s' recipient='%s'\n",
         changed ? "updated" : "unchanged",
         m_smtpServer,
         (unsigned)m_smtpPort,
         m_senderEmail,
         m_recipientEmail);
  SafeCopy(g_lastConfiguredSmtpHost, sizeof(g_lastConfiguredSmtpHost), m_smtpServer);
  SafeCopy(g_lastConfiguredSender, sizeof(g_lastConfiguredSender), m_senderEmail);

  if (contains_case_insensitive(m_smtpServer, "gmail") &&
      !ends_with_case_insensitive(m_senderEmail, "@gmail.com"))
  {
    printf("[SMTP] Warning: smtp.gmail.com usually requires a @gmail.com sender account.\n");
  }
}

void init_platform_clock()
{
#if PICO_RP2040
  rtc_init();
#else
  aon_timer_start_with_timeofday();
#endif
}

static bool set_platform_utc_time(const struct tm *utc_time)
{
  if (utc_time == NULL)
  {
    return false;
  }

#if PICO_RP2040
  datetime_t t = {
      .year  = (int16_t)(utc_time->tm_year + 1900),
      .month = (int8_t)(utc_time->tm_mon + 1),
      .day   = (int8_t)utc_time->tm_mday,
      .dotw  = (int8_t)utc_time->tm_wday,
      .hour  = (int8_t)utc_time->tm_hour,
      .min   = (int8_t)utc_time->tm_min,
      .sec   = (int8_t)utc_time->tm_sec
  };
  return rtc_set_datetime(&t);
#else
  if (!aon_timer_is_running())
  {
    return aon_timer_start_calendar(utc_time);
  }
  return aon_timer_set_time_calendar(utc_time);
#endif
}

static bool get_platform_utc_time(struct tm *utc_time)
{
  if (utc_time == NULL)
  {
    return false;
  }

#if PICO_RP2040
  datetime_t t;
  if (!rtc_get_datetime(&t))
  {
    return false;
  }

  utc_time->tm_sec = t.sec;
  utc_time->tm_min = t.min;
  utc_time->tm_hour = t.hour;
  utc_time->tm_mday = t.day;
  utc_time->tm_mon = t.month - 1;
  utc_time->tm_year = t.year - 1900;
  utc_time->tm_wday = t.dotw;
  utc_time->tm_yday = 0;
  utc_time->tm_isdst = 0;
  return true;
#else
  return aon_timer_get_time_calendar(utc_time);
#endif
}

static bool convert_epoch_to_utc_tm(time_t epoch, struct tm *out_tm)
{
  if (out_tm == NULL)
  {
    return false;
  }

#if defined(_WIN32)
  return gmtime_s(out_tm, &epoch) == 0;
#elif defined(__NEWLIB__) || defined(__GLIBC__) || defined(__APPLE__) || defined(__unix__)
  return gmtime_r(&epoch, out_tm) != NULL;
#else
  struct tm *tmp = gmtime(&epoch);
  if (tmp == NULL)
  {
    return false;
  }
  *out_tm = *tmp;
  return true;
#endif
}

static int64_t days_from_civil(int64_t year, unsigned month, unsigned day)
{
  year -= (month <= 2) ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = (unsigned)(year - era * 400);
  const unsigned doy = (153u * (month + ((month > 2) ? -3 : 9)) + 2u) / 5u + day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static bool convert_utc_tm_to_epoch(const struct tm *utc_tm, time_t *out_epoch)
{
  if ((utc_tm == NULL) || (out_epoch == NULL))
  {
    return false;
  }

  const int64_t year = (int64_t)utc_tm->tm_year + 1900;
  const unsigned month = (unsigned)utc_tm->tm_mon + 1u;
  const unsigned day = (unsigned)utc_tm->tm_mday;
  if ((month < 1u) || (month > 12u) || (day < 1u) || (day > 31u))
  {
    return false;
  }

  const int64_t days = days_from_civil(year, month, day);
  const int64_t seconds = days * 86400ll +
                          (int64_t)utc_tm->tm_hour * 3600ll +
                          (int64_t)utc_tm->tm_min * 60ll +
                          (int64_t)utc_tm->tm_sec;
  *out_epoch = (time_t)seconds;
  return true;
}


/**
 * @brief SNTP synchronization callback that updates RTC from network time.
 * @param sec Seconds value provided by SNTP callback.
 * @param us Microseconds value provided by SNTP callback.
 */
extern "C" void sntp_sync_callback(uint32_t sec, uint32_t us)
{
    // 1. Convert NTP time to Unix epoch
    time_t epoch = (time_t)sec;
    struct tm utc_time = {0};

    (void)us;

    if (!convert_epoch_to_utc_tm(epoch, &utc_time))
    {
      return;
    }

    if (!set_platform_utc_time(&utc_time))
    {
      printf("Failed to set platform clock from SNTP\n");
      return;
    }
    g_ntpSynced.store(true);
    printf("RTC Synchronized to UTC!\n");
}


/**
 * @brief Starts SNTP synchronization without blocking caller execution.
 */
bool start_ntp_sync(uint32_t timeoutMs)
{
  if (!PicoMail::m_isConnected.load())
  {
    printf("Cannot initialize NTP: Not connected to Wi-Fi.\n");
    g_ntpSyncState.store(NtpSyncState::Failed);
    return false;
  }

  g_ntpSynced.store(false);
  g_ntpSyncStartMs = to_ms_since_boot(get_absolute_time());
  g_ntpSyncTimeoutMs = timeoutMs;

  cyw43_arch_lwip_begin();
  if (sntp_enabled())
  {
    sntp_stop();
  }
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, "pool.ntp.org");
  sntp_init();
  g_sntpRunning = (sntp_enabled() != 0);
  cyw43_arch_lwip_end();

  if (!g_sntpRunning)
  {
    printf("[NTP] SNTP failed to start (no UDP PCB available).\n");
    g_ntpSyncState.store(NtpSyncState::Failed);
    return false;
  }

  g_ntpSyncState.store(NtpSyncState::Waiting);
  return true;
}

static const char *ntp_state_message(NtpSyncState state)
{
  switch (state)
  {
    case NtpSyncState::Idle:
      return "NTP sync not started yet";
    case NtpSyncState::Waiting:
      return "waiting for NTP sync";
    case NtpSyncState::Synced:
      return "NTP sync completed";
    case NtpSyncState::TimedOut:
      return "NTP sync timed out (will retry)";
    case NtpSyncState::Failed:
      return "NTP sync failed (will retry)";
  }
  return "NTP status unknown";
}

/**
 * @brief Advances non-blocking SNTP synchronization state machine.
 * @return true when sync finished (success/failure/timeout), false while still waiting.
 */
bool poll_ntp_sync()
{
  const NtpSyncState state = g_ntpSyncState.load();

  // Finalize successful sync even if callback timing races state transitions.
  if (g_ntpSynced.load() && (state != NtpSyncState::Synced))
  {
    uint32_t elapsedMs = to_ms_since_boot(get_absolute_time()) - g_ntpSyncStartMs;
    printf("[NTP] Sync complete after %u ms.\n", elapsedMs);
    stop_ntp_client();
    g_ntpSyncState.store(NtpSyncState::Synced);
    return true;
  }

  if (state != NtpSyncState::Waiting)
  {
    if (state == NtpSyncState::Synced)
    {
      return true;
    }

    if (PicoMail::m_isConnected.load() &&
        ((state == NtpSyncState::Idle) ||
         (state == NtpSyncState::TimedOut) ||
         (state == NtpSyncState::Failed)))
    {
      const uint32_t nowMs = to_ms_since_boot(get_absolute_time());
      if ((nowMs - g_ntpSyncStartMs) >= g_ntpRetryDelayMs)
      {
        printf("[NTP] Auto-retrying sync (%s).\n", ntp_state_message(state));
        (void)start_ntp_sync((g_ntpSyncTimeoutMs > 0) ? g_ntpSyncTimeoutMs : 40000);
      }
    }
    return false;
  }

  if (g_ntpSynced.load())
  {
    uint32_t elapsedMs = to_ms_since_boot(get_absolute_time()) - g_ntpSyncStartMs;
    printf("[NTP] Sync complete after %u ms.\n", elapsedMs);
    stop_ntp_client();
    g_ntpSyncState.store(NtpSyncState::Synced);
    return true;
  }

  if (!PicoMail::m_isConnected.load())
  {
    printf("[NTP] Sync aborted: Wi-Fi disconnected.\n");
    stop_ntp_client();
    g_ntpSyncState.store(NtpSyncState::Failed);
    return true;
  }

  uint32_t elapsedMs = to_ms_since_boot(get_absolute_time()) - g_ntpSyncStartMs;
  if (elapsedMs >= g_ntpSyncTimeoutMs)
  {
    printf("[NTP] Sync timeout after %u ms.\n", g_ntpSyncTimeoutMs);
    stop_ntp_client();
    g_ntpSyncState.store(NtpSyncState::TimedOut);
    return true;
  }

  return false;
}

bool is_ntp_synced()
{
  return g_ntpSynced.load();
}

bool is_wifi_stack_initialized()
{
  return g_cyw43ArchInitialized;
}

void print_runtime_status(PicoMail *mail)
{
  printf("\n[STATUS] ------------------------------\n");
  printf("[STATUS] Wi-Fi       : %s\n", PicoMail::m_isConnected.load() ? "connected" : "disconnected");
  if (mail != NULL)
  {
    printf("[STATUS] Outbox      : %u\n", mail->GetOutboxCount());
  }
  printf("[STATUS] NTP         : %s\n", ntp_state_message(g_ntpSyncState.load()));

  char time[32] = {0};
  char date[32] = {0};
  if (get_local_time(time, sizeof(time), date, sizeof(date)) == 0)
  {
    printf("[STATUS] Local Time  : %s %s\n", date, time);
  } else
  {
    printf("[STATUS] Local Time  : unavailable\n");
  }
  printf("[STATUS] --------------------------------\n\n");
}

/**
 * @brief Reads RTC, applies UTC offset, and formats local time/date strings.
 * @param time Output time string buffer (HH:MM:SS).
 * @param timeSize Size of time buffer.
 * @param date Output date string buffer.
 * @param dateSize Size of date buffer.
 * @return 0 on success; -1 when arguments are invalid or RTC read fails.
 */
static int get_local_time(char *time, size_t timeSize, char *date, size_t dateSize)
{
  if ((time == NULL) || (timeSize == 0) || (date == NULL) || (dateSize == 0))
  {
    return -1;
  }

  // Avoid reporting epoch fallback values before SNTP synchronization finishes.
  if (!g_ntpSynced.load())
  {
    return -1;
  }

  struct tm utc_tm = {0};
  if (get_platform_utc_time(&utc_tm))
  {
    // Reject obviously unsynced/default calendar values.
    if (utc_tm.tm_year < (2020 - 1900))
    {
      return -1;
    }

    time_t utc_epoch = 0;
    if (!convert_utc_tm_to_epoch(&utc_tm, &utc_epoch))
    {
      return -1;
    }

    time_t local_epoch = utc_epoch + UTC_OFFSET_SECONDS;
    struct tm local_tm = {0};

    if (!convert_epoch_to_utc_tm(local_epoch, &local_tm))
    {
      return -1;
    }

    snprintf(time, timeSize, "%02d:%02d:%02d",
                 local_tm.tm_hour,local_tm.tm_min,local_tm.tm_sec);

    snprintf(date, dateSize, "%02d/%02d/%04d",
                local_tm.tm_mon + 1,local_tm.tm_mday,local_tm.tm_year + 1900);

    return 0;
  } else
  {
    return -1;
  }
  return -1;
}


/**
 * @brief Computes estimated total heap size from linker symbols.
 * @return Estimated heap size in bytes.
 */
uint32_t getTotalHeap(void)
{
  extern char __StackLimit, __bss_end__;
  return &__StackLimit - &__bss_end__;
}

struct AllocatorStats
{
  uint32_t allocatedBytes;
  uint32_t largestFreeBlock;
  uint32_t freeBlocks;
  bool available;
};

static AllocatorStats ReadAllocatorStats()
{
#if defined(__GLIBC__) && defined(__GLIBC_MINOR__) && \
  ((__GLIBC__ > 2) || ((__GLIBC__ == 2) && (__GLIBC_MINOR__ >= 33)))
  struct mallinfo2 info = mallinfo2();
  return {
      (uint32_t)info.uordblks,
      (uint32_t)info.ordblks,
      (uint32_t)info.fordblks,
      true};
#elif defined(__NEWLIB__) || defined(__GLIBC__) || defined(__APPLE__) || defined(_WIN32)
  struct mallinfo info = mallinfo();
  return {
      (uint32_t)info.uordblks,
      (uint32_t)info.ordblks,
      (uint32_t)info.fordblks,
      true};
#else
  return {0, 0, 0, false};
#endif
}

/**
 * @brief Computes approximate free heap.
 * @return Estimated free heap bytes.
 */
uint32_t getFreeHeap(void)
{
  const AllocatorStats stats = ReadAllocatorStats();
  if (!stats.available)
  {
    return 0;
  }
  return getTotalHeap() - stats.allocatedBytes;
}

/**
 * @brief Prints heap and local-time diagnostics.
 */
void print_memory_usage()
{
  const AllocatorStats stats = ReadAllocatorStats();
  const uint32_t totalHeap = getTotalHeap();
  if (stats.available)
  {
    const uint32_t freeHeapApprox = totalHeap - stats.allocatedBytes;
    const uint32_t totalTracked = stats.largestFreeBlock + stats.allocatedBytes;
    const float fragmentation = (totalTracked > 0)
                                    ? ((float)stats.largestFreeBlock / (float)totalTracked) * 100.0f
                                    : 0.0f;

    printf("Total allocated: %lu bytes\n", (unsigned long)stats.allocatedBytes);
    printf("Total free (approx): %lu bytes\n", (unsigned long)freeHeapApprox);
    printf("Total heap size: %lu bytes\n", (unsigned long)totalHeap);
    printf("Largest free block: %lu bytes\n", (unsigned long)stats.largestFreeBlock);
    printf("Number of free blocks: %lu\n", (unsigned long)stats.freeBlocks);
    printf("Number of allocated blocks: %lu\n", (unsigned long)(stats.allocatedBytes / 32));
    printf("Fragmentation: %.2f%%\n", fragmentation);
  } else
  {
    printf("Total allocated: unavailable\n");
    printf("Total free (approx): unavailable\n");
    printf("Total heap size: %lu bytes\n", (unsigned long)totalHeap);
    printf("Largest free block: unavailable\n");
    printf("Number of free blocks: unavailable\n");
    printf("Number of allocated blocks: unavailable\n");
    printf("Fragmentation: unavailable\n");
  }

  char time[32]="nosync rtc";
  char date[32]="nosync rtc";
  if (get_local_time(time, sizeof(time), date, sizeof(date))==-1)
  {
    if (!g_ntpSynced.load())
    {
      printf("Local time unavailable: %s\n", ntp_state_message(g_ntpSyncState.load()));
    } else
    {
      printf("Failed to get local time\n");
    }
  } else
  {
    printf("Local Time: %s Date : %s\n", time,date); // Print local time to correlate with memory usage
  }
}


/**
 * @brief Safely copies a source string into a bounded destination buffer.
 */
void PicoMail::SafeCopy(char *destination, size_t destinationSize, const char *source)
{
  if ((destination == NULL) || (destinationSize == 0))
  {
    return;
  }
  snprintf(destination, destinationSize, "%s", (source != NULL) ? source : "");
}


/**
 * @brief Enqueues an email payload in the ring-buffer outbox.
 * @return true when queued; false for invalid arguments or full queue.
 */
bool PicoMail::EnQueueEmail(const char *from, const char *to, const char *subject, const char *body)
{
  // Note better validation for email addresses should be added
  if ((from == NULL) || (to == NULL) || (strchr(from, '@') == NULL) || (strchr(to, '@') == NULL))
  {
    printf("Invalid email address argument. from='%s' to='%s'\n",
           from ? from : "<null>",
           to ? to : "<null>");
    return false;
  }

  critical_section_enter_blocking(&m_outboxLock);
  if (m_outboxCount >= OUTBOX_CAPACITY)
  {
    critical_section_exit(&m_outboxLock);
    printf("Outbox full (%u). Email was not queued.\n", OUTBOX_CAPACITY);
    return false;
  }
  QueuedEmail &slot = m_outbox[m_outboxTail];
  SafeCopy(slot.from, sizeof(slot.from), from);
  SafeCopy(slot.to, sizeof(slot.to), to);
  SafeCopy(slot.subject, sizeof(slot.subject), subject);
  SafeCopy(slot.body, sizeof(slot.body), body);
  slot.retryCount = 0;
  m_outboxTail = (uint8_t)((m_outboxTail + 1) % OUTBOX_CAPACITY);
  m_outboxCount++;
  uint8_t depth = m_outboxCount;
  critical_section_exit(&m_outboxLock);
  printf("Email queued. Outbox depth: %u\n", depth);
  return true;
}


/**
 * @brief Builds a human-readable status email and enqueues it.
 * @return true when the status email is queued; false on enqueue failure.
 */
bool PicoMail::EnQueueGeneratorStatus(GeneratorEvent statusCode)
{
  // this creates a status update and add a timestamp
  char time[32] = "no clock";
  char date[32] = "no date";
  char msg[512];
  if (get_local_time(time, sizeof(time), date, sizeof(date)) == -1)
  {
    // Keep status emails quiet before NTP sync; fallback text remains in message body.
  } else
  {
    printf("NIST Synced Time [%s %s]\n", time,date);
  }
  switch (statusCode)
  {
    case GeneratorEvent::ControllerOnline:    snprintf(msg, sizeof(msg), "Controller is online\nTime: %s\nDate: %s\n", time, date); break;
    case GeneratorEvent::PowerFailure:        snprintf(msg, sizeof(msg), "Power failure detected\nTime: %s\nDate: %s\n", time, date); break;
    case GeneratorEvent::Running:             snprintf(msg, sizeof(msg), "Generator is running\nTime: %s\nDate: %s\n", time, date); break;
    case GeneratorEvent::PowerRestored:       snprintf(msg, sizeof(msg), "Utility power restored\nTime: %s\nDate: %s\n", time, date); break;
    case GeneratorEvent::WeeklyExerciseEnd:   snprintf(msg, sizeof(msg), "Weekly exerciser ended\nTime: %s\nDate: %s\n", time, date); break;
    case GeneratorEvent::WeeklyExerciseStart: snprintf(msg, sizeof(msg), "Weekly exerciser started\nTime: %s\nDate: %s\n", time, date); break;
    case GeneratorEvent::StartFailure:        snprintf(msg, sizeof(msg), "Generator failed to start\nPlease check generator and power cycle the controller\nTime: %s\nDate: %s\n", time, date); break;
    default:                                  snprintf(msg, sizeof(msg), "Unknown status code %d\nTime: %s\nDate: %s\n", (int)statusCode, time, date); break;
  }
  return EnQueueEmail(GetSenderEmail(), GetRecipientEmail(), "Generator Status Update", msg);
}



/**
 * @brief Copies the current head item from outbox without removing it.
 * @return true if an item exists and was copied; false if queue is empty.
 */
bool PicoMail::PeekOutbox(QueuedEmail *email)
{
  if (email == NULL)
  {
    return false;
  }

  critical_section_enter_blocking(&m_outboxLock);
  if (m_outboxCount == 0)
  {
    critical_section_exit(&m_outboxLock);
    return false;
  }

  *email = m_outbox[m_outboxHead];
  critical_section_exit(&m_outboxLock);
  return true;
}

/**
 * @brief Removes the current head item from the outbox queue.
 */
void PicoMail::PopOutbox()
{
  critical_section_enter_blocking(&m_outboxLock);
  if (m_outboxCount == 0)
  {
    critical_section_exit(&m_outboxLock);
    return;
  }
  m_outboxHead = (uint8_t)((m_outboxHead + 1) % OUTBOX_CAPACITY);
  m_outboxCount--;
  critical_section_exit(&m_outboxLock);
}

void PicoMail::IncrementHeadRetryCount()
{
  critical_section_enter_blocking(&m_outboxLock);
  if (m_outboxCount > 0)
  {
    m_outbox[m_outboxHead].retryCount++;
  }
  critical_section_exit(&m_outboxLock);
}

/**
 * @brief Replaces placeholder status-email timestamp fields just before send.
 */
void PicoMail::RefreshQueuedStatusTimestamp(QueuedEmail *email)
{
  if (email == NULL)
  {
    return;
  }

  // Only patch generator status payloads that were queued before RTC was synced.
  if (strcmp(email->subject, "Generator Status Update") != 0)
  {
    return;
  }
  if ((strstr(email->body, "Time: no clock") == NULL) &&
      (strstr(email->body, "Date: no date") == NULL))
  {
    return;
  }

  char localTime[32] = {0};
  char localDate[32] = {0};
  if (get_local_time(localTime, sizeof(localTime), localDate, sizeof(localDate)) != 0)
  {
    return;
  }

  char rebuiltBody[sizeof(email->body)] = {0};
  const char *timeLine = strstr(email->body, "Time:");
  size_t prefixLen = 0;
  if (timeLine != NULL)
  {
    prefixLen = (size_t)(timeLine - email->body);
    if (prefixLen >= sizeof(rebuiltBody))
    {
      prefixLen = sizeof(rebuiltBody) - 1;
    }
    memcpy(rebuiltBody, email->body, prefixLen);
    rebuiltBody[prefixLen] = '\0';
  }

  snprintf(rebuiltBody + prefixLen,
           sizeof(rebuiltBody) - prefixLen,
           "Time: %s\nDate: %s\n",
           localTime,
           localDate);

  SafeCopy(email->body, sizeof(email->body), rebuiltBody);
}

/**
 * @brief Sends queued emails while connected and keeps failed sends queued for retry.
 * @return 0 when queue is empty or successfully drained; -1 when not connected or send fails.
 */
int PicoMail::FlushOutbox()
{
  if (GetOutboxCount() == 0)
  {
    m_flushState = FlushState::Idle;
    m_retryDelayMs = kBusyRetryDelayMs;
    m_busyRetryCount = 0;
    return 0;
  }

  if (m_isConnected.load())
  {
    int linkStatus = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    cyw43_arch_poll();
    if (linkStatus != CYW43_LINK_UP)
    {
      printf("Wi-Fi link lost (status=%d). Marking disconnected and skipping send.\n", linkStatus);
      m_isConnected.store(false);
    }
  }

  if (!m_isConnected.load())
  {
    printf("Outbox flush skipped (not connected). Pending emails: %u\n", GetOutboxCount());
    m_flushState = FlushState::Idle;
    m_retryDelayMs = kBusyRetryDelayMs;
    m_busyRetryCount = 0;
    return -1;
  }

  QueuedEmail email = {};
  if (!PeekOutbox(&email))
  {
    m_flushState = FlushState::Idle;
    return 0;
  }
  RefreshQueuedStatusTimestamp(&email);

  if (m_flushState == FlushState::WaitingForBusyRetry)
  {
    if ((GetNowMs() - m_flushStateStartMs) < m_retryDelayMs)
    {
      return -1;
    }
    m_flushState = FlushState::Idle;
  }

  if (m_flushState == FlushState::WaitingForDns)
  {
    // Only call VerifyDnsAndSend while the query is still in-flight so it can
    // check the application-level timeout.  Do NOT call it when the callback
    // has already set a terminal failure state: VerifyDnsAndSend treats Failed/
    // TimedOut as restartable and would immediately launch a new query, flipping
    // state back to Waiting before we can detect the failure below.
    if (g_dnsQueryState == DnsQueryState::Waiting)
    {
      VerifyDnsAndSend(m_smtpServer);
    }

    if (g_dnsQueryState == DnsQueryState::Waiting)
    {
      return -1;
    }

    if ((g_dnsQueryState == DnsQueryState::Failed) ||
        (g_dnsQueryState == DnsQueryState::TimedOut))
    {
      IncrementHeadRetryCount();
      EnsureDnsServersConfigured("DNS preflight wait-state failure");
      PrintDnsServerDiagnostics("DNS preflight failed");
      m_flushState = FlushState::WaitingForBusyRetry;
      m_retryDelayMs = kDnsRetryDelayMs;
      m_flushStateStartMs = GetNowMs();
      m_busyRetryCount = 0;
      printf("[SMTP] DNS preflight did not resolve '%s'. Backing off retry for %u ms.\n",
             m_smtpServer,
             (unsigned)m_retryDelayMs);
      return -1;
    }

    m_flushState = FlushState::Idle;
  }

  if (m_flushState == FlushState::WaitingForCallback)
  {
    uint8_t sendState = PicoMail::m_emailSent.load();
    if (sendState == 1)
    {
      PopOutbox();
      PicoMail::m_emailSent.store(0);
      m_flushState = FlushState::Idle;
      m_retryDelayMs = kBusyRetryDelayMs;
      m_busyRetryCount = 0;
      return (GetOutboxCount() == 0) ? 0 : -1;
    }

    if (sendState == 2)
    {
      const uint8_t lastResult = m_lastSmtpResult.load();
      const uint16_t lastSrvErr = m_lastSmtpSrvErr.load();
      const err_t lastErr = (err_t)m_lastSmtpErr.load();
      IncrementHeadRetryCount();
      PicoMail::m_emailSent.store(0);
      if ((lastResult == SMTP_RESULT_ERR_SVR_RESP) && (lastSrvErr == 535))
      {
        m_flushState = FlushState::WaitingForBusyRetry;
        m_retryDelayMs = kAuthRetryDelayMs;
        m_flushStateStartMs = GetNowMs();
        m_busyRetryCount = 0;
        printf("[SMTP] Authentication rejected (535). Pausing retries for %u ms.\n",
               (unsigned)m_retryDelayMs);
      } else
      if ((lastResult == SMTP_RESULT_ERR_HOSTNAME) || (lastErr == ERR_ARG))
      {
        PrintDnsServerDiagnostics("Callback hostname failure");
        m_flushState = FlushState::WaitingForBusyRetry;
        m_retryDelayMs = kDnsRetryDelayMs;
        m_flushStateStartMs = GetNowMs();
        m_busyRetryCount = 0;
        printf("[SMTP] DNS/hostname failure detected. Backing off retry for %u ms.\n",
               (unsigned)m_retryDelayMs);
      } else
      {
        m_flushState = FlushState::Idle;
        m_retryDelayMs = kBusyRetryDelayMs;
        m_busyRetryCount = 0;
      }
      printf("Email send failed from callback. Keeping message queued for retry.\n");
      return -1;
    }

    if ((GetNowMs() - m_flushStateStartMs) >= m_sendTimeoutMs)
    {
      IncrementHeadRetryCount();
      PicoMail::m_emailSent.store(0);
      m_flushState = FlushState::Idle;
      m_retryDelayMs = kBusyRetryDelayMs;
      m_busyRetryCount = 0;
      printf("Email send timed out. Keeping message in outbox for retry.\n");

      int linkStatus = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
      cyw43_arch_poll();
      if (linkStatus != CYW43_LINK_UP)
      {
        printf("Wi-Fi link down after timeout (status=%d).\n", linkStatus);
        m_isConnected.store(false);
      }
      return -1;
    }
    return -1;
  }

  VerifyDnsAndSend(m_smtpServer);
  if (g_dnsQueryState == DnsQueryState::Waiting)
  {
    m_flushState = FlushState::WaitingForDns;
    return -1;
  }
  if ((g_dnsQueryState == DnsQueryState::Failed) ||
      (g_dnsQueryState == DnsQueryState::TimedOut))
  {
    IncrementHeadRetryCount();
    EnsureDnsServersConfigured("dns preflight immediate failure");
    PrintDnsServerDiagnostics("DNS preflight failed");
    m_flushState = FlushState::WaitingForBusyRetry;
    m_retryDelayMs = kDnsRetryDelayMs;
    m_flushStateStartMs = GetNowMs();
    m_busyRetryCount = 0;
    printf("[SMTP] DNS preflight did not resolve '%s'. Backing off retry for %u ms.\n",
           m_smtpServer,
           (unsigned)m_retryDelayMs);
    return -1;
  }

  PicoMail::m_emailSent.store(0);
  SafeCopy(g_lastConfiguredSmtpHost, sizeof(g_lastConfiguredSmtpHost), m_smtpServer);
  printf("Sending queued email to %s (retry %u, queue depth %u)...\n", email.to, email.retryCount, GetOutboxCount());

  cyw43_arch_lwip_begin();
  err_t sendResult = smtp_send_mail(email.from, email.to, email.subject, email.body, PicoMail::mailsent_callback, NULL);
  cyw43_arch_lwip_end();

  if (sendResult == ERR_OK)
  {
    m_flushState = FlushState::WaitingForCallback;
    m_flushStateStartMs = GetNowMs();
    return -1;
  }

  if (IsBusySmtpError(sendResult))
  {
    ++m_busyRetryCount;
    if (m_busyRetryCount < kMaxBusyRetries)
    {
      printf("[SMTP] Busy (%d=%s). Retrying in %u ms (%u/%u).\n",
             sendResult,
             LwipErrToString(sendResult),
             (unsigned)kBusyRetryDelayMs,
             (unsigned)m_busyRetryCount,
             (unsigned)kMaxBusyRetries);
      m_retryDelayMs = kBusyRetryDelayMs;
      m_flushState = FlushState::WaitingForBusyRetry;
      m_flushStateStartMs = GetNowMs();
      return -1;
    }
    m_busyRetryCount = 0;
  }

  printf("smtp_send_mail failed immediately: %d (%s)\n", sendResult, LwipErrToString(sendResult));
  if (sendResult == ERR_ARG)
  {
    printf("[SMTP] Immediate ERR_ARG diagnostics:\n");
    printf("[SMTP] Server=%s Port=%u\n", m_smtpServer, (unsigned)m_smtpPort);
#ifdef SMTP_CHECK_DATA
    printf("[SMTP] Build option SMTP_CHECK_DATA=%d\n", SMTP_CHECK_DATA);
#else
    printf("[SMTP] Build option SMTP_CHECK_DATA is undefined\n");
#endif
    PrintSmtpFieldDiagnostics("from", email.from);
    PrintSmtpFieldDiagnostics("to", email.to);
    PrintSmtpFieldDiagnostics("subject", email.subject);
    PrintSmtpFieldDiagnostics("body", email.body);
  }

  IncrementHeadRetryCount();
  m_flushState = FlushState::Idle;

  int linkStatus = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
  cyw43_arch_poll();
  if (linkStatus != CYW43_LINK_UP)
  {
    printf("Wi-Fi link down after immediate SMTP failure (status=%d).\n", linkStatus);
    m_isConnected.store(false);
  }
  return -1;
}

// Callback function triggered when DNS finishes
/**
 * @brief DNS callback used by lwIP asynchronous resolver.
 */
void PicoMail::dns_callback(const char *name, const ip_addr_t *ipaddr, void *arg)
{
  (void) arg;  // to supress compiler warning about unused parameter
  if (ipaddr)
  {
    printf("DNS Success: %s resolved to %s\n", name, ipaddr_ntoa(ipaddr));
    g_dnsQueryState = DnsQueryState::Completed;
  } else
  {
    printf("DNS Failed: Could not resolve %s\n", name);
    g_dnsQueryState = DnsQueryState::Failed;
    PrintDnsServerDiagnostics("DNS callback failure");
  }
  PicoMail::m_isDnsResolved.store(true);
}


/**
 * @brief Issues DNS lookup and advances state without blocking.
 */
void PicoMail::VerifyDnsAndSend(const char *hostname)
{
  if (hostname == NULL)
  {
    return;
  }
  // Start a new async query when idle or after a previous terminal state.
  if ((g_dnsQueryState == DnsQueryState::Idle) ||
      (g_dnsQueryState == DnsQueryState::Completed) ||
      (g_dnsQueryState == DnsQueryState::Failed) ||
      (g_dnsQueryState == DnsQueryState::TimedOut))
  {
    ip_addr_t resolved_ip;
    PicoMail::m_isDnsResolved.store(false);
    g_dnsQueryState = DnsQueryState::Waiting;
    g_dnsQueryStartMs = to_ms_since_boot(get_absolute_time());

    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(hostname, &resolved_ip, PicoMail::dns_callback, NULL);
    cyw43_arch_lwip_end();

    if (err == ERR_OK)
    {
      printf("Cached: %s is %s\n", hostname, ipaddr_ntoa(&resolved_ip));
      PicoMail::m_isDnsResolved.store(true);
      g_dnsQueryState = DnsQueryState::Completed;
      return;
    }

    if (err == ERR_INPROGRESS)
    {
      printf("DNS query in progress for %s...\n", hostname);
      return;
    }

    printf("Error starting DNS query: %d\n", err);
    PicoMail::m_isDnsResolved.store(true);
    g_dnsQueryState = DnsQueryState::Failed;
    return;
  }

  if (g_dnsQueryState == DnsQueryState::Waiting)
  {
    uint32_t elapsedMs = to_ms_since_boot(get_absolute_time()) - g_dnsQueryStartMs;
    if (elapsedMs >= g_dnsQueryTimeoutMs)
    {
      printf("DNS wait timed out for %s after %u ms\n", hostname, g_dnsQueryTimeoutMs);
      PicoMail::m_isDnsResolved.store(true);
      g_dnsQueryState = DnsQueryState::TimedOut;
    }
  }
}

// Call back functions needed by the SDK

// this function is called by mbedtls to get random data for cryptographic operations, such as TLS handshakes.
// in some instances of the SDK the function will generate a linker error if not defined, even if you don't use TLS.
// I have not been able to determine exactly why this is the case, but it may be related to the way the SDK's mbedtls configuration is set up.
// __attribute__((weak)) allows us to provide a default implementation that can be overridden if the sdk provides one
// without this, you may get a linker error about mbedtls_hardware_poll being undefined, even if you don't use TLS or the function is not called.


/**
 * @brief Weak fallback entropy provider for mbedTLS.
 * @return 0 on success.
 */
extern "C" int __attribute__((weak)) mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
  for (size_t i = 0; i < len; i++)
  {
    // Collect hardware randomness from the Ring Oscillator
    output[i] = (unsigned char)(rosc_hw->randombit);
  }
  *olen = len;
  return 0;
}

/**
 * @brief mbedTLS timing hook returning milliseconds since boot.
 * @return Time in milliseconds as mbedTLS timing type.
 */
extern "C" mbedtls_ms_time_t mbedtls_ms_time(void)
{
  // Pico SDK function to get absolute time in microseconds, converted to milliseconds
  return (mbedtls_ms_time_t)(time_us_64() / 1000);
}

// Callback function to handle the result of the email sending attempt
/**
 * @brief SMTP completion callback that records send status and prints diagnostics.
 */
void PicoMail::mailsent_callback(void *arg, u8_t smtp_result, u16_t srv_err, err_t err)
{
  PicoMail::m_lastSmtpResult.store(smtp_result);
  PicoMail::m_lastSmtpSrvErr.store(srv_err);
  PicoMail::m_lastSmtpErr.store((int16_t)err);

  if (smtp_result == SMTP_RESULT_OK)
  {
    PicoMail::m_emailSent.store(1);
    printf("Email sent successfully!\n");
  } else
  {
    PicoMail::m_emailSent.store(2);
    printf("Email failed to send results: 0x%02x, srv_err: 0x%04x, err: %d\n",
           (unsigned int)smtp_result,
           (unsigned int)srv_err,
           (int)err);
    // Descriptive error messages based on smtp_result
    /*
    switch (smtp_result)
    {
      case 1:  // SMTP_RESULT_ERR_UNKNOWN
        printf("Error: Unknown SMTP error occurred.\n");
        printf("Suggestion: Check network connectivity and SMTP server settings.\n");
        printf("DEBUG: Check if TLS certificate verification failed (err=%d may indicate cert error).\n", err);
        break;
      case 2:  // SMTP_RESULT_ERR_CONNECT
        printf("Error: Failed to connect to SMTP server.\n");
        printf("Suggestion: Verify server address, port, and firewall settings. Ensure Wi-Fi is connected.\n");
        printf("DEBUG: TLS handshake may have failed. Check certificate, SNI, or firewall blocking port 465.\n");
        break;
      case 3:  // SMTP_RESULT_ERR_HOSTNAME
        printf("Error: SMTP server hostname resolution failed.\n");
        printf("Suggestion: Check DNS settings and server hostname. Try using IP address instead.\n");
        break;
      case 4:  // SMTP_RESULT_ERR_CLOSED
        printf("Error: SMTP connection was closed unexpectedly.\n");
        printf("Suggestion: Check server stability and network reliability.\n");
        printf("DEBUG: Server may have rejected the connection (possibly TLS or authentication issue).\n");
        break;
      case 5:  // SMTP_RESULT_ERR_TIMEOUT
        printf("Error: SMTP operation timed out.\n");
        printf("Suggestion: Increase timeout settings or check network speed.\n");
        break;
      case 6:  // SMTP_RESULT_ERR_SVR_RESP
        printf("Error: Invalid response from SMTP server.\n");
        printf("Suggestion: Check server compatibility and credentials.\n");
        break;
      case 7:  // SMTP_RESULT_ERR_MEM
        printf("Error: Memory allocation failed.\n");
        printf("Suggestion: Free up memory or increase heap size.\n");
        break;
      case 8:  // SMTP_RESULT_ERR_AUTH
        printf("Error: SMTP authentication failed.\n");
        printf("Suggestion: Verify username and password. Use app-specific passwords if required.\n");
        break;

        default:
        printf("Error: Unspecified SMTP error (code: %d).\n", smtp_result);
        printf("Suggestion: Check lwIP SMTP documentation for this error code.\n");
        break;
    }
    */
    // Additional messages based on srv_err (SMTP response codes)
    if (srv_err != 0)
    {
      printf("SMTP Server Response Code: %d\n", srv_err);
      if (srv_err == 535)
      {
        printf("[SMTP] Auth failed (535). Verify SMTP account/password and app-password requirements.\n");
        printf("[SMTP] Current host='%s' sender='%s'\n",
               g_lastConfiguredSmtpHost,
               g_lastConfiguredSender);
      }
      /*
      switch (srv_err)
      {
        case 421:
          printf("Server response: Service not available.\n");
          printf("Suggestion: Try again later; server may be temporarily down.\n");
          break;
        case 450:
          printf("Server response: Mailbox unavailable.\n");
          printf("Suggestion: Check recipient email address.\n");
          break;
        case 451:
          printf("Server response: Local error in processing.\n");
          printf("Suggestion: Retry the email; may be a temporary server issue.\n");
          break;
        case 452:
          printf("Server response: Insufficient system storage.\n");
           printf("Suggestion: Server storage full; try again later.\n");
          break;
        case 500:
        case 501:
        case 502:
        case 503:
        case 504:
          printf("Server response: Command syntax error.\n");
          printf("Suggestion: Check email format and SMTP client implementation.\n");
          break;
        case 535:
          printf("Server response: Authentication failed.\n");
          printf("Suggestion: Verify credentials and authentication method.\n");
          break;
        case 550:
          printf("Server response: Mailbox unavailable or access denied.\n");
          printf("Suggestion: Check recipient address and sender permissions.\n");
          break;
        case 552:
          printf("Server response: Exceeded storage allocation.\n");
          printf("Suggestion: Recipient mailbox full; contact recipient.\n");
          break;
        case 553:
          printf("Server response: Mailbox name not allowed.\n");
          printf("Suggestion: Invalid email address format.\n");
          break;
        case 554:
          printf("Server response: Transaction failed.\n");
          printf("Suggestion: General server error; check all settings.\n");
          break;
        default:
          printf("Server response: Unhandled code %d.\n", srv_err);
          printf("Suggestion: Refer to RFC 5321 for SMTP response code meanings.\n");
          break;
      }
      */
    }

    // Messages based on err (lwIP err_t)
    if (err != ERR_OK)
    {
      printf("lwIP Error: ");
      switch (err)
      {
        case ERR_MEM:
          printf("Out of memory.\n");
          //printf("Suggestion: Reduce memory usage or increase heap.\n");
          break;
        case ERR_BUF:
          printf("Buffer error.\n");
          //printf("Suggestion: Check buffer sizes in lwIP configuration.\n");
          break;
        case ERR_TIMEOUT:
          printf("Timeout.\n");
          //printf("Suggestion: Increase timeout values or check network.\n");
          break;
        case ERR_RTE:
          printf("Routing problem.\n");
          //printf("Suggestion: Check network routing and gateway.\n");
          break;
        case ERR_INPROGRESS:
          printf("Operation in progress.\n");
          //printf("Suggestion: Wait for operation to complete.\n");
          break;
        case ERR_VAL:
          printf("Illegal value.\n");
          //printf("Suggestion: Check input parameters.\n");
          break;
        case ERR_WOULDBLOCK:
          printf("Operation would block.\n");
          //printf("Suggestion: Use non-blocking calls or wait.\n");
          break;
        case ERR_USE:
          printf("Address in use.\n");
          //printf("Suggestion: Wait for previous connection to close.\n");
          break;
        case ERR_ALREADY:
          printf("Already connected.\n");
          //printf("Suggestion: Close existing connection first.\n");
          break;
        case ERR_ISCONN:
          printf("Already connected.\n");
          //printf("Suggestion: Connection already established.\n");
          break;
        case ERR_CONN:
          printf("Not connected.\n");
          //printf("Suggestion: Establish connection first.\n");
          break;
        case ERR_IF:
          printf("Low-level netif error.\n");
          //printf("Suggestion: Check network interface configuration.\n");
          break;
        case ERR_ABRT:
          printf("Connection aborted.\n");
          //printf("Suggestion: Check for network interruptions.\n");
          break;
        case ERR_RST:
          printf("Connection reset.\n");
          //printf("Suggestion: Server may have reset the connection.\n");
          break;
        case ERR_CLSD:
          printf("Connection closed.\n");
          //printf("Suggestion: Connection was closed by peer.\n");
          break;
        case ERR_ARG:
          printf("Illegal argument.\n");
          //printf("Suggestion: Check function arguments.\n");
          break;
        default:
          printf("Unknown error (code: %d).\n", err);
          //printf("Suggestion: Refer to lwIP err.h for error code definitions.\n");
          break;
      }
    }
  }
}

// this prints out the DHCP info from the router and signal strength
/**
 * @brief Prints network address information, DNS servers, RSSI, and memory data.
 */
void PicoMail::CheckGatewayIpDns()
{
  if (!m_isConnected)
  {
    printf("Cannot check gateway and DNS: Not connected to Wi-Fi.\n");
    return;
  }
  cyw43_arch_lwip_begin();
  // Get IP, Mask, and Gateway
  printf("IP Address   : %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
  printf("Net Mask     : %s\n", ip4addr_ntoa(netif_ip4_netmask(netif_default)));
  printf("Gateway      : %s\n", ip4addr_ntoa(netif_ip4_gw(netif_default)));
  const ip_addr_t *dns1 = dns_getserver(0);
  const ip_addr_t *dns2 = dns_getserver(1);
  printf("DNS Primary  : %s\n", ipaddr_ntoa(dns1));
  printf("DNS Secondary: %s\n", ipaddr_ntoa(dns2));
  cyw43_arch_lwip_end();
  int32_t rssi;
  if (cyw43_wifi_get_rssi(&cyw43_state, &rssi) == 0)
  {
    printf("WiFi RSSI    : %d dBm\n", rssi);
    if (rssi < -70)
    {
      printf("Warning: Weak WiFi signal (RSSI < -70 dBm) may cause connection issues\n");
    }
  }
  printf("Memory Usage:\n");
  print_memory_usage();
}


/**
 * @brief Initializes CYW43 architecture and prepares STA mode.
 * @return 0 on success; -1 on initialization failure.
 */
int PicoMail::InitLwip()
{
  if (g_cyw43ArchInitialized)
  {
    printf("InitLwip() Error : Wi-Fi already initialized, skipping re-init.\n");
    return 0;
  }
  // Initialize the WiFi chip and attempt to connect to the network
  printf("Initializing Wi-Fi...\n");
  // you can also use the more generic cyw43_arch_init() if you don't need to set a specific country code, but some wifi features may not work properly without it
  if (cyw43_arch_init_with_country(CYW43_COUNTRY_USA)) // Adjust country code as needed for your location (e.g., CYW43_COUNTRY_EUROPE, CYW43_COUNTRY_JAPAN)
  {
    DeInitLwip(); // Ensure we clean up the WiFi state before exiting, especially if we failed to initialize, to avoid leaving the driver in a bad state for subsequent tests
    printf("***Wi-Fi init failed***\n\n\n");
    return -1;
  }
  g_cyw43ArchInitialized = true;
  // Allow WiFi chip to stabilize after initialization
  MailDelayMs(500);
  // Needs further investigation!!!!! (not working as expected!) turn off low-power mode to improve performance and range, this will increase power consumption somewhat!
  // Note: Disabling PM can cause instability on some networks - try with PM enabled if issues persist
  cyw43_wifi_pm(&cyw43_state, CYW43_DEFAULT_PM & ~0xf); // Disable low-power mode (keep bit 4 for "keep Wi-Fi on during sleep")
  cyw43_arch_poll();
  cyw43_arch_enable_sta_mode(); // set WiFi mode to client
  cyw43_arch_poll();
  MailDelayMs(1500);
  cyw43_arch_poll();
  return 0;
}

/**
 * @brief Deinitializes CYW43 architecture if initialized.
 * @return 0 on success; -1 when there is nothing to deinitialize.
 */
int PicoMail::DeInitLwip()
{
  // Initialize the WiFi chip and attempt to connect to the network
  printf("DeInitLwIP...\n");
  if (g_cyw43ArchInitialized)
  {
    cyw43_arch_deinit();
  } else
  {
    printf("Wi-Fi was not initialized, skipping deinit.\n");
    return -1;
  }
  g_cyw43ArchInitialized = false;
  return 0;
}


/**
 * @brief Converts CYW43 Wi-Fi connection error code to text.
 * @return Static string describing the error.
 */
const char *PicoMail::WifiConnectErrorToString(int err)
{
  switch (err)
  {
    case  0: return "Success";
    case -1: return "Timeout";
    case -2: return "Bad authentication (wrong password)";
    case -3: return "Connection failed";
    default: return "Unknown error";
  }
}

/**
 * @brief Converts lwIP err_t values to symbolic text.
 * @return Static string representation of the error value.
 */
static const char *LwipErrToString(err_t err)
{
  switch (err)
  {
    case ERR_OK: return "ERR_OK";
    case ERR_MEM: return "ERR_MEM";
    case ERR_BUF: return "ERR_BUF";
    case ERR_TIMEOUT: return "ERR_TIMEOUT";
    case ERR_RTE: return "ERR_RTE";
    case ERR_INPROGRESS: return "ERR_INPROGRESS";
    case ERR_VAL: return "ERR_VAL";
    case ERR_WOULDBLOCK: return "ERR_WOULDBLOCK";
    case ERR_USE: return "ERR_USE";
    case ERR_ALREADY: return "ERR_ALREADY";
    case ERR_ISCONN: return "ERR_ISCONN";
    case ERR_CONN: return "ERR_CONN";
    case ERR_IF: return "ERR_IF";
    case ERR_ABRT: return "ERR_ABRT";
    case ERR_RST: return "ERR_RST";
    case ERR_CLSD: return "ERR_CLSD";
    case ERR_ARG: return "ERR_ARG";
    default: return "ERR_UNKNOWN";
  }
}

/**
 * @brief Connects to Wi-Fi with retry, waits for IP, and configures SMTP transport.
 * @return 0 on successful network and SMTP setup; -1 on failure.
 */
int PicoMail::WifiConnect(const char *ssid, const char *password, bool force_dns)
{
  // Add some simple retry logic for WiFi connection
  // retrying the connection can help recover from transient issues, and the forced polling/starvation
  // fix can help clear out any internal states in the WiFi driver that might be causing connection failures, especially
  // after multiple runs of the test without a reset. The exponential backoff helps to avoid overwhelming the network
  // or the WiFi chip with rapid retries, which can sometimes make things worse.
  int retry_count = 0;
  const int max_retries = 3; // you can adjust this as needed, but 3 attempts is usually a good balance between giving it a chance to recover and not getting stuck in a long loop of retries.
  int connect_result;
  do
  {
    printf("WiFi connection attempt %d/%d...\n", retry_count + 1, max_retries);
    connect_result = cyw43_arch_wifi_connect_timeout_ms(ssid, password, CYW43_AUTH_WPA2_AES_PSK, 10000);
    if (connect_result == 0)
    {
      printf("WiFi connected on attempt %d\n", retry_count + 1);
      cyw43_arch_poll();
      break;
    } else
    {
      printf("WiFi connection failed on attempt %d, error: %s (%d)\n", retry_count + 1, WifiConnectErrorToString(connect_result), connect_result);
      retry_count++;
      // Starvation Fix: Force-poll the driver for 500ms to clear internal states
      for (int i = 0; i < 50; i++)
      {
        cyw43_arch_poll();
        MailDelayMs(10);
      }
      if (retry_count < max_retries)
      {
        // Wait before retrying, with exponential backoff
        int wait_time = 2000 * retry_count; // 2s, 4s, 6s
        printf("Waiting %dms before retry...\n", wait_time);
        MailDelayMs(wait_time);
        // Reset WiFi state before retry
        cyw43_arch_disable_sta_mode();
        MailDelayMs(1000);
        cyw43_arch_enable_sta_mode();
      }
    }
    cyw43_arch_poll();
  } while (retry_count < max_retries);
  // Check final connection result
  if (connect_result != 0)
  {
    m_isConnected.store(false);
    if (g_cyw43ArchInitialized)
    {
      cyw43_arch_disable_sta_mode();
      MailDelayMs(200);
      cyw43_arch_enable_sta_mode();
      cyw43_arch_poll();
    }
    printf("Failed to connect to Wi-Fi after %d attempts: %s (%d)\n", max_retries, WifiConnectErrorToString(connect_result), connect_result);
    return -1;
  }
  // After AP mode, cyw43_arch_disable_ap_mode() tears down the AP radio but does NOT
  // call cyw43_cb_tcpip_deinit(), so netif_default remains pointed at the AP netif
  // (still holding 192.168.0.1).  The DHCP IP check below would then see that stale
  // address, report it as valid, and exit immediately without waiting for the actual
  // router-assigned lease.  Restore netif_default to the STA netif before the check.
  cyw43_arch_lwip_begin();
  {
    struct netif *sta_netif = &cyw43_state.netif[CYW43_ITF_STA];
    if (netif_default != sta_netif)
    {
      printf("Restoring STA netif as default (displaced by AP mode, current IP: %s)\n",
             ip4addr_ntoa(netif_ip4_addr(netif_default)));
      netif_set_default(sta_netif);
    }
  }
  cyw43_arch_lwip_end();

  // Wait until we get assigned IP
  // Netgear routers are notoriously slow to assign IPs, so we need to wait here
  // before trying to send email or we will get a timeout error from the SMTP client
  printf("Waiting for IP assignment...\n");
  uint32_t start_time = time_us_32();
  while (true)
  {
    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    cyw43_arch_poll(); // Poll to ensure network stack is updated and we can get the link status
    if (status == CYW43_LINK_UP)
    {
      printf("Link is up, checking IP...\n");
      // Check if we have a valid IP? Some routers take their sweet time to assign an IP, and we
      // need to wait for that before we can do anything useful, otherwise we will just get timeouts
      // from the SMTP client when it tries to connect to the server.
      cyw43_arch_lwip_begin();
      const ip4_addr_t *ip = netif_ip4_addr(netif_default);
      bool hasIp = !ip4_addr_isany(ip);
      if (hasIp)
      {
        printf("IP assigned  : %s\n", ip4addr_ntoa(ip));
        cyw43_arch_lwip_end();
        break;
      }
      cyw43_arch_lwip_end();
    }
    // Timeout after 30 seconds
    if (time_us_32() - start_time > 30000000)
    {
      printf("Timeout waiting for IP assignment\n");
      m_isConnected.store(false);
      if (g_cyw43ArchInitialized)
      {
        cyw43_arch_disable_sta_mode();
        MailDelayMs(200);
        cyw43_arch_enable_sta_mode();
        cyw43_arch_poll();
      }
      return -1;
    }
    MailDelayMs(100); // Check every 100ms
  }

  // Some routers assign IP first and populate DNS/gateway shortly after.
  // Give DHCP a small additional window before applying fallback DNS servers.
  WaitForDhcpDetails(15000);

  // if DNS verification fails we can set the DNS servers to known public servers like Google's or Cloudflare's, but
  // this should not be necessary and may indicate a deeper issue with the network configuration or the Wi-Fi driver if it is.
  // check_dns_servers(); // <<<--- Uncomment this to see what DNS servers are currently set, which
  // force DNS servers to a known public servers for testing - this should not be necessary if the WiFi router's DHCP
  //  is working properly, and if it is not working there may be deeper issues with the Wi-Fi driver that need to be addressed. You can uncomment this section to force the use of public DNS servers like Google's or Cloudflare's, which can help ensure that DNS resolution works for testing purposes, but ideally you should investigate and fix any underlying issues with the DHCP/DNS configuration in your network or Wi-Fi driver if you find that you need to do this.
  if (force_dns)
  {
    ip_addr_t dns_server1;
    IP4_ADDR(&dns_server1, 8, 8, 8, 8); // Google Public DNS
    cyw43_arch_lwip_begin();
    dns_setserver(0, &dns_server1);     // 0 is the primary DNS
    ip_addr_t dns_server2;
    IP4_ADDR(&dns_server2, 1, 1, 1, 1); // Cloudflare Public DNS
    dns_setserver(1, &dns_server2);     // 1 is the secondary DNS
    dns_init();                         // re-initialize DNS to clear cache and apply new servers
    cyw43_arch_lwip_end();
  } else
  {
    // Some DHCP paths occasionally leave DNS servers unset (0.0.0.0).
    // Apply a fallback pair only when unset to avoid unnecessary overrides.
    EnsureDnsServersConfigured("post-connect DHCP validation");
  }

  printf("Connected to Wi-Fi\n\n");
  m_isConnected.store(true);

  // Port 465 is the only port supported by the lwIP SMTP client with TLS,
  // which uses implicit TLS. Port 587 with STARTTLS is not currently supported by
  // the lwIP SMTP client, and attempting to use it will likely result in connection
  // failures or timeouts, because the client will not be able to properly negotiate the
  // TLS connection with the server. If you want to use port 587 with STARTTLS,
  // you would need to implement the STARTTLS negotiation yourself in your code before calling
  // smtp_send_mail, which can be complex and is not currently provided by the lwIP
  // SMTP client out of the box. For testing purposes, it's recommended to use port 465
  // with a proper TLS configuration if you want to send emails securely using the lwIP SMTP client.

  if (m_smtpPort == 465)
  {
    m_tlsConfig = altcp_tls_create_config_client(NULL, 0);
    if (m_tlsConfig != NULL)
    {
      cyw43_arch_lwip_begin();
      smtp_set_tls_config(m_tlsConfig);
      cyw43_arch_lwip_end();
    }
  } else
  if (m_smtpPort == 587)
  {
    printf("Port 587 (STARTTLS): Not supported by lwIP SMTP client.\n");
    m_tlsConfig = NULL;
  }
  // Configure SMTP server settings
  cyw43_arch_lwip_begin();
  smtp_set_server_addr(m_smtpServer);
  smtp_set_server_port(m_smtpPort);
  smtp_set_auth(m_senderEmail, m_senderPassword);
  cyw43_arch_lwip_end();
  if (m_outboxCount > 0)
  {
    printf("Attempting to flush %u queued email(s) after connect...\n", m_outboxCount);
    FlushOutbox();
  }
  return 0;
}

/**
 * @brief Convenience wrapper to initialize and connect Wi-Fi/SMTP stack.
 * @return 0 on success; -1 on failure.
 */
int PicoMail::Connect()
{
  // Prefer runtime credentials cached from a prior ConnectWithCredentials() call
  // (runtime-provisioning / AP-config path).  Fall back to compile-time macros
  // only when no runtime credentials have been stored yet, so that non-provisioning
  // builds that supply WIFI_SSID/WIFI_PASSWORD via CMake flags continue to work.
  const char *ssid     = (m_wifiSsid[0] != '\0') ? m_wifiSsid     : WIFI_SSID;
  const char *password = (m_wifiSsid[0] != '\0') ? m_wifiPassword : WIFI_PASSWORD;
  return ConnectWithCredentials(ssid, password, false);
}

int PicoMail::ConnectWithCredentials(const char *ssid, const char *password, bool force_dns)
{
  if ((ssid == NULL) || (password == NULL) || (ssid[0] == '\0'))
  {
    printf("Connect aborted: missing runtime Wi-Fi credentials.\n");
    return -1;
  }

  // Cache credentials so that subsequent Connect() calls (e.g. reconnect) work
  // without needing the caller to pass them in again.
  SafeCopy(m_wifiSsid, sizeof(m_wifiSsid), ssid);
  SafeCopy(m_wifiPassword, sizeof(m_wifiPassword), password);

  if (m_isConnected) // prevent multiple simultaneous connection attempts, which can cause issues with the Wi-Fi driver and lead to connection failures
  {
    printf("Already connected to Wi-Fi\n");
    return 0;
  }
  if (!is_wifi_stack_initialized() && (InitLwip() != 0))
  {
    printf("Connect aborted: Wi-Fi/lwIP init failed.\n");
    return -1;
  }
  return WifiConnect(ssid, password, force_dns);
}

/**
 * @brief Stops network-dependent services and releases TLS/network resources.
 * @return Always returns 0.
 */
int PicoMail::Disconnect(bool hardDeinit)
{
  // free TLS memory and de-initialize
  stop_ntp_client();
  if (m_tlsConfig)
  {
    altcp_tls_free_config(m_tlsConfig);
    m_tlsConfig = NULL;
  }
  if (hardDeinit)
  {
    if (g_cyw43ArchInitialized)
    {
      DeInitLwip();
    }
  } else
  {
    if (g_cyw43ArchInitialized)
    {
      cyw43_arch_disable_sta_mode();
      cyw43_arch_poll();
    }
  }
  m_isConnected.store(false);
  g_ntpSynced.store(false);
  g_ntpSyncState.store(NtpSyncState::Idle);
  return 0;
}

/**
 * @brief Queues an email request for deferred send.
 * @return 0 when queued; -2 if queue operation fails.
 */
int PicoMail::SendEmail(const char *from, const char *to, const char *subject, const char *body)
{
  if (!EnQueueEmail(from, to, subject, body))
  {
    printf("Failed to enqueue email.\n");
    return -2;
  }
  return 0;  // Do not flush yet. Flush in the main loop or after connecting so emails can be queued while Wi-Fi is down and sent once a connection is available.
}

