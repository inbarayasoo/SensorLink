#pragma once

// A typed, statically-allocated wrapper around FreeRTOS's queue: the pipe
// sensor_task uses to hand each reading to telemetry_task without either of
// them touching the other's memory directly. Without a queue here,
// sensor_task would have to write straight into some variable telemetry_task
// also reads -- and since they run as independent, preemptible tasks, a
// context switch landing mid-write would hand telemetry_task a torn, half
// updated reading. The queue makes "one whole item, or wait" the only thing
// either side can observe.
//
// Capacity and the item type T are both template parameters, so the storage
// for Capacity items of type T sits inside the Queue object itself --
// exactly the same static-allocation discipline as Task: reserved once at
// compile time, nothing to free, no fragmentation possible after months of
// uptime.
#include "FreeRTOS.h"
#include "queue.h"

namespace os {

template <typename T, UBaseType_t Capacity>
class Queue {
public:
    Queue() {
        // Signature (see third_party/FreeRTOS-Kernel/include/queue.h):
        //   xQueueCreateStatic(UBaseType_t uxQueueLength,
        //                       UBaseType_t uxItemSize,
        //                       uint8_t *pucQueueStorage,
        //                       StaticQueue_t *pxQueueBuffer)
        // It returns a QueueHandle_t -- assign it to handle_.
        // uxItemSize is sizeof(T); the storage buffer is storage_ below,
        // and its size in bytes must be Capacity * sizeof(T).
        handle_ = xQueueCreateStatic(Capacity, sizeof(T), storage_, &queue_buffer_);
    }

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    // Copies item onto the back of the queue. Blocks the calling task for up
    // to ticks_to_wait if the queue is currently full (this is exactly the
    // backpressure point: if telemetry_task falls behind and the queue
    // fills, sensor_task blocks here instead of racing ahead and silently
    // dropping readings). Returns false only if it timed out still full.
    bool send(const T& item, TickType_t ticks_to_wait = portMAX_DELAY) {
        // Compare the result to pdTRUE and return that as a bool.
        return xQueueSendToBack(handle_, &item, ticks_to_wait) == pdTRUE;
    }

    // Copies the item at the front of the queue into out and removes it.
    // Blocks the calling task for up to ticks_to_wait if the queue is
    // currently empty. Returns false only if it timed out still empty.
    bool receive(T& out, TickType_t ticks_to_wait = portMAX_DELAY) {
        // Compare the result to pdTRUE and return that as a bool.
        return xQueueReceive(handle_, &out, ticks_to_wait) == pdTRUE;
    }

private:
    uint8_t storage_[Capacity * sizeof(T)]{};
    StaticQueue_t queue_buffer_{};
    QueueHandle_t handle_ = nullptr;
};

}  // namespace os
