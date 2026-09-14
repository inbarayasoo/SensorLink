#pragma once

namespace tasks {

// Entry point for the sensor task. context must point to a tasks::Pipeline
// (see node/shared_state.hpp); ownership stays with whoever created it --
// this task only ever borrows the pointer.
void SensorTask(void* context);

}  // namespace tasks
