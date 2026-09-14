# CMake toolchain file for cross-compiling node/ to the ARM Cortex-M3 target
# emulated by QEMU (mps2-an385). Select it at configure time:
#
#   cmake --preset firmware
#
# CMAKE_SYSTEM_NAME "Generic" tells CMake there is no operating system on the
# target, so it will not try to link a host-style executable (no libc startup
# expecting argv/environ, no shared libraries). Everything the firmware needs
# — the vector table, the C runtime init, main() — is supplied by our own
# startup_gcc.c and the linker script.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy CACHE FILEPATH "")
set(CMAKE_SIZE         arm-none-eabi-size    CACHE FILEPATH "")

# A plain "does this compiler work" link test fails here: our target has no
# _start / no OS to satisfy a normal executable link. Ask CMake to only try
# building a static library during compiler detection, not a full link.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Cortex-M3, Thumb-2 instruction set, no FPU on this core.
set(SENSORLINK_TARGET_FLAGS "-mthumb -mcpu=cortex-m3")

set(CMAKE_C_FLAGS_INIT   "${SENSORLINK_TARGET_FLAGS} -ffreestanding -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "${SENSORLINK_TARGET_FLAGS} -ffreestanding -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-threadsafe-statics")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${SENSORLINK_TARGET_FLAGS} -nostartfiles -specs=nosys.specs -Wl,--gc-sections")

# Only look for libraries/headers in the toolchain's own sysroot, never the
# host machine's /usr/include or /usr/lib — those are for x86-64, not ARM.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
