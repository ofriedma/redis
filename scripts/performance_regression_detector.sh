#!/bin/bash

# Redis io_uring Performance Regression Detection
# Team Member D - Week 2 Task D.4
# Automated performance regression detection system

set -e

# Configuration
BASELINE_DIR="performance_baselines"
CURRENT_RESULTS_DIR="performance_results"
REGRESSION_THRESHOLD=10  # Percentage threshold for regression detection
IMPROVEMENT_THRESHOLD=5  # Percentage threshold for improvement detection

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

regression() {
    echo -e "${RED}[REGRESSION]${NC} $1"
}

improvement() {
    echo -e "${GREEN}[IMPROVEMENT]${NC} $1"
}

# Check if baseline exists
check_baseline() {
    if [ ! -f "${BASELINE_DIR}/performance_metrics.csv" ]; then
        error "No baseline performance metrics found. Please run performance_baseline.sh first."
        exit 1
    fi
    
    if [ ! -f "${BASELINE_DIR}/uring_metrics.csv" ]; then
        warning "No baseline io_uring metrics found."
    fi
    
    log "Baseline files found"
}

# Get latest baseline metrics
get_baseline_metrics() {
    local variant=$1
    local clients=$2
    local operations=$3
    local data_size=$4
    
    # Get the most recent baseline for this configuration
    grep "^${variant},${clients},${operations},${data_size}," "${BASELINE_DIR}/performance_metrics.csv" | tail -n 1
}

# Get current metrics
get_current_metrics() {
    local variant=$1
    local clients=$2
    local operations=$3
    local data_size=$4
    
    # Get the most recent current result for this configuration
    if [ -f "${CURRENT_RESULTS_DIR}/current_metrics.csv" ]; then
        grep "^${variant},${clients},${operations},${data_size}," "${CURRENT_RESULTS_DIR}/current_metrics.csv" | tail -n 1
    fi
}

# Calculate percentage change
calculate_percentage_change() {
    local baseline=$1
    local current=$2
    
    if [ "$baseline" = "0" ] || [ -z "$baseline" ]; then
        echo "N/A"
        return
    fi
    
    echo "scale=2; (($current - $baseline) / $baseline) * 100" | bc -l
}

# Analyze performance change
analyze_performance_change() {
    local metric_name=$1
    local baseline_value=$2
    local current_value=$3
    local change_percent=$4
    
    if [ "$change_percent" = "N/A" ]; then
        echo "  $metric_name: $baseline_value → $current_value (N/A)"
        return 0
    fi
    
    local abs_change=$(echo "$change_percent" | sed 's/-//')
    
    if (( $(echo "$change_percent < -$REGRESSION_THRESHOLD" | bc -l) )); then
        regression "  $metric_name: $baseline_value → $current_value (${change_percent}% - REGRESSION DETECTED)"
        return 1
    elif (( $(echo "$change_percent > $IMPROVEMENT_THRESHOLD" | bc -l) )); then
        improvement "  $metric_name: $baseline_value → $current_value (+${change_percent}% - IMPROVEMENT)"
        return 0
    else
        echo "  $metric_name: $baseline_value → $current_value (${change_percent}%)"
        return 0
    fi
}

# Compare performance metrics
compare_performance_metrics() {
    local regression_count=0
    local total_comparisons=0
    
    log "Comparing performance metrics..."
    
    # Read current metrics and compare with baseline
    if [ ! -f "${CURRENT_RESULTS_DIR}/current_metrics.csv" ]; then
        error "No current metrics file found"
        return 1
    fi
    
    # Skip header line and process each metric
    tail -n +2 "${CURRENT_RESULTS_DIR}/current_metrics.csv" | while IFS=',' read variant clients operations data_size set_rps get_rps incr_rps avg_rps timestamp; do
        echo ""
        log "Analyzing: $variant variant, $clients clients, $operations ops, ${data_size}B data"
        
        # Get corresponding baseline
        local baseline_line=$(get_baseline_metrics "$variant" "$clients" "$operations" "$data_size")
        
        if [ -z "$baseline_line" ]; then
            warning "No baseline found for this configuration"
            continue
        fi
        
        # Parse baseline values
        IFS=',' read baseline_variant baseline_clients baseline_operations baseline_data_size baseline_set baseline_get baseline_incr baseline_avg baseline_timestamp <<< "$baseline_line"
        
        # Calculate percentage changes
        local set_change=$(calculate_percentage_change "$baseline_set" "$set_rps")
        local get_change=$(calculate_percentage_change "$baseline_get" "$get_rps")
        local incr_change=$(calculate_percentage_change "$baseline_incr" "$incr_rps")
        local avg_change=$(calculate_percentage_change "$baseline_avg" "$avg_rps")
        
        # Analyze each metric
        local config_regressions=0
        analyze_performance_change "SET RPS" "$baseline_set" "$set_rps" "$set_change" || ((config_regressions++))
        analyze_performance_change "GET RPS" "$baseline_get" "$get_rps" "$get_change" || ((config_regressions++))
        analyze_performance_change "INCR RPS" "$baseline_incr" "$incr_rps" "$incr_change" || ((config_regressions++))
        analyze_performance_change "AVG RPS" "$baseline_avg" "$avg_rps" "$avg_change" || ((config_regressions++))
        
        ((total_comparisons++))
        ((regression_count += config_regressions))
    done
    
    return $regression_count
}

# Compare io_uring specific metrics
compare_uring_metrics() {
    local regression_count=0
    
    if [ ! -f "${BASELINE_DIR}/uring_metrics.csv" ] || [ ! -f "${CURRENT_RESULTS_DIR}/current_uring_metrics.csv" ]; then
        warning "io_uring metrics comparison skipped - files not found"
        return 0
    fi
    
    log "Comparing io_uring specific metrics..."
    
    # Get latest baseline and current io_uring metrics
    local baseline_line=$(tail -n 1 "${BASELINE_DIR}/uring_metrics.csv")
    local current_line=$(tail -n 1 "${CURRENT_RESULTS_DIR}/current_uring_metrics.csv")
    
    if [ -z "$baseline_line" ] || [ -z "$current_line" ]; then
        warning "Insufficient io_uring metrics for comparison"
        return 0
    fi
    
    # Parse baseline values
    IFS=',' read baseline_success baseline_time baseline_buffer baseline_ops baseline_timestamp <<< "$baseline_line"
    
    # Parse current values
    IFS=',' read current_success current_time current_buffer current_ops current_timestamp <<< "$current_line"
    
    # Calculate changes
    local success_change=$(calculate_percentage_change "$baseline_success" "$current_success")
    local time_change=$(calculate_percentage_change "$baseline_time" "$current_time")
    local buffer_change=$(calculate_percentage_change "$baseline_buffer" "$current_buffer")
    local ops_change=$(calculate_percentage_change "$baseline_ops" "$current_ops")
    
    echo ""
    log "io_uring Metrics Analysis:"
    
    # Analyze metrics (note: for completion time, negative change is good)
    analyze_performance_change "Success Rate %" "$baseline_success" "$current_success" "$success_change" || ((regression_count++))
    
    # For completion time, invert the logic (lower is better)
    if [ "$time_change" != "N/A" ]; then
        local inverted_time_change=$(echo "scale=2; -1 * $time_change" | bc -l)
        analyze_performance_change "Avg Completion Time μs" "$baseline_time" "$current_time" "$inverted_time_change" || ((regression_count++))
    fi
    
    analyze_performance_change "Buffer Hit Rate %" "$baseline_buffer" "$current_buffer" "$buffer_change" || ((regression_count++))
    analyze_performance_change "Operations Completed" "$baseline_ops" "$current_ops" "$ops_change" || ((regression_count++))
    
    return $regression_count
}

# Generate regression report
generate_regression_report() {
    local total_regressions=$1
    local report_file="${CURRENT_RESULTS_DIR}/regression_report_$(date +%Y%m%d_%H%M%S).md"
    
    log "Generating regression report..."
    
    cat > "$report_file" << EOF
# Performance Regression Analysis Report

**Generated:** $(date)
**Baseline Directory:** $BASELINE_DIR
**Current Results Directory:** $CURRENT_RESULTS_DIR

## Summary

- **Total Regressions Detected:** $total_regressions
- **Regression Threshold:** ${REGRESSION_THRESHOLD}%
- **Improvement Threshold:** ${IMPROVEMENT_THRESHOLD}%

## Analysis Results

EOF

    if [ $total_regressions -eq 0 ]; then
        echo "✅ **No performance regressions detected**" >> "$report_file"
        echo "" >> "$report_file"
        echo "All performance metrics are within acceptable thresholds or show improvements." >> "$report_file"
    else
        echo "⚠️ **Performance regressions detected: $total_regressions**" >> "$report_file"
        echo "" >> "$report_file"
        echo "Please review the detailed analysis above and investigate the following:" >> "$report_file"
        echo "" >> "$report_file"
        echo "1. Recent code changes that might impact performance" >> "$report_file"
        echo "2. System configuration changes" >> "$report_file"
        echo "3. Test environment differences" >> "$report_file"
        echo "4. Measurement methodology changes" >> "$report_file"
    fi
    
    echo "" >> "$report_file"
    echo "## Recommendations" >> "$report_file"
    echo "" >> "$report_file"
    
    if [ $total_regressions -gt 0 ]; then
        echo "- Investigate and fix performance regressions before merging" >> "$report_file"
        echo "- Run additional performance tests to confirm results" >> "$report_file"
        echo "- Consider updating baseline if changes are intentional" >> "$report_file"
    else
        echo "- Performance is stable or improved" >> "$report_file"
        echo "- Consider updating baseline to capture improvements" >> "$report_file"
    fi
    
    echo "" >> "$report_file"
    echo "## Files Analyzed" >> "$report_file"
    echo "" >> "$report_file"
    echo "- Baseline: ${BASELINE_DIR}/performance_metrics.csv" >> "$report_file"
    echo "- Current: ${CURRENT_RESULTS_DIR}/current_metrics.csv" >> "$report_file"
    
    if [ -f "${BASELINE_DIR}/uring_metrics.csv" ]; then
        echo "- Baseline io_uring: ${BASELINE_DIR}/uring_metrics.csv" >> "$report_file"
        echo "- Current io_uring: ${CURRENT_RESULTS_DIR}/current_uring_metrics.csv" >> "$report_file"
    fi
    
    success "Regression report generated: $report_file"
    echo "$report_file"
}

# Main execution
main() {
    log "Starting Performance Regression Detection"
    
    check_baseline
    
    local total_regressions=0
    
    # Compare performance metrics
    compare_performance_metrics
    local perf_regressions=$?
    ((total_regressions += perf_regressions))
    
    # Compare io_uring specific metrics
    compare_uring_metrics
    local uring_regressions=$?
    ((total_regressions += uring_regressions))
    
    # Generate report
    local report_file=$(generate_regression_report $total_regressions)
    
    echo ""
    if [ $total_regressions -eq 0 ]; then
        success "No performance regressions detected!"
        exit 0
    else
        error "Performance regressions detected: $total_regressions"
        error "See report: $report_file"
        exit 1
    fi
}

# Run main function
main "$@"
