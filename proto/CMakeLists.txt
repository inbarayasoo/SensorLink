# The wire protocol library.
#
# Header-only for now, so it is an INTERFACE library: it builds no object files
# of its own, it only propagates an include path to whoever links it. Both the
# firmware and the server will link this same target, which is what keeps the
# two ends of the wire format in sync.
add_library(proto INTERFACE)

# Consumers include headers as  #include "proto/crc16.hpp"  , so the include
# root is the repository root, not this directory.
target_include_directories(proto INTERFACE ${PROJECT_SOURCE_DIR})

target_compile_features(proto INTERFACE cxx_std_17)
