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
    src/ring_buffer.cpp
    src/encoding.cpp
    src/simulator.cpp
    src/ingestion.cpp
)
target_include_directories(mdfh PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(mdfh PUBLIC cxx_std_23)
target_link_libraries(mdfh PUBLIC Boost::boost)

# Add subdirectories
add_subdirectory(src)
enable_testing()
add_subdirectory(tests)

