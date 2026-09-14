# Standalone benchmarking tools -- not GoogleTest binaries, not part of the
# shipped product. bench/run_benchmark.py drives these as subprocesses to
# measure throughput, latency, and server memory under load. Host-only, like
# server/ and client/ -- there is no reason for this to cross-compile.
add_executable(sensorlink-bench-generator load_generator.cpp)

target_link_libraries(sensorlink-bench-generator PRIVATE
    proto
    sensorlink_warnings)

target_compile_features(sensorlink-bench-generator PRIVATE cxx_std_17)

add_executable(sensorlink-bench-subscriber latency_subscriber.cpp)

target_link_libraries(sensorlink-bench-subscriber PRIVATE sensorlink_warnings)

target_compile_features(sensorlink-bench-subscriber PRIVATE cxx_std_17)
