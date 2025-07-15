#!/bin/bash

# Redis io_uring Rollback Script
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
REDIS_SERVICE="redis"
ROLLBACK_TYPE=""
BACKUP_FILE=""

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
Redis io_uring Rollback Script

Usage: $0 [OPTIONS]

OPTIONS:
    -t, --type TYPE         Rollback type: disable, partial, full
    -b, --backup FILE       Backup configuration file to restore
    -f, --config PATH       Path to redis.conf file (default: /etc/redis/redis.conf)
    -s, --service NAME      Redis service name (default: redis)
    -h, --help              Show this help message

ROLLBACK TYPES:
    disable                 Disable io_uring, keep other optimizations
    partial                 Disable advanced io_uring features only
    full                    Restore from backup configuration file

EXAMPLES:
    $0 -t disable
    $0 -t partial
    $0 -t full -b /etc/redis/redis.conf.backup.20231215_143022

EOF
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -t|--type)
                ROLLBACK_TYPE="$2"
                shift 2
                ;;
            -b|--backup)
                BACKUP_FILE="$2"
                shift 2
                ;;
            -f|--config)
                REDIS_CONF_PATH="$2"
                shift 2
                ;;
            -s|--service)
                REDIS_SERVICE="$2"
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
    if [[ -z "$ROLLBACK_TYPE" ]]; then
        log_error "Rollback type is required"
        show_help
        exit 1
    fi

    if [[ "$ROLLBACK_TYPE" != "disable" && "$ROLLBACK_TYPE" != "partial" && "$ROLLBACK_TYPE" != "full" ]]; then
        log_error "Invalid rollback type: $ROLLBACK_TYPE"
        show_help
        exit 1
    fi

    if [[ "$ROLLBACK_TYPE" == "full" && -z "$BACKUP_FILE" ]]; then
        log_error "Backup file is required for full rollback"
        show_help
        exit 1
    fi

    if [[ ! -f "$REDIS_CONF_PATH" ]]; then
        log_error "Redis configuration file not found: $REDIS_CONF_PATH"
        exit 1
    fi

    if [[ "$ROLLBACK_TYPE" == "full" && ! -f "$BACKUP_FILE" ]]; then
        log_error "Backup file not found: $BACKUP_FILE"
        exit 1
    fi
}

# Check Redis status
check_redis_status() {
    if systemctl is-active --quiet "$REDIS_SERVICE"; then
        log_info "Redis service is running"
        return 0
    else
        log_warning "Redis service is not running"
        return 1
    fi
}

# Stop Redis service
stop_redis() {
    log_info "Stopping Redis service..."
    if systemctl stop "$REDIS_SERVICE"; then
        log_success "Redis service stopped"
    else
        log_error "Failed to stop Redis service"
        exit 1
    fi
}

# Start Redis service
start_redis() {
    log_info "Starting Redis service..."
    if systemctl start "$REDIS_SERVICE"; then
        log_success "Redis service started"
        
        # Wait for Redis to be ready
        local retries=30
        while [[ $retries -gt 0 ]]; do
            if redis-cli ping >/dev/null 2>&1; then
                log_success "Redis is responding to commands"
                return 0
            fi
            sleep 1
            ((retries--))
        done
        
        log_error "Redis is not responding after startup"
        return 1
    else
        log_error "Failed to start Redis service"
        exit 1
    fi
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

# Remove configuration line
remove_config() {
    local key="$1"
    local config_file="$2"
    
    sed -i "/^${key}/d" "$config_file"
}

# Disable io_uring completely
disable_uring() {
    log_info "Disabling io_uring completely..."
    
    # Create backup before changes
    local backup_file="${REDIS_CONF_PATH}.rollback.$(date +%Y%m%d_%H%M%S)"
    cp "$REDIS_CONF_PATH" "$backup_file"
    log_info "Configuration backed up to $backup_file"
    
    # Disable io_uring
    set_config "uring-enabled" "no" "$REDIS_CONF_PATH"
    
    # Comment out all io_uring related settings
    sed -i 's/^uring-/#uring-/g' "$REDIS_CONF_PATH"
    
    log_success "io_uring disabled"
}

# Partial rollback - disable advanced features
partial_rollback() {
    log_info "Performing partial rollback - disabling advanced io_uring features..."
    
    # Create backup before changes
    local backup_file="${REDIS_CONF_PATH}.partial_rollback.$(date +%Y%m%d_%H%M%S)"
    cp "$REDIS_CONF_PATH" "$backup_file"
    log_info "Configuration backed up to $backup_file"
    
    # Disable advanced features but keep basic io_uring
    set_config "uring-sqpoll" "no" "$REDIS_CONF_PATH"
    set_config "uring-use-provided-buffers" "no" "$REDIS_CONF_PATH"
    set_config "uring-use-registered-fds" "no" "$REDIS_CONF_PATH"
    set_config "uring-multishot-accept" "no" "$REDIS_CONF_PATH"
    
    # Reduce queue depth and buffer sizes
    set_config "uring-queue-depth" "1024" "$REDIS_CONF_PATH"
    set_config "uring-buffer-pool-size" "4096" "$REDIS_CONF_PATH"
    set_config "uring-buffer-size" "8192" "$REDIS_CONF_PATH"
    
    log_success "Advanced io_uring features disabled"
}

# Full rollback from backup
full_rollback() {
    log_info "Performing full rollback from backup file..."
    
    # Validate backup file
    if ! redis-server "$BACKUP_FILE" --test-memory >/dev/null 2>&1; then
        log_error "Backup configuration file is invalid"
        exit 1
    fi
    
    # Create backup of current config
    local current_backup="${REDIS_CONF_PATH}.before_rollback.$(date +%Y%m%d_%H%M%S)"
    cp "$REDIS_CONF_PATH" "$current_backup"
    log_info "Current configuration backed up to $current_backup"
    
    # Restore from backup
    cp "$BACKUP_FILE" "$REDIS_CONF_PATH"
    log_success "Configuration restored from $BACKUP_FILE"
}

# Verify configuration
verify_config() {
    log_info "Verifying configuration..."
    
    if redis-server "$REDIS_CONF_PATH" --test-memory >/dev/null 2>&1; then
        log_success "Configuration is valid"
        return 0
    else
        log_error "Configuration is invalid"
        return 1
    fi
}

# Test Redis functionality
test_redis_functionality() {
    log_info "Testing Redis functionality..."
    
    # Basic connectivity test
    if ! redis-cli ping >/dev/null 2>&1; then
        log_error "Redis is not responding to PING"
        return 1
    fi
    
    # Basic operations test
    local test_key="rollback_test_$(date +%s)"
    local test_value="test_value"
    
    if redis-cli set "$test_key" "$test_value" >/dev/null 2>&1; then
        if [[ "$(redis-cli get "$test_key")" == "$test_value" ]]; then
            redis-cli del "$test_key" >/dev/null 2>&1
            log_success "Basic Redis operations working"
            return 0
        fi
    fi
    
    log_error "Basic Redis operations failed"
    return 1
}

# Check io_uring status
check_uring_status() {
    log_info "Checking io_uring status..."
    
    local uring_info=$(redis-cli INFO | grep uring_enabled || echo "uring_enabled:unknown")
    log_info "Current status: $uring_info"
    
    if [[ "$uring_info" == *"uring_enabled:1"* ]]; then
        log_info "io_uring is currently enabled"
        return 0
    elif [[ "$uring_info" == *"uring_enabled:0"* ]]; then
        log_info "io_uring is currently disabled"
        return 1
    else
        log_warning "io_uring status unknown"
        return 2
    fi
}

# Generate rollback report
generate_rollback_report() {
    local report_file="/tmp/redis-uring-rollback-report.txt"
    
    cat > "$report_file" << EOF
Redis io_uring Rollback Report
==============================

Date: $(date)
Rollback Type: $ROLLBACK_TYPE
Configuration File: $REDIS_CONF_PATH
$([ -n "$BACKUP_FILE" ] && echo "Backup File: $BACKUP_FILE")

Pre-Rollback Status:
$(redis-cli INFO | grep uring || echo "io_uring info not available")

Post-Rollback Status:
$(redis-cli INFO | grep uring || echo "io_uring info not available")

Redis Service Status: $(systemctl is-active "$REDIS_SERVICE")

Rollback completed successfully at $(date)
EOF

    log_success "Rollback report generated: $report_file"
}

# Main function
main() {
    echo "Redis io_uring Rollback Script"
    echo "=============================="
    echo

    parse_args "$@"
    validate_args

    log_info "Starting rollback process..."
    log_info "Rollback type: $ROLLBACK_TYPE"
    
    # Check current status
    local redis_was_running=false
    if check_redis_status; then
        redis_was_running=true
    fi
    
    if [[ "$redis_was_running" == true ]]; then
        check_uring_status
    fi
    
    # Stop Redis if running
    if [[ "$redis_was_running" == true ]]; then
        stop_redis
    fi
    
    # Perform rollback based on type
    case "$ROLLBACK_TYPE" in
        "disable")
            disable_uring
            ;;
        "partial")
            partial_rollback
            ;;
        "full")
            full_rollback
            ;;
    esac
    
    # Verify configuration
    if ! verify_config; then
        log_error "Configuration verification failed"
        exit 1
    fi
    
    # Start Redis if it was running
    if [[ "$redis_was_running" == true ]]; then
        if start_redis; then
            test_redis_functionality
            check_uring_status
        else
            log_error "Failed to start Redis after rollback"
            exit 1
        fi
    fi
    
    # Generate report
    if [[ "$redis_was_running" == true ]]; then
        generate_rollback_report
    fi
    
    echo
    log_success "Rollback completed successfully!"
    log_info "Rollback type: $ROLLBACK_TYPE"
    
    if [[ "$ROLLBACK_TYPE" == "disable" ]]; then
        log_warning "io_uring has been completely disabled"
    elif [[ "$ROLLBACK_TYPE" == "partial" ]]; then
        log_warning "Advanced io_uring features have been disabled"
    elif [[ "$ROLLBACK_TYPE" == "full" ]]; then
        log_warning "Configuration has been restored from backup"
    fi
    
    echo
    log_info "Please monitor Redis performance and stability"
    log_info "Check logs for any issues: journalctl -u $REDIS_SERVICE -f"
}

# Run main function
main "$@"
