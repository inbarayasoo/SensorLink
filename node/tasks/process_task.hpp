#pragma once

namespace tasks {

// Entry point for the process task. context must point to a tasks::Pipeline
// (see node/shared_state.hpp).
void ProcessTask(void* context);

}  // namespace tasks
