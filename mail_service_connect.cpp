// SPDX-License-Identifier: MIT

#include "mail_service_internal.h"

#include <cstdio>
#include <cstring>

#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

namespace
{
std::atomic<bool> g_cyw43ArchInitialized(false);

bool dhcp_details_ready()
{
  cyw43_arch_lwip_begin();
  const ip4_addr_t *gateway = netif_ip4_gw(netif_default);
  const ip_addr_t *dns1 = dns_getserver(0);
  const ip_addr_t *dns2 = dns_getserver(1);
  const bool hasGateway = !ip4_addr_isany(gateway);

  char dns1Text[IPADDR_STRLEN_MAX] = {0};
  char dns2Text[IPADDR_STRLEN_MAX] = {0};
  ipaddr_ntoa_r(dns1, dns1Text, sizeof(dns1Text));
  ipaddr_ntoa_r(dns2, dns2Text, sizeof(dns2Text));
  const bool hasDns = (strcmp(dns1Text, "0.0.0.0") != 0) ||
                      (strcmp(dns2Text, "0.0.0.0") != 0);
  cyw43_arch_lwip_end();

  return hasGateway && hasDns;
}

void restore_sta_netif_as_default()
{
  cyw43_arch_lwip_begin();
  struct netif *sta_netif = &cyw43_state.netif[CYW43_ITF_STA];
  if (netif_default != sta_netif)
  {
    MAIL_LOG_DEBUG("[WiFi] Restoring STA netif as default (current IP: %s)\n",
                   ip4addr_ntoa(netif_ip4_addr(netif_default)));
    netif_set_default(sta_netif);
  }
  cyw43_arch_lwip_end();
}
}

bool is_wifi_stack_initialized()
{
  return g_cyw43ArchInitialized.load();
}

bool PicoMail::BeginConnectionAttempt(bool forceDns)
{
  if (m_wifiSsid[0] == '\0')
  {
    printf("[WiFi] Connect aborted: missing runtime Wi-Fi credentials.\n");
    return false;
  }

  if (!g_cyw43ArchInitialized.load() && (InitLwip() != 0))
  {
    return false;
  }

  cyw43_arch_enable_sta_mode();
  cyw43_arch_poll();

  const int connectResult = cyw43_arch_wifi_connect_async(m_wifiSsid,
                                                          m_wifiPassword,
                                                          CYW43_AUTH_WPA2_AES_PSK);
  if (connectResult != 0)
  {
    FailConnectionAttempt("Failed to start async Wi-Fi join",
                          connectResult,
                          kConnectRetryBaseDelayMs);
    return false;
  }

  m_forceDnsOnConnect = forceDns;
  m_staNetifRestored = false;
  m_connectionState = ConnectionState::Joining;
  m_connectionStateStartMs = GetNowMs();
  MAIL_LOG_DEBUG("[WiFi] Starting async join for SSID '%s'.\n", m_wifiSsid);
  return true;
}

void PicoMail::FailConnectionAttempt(const char *reason, int errorCode, uint32_t retryDelayMs)
{
  const uint32_t nowMs = GetNowMs();
  m_isConnected.store(false);
  m_connectionState = ConnectionState::Backoff;
  m_connectionStateStartMs = nowMs;
  m_nextConnectAttemptMs = nowMs + retryDelayMs;
  m_staNetifRestored = false;

  if (g_cyw43ArchInitialized.load())
  {
    cyw43_arch_disable_sta_mode();
    cyw43_arch_poll();
  }

  printf("[WiFi] %s: %s (%d). Retrying in %lu ms.\n",
         reason,
         WifiConnectErrorToString(errorCode),
         errorCode,
         (unsigned long)retryDelayMs);
}

bool PicoMail::FinalizeConnectionSetup()
{
  restore_sta_netif_as_default();

  if (m_forceDnsOnConnect)
  {
    ip_addr_t dnsServer1;
    ip_addr_t dnsServer2;
    IP4_ADDR(&dnsServer1, 8, 8, 8, 8);
    IP4_ADDR(&dnsServer2, 1, 1, 1, 1);

    cyw43_arch_lwip_begin();
    dns_setserver(0, &dnsServer1);
    dns_setserver(1, &dnsServer2);
    dns_init();
    cyw43_arch_lwip_end();
  } else
  {
    EnsureDnsServersConfigured("post-connect DHCP validation");
  }

  if (m_tlsConfig != NULL)
  {
    altcp_tls_free_config(m_tlsConfig);
    m_tlsConfig = NULL;
  }

  cyw43_arch_lwip_begin();
  smtp_set_tls_config(NULL);
  cyw43_arch_lwip_end();

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
    printf("[SMTP] Port 587 (STARTTLS) is not supported by the lwIP SMTP client.\n");
  }

  cyw43_arch_lwip_begin();
  smtp_set_server_addr(m_smtpServer);
  smtp_set_server_port(m_smtpPort);
  smtp_set_auth(m_senderEmail, m_senderPassword);
  cyw43_arch_lwip_end();

  m_isConnected.store(true);
  m_connectionState = ConnectionState::Idle;
  m_connectionStateStartMs = GetNowMs();
  m_nextConnectAttemptMs = 0;
  m_staNetifRestored = false;

  printf("Connected to Wi-Fi\n");
  if (GetOutboxCount() > 0)
  {
    MAIL_LOG_DEBUG("[SMTP] %u queued email(s) pending after connect.\n",
                   (unsigned)GetOutboxCount());
  }
  return true;
}

bool PicoMail::PollConnection(bool forceDns)
{
  const uint32_t nowMs = GetNowMs();

  if (m_isConnected.load())
  {
    const int linkStatus = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    cyw43_arch_poll();
    if (linkStatus == CYW43_LINK_UP)
    {
      return true;
    }

    printf("[WiFi] Link lost (status=%d). Scheduling reconnect.\n", linkStatus);
    Disconnect(false);
    m_connectionState = ConnectionState::Backoff;
    m_connectionStateStartMs = nowMs;
    m_nextConnectAttemptMs = nowMs + kConnectRetryBaseDelayMs;
    return false;
  }

  switch (m_connectionState)
  {
    case ConnectionState::Idle:
      m_nextConnectAttemptMs = nowMs;
      [[fallthrough]];

    case ConnectionState::Backoff:
      if (nowMs < m_nextConnectAttemptMs)
      {
        return false;
      }
      return BeginConnectionAttempt(forceDns);

    case ConnectionState::Joining:
    {
      const int linkStatus = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
      cyw43_arch_poll();
      if ((linkStatus == CYW43_LINK_UP) || (linkStatus == CYW43_LINK_NOIP))
      {
        m_connectionState = ConnectionState::WaitingForIp;
        m_connectionStateStartMs = nowMs;
        return false;
      }
      if (linkStatus == CYW43_LINK_BADAUTH)
      {
        FailConnectionAttempt("Wi-Fi authentication failed", -2, kAuthRetryDelayMs);
        return false;
      }
      if (linkStatus == CYW43_LINK_FAIL)
      {
        FailConnectionAttempt("Wi-Fi join failed", -3, kConnectRetryBaseDelayMs);
        return false;
      }
      if ((nowMs - m_connectionStateStartMs) >= kConnectJoinTimeoutMs)
      {
        FailConnectionAttempt("Wi-Fi join timed out", -1, kConnectRetryBaseDelayMs);
      }
      return false;
    }

    case ConnectionState::WaitingForIp:
    {
      if (!m_staNetifRestored)
      {
        restore_sta_netif_as_default();
        m_staNetifRestored = true;
      }

      const int linkStatus = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
      cyw43_arch_poll();
      if (linkStatus == CYW43_LINK_BADAUTH)
      {
        FailConnectionAttempt("Wi-Fi authentication failed", -2, kAuthRetryDelayMs);
        return false;
      }
      if ((linkStatus == CYW43_LINK_FAIL) || (linkStatus == CYW43_LINK_DOWN))
      {
        FailConnectionAttempt("Wi-Fi link dropped before IP assignment", -3, kConnectRetryBaseDelayMs);
        return false;
      }

      cyw43_arch_lwip_begin();
      const ip4_addr_t *ip = netif_ip4_addr(netif_default);
      const bool hasIp = !ip4_addr_isany(ip);
      cyw43_arch_lwip_end();
      if (hasIp)
      {
        m_connectionState = ConnectionState::WaitingForDhcp;
        m_connectionStateStartMs = nowMs;
        return false;
      }

      if ((nowMs - m_connectionStateStartMs) >= kConnectIpTimeoutMs)
      {
        FailConnectionAttempt("Timed out waiting for DHCP IP assignment", -1, kConnectRetryBaseDelayMs);
      }
      return false;
    }

    case ConnectionState::WaitingForDhcp:
      if (dhcp_details_ready())
      {
        return FinalizeConnectionSetup();
      }
      if ((nowMs - m_connectionStateStartMs) >= kConnectDhcpTimeoutMs)
      {
        PrintDnsServerDiagnostics("DHCP details wait timeout");
        return FinalizeConnectionSetup();
      }
      return false;
  }

  return false;
}

int PicoMail::InitLwip()
{
  if (g_cyw43ArchInitialized.load())
  {
    return 0;
  }

  MAIL_LOG_DEBUG("Initializing Wi-Fi...\n");
  if (cyw43_arch_init_with_country(CYW43_COUNTRY_USA))
  {
    printf("***Wi-Fi init failed***\n");
    return -1;
  }

  g_cyw43ArchInitialized.store(true);
  MailDelayMs(500);
  cyw43_wifi_pm(&cyw43_state, CYW43_DEFAULT_PM & ~0xf);
  cyw43_arch_poll();
  cyw43_arch_enable_sta_mode();
  cyw43_arch_poll();
  MailDelayMs(1500);
  cyw43_arch_poll();
  return 0;
}

int PicoMail::DeInitLwip()
{
  MAIL_LOG_DEBUG("DeInitLwIP...\n");
  if (!g_cyw43ArchInitialized.load())
  {
    return -1;
  }

  cyw43_arch_deinit();
  g_cyw43ArchInitialized.store(false);
  return 0;
}

const char *PicoMail::WifiConnectErrorToString(int err)
{
  switch (err)
  {
    case 0:
      return "Success";
    case -1:
      return "Timeout";
    case -2:
      return "Bad authentication (wrong password)";
    case -3:
      return "Connection failed";
    default:
      return "Unknown error";
  }
}

int PicoMail::WifiConnect(const char *ssid, const char *password, bool forceDns)
{
  SetRuntimeWifiCredentials(ssid, password);

  const uint32_t deadlineMs = GetNowMs() +
                              kConnectJoinTimeoutMs +
                              kConnectIpTimeoutMs +
                              kConnectDhcpTimeoutMs +
                              kAuthRetryDelayMs;

  m_connectionState = ConnectionState::Idle;
  m_nextConnectAttemptMs = 0;
  while ((int32_t)(deadlineMs - GetNowMs()) > 0)
  {
    if (PollConnection(forceDns))
    {
      return 0;
    }
    MailDelayMs(kConnectPollIntervalMs);
  }

  printf("[WiFi] Blocking connect budget expired before connection completed.\n");
  return -1;
}

int PicoMail::Connect()
{
  const char *ssid = (m_wifiSsid[0] != '\0') ? m_wifiSsid : WIFI_SSID;
  const char *password = (m_wifiSsid[0] != '\0') ? m_wifiPassword : WIFI_PASSWORD;
  return ConnectWithCredentials(ssid, password, false);
}

int PicoMail::ConnectWithCredentials(const char *ssid, const char *password, bool forceDns)
{
  if ((ssid == NULL) || (password == NULL) || (ssid[0] == '\0'))
  {
    printf("Connect aborted: missing runtime Wi-Fi credentials.\n");
    return -1;
  }

  SafeCopy(m_wifiSsid, sizeof(m_wifiSsid), ssid);
  SafeCopy(m_wifiPassword, sizeof(m_wifiPassword), password);

  if (m_isConnected.load())
  {
    return 0;
  }

  return WifiConnect(ssid, password, forceDns);
}

int PicoMail::Disconnect(bool hardDeinit)
{
  reset_ntp_sync_state();
  if (m_tlsConfig != NULL)
  {
    altcp_tls_free_config(m_tlsConfig);
    m_tlsConfig = NULL;
  }

  if (hardDeinit)
  {
    if (g_cyw43ArchInitialized.load())
    {
      DeInitLwip();
    }
  } else
  if (g_cyw43ArchInitialized.load())
  {
    cyw43_arch_disable_sta_mode();
    cyw43_arch_poll();
  }

  m_isConnected.store(false);
  m_connectionState = ConnectionState::Idle;
  m_connectionStateStartMs = GetNowMs();
  m_nextConnectAttemptMs = 0;
  m_staNetifRestored = false;
  return 0;
}

void PicoMail::CheckGatewayIpDns()
{
  if (!m_isConnected.load())
  {
    printf("Cannot check gateway and DNS: Not connected to Wi-Fi.\n");
    return;
  }

  cyw43_arch_lwip_begin();
  printf("IP Address   : %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
  printf("Net Mask     : %s\n", ip4addr_ntoa(netif_ip4_netmask(netif_default)));
  printf("Gateway      : %s\n", ip4addr_ntoa(netif_ip4_gw(netif_default)));
  const ip_addr_t *dns1 = dns_getserver(0);
  const ip_addr_t *dns2 = dns_getserver(1);
  printf("DNS Primary  : %s\n", ipaddr_ntoa(dns1));
  printf("DNS Secondary: %s\n", ipaddr_ntoa(dns2));
  cyw43_arch_lwip_end();

  int32_t rssi = 0;
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
