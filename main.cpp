// SPDX-License-Identifier: MIT
#include "hardware/watchdog.h"
#include "mail_service.h"
#include "generator_controller.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include <atomic>

#ifdef _PICO_W
extern "C" {
#include "access_point.h"
#include "flash_program.h"
}
#endif

extern "C" {
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
}

/**
 * @file main.cpp
 * @brief Application entry point and top-level runtime orchestration.
 *
 * This translation unit wires together the generator controller, mail service,
 * serial console, and optional FreeRTOS task-based runtime.
 */

#ifdef _PICO_W
const uint LED_PIN = 0;   // onboard pico_w and pico2_w (WiFi chip LED)
#else
const uint LED_PIN = 25;  // onboard pico and pico2 led (GPIO LED)
#endif

static constexpr uint32_t kNtpSyncTimeoutMs = 40000;
static constexpr char kDefaultRecipientEmail[] = "csvanholm@comcast.net";

// RTOS task priority constants
static constexpr UBaseType_t kGeneratorTaskPriority = 3;
static constexpr UBaseType_t kMailTaskPriority      = 2;
static constexpr UBaseType_t kConsoleTaskPriority   = 1;
static constexpr UBaseType_t kWatchdogTaskPriority  = 4;
static constexpr uint8_t kGeneratorQueueDepth       = 32;
static constexpr uint32_t kGeneratorHeartbeatTimeoutMs = 5000;
static constexpr uint32_t kGeneratorStartupGraceMs = 10000;
static constexpr uint32_t kMailTaskLoopDelayMs = 10;
static constexpr bool kVerboseRtosLogs = false;
/**
 * Hardware watchdog timeout in milliseconds.
 * Must be longer than kGeneratorHeartbeatTimeoutMs to allow watchdog_task
 * time to detect stale heartbeat and stop feeding. When heartbeat is healthy,
 * this timer is continuously reset.  When heartbeat stales, the timer expires
 * and triggers a hardware reset.
 */
static constexpr uint32_t kWatchdogHardwareTimeoutMs = 8000;
static constexpr uint32_t kWatchdogPollPeriodMs = 250;

/**
 * @brief Queue used to transfer generator events to the network-owning mail task.
 */
static QueueHandle_t g_generatorEventQueue = NULL;

/**
 * @brief Requests that the network-owning runtime path start an NTP sync attempt.
 */
static std::atomic<bool> g_ntpSyncRequested(false);

/**
 * @brief Last observed generator main-loop heartbeat timestamp.
 */
static std::atomic<uint32_t> g_generatorHeartbeatMs(0);

/**
 * @brief Indicates at least one generator heartbeat has been observed.
 */
static std::atomic<bool> g_generatorHeartbeatSeen(false);

/**
 * @brief Indicates hardware watchdog has been armed.
 */
static std::atomic<bool> g_watchdogArmed(false);

/**
 * @brief Initializes the board LED when using a GPIO-backed target.
 */
static void init_onboard_led()
{
#ifndef _PICO_W
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);
#endif
}

#ifdef _PICO_W
/**
 * @brief Normalizes flash-backed C strings to safe null-terminated values.
 * @param buffer Mutable flash field buffer.
 * @param length Buffer size in bytes.
 */
static void sanitize_flash_string(char *buffer, size_t length)
{
  if ((buffer == NULL) || (length == 0))
  {
    return;
  }

  bool hasTerminator = false;
  for (size_t i = 0; i < length; i++)
  {
    if (buffer[i] == '\0')
    {
      hasTerminator = true;
      break;
    }
  }

  if (!hasTerminator)
  {
    buffer[length - 1] = '\0';
  }

  if (static_cast<uint8_t>(buffer[0]) == 0xFFu)
  {
    buffer[0] = '\0';
  }
}

/**
 * @brief Checks whether a flash-backed string is unset or erased.
 * @param value String pointer to test.
 * @return true when value is empty or starts with erased flash pattern.
 */
static bool is_unset_flash_string(const char *value)
{
  return (value == NULL) || (value[0] == '\0') || (static_cast<uint8_t>(value[0]) == 0xFFu);
}

/**
 * @brief Loads stored Wi-Fi configuration and optionally runs AP setup flow.
 * @param wifiConfig Destination config structure used by connection logic.
 */
static void apply_runtime_mail_defaults(config *wifiConfig)
{
  if (wifiConfig == NULL)
  {
    return;
  }

  sanitize_flash_string(wifiConfig->smtp_server, sizeof(wifiConfig->smtp_server));
  sanitize_flash_string(wifiConfig->sender_email, sizeof(wifiConfig->sender_email));
  sanitize_flash_string(wifiConfig->sender_password, sizeof(wifiConfig->sender_password));
  sanitize_flash_string(wifiConfig->recipient_email, sizeof(wifiConfig->recipient_email));

  if (is_unset_flash_string(wifiConfig->smtp_server))
  {
    snprintf(wifiConfig->smtp_server, sizeof(wifiConfig->smtp_server), "%s", SMTP_SERVER);
  }
  if ((wifiConfig->smtp_port == 0) || (wifiConfig->smtp_port == 0xFFFFu))
  {
    wifiConfig->smtp_port = (uint16_t)SMTP_PORT;
  }
  if (is_unset_flash_string(wifiConfig->sender_email))
  {
    snprintf(wifiConfig->sender_email, sizeof(wifiConfig->sender_email), "%s", SENDER_EMAIL);
  }
  if (is_unset_flash_string(wifiConfig->sender_password))
  {
    snprintf(wifiConfig->sender_password, sizeof(wifiConfig->sender_password), "%s", SENDER_PASSWORD);
  }
  if (is_unset_flash_string(wifiConfig->recipient_email))
  {
    snprintf(wifiConfig->recipient_email, sizeof(wifiConfig->recipient_email), "%s", kDefaultRecipientEmail);
  }
}

/**
 * @brief Upgrades legacy flash config records to the current schema.
 * @param wifiConfig Mutable runtime configuration object.
 * @return true when a migration was performed; otherwise false.
 */
static bool migrate_legacy_config(config *wifiConfig)
{
  if (wifiConfig == NULL)
  {
    return false;
  }

  if (wifiConfig->magic != LEGACY_MAGIC)
  {
    return false;
  }

  wifiConfig->magic = MAGIC;
  wifiConfig->version = CONFIG_VERSION;
  apply_runtime_mail_defaults(wifiConfig);
  return true;
}

/**
 * @brief Loads, migrates, and validates runtime Wi-Fi/mail configuration.
 * @param wifiConfig Destination config structure persisted in flash.
 */
static void ensure_runtime_wifi_config(config *wifiConfig)
{
  if (wifiConfig == NULL)
  {
    return;
  }

  flash_read((uint8_t *)wifiConfig, sizeof(*wifiConfig), WIFI_CONFIG_PAGE);

  const bool migratedLegacy = migrate_legacy_config(wifiConfig);
  if (migratedLegacy)
  {
    printf("[SETUP] Migrated legacy flash config to schema v%u\n", (unsigned)CONFIG_VERSION);
    flash_write_page((uint8_t *)wifiConfig, sizeof(*wifiConfig), WIFI_CONFIG_PAGE);
  }

  apply_runtime_mail_defaults(wifiConfig);

  const bool configMissing = (wifiConfig->magic != MAGIC);
  const bool versionMismatch = (!configMissing && (wifiConfig->version != CONFIG_VERSION));
  const bool setupRequested = forceSetup();
  if (configMissing || versionMismatch || setupRequested)
  {
    if (versionMismatch)
    {
      printf("[SETUP] Config schema v%u does not match expected v%u, re-running setup\n",
             (unsigned)wifiConfig->version, (unsigned)CONFIG_VERSION);
    }
    // AP setup may run for minutes while waiting for user input, so ensure no
    // inherited watchdog state can reset the device during provisioning.
    printf("[SETUP] Disabling watchdog during AP provisioning mode.\n");
    watchdog_disable();
    printf("[SETUP] Entering AP config mode at 192.168.0.1\n");
    run_access_point(wifiConfig);
    apply_runtime_mail_defaults(wifiConfig);
    flash_write_page((uint8_t *)wifiConfig, sizeof(*wifiConfig), WIFI_CONFIG_PAGE);
    flash_read((uint8_t *)wifiConfig, sizeof(*wifiConfig), WIFI_CONFIG_PAGE);
  }
}
#endif

/**
 * @brief Handles single-character serial commands for runtime diagnostics/control.
 * @param ch Received ASCII command character.
 * @param mail Active mail service instance.
 */
static void handle_serial_command(int ch, PicoMail *mail)
{
  if (mail == NULL)
  {
    return;
  }

  switch (ch)
  {
    case 's':
    case 'S':
      print_runtime_status(mail);
      break;

    case 'n':
    case 'N':
      if (mail->IsConnected())
      {
        printf("[CMD] Manual NTP sync requested.\n");
        g_ntpSyncRequested.store(true);
      } else
      {
        printf("[CMD] Cannot start NTP: Wi-Fi disconnected.\n");
      }
      break;

    case 'h':
    case 'H':
    case '?':
      printf("[CMD] Commands: s=status, n=NTP sync, r=reboot, h=help\n");
      break;
   
    case 'r':
    case 'R': 
      printf("[CMD] Rebooting...\n");
       watchdog_reboot(0, 0, 0);
      break;
   
        
    default:
      break;
  }
}

/**
 * @brief Adapts generator events into the FreeRTOS queue transport.
 * @param context Queue handle used as the event sink target.
 * @param message Event payload published by the generator controller.
 * @return true when the message was queued; false when the queue was unavailable or full.
 */
static bool queue_event_sink(void *context, const GeneratorEventMessage &message)
{
  QueueHandle_t queue = static_cast<QueueHandle_t>(context);
  if (queue == NULL)
  {
    return false;
  }
  return xQueueSend(queue, &message, 0) == pdPASS;
}

/**
 * @brief Periodic RTOS task that owns the generator control loop.
 * @param param Queue handle used to export generator events.
 *
 * This task calls Generator::RunOneTick() every 500 ms and publishes a heartbeat
 * timestamp after each tick. The heartbeat is monitored by watchdog_task to detect
 * hangs or excessive blocking. If RunOneTick() blocks for more than
 * kGeneratorHeartbeatTimeoutMs, the hardware watchdog will trigger a reset.
 */
static void generator_task(void *param)
{
  QueueHandle_t eventQueue = static_cast<QueueHandle_t>(param);
  Generator generator(queue_event_sink, eventQueue);
  while (true)
  {
    generator.RunOneTick();
    g_generatorHeartbeatMs.store(to_ms_since_boot(get_absolute_time()));
    g_generatorHeartbeatSeen.store(true);
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

/**
 * @brief RTOS task that supervises generator heartbeat and feeds hardware watchdog.
 * @param param Unused.
 *
 * This task is the sole driver of the hardware watchdog. It monitors the generator
 * task heartbeat (updated by generator_task after each RunOneTick) and only feeds
 * the watchdog when the heartbeat is current (not stale). If the generator task
 * hangs or blocks for too long, the watchdog will detect stale heartbeat, stop feeding,
 * and allow the hardware timeout to expire, triggering a device reset.
 *
 * The watchdog is not armed until the generator task publishes its first real
 * heartbeat. This avoids treating task startup ordering as a liveness failure.
 * If no initial heartbeat arrives within kGeneratorStartupGraceMs, the system
 * forces a reboot rather than running indefinitely without supervision.
 *
 * Initial heartbeat grace timeout: kGeneratorStartupGraceMs (10000 ms)
 * Heartbeat age threshold: kGeneratorHeartbeatTimeoutMs (5000 ms)
 * Hardware timeout: kWatchdogHardwareTimeoutMs (8000 ms)
 * Watchdog poll period: kWatchdogPollPeriodMs (250 ms)
 */
static void watchdog_task(void *param)
{
  (void)param;

  const uint32_t startupWaitStartMs = to_ms_since_boot(get_absolute_time());
  while (!g_generatorHeartbeatSeen.load())
  {
    const uint32_t nowMs = to_ms_since_boot(get_absolute_time());
    if ((nowMs - startupWaitStartMs) > kGeneratorStartupGraceMs)
    {
      printf("[WDT] No initial generator heartbeat within %lu ms. Rebooting...\n",
             (unsigned long)kGeneratorStartupGraceMs);
      watchdog_reboot(0, 0, 0);
      vTaskSuspend(NULL);
    }

    vTaskDelay(pdMS_TO_TICKS(kWatchdogPollPeriodMs));
  }

  watchdog_enable(kWatchdogHardwareTimeoutMs, true);
  g_watchdogArmed.store(true);
  printf("[WDT] Armed after first generator heartbeat (timeout=%lu ms, generator heartbeat timeout=%lu ms).\n",
         (unsigned long)kWatchdogHardwareTimeoutMs,
         (unsigned long)kGeneratorHeartbeatTimeoutMs);

  while (true)
  {
    const uint32_t nowMs = to_ms_since_boot(get_absolute_time());
    const bool seenHeartbeat = g_generatorHeartbeatSeen.load();
    const uint32_t lastHeartbeatMs = g_generatorHeartbeatMs.load();

    bool generatorHealthy = true;
    if (seenHeartbeat)
    {
      generatorHealthy = ((nowMs - lastHeartbeatMs) <= kGeneratorHeartbeatTimeoutMs);
    }

    if (generatorHealthy)
    {
      watchdog_update();
    } else
    {
      // Do not feed while generator heartbeat appears stale.
      printf("[WDT] Generator heartbeat stale (%lu ms). Waiting for watchdog reset...\n",
             (unsigned long)(nowMs - lastHeartbeatMs));
    }

    vTaskDelay(pdMS_TO_TICKS(kWatchdogPollPeriodMs));
  }
}

/**
 * @brief RTOS task that owns Wi-Fi, SMTP, and NTP state transitions.
 * @param param Pointer to the shared mail service instance.
 *
 * This task is the sole network owner in the RTOS path. It drains generator
 * events from the queue, starts and polls NTP synchronization, handles reconnects,
 * and flushes the email outbox.
 */

static void mail_task(void *param)
{
  // Network ownership rule for the RTOS path:
  // This task is the only task allowed to drive Wi-Fi, SMTP, and NTP state.
  // mail_service.cpp still contains legacy single-loop network logic, so
  // splitting reconnect, NTP, and outbox flushing across multiple tasks is unsafe.
  PicoMail *mail = static_cast<PicoMail *>(param);
  uint32_t bootMs = to_ms_since_boot(get_absolute_time());
  uint32_t tickCounter = 0;
  static constexpr uint32_t kReconnectPeriodTicks = 60;
  bool wasConnected = mail->IsConnected();
  bool wasNtpSynced = is_ntp_synced();
  uint8_t lastOutboxCount = mail->GetOutboxCount();

  (void)mail->EnQueueGeneratorStatus(GeneratorEvent::ControllerOnline, bootMs);

  if (wasConnected)
  {
    g_ntpSyncRequested.store(true);
  }

  while (true)
  {
    GeneratorEventMessage firstEvent;
    const BaseType_t hasEvent = xQueueReceive(g_generatorEventQueue, &firstEvent, 0);
    if (hasEvent == pdTRUE)
    {
      (void)mail->EnQueueGeneratorStatus(firstEvent.eventCode, firstEvent.timestampMs);
    }

    const bool isConnected = mail->IsConnected();
    const bool isNtpSynced = is_ntp_synced();
    const uint8_t outboxCount = mail->GetOutboxCount();
    if (isConnected && !wasConnected)
    {
      if (kVerboseRtosLogs)
      {
        printf("[RTOS] Wi-Fi connection restored in mail task.\n");
      }
      g_ntpSyncRequested.store(true);
    } else if (!isConnected && wasConnected)
    {
      if (kVerboseRtosLogs)
      {
        printf("[RTOS] Wi-Fi connection lost in mail task.\n");
      }
    }
    wasConnected = isConnected;

    if (isNtpSynced && !wasNtpSynced)
    {
      if (kVerboseRtosLogs)
      {
        printf("[RTOS] NTP state transitioned to synced.\n");
      }
    }
    wasNtpSynced = isNtpSynced;

    if (outboxCount != lastOutboxCount)
    {
      if (kVerboseRtosLogs)
      {
        printf("[RTOS] Outbox depth changed: %u -> %u\n",
               static_cast<unsigned>(lastOutboxCount),
               static_cast<unsigned>(outboxCount));
      }
      lastOutboxCount = outboxCount;
    }

    GeneratorEventMessage eventMessage;
    while (xQueueReceive(g_generatorEventQueue, &eventMessage, 0) == pdTRUE)
    {
      (void)mail->EnQueueGeneratorStatus(eventMessage.eventCode, eventMessage.timestampMs);
    }

    if (isConnected && g_ntpSyncRequested.exchange(false))
    {
      printf("[RTOS] Starting NTP sync attempt.\n");
      (void)start_ntp_sync(kNtpSyncTimeoutMs);
    }

    (void)poll_ntp_sync();

    if (mail->GetOutboxCount() > 0)
    {
      if (!mail->IsConnected() && ((tickCounter % kReconnectPeriodTicks) == 0u))
      {
        if (kVerboseRtosLogs)
        {
          printf("Pending email exists while disconnected. Attempting reconnect...\n");
        }
        if (mail->Connect() == 0)
        {
          g_ntpSyncRequested.store(true);
        }
      }
      (void)mail->FlushOutbox();
    }

    tickCounter++;
    vTaskDelay(pdMS_TO_TICKS(kMailTaskLoopDelayMs));
  }
}

/**
 * @brief RTOS task that handles non-blocking serial command input.
 * @param param Pointer to the shared mail service instance.
 */
static void console_task(void *param)
{
  PicoMail *mail = static_cast<PicoMail *>(param);
  printf("[CMD] Commands: s=status, n=NTP sync, r=reboot, h=help\n");

  while (true)
  {
    const int ch = getchar_timeout_us(0);
    if (ch != PICO_ERROR_TIMEOUT)
    {
      handle_serial_command(ch, mail);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

/**
 * @brief Program entry point.
 * @return Does not normally return; returns a negative value on startup failure.
 *
 * The function initializes the board and mail service, then selects either the
 * FreeRTOS task-based runtime on all supported target platforms.
 */

int main()
{
  stdio_init_all();
  if (watchdog_enable_caused_reboot())
  {
    printf("[BOOT] Reset cause: hardware watchdog timeout (unrecovered liveness failure).\n");
  } else
  if (watchdog_caused_reboot())
  {
    printf("[BOOT] Reset cause: controlled watchdog reboot (firmware-initiated).\n");
  } else
  {
    printf("[BOOT] Reset cause: power-on or external reset.\n");
  }
  init_onboard_led();
  init_platform_clock();

  PicoMail *picoMail = new PicoMail();
  if (picoMail == NULL)
  {
    printf("Failed to allocate mail service.\n");
    return -1;
  }

#ifdef _PICO_W
  config wifiConfig = {};

  // AP setup uses cyw43/lwIP directly, so stack init must happen first.
  if (picoMail->InitLwip() != 0)
  {
    printf("Failed to initialize Wi-Fi stack.\n");
    return -1;
  }

  // read config from flash and run AP setup if needed. This ensures we have Wi-Fi credentials before proceeding and also migrates legacy config formats.
  ensure_runtime_wifi_config(&wifiConfig);

  picoMail->ConfigureRuntimeSmtp(wifiConfig.smtp_server,
                                 wifiConfig.smtp_port,
                                 wifiConfig.sender_email,
                                 wifiConfig.sender_password,
                                 wifiConfig.recipient_email);

  // Cache runtime credentials now; connect is deferred to mail_task after scheduler
  // start so watchdog supervision is active during blocking Wi-Fi operations.
  picoMail->SetRuntimeWifiCredentials(wifiConfig.ssid, wifiConfig.passwd);
#else
  if (picoMail->Connect() == 0)
  {
    g_ntpSyncRequested.store(true);
  }
#endif

  if (picoMail->IsConnected())
  {
    sleep_ms(2000);
    picoMail->CheckGatewayIpDns();
  }

  g_generatorEventQueue = xQueueCreate(kGeneratorQueueDepth, sizeof(GeneratorEventMessage));
  if (g_generatorEventQueue == NULL)
  {
    printf("Failed to create generator event queue.\n");
    return -1;
  }

#if (configQUEUE_REGISTRY_SIZE > 0)
  vQueueAddToRegistry(g_generatorEventQueue, "GenEventQ");
#endif

  if (xTaskCreate(generator_task, "Generator", 1024, g_generatorEventQueue, kGeneratorTaskPriority, NULL) != pdPASS)
  {
    printf("Failed to create generator task.\n");
    return -1;
  }

  if (xTaskCreate(mail_task, "Mail", 2048, picoMail, kMailTaskPriority, NULL) != pdPASS)
  {
    printf("Failed to create mail task.\n");
    return -1;
  }

  if (xTaskCreate(console_task, "Console", 1024, picoMail, kConsoleTaskPriority, NULL) != pdPASS)
  {
    printf("Failed to create console task.\n");
    return -1;
  }

  if (xTaskCreate(watchdog_task, "Watchdog", 1024, NULL, kWatchdogTaskPriority, NULL) != pdPASS)
  {
    printf("Failed to create watchdog task.\n");
    return -1;
  }

  printf("[RTOS] Tasks created, starting scheduler...\n");
  vTaskStartScheduler();
  printf("Scheduler exited unexpectedly.\n");
  return -1;
}
