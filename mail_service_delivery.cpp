// SPDX-License-Identifier: MIT

#include "mail_service_internal.h"

#include <cstring>

#include "mbedtls/platform_time.h"
#include "lwip/dns.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "hardware/regs/rosc.h"
#include "hardware/structs/rosc.h"

std::atomic<DnsQueryState> g_dnsQueryState(DnsQueryState::Idle);
uint32_t g_dnsQueryStartMs = 0;
uint32_t g_dnsQueryTimeoutMs = 10000;
std::atomic<bool> g_dnsResultPending(false);
std::atomic<uint32_t> g_dnsResolvedIpv4Raw(0);
std::atomic<bool> g_dnsResolvedIpv4Valid(false);
std::atomic<bool> g_smtpClosedDiagnosticsPrinted(false);

namespace
{
void PrintSmtpFieldDiagnostics(const char *name, const char *value)
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

void LogPendingDnsResult(const char *hostname)
{
  if (!g_dnsResultPending.exchange(false))
  {
    return;
  }

  const DnsQueryState state = g_dnsQueryState.load();
  if (state == DnsQueryState::Completed)
  {
    char addrText[IPADDR_STRLEN_MAX] = {0};
    if (g_dnsResolvedIpv4Valid.load())
    {
      ip4_addr_t resolvedAddr;
      resolvedAddr.addr = g_dnsResolvedIpv4Raw.load();
      ip4addr_ntoa_r(&resolvedAddr, addrText, sizeof(addrText));
    } else
    {
      snprintf(addrText, sizeof(addrText), "%s", "<non-ipv4>");
    }

    printf("DNS Success: %s resolved to %s\n", hostname, addrText);
  } else
  if (state == DnsQueryState::Failed)
  {
    printf("DNS Failed: Could not resolve %s\n", hostname);
    PrintDnsServerDiagnostics("DNS callback failure");
  }
}
}

const char *DnsQueryStateToString(DnsQueryState state)
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

bool PicoMail::IsBusySmtpError(err_t err)
{
  return (err == ERR_ISCONN) || (err == ERR_ALREADY) || (err == ERR_INPROGRESS);
}

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

  if (is_ntp_synced())
  {
    RefreshQueuedStatusTimestamps();
  }

  QueuedEmail email = {};
  if (!PeekOutbox(&email))
  {
    m_flushState = FlushState::Idle;
    return 0;
  }

  if (QueuedStatusNeedsTimestampRefresh(email) && !is_ntp_synced())
  {
    if (m_flushState != FlushState::WaitingForNtp)
    {
      m_flushState = FlushState::WaitingForNtp;
      m_flushStateStartMs = GetNowMs();
      printf("[SMTP] Deferring queued status email until NTP sync completes. Pending emails: %u\n",
             GetOutboxCount());
    }
    return -1;
  }

  if (m_flushState == FlushState::WaitingForNtp)
  {
    m_flushState = FlushState::Idle;
  }

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
    LogPendingDnsResult(m_smtpServer);

    if (g_dnsQueryState.load() == DnsQueryState::Waiting)
    {
      VerifyDnsAndSend(m_smtpServer);
    }

    if (g_dnsQueryState.load() == DnsQueryState::Waiting)
    {
      return -1;
    }

    if ((g_dnsQueryState.load() == DnsQueryState::Failed) ||
        (g_dnsQueryState.load() == DnsQueryState::TimedOut))
    {
      printf("[SMTP] DNS preflight failed for %s.\n", m_smtpServer);
      PrintDnsServerDiagnostics("SMTP DNS preflight failure");
      m_flushState = FlushState::Idle;
      m_retryDelayMs = kDnsRetryDelayMs;
      m_busyRetryCount = 0;
      return -1;
    }

    m_flushState = FlushState::Idle;
  }

  if (m_flushState == FlushState::WaitingForCallback)
  {
    const uint8_t emailSent = m_emailSent.load();
    if (emailSent == 0)
    {
      if ((GetNowMs() - m_flushStateStartMs) >= m_sendTimeoutMs)
      {
        printf("Email send timed out after %u ms.\n", m_sendTimeoutMs);
        m_lastSmtpResult.store(SMTP_RESULT_ERR_TIMEOUT);
        m_lastSmtpErr.store((int16_t)ERR_TIMEOUT);
        m_emailSent.store(2);
      } else
      {
        return -1;
      }
    }

    const uint8_t lastResult = m_lastSmtpResult.load();
    const uint16_t lastSrvErr = m_lastSmtpSrvErr.load();
    const err_t lastErr = (err_t)m_lastSmtpErr.load();
    m_flushState = FlushState::Idle;

    if (emailSent == 1)
    {
      printf("Email sent successfully.\n");
      PopOutbox();
      return (GetOutboxCount() == 0) ? 0 : -1;
    }

    printf("Email send failed. result=%u srv_err=%u err=%d (%s)\n",
           (unsigned)lastResult,
           (unsigned)lastSrvErr,
           (int)lastErr,
           LwipErrToString(lastErr));

    if ((lastResult == SMTP_RESULT_ERR_SVR_RESP) && (lastSrvErr == 535))
    {
      printf("[SMTP] Authentication rejected by SMTP server (535). Keeping email queued for manual correction.\n");
      m_retryDelayMs = kAuthRetryDelayMs;
      m_flushState = FlushState::WaitingForBusyRetry;
      m_flushStateStartMs = GetNowMs();
      return -1;
    }

    if ((lastResult == SMTP_RESULT_ERR_HOSTNAME) || (lastErr == ERR_ARG))
    {
      printf("[SMTP] Hostname/DNS resolution failed during send. Scheduling DNS retry.\n");
      m_retryDelayMs = kDnsRetryDelayMs;
      m_flushState = FlushState::WaitingForBusyRetry;
      m_flushStateStartMs = GetNowMs();
      return -1;
    }

    if ((lastResult == SMTP_RESULT_ERR_CLOSED) ||
        (lastErr == ERR_CLSD) ||
        (lastErr == ERR_CONN) ||
        (lastErr == ERR_TIMEOUT))
    {
      if (!g_smtpClosedDiagnosticsPrinted.exchange(true))
      {
        printf("[SMTP] Connection closed or timed out by server. Capturing payload diagnostics once.\n");
        PrintSmtpFieldDiagnostics("from", email.from);
        PrintSmtpFieldDiagnostics("to", email.to);
        PrintSmtpFieldDiagnostics("subject", email.subject);
        PrintSmtpFieldDiagnostics("body", email.body);
      }

      m_retryDelayMs = kClosedRetryDelayMs;
      m_flushState = FlushState::WaitingForBusyRetry;
      m_flushStateStartMs = GetNowMs();
      return -1;
    }

    if (IsBusySmtpError(lastErr))
    {
      if (m_busyRetryCount < kMaxBusyRetries)
      {
        m_busyRetryCount++;
        m_retryDelayMs = kBusyRetryDelayMs;
        m_flushState = FlushState::WaitingForBusyRetry;
        m_flushStateStartMs = GetNowMs();
        return -1;
      }

      printf("[SMTP] Busy retry budget exhausted. Keeping email queued for next loop.\n");
      m_busyRetryCount = 0;
      return -1;
    }

    IncrementHeadRetryCount();
    return -1;
  }

  VerifyDnsAndSend(m_smtpServer);
  if (g_dnsQueryState.load() == DnsQueryState::Waiting)
  {
    m_flushState = FlushState::WaitingForDns;
    return -1;
  }

  if ((g_dnsQueryState.load() == DnsQueryState::Failed) ||
      (g_dnsQueryState.load() == DnsQueryState::TimedOut))
  {
    printf("[SMTP] DNS preflight failed for %s.\n", m_smtpServer);
    PrintDnsServerDiagnostics("SMTP DNS preflight failure");
    m_retryDelayMs = kDnsRetryDelayMs;
    m_busyRetryCount = 0;
    return -1;
  }

  printf("Sending queued email to %s (retry %u, queue depth %u)...\n", email.to, email.retryCount, GetOutboxCount());
  m_emailSent.store(0);
  g_smtpClosedDiagnosticsPrinted.store(false);
  err_t sendResult = smtp_send_mail(email.from, email.to, email.subject, email.body, PicoMail::mailsent_callback, NULL);
  if (sendResult == ERR_OK)
  {
    m_flushState = FlushState::WaitingForCallback;
    m_flushStateStartMs = GetNowMs();
    return -1;
  }

  if (IsBusySmtpError(sendResult))
  {
    if (m_busyRetryCount < kMaxBusyRetries)
    {
      m_busyRetryCount++;
      printf("smtp_send_mail busy (%d: %s). Retrying in %u ms (%u/%u).\n",
             sendResult,
             LwipErrToString(sendResult),
             kBusyRetryDelayMs,
             m_busyRetryCount,
             kMaxBusyRetries);
      m_retryDelayMs = kBusyRetryDelayMs;
      m_flushState = FlushState::WaitingForBusyRetry;
      m_flushStateStartMs = GetNowMs();
      return -1;
    }
  }

  printf("smtp_send_mail failed immediately: %d (%s)\n", sendResult, LwipErrToString(sendResult));
#ifdef SMTP_CHECK_DATA
  printf("[SMTP] Build option SMTP_CHECK_DATA=%d\n", SMTP_CHECK_DATA);
#else
  printf("[SMTP] Build option SMTP_CHECK_DATA is undefined\n");
#endif
  PrintSmtpFieldDiagnostics("from", email.from);
  PrintSmtpFieldDiagnostics("to", email.to);
  PrintSmtpFieldDiagnostics("subject", email.subject);
  PrintSmtpFieldDiagnostics("body", email.body);

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

void PicoMail::dns_callback(const char *name, const ip_addr_t *ipaddr, void *arg)
{
  (void)name;
  (void)arg;
  if (ipaddr)
  {
    if (IP_IS_V4(ipaddr))
    {
      g_dnsResolvedIpv4Raw.store(ip_2_ip4(ipaddr)->addr);
      g_dnsResolvedIpv4Valid.store(true);
    } else
    {
      g_dnsResolvedIpv4Raw.store(0);
      g_dnsResolvedIpv4Valid.store(false);
    }
    g_dnsQueryState.store(DnsQueryState::Completed);
  } else
  {
    g_dnsResolvedIpv4Raw.store(0);
    g_dnsResolvedIpv4Valid.store(false);
    g_dnsQueryState.store(DnsQueryState::Failed);
  }
  g_dnsResultPending.store(true);
  PicoMail::m_isDnsResolved.store(true);
}

void PicoMail::VerifyDnsAndSend(const char *hostname)
{
  if (hostname == NULL)
  {
    return;
  }

  if ((g_dnsQueryState.load() == DnsQueryState::Idle) ||
      (g_dnsQueryState.load() == DnsQueryState::Completed) ||
      (g_dnsQueryState.load() == DnsQueryState::Failed) ||
      (g_dnsQueryState.load() == DnsQueryState::TimedOut))
  {
    ip_addr_t resolvedIp;
    PicoMail::m_isDnsResolved.store(false);
    g_dnsResultPending.store(false);
    g_dnsResolvedIpv4Raw.store(0);
    g_dnsResolvedIpv4Valid.store(false);
    g_dnsQueryState.store(DnsQueryState::Waiting);
    g_dnsQueryStartMs = to_ms_since_boot(get_absolute_time());

    cyw43_arch_lwip_begin();
    err_t err = dns_gethostbyname(hostname, &resolvedIp, PicoMail::dns_callback, NULL);
    cyw43_arch_lwip_end();

    if (err == ERR_OK)
    {
      MAIL_LOG_DEBUG("[DNS] Cached: %s is %s\n", hostname, ipaddr_ntoa(&resolvedIp));
      PicoMail::m_isDnsResolved.store(true);
      g_dnsQueryState.store(DnsQueryState::Completed);
      return;
    }

    if (err == ERR_INPROGRESS)
    {
      MAIL_LOG_DEBUG("[DNS] Query in progress for %s...\n", hostname);
      return;
    }

    printf("Error starting DNS query: %d\n", err);
    PicoMail::m_isDnsResolved.store(true);
    g_dnsQueryState.store(DnsQueryState::Failed);
    return;
  }

  if (g_dnsQueryState.load() == DnsQueryState::Waiting)
  {
    uint32_t elapsedMs = to_ms_since_boot(get_absolute_time()) - g_dnsQueryStartMs;
    if (elapsedMs >= g_dnsQueryTimeoutMs)
    {
      printf("DNS wait timed out for %s after %u ms\n", hostname, g_dnsQueryTimeoutMs);
      PicoMail::m_isDnsResolved.store(true);
      g_dnsQueryState.store(DnsQueryState::TimedOut);
    }
  }
}

extern "C" int __attribute__((weak)) mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
  for (size_t i = 0; i < len; i++)
  {
    output[i] = (unsigned char)(rosc_hw->randombit);
  }
  *olen = len;
  return 0;
}

extern "C" mbedtls_ms_time_t mbedtls_ms_time(void)
{
  return (mbedtls_ms_time_t)(time_us_64() / 1000);
}

void PicoMail::mailsent_callback(void *arg, u8_t smtp_result, u16_t srv_err, err_t err)
{
  (void)arg;
  PicoMail::m_lastSmtpResult.store(smtp_result);
  PicoMail::m_lastSmtpSrvErr.store(srv_err);
  PicoMail::m_lastSmtpErr.store((int16_t)err);

  if (smtp_result == SMTP_RESULT_OK)
  {
    PicoMail::m_emailSent.store(1);
  } else
  {
    PicoMail::m_emailSent.store(2);
  }
}

int PicoMail::SendEmail(const char *from, const char *to, const char *subject, const char *body)
{
  if (!EnQueueEmail(from, to, subject, body))
  {
    printf("Failed to enqueue email.\n");
    return -2;
  }
  return 0;
}