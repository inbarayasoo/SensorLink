// Wires the full four-task pipeline together and boots it:
//   sensor_task -> process_task -> telemetry_task  (data going out)
//   command_task                                    (config coming in)
// This replaces the temporary MonitorTask stand-in from the previous
// sub-section now that telemetry_task can actually put real proto:: frames
// on the wire. session.hpp (the HELLO/CONFIG handshake itself) is still a
// later sub-section -- command_task here only reacts to a CONFIG frame
// whenever one arrives, regardless of who sent it.
#include "FreeRTOS.h"
#include "task.h"

#include "drivers/uart_cmsdk.hpp"
#include "os/Task.hpp"
#include "semihosting.hpp"
#include "shared_state.hpp"
#include "tasks/command_task.hpp"
#include "tasks/process_task.hpp"
#include "tasks/sensor_task.hpp"
#include "tasks/telemetry_task.hpp"

namespace {
constexpr UBaseType_t kStackWords = configMINIMAL_STACK_SIZE * 2;
}  // namespace

int main() {
    semihosting::write_line("SensorLink node booting\n");

    static drivers::UartCmsdk uart0(CMSDK_UART0, configCPU_CLOCK_HZ, 115200);

    static tasks::Pipeline pipeline;
    pipeline.uart = &uart0;

    // Priorities: sensor_task highest -- sampling must never be preempted by
    // anything downstream of it. command_task next -- a SLOW_DOWN or CONFIG
    // message should be applied promptly. process_task and telemetry_task
    // are bulk work further down the pipeline; a short delay in either just
    // means data reaches the wire a little later, not a missed sample.
    static os::Task<kStackWords> sensor("sensor", 3, tasks::SensorTask, &pipeline);
    static os::Task<kStackWords> command("command", 2, tasks::CommandTask, &pipeline);
    static os::Task<kStackWords> process("process", 2, tasks::ProcessTask, &pipeline);
    static os::Task<kStackWords> telemetry("telemetry", 1, tasks::TelemetryTask, &pipeline);

    vTaskStartScheduler();

    // Unreachable with fully static allocation -- see the note on this same
    // loop in earlier revisions of this file.
    for (;;) {
    }
}
