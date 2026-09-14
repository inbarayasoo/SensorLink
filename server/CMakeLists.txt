# The epoll-based server: connection multiplexing, session handling, and the
# in-memory device store. Linux-only (epoll), so this is never added to the
# firmware's cross-compiling configure -- see the top-level CMakeLists.txt.
add_library(server STATIC
    event_loop.cpp
    connection.cpp
    device_session.cpp
    ingest.cpp
    subscriber.cpp
    store.cpp)

# Consumers include headers as  #include "server/event_loop.hpp"  , so the
# include root is the repository root, matching how proto/ is set up.
target_include_directories(server PUBLIC ${PROJECT_SOURCE_DIR})

target_link_libraries(server PUBLIC
    proto
    sensorlink_warnings)

target_compile_features(server PUBLIC cxx_std_17)

# The actual running program: an accept loop that turns the policy/mechanism
# classes above into live sockets. Kept separate from the server library so
# server_tests can link the library alone, without pulling in main().
add_executable(sensorlink-server main.cpp)
target_link_libraries(sensorlink-server PRIVATE server sensorlink_warnings)
