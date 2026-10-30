cmake_minimum_required(VERSION 3.22)
project(bskernel VERSION 0.1.0 LANGUAGES CXX)

# Kernel modules use the root Makefile; CMake builds the Android userspace SDK.
add_subdirectory(sdk)
