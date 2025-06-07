#!/bin/bash

# Build script for mdfh market feed simulator with Conan dependencies

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
    echo "Feed simulator executable: ./apps/feed_sim/feed_sim"
    echo ""
    echo "Usage examples:"
    echo "  ./apps/feed_sim/feed_sim --help"
    echo "  ./apps/feed_sim/feed_sim --port 9001 --rate 1000 --batch 10"
    echo "  ./apps/feed_sim/feed_sim -p 9002 -r 5000 -b 50 --jitter 0.1"
    echo ""
    echo "Default configuration:"
    echo "  Port: 9001, Rate: 100000 msgs/sec, Batch: 100 msgs"
else
    echo " Build failed!"
    exit 1
fi 