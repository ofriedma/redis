#!/bin/bash

# Redis io_uring Enablement Script
# Safely enables io_uring with proper validation

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
Redis io_uring Enablement Script

Usage: $0 [OPTIONS]

OPTIONS:
    -f, --config PATH       Path to redis.conf file (default: /etc/redis/redis.conf)
    -w, --workload TYPE     Workload optimization: high-throughput, low-latency, balanced
    -t, --test              Test mode - validate but don't restart Redis
    -h, --help              Show this help message

EXAMPLES:
    $0                      # Enable with balanced settings
    $0 -w high-throughput   # Optimize for high throughput
    $0 -w low-latency       # Optimize for low latency
    $0 -t                   # Test configuration without restart

EOF
}

# Parse command line arguments
WORKLOAD_TYPE="balanced"
TEST_MODE=false

while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--config)
            REDIS_CONF_PATH="$2"
            shift 2
            ;;
        -w|--workload)
            WORKLOAD_TYPE="$2"
            shift 2
            ;;
        -t|--test)
            TEST_MODE=true
            shift
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

# Check prerequisites
check_prerequisites() {
    log_info "Checking prerequisites..."
    
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
    if ! redis-server --help 2>&1 | grep -q "uring\|URING"; then
        log_warning "Redis may not be compiled with io_uring support"
    fi
    
    log_success "Prerequisites check completed"
}

# Backup configuration
backup_config() {
    if [ -f "$REDIS_CONF_PATH" ]; then
        log_info "Backing up configuration file..."
        cp "$REDIS_CONF_PATH" "${REDIS_CONF_PATH}${BACKUP_SUFFIX}"
        log_success "Configuration backed up to ${REDIS_CONF_PATH}${BACKUP_SUFFIX}"
    else
        log_warning "Configuration file not found: $REDIS_CONF_PATH"
    fi
}

# Set configuration value
set_config() {
    local key="$1"
    local value="$2"
    local config_file="$3"

    if grep -q "^${key}" "$config_file" 2>/dev/null; then
        sed -i "s/^${key}.*/${key} ${value}/" "$config_file"
    else
        echo "${key} ${value}" >> "$config_file"
    fi
}

# Apply io_uring configuration
apply_uring_config() {
    log_info "Applying io_uring configuration for workload: $WORKLOAD_TYPE"
    
    # Ensure config file exists
    if [ ! -f "$REDIS_CONF_PATH" ]; then
        log_info "Creating new configuration file: $REDIS_CONF_PATH"
        mkdir -p "$(dirname "$REDIS_CONF_PATH")"
        touch "$REDIS_CONF_PATH"
    fi
    
    # Enable io_uring
    set_config "uring-enabled" "yes" "$REDIS_CONF_PATH"
    
    case "$WORKLOAD_TYPE" in
        "high-throughput")
            log_info "Applying high-throughput optimizations..."
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
            ;;
        "low-latency")
            log_info "Applying low-latency optimizations..."
            set_config "uring-queue-depth" "2048" "$REDIS_CONF_PATH"
            set_config "uring-buffer-pool-size" "8192" "$REDIS_CONF_PATH"
            set_config "uring-buffer-size" "8192" "$REDIS_CONF_PATH"
            set_config "uring-multishot-accept" "yes" "$REDIS_CONF_PATH"
            set_config "uring-sqpoll" "no" "$REDIS_CONF_PATH"
            set_config "uring-use-provided-buffers" "yes" "$REDIS_CONF_PATH"
            set_config "uring-provided-buffer-count" "4096" "$REDIS_CONF_PATH"
            set_config "uring-use-registered-fds" "yes" "$REDIS_CONF_PATH"
            set_config "uring-max-registered-fds" "10000" "$REDIS_CONF_PATH"
            set_config "tcp-nodelay" "yes" "$REDIS_CONF_PATH"
            ;;
        "balanced"|*)
            log_info "Applying balanced configuration..."
            set_config "uring-queue-depth" "4096" "$REDIS_CONF_PATH"
            set_config "uring-buffer-pool-size" "16384" "$REDIS_CONF_PATH"
            set_config "uring-buffer-size" "16384" "$REDIS_CONF_PATH"
            set_config "uring-multishot-accept" "yes" "$REDIS_CONF_PATH"
            set_config "uring-sqpoll" "no" "$REDIS_CONF_PATH"
            set_config "uring-use-provided-buffers" "yes" "$REDIS_CONF_PATH"
            set_config "uring-provided-buffer-count" "8192" "$REDIS_CONF_PATH"
            set_config "uring-use-registered-fds" "yes" "$REDIS_CONF_PATH"
            set_config "uring-max-registered-fds" "10000" "$REDIS_CONF_PATH"
            ;;
    esac
    
    log_success "io_uring configuration applied"
}

# Validate configuration
validate_config() {
    log_info "Validating configuration..."
    
    if redis-server "$REDIS_CONF_PATH" --test-memory >/dev/null 2>&1; then
        log_success "Configuration validation passed"
        return 0
    else
        log_error "Configuration validation failed"
        return 1
    fi
}

# Test io_uring functionality
test_uring() {
    log_info "Testing io_uring functionality..."
    
    local port=6379
    local test_port=6500
    
    # Check if Redis is running on default port
    if pgrep -f "redis-server.*$port" >/dev/null; then
        test_port=6501
    fi
    
    log_info "Starting test Redis server on port $test_port..."
    timeout 30 redis-server "$REDIS_CONF_PATH" --port $test_port --daemonize yes >/dev/null 2>&1
    sleep 3
    
    if pgrep -f "redis-server.*$test_port" >/dev/null; then
        log_success "Test Redis server started"
        
        # Test basic functionality
        if echo "PING" | nc localhost $test_port 2>/dev/null | grep -q "PONG"; then
            log_success "Basic connectivity test passed"
        else
            log_error "Basic connectivity test failed"
            pkill -f "redis-server.*$test_port"
            return 1
        fi
        
        # Check io_uring status
        local uring_info=$(echo "INFO uring" | nc localhost $test_port 2>/dev/null)
        if echo "$uring_info" | grep -q "uring_enabled:1"; then
            log_success "io_uring is enabled and working"
        else
            log_warning "io_uring may not be fully functional"
        fi
        
        # Stop test server
        pkill -f "redis-server.*$test_port"
        log_success "Test completed successfully"
        return 0
    else
        log_error "Failed to start test Redis server"
        return 1
    fi
}

# Main function
main() {
    echo "Redis io_uring Enablement Script"
    echo "================================"
    echo "Workload: $WORKLOAD_TYPE"
    echo "Config: $REDIS_CONF_PATH"
    echo "Test Mode: $TEST_MODE"
    echo
    
    check_prerequisites
    backup_config
    apply_uring_config
    
    if ! validate_config; then
        log_error "Configuration validation failed"
        if [ -f "${REDIS_CONF_PATH}${BACKUP_SUFFIX}" ]; then
            log_info "Restoring backup configuration..."
            cp "${REDIS_CONF_PATH}${BACKUP_SUFFIX}" "$REDIS_CONF_PATH"
        fi
        exit 1
    fi
    
    if [ "$TEST_MODE" = true ]; then
        test_uring
        log_info "Test mode completed. Redis configuration updated but not restarted."
    else
        log_warning "Configuration updated. Please restart Redis to apply changes:"
        echo "  systemctl restart redis"
        echo "  # or"
        echo "  redis-cli SHUTDOWN"
        echo "  redis-server $REDIS_CONF_PATH"
    fi
    
    echo
    log_success "io_uring enablement completed!"
    log_info "Monitor with: redis-cli INFO uring"
    log_info "Backup saved: ${REDIS_CONF_PATH}${BACKUP_SUFFIX}"
}

# Run main function
main "$@"
