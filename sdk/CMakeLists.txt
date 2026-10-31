cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(bskernel VERSION 1.0.6 LANGUAGES CXX)
endif()

if(NOT ANDROID OR NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Use the Android NDK toolchain with -DANDROID_ABI=arm64-v8a")
endif()
if(ANDROID_NATIVE_API_LEVEL LESS 26)
    message(FATAL_ERROR "Use -DANDROID_PLATFORM=android-26 or newer")
endif()

option(BSKERNEL_BUILD_TOOLS "Build the bs_loader command-line tool" ON)
option(BSKERNEL_BUILD_EXAMPLE "Build the SDK calling example" ON)

# The kernel ABI is the only source of truth, including in installed SDKs.
configure_file(../abi.h include/bs/abi.h COPYONLY)

add_library(bskernel_sdk STATIC src/driver.cpp src/loader.cpp)
add_library(bskernel::sdk ALIAS bskernel_sdk)
set_target_properties(bskernel_sdk PROPERTIES
    EXPORT_NAME sdk
    POSITION_INDEPENDENT_CODE ON
)
target_compile_features(bskernel_sdk PUBLIC cxx_std_20)
target_compile_options(bskernel_sdk PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(bskernel_sdk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(bskernel_sdk PUBLIC log)
# Propagate ELF alignment to tools and external consumers for 16 KiB devices.
target_link_options(bskernel_sdk INTERFACE "LINKER:-z,max-page-size=16384")

if(BSKERNEL_BUILD_TOOLS)
    add_executable(bs_loader tools/bs_loader.cpp)
    target_link_libraries(bs_loader PRIVATE bskernel::sdk)
    target_compile_options(bs_loader PRIVATE -Wall -Wextra -Wpedantic)
    install(TARGETS bs_loader RUNTIME DESTINATION bin)
endif()
if(BSKERNEL_BUILD_EXAMPLE)
    add_executable(bs_example examples/basic.cpp)
    target_link_libraries(bs_example PRIVATE bskernel::sdk)
    target_compile_options(bs_example PRIVATE -Wall -Wextra -Wpedantic)
    install(TARGETS bs_example RUNTIME DESTINATION bin)
endif()

# Self-contained loader: all six modules are embedded (zlib + XOR) at build
# time, loaded through memfd + finit_module so no .ko ever touches the disk.
option(BSKERNEL_BUILD_NATIVE_LOADER "Build the self-contained BSLoADERDriver" OFF)
set(BSKERNEL_EMBEDDED_HEADER "" CACHE STRING "Directory holding bs_native_modules.h")
if(BSKERNEL_BUILD_NATIVE_LOADER)
    add_executable(BSLoaderDriver tools/native_loader.cpp)
    target_compile_features(BSLoaderDriver PRIVATE cxx_std_20)
    target_compile_options(BSLoaderDriver PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(BSLoaderDriver PRIVATE BSKERNEL_EMBEDDED_MODULES=1)
    if(BSKERNEL_EMBEDDED_HEADER)
        target_include_directories(BSLoaderDriver PRIVATE "${BSKERNEL_EMBEDDED_HEADER}")
    endif()
    target_link_libraries(BSLoaderDriver PRIVATE z)
    option(BSKERNEL_USE_MBEDTLS "Link mbedTLS for the HTTPS version check" OFF)
    set(BSKERNEL_MBEDTLS_DIR "" CACHE PATH "mbedTLS source tree")
    if(BSKERNEL_USE_MBEDTLS AND NOT BSKERNEL_MBEDTLS_DIR)
        message(FATAL_ERROR "BSKERNEL_USE_MBEDTLS requires BSKERNEL_MBEDTLS_DIR")
    endif()
    if(BSKERNEL_USE_MBEDTLS)
        set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${BSKERNEL_MBEDTLS_DIR}" third_party/mbedtls)
        target_compile_definitions(BSLoaderDriver PRIVATE BSKERNEL_HTTPS=1)
        target_link_libraries(BSLoaderDriver PRIVATE mbedtls mbedx509 mbedcrypto)
    endif()
    target_link_options(BSLoaderDriver PRIVATE "LINKER:-z,max-page-size=16384")
    install(TARGETS BSLoaderDriver RUNTIME DESTINATION bin)
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/bskernelConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/bskernelConfig.cmake"
    INSTALL_DESTINATION lib/cmake/bskernel
)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/bskernelConfigVersion.cmake"
    VERSION 1.0.6 COMPATIBILITY SameMajorVersion
)
install(TARGETS bskernel_sdk EXPORT bskernelTargets ARCHIVE DESTINATION lib)
install(EXPORT bskernelTargets NAMESPACE bskernel:: DESTINATION lib/cmake/bskernel)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/bskernelConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/bskernelConfigVersion.cmake"
    DESTINATION lib/cmake/bskernel
)
install(DIRECTORY include/bs DESTINATION include)
install(FILES ../abi.h DESTINATION include/bs)
install(FILES examples/basic.cpp examples/CMakeLists.txt DESTINATION examples)
