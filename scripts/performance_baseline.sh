#!/bin/bash

# Redis io_uring Performance Baseline Establishment
# Team Member D - Week 2 Task D.4
# Creates performance benchmarks and establishes baseline metrics

set -e

# Configuration
REDIS_PORT_EPOLL=6379
REDIS_PORT_URING=6380
BASELINE_DIR="performance_baselines"
RESULTS_DIR="performance_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Performance test parameters
BENCHMARK_DURATION=60
BENCHMARK_CLIENTS=(1 10 50 100)
BENCHMARK_OPERATIONS=(1000 10000 50000)
BENCHMARK_DATA_SIZES=(64 256 1024 4096)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Logging function
log() {
    echo -e "${BLUE}[$(date '+%Y-%m-%d %H:%M:%S')]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

# Check prerequisites
check_prerequisites() {
    log "Checking prerequisites..."
    
    # Check if Redis binaries exist
    if [ ! -f "src/redis-server" ] || [ ! -f "src/redis-benchmark" ] || [ ! -f "src/redis-cli" ]; then
        error "Redis binaries not found. Please build Redis first."
        exit 1
    fi
    
    # Check for required tools
    for tool in bc awk sed; do
        if ! command -v $tool &> /dev/null; then
            error "$tool is required but not installed."
            exit 1
        fi
    done
    
    # Create directories
    mkdir -p "$BASELINE_DIR" "$RESULTS_DIR"
    
    success "Prerequisites check completed"
}

# Build Redis variants
build_redis_variants() {
    log "Building Redis variants..."
    
    # Build without io_uring
    log "Building Redis without io_uring..."
    make clean > /dev/null 2>&1
    make > /dev/null 2>&1
    cp src/redis-server redis-server-epoll
    cp src/redis-benchmark redis-benchmark-epoll
    cp src/redis-cli redis-cli-epoll
    
    # Build with io_uring (if available)
    log "Building Redis with io_uring..."
    make clean > /dev/null 2>&1
    if make USE_URING=yes > /dev/null 2>&1; then
        cp src/redis-server redis-server-uring
        cp src/redis-benchmark redis-benchmark-uring
        cp src/redis-cli redis-cli-uring
        success "io_uring build successful"
        return 0
    else
        warning "io_uring build failed - will only test epoll variant"
        return 1
    fi
}

# Start Redis server
start_redis_server() {
    local variant=$1
    local port=$2
    local pid_file="redis-${variant}-${port}.pid"
    
    log "Starting Redis server ($variant) on port $port..."
    
    timeout ${BENCHMARK_DURATION}s ./redis-server-${variant} \
        --port $port \
        --save "" \
        --appendonly no \
        --daemonize yes \
        --pidfile $pid_file \
        --logfile redis-${variant}-${port}.log \
        > /dev/null 2>&1
    
    # Wait for server to start
    sleep 3
    
    # Verify server is running
    if ./redis-cli-${variant} -p $port ping > /dev/null 2>&1; then
        success "Redis server ($variant) started on port $port"
        echo $pid_file
    else
        error "Failed to start Redis server ($variant) on port $port"
        return 1
    fi
}

# Stop Redis server
stop_redis_server() {
    local pid_file=$1
    
    if [ -f "$pid_file" ]; then
        local pid=$(cat "$pid_file")
        if kill -0 $pid 2>/dev/null; then
            kill $pid
            rm -f "$pid_file"
            log "Stopped Redis server (PID: $pid)"
        fi
    fi
}

# Run performance benchmark
run_benchmark() {
    local variant=$1
    local port=$2
    local clients=$3
    local operations=$4
    local data_size=$5
    local test_name="${variant}_c${clients}_n${operations}_d${data_size}"
    local result_file="${RESULTS_DIR}/benchmark_${test_name}_${TIMESTAMP}.csv"
    
    log "Running benchmark: $test_name"
    
    # Run benchmark
    ./redis-benchmark-${variant} \
        -h 127.0.0.1 \
        -p $port \
        -c $clients \
        -n $operations \
        -d $data_size \
        -t set,get,incr,lpush,rpush,lpop,rpop,sadd,hset,spop,zadd,zpopmin,lrange \
        --csv \
        > "$result_file" 2>/dev/null
    
    if [ $? -eq 0 ]; then
        success "Benchmark completed: $test_name"
        echo "$result_file"
    else
        error "Benchmark failed: $test_name"
        return 1
    fi
}

# Extract performance metrics
extract_metrics() {
    local result_file=$1
    local variant=$2
    local clients=$3
    local operations=$4
    local data_size=$5
    
    # Extract key metrics from CSV
    local set_rps=$(grep '"SET"' "$result_file" | cut -d',' -f2 | tr -d '"')
    local get_rps=$(grep '"GET"' "$result_file" | cut -d',' -f2 | tr -d '"')
    local incr_rps=$(grep '"INCR"' "$result_file" | cut -d',' -f2 | tr -d '"')
    
    # Calculate average RPS
    local avg_rps=$(echo "scale=2; ($set_rps + $get_rps + $incr_rps) / 3" | bc -l)
    
    # Store metrics
    echo "$variant,$clients,$operations,$data_size,$set_rps,$get_rps,$incr_rps,$avg_rps,$TIMESTAMP" >> "${BASELINE_DIR}/performance_metrics.csv"
    
    log "Metrics extracted: SET=$set_rps, GET=$get_rps, INCR=$incr_rps, AVG=$avg_rps RPS"
}

# Get io_uring specific statistics
get_uring_statistics() {
    local port=$1
    local variant=$2
    local stats_file="${RESULTS_DIR}/uring_stats_${variant}_${TIMESTAMP}.txt"
    
    if [ "$variant" = "uring" ]; then
        log "Collecting io_uring statistics..."
        ./redis-cli-uring -p $port INFO uring > "$stats_file" 2>/dev/null
        
        if [ -s "$stats_file" ]; then
            # Extract key io_uring metrics
            local success_rate=$(grep "uring_success_rate" "$stats_file" | cut -d: -f2)
            local avg_completion_time=$(grep "uring_avg_completion_time_us" "$stats_file" | cut -d: -f2)
            local buffer_hit_rate=$(grep "buffer_pool_hit_rate" "$stats_file" | cut -d: -f2)
            local ops_completed=$(grep "uring_ops_completed" "$stats_file" | cut -d: -f2)
            
            # Store io_uring specific metrics
            echo "$success_rate,$avg_completion_time,$buffer_hit_rate,$ops_completed,$TIMESTAMP" >> "${BASELINE_DIR}/uring_metrics.csv"
            
            log "io_uring metrics: Success=${success_rate}%, AvgTime=${avg_completion_time}μs, BufferHit=${buffer_hit_rate}%"
        fi
    fi
}

# Run comprehensive performance test suite
run_performance_suite() {
    local variant=$1
    local port=$2
    
    log "Running performance suite for $variant variant..."
    
    # Start server
    local pid_file=$(start_redis_server "$variant" "$port")
    if [ $? -ne 0 ]; then
        return 1
    fi
    
    # Run benchmarks with different parameters
    for clients in "${BENCHMARK_CLIENTS[@]}"; do
        for operations in "${BENCHMARK_OPERATIONS[@]}"; do
            for data_size in "${BENCHMARK_DATA_SIZES[@]}"; do
                local result_file=$(run_benchmark "$variant" "$port" "$clients" "$operations" "$data_size")
                if [ $? -eq 0 ]; then
                    extract_metrics "$result_file" "$variant" "$clients" "$operations" "$data_size"
                fi
                
                # Small delay between tests
                sleep 1
            done
        done
    done
    
    # Get io_uring statistics
    get_uring_statistics "$port" "$variant"
    
    # Stop server
    stop_redis_server "$pid_file"
    
    success "Performance suite completed for $variant variant"
}

# Generate performance baseline report
generate_baseline_report() {
    local report_file="${BASELINE_DIR}/performance_baseline_${TIMESTAMP}.md"
    
    log "Generating performance baseline report..."
    
    cat > "$report_file" << EOF
# Redis io_uring Performance Baseline Report

**Generated:** $(date)
**Timestamp:** $TIMESTAMP

## Test Configuration

- Benchmark Duration: ${BENCHMARK_DURATION}s
- Client Counts: ${BENCHMARK_CLIENTS[*]}
- Operation Counts: ${BENCHMARK_OPERATIONS[*]}
- Data Sizes: ${BENCHMARK_DATA_SIZES[*]} bytes

## Performance Metrics

### Summary Statistics

EOF

    # Add performance comparison if both variants were tested
    if [ -f "${BASELINE_DIR}/performance_metrics.csv" ]; then
        echo "### Performance Comparison" >> "$report_file"
        echo "" >> "$report_file"
        echo "| Variant | Clients | Operations | Data Size | SET RPS | GET RPS | INCR RPS | AVG RPS |" >> "$report_file"
        echo "|---------|---------|------------|-----------|---------|---------|----------|---------|" >> "$report_file"
        
        tail -n +2 "${BASELINE_DIR}/performance_metrics.csv" | while IFS=',' read variant clients ops data_size set_rps get_rps incr_rps avg_rps timestamp; do
            echo "| $variant | $clients | $ops | $data_size | $set_rps | $get_rps | $incr_rps | $avg_rps |" >> "$report_file"
        done
    fi
    
    # Add io_uring specific metrics if available
    if [ -f "${BASELINE_DIR}/uring_metrics.csv" ]; then
        echo "" >> "$report_file"
        echo "### io_uring Specific Metrics" >> "$report_file"
        echo "" >> "$report_file"
        echo "| Success Rate | Avg Completion Time (μs) | Buffer Hit Rate | Operations Completed |" >> "$report_file"
        echo "|--------------|---------------------------|-----------------|---------------------|" >> "$report_file"
        
        tail -n +2 "${BASELINE_DIR}/uring_metrics.csv" | while IFS=',' read success_rate avg_time buffer_hit ops_completed timestamp; do
            echo "| ${success_rate}% | ${avg_time} | ${buffer_hit}% | $ops_completed |" >> "$report_file"
        done
    fi
    
    echo "" >> "$report_file"
    echo "## Files Generated" >> "$report_file"
    echo "" >> "$report_file"
    find "$RESULTS_DIR" -name "*${TIMESTAMP}*" | sort | while read file; do
        echo "- $file" >> "$report_file"
    done
    
    success "Performance baseline report generated: $report_file"
}

# Initialize CSV headers
initialize_csv_files() {
    # Performance metrics header
    if [ ! -f "${BASELINE_DIR}/performance_metrics.csv" ]; then
        echo "variant,clients,operations,data_size,set_rps,get_rps,incr_rps,avg_rps,timestamp" > "${BASELINE_DIR}/performance_metrics.csv"
    fi
    
    # io_uring metrics header
    if [ ! -f "${BASELINE_DIR}/uring_metrics.csv" ]; then
        echo "success_rate,avg_completion_time_us,buffer_hit_rate,ops_completed,timestamp" > "${BASELINE_DIR}/uring_metrics.csv"
    fi
}

# Main execution
main() {
    log "Starting Redis io_uring Performance Baseline Establishment"
    
    check_prerequisites
    initialize_csv_files
    
    # Build Redis variants
    local uring_available=false
    if build_redis_variants; then
        uring_available=true
    fi
    
    # Run performance tests for epoll variant
    run_performance_suite "epoll" "$REDIS_PORT_EPOLL"
    
    # Run performance tests for io_uring variant if available
    if [ "$uring_available" = true ]; then
        run_performance_suite "uring" "$REDIS_PORT_URING"
    fi
    
    # Generate baseline report
    generate_baseline_report
    
    # Cleanup
    rm -f redis-server-* redis-benchmark-* redis-cli-* redis-*.log redis-*.pid
    
    success "Performance baseline establishment completed!"
    log "Results stored in: $BASELINE_DIR"
    log "Raw data stored in: $RESULTS_DIR"
}

# Run main function
main "$@"
