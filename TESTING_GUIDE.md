# Redis io_uring Testing Guide

## Team Member D - Comprehensive Testing Framework

This guide provides complete instructions for using the Redis io_uring testing infrastructure created by Team Member D during Week 1.

## Overview

The testing framework consists of several components designed to validate every aspect of the Redis io_uring implementation:

1. **Environment Validation** - Verify development environment setup
2. **Unit Testing Framework** - Test individual components
3. **Build System Validation** - Ensure proper compilation
4. **Performance Benchmarking** - Compare io_uring vs epoll performance
5. **Integration Testing** - Validate complete Redis functionality
6. **Continuous Integration** - Automated testing pipeline

## Quick Start

### 1. Validate Your Environment

```bash
# Check if your system is ready for Redis io_uring development
./scripts/validate_uring_environment.sh
```

This will check:
- Kernel version (requires Linux 5.1+)
- liburing installation and version
- Development tools (GCC, Make, pkg-config)
- System resources (memory, file descriptors)

### 2. Run All Validations

```bash
# Run complete validation suite
./scripts/run_all_validations.sh
```

This comprehensive test will:
- Validate environment
- Test build system
- Run unit tests
- Execute integration tests
- Provide summary report

### 3. Run Unit Tests

```bash
cd tests/unit
make test                # Run all unit tests
make test-valgrind      # Run with memory checking
make test-no-uring      # Run without io_uring support
```

### 4. Performance Benchmarking

```bash
cd tests/performance
make benchmark          # Run comprehensive performance tests
make compare           # Compare epoll vs io_uring
make quick-test        # Quick performance validation
```

## Detailed Testing Procedures

### Environment Validation

The environment validation script checks all prerequisites for Redis io_uring development:

```bash
./scripts/validate_uring_environment.sh
```

**What it checks:**
- Kernel version and io_uring support
- liburing library and headers
- Compiler and build tools
- System resources
- Basic io_uring functionality
- SQPOLL support

**Expected output:**
- ✅ Green PASS messages for working components
- ❌ Red FAIL messages for missing components
- ⚠️ Yellow WARN messages for optional features

**Common issues:**
- Missing liburing: Install with `sudo apt-get install liburing-dev`
- Old kernel: Upgrade to Linux 5.1+ for basic support, 5.19+ for advanced features
- Missing build tools: Install with `sudo apt-get install build-essential`

### Unit Testing

The unit testing framework provides comprehensive testing for io_uring components:

#### Running Tests

```bash
cd tests/unit

# Basic test execution
make test

# Test with memory checking
make test-valgrind

# Test without io_uring (fallback testing)
make test-no-uring

# Generate coverage report
make coverage
```

#### Test Framework Features

- **Assertion Macros**: `TEST_ASSERT`, `TEST_ASSERT_EQ`, `TEST_ASSERT_STR_EQ`
- **Memory Tracking**: Automatic leak detection
- **File Management**: Temporary file cleanup
- **Network Testing**: Socket pair creation and testing
- **Time Utilities**: Precise timing measurements
- **Conditional Testing**: Works with/without io_uring

#### Adding New Tests

Create a new test file following this pattern:

```c
#include "test_uring_framework.h"

int test_my_feature(void) {
    TEST_ASSERT(1 == 1, "Basic test");
    TEST_ASSERT_EQ(42, 42, "Equality test");
    return 0;
}

test_case_t my_tests[] = {
    REGISTER_TEST("my_feature", test_my_feature),
};

REGISTER_TEST_SUITE("My Test Suite", my_tests);
```

### Build System Validation

Validates that Redis builds correctly with and without io_uring:

```bash
./scripts/validate_build_system.sh
```

**What it validates:**
- Clean build without io_uring
- Build with USE_URING=yes flag
- Makefile detection logic
- Configuration file handling
- Header file structure
- Dependency management

### Performance Benchmarking

Comprehensive performance testing framework for comparing implementations:

#### Basic Benchmarking

```bash
cd tests/performance

# Build the benchmark tools
make all

# Run basic performance test
make test

# Run comprehensive benchmark
make benchmark

# Compare epoll vs io_uring
make compare
```

#### Advanced Benchmarking

```bash
# Custom benchmark with specific parameters
./run_benchmarks -c 100 -n 50000 -t set,get,incr -o results.csv

# Stress testing
make stress-test

# Generate performance graphs
make graphs

# Generate benchmark report
make report
```

#### Benchmark Options

- `-c, --clients NUM`: Number of concurrent clients
- `-n, --operations NUM`: Operations per client
- `-t, --tests TYPES`: Test types (set,get,incr,lpush,etc.)
- `-o, --output FILE`: Output CSV file
- `--epoll-only`: Test only epoll implementation
- `--uring-only`: Test only io_uring implementation

### Integration Testing

Integration tests validate complete Redis functionality:

```bash
# Integration tests are included in the complete validation
./scripts/run_all_validations.sh

# Or run manually
cd tests/integration
# (Integration test scripts will be added as implementation progresses)
```

### Continuous Integration

The CI pipeline automatically runs all tests on multiple environments:

#### GitHub Actions Workflow

The `.github/workflows/uring-ci.yml` file defines:

- **Environment Validation**: Tests on Ubuntu 20.04, 22.04, 24.04
- **Build Testing**: With and without io_uring
- **Unit Tests**: Complete test suite execution
- **Integration Tests**: Redis functionality validation
- **Performance Tests**: Benchmark comparisons
- **Artifact Collection**: Results and logs

#### Local CI Simulation

```bash
# Simulate CI environment locally
./scripts/run_all_validations.sh

# Test specific configurations
make USE_URING=yes  # Test io_uring build
make               # Test fallback build
```

## Test Results Interpretation

### Environment Validation Results

- **All PASS**: Environment ready for development
- **Some FAIL**: Missing dependencies (follow installation instructions)
- **Warnings**: Optional features not available (may be acceptable)

### Unit Test Results

- **All tests PASSED**: Framework working correctly
- **Some tests FAILED**: Implementation issues need attention
- **Memory leaks detected**: Use valgrind output to identify issues

### Build Validation Results

- **Build successful**: Redis compiles correctly
- **Build failed**: Check compiler errors and dependencies
- **Warnings**: May indicate missing features (expected during development)

### Performance Results

- **Higher ops/sec**: Better performance
- **Lower latency**: Better responsiveness
- **Compare epoll vs io_uring**: Measure improvement

## Troubleshooting

### Common Issues

1. **liburing not found**
   ```bash
   sudo apt-get install liburing-dev
   ```

2. **Kernel too old**
   - Upgrade to Linux 5.1+ for basic io_uring
   - Upgrade to Linux 5.19+ for advanced features

3. **Build failures**
   ```bash
   sudo apt-get install build-essential pkg-config
   ```

4. **Permission errors**
   ```bash
   chmod +x scripts/*.sh
   ```

5. **Port conflicts in tests**
   - Tests use ports 6379, 6380, etc.
   - Stop any running Redis instances

### Getting Help

1. **Check logs**: Test scripts provide detailed output
2. **Run with verbose**: Use `-x` flag for shell scripts
3. **Validate step by step**: Run individual validation scripts
4. **Check dependencies**: Ensure all required packages installed

## Development Workflow

### For Team Members A, B, C

1. **Before starting**: Run environment validation
2. **During development**: Use unit tests to validate components
3. **After changes**: Run build validation
4. **Before commits**: Run complete validation suite

### For Team Member D

1. **Monitor progress**: Regularly run validations on team implementations
2. **Extend tests**: Add new tests as features are implemented
3. **Performance tracking**: Benchmark improvements
4. **CI maintenance**: Update pipeline as needed

## Future Enhancements

The testing framework is designed to be extensible:

- **Cluster Testing**: Add Redis cluster validation
- **Replication Testing**: Test master-replica scenarios
- **Load Testing**: High-concurrency scenarios
- **Failure Testing**: Network partition and recovery
- **Security Testing**: Validate security features

## Summary

This comprehensive testing framework ensures:

✅ **Quality Assurance**: Thorough validation of all components  
✅ **Performance Monitoring**: Continuous performance tracking  
✅ **Regression Prevention**: Automated testing prevents regressions  
✅ **Development Support**: Tools to assist all team members  
✅ **Documentation**: Clear guides and procedures  

The framework is ready to support the entire Redis io_uring development effort and will evolve as the implementation progresses.
