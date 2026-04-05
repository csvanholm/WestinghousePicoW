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
   */
  Generator(EventSinkFn eventSinkFn, void *eventSinkContext);

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

  /**
   * @brief Drives the start relay for a fixed pulse width.
   */
  void PulseStartButton();

  /**
   * @brief Drives the stop relay for a fixed pulse width.
   */
  void PulseStopButton();

  /**
   * @brief Updates activity/status LEDs once per controller tick.
   */
  void AliveBlink();

  /**
   * @brief Configures all GPIO inputs/outputs used by the controller.
   */
  void InitIoPins();

  /**
   * @brief Samples and debounces hardware inputs, publishing edge events.
   */
  void ReadInputs();

  /**
   * @brief Applies target LED state respecting hardware build mode.
   * @param id GPIO number for the LED.
   * @param state Desired LED logical state.
   */
  void Led(uint id, bool state);

  /**
   * @brief Repeating failure blink pattern used after exhausted start retries.
   */
  void BlinkFailure();

  /**
   * @brief Checks whether weekly exercise run window is currently active.
   * @return true when an exercise run should be forced this tick.
   */
  bool ShouldExerciseNow();

  /**
   * @brief Publishes a generator event through the configured sink.
   * @param eventCode Event code to emit.
   */
  void PublishEvent(GeneratorEvent eventCode);
};
