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

# Create main library
add_library(mdfh STATIC
    src/ring_buffer.cpp
    src/encoding.cpp
    src/simulator.cpp
    src/ingestion.cpp
    src/multi_feed_ingestion.cpp
)
target_include_directories(mdfh PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(mdfh PUBLIC cxx_std_23)
target_link_libraries(mdfh PUBLIC Boost::boost yaml-cpp)

# Add applications
add_executable(multi_feed_benchmark apps/multi_feed_benchmark.cpp)
target_link_libraries(multi_feed_benchmark PRIVATE mdfh CLI11::CLI11)

# Add subdirectories
add_subdirectory(src)
enable_testing()
add_subdirectory(tests)

# Enable advanced performance tests
option(ENABLE_ADVANCED_PERF "Enable advanced performance tests" ON)

