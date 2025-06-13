# MDFH Build Instructions

## Quick Start

The easiest way to build the optimized MDFH library is to use the provided build script:

```bash
# From anywhere in the project:
./scripts/build.sh

# Or from the scripts directory:
cd scripts && ./build.sh
```

The build script automatically:
- Detects and changes to the project root directory
- Sets up Conan dependencies
- Configures CMake with optimizations
- Builds the library and all tests

## Manual Build Process

If you prefer to build manually:

```bash
# 1. Install dependencies with Conan
cd build
conan install .. --output-folder=. --build=missing -s build_type=Release

# 2. Configure with CMake (with optimizations)
cmake .. -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release

# 3. Build
make -j$(nproc)
```

## Performance Testing

After building, run the performance tests to verify optimizations:

```bash
cd build

# Ring buffer performance tests
./tests/test_ring_buffer --gtest_filter="*PerfTest*"

# Encoding performance tests (including optimized FIX encoder)
./tests/test_encoding --gtest_filter="*PerfTest*"

# All performance tests
./tests/perf_tests --gtest_filter="*PerfTest*"
```

## Build Outputs

After a successful build, you'll have:

- **Core library**: `build/libmdfh.a`
- **Applications**: `build/multi_feed_benchmark`
- **Test executables**: `build/tests/test_*`
- **Performance tests**: `build/tests/perf_tests`

## Build Optimizations Enabled

The Release build includes:
- `-O3` - Maximum optimization
- `-march=native` - CPU-specific optimizations
- `-flto` - Link-time optimization
- Cache-line aligned data structures
- Fast FIX encoder implementation

## Requirements

- **CMake** 3.25+
- **Conan** 2.x
- **C++23** compatible compiler
- **Dependencies**: Boost, GTest, Catch2, CLI11, yaml-cpp (auto-installed by Conan)

## Troubleshooting

### "CMake not found"
Install CMake:
- **macOS**: `brew install cmake` or install Xcode Command Line Tools
- **Linux**: `sudo apt install cmake` (Ubuntu/Debian)

### "Conan not found"
Install Conan:
```bash
pip install conan
```

### Build fails with optimization errors
Try building without optimizations first:
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

## Performance Verification

Expected performance after optimizations:
- **Ring Buffer Producer-Consumer**: ~17.9M msgs/sec
- **FIX Encoder**: ~15.7M msgs/sec (63 ns/msg)
- **Single-threaded Ring Buffer**: ~87.7M msgs/sec

See `PERFORMANCE_IMPROVEMENTS.md` for detailed performance analysis. 