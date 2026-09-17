#pragma once

// A thin RAII wrapper around FreeRTOS's static task creation. Every one of
// the three pipeline stages -- sensor_task, telemetry_task, command_task --
// is one instance of this class: same wrapper, three different priorities
// and stack sizes, three independent "workers" that the scheduler
// interleaves on one CPU core.
//
// Design choice: the task's stack lives inside the Task object itself, sized
// at compile time via the StackWords template parameter. That is what makes
// this RAII rather than a bare API call -- construction reserves the memory
// once and for all, and there is no separate "free the stack" step for
// anyone to forget, because there is nothing to free: it was never on the
// heap.
//
// Design choice: the task body is a plain C function pointer
// (void (*)(void*)), matching FreeRTOS's own TaskFunction_t exactly --
// not std::function. std::function can allocate on the heap to store
// whatever callable you give it (a capturing lambda, in particular), and
// this project's rule is zero dynamic allocation in the data path. A plain
// function pointer plus a void* context parameter (the same pattern
// pthread_create and every other C threading API uses) gets a task its
// per-instance state without ever touching the heap.
#include "FreeRTOS.h"
#include "task.h"

namespace os {

template <UBaseType_t StackWords>
class Task {
public:
    using Function = void (*)(void*);

    Task(const char* name, UBaseType_t priority, Function function, void* context) {
        // Signature (see third_party/FreeRTOS-Kernel/include/task.h):
        //   xTaskCreateStatic(TaskFunction_t pxTaskCode,
        //                      const char *pcName,
        //                      uint32_t ulStackDepth,
        //                      void *pvParameters,
        //                      UBaseType_t uxPriority,
        //                      StackType_t *puxStackBuffer,
        //                      StaticTask_t *pxTaskBuffer)
        // It returns a TaskHandle_t -- assign it to handle_.
        // The stack buffer and TCB it needs are the members below:
        // stack_ and tcb_.
        handle_ = xTaskCreateStatic(function, name, StackWords, context, priority, stack_, &tcb_);
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // A Task is meant to be created once, live for the lifetime of the
    // program, and never be destroyed while the scheduler is running -- so
    // there is deliberately no destructor that calls vTaskDelete(). See the
    // note in node/main.cpp (or wherever this is instantiated) about static
    // storage duration: this object's stack_ and tcb_ must outlive every
    // context switch into this task, which for an embedded device that
    // never returns from main() means "forever".
    TaskHandle_t handle() const { return handle_; }

private:
    StackType_t stack_[StackWords]{};
    StaticTask_t tcb_{};
    TaskHandle_t handle_ = nullptr;
};

}  // namespace os
