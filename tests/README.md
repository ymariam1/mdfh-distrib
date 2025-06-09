# MDFH Testing Framework

This directory contains comprehensive unit tests, performance benchmarks, and integration tests for the Market Data Feed Handler (MDFH) project.

## Overview

The testing framework is designed to validate:

1. **Correctness**: Unit tests ensure all components work as expected
2. **Performance**: Benchmarks verify latency and throughput targets
3. **Memory Safety**: Cache-line alignment and zero-copy operations
4. **Concurrency**: Multi-threaded producer-consumer scenarios
5. **Back-pressure**: Graceful handling of buffer overflow conditions

## Test Structure

### Core Test Files

- **`test_ring_buffer.cpp`**: Basic ring buffer functionality and performance
- **`test_ring_buffer_advanced.cpp`**: Advanced features (alignment, prefetch, back-pressure)
- **`test_encoding.cpp`**: Message encoding/decoding with zero-copy parsing

### Test Frameworks

- **GoogleTest**: Primary testing framework for unit tests and benchmarks
- **Catch2**: Additional testing for edge cases and specific scenarios

## Performance Targets

The tests validate the following performance requirements:

| Component | Metric | Target | Test |
|-----------|--------|--------|------|
| Ring Buffer | Throughput | > 1M msgs/sec | `RingBufferPerfTest.ThroughputBenchmark` |
| Ring Buffer | P95 Latency | < 5µs | `RingBufferAdvancedTest.LatencyMeasurement` |
| Ring Buffer | P50 Latency | < 1µs | `RingBufferAdvancedTest.LatencyMeasurement` |
| Binary Encoding | Throughput | > 1M msgs/sec | `EncodingPerfTest.BinaryEncodingPerformance` |
| FIX Encoding | Throughput | > 100K msgs/sec | `EncodingPerfTest.FIXEncodingPerformance` |
| ITCH Encoding | Throughput | > 500K msgs/sec | `EncodingPerfTest.ITCHEncodingPerformance` |
| Memory Layout | Alignment | 64-byte cache lines | `RingBufferAdvancedTest.CacheLineAlignment` |

## Quick Start

### Prerequisites

```bash
# Install dependencies
conan install . --build=missing

# Configure build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake

# Build tests
cmake --build build -j$(nproc)
```

### Running Tests

#### All Tests (Recommended)
```bash
./run_tests.sh
```

#### Individual Test Suites
```bash
# Ring buffer tests
./build/tests/test_ring_buffer

# Advanced ring buffer tests
./build/tests/test_ring_buffer_advanced

# Encoding tests
./build/tests/test_encoding
```

#### Performance Tests Only
```bash
# Ring buffer performance
./build/tests/test_ring_buffer --gtest_filter="*PerfTest*"

# Advanced features performance
./build/tests/test_ring_buffer_advanced --gtest_filter="*PerfTest*"

# Encoding performance
./build/tests/test_encoding --gtest_filter="*PerfTest*"
```

#### Specific Test Cases
```bash
# Cache-line alignment verification
./build/tests/test_ring_buffer_advanced --gtest_filter="*CacheLineAlignment*"

# Back-pressure handling
./build/tests/test_ring_buffer_advanced --gtest_filter="*BackPressure*"

# Zero-copy parsing
./build/tests/test_encoding --gtest_filter="*ZeroCopy*"
```

## Test Categories

### Unit Tests

**Ring Buffer Core Functionality**
- Constructor validation (power-of-2 capacity)
- Basic push/pop operations
- Buffer wrap-around behavior
- High water mark tracking
- Thread safety verification

**Encoding/Decoding**
- Binary format correctness
- FIX protocol compliance
- ITCH protocol compliance
- Zero-copy parsing views
- Factory function validation

### Performance Tests

**Throughput Benchmarks**
- Single-threaded ring buffer operations
- Multi-threaded producer-consumer scenarios
- Encoding performance across formats
- Zero-copy parsing performance

**Latency Measurements**
- Per-operation latency distribution
- P50, P95, P99 percentile tracking
- Cache-friendly access patterns
- Prefetch effectiveness

### Advanced Features

**Cache-Line Optimization**
- 64-byte alignment verification
- False sharing prevention
- Memory layout validation

**Prefetch Operations**
- Prefetch-enabled push/pop
- Performance comparison vs regular operations
- Memory access pattern optimization

**Back-Pressure Handling**
- Drop mode (fail-fast)
- Block mode with timeout
- Producer-consumer coordination

## Zero-Copy Parsing

The framework includes zero-copy parser views for efficient message processing:

### Binary Messages
```cpp
BinaryMessageView view(encoded_data);
double price = view.get_price();
std::int32_t qty = view.get_quantity();
std::uint64_t seq = view.get_sequence();
```

### FIX Messages
```cpp
FIXMessageView view(encoded_data);
double price = view.get_price();        // Tag 44
std::int32_t qty = view.get_quantity(); // Tag 38
std::uint64_t seq = view.get_sequence(); // Tag 34
```

### ITCH Messages
```cpp
ITCHMessageView view(encoded_data);
if (view.is_valid()) {
    double price = view.get_price();
    std::int32_t qty = view.get_quantity();
    std::uint64_t seq = view.get_sequence();
    std::uint64_t timestamp = view.get_timestamp();
}
```

## Continuous Integration

### Test Automation

The `run_tests.sh` script provides comprehensive test automation:

1. **Environment Setup**: Clean build, dependency installation
2. **Build Verification**: Compile all targets with optimizations
3. **Unit Testing**: Run all correctness tests
4. **Performance Benchmarking**: Multiple iterations with statistical analysis
5. **Report Generation**: XML output for CI integration

### CI/CD Integration

For integration with CI systems:

```yaml
# Example GitHub Actions workflow
- name: Run MDFH Tests
  run: |
    ./run_tests.sh
    
- name: Upload Test Results
  uses: actions/upload-artifact@v3
  with:
    name: test-results
    path: build/*.xml
    
- name: Upload Performance Results
  uses: actions/upload-artifact@v3
  with:
    name: performance-results
    path: build/performance_results.txt
```

## Debugging and Profiling

### Memory Analysis
```bash
# Valgrind memory check
valgrind --tool=memcheck ./build/tests/test_ring_buffer

# AddressSanitizer build
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address"
```

### Performance Profiling
```bash
# Linux perf profiling
perf record -g ./build/tests/test_ring_buffer --gtest_filter="*PerfTest*"
perf report

# CPU cache analysis
perf stat -e cache-references,cache-misses ./build/tests/test_ring_buffer_advanced
```

### Custom Benchmarking
```bash
# Run specific performance test with custom parameters
./build/tests/perf_tests --gtest_filter="*ThroughputBenchmark*"
```

## Extending Tests

### Adding New Test Cases

1. **Unit Tests**: Add to existing test files or create new ones
2. **Performance Tests**: Use `*PerfTest*` naming convention
3. **Integration Tests**: Add to CTest configuration

### Performance Test Guidelines

- Use `high_resolution_clock` for timing
- Run multiple iterations for statistical significance
- Include warm-up phases for cache effects
- Validate against established baselines
- Document expected performance characteristics

### Memory Layout Tests

- Verify alignment requirements
- Test cache-line boundaries
- Validate padding effectiveness
- Check for false sharing

## Troubleshooting

### Common Issues

**Build Failures**
- Ensure Conan dependencies are installed
- Check C++23 compiler support
- Verify CMake version >= 3.25

**Test Failures**
- Performance tests may fail on slower systems
- Adjust timeout values for CI environments
- Check system load during performance tests

**Memory Issues**
- Large buffer allocations may fail on constrained systems
- Reduce test parameters for memory-limited environments
- Use appropriate alignment for target architecture

### Performance Tuning

**System Configuration**
- Disable CPU frequency scaling
- Set process affinity for consistent results
- Minimize background processes during benchmarking

**Compiler Optimizations**
- Use Release build for performance tests
- Enable LTO (Link Time Optimization)
- Consider profile-guided optimization (PGO)

## Contributing

When adding new tests:

1. Follow existing naming conventions
2. Include both correctness and performance validation
3. Document expected behavior and performance characteristics
4. Update this README with new test descriptions
5. Ensure tests pass in CI environment

## References

- [GoogleTest Documentation](https://google.github.io/googletest/)
- [Catch2 Documentation](https://github.com/catchorg/Catch2)
- [CMake Testing](https://cmake.org/cmake/help/latest/manual/ctest.1.html)
- [Performance Testing Best Practices](https://github.com/google/benchmark) 