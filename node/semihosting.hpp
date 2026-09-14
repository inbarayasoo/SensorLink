#pragma once

// A minimal wrapper around ARM semihosting: a debug-only channel where code
// running on the target (real silicon under a debugger, or here, QEMU) asks
// the host machine to do something on its behalf — print text, read a file,
// exit the process. It only works under a debugger/emulator; it is not a
// mechanism a shipped device could rely on. We use it for exactly one thing:
// getting diagnostic text out of the firmware before the real telemetry link
// (the UART -> proto:: framing -> server pipeline) exists to carry it.
//
// The mechanism itself is a single instruction: BKPT 0xAB. The debugger/QEMU
// traps that breakpoint, inspects r0 for an operation code and r1 for its
// argument, performs the operation, and resumes execution. Operation 0x04 is
// SYS_WRITE0: "print the null-terminated string at the address in r1".
namespace semihosting {

inline void write_line(const char* message) noexcept {
    register long operation asm("r0") = 0x04;  // SYS_WRITE0
    register const char* argument asm("r1") = message;
    asm volatile("bkpt 0xAB" : "+r"(operation) : "r"(argument) : "memory");
}

}  // namespace semihosting
