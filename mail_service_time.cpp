// SPDX-License-Identifier: MIT

#include "mail_service_internal.h"

#include <malloc.h>
#include <cstdio>
#include <cstdint>
#include <ctime>

#include "pico/cyw43_arch.h"
#include "lwip/apps/sntp.h"

#if PICO_RP2040
#include "hardware/rtc.h"
#else
#include "pico/aon_timer.h"
#endif

#include "pico/util/datetime.h"

extern char __StackLimit;
extern char __bss_end__;

namespace
{
constexpr int kUtcOffsetSeconds = (-4 * 3600);

enum class NtpSyncState
{
  Idle,
  Waiting,
  Synced,
  TimedOut,
  Failed
};

std::atomic<bool> g_ntpSynced(false);
std::atomic<bool> g_sntpRunning(false);
std::atomic<NtpSyncState> g_ntpSyncState(NtpSyncState::Idle);
uint32_t g_ntpSyncStartMs = 0;
uint32_t g_ntpSyncTimeoutMs = 0;
uint32_t g_ntpRetryDelayMs = 15000;

bool set_platform_utc_time(const struct tm *utcTime)
{
  if (utcTime == NULL)
  {
    return false;
  }

#if PICO_RP2040
  datetime_t timeValue = {
      .year = (int16_t)(utcTime->tm_year + 1900),
      .month = (int8_t)(utcTime->tm_mon + 1),
      .day = (int8_t)utcTime->tm_mday,
      .dotw = (int8_t)utcTime->tm_wday,
      .hour = (int8_t)utcTime->tm_hour,
      .min = (int8_t)utcTime->tm_min,
      .sec = (int8_t)utcTime->tm_sec};
  return rtc_set_datetime(&timeValue);
#else
  if (!aon_timer_is_running())
  {
    return aon_timer_start_calendar(utcTime);
  }
  return aon_timer_set_time_calendar(utcTime);
#endif
}

bool get_platform_utc_time(struct tm *utcTime)
{
  if (utcTime == NULL)
  {
    return false;
  }

#if PICO_RP2040
  datetime_t currentTime;
  if (!rtc_get_datetime(&currentTime))
  {
    return false;
  }

  utcTime->tm_sec = currentTime.sec;
  utcTime->tm_min = currentTime.min;
  utcTime->tm_hour = currentTime.hour;
  utcTime->tm_mday = currentTime.day;
  utcTime->tm_mon = currentTime.month - 1;
  utcTime->tm_year = currentTime.year - 1900;
  utcTime->tm_wday = currentTime.dotw;
  utcTime->tm_yday = 0;
  utcTime->tm_isdst = 0;
  return true;
#else
  return aon_timer_get_time_calendar(utcTime);
#endif
}

bool convert_epoch_to_utc_tm(time_t epoch, struct tm *outTm)
{
  if (outTm == NULL)
  {
    return false;
  }

#if defined(_WIN32)
  return gmtime_s(outTm, &epoch) == 0;
#elif defined(__NEWLIB__) || defined(__GLIBC__) || defined(__APPLE__) || defined(__unix__)
  return gmtime_r(&epoch, outTm) != NULL;
#else
  struct tm *tmp = gmtime(&epoch);
  if (tmp == NULL)
  {
    return false;
  }
  *outTm = *tmp;
  return true;
#endif
}

int64_t days_from_civil(int64_t year, unsigned month, unsigned day)
{
  year -= (month <= 2) ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = (unsigned)(year - era * 400);
  const unsigned doy = (153u * (month + ((month > 2) ? -3 : 9)) + 2u) / 5u + day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

bool convert_utc_tm_to_epoch(const struct tm *utcTm, time_t *outEpoch)
{
  if ((utcTm == NULL) || (outEpoch == NULL))
  {
    return false;
  }

  const int64_t year = (int64_t)utcTm->tm_year + 1900;
  const unsigned month = (unsigned)utcTm->tm_mon + 1u;
  const unsigned day = (unsigned)utcTm->tm_mday;
  if ((month < 1u) || (month > 12u) || (day < 1u) || (day > 31u))
  {
    return false;
  }

  const int64_t days = days_from_civil(year, month, day);
  const int64_t seconds = days * 86400ll +
                          (int64_t)utcTm->tm_hour * 3600ll +
                          (int64_t)utcTm->tm_min * 60ll +
                          (int64_t)utcTm->tm_sec;
  *outEpoch = (time_t)seconds;
  return true;
}

const char *ntp_state_message(NtpSyncState state)
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

uint32_t get_total_heap()
{
  return &__StackLimit - &__bss_end__;
}

struct AllocatorStats
{
  uint32_t allocatedBytes;
  uint32_t totalFreeBytes;
  uint32_t freeChunkCount;
  bool available;
};

AllocatorStats read_allocator_stats()
{
#if defined(__GLIBC__) && defined(__GLIBC_MINOR__) && \
  ((__GLIBC__ > 2) || ((__GLIBC__ == 2) && (__GLIBC_MINOR__ >= 33)))
  struct mallinfo2 info = mallinfo2();
  return {(uint32_t)info.uordblks,
          (uint32_t)info.fordblks,
          (uint32_t)info.ordblks,
          true};
#elif defined(__NEWLIB__) || defined(__GLIBC__) || defined(__APPLE__) || defined(_WIN32)
  struct mallinfo info = mallinfo();
  return {(uint32_t)info.uordblks,
          (uint32_t)info.fordblks,
          (uint32_t)info.ordblks,
          true};
#else
  return {0, 0, 0, false};
#endif
}
}

extern "C" void sntp_sync_callback(uint32_t sec, uint32_t us)
{
  time_t epoch = (time_t)sec;
  struct tm utcTime = {0};

  (void)us;

  if (!convert_epoch_to_utc_tm(epoch, &utcTime))
  {
    return;
  }

  if (!set_platform_utc_time(&utcTime))
  {
    printf("Failed to set platform clock from SNTP\n");
    return;
  }

  g_ntpSynced.store(true);
  printf("RTC Synchronized to UTC!\n");
}

void init_platform_clock()
{
#if PICO_RP2040
  rtc_init();
#else
  aon_timer_start_with_timeofday();
#endif
}

void stop_ntp_client()
{
  if (!g_sntpRunning.load())
  {
    return;
  }

  cyw43_arch_lwip_begin();
  sntp_stop();
  cyw43_arch_lwip_end();
  g_sntpRunning.store(false);
}

void reset_ntp_sync_state()
{
  stop_ntp_client();
  g_ntpSynced.store(false);
  g_ntpSyncState.store(NtpSyncState::Idle);
}

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
  g_sntpRunning.store(sntp_enabled() != 0);
  cyw43_arch_lwip_end();

  if (!g_sntpRunning.load())
  {
    printf("[NTP] SNTP failed to start (no UDP PCB available).\n");
    g_ntpSyncState.store(NtpSyncState::Failed);
    return false;
  }

  g_ntpSyncState.store(NtpSyncState::Waiting);
  return true;
}

bool poll_ntp_sync()
{
  const NtpSyncState state = g_ntpSyncState.load();

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

  const uint32_t elapsedMs = to_ms_since_boot(get_absolute_time()) - g_ntpSyncStartMs;
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

int get_local_time(char *time, size_t timeSize, char *date, size_t dateSize)
{
  if ((time == NULL) || (timeSize == 0) || (date == NULL) || (dateSize == 0))
  {
    return -1;
  }

  if (!g_ntpSynced.load())
  {
    return -1;
  }

  struct tm utcTm = {0};
  if (!get_platform_utc_time(&utcTm))
  {
    return -1;
  }
  if (utcTm.tm_year < (2020 - 1900))
  {
    return -1;
  }

  time_t utcEpoch = 0;
  if (!convert_utc_tm_to_epoch(&utcTm, &utcEpoch))
  {
    return -1;
  }

  const time_t localEpoch = utcEpoch + kUtcOffsetSeconds;
  struct tm localTm = {0};
  if (!convert_epoch_to_utc_tm(localEpoch, &localTm))
  {
    return -1;
  }

  snprintf(time, timeSize, "%02d:%02d:%02d",
           localTm.tm_hour,
           localTm.tm_min,
           localTm.tm_sec);
  snprintf(date, dateSize, "%02d/%02d/%04d",
           localTm.tm_mon + 1,
           localTm.tm_mday,
           localTm.tm_year + 1900);
  return 0;
}

bool get_local_time_for_ms(uint32_t timestampMs,
                           char *time,
                           size_t timeSize,
                           char *date,
                           size_t dateSize)
{
  if ((time == NULL) || (timeSize == 0) || (date == NULL) || (dateSize == 0))
  {
    return false;
  }

  if (!g_ntpSynced.load())
  {
    return false;
  }

  struct tm currentUtc = {0};
  if (!get_platform_utc_time(&currentUtc))
  {
    return false;
  }
  if (currentUtc.tm_year < (2020 - 1900))
  {
    return false;
  }

  time_t currentUtcEpoch = 0;
  if (!convert_utc_tm_to_epoch(&currentUtc, &currentUtcEpoch))
  {
    return false;
  }

  const uint32_t nowMs = to_ms_since_boot(get_absolute_time());
  const uint32_t elapsedMs = nowMs - timestampMs;
  const time_t eventUtcEpoch = currentUtcEpoch - (time_t)(elapsedMs / 1000u);
  const time_t localEpoch = eventUtcEpoch + kUtcOffsetSeconds;
  struct tm localTm = {0};
  if (!convert_epoch_to_utc_tm(localEpoch, &localTm))
  {
    return false;
  }

  snprintf(time, timeSize, "%02d:%02d:%02d",
           localTm.tm_hour,
           localTm.tm_min,
           localTm.tm_sec);
  snprintf(date, dateSize, "%02d/%02d/%04d",
           localTm.tm_mon + 1,
           localTm.tm_mday,
           localTm.tm_year + 1900);
  return true;
}

void print_memory_usage()
{
  const AllocatorStats stats = read_allocator_stats();
  const uint32_t totalHeap = get_total_heap();
  if (stats.available)
  {
    const uint32_t freeHeapApprox = totalHeap - stats.allocatedBytes;
    printf("Total allocated: %lu bytes\n", (unsigned long)stats.allocatedBytes);
    printf("Total free (approx): %lu bytes\n", (unsigned long)freeHeapApprox);
    printf("Total free (allocator): %lu bytes\n", (unsigned long)stats.totalFreeBytes);
    printf("Total heap size: %lu bytes\n", (unsigned long)totalHeap);
  } else
  {
    printf("Total allocated: unavailable\n");
    printf("Total free (approx): unavailable\n");
    printf("Total free (allocator): unavailable\n");
    printf("Total heap size: %lu bytes\n", (unsigned long)totalHeap);
  }

  char time[32] = "nosync rtc";
  char date[32] = "nosync rtc";
  if (get_local_time(time, sizeof(time), date, sizeof(date)) == -1)
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
    printf("Local Time: %s Date : %s\n", time, date);
  }
}