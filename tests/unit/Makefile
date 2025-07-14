# Redis io_uring Unit Tests Makefile
# Team Member D - Week 1 Task 1.D.2

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -O0
INCLUDES = -I. -I../../src

# Check for liburing support
LIBURING_AVAILABLE := $(shell pkg-config --exists liburing && echo yes)
ifeq ($(LIBURING_AVAILABLE),yes)
    CFLAGS += -DHAVE_LIBURING
    LIBS += -luring
    LIBURING_CFLAGS := $(shell pkg-config --cflags liburing)
    LIBURING_LIBS := $(shell pkg-config --libs liburing)
    CFLAGS += $(LIBURING_CFLAGS)
    LIBS += $(LIBURING_LIBS)
endif

# Test framework
FRAMEWORK_OBJS = test_uring_framework.o

# Test executables
TESTS = test_uring_basic

# Default target
all: check-env $(TESTS)

# Environment check
check-env:
	@echo "=== Build Environment Check ==="
	@echo "Compiler: $(CC)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "INCLUDES: $(INCLUDES)"
	@echo "LIBS: $(LIBS)"
ifeq ($(LIBURING_AVAILABLE),yes)
	@echo "liburing: Available"
else
	@echo "liburing: Not available (tests will run without io_uring functionality)"
endif
	@echo "================================"
	@echo

# Test framework compilation
test_uring_framework.o: test_uring_framework.c test_uring_framework.h
	$(CC) $(CFLAGS) $(INCLUDES) -c test_uring_framework.c -o test_uring_framework.o

# Individual test compilation
test_uring_basic: test_uring_basic.c $(FRAMEWORK_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) test_uring_basic.c $(FRAMEWORK_OBJS) $(LIBS) -o test_uring_basic

# Run all tests
test: $(TESTS)
	@echo "=== Running Unit Tests ==="
	@for test in $(TESTS); do \
		echo "Running $$test..."; \
		./$$test; \
		if [ $$? -ne 0 ]; then \
			echo "Test $$test FAILED"; \
			exit 1; \
		fi; \
		echo; \
	done
	@echo "All tests completed successfully!"

# Run tests with valgrind for memory checking
test-valgrind: $(TESTS)
	@echo "=== Running Tests with Valgrind ==="
	@for test in $(TESTS); do \
		echo "Running $$test with valgrind..."; \
		valgrind --leak-check=full --error-exitcode=1 ./$$test; \
		if [ $$? -ne 0 ]; then \
			echo "Test $$test FAILED (valgrind)"; \
			exit 1; \
		fi; \
		echo; \
	done
	@echo "All valgrind tests completed successfully!"

# Run tests with specific conditions
test-no-uring: CFLAGS := $(filter-out -DHAVE_LIBURING,$(CFLAGS))
test-no-uring: LIBS := $(filter-out -luring,$(LIBS))
test-no-uring: clean $(TESTS)
	@echo "=== Running Tests without io_uring ==="
	@for test in $(TESTS); do \
		echo "Running $$test (no io_uring)..."; \
		./$$test; \
		if [ $$? -ne 0 ]; then \
			echo "Test $$test FAILED"; \
			exit 1; \
		fi; \
		echo; \
	done

# Coverage analysis (requires gcov)
coverage: CFLAGS += --coverage
coverage: LIBS += --coverage
coverage: clean $(TESTS)
	@echo "=== Running Tests with Coverage ==="
	@for test in $(TESTS); do \
		./$$test; \
	done
	@echo "Generating coverage report..."
	@gcov *.c
	@echo "Coverage files generated (*.gcov)"

# Performance testing
perf: $(TESTS)
	@echo "=== Performance Testing ==="
	@for test in $(TESTS); do \
		echo "Performance testing $$test..."; \
		time ./$$test; \
		echo; \
	done

# Install test dependencies (requires sudo)
install-deps:
	@echo "Installing test dependencies..."
	@if command -v apt-get >/dev/null 2>&1; then \
		sudo apt-get update && sudo apt-get install -y valgrind gcov; \
	elif command -v yum >/dev/null 2>&1; then \
		sudo yum install -y valgrind gcov; \
	else \
		echo "Please install valgrind and gcov manually"; \
	fi

# Clean build artifacts
clean:
	rm -f *.o $(TESTS) *.gcov *.gcda *.gcno
	rm -f /tmp/redis_test_*

# Help target
help:
	@echo "Available targets:"
	@echo "  all          - Build all tests"
	@echo "  test         - Run all tests"
	@echo "  test-valgrind - Run tests with memory checking"
	@echo "  test-no-uring - Run tests without io_uring support"
	@echo "  coverage     - Run tests with coverage analysis"
	@echo "  perf         - Run performance tests"
	@echo "  install-deps - Install test dependencies"
	@echo "  clean        - Clean build artifacts"
	@echo "  check-env    - Show build environment"
	@echo "  help         - Show this help"

# Phony targets
.PHONY: all test test-valgrind test-no-uring coverage perf install-deps clean help check-env
