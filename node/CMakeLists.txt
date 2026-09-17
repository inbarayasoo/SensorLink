# The firmware image: FreeRTOS running our application code on the emulated
# Cortex-M3. Only configured when CMake is invoked with the ARM toolchain
# file (see cmake/arm-none-eabi.cmake and the "firmware" preset) -- this
# directory is never part of a normal host build.

set(FREERTOS_KERNEL_DIR ${CMAKE_SOURCE_DIR}/third_party/FreeRTOS-Kernel)
set(FREERTOS_PORT_DIR ${FREERTOS_KERNEL_DIR}/portable/GCC/ARM_CM3)

add_executable(sensorlink-node
    main.cpp
    hooks.cpp
    startup_gcc.cpp
    tasks/sensor_task.cpp
    tasks/telemetry_task.cpp
    tasks/command_task.cpp
    session.cpp

    # The kernel proper: task/queue/mutex bookkeeping (tasks.c, queue.c),
    # its internal linked-list type (list.c), and the Cortex-M3-specific
    # context-switch / tick / SVC assembly glue (port.c). Note what is
    # deliberately absent: no heap_*.c (configSUPPORT_DYNAMIC_ALLOCATION is
    # 0 -- see node/FreeRTOSConfig.h), no timers.c / event_groups.c /
    # stream_buffer.c (this project uses neither software timers, event
    # groups, nor stream buffers).
    ${FREERTOS_KERNEL_DIR}/tasks.c
    ${FREERTOS_KERNEL_DIR}/list.c
    ${FREERTOS_KERNEL_DIR}/queue.c
    ${FREERTOS_PORT_DIR}/port.c
)

target_include_directories(sensorlink-node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}       # FreeRTOSConfig.h
    ${CMAKE_CURRENT_SOURCE_DIR}/cmsis  # CMSDK_CM3.h -- the UART/timer register layout
    ${FREERTOS_KERNEL_DIR}/include
    ${FREERTOS_PORT_DIR}
)

# The shared wire-protocol library -- the same target the host build links
# (see proto/CMakeLists.txt). This is the one dependency that makes this
# firmware and the eventual server two halves of one contract instead of two
# unrelated programs that happen to agree on a byte layout.
target_link_libraries(sensorlink-node PRIVATE proto)

set(SENSORLINK_NODE_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker/mps2_an385.ld)

target_link_options(sensorlink-node PRIVATE
    -T${SENSORLINK_NODE_LINKER_SCRIPT}
    -Wl,-Map=$<TARGET_FILE_DIR:sensorlink-node>/sensorlink-node.map
)

# Relink (not just reconfigure) if the memory layout changes -- CMake does
# not treat a linker script as a tracked input by default.
set_target_properties(sensorlink-node PROPERTIES
    LINK_DEPENDS ${SENSORLINK_NODE_LINKER_SCRIPT}
    SUFFIX ".elf"
)

# arm-none-eabi-size's flash/RAM breakdown, printed after every build --
# this is the number the roadmap asks us to record for this stage.
add_custom_command(TARGET sensorlink-node POST_BUILD
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:sensorlink-node>
)
