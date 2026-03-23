// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

#include "pico/stdlib.h"

/**
 * @brief High-level generator state changes emitted by the controller.
 *
 * These values are used to decouple generator edge detection from downstream
 * notification logic. The generator controller publishes one of these events,
 * and a caller-provided sink decides whether to queue, drop, or transform it.
 */
enum class GeneratorEvent
{
  ControllerOnline,
  PowerFailure,
  Running,
  PowerRestored,
  StartFailure,
  WeeklyExerciseStart,
  WeeklyExerciseEnd
};

/**
 * @brief Timestamped generator event payload.
 *
 * @var GeneratorEventMessage::eventCode
 * Logical event emitted by the generator controller.
 *
 * @var GeneratorEventMessage::timestampMs
 * Monotonic milliseconds since boot when the event was published.
 */
struct GeneratorEventMessage
{
  GeneratorEvent eventCode;
  uint32_t timestampMs;
};

/**
 * @brief Generator input polling and start/stop control state machine.
 *
 * The controller reads the hardware inputs, debounces state transitions, and
 * publishes generator events through a caller-provided sink callback. In the
 * RTOS path the sink is backed by a queue; in the fallback path it can enqueue
 * directly into the mail service.
 */
class Generator
{
public:
  /**
   * @brief Optional heartbeat callback invoked once per controller tick.
   */
  using HeartbeatFn = void (*)();

  /**
   * @brief Event sink callback used to export generator events.
   * @param context Caller-provided sink context.
   * @param message Event payload to deliver.
   * @return true if the event was accepted; false if it was dropped.
   */
  using EventSinkFn = bool (*)(void *context, const GeneratorEventMessage &message);

  /**
   * @brief Constructs the generator controller.
   * @param eventSinkFn Callback used to publish generator events.
   * @param eventSinkContext Context pointer passed back to the event sink.
   * @param heartbeatFn Optional callback used for a simple alive pulse.
   */
  Generator(EventSinkFn eventSinkFn, void *eventSinkContext, HeartbeatFn heartbeatFn);

  /**
   * @brief Returns the number of controller ticks since startup.
   * @return Tick count used by internal timing logic.
   */
  uint64_t GetTicksSinceStart() const { return m_ticksSinceStart; }

  /**
   * @brief Advances the controller by one periodic tick.
   */
  void RunOneTick();

private:
  static constexpr uint kGeneratorRunningDebounceTicks = 12;
  static constexpr uint kPowerRequestDebounceTicks = 18;
  static constexpr uint kMaxStartAttempts = 2;
  static constexpr uint kTimeTicksBetweenRestartAttempts = 110;
  static constexpr uint64_t kWeeklyStartTicks = 7ull * 24ull * 60ull * 60ull * 2ull;
  static constexpr uint64_t kWeeklyEndTicks = kWeeklyStartTicks + (5ull * 60ull * 2ull);
  static constexpr uint kCooldownTicks = 80;

  EventSinkFn m_eventSinkFn;
  void *m_eventSinkContext;
  HeartbeatFn m_heartbeatFn;
  uint32_t m_droppedEvents = 0;

  uint64_t m_ticksSinceStart = 0;
  bool m_generatorRunning = false;
  bool m_genPowerRequested = false;
  bool m_usesWeeklyExerciser = false;
  bool m_forcedWeeklyRun = false;
  bool m_startedByExerciser = false;
  uint m_coolDownCounter = 0;
  uint m_startCounter = 0;
  uint m_failedStartRetryCounter = 0;
  uint m_generatorPowerStable = 0;
  uint m_genPowerRequestedStable = 0;

  void PulseStartButton();
  void PulseStopButton();
  void AliveBlink();
  void InitIoPins();
  void ReadInputs();
  void Led(uint id, bool state);
  void BlinkFailure();
  bool ShouldExerciseNow();

  /**
   * @brief Publishes a generator event through the configured sink.
   * @param eventCode Event code to emit.
   */
  void PublishEvent(GeneratorEvent eventCode);
};
