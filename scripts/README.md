# MDFH Scripts

This directory contains build and test automation scripts for the MDFH project.

## Scripts

- **`build.sh`** - Main build script that sets up Conan dependencies and builds the project
- **`run_tests.sh`** - Comprehensive test runner that builds and runs all test suites

## Usage

From the project root directory:

```bash
# Build the project
./build.sh

# Run all tests
./run_tests.sh
```

Or run the scripts directly:

```bash
# Build the project
./scripts/build.sh

# Run all tests  
./scripts/run_tests.sh
```

## Output Locations

- **Build artifacts**: `build/` directory
- **Test results**: `test_results/` directory
  - XML test reports
  - Performance benchmark results
  - Test logs

## Requirements

- CMake 3.15+
- Conan 2.0+
- C++23 compatible compiler
- GTest (installed via Conan) 