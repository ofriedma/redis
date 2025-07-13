#!/bin/bash

# Redis io_uring Development Environment Validation Script
# Team Member D - Week 1 Task 1.D.1
#
# This script validates that the development environment is properly
# configured for Redis io_uring development work.

# Don't exit on first error - we want to check everything
set +e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Counters
CHECKS_PASSED=0
CHECKS_FAILED=0
WARNINGS=0

# Logging functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((CHECKS_PASSED++))
}

log_error() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((CHECKS_FAILED++))
}

log_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
    ((WARNINGS++))
}

# Header
echo "=============================================="
echo "Redis io_uring Environment Validation"
echo "Team Member D - Week 1 Development Setup"
echo "=============================================="
echo

# Check 1: Kernel Version
log_info "Checking kernel version for io_uring support..."
KERNEL_VERSION=$(uname -r)
KERNEL_MAJOR=$(echo $KERNEL_VERSION | cut -d. -f1)
KERNEL_MINOR=$(echo $KERNEL_VERSION | cut -d. -f2)

if [ "$KERNEL_MAJOR" -gt 5 ] || ([ "$KERNEL_MAJOR" -eq 5 ] && [ "$KERNEL_MINOR" -ge 1 ]); then
    log_success "Kernel $KERNEL_VERSION supports io_uring (requires 5.1+)"
    
    # Check for advanced features
    if [ "$KERNEL_MAJOR" -gt 5 ] || ([ "$KERNEL_MAJOR" -eq 5 ] && [ "$KERNEL_MINOR" -ge 19 ]); then
        log_success "Kernel supports advanced io_uring features (multishot, provided buffers)"
    else
        log_warning "Kernel $KERNEL_VERSION has basic io_uring support. Advanced features require 5.19+"
    fi
else
    log_error "Kernel $KERNEL_VERSION does not support io_uring (requires 5.1+)"
fi

# Check 2: liburing installation
log_info "Checking liburing installation..."
if pkg-config --exists liburing; then
    LIBURING_VERSION=$(pkg-config --modversion liburing)
    log_success "liburing $LIBURING_VERSION is installed"
    
    # Check version requirements
    LIBURING_MAJOR=$(echo $LIBURING_VERSION | cut -d. -f1)
    LIBURING_MINOR=$(echo $LIBURING_VERSION | cut -d. -f2)
    
    if [ "$LIBURING_MAJOR" -gt 2 ] || ([ "$LIBURING_MAJOR" -eq 2 ] && [ "$LIBURING_MINOR" -ge 0 ]); then
        log_success "liburing version is sufficient for Redis io_uring implementation"
    else
        log_warning "liburing $LIBURING_VERSION may be too old. Recommend 2.0+"
    fi
else
    log_error "liburing not found. Install with: sudo apt-get install liburing-dev (Ubuntu/Debian) or sudo yum install liburing-devel (RHEL/CentOS)"
fi

# Check 3: Development headers
log_info "Checking for liburing development headers..."
if [ -f "/usr/include/liburing.h" ] || [ -f "/usr/local/include/liburing.h" ]; then
    log_success "liburing development headers found"
else
    log_error "liburing development headers not found. Install liburing-dev package"
fi

# Check 4: Compiler support
log_info "Checking compiler support..."
if command -v gcc >/dev/null 2>&1; then
    GCC_VERSION=$(gcc --version | head -n1)
    log_success "GCC found: $GCC_VERSION"
else
    log_error "GCC not found. Install build-essential package"
fi

if command -v make >/dev/null 2>&1; then
    MAKE_VERSION=$(make --version | head -n1)
    log_success "Make found: $MAKE_VERSION"
else
    log_error "Make not found. Install build-essential package"
fi

# Check 5: Basic io_uring functionality test
log_info "Testing basic io_uring functionality..."
cat > /tmp/test_uring_basic.c << 'EOF'
#include <liburing.h>
#include <stdio.h>
#include <unistd.h>

int main() {
    struct io_uring ring;
    int ret = io_uring_queue_init(8, &ring, 0);
    if (ret < 0) {
        printf("io_uring_queue_init failed: %d\n", ret);
        return 1;
    }
    
    printf("Basic io_uring functionality: OK\n");
    io_uring_queue_exit(&ring);
    return 0;
}
EOF

if gcc -o /tmp/test_uring_basic /tmp/test_uring_basic.c -luring 2>/dev/null; then
    if /tmp/test_uring_basic >/dev/null 2>&1; then
        log_success "Basic io_uring functionality test passed"
    else
        log_error "Basic io_uring functionality test failed - kernel may not support io_uring"
    fi
    rm -f /tmp/test_uring_basic
else
    log_error "Failed to compile basic io_uring test - check liburing installation"
fi
rm -f /tmp/test_uring_basic.c

# Check 6: SQPOLL support test
log_info "Testing SQPOLL support..."
cat > /tmp/test_sqpoll.c << 'EOF'
#include <liburing.h>
#include <stdio.h>

int main() {
    struct io_uring ring;
    struct io_uring_params params = {0};
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 1000;
    
    int ret = io_uring_queue_init_params(8, &ring, &params);
    if (ret < 0) {
        printf("SQPOLL not supported: %d\n", ret);
        return 1;
    }
    
    printf("SQPOLL support: OK\n");
    io_uring_queue_exit(&ring);
    return 0;
}
EOF

if gcc -o /tmp/test_sqpoll /tmp/test_sqpoll.c -luring 2>/dev/null; then
    if /tmp/test_sqpoll >/dev/null 2>&1; then
        log_success "SQPOLL support test passed"
    else
        log_warning "SQPOLL not supported - may require newer kernel or privileges"
    fi
    rm -f /tmp/test_sqpoll
else
    log_error "Failed to compile SQPOLL test"
fi
rm -f /tmp/test_sqpoll.c

# Check 7: Redis build dependencies
log_info "Checking Redis build dependencies..."

# Check for required tools
for tool in pkg-config; do
    if command -v $tool >/dev/null 2>&1; then
        log_success "$tool is available"
    else
        log_error "$tool not found - required for Redis build system"
    fi
done

# Check 8: Memory and system limits
log_info "Checking system limits for development..."

# Check available memory
MEMORY_KB=$(grep MemTotal /proc/meminfo | awk '{print $2}')
MEMORY_GB=$((MEMORY_KB / 1024 / 1024))

if [ $MEMORY_GB -ge 4 ]; then
    log_success "System has ${MEMORY_GB}GB RAM (sufficient for development)"
else
    log_warning "System has ${MEMORY_GB}GB RAM (recommend 4GB+ for comfortable development)"
fi

# Check ulimits
ULIMIT_FILES=$(ulimit -n)
if [ $ULIMIT_FILES -ge 1024 ]; then
    log_success "File descriptor limit: $ULIMIT_FILES (sufficient)"
else
    log_warning "File descriptor limit: $ULIMIT_FILES (may need to increase for testing)"
fi

# Summary
echo
echo "=============================================="
echo "Environment Validation Summary"
echo "=============================================="
echo -e "Checks passed: ${GREEN}$CHECKS_PASSED${NC}"
echo -e "Checks failed: ${RED}$CHECKS_FAILED${NC}"
echo -e "Warnings: ${YELLOW}$WARNINGS${NC}"
echo

if [ $CHECKS_FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ Environment is ready for Redis io_uring development!${NC}"
    exit 0
else
    echo -e "${RED}✗ Environment setup incomplete. Please address the failed checks above.${NC}"
    exit 1
fi
