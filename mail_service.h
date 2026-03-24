// SPDX-License-Identifier: MIT

#pragma once

#include "generator_controller.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "lwip/apps/smtp.h"
#include "lwip/ip_addr.h"
#include "pico/sync.h"

#ifndef WIFI_SSID
#define WIFI_SSID "your_wifi_ssid"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your_wifi_password"
#endif

#ifndef SMTP_SERVER
#define SMTP_SERVER "smtp.gmail.com"
#endif

#ifndef SENDER_EMAIL
#define SENDER_EMAIL "your_email@example.com"
#endif

#ifndef SENDER_PASSWORD
#define SENDER_PASSWORD "your_email_password"
#endif

#ifndef SMTP_PORT
#define SMTP_PORT 465
#endif

class PicoMail
{
private:
  struct QueuedEmail
  {
    char from[64];
    char to[64];
    char subject[80];
    char body[128];
    uint16_t retryCount;
  };

  enum class FlushState
  {
    Idle,
    WaitingForBusyRetry,
    WaitingForDns,
    WaitingForCallback
  };

  static constexpr uint8_t OUTBOX_CAPACITY = 16;
  static constexpr uint8_t kMaxBusyRetries = 3;
  static constexpr uint32_t kBusyRetryDelayMs = 200;
  static constexpr uint32_t kDnsRetryDelayMs = 3000;

public:
  static std::atomic<uint8_t> m_emailSent;
  static std::atomic<bool> m_isDnsResolved;
  static std::atomic<bool> m_isConnected;
  static std::atomic<uint8_t> m_lastSmtpResult;
  static std::atomic<uint16_t> m_lastSmtpSrvErr;
  static std::atomic<int16_t> m_lastSmtpErr;

private:
  struct altcp_tls_config *m_tlsConfig;

  static QueuedEmail m_outbox[OUTBOX_CAPACITY];
  static uint8_t m_outboxHead;
  static uint8_t m_outboxTail;
  static uint8_t m_outboxCount;
  static critical_section_t m_outboxLock;
  static bool m_outboxLockInitialized;
  static uint32_t m_sendTimeoutMs;

  FlushState m_flushState = FlushState::Idle;
  uint32_t m_flushStateStartMs = 0;
  uint32_t m_retryDelayMs = kBusyRetryDelayMs;
  uint8_t m_busyRetryCount = 0;
  char m_smtpServer[64] = {};
  char m_senderEmail[64] = {};
  char m_senderPassword[64] = {};
  char m_recipientEmail[64] = {};
  uint16_t m_smtpPort = 0;
  char m_wifiSsid[33] = {};
  char m_wifiPassword[64] = {};

public:
  PicoMail();
  ~PicoMail();

  int FlushOutbox();
  int InitLwip();
  int DeInitLwip();
  const char *WifiConnectErrorToString(int err);
  int WifiConnect(const char *ssid, const char *password, bool force_dns = false);
  void ConfigureRuntimeSmtp(const char *server,
                            uint16_t port,
                            const char *senderEmail,
                            const char *senderPassword,
                            const char *recipientEmail);
  int ConnectWithCredentials(const char *ssid, const char *password, bool force_dns = false);
  int Connect();
  int Disconnect(bool hardDeinit = true);
  int SendEmail(const char *from, const char *to, const char *subject, const char *body);
  bool EnQueueEmail(const char *from, const char *to, const char *subject, const char *body);
  bool EnQueueGeneratorStatus(GeneratorEvent status);

  void VerifyDnsAndSend(const char *hostname);
  void CheckGatewayIpDns();
  static void dns_callback(const char *name, const ip_addr_t *ipaddr, void *arg);
  static void mailsent_callback(void *arg, u8_t smtp_result, u16_t srv_err, err_t err);
  static void SafeCopy(char *destination, size_t destinationSize, const char *source);
  bool PeekOutbox(QueuedEmail *email);
  void PopOutbox();

  /**
   * @brief Increments retry counter for the current outbox head item.
   */
  void IncrementHeadRetryCount();

  /**
   * @brief Rewrites placeholder status-email timestamps when local time becomes available.
   * @param email Mutable queued email payload snapshot.
   */
  void RefreshQueuedStatusTimestamp(QueuedEmail *email);
  static bool IsBusySmtpError(err_t err);

  /**
   * @brief Returns monotonic milliseconds since boot for timeout/state checks.
   * @return Milliseconds since boot.
   */
  uint32_t GetNowMs() const;

  /**
   * @brief Returns current outbox depth.
   * @return Number of queued emails.
   */
  uint8_t GetOutboxCount();

  /**
   * @brief Returns current Wi-Fi/SMTP connectivity state.
   * @return true when connected; false otherwise.
   */
  bool IsConnected() const;
  const char *GetSenderEmail() const;
  const char *GetRecipientEmail() const;
};


/**
 * @brief Initializes platform clock backend used by RTC/NTP conversion helpers.
 */
void init_platform_clock();

/**
 * @brief Starts asynchronous SNTP synchronization.
 * @param timeoutMs Timeout budget for a sync attempt.
 * @return true when SNTP start succeeds; false otherwise.
 */
bool start_ntp_sync(uint32_t timeoutMs);

/**
 * @brief Advances non-blocking SNTP state machine and retry logic.
 * @return true when sync reached a terminal state in this call; false otherwise.
 */
bool poll_ntp_sync();

/**
 * @brief Returns whether local RTC has been synchronized from SNTP.
 * @return true when synced; false otherwise.
 */
bool is_ntp_synced();

/**
 * @brief Returns whether CYW43/lwIP stack has been initialized.
 * @return true when initialized; false otherwise.
 */
bool is_wifi_stack_initialized();

/**
 * @brief Prints compact runtime health snapshot (Wi-Fi, outbox, NTP, local time).
 * @param mail Optional mail service pointer for queue depth details.
 */
void print_runtime_status(PicoMail *mail);
