#!/bin/bash

# Redis io_uring Configuration Optimization Script
# Week 5 - Production Deployment Tool

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
REDIS_CONF_PATH="/etc/redis/redis.conf"
BACKUP_SUFFIX=".backup.$(date +%Y%m%d_%H%M%S)"
WORKLOAD_TYPE=""
MEMORY_SIZE=""
CPU_CORES=""

# Logging functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Help function
show_help() {
    cat << EOF
Redis io_uring Configuration Optimization Script

Usage: $0 [OPTIONS]

OPTIONS:
    -w, --workload TYPE     Workload type: high-throughput, low-latency, memory-constrained
    -m, --memory SIZE       Available memory in GB (e.g., 16)
    -c, --cores COUNT       Number of CPU cores
    -f, --config PATH       Path to redis.conf file (default: /etc/redis/redis.conf)
    -h, --help              Show this help message

EXAMPLES:
    $0 -w high-throughput -m 32 -c 16
    $0 -w low-latency -m 8 -c 4
    $0 -w memory-constrained -m 4 -c 2

WORKLOAD TYPES:
    high-throughput         Optimize for maximum operations per second
    low-latency            Optimize for minimum response time
    memory-constrained     Optimize for minimal memory usage
EOF
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -w|--workload)
                WORKLOAD_TYPE="$2"
                shift 2
                ;;
            -m|--memory)
                MEMORY_SIZE="$2"
                shift 2
                ;;
            -c|--cores)
                CPU_CORES="$2"
                shift 2
                ;;
            -f|--config)
                REDIS_CONF_PATH="$2"
                shift 2
                ;;
            -h|--help)
                show_help
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                show_help
                exit 1
                ;;
        esac
    done
}

# Validate arguments
validate_args() {
    if [[ -z "$WORKLOAD_TYPE" ]]; then
        log_error "Workload type is required"
        show_help
        exit 1
    fi

    if [[ "$WORKLOAD_TYPE" != "high-throughput" && "$WORKLOAD_TYPE" != "low-latency" && "$WORKLOAD_TYPE" != "memory-constrained" ]]; then
        log_error "Invalid workload type: $WORKLOAD_TYPE"
        show_help
        exit 1
    fi

    if [[ -z "$MEMORY_SIZE" ]]; then
        log_error "Memory size is required"
        show_help
        exit 1
    fi

    if [[ -z "$CPU_CORES" ]]; then
        log_error "CPU cores count is required"
        show_help
        exit 1
    fi

    if [[ ! -f "$REDIS_CONF_PATH" ]]; then
        log_error "Redis configuration file not found: $REDIS_CONF_PATH"
        exit 1
    fi
}

# Check system requirements
check_system_requirements() {
    log_info "Checking system requirements..."

    # Check kernel version
    local kernel_version=$(uname -r | cut -d. -f1,2)
    local major=$(echo $kernel_version | cut -d. -f1)
    local minor=$(echo $kernel_version | cut -d. -f2)

    if [[ $major -lt 5 ]] || [[ $major -eq 5 && $minor -lt 6 ]]; then
        log_error "Kernel version $kernel_version is too old. Minimum required: 5.6"
        exit 1
    fi

    log_success "Kernel version $kernel_version is compatible"

    # Check liburing
    if ! pkg-config --exists liburing; then
        log_error "liburing not found. Please install liburing development package"
        exit 1
    fi

    local liburing_version=$(pkg-config --modversion liburing)
    log_success "liburing version $liburing_version found"

    # Check Redis binary
    if ! command -v redis-server >/dev/null 2>&1; then
        log_error "redis-server not found in PATH"
        exit 1
    fi

    # Check if Redis was compiled with io_uring support
    if ! redis-server --help | grep -q "uring"; then
        log_warning "Redis may not be compiled with io_uring support"
    fi

    log_success "System requirements check completed"
}

# Backup configuration
backup_config() {
    log_info "Backing up configuration file..."
    cp "$REDIS_CONF_PATH" "${REDIS_CONF_PATH}${BACKUP_SUFFIX}"
    log_success "Configuration backed up to ${REDIS_CONF_PATH}${BACKUP_SUFFIX}"
}

# Set configuration value
set_config() {
    local key="$1"
    local value="$2"
    local config_file="$3"

    if grep -q "^${key}" "$config_file"; then
        sed -i "s/^${key}.*/${key} ${value}/" "$config_file"
    else
        echo "${key} ${value}" >> "$config_file"
    fi
}

# Apply high-throughput optimizations
apply_high_throughput_config() {
    log_info "Applying high-throughput optimizations..."

    # io_uring settings
    set_config "uring-enabled" "yes" "$REDIS_CONF_PATH"
    set_config "uring-queue-depth" "8192" "$REDIS_CONF_PATH"
    set_config "uring-buffer-pool-size" "32768" "$REDIS_CONF_PATH"
    set_config "uring-buffer-size" "32768" "$REDIS_CONF_PATH"
    set_config "uring-multishot-accept" "yes" "$REDIS_CONF_PATH"
    set_config "uring-sqpoll" "yes" "$REDIS_CONF_PATH"
    set_config "uring-sqpoll-idle-ms" "10" "$REDIS_CONF_PATH"
    set_config "uring-use-provided-buffers" "yes" "$REDIS_CONF_PATH"
    set_config "uring-provided-buffer-count" "16384" "$REDIS_CONF_PATH"
    set_config "uring-use-registered-fds" "yes" "$REDIS_CONF_PATH"
    set_config "uring-max-registered-fds" "20000" "$REDIS_CONF_PATH"

    # General Redis settings
    set_config "maxclients" "100000" "$REDIS_CONF_PATH"
    set_config "tcp-backlog" "65536" "$REDIS_CONF_PATH"
    set_config "tcp-keepalive" "300" "$REDIS_CONF_PATH"

    # Memory settings
    local max_memory=$((MEMORY_SIZE * 3 / 4))  # Use 75% of available memory
    set_config "maxmemory" "${max_memory}gb" "$REDIS_CONF_PATH"
    set_config "maxmemory-policy" "allkeys-lru" "$REDIS_CONF_PATH"

    log_success "High-throughput configuration applied"
}

# Apply low-latency optimizations
apply_low_latency_config() {
    log_info "Applying low-latency optimizations..."

    # io_uring settings
    set_config "uring-enabled" "yes" "$REDIS_CONF_PATH"
    set_config "uring-queue-depth" "2048" "$REDIS_CONF_PATH"
    set_config "uring-buffer-pool-size" "8192" "$REDIS_CONF_PATH"
    set_config "uring-buffer-size" "8192" "$REDIS_CONF_PATH"
    set_config "uring-multishot-accept" "yes" "$REDIS_CONF_PATH"
    set_config "uring-sqpoll" "no" "$REDIS_CONF_PATH"  # Disable SQPOLL for lower latency
    set_config "uring-use-provided-buffers" "yes" "$REDIS_CONF_PATH"
    set_config "uring-provided-buffer-count" "4096" "$REDIS_CONF_PATH"
    set_config "uring-use-registered-fds" "yes" "$REDIS_CONF_PATH"
    set_config "uring-max-registered-fds" "10000" "$REDIS_CONF_PATH"

    # General Redis settings
    set_config "maxclients" "50000" "$REDIS_CONF_PATH"
    set_config "tcp-backlog" "32768" "$REDIS_CONF_PATH"
    set_config "tcp-keepalive" "60" "$REDIS_CONF_PATH"
    set_config "tcp-nodelay" "yes" "$REDIS_CONF_PATH"  # Enable for low latency

    # Memory settings
    local max_memory=$((MEMORY_SIZE * 2 / 3))  # Use 66% of available memory
    set_config "maxmemory" "${max_memory}gb" "$REDIS_CONF_PATH"
    set_config "maxmemory-policy" "allkeys-lru" "$REDIS_CONF_PATH"

    log_success "Low-latency configuration applied"
}

# Apply memory-constrained optimizations
apply_memory_constrained_config() {
    log_info "Applying memory-constrained optimizations..."

    # io_uring settings
    set_config "uring-enabled" "yes" "$REDIS_CONF_PATH"
    set_config "uring-queue-depth" "1024" "$REDIS_CONF_PATH"
    set_config "uring-buffer-pool-size" "2048" "$REDIS_CONF_PATH"
    set_config "uring-buffer-size" "4096" "$REDIS_CONF_PATH"
    set_config "uring-multishot-accept" "yes" "$REDIS_CONF_PATH"
    set_config "uring-sqpoll" "no" "$REDIS_CONF_PATH"
    set_config "uring-use-provided-buffers" "no" "$REDIS_CONF_PATH"  # Disable to save memory
    set_config "uring-use-registered-fds" "no" "$REDIS_CONF_PATH"   # Disable to save memory

    # General Redis settings
    set_config "maxclients" "10000" "$REDIS_CONF_PATH"
    set_config "tcp-backlog" "8192" "$REDIS_CONF_PATH"
    set_config "tcp-keepalive" "300" "$REDIS_CONF_PATH"

    # Memory settings
    local max_memory=$((MEMORY_SIZE / 2))  # Use 50% of available memory
    set_config "maxmemory" "${max_memory}gb" "$REDIS_CONF_PATH"
    set_config "maxmemory-policy" "allkeys-lru" "$REDIS_CONF_PATH"

    # Additional memory optimizations
    set_config "hash-max-ziplist-entries" "512" "$REDIS_CONF_PATH"
    set_config "hash-max-ziplist-value" "64" "$REDIS_CONF_PATH"
    set_config "list-max-ziplist-size" "-2" "$REDIS_CONF_PATH"
    set_config "set-max-intset-entries" "512" "$REDIS_CONF_PATH"
    set_config "zset-max-ziplist-entries" "128" "$REDIS_CONF_PATH"
    set_config "zset-max-ziplist-value" "64" "$REDIS_CONF_PATH"

    log_success "Memory-constrained configuration applied"
}

# Validate configuration
validate_config() {
    log_info "Validating configuration..."

    # Test configuration syntax
    if ! redis-server "$REDIS_CONF_PATH" --test-memory >/dev/null 2>&1; then
        log_error "Configuration validation failed"
        log_info "Restoring backup configuration..."
        cp "${REDIS_CONF_PATH}${BACKUP_SUFFIX}" "$REDIS_CONF_PATH"
        exit 1
    fi

    log_success "Configuration validation passed"
}

# Generate system tuning recommendations
generate_system_tuning() {
    log_info "Generating system tuning recommendations..."

    cat > "/tmp/redis-uring-sysctl.conf" << EOF
# Redis io_uring system tuning recommendations
# Add these to /etc/sysctl.conf and run 'sysctl -p'

# io_uring settings
fs.aio-max-nr = 1048576

# File descriptor limits
fs.file-max = 1000000

# Network settings
net.core.somaxconn = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.core.netdev_max_backlog = 5000

# Memory settings
vm.overcommit_memory = 1
vm.swappiness = 1
EOF

    cat > "/tmp/redis-uring-limits.conf" << EOF
# Redis io_uring limits recommendations
# Add these to /etc/security/limits.conf

redis soft nofile 1000000
redis hard nofile 1000000
redis soft nproc 32768
redis hard nproc 32768
EOF

    log_success "System tuning files generated:"
    log_info "  - /tmp/redis-uring-sysctl.conf"
    log_info "  - /tmp/redis-uring-limits.conf"
}

# Main function
main() {
    echo "Redis io_uring Configuration Optimization Script"
    echo "================================================"
    echo

    parse_args "$@"
    validate_args
    check_system_requirements
    backup_config

    case "$WORKLOAD_TYPE" in
        "high-throughput")
            apply_high_throughput_config
            ;;
        "low-latency")
            apply_low_latency_config
            ;;
        "memory-constrained")
            apply_memory_constrained_config
            ;;
    esac

    validate_config
    generate_system_tuning

    echo
    log_success "Configuration optimization completed!"
    log_info "Workload type: $WORKLOAD_TYPE"
    log_info "Memory size: ${MEMORY_SIZE}GB"
    log_info "CPU cores: $CPU_CORES"
    log_info "Configuration file: $REDIS_CONF_PATH"
    log_info "Backup file: ${REDIS_CONF_PATH}${BACKUP_SUFFIX}"
    echo
    log_warning "Please review the generated system tuning files and apply them if needed"
    log_warning "Restart Redis service to apply the new configuration"
}

# Run main function
main "$@"
