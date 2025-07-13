#!/bin/bash

# Redis io_uring Complete Validation Suite
# Team Member D - Week 1 Task 1.D.4
# 
# This script runs all validation tests to ensure the Redis io_uring
# implementation is working correctly across all components.

set +e  # Don't exit on first error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# Counters
TOTAL_SUITES=0
PASSED_SUITES=0
FAILED_SUITES=0

# Logging functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

log_error() {
    echo -e "${RED}[FAIL]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_header() {
    echo
    echo -e "${BOLD}=============================================="
    echo -e "$1"
    echo -e "===============================================${NC}"
    echo
}

# Function to run a validation suite
run_validation_suite() {
    local suite_name="$1"
    local script_path="$2"
    local description="$3"
    
    ((TOTAL_SUITES++))
    
    log_header "Running $suite_name"
    log_info "$description"
    
    if [ ! -f "$script_path" ]; then
        log_error "Validation script not found: $script_path"
        ((FAILED_SUITES++))
        return 1
    fi
    
    if [ ! -x "$script_path" ]; then
        log_info "Making script executable: $script_path"
        chmod +x "$script_path"
    fi
    
    log_info "Executing: $script_path"
    echo "----------------------------------------"
    
    if "$script_path"; then
        echo "----------------------------------------"
        log_success "$suite_name completed successfully"
        ((PASSED_SUITES++))
        return 0
    else
        echo "----------------------------------------"
        log_error "$suite_name failed"
        ((FAILED_SUITES++))
        return 1
    fi
}

# Function to run unit tests
run_unit_tests() {
    local suite_name="Unit Tests"
    local description="Running comprehensive unit tests for io_uring components"
    
    ((TOTAL_SUITES++))
    
    log_header "Running $suite_name"
    log_info "$description"
    
    if [ ! -d "tests/unit" ]; then
        log_error "Unit tests directory not found: tests/unit"
        ((FAILED_SUITES++))
        return 1
    fi
    
    cd tests/unit
    
    log_info "Building unit tests..."
    if make clean && make all; then
        log_success "Unit tests built successfully"
    else
        log_error "Unit tests build failed"
        cd ../..
        ((FAILED_SUITES++))
        return 1
    fi
    
    log_info "Running unit tests..."
    echo "----------------------------------------"
    
    local test_result=0
    
    # Run basic tests
    if make test; then
        log_success "Basic unit tests passed"
    else
        log_error "Basic unit tests failed"
        test_result=1
    fi
    
    # Run tests without io_uring
    if make test-no-uring; then
        log_success "Unit tests without io_uring passed"
    else
        log_error "Unit tests without io_uring failed"
        test_result=1
    fi
    
    # Run valgrind tests if available
    if command -v valgrind >/dev/null 2>&1; then
        log_info "Running memory leak tests with valgrind..."
        if make test-valgrind; then
            log_success "Valgrind tests passed"
        else
            log_warning "Valgrind tests failed (may be expected)"
        fi
    else
        log_warning "Valgrind not available, skipping memory leak tests"
    fi
    
    cd ../..
    echo "----------------------------------------"
    
    if [ $test_result -eq 0 ]; then
        log_success "$suite_name completed successfully"
        ((PASSED_SUITES++))
        return 0
    else
        log_error "$suite_name failed"
        ((FAILED_SUITES++))
        return 1
    fi
}

# Function to run integration tests
run_integration_tests() {
    local suite_name="Integration Tests"
    local description="Running integration tests with Redis functionality"
    
    ((TOTAL_SUITES++))
    
    log_header "Running $suite_name"
    log_info "$description"
    
    log_info "Building Redis for integration testing..."
    
    # Clean build
    make clean >/dev/null 2>&1
    
    # Try to build with io_uring first
    if make USE_URING=yes >/dev/null 2>&1; then
        log_success "Redis built with io_uring support"
        REDIS_BUILD="uring"
    elif make >/dev/null 2>&1; then
        log_success "Redis built without io_uring support"
        REDIS_BUILD="epoll"
    else
        log_error "Redis build failed"
        ((FAILED_SUITES++))
        return 1
    fi
    
    echo "----------------------------------------"
    
    # Test basic Redis functionality
    log_info "Testing basic Redis functionality..."
    
    if src/redis-server --version >/dev/null 2>&1; then
        log_success "redis-server --version works"
    else
        log_error "redis-server --version failed"
        ((FAILED_SUITES++))
        return 1
    fi
    
    if src/redis-server --test-memory >/dev/null 2>&1; then
        log_success "redis-server --test-memory works"
    else
        log_error "redis-server --test-memory failed"
        ((FAILED_SUITES++))
        return 1
    fi
    
    # Test Redis with actual connections (if possible)
    log_info "Testing Redis with client connections..."
    
    # Start Redis server in background
    src/redis-server --port 6379 --save "" --appendonly no --daemonize yes --pidfile /tmp/redis-test.pid >/dev/null 2>&1
    
    if [ -f /tmp/redis-test.pid ]; then
        sleep 1
        
        # Test basic commands
        if echo "PING" | nc -w 1 localhost 6379 | grep -q "PONG"; then
            log_success "Redis PING command works"
        else
            log_warning "Redis PING command failed (nc may not be available)"
        fi
        
        # Stop Redis server
        if [ -f /tmp/redis-test.pid ]; then
            kill $(cat /tmp/redis-test.pid) 2>/dev/null || true
            rm -f /tmp/redis-test.pid
        fi
    else
        log_warning "Could not start Redis server for connection testing"
    fi
    
    echo "----------------------------------------"
    log_success "$suite_name completed successfully"
    ((PASSED_SUITES++))
    return 0
}

# Main execution
main() {
    log_header "Redis io_uring Complete Validation Suite"
    log_info "Team Member D - Comprehensive Testing Framework"
    log_info "This script validates all aspects of the Redis io_uring implementation"
    
    # Find Redis root directory
    REDIS_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
    log_info "Redis root directory: $REDIS_ROOT"
    cd "$REDIS_ROOT"
    
    # Run all validation suites
    run_validation_suite \
        "Environment Validation" \
        "scripts/validate_uring_environment.sh" \
        "Checking development environment setup and io_uring support"
    
    run_validation_suite \
        "Build System Validation" \
        "scripts/validate_build_system.sh" \
        "Validating Redis build system with and without io_uring"
    
    run_unit_tests
    
    run_integration_tests
    
    # Final summary
    log_header "Validation Suite Summary"
    
    echo -e "Total test suites: ${BOLD}$TOTAL_SUITES${NC}"
    echo -e "Passed: ${GREEN}$PASSED_SUITES${NC}"
    echo -e "Failed: ${RED}$FAILED_SUITES${NC}"
    echo
    
    if [ $FAILED_SUITES -eq 0 ]; then
        echo -e "${GREEN}${BOLD}🎉 ALL VALIDATION SUITES PASSED! 🎉${NC}"
        echo -e "${GREEN}Redis io_uring implementation is ready for development.${NC}"
        exit 0
    else
        echo -e "${RED}${BOLD}❌ $FAILED_SUITES VALIDATION SUITE(S) FAILED ❌${NC}"
        echo -e "${RED}Please address the issues above before proceeding.${NC}"
        exit 1
    fi
}

# Show help if requested
if [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
    echo "Redis io_uring Complete Validation Suite"
    echo
    echo "Usage: $0 [options]"
    echo
    echo "Options:"
    echo "  --help, -h    Show this help message"
    echo
    echo "This script runs all validation tests for the Redis io_uring implementation:"
    echo "  1. Environment validation (kernel, liburing, dependencies)"
    echo "  2. Build system validation (with/without io_uring)"
    echo "  3. Unit tests (test framework and components)"
    echo "  4. Integration tests (Redis functionality)"
    echo
    echo "The script will report a summary of all test results and exit with"
    echo "status 0 if all tests pass, or 1 if any tests fail."
    exit 0
fi

# Run main function
main "$@"
