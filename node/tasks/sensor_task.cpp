// The first stage of the pipeline: on every tick of its own, invents one
// temperature reading (there is no real probe attached to this emulator)
// and hands it downstream. This is the task that would, on a real cooling
// unit, be the one actually talking to the temperature/pressure probes --
// everything after it (process_task, telemetry_task) has no idea whether
// the reading came from real hardware or not.
#include "tasks/sensor_task.hpp"

#include <cstdint>

#include "FreeRTOS.h"
#include "task.h"

#include "os/Mutex.hpp"
#include "shared_state.hpp"

namespace tasks {
namespace {

// A stand-in for a real probe. Deliberately deterministic (a triangle wave,
// not random noise): the point of a fake sensor in this project is to
// exercise everything downstream -- process_task's averaging, the
// threshold, telemetry_task's encoding -- against known, reproducible
// input, not to look realistic.
std::int32_t FakeReadingMilli(std::uint32_t sample_index) {
    
    constexpr std::int32_t kBaselineMilli = 4000;
    constexpr std::int32_t kSwingMilli = 3000;
    constexpr std::uint32_t kPeriodSamples = 20;

    std::int32_t position = static_cast<std::int32_t>(sample_index % kPeriodSamples);
    std::int32_t triangle = position <= static_cast<std::int32_t>(kPeriodSamples / 2)
                            ? position
                            : static_cast<std::int32_t>(kPeriodSamples) - position;
    return kBaselineMilli - kSwingMilli + (triangle * 4 * kSwingMilli) / static_cast<std::int32_t>(kPeriodSamples);
}

}  // namespace

void SensorTask(void* context) {
    auto* pipeline = static_cast<Pipeline*>(context);
    std::uint32_t sample_index = 0;

    for (;;) {
        std::uint8_t rate_hz;
        {
            os::Mutex::Guard lock(pipeline->config_mutex);
            rate_hz = pipeline->config.sample_rate_hz;
        }

        RawReading reading{};
        reading.value_milli = FakeReadingMilli(sample_index);
        reading.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        pipeline->raw_queue.send(reading);
        ++sample_index;

        const std::uint8_t safe_rate_hz = (rate_hz == 0) ? 1 : rate_hz;
        vTaskDelay(pdMS_TO_TICKS(1000 / safe_rate_hz));
        
    }
}

}  // namespace tasks
