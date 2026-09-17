// The handful of callbacks FreeRTOS requires from the application when
// configSUPPORT_STATIC_ALLOCATION and configCHECK_FOR_STACK_OVERFLOW are
// turned on (see FreeRTOSConfig.h). None of this is telemetry logic — it is
// the safety net underneath it: if a task's stack budget was sized wrong, or
// an invariant we asserted on is violated, we want a clear halt with a
// message, not silent memory corruption creeping into the sensor pipeline.
//
// extern "C": FreeRTOS's kernel is C code and calls these by their exact,
// unmangled names. Without extern "C", the C++ compiler would mangle each
// name (encoding the parameter types into it) and the kernel's call to the
// plain C symbol would fail to link.
#include "FreeRTOS.h"
#include "task.h"

#include "semihosting.hpp"

extern "C" {

// Required because configSUPPORT_STATIC_ALLOCATION == 1: FreeRTOS still
// creates its own idle task (it needs somewhere for the CPU to go when every
// sensor/process/telemetry/command task is blocked), but with dynamic
// allocation off it cannot pvPortMalloc that task's memory — it asks us for
// a fixed, compile-time buffer instead.
void vApplicationGetIdleTaskMemory(StaticTask_t** idle_task_tcb_buffer,
                                    StackType_t** idle_task_stack_buffer,
                                    uint32_t* idle_task_stack_size) {
    static StaticTask_t idle_task_tcb;
    static StackType_t idle_task_stack[configMINIMAL_STACK_SIZE];

    *idle_task_tcb_buffer = &idle_task_tcb;
    *idle_task_stack_buffer = idle_task_stack;
    *idle_task_stack_size = configMINIMAL_STACK_SIZE;
}

// configCHECK_FOR_STACK_OVERFLOW == 2: on every context switch, FreeRTOS
// checks the outgoing task's stack against both its high-water mark and a
// canary pattern. A cooling-unit node runs unattended for months; a
// sensor_task whose stack was under-budgeted must stop loudly here, not
// corrupt some other task's state next to it in RAM.
//
// task_name stays a plain char* (not const): FreeRTOS's own task.h forward-
// declares this hook with that exact signature, and a C-linkage function's
// definition must match its declaration exactly -- adding const here is a
// conflicting-declaration compile error, not a style improvement.
// cppcheck-suppress constParameterPointer
void vApplicationStackOverflowHook(TaskHandle_t, char* task_name) {
    semihosting::write_line("STACK OVERFLOW in task: ");
    semihosting::write_line(task_name);
    for (;;) {
    }
}

// configASSERT(x) calls this when x is false. Kept deliberately simple: this
// fires during development against a fixed set of invariants (e.g. "the
// queue we just sized actually holds one in-flight sample"), not against
// field data, so a message plus a halt is enough to go find it in the
// debugger.
void vAssertCalled(const char* file_name, unsigned long line) {
    semihosting::write_line("ASSERT FAILED in: ");
    semihosting::write_line(file_name);
    (void)line;
    for (;;) {
    }
}

}  // extern "C"
