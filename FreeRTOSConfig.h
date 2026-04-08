#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration avoids pulling Pico SDK headers into FreeRTOSConfig.h. */
uint64_t time_us_64(void);

#if PICO_RP2040
#define configCPU_CLOCK_HZ                         ( ( uint32_t ) 125000000 )
#else
#define configCPU_CLOCK_HZ                         ( ( uint32_t ) 150000000 )
#endif

#define configUSE_PREEMPTION                       1
#define configUSE_TIME_SLICING                     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION    0
#define configUSE_TICKLESS_IDLE                    0
#define configUSE_IDLE_HOOK                        0
#define configUSE_TICK_HOOK                        0
#define configTICK_RATE_HZ                         ( ( uint32_t ) 1000 )
#define configMAX_PRIORITIES                       6
#if PICO_RP2350
#define configMINIMAL_STACK_SIZE                   1024
#else
#define configMINIMAL_STACK_SIZE                   256
#endif
#define configMAX_TASK_NAME_LEN                    16
#define configTICK_TYPE_WIDTH_IN_BITS              TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                    1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES      1
#define configQUEUE_REGISTRY_SIZE                  8
#define configENABLE_BACKWARD_COMPATIBILITY        0

#define configSUPPORT_DYNAMIC_ALLOCATION           1
#define configSUPPORT_STATIC_ALLOCATION            0
#if PICO_RP2350
#define configTOTAL_HEAP_SIZE                      ( ( size_t ) ( 128 * 1024 ) )
#else
#define configTOTAL_HEAP_SIZE                      ( ( size_t ) ( 96 * 1024 ) )
#endif
#define configAPPLICATION_ALLOCATED_HEAP           0
#define configUSE_MALLOC_FAILED_HOOK               1
#define configCHECK_FOR_STACK_OVERFLOW             2
#define configRECORD_STACK_HIGH_ADDRESS            1

#define configUSE_MUTEXES                          1
#define configUSE_RECURSIVE_MUTEXES                1
#define configUSE_COUNTING_SEMAPHORES              1

#define configUSE_TIMERS                           1
#define configTIMER_TASK_PRIORITY                  ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH                   8
#if PICO_RP2350
#define configTIMER_TASK_STACK_DEPTH               1024
#else
#define configTIMER_TASK_STACK_DEPTH               512
#endif

#define configNUMBER_OF_CORES                      2
#if ( configNUMBER_OF_CORES > 1 )
#define configRUN_MULTIPLE_PRIORITIES              1
#else
#define configRUN_MULTIPLE_PRIORITIES              0
#endif
#define configUSE_CORE_AFFINITY                    1
#define configTASK_DEFAULT_CORE_AFFINITY           0xFFFFFFFFUL
#define configUSE_TASK_PREEMPTION_DISABLE          0
#define configUSE_PASSIVE_IDLE_HOOK                0
#define configTIMER_SERVICE_TASK_CORE_AFFINITY     0xFFFFFFFFUL

#define configUSE_CO_ROUTINES                      0
#define configMAX_CO_ROUTINE_PRIORITIES            1

#define configUSE_TRACE_FACILITY                   1
#define configUSE_STATS_FORMATTING_FUNCTIONS       1
#define configGENERATE_RUN_TIME_STATS              1

/*
 * Runtime stats counter source:
 * - Uses the SDK's 1 MHz free-running timer (1 us resolution)
 * - Scheduler tick is 1 kHz, so this is 1000x faster than the tick source
 * - No extra hardware timer setup required, minimizing resource impact
 */
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()   do { } while(0)
#define portGET_RUN_TIME_COUNTER_VALUE()           ( ( uint32_t ) time_us_64() )

#if PICO_RP2350
#define configENABLE_MPU                           0
#define configENABLE_TRUSTZONE                     0
#define configRUN_FREERTOS_SECURE_ONLY            1
#define configENABLE_FPU                           1
#define configMAX_SYSCALL_INTERRUPT_PRIORITY      16
#endif

#define INCLUDE_vTaskPrioritySet                   1
#define INCLUDE_uxTaskPriorityGet                  1
#define INCLUDE_vTaskDelete                        1
#define INCLUDE_vTaskSuspend                       1
#define INCLUDE_xResumeFromISR                     1
#define INCLUDE_vTaskDelayUntil                    1
#define INCLUDE_vTaskDelay                         1
#define INCLUDE_xTaskGetSchedulerState             1
#define INCLUDE_xTaskGetCurrentTaskHandle          1
#define INCLUDE_uxTaskGetStackHighWaterMark        1
#define INCLUDE_xTaskGetIdleTaskHandle             1
#define INCLUDE_eTaskGetState                      1
#define INCLUDE_xEventGroupSetBitFromISR           1
#define INCLUDE_xTimerPendFunctionCall             1

#define configASSERT( x )

#ifdef __cplusplus
}
#endif

#endif
