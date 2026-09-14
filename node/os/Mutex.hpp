#pragma once

// A statically-allocated mutex plus an RAII guard: this is exactly what
// protects the shared sample-rate config that command_task writes to
// (whenever a CONFIG or SLOW_DOWN message arrives from the server) and
// sensor_task reads from (every time it decides how long to sleep before the
// next reading). Without the mutex, a context switch landing between "sensor
// task reads the rate" and "command task finishes writing a new rate" could
// hand sensor_task a rate that is neither the old value nor the new one --
// a torn read of a multi-field struct, mid-update.
//
// The nested Guard class is the actual RAII: its constructor takes the
// mutex, its destructor releases it. That is the whole mechanism -- as soon
// as a Guard goes out of scope (a normal return, an early return, any exit
// path), the lock is released, because the guard's destructor runs
// automatically. There is no code path through the function that can leave
// the mutex held.
#include "FreeRTOS.h"
#include "semphr.h"

namespace os {

class Mutex {
public:
    Mutex() {
        // Signature (see third_party/FreeRTOS-Kernel/include/semphr.h):
        //   xSemaphoreCreateMutexStatic(StaticSemaphore_t *pxMutexBuffer)
        // It returns a SemaphoreHandle_t -- assign it to handle_.
        // The buffer it needs is buffer_ below.
        handle_ = xSemaphoreCreateMutexStatic(&buffer_);
    }

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    class Guard {
    public:
        explicit Guard(Mutex& mutex) : mutex_(mutex) {
            xSemaphoreTake(mutex_.handle_, portMAX_DELAY);
        }

        ~Guard() {
            xSemaphoreGive(mutex_.handle_);
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        Mutex& mutex_;
    };

private:
    StaticSemaphore_t buffer_{};
    SemaphoreHandle_t handle_ = nullptr;
};

}  // namespace os
