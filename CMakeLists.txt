cmake_minimum_required(VERSION 3.25)
project(mdfh LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Performance optimization flags for release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native -flto")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -flto")
endif()

# Include Conan-generated files
include(${CMAKE_BINARY_DIR}/conan_toolchain.cmake OPTIONAL)
find_package(Boost REQUIRED)
find_package(CLI11 REQUIRED)
find_package(yaml-cpp REQUIRED)

# Check for Linux perf support
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CheckIncludeFile)
    check_include_file("linux/perf_event.h" HAVE_PERF_EVENT_H)
    if(HAVE_PERF_EVENT_H)
        add_definitions(-DMDFH_HAVE_PERF_EVENT_H)
    endif()
endif()

# Create main library
add_library(mdfh STATIC
    src/ring_buffer.cpp
    src/encoding.cpp
    src/simulator.cpp
    src/ingestion.cpp
    src/multi_feed_ingestion.cpp
    src/kernel_bypass.cpp
    src/performance_tracker.cpp
)
target_include_directories(mdfh PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(mdfh PUBLIC cxx_std_23)
target_link_libraries(mdfh PUBLIC Boost::boost yaml-cpp)

# Add applications
add_executable(multi_feed_benchmark apps/multi_feed_benchmark.cpp)
target_link_libraries(multi_feed_benchmark PRIVATE mdfh CLI11::CLI11)

add_executable(bypass_ingestion_benchmark apps/bypass_ingestion_benchmark.cpp)
target_link_libraries(bypass_ingestion_benchmark PRIVATE mdfh CLI11::CLI11)



add_executable(simple_bypass_test apps/simple_bypass_test.cpp)
target_link_libraries(simple_bypass_test PRIVATE mdfh)

add_executable(market_data_server apps/market_data_server.cpp)
target_link_libraries(market_data_server PRIVATE mdfh CLI11::CLI11)

# Add subdirectories
add_subdirectory(src)
enable_testing()
add_subdirectory(tests)

# Enable advanced performance tests
option(ENABLE_ADVANCED_PERF "Enable advanced performance tests" ON)

# Kernel bypass networking options
option(ENABLE_DPDK "Enable DPDK kernel bypass networking" OFF)
option(ENABLE_SOLARFLARE "Enable Solarflare ef_vi kernel bypass networking" OFF)

# Configure kernel bypass features
if(ENABLE_DPDK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DPDK REQUIRED libdpdk)
    target_compile_definitions(mdfh PUBLIC MDFH_ENABLE_DPDK)
    target_link_libraries(mdfh PUBLIC ${DPDK_LIBRARIES})
    target_include_directories(mdfh PUBLIC ${DPDK_INCLUDE_DIRS})
    message(STATUS "DPDK kernel bypass enabled")
endif()

if(ENABLE_SOLARFLARE)
    find_library(SOLARFLARE_LIBS ef_vi REQUIRED)
    target_compile_definitions(mdfh PUBLIC MDFH_ENABLE_SOLARFLARE)
    target_link_libraries(mdfh PUBLIC ${SOLARFLARE_LIBS})
    message(STATUS "Solarflare ef_vi kernel bypass enabled")
endif()

