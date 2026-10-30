cmake_minimum_required(VERSION 3.22)
project(bskernel_sdk_example LANGUAGES CXX)

# Configure with the NDK toolchain and
# -Dbskernel_DIR=/path/to/installed/sdk/lib/cmake/bskernel.
find_package(bskernel CONFIG REQUIRED)
add_executable(dfm_example basic.cpp)
target_link_libraries(dfm_example PRIVATE bskernel::sdk)
