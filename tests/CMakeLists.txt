include(FetchContent)

# GoogleTest is pulled in at configure time and built as part of this project.
# Pinned to a release tag so CI is reproducible.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE)

# Do not install GoogleTest, and do not let it clobber our compiler/linker flags.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# --- Test executable -------------------------------------------------------
add_executable(proto_tests
    smoke_test.cpp
    crc16_test.cpp
    cobs_test.cpp
    static_vector_test.cpp
    ring_buffer_test.cpp
    messages_test.cpp
    frame_encoder_test.cpp
    frame_parser_test.cpp)

target_link_libraries(proto_tests PRIVATE
    proto
    sensorlink_warnings
    GTest::gtest_main)

# --- server test executable -------------------------------------------------
# Separate from proto_tests because it links the server library (epoll,
# sockets) instead of the header-only proto library -- there is no reason to
# pull sys/epoll.h into a binary that only exercises framing and CRC.
add_executable(server_tests
    event_loop_test.cpp
    connection_test.cpp
    device_session_test.cpp
    ingest_test.cpp
    subscriber_test.cpp
    store_test.cpp)

target_link_libraries(server_tests PRIVATE
    server
    sensorlink_warnings
    GTest::gtest_main)

# --- client test executable -------------------------------------------------
# Separate from server_tests because it links client_line_protocol, not the
# server library -- the two share nothing but GoogleTest.
add_executable(client_tests
    client_line_protocol_test.cpp)

target_link_libraries(client_tests PRIVATE
    client_line_protocol
    sensorlink_warnings
    GTest::gtest_main)

# Register every TEST() with CTest so `ctest` runs them individually.
include(GoogleTest)
gtest_discover_tests(proto_tests)
gtest_discover_tests(server_tests)
gtest_discover_tests(client_tests)

# tests/integration/ is not GoogleTest -- see run_integration_test.py, which
# drives real server/client/QEMU subprocesses instead of running in-process.
add_subdirectory(integration)
