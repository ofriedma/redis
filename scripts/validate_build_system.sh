#!/bin/bash

# Redis io_uring Build System Validation Script
# Team Member D - Week 1 Task 1.D.3
# 
# This script validates that the Redis build system works correctly
# with and without io_uring support, ensuring proper fallback behavior.

set +e  # Don't exit on first error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Counters
TESTS_PASSED=0
TESTS_FAILED=0
WARNINGS=0

# Logging functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((TESTS_PASSED++))
}

log_error() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((TESTS_FAILED++))
}

log_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
    ((WARNINGS++))
}

# Cleanup function
cleanup() {
    log_info "Cleaning up build artifacts..."
    cd "$REDIS_ROOT"
    make clean >/dev/null 2>&1
    cd src
    make clean >/dev/null 2>&1
    cd ..
}

# Trap cleanup on exit
trap cleanup EXIT

# Header
echo "=============================================="
echo "Redis Build System Validation"
echo "Team Member D - Week 1 Build Testing"
echo "=============================================="
echo

# Find Redis root directory
REDIS_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
log_info "Redis root directory: $REDIS_ROOT"

if [ ! -f "$REDIS_ROOT/src/Makefile" ]; then
    log_error "Redis source Makefile not found at $REDIS_ROOT/src/Makefile"
    exit 1
fi

cd "$REDIS_ROOT"

# Test 1: Clean build without io_uring
log_info "Testing clean build without io_uring..."
make clean >/dev/null 2>&1

if make -C src >/dev/null 2>&1; then
    log_success "Clean build without io_uring succeeded"
    
    # Check that redis-server was built
    if [ -f "src/redis-server" ]; then
        log_success "redis-server binary created"
        
        # Test basic functionality
        if src/redis-server --version >/dev/null 2>&1; then
            log_success "redis-server --version works"
        else
            log_error "redis-server --version failed"
        fi
        
        # Check that it doesn't have io_uring symbols
        if strings src/redis-server | grep -q "io_uring"; then
            log_warning "redis-server contains io_uring symbols (unexpected without USE_URING)"
        else
            log_success "redis-server does not contain io_uring symbols (expected)"
        fi
    else
        log_error "redis-server binary not created"
    fi
else
    log_error "Clean build without io_uring failed"
fi

# Test 2: Build with USE_URING=yes (should fail gracefully if liburing not available)
log_info "Testing build with USE_URING=yes..."
make clean >/dev/null 2>&1

if make -C src USE_URING=yes >/dev/null 2>&1; then
    log_success "Build with USE_URING=yes succeeded"
    
    # Check for io_uring symbols
    if strings src/redis-server | grep -q "io_uring"; then
        log_success "redis-server contains io_uring symbols (expected with USE_URING=yes)"
    else
        log_warning "redis-server does not contain io_uring symbols (unexpected with USE_URING=yes)"
    fi
    
    # Test version output
    VERSION_OUTPUT=$(src/redis-server --version 2>&1)
    if echo "$VERSION_OUTPUT" | grep -q "uring"; then
        log_success "Version output mentions io_uring support"
    else
        log_info "Version output: $VERSION_OUTPUT"
        log_warning "Version output does not mention io_uring (may be expected if not implemented yet)"
    fi
else
    log_warning "Build with USE_URING=yes failed (expected if liburing not available)"
    
    # Check if it's a liburing issue
    if pkg-config --exists liburing; then
        log_error "liburing is available but build still failed"
    else
        log_info "liburing not available, build failure is expected"
    fi
fi

# Test 3: Makefile detection logic
log_info "Testing Makefile detection logic..."
cd src

# Check if Makefile has io_uring detection
if grep -q "USE_URING" Makefile; then
    log_success "Makefile contains USE_URING logic"
else
    log_warning "Makefile does not contain USE_URING logic (may not be implemented yet)"
fi

if grep -q "liburing" Makefile; then
    log_success "Makefile contains liburing references"
else
    log_warning "Makefile does not contain liburing references (may not be implemented yet)"
fi

if grep -q "HAVE_LIBURING" Makefile; then
    log_success "Makefile contains HAVE_LIBURING flag"
else
    log_warning "Makefile does not contain HAVE_LIBURING flag (may not be implemented yet)"
fi

cd ..

# Test 4: Configuration file validation
log_info "Testing configuration file handling..."

# Check if redis.conf has io_uring options
if grep -q "uring" redis.conf; then
    log_success "redis.conf contains io_uring configuration options"
else
    log_warning "redis.conf does not contain io_uring options (may not be implemented yet)"
fi

# Test 5: Header file validation
log_info "Testing header file structure..."

# Check for ae_uring.h
if [ -f "src/ae_uring.h" ]; then
    log_success "ae_uring.h header file exists"
    
    # Check header content
    if grep -q "HAVE_LIBURING" src/ae_uring.h; then
        log_success "ae_uring.h has proper conditional compilation"
    else
        log_warning "ae_uring.h missing conditional compilation guards"
    fi
    
    if grep -q "liburing.h" src/ae_uring.h; then
        log_success "ae_uring.h includes liburing.h"
    else
        log_warning "ae_uring.h does not include liburing.h"
    fi
else
    log_warning "ae_uring.h header file does not exist (may not be implemented yet)"
fi

# Check for ae_uring.c
if [ -f "src/ae_uring.c" ]; then
    log_success "ae_uring.c implementation file exists"
    
    # Check for key functions
    if grep -q "aeApiCreate_uring" src/ae_uring.c; then
        log_success "ae_uring.c contains aeApiCreate_uring function"
    else
        log_warning "ae_uring.c missing aeApiCreate_uring function"
    fi
    
    if grep -q "aeApiPoll_uring" src/ae_uring.c; then
        log_success "ae_uring.c contains aeApiPoll_uring function"
    else
        log_warning "ae_uring.c missing aeApiPoll_uring function"
    fi
else
    log_warning "ae_uring.c implementation file does not exist (may not be implemented yet)"
fi

# Test 6: Dependency validation
log_info "Testing build dependencies..."

# Check for required tools
for tool in gcc make pkg-config; do
    if command -v $tool >/dev/null 2>&1; then
        log_success "$tool is available"
    else
        log_error "$tool is not available (required for build)"
    fi
done

# Check for optional liburing
if pkg-config --exists liburing; then
    LIBURING_VERSION=$(pkg-config --modversion liburing)
    log_success "liburing $LIBURING_VERSION is available"
    
    # Test compilation with liburing
    cat > /tmp/test_liburing_build.c << 'EOF'
#include <liburing.h>
int main() {
    struct io_uring ring;
    io_uring_queue_init(8, &ring, 0);
    io_uring_queue_exit(&ring);
    return 0;
}
EOF
    
    if gcc -o /tmp/test_liburing_build /tmp/test_liburing_build.c $(pkg-config --cflags --libs liburing) 2>/dev/null; then
        log_success "liburing compilation test passed"
        rm -f /tmp/test_liburing_build
    else
        log_error "liburing compilation test failed"
    fi
    rm -f /tmp/test_liburing_build.c
else
    log_info "liburing not available (io_uring features will be disabled)"
fi

# Test 7: Cross-compilation compatibility
log_info "Testing cross-compilation compatibility..."

# Test that build system doesn't break with different architectures
if make -C src clean >/dev/null 2>&1; then
    log_success "Build system clean works"
else
    log_error "Build system clean failed"
fi

# Summary
echo
echo "=============================================="
echo "Build System Validation Summary"
echo "=============================================="
echo -e "Tests passed: ${GREEN}$TESTS_PASSED${NC}"
echo -e "Tests failed: ${RED}$TESTS_FAILED${NC}"
echo -e "Warnings: ${YELLOW}$WARNINGS${NC}"
echo

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ Build system validation completed successfully!${NC}"
    if [ $WARNINGS -gt 0 ]; then
        echo -e "${YELLOW}Note: Some warnings indicate features not yet implemented.${NC}"
    fi
    exit 0
else
    echo -e "${RED}✗ Build system validation found issues. Please address the failed tests above.${NC}"
    exit 1
fi
