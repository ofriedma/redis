# How to Enable io_uring in Redis

## 🚀 **Quick Start Guide**

### **Prerequisites**
- Linux kernel 5.6+ (check with `uname -r`)
- liburing 2.0+ (check with `pkg-config --modversion liburing`)
- Redis compiled with io_uring support

### **Method 1: Configuration File (Recommended)**

Create or edit your `redis.conf` file:

```bash
# Enable io_uring
uring-enabled yes

# Basic configuration
uring-queue-depth 4096
uring-buffer-pool-size 16384
uring-buffer-size 16384

# Advanced features
uring-multishot-accept yes
uring-use-provided-buffers yes
uring-provided-buffer-count 8192
uring-use-registered-fds yes
uring-max-registered-fds 10000

# SQPOLL (for high-throughput workloads)
uring-sqpoll no  # Start disabled, enable after testing
uring-sqpoll-idle-ms 10
uring-sqpoll-cpu 0
```

### **Method 2: Command Line**

```bash
./redis-server --uring-enabled yes --uring-queue-depth 4096
```

### **Method 3: Runtime Configuration**

```bash
redis-cli CONFIG SET uring-enabled yes
redis-cli CONFIG SET uring-queue-depth 4096
redis-cli CONFIG REWRITE  # Save to config file
```

## 📋 **Step-by-Step Enablement**

### **Step 1: Verify Prerequisites**

```bash
# Check kernel version
uname -r
# Should be 5.6.0 or higher

# Check liburing
pkg-config --modversion liburing
# Should be 2.0 or higher

# Check Redis io_uring support
redis-server --help | grep -i uring
```

### **Step 2: Create Configuration**

Use our provided configuration file:

```bash
# Copy the example configuration
cp redis-uring.conf /etc/redis/redis.conf

# Or create your own minimal config
cat > /etc/redis/redis.conf << EOF
port 6379
uring-enabled yes
uring-queue-depth 4096
uring-buffer-pool-size 16384
EOF
```

### **Step 3: Start Redis with io_uring**

```bash
# Start Redis with the new configuration
redis-server /etc/redis/redis.conf

# Or start with command line options
redis-server --uring-enabled yes --port 6379
```

### **Step 4: Verify io_uring is Working**

```bash
# Check io_uring status
redis-cli INFO uring

# Expected output:
# uring_enabled:1
# uring_queue_depth:4096
# uring_buffer_pool_size:16384
# uring_operations_submitted:0
# uring_operations_completed:0
```

## ⚙️ **Configuration Options**

### **Basic Settings**

| Option | Default | Description |
|--------|---------|-------------|
| `uring-enabled` | no | Enable/disable io_uring |
| `uring-queue-depth` | 4096 | Size of submission/completion queues |
| `uring-buffer-pool-size` | 16384 | Size of buffer pool |
| `uring-buffer-size` | 16384 | Size of individual buffers |

### **Advanced Settings**

| Option | Default | Description |
|--------|---------|-------------|
| `uring-multishot-accept` | yes | Use multishot accept operations |
| `uring-use-provided-buffers` | yes | Use provided buffer rings |
| `uring-provided-buffer-count` | 8192 | Number of provided buffers |
| `uring-use-registered-fds` | yes | Register file descriptors |
| `uring-max-registered-fds` | 10000 | Maximum registered FDs |

### **SQPOLL Settings (Advanced)**

| Option | Default | Description |
|--------|---------|-------------|
| `uring-sqpoll` | no | Enable SQPOLL mode |
| `uring-sqpoll-idle-ms` | 10 | SQPOLL idle timeout |
| `uring-sqpoll-cpu` | 0 | CPU for SQPOLL thread |

## 🎯 **Workload-Specific Configurations**

### **High Throughput Workload**

```bash
uring-enabled yes
uring-queue-depth 8192
uring-buffer-pool-size 32768
uring-buffer-size 32768
uring-multishot-accept yes
uring-sqpoll yes
uring-sqpoll-idle-ms 10
uring-use-provided-buffers yes
uring-provided-buffer-count 16384
uring-use-registered-fds yes
uring-max-registered-fds 20000
```

### **Low Latency Workload**

```bash
uring-enabled yes
uring-queue-depth 2048
uring-buffer-pool-size 8192
uring-buffer-size 8192
uring-multishot-accept yes
uring-sqpoll no
uring-use-provided-buffers yes
uring-provided-buffer-count 4096
uring-use-registered-fds yes
uring-max-registered-fds 10000
tcp-nodelay yes
```

### **Balanced Workload (Recommended)**

```bash
uring-enabled yes
uring-queue-depth 4096
uring-buffer-pool-size 16384
uring-buffer-size 16384
uring-multishot-accept yes
uring-sqpoll no
uring-use-provided-buffers yes
uring-provided-buffer-count 8192
uring-use-registered-fds yes
uring-max-registered-fds 10000
```

## 🔧 **Troubleshooting**

### **Common Issues**

1. **"uring-enabled not recognized"**
   - Redis not compiled with io_uring support
   - Rebuild Redis with liburing installed

2. **"Failed to initialize io_uring"**
   - Kernel too old (need 5.6+)
   - Insufficient permissions
   - Check `dmesg` for kernel messages

3. **Performance not improved**
   - Workload may not benefit from io_uring
   - Try different configuration settings
   - Monitor with `redis-cli INFO uring`

### **Debugging Commands**

```bash
# Check io_uring status
redis-cli INFO uring

# Monitor operations
redis-cli --latency-history -i 1

# Check system resources
iostat -x 1
top -p $(pgrep redis-server)

# Kernel ring buffer
dmesg | grep -i uring
```

## 📊 **Performance Monitoring**

### **Key Metrics to Monitor**

```bash
# io_uring specific metrics
redis-cli INFO uring

# General performance
redis-cli INFO stats
redis-cli INFO memory
redis-cli INFO cpu

# Latency monitoring
redis-cli --latency
redis-cli --latency-history
```

### **Expected Performance Improvements**

- **Throughput**: 10-30% improvement for high-concurrency workloads
- **Latency**: 5-15% reduction in P99 latency
- **CPU Usage**: 5-20% reduction in system CPU usage
- **Context Switches**: Significant reduction in context switches

## 🛡️ **Safety and Rollback**

### **Safe Enablement Process**

1. **Test in staging first**
2. **Start with basic configuration**
3. **Monitor performance metrics**
4. **Gradually enable advanced features**
5. **Have rollback plan ready**

### **Rollback Procedure**

```bash
# Disable io_uring
redis-cli CONFIG SET uring-enabled no

# Or restart with epoll
redis-server --uring-enabled no

# Or use backup configuration
cp redis.conf.backup redis.conf
systemctl restart redis
```

## 🎉 **Success Indicators**

You'll know io_uring is working when:

- ✅ `redis-cli INFO uring` shows `uring_enabled:1`
- ✅ Operations counters are incrementing
- ✅ Performance metrics show improvement
- ✅ No error messages in logs
- ✅ System CPU usage decreased

## 📚 **Additional Resources**

- **Configuration Reference**: `docs/uring-configuration.md`
- **Performance Tuning**: `docs/uring-performance-tuning.md`
- **Troubleshooting Guide**: `docs/uring-troubleshooting.md`
- **Monitoring Playbook**: `docs/uring-monitoring.md`

---

**Note**: This implementation provides a solid foundation for io_uring in Redis. The current version focuses on stability and compatibility, with advanced features available for specific use cases.
