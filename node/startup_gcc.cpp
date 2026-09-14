// Adapted from the FreeRTOS project's official CORTEX_MPS2_QEMU_GCC demo
// (FreeRTOS/FreeRTOS, path FreeRTOS/Demo/CORTEX_MPS2_QEMU_IAR_GCC/build/gcc/
// startup_gcc.c), pinned at commit f4fcc3b228643144727e9257ba12db1cb632b6e6.
// Original license: MIT (see third_party/FreeRTOS-Kernel/LICENSE.md for the
// kernel this vector table hands control to).
//
// Changes from the original: converted to C++ (extern "C" linkage on every
// symbol the C-compiled kernel port calls by name); dropped the two demo
// timer-test interrupt handlers we don't build; the fault handler now prints
// through our own semihosting helper instead of libc's printf, so a
// HardFault never depends on a heap-allocating stdio path we otherwise
// disable project-wide.
//
// This is the very first code that runs after reset, before FreeRTOS, before
// main() -- it exists to answer two questions the CPU asks the instant power
// (or, here, QEMU) applies: "where is the initial stack?" and "where do I
// jump to?". Both answers live in one array below, the vector table, placed
// at the fixed address the Cortex-M3 hardware reads on reset. Without this
// file there is no entry point at all -- the emulator would have nothing
// valid to load.
#include <cstdint>

#include "semihosting.hpp"

extern "C" {

// The kernel's ARM_CM3 port (third_party/FreeRTOS-Kernel/portable/GCC/ARM_CM3/port.c)
// implements these three; the vector table below wires them to the three
// exceptions FreeRTOS's scheduler is built on: SVC to start the first task,
// PendSV to context-switch, SysTick to drive the tick that makes time-slicing
// and vTaskDelay() possible.
extern void vPortSVCHandler(void);
extern void xPortPendSVHandler(void);
extern void xPortSysTickHandler(void);

static void HardFault_Handler(void) __attribute__((naked));
static void Default_Handler(void) __attribute__((naked));
void Reset_Handler(void) __attribute__((naked));

extern int main(void);
extern uint32_t _estack;

// Vector table: an array of addresses at a fixed flash location (see
// node/linker/mps2_an385.ld, section .isr_vector). Entry 0 is not a handler
// at all -- it is the initial stack pointer value the CPU loads before
// anything else runs. Entry 1 is where it jumps. Everything after that is
// "if exception N fires, jump here".
const uint32_t* isr_vector[] __attribute__((section(".isr_vector"), used)) = {
    (uint32_t*)&_estack,
    (uint32_t*)&Reset_Handler,       // Reset                -15
    (uint32_t*)&Default_Handler,     // NMI_Handler          -14
    (uint32_t*)&HardFault_Handler,   // HardFault_Handler    -13
    (uint32_t*)&Default_Handler,     // MemManage_Handler    -12
    (uint32_t*)&Default_Handler,     // BusFault_Handler     -11
    (uint32_t*)&Default_Handler,     // UsageFault_Handler   -10
    0,                                // reserved             -9
    0,                                // reserved             -8
    0,                                // reserved             -7
    0,                                // reserved             -6
    (uint32_t*)&vPortSVCHandler,     // SVC_Handler          -5
    (uint32_t*)&Default_Handler,     // DebugMon_Handler     -4
    0,                                // reserved             -3
    (uint32_t*)&xPortPendSVHandler,  // PendSV_Handler       -2
    (uint32_t*)&xPortSysTickHandler, // SysTick_Handler      -1
    0, 0, 0, 0, 0, 0, 0, 0,          // external interrupts 0-7 (unused so far)
    (uint32_t*)&Default_Handler,     // Timer 0 -- wired once node/drivers/ needs it
    (uint32_t*)&Default_Handler,     // Timer 1
    0, 0, 0,
    0,                                // Ethernet
};

void Reset_Handler(void) {
    (void)main();
}

// Registers captured from the exception stack frame at the moment a
// HardFault fired -- volatile so the compiler can't optimize away variables
// that are never read by the program itself, only inspected by a human in
// GDB after the fact.
volatile uint32_t r0;
volatile uint32_t r1;
volatile uint32_t r2;
volatile uint32_t r3;
volatile uint32_t r12;
volatile uint32_t lr;
volatile uint32_t pc;
volatile uint32_t psr;

static __attribute__((used)) void prvGetRegistersFromStack(uint32_t* fault_stack_address) {
    r0 = fault_stack_address[0];
    r1 = fault_stack_address[1];
    r2 = fault_stack_address[2];
    r3 = fault_stack_address[3];
    r12 = fault_stack_address[4];
    lr = fault_stack_address[5];
    pc = fault_stack_address[6];
    psr = fault_stack_address[7];

    semihosting::write_line("HardFault -- inspect r0..psr in the debugger\n");

    // Halt here, with the register values sitting in the four statics above:
    // that is the whole point of capturing them.
    for (;;) {
    }
}

void Default_Handler(void) {
    __asm volatile(
        ".align 8                                \n"
        " ldr r3, =0xe000ed04                    \n"  // interrupt control and state register
        " ldr r2, [r3, #0]                       \n"
        " uxtb r2, r2                            \n"  // low byte: number of the currently executing exception
        "Infinite_Loop:                          \n"  // r2 is left set for a debugger to read
        " b  Infinite_Loop                       \n"
        " .ltorg                                 \n");
}

void HardFault_Handler(void) {
    __asm volatile(
        ".align 8                                                   \n"
        " tst lr, #4                                                \n"  // bit 2 of EXC_RETURN: which stack was in use
        " ite eq                                                    \n"
        " mrseq r0, msp                                             \n"
        " mrsne r0, psp                                             \n"
        " ldr r1, [r0, #24]                                         \n"  // saved PC is 6 words into the frame
        " ldr r2, =prvGetRegistersFromStack                         \n"
        " bx r2                                                     \n"
        " .ltorg                                                    \n");
}

}  // extern "C"
