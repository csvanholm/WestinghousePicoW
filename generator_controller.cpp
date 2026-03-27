// SPDX-License-Identifier: MIT

#include "generator_controller.h"

#include "pico/stdlib.h"

extern "C" 
{
#include "FreeRTOS.h"
#include "task.h"
}

#include <cstdio>

#define USING_LED_RESISTORS

namespace
{
 const uint GENERATOR_RUNNING = 14;
 const uint LINE_POWER_FAIL = 16;
 const uint START_STOP_RELAY = 18;
 const uint USE_WEEKLY_EXERCISER = 5;

 const uint LED_BLUE = 15;
 const uint LED_RED = 11;
 const uint LED_YELLOW = 9;
 const uint LED_WHITE = 7;

 static void DelayMs(uint32_t delayMs)
 {
   if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
   {
     vTaskDelay(pdMS_TO_TICKS(delayMs));
   } else
   {
     sleep_ms(delayMs);
   }
 }
}


Generator::Generator(EventSinkFn eventSinkFn, void *eventSinkContext, HeartbeatFn heartbeatFn)
    : m_eventSinkFn(eventSinkFn), m_eventSinkContext(eventSinkContext), m_heartbeatFn(heartbeatFn)
{
  InitIoPins();
}

void Generator::PublishEvent(GeneratorEvent eventCode)
{
  if (m_eventSinkFn == NULL)
  {
    return;
  }

  GeneratorEventMessage eventMessage = {
      .eventCode = eventCode,
      .timestampMs = to_ms_since_boot(get_absolute_time())};

  if (!m_eventSinkFn(m_eventSinkContext, eventMessage))
  {
    m_droppedEvents++;
    if ((m_droppedEvents & 0x0Fu) == 1u)
    {
      printf("Generator event queue full; dropped=%lu\n", (unsigned long)m_droppedEvents);
    }
  }
}

void Generator::PulseStartButton()
{
  Led(LED_WHITE, true);
  gpio_put(START_STOP_RELAY, true);
  DelayMs(1200);
  gpio_put(START_STOP_RELAY, false);
  Led(LED_WHITE, false);
}

void Generator::PulseStopButton()
{
  Led(LED_WHITE, true);
  gpio_put(START_STOP_RELAY, true);
  DelayMs(2500);
  gpio_put(START_STOP_RELAY, false);
  Led(LED_WHITE, false);
}

void Generator::Led(uint id, bool state)
{
#ifdef USING_LED_RESISTORS
  gpio_put(id, state);
#else
  if (state)
  {
    gpio_pull_up(id);
  } else
  {
    gpio_disable_pulls(id);
  }
#endif
}

void Generator::AliveBlink()
{
  if (m_heartbeatFn != nullptr)
  {
    m_heartbeatFn();
  }

  if (m_coolDownCounter > 0)
  {
    Led(LED_BLUE, !gpio_get(LED_BLUE));
  } else
  {
    Led(LED_BLUE, false);
  }

  if (m_generatorPowerStable > 0)
  {
    Led(LED_YELLOW, !gpio_get(LED_YELLOW));
  } else
  {
    Led(LED_YELLOW, m_generatorRunning);
  }

  if (m_genPowerRequestedStable > 0)
  {
    Led(LED_RED, !gpio_get(LED_RED));
  } else
  {
    Led(LED_RED, m_genPowerRequested);
  }
}

void Generator::BlinkFailure()
{
  bool state = true;
  for (int i = 0; i < 10; ++i)
  {
    Led(LED_BLUE, state);
    Led(LED_RED, state);
    Led(LED_YELLOW, state);
    Led(LED_WHITE, state);
    DelayMs(40);
    state = !state;
  }

  if (m_heartbeatFn != nullptr)
  {
    m_heartbeatFn();
  }
}

void Generator::InitIoPins()
{
  gpio_init(USE_WEEKLY_EXERCISER);
  gpio_set_dir(USE_WEEKLY_EXERCISER, GPIO_IN);
  gpio_pull_up(USE_WEEKLY_EXERCISER);

  gpio_init(GENERATOR_RUNNING);
  gpio_set_dir(GENERATOR_RUNNING, GPIO_IN);
  gpio_pull_up(GENERATOR_RUNNING);

  gpio_init(LINE_POWER_FAIL);
  gpio_set_dir(LINE_POWER_FAIL, GPIO_IN);
  gpio_pull_up(LINE_POWER_FAIL);

  gpio_init(START_STOP_RELAY);
  gpio_set_dir(START_STOP_RELAY, GPIO_OUT);

#ifdef USING_LED_RESISTORS
  gpio_init(LED_BLUE);
  gpio_set_dir(LED_BLUE, GPIO_OUT);

  gpio_init(LED_RED);
  gpio_set_dir(LED_RED, GPIO_OUT);

  gpio_init(LED_YELLOW);
  gpio_set_dir(LED_YELLOW, GPIO_OUT);

  gpio_init(LED_WHITE);
  gpio_set_dir(LED_WHITE, GPIO_OUT);
#else
  gpio_init(LED_BLUE);
  gpio_set_dir(LED_BLUE, GPIO_IN);
  gpio_pull_down(LED_BLUE);

  gpio_init(LED_RED);
  gpio_set_dir(LED_RED, GPIO_IN);
  gpio_pull_down(LED_RED);

  gpio_init(LED_YELLOW);
  gpio_set_dir(LED_YELLOW, GPIO_IN);
  gpio_pull_down(LED_YELLOW);

  gpio_init(LED_WHITE);
  gpio_set_dir(LED_WHITE, GPIO_IN);
  gpio_pull_down(LED_WHITE);
#endif
}

bool Generator::ShouldExerciseNow()
{
  if (!m_usesWeeklyExerciser)
  {
    m_forcedWeeklyRun = false;
    return false;
  }
  if (m_ticksSinceStart > kWeeklyEndTicks)
  {
    m_forcedWeeklyRun = false;
    return false;
  }
  if ((m_ticksSinceStart >= kWeeklyStartTicks) && (m_ticksSinceStart <= kWeeklyEndTicks))
  {
    m_forcedWeeklyRun = true;
    return true;
  }
  m_forcedWeeklyRun = false;
  return false;
}

void Generator::ReadInputs()
{
  const bool weeklyExerciserSelected = !gpio_get(USE_WEEKLY_EXERCISER);
  if (!m_usesWeeklyExerciser && weeklyExerciserSelected)
  {
    m_ticksSinceStart = 21;
  }
  m_usesWeeklyExerciser = weeklyExerciserSelected;

  const bool generatorRunningNow = !gpio_get(GENERATOR_RUNNING);
  if (generatorRunningNow != m_generatorRunning)
  {
    if (m_generatorPowerStable > kGeneratorRunningDebounceTicks)
    {
      m_generatorRunning = generatorRunningNow;
      if (m_generatorRunning)
      {
        PublishEvent(GeneratorEvent::Running);
      } else 
      if (!m_genPowerRequested)
      {
        m_ticksSinceStart = 20;
      }
      m_generatorPowerStable = 0; // test this case CS
    } else
    {
      ++m_generatorPowerStable;
    }
  } else
  {
    m_generatorPowerStable = 0;
  }

  const bool powerRequestedNow = (!gpio_get(LINE_POWER_FAIL)) || m_forcedWeeklyRun;
  if (powerRequestedNow != m_genPowerRequested)
  {
    if (m_genPowerRequestedStable >= kPowerRequestDebounceTicks)
    {
      m_genPowerRequested = powerRequestedNow;
      if (m_forcedWeeklyRun || m_startedByExerciser)
      {
        if (m_genPowerRequested)
        {
          PublishEvent(GeneratorEvent::WeeklyExerciseStart);
        } else
        {
          PublishEvent(GeneratorEvent::WeeklyExerciseEnd);
        }
        m_startedByExerciser = m_genPowerRequested;
      } else
      {
        if (m_genPowerRequested)
        {
          PublishEvent(GeneratorEvent::PowerFailure);
        } else
        {
          PublishEvent(GeneratorEvent::PowerRestored);
        }
      }
      m_genPowerRequestedStable = 0; 
    } else
    {
      ++m_genPowerRequestedStable;
    }
  } else
  {
    m_genPowerRequestedStable = 0;
  }
}

void Generator::RunOneTick()
{
  ++m_ticksSinceStart;
  if (m_failedStartRetryCounter > kMaxStartAttempts)
  {
    BlinkFailure();
    return;
  }
  ReadInputs();
  AliveBlink();
  if (m_usesWeeklyExerciser)
  {
    ShouldExerciseNow();
  } else
  {
    m_forcedWeeklyRun = false;
  }

  if (m_generatorRunning)
  {
    m_startCounter = 0;
    m_failedStartRetryCounter = 0;
    if (m_genPowerRequested)
    {
      m_coolDownCounter = 0;
    } else
    {
      ++m_coolDownCounter;
      if (m_coolDownCounter >= kCooldownTicks)
      {
        PulseStopButton();
        m_coolDownCounter = 0;
      }
    }
    return;
  }

  m_coolDownCounter = 0;
  if (m_genPowerRequested && (m_ticksSinceStart > 15))
  {
    if (m_startCounter == 0)
    {
      PulseStartButton();
      ++m_failedStartRetryCounter;
    }

    ++m_startCounter;
    if ((m_startCounter > kTimeTicksBetweenRestartAttempts) &&
        (m_failedStartRetryCounter <= kMaxStartAttempts))
    {
      if (m_failedStartRetryCounter == kMaxStartAttempts)
      {
        printf("Failed to start generator after 2 attempts, entering failure mode\n");
        PublishEvent(GeneratorEvent::StartFailure);
        ++m_failedStartRetryCounter;
      } else
      {
        m_startCounter = 0;
      }
    }
    return;
  }
  m_startCounter = 0;
  m_failedStartRetryCounter = 0;
}
