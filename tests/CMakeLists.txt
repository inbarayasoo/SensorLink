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

# Register every TEST() with CTest so `ctest` runs them individually.
include(GoogleTest)
gtest_discover_tests(proto_tests)
