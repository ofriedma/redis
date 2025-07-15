# Redis io_uring Troubleshooting Playbook

This playbook provides step-by-step procedures for diagnosing and resolving common issues with Redis io_uring implementation.

## Table of Contents

1. [Quick Diagnostic Checklist](#quick-diagnostic-checklist)
2. [Common Issues and Solutions](#common-issues-and-solutions)
3. [Performance Issues](#performance-issues)
4. [Memory Issues](#memory-issues)
5. [Connection Issues](#connection-issues)
6. [System-Level Issues](#system-level-issues)
7. [Emergency Procedures](#emergency-procedures)
8. [Escalation Guidelines](#escalation-guidelines)

## Quick Diagnostic Checklist

When encountering issues with Redis io_uring, run through this checklist first:

### 1. Basic Health Check
```bash
# Check if Redis is running
systemctl status redis

# Check if io_uring is enabled
redis-cli INFO uring | grep uring_enabled

# Check for basic connectivity
redis-cli PING
```

### 2. System Requirements
```bash
# Kernel version (should be 5.6+)
uname -r

# liburing availability
pkg-config --modversion liburing

# System limits
cat /proc/sys/fs/aio-max-nr
ulimit -n
```

### 3. Error Indicators
```bash
# Check for errors in Redis logs
journalctl -u redis -n 100 | grep -i error

# Check system logs for io_uring issues
dmesg | grep -i uring

# Check io_uring error statistics
redis-cli INFO uring | grep error
```

## Common Issues and Solutions

### Issue 1: Redis Fails to Start with io_uring

**Symptoms:**
- Redis fails to start after enabling io_uring
- Error messages about io_uring initialization
- Service status shows failed state

**Diagnostic Steps:**
```bash
# Check Redis configuration
redis-server /etc/redis/redis.conf --test-memory

# Check kernel support
cat /proc/version | grep -E "5\.[6-9]|[6-9]\."

# Check liburing installation
ldconfig -p | grep uring
```

**Solutions:**
1. **Kernel too old:**
   ```bash
   # Upgrade kernel to 5.6 or newer
   sudo apt update && sudo apt upgrade linux-generic
   ```

2. **liburing missing:**
   ```bash
   # Install liburing
   sudo apt install liburing-dev  # Debian/Ubuntu
   sudo dnf install liburing-devel  # RHEL/CentOS
   ```

3. **Configuration error:**
   ```bash
   # Disable io_uring temporarily
   sed -i 's/uring-enabled yes/uring-enabled no/' /etc/redis/redis.conf
   systemctl start redis
   ```

### Issue 2: High Memory Usage

**Symptoms:**
- Redis RSS memory growing continuously
- Out of memory errors
- System becoming unresponsive

**Diagnostic Steps:**
```bash
# Check memory usage
redis-cli INFO memory

# Check io_uring buffer usage
redis-cli INFO uring | grep buffer

# Monitor memory growth
watch -n 1 'redis-cli INFO memory | grep used_memory_human'
```

**Solutions:**
1. **Reduce buffer pool size:**
   ```bash
   # Edit redis.conf
   uring-buffer-pool-size 8192  # Reduce from default 16384
   uring-buffer-size 8192       # Reduce from default 16384
   ```

2. **Disable provided buffers:**
   ```bash
   uring-use-provided-buffers no
   ```

3. **Monitor and tune:**
   ```bash
   # Reset statistics and monitor
   redis-cli DEBUG uring reset-stats
   # Monitor for buffer leaks
   watch -n 5 'redis-cli INFO uring | grep -E "(buffer_|memory_)"'
   ```

### Issue 3: Poor Performance

**Symptoms:**
- Lower throughput than expected
- Higher latency than epoll
- CPU usage higher than expected

**Diagnostic Steps:**
```bash
# Compare with epoll performance
redis-benchmark -t set,get -n 100000 -c 50

# Check io_uring statistics
redis-cli INFO uring | grep -E "(ops_|batch_|avg_)"

# Check for errors
redis-cli INFO uring | grep error
```

**Solutions:**
1. **Optimize queue depth:**
   ```bash
   # For high throughput
   uring-queue-depth 8192
   
   # For low latency
   uring-queue-depth 2048
   ```

2. **Tune SQPOLL settings:**
   ```bash
   # Enable for high throughput
   uring-sqpoll yes
   uring-sqpoll-idle-ms 10
   
   # Disable for low latency
   uring-sqpoll no
   ```

3. **Check system configuration:**
   ```bash
   # Increase system limits
   echo "fs.aio-max-nr = 1048576" >> /etc/sysctl.conf
   sysctl -p
   ```

## Performance Issues

### Throughput Lower Than Expected

**Investigation Steps:**
1. **Check batching efficiency:**
   ```bash
   redis-cli INFO uring | grep batch
   # Look for avg_completions_per_batch > 1
   ```

2. **Monitor queue utilization:**
   ```bash
   redis-cli DEBUG uring | grep "SQ Entries\|CQ Entries"
   ```

3. **Compare with baseline:**
   ```bash
   # Test with io_uring disabled
   redis-cli CONFIG SET uring-enabled no
   redis-benchmark -t set,get -n 100000 -c 50
   
   # Re-enable and test
   redis-cli CONFIG SET uring-enabled yes
   redis-benchmark -t set,get -n 100000 -c 50
   ```

**Optimization Actions:**
- Increase queue depth for high-throughput workloads
- Enable SQPOLL for CPU-bound scenarios
- Tune buffer sizes based on typical payload size

### High Latency Issues

**Investigation Steps:**
1. **Check tail latencies:**
   ```bash
   redis-cli --latency-history -i 1
   ```

2. **Monitor completion times:**
   ```bash
   redis-cli INFO uring | grep completion
   ```

**Optimization Actions:**
- Disable SQPOLL for latency-sensitive workloads
- Reduce queue depth
- Enable TCP_NODELAY

## Memory Issues

### Memory Leaks

**Detection:**
```bash
# Monitor memory growth over time
while true; do
  echo "$(date): $(redis-cli INFO memory | grep used_memory_human)"
  sleep 60
done
```

**Investigation:**
```bash
# Check buffer pool statistics
redis-cli INFO uring | grep -E "(buffer_allocations|buffer_deallocations)"

# Look for growing differences
redis-cli DEBUG uring reset-stats
# Wait and check again
redis-cli INFO uring | grep buffer
```

**Resolution:**
1. **Restart Redis if critical:**
   ```bash
   systemctl restart redis
   ```

2. **Reduce buffer usage:**
   ```bash
   uring-buffer-pool-size 4096
   uring-use-provided-buffers no
   ```

### Buffer Pool Exhaustion

**Symptoms:**
- `buffer_pool_free` approaching 0
- Increased `buffer_allocation` errors

**Resolution:**
```bash
# Increase buffer pool size
uring-buffer-pool-size 32768

# Or reduce buffer size to fit more buffers
uring-buffer-size 8192
```

## Connection Issues

### Connection Drops

**Investigation:**
```bash
# Check connection statistics
redis-cli INFO uring | grep connection

# Monitor active connections
watch -n 1 'redis-cli INFO clients | grep connected_clients'

# Check for accept errors
redis-cli INFO uring | grep accept
```

**Solutions:**
1. **Increase connection limits:**
   ```bash
   maxclients 100000
   tcp-backlog 65536
   ```

2. **Optimize accept handling:**
   ```bash
   uring-multishot-accept yes
   ```

### Slow Connection Establishment

**Investigation:**
```bash
# Check accept operation statistics
redis-cli INFO uring | grep accept

# Monitor TCP backlog
ss -lnt | grep :6379
```

**Solutions:**
- Increase TCP backlog
- Enable multishot accept
- Check system TCP settings

## System-Level Issues

### Kernel Compatibility

**Check kernel features:**
```bash
# Check io_uring support
cat /proc/version
grep -r URING /boot/config-$(uname -r)
```

**Upgrade if needed:**
```bash
# Ubuntu/Debian
sudo apt update && sudo apt upgrade linux-generic

# RHEL/CentOS
sudo dnf update kernel
```

### System Resource Limits

**Check and adjust limits:**
```bash
# File descriptor limits
echo "fs.file-max = 1000000" >> /etc/sysctl.conf

# io_uring limits
echo "fs.aio-max-nr = 1048576" >> /etc/sysctl.conf

# Apply changes
sysctl -p
```

## Emergency Procedures

### Immediate Rollback

If critical issues occur in production:

```bash
# 1. Disable io_uring immediately
redis-cli CONFIG SET uring-enabled no

# 2. If that fails, restart with modified config
systemctl stop redis
sed -i 's/uring-enabled yes/uring-enabled no/' /etc/redis/redis.conf
systemctl start redis

# 3. Verify service is working
redis-cli PING
redis-cli INFO replication
```

### Data Recovery

If data corruption is suspected:

```bash
# 1. Stop Redis
systemctl stop redis

# 2. Check data integrity
redis-check-rdb /var/lib/redis/dump.rdb
redis-check-aof /var/lib/redis/appendonly.aof

# 3. Restore from backup if needed
cp /backup/dump.rdb /var/lib/redis/
chown redis:redis /var/lib/redis/dump.rdb

# 4. Start with io_uring disabled
echo "uring-enabled no" >> /etc/redis/redis.conf
systemctl start redis
```

### Service Recovery

Complete service recovery procedure:

```bash
# 1. Use rollback script
./scripts/rollback-uring.sh -t disable

# 2. Verify service health
redis-cli INFO server
redis-cli INFO replication

# 3. Monitor for stability
watch -n 5 'redis-cli INFO stats | grep -E "(total_commands|total_connections)"'
```

## Escalation Guidelines

### When to Escalate

Escalate to senior engineers when:
- Data corruption is suspected
- Multiple rollback attempts fail
- System-wide performance degradation
- Security implications identified

### Information to Collect

Before escalating, gather:

```bash
# System information
uname -a > system_info.txt
cat /proc/version >> system_info.txt
lscpu >> system_info.txt

# Redis configuration and status
redis-cli CONFIG GET "*" > redis_config.txt
redis-cli INFO ALL > redis_info.txt
redis-cli DEBUG uring > uring_debug.txt

# System logs
journalctl -u redis -n 1000 > redis_logs.txt
dmesg | grep -i uring > kernel_logs.txt

# Performance data
redis-benchmark -t set,get -n 10000 -c 10 > benchmark.txt
```

### Contact Information

- **Level 1**: Operations team
- **Level 2**: Senior Redis engineers
- **Level 3**: io_uring implementation team
- **Emergency**: On-call engineer

---

This troubleshooting playbook is part of the Redis io_uring implementation project. Keep this document updated with new issues and solutions as they are discovered.
