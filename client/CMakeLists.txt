# Pure protocol-parsing logic (no sockets) lives in its own small library so
# it can be exercised directly by GoogleTest, in-memory, the same way
# server/subscriber.cpp's own line-splitting is tested without a real
# network connection.
add_library(client_line_protocol STATIC line_protocol.cpp)

target_include_directories(client_line_protocol PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(client_line_protocol PUBLIC sensorlink_warnings)
target_compile_features(client_line_protocol PUBLIC cxx_std_17)

# The actual running program: connects, subscribes, and prints the feed.
# Host-only, like server/ -- there is no reason for this to cross-compile
# for the firmware.
add_executable(sensorlink-client main.cpp)
target_link_libraries(sensorlink-client PRIVATE client_line_protocol sensorlink_warnings)
