#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

// FreeRTOS configuration for the SensorLink node, running on a Cortex-M3
// (mps2-an385) emulated by QEMU. Trimmed down from the stock FreeRTOS demo
// config to only what this project actually uses — every enabled feature
// below has to justify its RAM/flash cost on a device this small.

// --- Scheduling ---------------------------------------------------------
#define configUSE_PREEMPTION                     1  // a higher-priority task always preempts a lower one
#define configUSE_TIME_SLICING                   1  // equal-priority tasks round-robin on the tick
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  1  // Cortex-M3 has a CLZ instruction; use it to pick the next task in O(1)
#define configCPU_CLOCK_HZ                       ( 25000000UL )  // mps2-an385 default core clock
#define configTICK_RATE_HZ                       ( ( TickType_t ) 1000 )  // 1 ms tick
#define configMAX_PRIORITIES                     ( 5 )
#define configMINIMAL_STACK_SIZE                 ( ( uint16_t ) 128 )  // words; the idle task's stack
#define configMAX_TASK_NAME_LEN                  16
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1

// --- Synchronization primitives ------------------------------------------
// sensor_task / telemetry_task / command_task talk to each other through
// exactly these two mechanisms: a queue for readings, a mutex for the
// shared sample-rate config.
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              0
#define configUSE_COUNTING_SEMAPHORES            0
#define configUSE_QUEUE_SETS                     0
#define configQUEUE_REGISTRY_SIZE                8
#define configUSE_TASK_NOTIFICATIONS             1

// Software timers and co-routines are unused in this project.
#define configUSE_TIMERS                         0
#define configUSE_CO_ROUTINES                    0

// --- Memory ---------------------------------------------------------------
// Every task, queue and mutex in this project is allocated statically (see
// os/Task.hpp, os/Queue.hpp, os/Mutex.hpp): fixed memory, known at compile
// time, no fragmentation, no allocation failure possible at runtime. Turning
// dynamic allocation off entirely means portable/MemMang/heap_*.c is never
// even compiled in — calling pvPortMalloc by mistake is a link error, not a
// runtime surprise.
#define configSUPPORT_STATIC_ALLOCATION           1
#define configSUPPORT_DYNAMIC_ALLOCATION          0

// --- Fault detection --------------------------------------------------------
#define configCHECK_FOR_STACK_OVERFLOW            2  // both overflow-detection methods
#define configUSE_MALLOC_FAILED_HOOK              0  // no dynamic allocation, so no such hook to call
#define configASSERT( x )    if( ( x ) == 0 ) vAssertCalled( __FILE__, __LINE__ )
#ifdef __cplusplus
extern "C" {
#endif
void vAssertCalled( const char * pcFileName, unsigned long ulLine );
#ifdef __cplusplus
}
#endif

// --- Diagnostics ------------------------------------------------------------
#define configUSE_TRACE_FACILITY                  0
#define configGENERATE_RUN_TIME_STATS             0
#define configUSE_IDLE_HOOK                       0
#define configUSE_TICK_HOOK                       0

// --- NVIC priorities (Cortex-M3 requirement, see FreeRTOS's Cortex-M port docs) ---
// QEMU's model of this board does not implement priority-bit grouping, so we
// use the values the reference demo for this exact board uses.
#define configKERNEL_INTERRUPT_PRIORITY           ( 255 )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY      ( 4 )

// --- API inclusion ---------------------------------------------------------
// Only the calls this project actually makes; everything else stays out of
// the firmware image.
#define INCLUDE_vTaskDelay                        1
#define INCLUDE_vTaskDelayUntil                   1
#define INCLUDE_vTaskSuspend                      1
#define INCLUDE_uxTaskGetStackHighWaterMark        1  // lets us measure real stack usage per task, not just guess it
#define INCLUDE_xTaskGetSchedulerState             1

#endif /* FREERTOS_CONFIG_H */
