#pragma once

namespace tasks {

// Entry point for the telemetry task. context must point to a
// tasks::Pipeline whose uart field has already been set (see node/main.cpp).
void TelemetryTask(void* context);

}  // namespace tasks
