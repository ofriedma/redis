#!/bin/bash

# Redis io_uring Performance Monitoring
# Team Member D - Week 2 Task D.4
# Continuous performance monitoring and alerting system

set -e

# Configuration
MONITOR_DURATION=${MONITOR_DURATION:-300}  # 5 minutes default
SAMPLE_INTERVAL=${SAMPLE_INTERVAL:-10}     # 10 seconds default
REDIS_PORT=${REDIS_PORT:-6379}
ALERT_THRESHOLD_RPS=${ALERT_THRESHOLD_RPS:-1000}
ALERT_THRESHOLD_LATENCY=${ALERT_THRESHOLD_LATENCY:-10}  # milliseconds
OUTPUT_DIR="performance_monitoring"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Logging functions
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

alert() {
    echo -e "${RED}[ALERT]${NC} $1"
}

# Initialize monitoring
initialize_monitoring() {
    log "Initializing performance monitoring..."
    
    # Create output directory
    mkdir -p "$OUTPUT_DIR"
    
    # Check if Redis is running
    if ! redis-cli -p $REDIS_PORT ping > /dev/null 2>&1; then
        error "Redis server not running on port $REDIS_PORT"
        exit 1
    fi
    
    # Check if io_uring is enabled
    local uring_info=$(redis-cli -p $REDIS_PORT INFO uring 2>/dev/null || echo "")
    if [ -n "$uring_info" ]; then
        log "io_uring monitoring enabled"
        echo "true" > "${OUTPUT_DIR}/uring_enabled.flag"
    else
        log "Standard Redis monitoring (no io_uring)"
        echo "false" > "${OUTPUT_DIR}/uring_enabled.flag"
    fi
    
    # Initialize CSV files
    local timestamp=$(date +%Y%m%d_%H%M%S)
    echo "timestamp,rps_set,rps_get,rps_incr,latency_set_ms,latency_get_ms,latency_incr_ms,memory_used_mb,connected_clients" > "${OUTPUT_DIR}/performance_${timestamp}.csv"
    
    if [ -f "${OUTPUT_DIR}/uring_enabled.flag" ] && [ "$(cat ${OUTPUT_DIR}/uring_enabled.flag)" = "true" ]; then
        echo "timestamp,success_rate,avg_completion_time_us,buffer_hit_rate,ops_completed,ops_failed,active_operations" > "${OUTPUT_DIR}/uring_metrics_${timestamp}.csv"
    fi
    
    echo "$timestamp" > "${OUTPUT_DIR}/current_session.txt"
    
    success "Monitoring initialized for session: $timestamp"
}

# Collect performance metrics
collect_performance_metrics() {
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    local session=$(cat "${OUTPUT_DIR}/current_session.txt")
    
    # Run quick benchmark to get current performance
    local benchmark_result=$(redis-benchmark -h 127.0.0.1 -p $REDIS_PORT -t set,get,incr -n 1000 -c 10 -q 2>/dev/null || echo "")
    
    if [ -z "$benchmark_result" ]; then
        warning "Failed to collect benchmark metrics"
        return 1
    fi
    
    # Parse benchmark results
    local rps_set=$(echo "$benchmark_result" | grep "SET:" | awk '{print $2}')
    local rps_get=$(echo "$benchmark_result" | grep "GET:" | awk '{print $2}')
    local rps_incr=$(echo "$benchmark_result" | grep "INCR:" | awk '{print $2}')
    
    # Get latency information
    local latency_info=$(redis-cli -p $REDIS_PORT --latency-history -i 1 2>/dev/null | head -n 3 || echo "")
    local latency_set_ms="0"
    local latency_get_ms="0"
    local latency_incr_ms="0"
    
    if [ -n "$latency_info" ]; then
        # Parse latency (simplified - in real implementation would be more sophisticated)
        latency_set_ms=$(echo "$latency_info" | head -n 1 | awk '{print $4}' | cut -d'=' -f2 || echo "0")
        latency_get_ms=$(echo "$latency_info" | head -n 2 | tail -n 1 | awk '{print $4}' | cut -d'=' -f2 || echo "0")
        latency_incr_ms=$(echo "$latency_info" | tail -n 1 | awk '{print $4}' | cut -d'=' -f2 || echo "0")
    fi
    
    # Get Redis INFO metrics
    local info_output=$(redis-cli -p $REDIS_PORT INFO memory,clients 2>/dev/null || echo "")
    local memory_used_mb=$(echo "$info_output" | grep "used_memory:" | cut -d: -f2 | tr -d '\r' | awk '{print int($1/1024/1024)}')
    local connected_clients=$(echo "$info_output" | grep "connected_clients:" | cut -d: -f2 | tr -d '\r')
    
    # Store performance metrics
    echo "$timestamp,$rps_set,$rps_get,$rps_incr,$latency_set_ms,$latency_get_ms,$latency_incr_ms,$memory_used_mb,$connected_clients" >> "${OUTPUT_DIR}/performance_${session}.csv"
    
    # Check for performance alerts
    check_performance_alerts "$rps_set" "$rps_get" "$rps_incr" "$latency_set_ms" "$latency_get_ms" "$latency_incr_ms"
    
    log "Performance metrics collected: SET=${rps_set} GET=${rps_get} INCR=${rps_incr} RPS, Latency=${latency_set_ms}ms"
}

# Collect io_uring specific metrics
collect_uring_metrics() {
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    local session=$(cat "${OUTPUT_DIR}/current_session.txt")
    
    if [ ! -f "${OUTPUT_DIR}/uring_enabled.flag" ] || [ "$(cat ${OUTPUT_DIR}/uring_enabled.flag)" != "true" ]; then
        return 0
    fi
    
    # Get io_uring statistics
    local uring_info=$(redis-cli -p $REDIS_PORT INFO uring 2>/dev/null || echo "")
    
    if [ -z "$uring_info" ]; then
        warning "Failed to collect io_uring metrics"
        return 1
    fi
    
    # Parse io_uring metrics
    local success_rate=$(echo "$uring_info" | grep "uring_success_rate:" | cut -d: -f2 | tr -d '\r')
    local avg_completion_time=$(echo "$uring_info" | grep "uring_avg_completion_time_us:" | cut -d: -f2 | tr -d '\r')
    local buffer_hit_rate=$(echo "$uring_info" | grep "buffer_pool_hit_rate:" | cut -d: -f2 | tr -d '\r')
    local ops_completed=$(echo "$uring_info" | grep "uring_ops_completed:" | cut -d: -f2 | tr -d '\r')
    local ops_failed=$(echo "$uring_info" | grep "uring_ops_failed:" | cut -d: -f2 | tr -d '\r')
    local active_operations=$(echo "$uring_info" | grep "active_operations:" | cut -d: -f2 | tr -d '\r')
    
    # Store io_uring metrics
    echo "$timestamp,$success_rate,$avg_completion_time,$buffer_hit_rate,$ops_completed,$ops_failed,$active_operations" >> "${OUTPUT_DIR}/uring_metrics_${session}.csv"
    
    # Check for io_uring specific alerts
    check_uring_alerts "$success_rate" "$avg_completion_time" "$buffer_hit_rate"
    
    log "io_uring metrics collected: Success=${success_rate}% AvgTime=${avg_completion_time}μs BufferHit=${buffer_hit_rate}%"
}

# Check performance alerts
check_performance_alerts() {
    local rps_set=$1
    local rps_get=$2
    local rps_incr=$3
    local latency_set_ms=$4
    local latency_get_ms=$5
    local latency_incr_ms=$6
    
    # Check RPS thresholds
    if [ -n "$rps_set" ] && [ "$rps_set" -lt "$ALERT_THRESHOLD_RPS" ]; then
        alert "SET RPS below threshold: $rps_set < $ALERT_THRESHOLD_RPS"
    fi
    
    if [ -n "$rps_get" ] && [ "$rps_get" -lt "$ALERT_THRESHOLD_RPS" ]; then
        alert "GET RPS below threshold: $rps_get < $ALERT_THRESHOLD_RPS"
    fi
    
    if [ -n "$rps_incr" ] && [ "$rps_incr" -lt "$ALERT_THRESHOLD_RPS" ]; then
        alert "INCR RPS below threshold: $rps_incr < $ALERT_THRESHOLD_RPS"
    fi
    
    # Check latency thresholds
    if [ -n "$latency_set_ms" ] && (( $(echo "$latency_set_ms > $ALERT_THRESHOLD_LATENCY" | bc -l 2>/dev/null || echo 0) )); then
        alert "SET latency above threshold: ${latency_set_ms}ms > ${ALERT_THRESHOLD_LATENCY}ms"
    fi
    
    if [ -n "$latency_get_ms" ] && (( $(echo "$latency_get_ms > $ALERT_THRESHOLD_LATENCY" | bc -l 2>/dev/null || echo 0) )); then
        alert "GET latency above threshold: ${latency_get_ms}ms > ${ALERT_THRESHOLD_LATENCY}ms"
    fi
    
    if [ -n "$latency_incr_ms" ] && (( $(echo "$latency_incr_ms > $ALERT_THRESHOLD_LATENCY" | bc -l 2>/dev/null || echo 0) )); then
        alert "INCR latency above threshold: ${latency_incr_ms}ms > ${ALERT_THRESHOLD_LATENCY}ms"
    fi
}

# Check io_uring specific alerts
check_uring_alerts() {
    local success_rate=$1
    local avg_completion_time=$2
    local buffer_hit_rate=$3
    
    # Check success rate
    if [ -n "$success_rate" ] && (( $(echo "$success_rate < 95" | bc -l 2>/dev/null || echo 0) )); then
        alert "io_uring success rate below 95%: ${success_rate}%"
    fi
    
    # Check completion time
    if [ -n "$avg_completion_time" ] && (( $(echo "$avg_completion_time > 50000" | bc -l 2>/dev/null || echo 0) )); then
        alert "io_uring average completion time above 50ms: ${avg_completion_time}μs"
    fi
    
    # Check buffer hit rate
    if [ -n "$buffer_hit_rate" ] && (( $(echo "$buffer_hit_rate < 70" | bc -l 2>/dev/null || echo 0) )); then
        alert "Buffer hit rate below 70%: ${buffer_hit_rate}%"
    fi
}

# Generate monitoring summary
generate_summary() {
    local session=$(cat "${OUTPUT_DIR}/current_session.txt")
    local summary_file="${OUTPUT_DIR}/monitoring_summary_${session}.md"
    
    log "Generating monitoring summary..."
    
    cat > "$summary_file" << EOF
# Performance Monitoring Summary

**Session:** $session
**Duration:** $MONITOR_DURATION seconds
**Sample Interval:** $SAMPLE_INTERVAL seconds
**Redis Port:** $REDIS_PORT

## Configuration

- RPS Alert Threshold: $ALERT_THRESHOLD_RPS
- Latency Alert Threshold: ${ALERT_THRESHOLD_LATENCY}ms

## Files Generated

- Performance Metrics: performance_${session}.csv
EOF

    if [ -f "${OUTPUT_DIR}/uring_enabled.flag" ] && [ "$(cat ${OUTPUT_DIR}/uring_enabled.flag)" = "true" ]; then
        echo "- io_uring Metrics: uring_metrics_${session}.csv" >> "$summary_file"
    fi
    
    echo "" >> "$summary_file"
    echo "## Summary Statistics" >> "$summary_file"
    
    # Calculate basic statistics from performance data
    if [ -f "${OUTPUT_DIR}/performance_${session}.csv" ]; then
        local sample_count=$(tail -n +2 "${OUTPUT_DIR}/performance_${session}.csv" | wc -l)
        echo "- Total Samples: $sample_count" >> "$summary_file"
        
        # Calculate averages (simplified)
        local avg_set_rps=$(tail -n +2 "${OUTPUT_DIR}/performance_${session}.csv" | cut -d',' -f2 | awk '{sum+=$1} END {print int(sum/NR)}')
        local avg_get_rps=$(tail -n +2 "${OUTPUT_DIR}/performance_${session}.csv" | cut -d',' -f3 | awk '{sum+=$1} END {print int(sum/NR)}')
        
        echo "- Average SET RPS: $avg_set_rps" >> "$summary_file"
        echo "- Average GET RPS: $avg_get_rps" >> "$summary_file"
    fi
    
    success "Monitoring summary generated: $summary_file"
}

# Main monitoring loop
run_monitoring() {
    log "Starting performance monitoring for $MONITOR_DURATION seconds..."
    
    local start_time=$(date +%s)
    local end_time=$((start_time + MONITOR_DURATION))
    
    while [ $(date +%s) -lt $end_time ]; do
        collect_performance_metrics
        collect_uring_metrics
        
        sleep $SAMPLE_INTERVAL
    done
    
    success "Monitoring completed"
}

# Main execution
main() {
    log "Redis io_uring Performance Monitor"
    log "Duration: ${MONITOR_DURATION}s, Interval: ${SAMPLE_INTERVAL}s"
    
    initialize_monitoring
    run_monitoring
    generate_summary
    
    success "Performance monitoring session completed"
    log "Results stored in: $OUTPUT_DIR"
}

# Handle script arguments
case "${1:-}" in
    --help|-h)
        echo "Usage: $0 [options]"
        echo "Options:"
        echo "  --duration SECONDS    Monitoring duration (default: 300)"
        echo "  --interval SECONDS    Sample interval (default: 10)"
        echo "  --port PORT          Redis port (default: 6379)"
        echo "  --help               Show this help"
        exit 0
        ;;
    --duration)
        MONITOR_DURATION=$2
        shift 2
        ;;
    --interval)
        SAMPLE_INTERVAL=$2
        shift 2
        ;;
    --port)
        REDIS_PORT=$2
        shift 2
        ;;
esac

# Run main function
main "$@"
