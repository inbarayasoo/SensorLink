#pragma once

// The lowest layer of the telemetry pipeline: raw access to one CMSDK UART
// peripheral -- the "wire" a cooling unit's controller would use to talk to
// the outside world. This class knows nothing about frames, CRCs, or
// message types; it knows exactly two things, "put one byte on the wire"
// and "is a byte waiting on the wire", the same two primitives every serial
// link boils down to underneath whatever protocol runs over it. Everything
// in proto/ -- COBS framing, the CRC, the message layouts -- is a layer
// built entirely on top of these two operations; this file has no idea any
// of that exists.
#include <cstddef>
#include <cstdint>

#include "CMSDK_CM3.h"

namespace drivers {

class UartCmsdk {
public:
    // registers: one of CMSDK_UART0..CMSDK_UART4 (see node/cmsis/CMSDK_CM3.h).
    // QEMU's mps2-an385 model routes CMSDK_UART0 to the console given on the
    // command line (-nographic), which is what lets us see the device's
    // output at all without a real server on the other end yet.
    UartCmsdk(CMSDK_UART_TypeDef* registers, std::uint32_t clock_hz, std::uint32_t baud_rate)
        : registers_(registers) {
        registers_->BAUDDIV = clock_hz / baud_rate;
        registers_->CTRL = CMSDK_UART_CTRL_TXEN_Msk | CMSDK_UART_CTRL_RXEN_Msk;
    }

    UartCmsdk(const UartCmsdk&) = delete;
    UartCmsdk& operator=(const UartCmsdk&) = delete;

    // Blocks -- polls the status register -- until the hardware's one-byte
    // transmit buffer has room, then hands it the byte. "Polling" means
    // exactly this: asking the same question in a tight loop instead of
    // being told the answer by an interrupt. Simple and correct, at the
    // cost of the calling task doing nothing else while it waits; good
    // enough while telemetry_task has nothing better to do until the byte
    // is sent anyway. Interrupt-driven transmit is a documented future
    // improvement, not implemented here.
    void write_byte(std::uint8_t byte) {
        while (registers_->STATE & CMSDK_UART_STATE_TXBF_Msk) {
        }
        registers_->DATA = byte;
    }

    void write(const std::uint8_t* data, std::size_t length) {
        for (std::size_t i = 0; i < length; ++i) {
            write_byte(data[i]);
        }
    }

    // Non-blocking on purpose: command_task calls this once per loop
    // iteration rather than waiting here, so a device with nothing to
    // receive never starves sensor_task or telemetry_task of CPU time.
    // Returns false immediately if no byte is waiting.
    bool try_read_byte(std::uint8_t* out_byte) {
        if ((registers_->STATE & CMSDK_UART_STATE_RXBF_Msk) == 0) {
            return false;
        }
        *out_byte = static_cast<std::uint8_t>(registers_->DATA & CMSDK_UART_DATA_Msk);
        return true;
    }

private:
    CMSDK_UART_TypeDef* registers_;
};

}  // namespace drivers
