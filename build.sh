#!/bin/bash

# Build script for mdfh market data feed handling library with Conan dependencies

echo "Setting up build environment for mdfh with Conan..."

# Ensure Homebrew path is available
export PATH="/opt/homebrew/bin:$PATH"

# Check if CMake is installed
if ! command -v cmake &> /dev/null; then
    echo "   CMake not found. Please install CMake first."
    echo "   macOS: Install Xcode Command Line Tools or use homebrew: brew install cmake"
    echo "   Linux: sudo apt install cmake (Ubuntu/Debian) or equivalent"
    exit 1
fi

echo "Found CMake: $(cmake --version | head -1)"

# Set up Conan profile (first time only)
if [ ! -f ~/.conan2/profiles/default ]; then
    echo "Setting up Conan default profile..."
    conan profile detect --force
    # Update profile to use C++23
    sed -i '' 's/compiler.cppstd=gnu17/compiler.cppstd=23/' ~/.conan2/profiles/default
else
    echo "Conan profile already exists"
fi

# Create build directory
mkdir -p build
cd build

# Install dependencies with Conan
echo "Installing dependencies with Conan..."
conan install .. --output-folder=. --build=missing -s build_type=Release

# Configure with CMake using Conan toolchain
echo "Configuring with CMake..."
cmake .. -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release

# Build the project
echo "Building..."
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu || echo 4)

# Check if build was successful
if [ $? -eq 0 ]; then
    echo ""
    echo " Build successful!"
    echo ""
    echo "MDFH library built successfully."
    echo "Core library: libmdfh.a"
    echo ""
    echo "Available components:"
    echo "  - Ring buffer (lock-free, single producer/consumer)"
    echo "  - Market data ingestion framework"
    echo "  - Market data simulation framework"
    echo "  - Message encoding/decoding (Binary, FIX, ITCH)"
    echo "  - High-performance timing utilities"
    echo ""
    echo "Build your own applications using the mdfh library."
else
    echo " Build failed!"
    exit 1
fi 