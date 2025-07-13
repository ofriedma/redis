# Redis io_uring Performance Testing Makefile
# Team Member D - Week 1 Additional Utilities

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -O2
INCLUDES = -I. -I../../src

# Libraries
LIBS = -lm

# Source files
FRAMEWORK_SRCS = benchmark_framework.c
RUNNER_SRCS = run_benchmarks.c

# Object files
FRAMEWORK_OBJS = $(FRAMEWORK_SRCS:.c=.o)
RUNNER_OBJS = $(RUNNER_SRCS:.c=.o)

# Executables
TARGETS = run_benchmarks

# Default target
all: $(TARGETS)

# Build benchmark runner
run_benchmarks: $(FRAMEWORK_OBJS) $(RUNNER_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LIBS)

# Object file compilation
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Run basic performance test
test: run_benchmarks
	@echo "=== Running Basic Performance Test ==="
	@echo "Note: This requires Redis to be built in ../../src/"
	@if [ -f "../../src/redis-server" ] && [ -f "../../src/redis-benchmark" ]; then \
		./run_benchmarks -c 10 -n 1000 -t set,get --epoll-only; \
	else \
		echo "Redis binaries not found. Please build Redis first:"; \
		echo "  cd ../.. && make"; \
	fi

# Run comprehensive benchmark
benchmark: run_benchmarks
	@echo "=== Running Comprehensive Benchmark ==="
	@if [ -f "../../src/redis-server" ] && [ -f "../../src/redis-benchmark" ]; then \
		./run_benchmarks -c 50 -n 10000 -t set,get,incr,lpush,lpop -o benchmark_results.csv; \
		echo "Results saved to benchmark_results.csv"; \
	else \
		echo "Redis binaries not found. Please build Redis first:"; \
		echo "  cd ../.. && make"; \
	fi

# Run comparison test (epoll vs io_uring)
compare: run_benchmarks
	@echo "=== Running Epoll vs io_uring Comparison ==="
	@if [ -f "../../src/redis-server" ] && [ -f "../../src/redis-benchmark" ]; then \
		./run_benchmarks -c 25 -n 5000 -t set,get -o comparison_results.csv; \
		echo "Comparison results saved to comparison_results.csv"; \
	else \
		echo "Redis binaries not found. Please build Redis first:"; \
		echo "  cd ../.. && make"; \
	fi

# Quick test with minimal load
quick-test: run_benchmarks
	@echo "=== Running Quick Performance Test ==="
	@if [ -f "../../src/redis-server" ] && [ -f "../../src/redis-benchmark" ]; then \
		./run_benchmarks -c 5 -n 100 -t ping,set,get --epoll-only; \
	else \
		echo "Redis binaries not found. Please build Redis first:"; \
		echo "  cd ../.. && make"; \
	fi

# Generate sample benchmark report
report: benchmark
	@echo "=== Generating Benchmark Report ==="
	@if [ -f "benchmark_results.csv" ]; then \
		echo "# Redis Performance Benchmark Report" > benchmark_report.md; \
		echo "" >> benchmark_report.md; \
		echo "Generated on: $$(date)" >> benchmark_report.md; \
		echo "" >> benchmark_report.md; \
		echo "## Configuration" >> benchmark_report.md; \
		echo "- Clients: 50" >> benchmark_report.md; \
		echo "- Operations: 10,000" >> benchmark_report.md; \
		echo "- Tests: set, get, incr, lpush, lpop" >> benchmark_report.md; \
		echo "" >> benchmark_report.md; \
		echo "## Results" >> benchmark_report.md; \
		echo "" >> benchmark_report.md; \
		echo "| Test | Ops/sec | Avg Latency (ms) |" >> benchmark_report.md; \
		echo "|------|---------|------------------|" >> benchmark_report.md; \
		tail -n +2 benchmark_results.csv | while IFS=, read test ops latency rest; do \
			printf "| %s | %.0f | %.3f |\n" "$$test" "$$ops" "$$latency" >> benchmark_report.md; \
		done; \
		echo "" >> benchmark_report.md; \
		echo "Raw data available in benchmark_results.csv" >> benchmark_report.md; \
		echo "Report generated: benchmark_report.md"; \
	else \
		echo "No benchmark results found. Run 'make benchmark' first."; \
	fi

# Install performance testing dependencies
install-deps:
	@echo "Installing performance testing dependencies..."
	@if command -v apt-get >/dev/null 2>&1; then \
		sudo apt-get update && sudo apt-get install -y gnuplot python3-matplotlib; \
	elif command -v yum >/dev/null 2>&1; then \
		sudo yum install -y gnuplot python3-matplotlib; \
	else \
		echo "Please install gnuplot and python3-matplotlib manually"; \
	fi

# Generate performance graphs (requires gnuplot)
graphs: benchmark_results.csv
	@echo "=== Generating Performance Graphs ==="
	@if command -v gnuplot >/dev/null 2>&1; then \
		echo "set terminal png size 800,600" > plot_performance.gp; \
		echo "set output 'performance_ops_per_sec.png'" >> plot_performance.gp; \
		echo "set title 'Redis Performance - Operations per Second'" >> plot_performance.gp; \
		echo "set xlabel 'Test Type'" >> plot_performance.gp; \
		echo "set ylabel 'Operations per Second'" >> plot_performance.gp; \
		echo "set style data histogram" >> plot_performance.gp; \
		echo "set style histogram cluster gap 1" >> plot_performance.gp; \
		echo "set style fill solid border -1" >> plot_performance.gp; \
		echo "set boxwidth 0.9" >> plot_performance.gp; \
		echo "set datafile separator ','" >> plot_performance.gp; \
		echo "plot 'benchmark_results.csv' using 2:xtic(1) title 'Ops/sec'" >> plot_performance.gp; \
		gnuplot plot_performance.gp; \
		echo "Performance graph generated: performance_ops_per_sec.png"; \
		rm -f plot_performance.gp; \
	else \
		echo "gnuplot not available. Install with 'make install-deps'"; \
	fi

# Stress test with high load
stress-test: run_benchmarks
	@echo "=== Running Stress Test ==="
	@echo "Warning: This test uses high load and may impact system performance"
	@if [ -f "../../src/redis-server" ] && [ -f "../../src/redis-benchmark" ]; then \
		./run_benchmarks -c 200 -n 50000 -t set,get,incr -o stress_test_results.csv; \
		echo "Stress test results saved to stress_test_results.csv"; \
	else \
		echo "Redis binaries not found. Please build Redis first:"; \
		echo "  cd ../.. && make"; \
	fi

# Clean build artifacts
clean:
	rm -f *.o $(TARGETS)
	rm -f *.csv *.png *.gp *.md

# Clean all generated files
clean-all: clean
	rm -f benchmark_*.csv comparison_*.csv stress_test_*.csv
	rm -f *.png *.md

# Help target
help:
	@echo "Available targets:"
	@echo "  all          - Build all performance testing tools"
	@echo "  test         - Run basic performance test"
	@echo "  benchmark    - Run comprehensive benchmark"
	@echo "  compare      - Run epoll vs io_uring comparison"
	@echo "  quick-test   - Run quick test with minimal load"
	@echo "  report       - Generate benchmark report"
	@echo "  graphs       - Generate performance graphs (requires gnuplot)"
	@echo "  stress-test  - Run high-load stress test"
	@echo "  install-deps - Install performance testing dependencies"
	@echo "  clean        - Clean build artifacts"
	@echo "  clean-all    - Clean all generated files"
	@echo "  help         - Show this help"

# Phony targets
.PHONY: all test benchmark compare quick-test report install-deps graphs stress-test clean clean-all help
