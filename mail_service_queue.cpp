// SPDX-License-Identifier: MIT

#include "mail_service_internal.h"

#include <cstdio>
#include <cstring>

void PicoMail::SafeCopy(char *destination, size_t destinationSize, const char *source)
{
  if ((destination == NULL) || (destinationSize == 0))
  {
    return;
  }
  snprintf(destination, destinationSize, "%s", (source != NULL) ? source : "");
}

bool PicoMail::EnQueueEmail(const char *from, const char *to, const char *subject, const char *body)
{
  return EnQueueEmailAt(from, to, subject, body, to_ms_since_boot(get_absolute_time()));
}

bool PicoMail::EnQueueEmailAt(const char *from,
                              const char *to,
                              const char *subject,
                              const char *body,
                              uint32_t timestampMs)
{
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
  slot.queuedAtMs = timestampMs;
  m_outboxTail = (uint8_t)((m_outboxTail + 1) % OUTBOX_CAPACITY);
  m_outboxCount++;
  const uint8_t depth = m_outboxCount;
  critical_section_exit(&m_outboxLock);
  printf("Email queued. Outbox depth: %u\n", depth);
  return true;
}

bool PicoMail::EnQueueGeneratorStatus(GeneratorEvent statusCode)
{
  return EnQueueGeneratorStatus(statusCode, to_ms_since_boot(get_absolute_time()));
}

bool PicoMail::EnQueueGeneratorStatus(GeneratorEvent statusCode, uint32_t eventTimestampMs)
{
  char time[32] = "no clock";
  char date[32] = "no date";
  char msg[512];
  if (get_local_time(time, sizeof(time), date, sizeof(date)) == -1)
  {
  } else
  {
    printf("NIST Synced Time [%s %s]\n", time, date);
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

  return EnQueueEmailAt(GetSenderEmail(),
                        GetRecipientEmail(),
                        "Generator Status Update",
                        msg,
                        eventTimestampMs);
}

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

bool PicoMail::QueuedStatusNeedsTimestampRefresh(const QueuedEmail &email)
{
  if (strcmp(email.subject, "Generator Status Update") != 0)
  {
    return false;
  }

  return (strstr(email.body, "Time: no clock") != NULL) ||
         (strstr(email.body, "Date: no date") != NULL);
}

void PicoMail::RefreshQueuedStatusTimestamps()
{
  if (!is_ntp_synced())
  {
    return;
  }

  critical_section_enter_blocking(&m_outboxLock);
  for (uint8_t i = 0; i < m_outboxCount; i++)
  {
    const uint8_t index = (uint8_t)((m_outboxHead + i) % OUTBOX_CAPACITY);
    RefreshQueuedStatusTimestamp(&m_outbox[index]);
  }
  critical_section_exit(&m_outboxLock);
}

void PicoMail::RefreshQueuedStatusTimestamp(QueuedEmail *email)
{
  if (email == NULL)
  {
    return;
  }

  if (!QueuedStatusNeedsTimestampRefresh(*email))
  {
    return;
  }

  char localTime[32] = {0};
  char localDate[32] = {0};
  if (!get_local_time_for_ms(email->queuedAtMs,
                             localTime,
                             sizeof(localTime),
                             localDate,
                             sizeof(localDate)))
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