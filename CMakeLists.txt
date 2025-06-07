cmake_minimum_required(VERSION 3.25)
project(mdfh LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include Conan-generated files
include(${CMAKE_BINARY_DIR}/conan_toolchain.cmake OPTIONAL)
find_package(Boost REQUIRED)
find_package(CLI11 REQUIRED)

# Create main library
add_library(mdfh STATIC
    ring_buffer.cpp
    parser.cpp
)
target_include_directories(mdfh PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(mdfh PUBLIC cxx_std_23)

# Add subdirectories
add_subdirectory(src)
add_subdirectory(apps/feed_sim)
add_subdirectory(apps/bench_ingest)
enable_testing()
add_subdirectory(tests)

