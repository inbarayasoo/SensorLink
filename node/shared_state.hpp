#pragma once

#include <cstddef>
#include <cstdint>

#include "os/Mutex.hpp"
#include "os/Queue.hpp"
#include "session.hpp"
#include "tasks/reading.hpp"

namespace drivers {
class UartCmsdk;
}

namespace tasks {

// Everything command_task will be able to change, and sensor_task must read
// before every reading: right now just the sample rate, matching
// proto::ConfigPayload's sample_rate_hz field one-for-one. Always accessed
// through config_mutex -- an unprotected read here is exactly the torn-read
// scenario os/Mutex.hpp's comment describes.
struct DeviceConfig {
    std::uint8_t sample_rate_hz = 2;  // default, until a CONFIG message arrives
};

constexpr std::size_t kQueueCapacity = 8;

// One instance of this exists, created inside main() (see node/main.cpp for
// why not as a global object), and handed to every task through its context
// pointer. The whole pipeline's shared state lives in exactly one place.
struct Pipeline {
    DeviceConfig config;
    SessionState session;

    // One mutex for both fields above: they are always updated together (a
    // CONFIG frame sets the rate and confirms the session in the same
    // instant), so one lock covering both is simpler to reason about than a
    // second mutex that would just always be taken alongside the first one
    // anyway.
    os::Mutex config_mutex;

    os::Queue<RawReading, kQueueCapacity> raw_queue;
    os::Queue<ProcessedReading, kQueueCapacity> processed_queue;

    // The one physical link out of (and into) the device. Not owned here --
    // main() constructs the UartCmsdk and points this at it before any task
    // starts, exactly like it does for the rest of Pipeline's fields. A
    // forward declaration is enough in this header because nothing here
    // calls a method on it; telemetry_task.cpp and command_task.cpp include
    // drivers/uart_cmsdk.hpp themselves for that.
    drivers::UartCmsdk* uart = nullptr;
};

}  // namespace tasks
