// SPDX-License-Identifier: MIT

#include "lwip/dns.h"

#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/ip_addr.h"
#include "mail_service_internal.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
#include "FreeRTOS.h"
#include "task.h"
}

#include "pico/sync.h"

PicoMail::QueuedEmail PicoMail::m_outbox[PicoMail::OUTBOX_CAPACITY] = {};
uint8_t PicoMail::m_outboxHead = 0;
uint8_t PicoMail::m_outboxTail = 0;
uint8_t PicoMail::m_outboxCount = 0;
critical_section_t PicoMail::m_outboxLock;
bool PicoMail::m_outboxLockInitialized = false;
uint32_t PicoMail::m_sendTimeoutMs = 30000;
std::atomic<uint8_t> PicoMail::m_emailSent(0);
std::atomic<bool> PicoMail::m_isDnsResolved(false);
std::atomic<bool> PicoMail::m_isConnected(false);
std::atomic<uint8_t> PicoMail::m_lastSmtpResult(0);
std::atomic<uint16_t> PicoMail::m_lastSmtpSrvErr(0);
std::atomic<int16_t> PicoMail::m_lastSmtpErr((int16_t)ERR_OK);

static char g_lastConfiguredSmtpHost[64] = {};

/**
 * @brief Delays for the requested time using RTOS delay when available.
 * @param delayMs Delay duration in milliseconds.
 */
void MailDelayMs(uint32_t delayMs)
{
  if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
  {
    static constexpr uint32_t kMailDelaySliceMs = 5;
    uint32_t remainingMs = delayMs;
    while (remainingMs > 0)
    {
      service_onboard_led_pulse();

      const uint32_t sleepMs = (remainingMs > kMailDelaySliceMs) ? kMailDelaySliceMs : remainingMs;
      vTaskDelay(pdMS_TO_TICKS(sleepMs));
      remainingMs -= sleepMs;
    }

    service_onboard_led_pulse();
  } else
  {
    sleep_ms(delayMs);
  }
}

/**
 * @brief Case-insensitive suffix test for ASCII strings.
 * @param text Full text to inspect.
 * @param suffix Suffix candidate.
 * @return true when @p text ends with @p suffix; otherwise false.
 */
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

/**
 * @brief Case-insensitive substring search for ASCII strings.
 * @param text Text to search.
 * @param needle Substring to locate.
 * @return true when @p needle appears in @p text; otherwise false.
 */
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

/**
 * @brief Validates that a DNS server entry contains a usable non-zero address.
 * @param dns DNS server address returned by lwIP.
 * @return true when the server appears usable; otherwise false.
 */
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

/**
 * @brief Prints current DNS server state for SMTP troubleshooting.
 * @param context Optional diagnostic context label.
 */
void PrintDnsServerDiagnostics(const char *context)
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
         DnsQueryStateToString(g_dnsQueryState.load()));
  cyw43_arch_lwip_end();
}

/**
 * @brief Applies public DNS fallback only when DHCP leaves both slots unusable.
 * @param reason Textual reason printed in diagnostics.
 */
void EnsureDnsServersConfigured(const char *reason)
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
}

PicoMail::~PicoMail()
{
  Disconnect(true);
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

  if ((server != NULL) && copy_trimmed(m_smtpServer, sizeof(m_smtpServer), server))
  {
  }
  if ((senderEmail != NULL) && copy_trimmed(m_senderEmail, sizeof(m_senderEmail), senderEmail))
  {
  }
  if ((senderPassword != NULL) && (senderPassword[0] != '\0'))
  {
    SafeCopy(m_senderPassword, sizeof(m_senderPassword), senderPassword);
  }
  if ((recipientEmail != NULL) && copy_trimmed(m_recipientEmail, sizeof(m_recipientEmail), recipientEmail))
  {
  }
  if (port > 0)
  {
    m_smtpPort = port;
  }

  SafeCopy(g_lastConfiguredSmtpHost, sizeof(g_lastConfiguredSmtpHost), m_smtpServer);

  if (contains_case_insensitive(m_smtpServer, "gmail") &&
      !ends_with_case_insensitive(m_senderEmail, "@gmail.com"))
  {
    printf("[SMTP] Warning: smtp.gmail.com usually requires a @gmail.com sender account.\n");
  }
}

void PicoMail::SetRuntimeWifiCredentials(const char *ssid, const char *password)
{
  if ((ssid == NULL) || (ssid[0] == '\0'))
  {
    return;
  }

  SafeCopy(m_wifiSsid, sizeof(m_wifiSsid), ssid);
  if (password != NULL)
  {
    SafeCopy(m_wifiPassword, sizeof(m_wifiPassword), password);
  } else
  {
    m_wifiPassword[0] = '\0';
  }
}

const char *LwipErrToString(err_t err)
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
