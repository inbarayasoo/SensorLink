// The second stage of the pipeline: smooths out sensor noise with a moving
// average, and decides whether the current reading means this cooling unit
// is out of its safe range. Neither sensor_task nor telemetry_task make
// that judgment -- it lives here, in exactly one place, so there is exactly
// one definition of "out of range" in the whole firmware.
#include "tasks/process_task.hpp"

#include <cstdint>

#include "shared_state.hpp"

namespace tasks {
namespace {

// Above this, a pharmacy fridge (this project's running example) is out of
// spec -- see docs/PLAN.md's cold-chain scenario.
constexpr std::int32_t kHighThresholdMilli = 5500;  // 5.5 degrees C

}  // namespace

void ProcessTask(void* context) {
    auto* pipeline = static_cast<Pipeline*>(context);

    bool have_average = false;
    std::int32_t average_milli = 0;

    for (;;) {
        RawReading raw{};
        if (!pipeline->raw_queue.receive(raw)) {
            continue;
        }

        if (!have_average) {
            average_milli = raw.value_milli;
            have_average = true;
        } else {
            average_milli += (raw.value_milli - average_milli) >> 2;
        }
        
        ProcessedReading processed{};
        processed.value_milli = average_milli;
        processed.timestamp_ms = raw.timestamp_ms;
        processed.out_of_range = average_milli > kHighThresholdMilli;

        pipeline->processed_queue.send(processed);
    }
}

}  // namespace tasks
