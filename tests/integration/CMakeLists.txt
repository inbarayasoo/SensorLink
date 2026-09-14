# Not a GoogleTest binary -- a small standalone helper that
# run_integration_test.py drives as a subprocess. Built whenever the rest of
# tests/ is (SENSORLINK_BUILD_TESTS), since it exists purely to support
# testing and ships in no product image.
add_executable(sensorlink-load-generator load_generator.cpp)

target_link_libraries(sensorlink-load-generator PRIVATE
    proto
    sensorlink_warnings)

target_compile_features(sensorlink-load-generator PRIVATE cxx_std_17)
