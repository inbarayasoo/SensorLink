#pragma once

namespace tasks {

// Entry point for the command task. context must point to a tasks::Pipeline
// whose uart field has already been set (see node/main.cpp).
void CommandTask(void* context);

}  // namespace tasks
