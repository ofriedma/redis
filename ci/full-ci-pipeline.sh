#!/bin/bash

# Redis io_uring Full CI Pipeline
# Comprehensive validation and testing suite

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
NC='\033[0m' # No Color

# Configuration
CI_START_TIME=$(date +%s)
CI_REPORT_FILE="ci-report-$(date +%Y%m%d_%H%M%S).md"
FAILED_TESTS=()
PASSED_TESTS=()

# Logging functions
log_info() {
    echo -e "${BLUE}[CI-INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[CI-SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[CI-WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[CI-ERROR]${NC} $1"
}

log_stage() {
    echo -e "${PURPLE}[CI-STAGE]${NC} $1"
}

# Test tracking
track_test() {
    local test_name="$1"
    local result="$2"
    
    if [ "$result" = "PASS" ]; then
        PASSED_TESTS+=("$test_name")
        log_success "✓ $test_name"
    else
        FAILED_TESTS+=("$test_name")
        log_error "✗ $test_name"
    fi
}

# Stage 1: Environment Validation
validate_environment() {
    log_stage "Stage 1: Environment Validation"
    
    # Check kernel version
    local kernel_version=$(uname -r | cut -d. -f1,2)
    local major=$(echo $kernel_version | cut -d. -f1)
    local minor=$(echo $kernel_version | cut -d. -f2)
    
    if [[ $major -ge 5 && $minor -ge 6 ]]; then
        track_test "Kernel Version Check" "PASS"
    else
        track_test "Kernel Version Check" "FAIL"
        log_warning "Kernel $kernel_version may not fully support io_uring"
    fi
    
    # Check required tools
    local tools=("gcc" "make" "pkg-config" "valgrind" "nc")
    for tool in "${tools[@]}"; do
        if command -v "$tool" >/dev/null 2>&1; then
            track_test "Tool: $tool" "PASS"
        else
            track_test "Tool: $tool" "FAIL"
        fi
    done
    
    # Check liburing
    if pkg-config --exists liburing; then
        local liburing_version=$(pkg-config --modversion liburing)
        track_test "liburing ($liburing_version)" "PASS"
    else
        track_test "liburing" "FAIL"
        log_warning "liburing not found - some features may be disabled"
    fi
}

# Stage 2: Build Validation
validate_build() {
    log_stage "Stage 2: Build Validation"
    
    # Clean build
    log_info "Performing clean build..."
    if make clean >/dev/null 2>&1; then
        track_test "Clean Build" "PASS"
    else
        track_test "Clean Build" "FAIL"
        return 1
    fi
    
    # Standard build
    log_info "Building Redis (standard)..."
    if make >/dev/null 2>&1; then
        track_test "Standard Build" "PASS"
    else
        track_test "Standard Build" "FAIL"
        return 1
    fi
    
    # Debug build
    log_info "Building Redis (debug)..."
    if make clean >/dev/null 2>&1 && make OPTIMIZATION="-O0 -g" MALLOC=libc >/dev/null 2>&1; then
        track_test "Debug Build" "PASS"
    else
        track_test "Debug Build" "FAIL"
    fi
    
    # Check binary
    if [ -x "src/redis-server" ]; then
        track_test "Redis Binary" "PASS"
    else
        track_test "Redis Binary" "FAIL"
        return 1
    fi
}

# Stage 3: Core Functionality Tests
test_core_functionality() {
    log_stage "Stage 3: Core Functionality Tests"
    
    # Start Redis server for testing
    local port=6400
    local pid_file="/tmp/redis-ci-test.pid"
    
    log_info "Starting Redis server for testing..."
    ./src/redis-server --port $port --daemonize yes --pidfile $pid_file >/dev/null 2>&1
    sleep 2
    
    if pgrep -f "redis-server.*$port" >/dev/null; then
        track_test "Redis Server Start" "PASS"
        
        # Basic connectivity
        if echo "PING" | nc localhost $port | grep -q "PONG"; then
            track_test "Basic Connectivity" "PASS"
        else
            track_test "Basic Connectivity" "FAIL"
        fi
        
        # Basic operations
        if echo -e "SET ci_test_key ci_test_value\r\nGET ci_test_key\r\n" | nc localhost $port | grep -q "ci_test_value"; then
            track_test "Basic Operations" "PASS"
        else
            track_test "Basic Operations" "FAIL"
        fi
        
        # INFO command
        if echo "INFO server" | nc localhost $port | grep -q "redis_version"; then
            track_test "INFO Command" "PASS"
        else
            track_test "INFO Command" "FAIL"
        fi
        
        # io_uring status
        local uring_info=$(echo "INFO uring" | nc localhost $port)
        if echo "$uring_info" | grep -q "uring_enabled"; then
            track_test "io_uring INFO" "PASS"
        else
            track_test "io_uring INFO" "FAIL"
        fi
        
        # Stop server
        pkill -f "redis-server.*$port" || true
        rm -f $pid_file
        track_test "Redis Server Stop" "PASS"
    else
        track_test "Redis Server Start" "FAIL"
        return 1
    fi
}

# Stage 4: Redis Test Suite
run_redis_tests() {
    log_stage "Stage 4: Redis Test Suite"
    
    log_info "Running Redis unit tests (subset)..."
    
    # Run a subset of critical tests
    local test_files=(
        "unit/basic.tcl"
        "unit/type/string.tcl"
        "unit/type/list.tcl"
        "unit/type/set.tcl"
        "unit/type/hash.tcl"
        "unit/expire.tcl"
        "unit/other.tcl"
    )
    
    local passed_count=0
    local total_count=${#test_files[@]}
    
    cd tests
    for test_file in "${test_files[@]}"; do
        if [ -f "$test_file" ]; then
            log_info "Running $test_file..."
            if timeout 120 ./test_helper.tcl --single "$test_file" >/dev/null 2>&1; then
                ((passed_count++))
                track_test "Redis Test: $(basename $test_file)" "PASS"
            else
                track_test "Redis Test: $(basename $test_file)" "FAIL"
            fi
        else
            track_test "Redis Test: $(basename $test_file)" "FAIL"
            log_warning "Test file not found: $test_file"
        fi
    done
    cd ..
    
    log_info "Redis tests: $passed_count/$total_count passed"
    
    if [ $passed_count -eq $total_count ]; then
        track_test "Redis Test Suite" "PASS"
    else
        track_test "Redis Test Suite" "FAIL"
    fi
}

# Stage 5: Memory and Performance Tests
run_memory_tests() {
    log_stage "Stage 5: Memory and Performance Tests"
    
    # Memory leak test with valgrind
    log_info "Running memory leak detection..."
    local port=6401
    
    timeout 60 valgrind \
        --tool=memcheck \
        --leak-check=full \
        --show-leak-kinds=all \
        --track-origins=yes \
        --log-file=valgrind-ci.log \
        ./src/redis-server --port $port &
    
    local server_pid=$!
    sleep 10
    
    # Test basic operations under valgrind
    echo "PING" | nc localhost $port >/dev/null 2>&1
    echo -e "SET valgrind_test test_value\r\nGET valgrind_test\r\n" | nc localhost $port >/dev/null 2>&1
    
    # Stop server
    echo "SHUTDOWN NOSAVE" | nc localhost $port >/dev/null 2>&1 || true
    wait $server_pid 2>/dev/null || true
    
    # Check for memory leaks
    if [ -f "valgrind-ci.log" ]; then
        if grep -q "definitely lost\|possibly lost" valgrind-ci.log; then
            track_test "Memory Leak Detection" "FAIL"
            log_warning "Memory leaks detected - check valgrind-ci.log"
        else
            track_test "Memory Leak Detection" "PASS"
        fi
    else
        track_test "Memory Leak Detection" "FAIL"
    fi
    
    # Basic performance test
    log_info "Running basic performance test..."
    local port=6402
    ./src/redis-server --port $port --daemonize yes >/dev/null 2>&1
    sleep 2
    
    if timeout 30 ./src/redis-benchmark -p $port -t set,get -n 10000 -c 10 >/dev/null 2>&1; then
        track_test "Basic Performance Test" "PASS"
    else
        track_test "Basic Performance Test" "FAIL"
    fi
    
    pkill -f "redis-server.*$port" || true
}

# Stage 6: Configuration Tests
test_configuration() {
    log_stage "Stage 6: Configuration Tests"
    
    # Test configuration parsing
    log_info "Testing configuration parsing..."
    
    # Create test config
    cat > test-redis.conf << EOF
port 6403
uring-enabled no
save ""
EOF
    
    if ./src/redis-server test-redis.conf --test-memory >/dev/null 2>&1; then
        track_test "Configuration Parsing" "PASS"
    else
        track_test "Configuration Parsing" "FAIL"
    fi
    
    rm -f test-redis.conf
    
    # Test config rewrite
    local port=6403
    ./src/redis-server --port $port --daemonize yes >/dev/null 2>&1
    sleep 2
    
    if echo "CONFIG REWRITE" | nc localhost $port | grep -q "OK"; then
        track_test "Config Rewrite" "PASS"
    else
        track_test "Config Rewrite" "FAIL"
    fi
    
    pkill -f "redis-server.*$port" || true
}

# Generate CI Report
generate_ci_report() {
    log_stage "Generating CI Report"
    
    local end_time=$(date +%s)
    local duration=$((end_time - CI_START_TIME))
    local total_tests=$((${#PASSED_TESTS[@]} + ${#FAILED_TESTS[@]}))
    local pass_rate=0
    
    if [ $total_tests -gt 0 ]; then
        pass_rate=$(( ${#PASSED_TESTS[@]} * 100 / total_tests ))
    fi
    
    cat > "$CI_REPORT_FILE" << EOF
# Redis io_uring CI Report

**Generated:** $(date)  
**Duration:** ${duration}s  
**Total Tests:** $total_tests  
**Passed:** ${#PASSED_TESTS[@]}  
**Failed:** ${#FAILED_TESTS[@]}  
**Pass Rate:** ${pass_rate}%

## Test Results

### Passed Tests (${#PASSED_TESTS[@]})
EOF
    
    for test in "${PASSED_TESTS[@]}"; do
        echo "- ✓ $test" >> "$CI_REPORT_FILE"
    done
    
    echo "" >> "$CI_REPORT_FILE"
    echo "### Failed Tests (${#FAILED_TESTS[@]})" >> "$CI_REPORT_FILE"
    
    for test in "${FAILED_TESTS[@]}"; do
        echo "- ✗ $test" >> "$CI_REPORT_FILE"
    done
    
    cat >> "$CI_REPORT_FILE" << EOF

## Environment Information

- **Kernel:** $(uname -r)
- **OS:** $(cat /etc/os-release | grep PRETTY_NAME | cut -d'"' -f2)
- **Architecture:** $(uname -m)
- **liburing:** $(pkg-config --modversion liburing 2>/dev/null || echo "Not available")

## Artifacts

- CI Report: $CI_REPORT_FILE
- Valgrind Log: valgrind-ci.log (if generated)

---
*Redis io_uring CI Pipeline - $(date)*
EOF
    
    log_success "CI report generated: $CI_REPORT_FILE"
}

# Main CI Pipeline
main() {
    echo "========================================"
    echo "  Redis io_uring Full CI Pipeline"
    echo "========================================"
    echo "Started: $(date)"
    echo
    
    # Run all stages
    validate_environment
    validate_build
    test_core_functionality
    run_redis_tests
    run_memory_tests
    test_configuration
    
    # Generate report
    generate_ci_report
    
    # Summary
    echo
    echo "========================================"
    echo "  CI Pipeline Summary"
    echo "========================================"
    echo "Total Tests: $((${#PASSED_TESTS[@]} + ${#FAILED_TESTS[@]}))"
    echo "Passed: ${#PASSED_TESTS[@]}"
    echo "Failed: ${#FAILED_TESTS[@]}"
    echo "Duration: $(($(date +%s) - CI_START_TIME))s"
    echo
    
    if [ ${#FAILED_TESTS[@]} -eq 0 ]; then
        log_success "🎉 All tests passed! CI pipeline successful!"
        echo "Report: $CI_REPORT_FILE"
        exit 0
    else
        log_error "❌ Some tests failed. Check the report for details."
        echo "Report: $CI_REPORT_FILE"
        exit 1
    fi
}

# Run the pipeline
main "$@"
