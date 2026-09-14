// The one path bytes travel *into* the device: sends the initial HELLO,
// then reads whatever the UART has waiting, feeds it through the same
// proto::frame_parser the server will run, and reacts to whatever completes
// -- CONFIG establishes (or re-confirms) the session and sets the sample
// rate, SLOW_DOWN lowers it again under backpressure. A periodic HEARTBEAT
// goes out on the same schedule, whether or not anything arrived to read.
#include "tasks/command_task.hpp"

#include <cstring>

#include "FreeRTOS.h"
#include "task.h"

#include "proto/frame_parser.hpp"
#include "proto/messages.hpp"

#include "drivers/uart_cmsdk.hpp"
#include "os/Mutex.hpp"
#include "session.hpp"
#include "shared_state.hpp"

namespace tasks {
namespace {

constexpr TickType_t kHeartbeatInterval = pdMS_TO_TICKS(2000);

}  // namespace

void CommandTask(void* context) {
    auto* pipeline = static_cast<Pipeline*>(context);

    // static, not a plain local: frame_parser<> holds two internal buffers
    // sized off kMaxPayloadSize (255), so one instance is roughly 580 bytes
    // -- more than half of this task's entire 1024-byte stack budget. As a
    // stack local, that leaves too little headroom once the tick interrupt's
    // own stacked context and a few levels of call nesting (push_byte ->
    // finish_frame -> cobs_decode/crc16) land on top of it, and it does
    // overflow in practice, not just in theory: this exact overflow was
    // caught by vApplicationStackOverflowHook (see node/hooks.cpp) the first
    // time this task ever parsed a CONFIG frame from the real server.
    // CommandTask runs as exactly one task for the life of the program, so a
    // single static instance is equivalent to the local it replaces --
    // moving it into .bss just takes it off the stack entirely.
    static proto::frame_parser<> parser;

    SendHello(*pipeline);
    TickType_t last_heartbeat = xTaskGetTickCount();

    for (;;) {
        std::uint8_t byte;
        if (!pipeline->uart->try_read_byte(&byte)) {
            // Nothing waiting right now. This is also where the heartbeat
            // timer is checked -- it has to happen on every loop iteration,
            // not just when a byte arrives, or a quiet link (no CONFIG, no
            // SLOW_DOWN) would mean no heartbeat either.
            if (xTaskGetTickCount() - last_heartbeat >= kHeartbeatInterval) {
                SendHeartbeat(*pipeline);
                last_heartbeat = xTaskGetTickCount();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        const proto::ParsedFrame* frame = parser.push_byte(byte);
        if (frame == nullptr) {
            continue;  // this byte didn't complete a frame (or one was dropped)
        }

        if (frame->type == proto::MessageType::kConfig &&
            frame->payload_size == sizeof(proto::ConfigPayload)) {
            proto::ConfigPayload config{};
            std::memcpy(&config, frame->payload, sizeof(config));

            os::Mutex::Guard lock(pipeline->config_mutex);
            pipeline->config.sample_rate_hz = config.sample_rate_hz;
            pipeline->session.session_id = config.session_id;
            pipeline->session.connected = true;
        } else if (frame->type == proto::MessageType::kSlowDown &&
                   frame->payload_size == sizeof(proto::SlowDownPayload)) {
            proto::SlowDownPayload slow_down{};
            std::memcpy(&slow_down, frame->payload, sizeof(slow_down));
            os::Mutex::Guard lock(pipeline->config_mutex);
            pipeline->config.sample_rate_hz = slow_down.new_rate_hz;
        }
    }
}

}  // namespace tasks
