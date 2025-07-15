# How to Run Redis with io_uring - Complete Instructions

## 🚀 **Quick Start (3 Steps)**

### **Step 1: Enable io_uring in Redis**
```bash
# Method A: Command line (easiest)
./src/redis-server --uring-enabled yes --port 6379

# Method B: Configuration file
echo "uring-enabled yes" >> redis.conf
./src/redis-server redis.conf
```

### **Step 2: Verify io_uring is Working**
```bash
redis-cli INFO uring
# Expected output: uring_enabled:1
```

### **Step 3: Monitor Performance**
```bash
redis-cli --latency-history
redis-benchmark -t set,get -n 10000
```

---

## 📋 **Detailed Instructions**

### **Prerequisites Check**
```bash
# 1. Check kernel version (need 5.6+)
uname -r

# 2. Check liburing availability
pkg-config --modversion liburing

# 3. Verify Redis binary exists
ls -la src/redis-server
```

### **Method 1: Command Line (Recommended for Testing)**

```bash
# Basic io_uring enablement
./src/redis-server --uring-enabled yes

# With custom settings
./src/redis-server \
  --uring-enabled yes \
  --uring-queue-depth 4096 \
  --uring-buffer-pool-size 16384 \
  --port 6379

# Background mode
./src/redis-server \
  --uring-enabled yes \
  --daemonize yes \
  --pidfile /var/run/redis.pid
```

### **Method 2: Configuration File (Recommended for Production)**

Create `redis-uring.conf`:
```bash
cat > redis-uring.conf << EOF
# Basic Redis settings
port 6379
bind 127.0.0.1
daemonize yes
pidfile /var/run/redis.pid
logfile /var/log/redis.log

# Enable io_uring
uring-enabled yes
uring-queue-depth 4096
uring-buffer-pool-size 16384
uring-buffer-size 16384

# Advanced io_uring features
uring-multishot-accept yes
uring-use-provided-buffers yes
uring-provided-buffer-count 8192
uring-use-registered-fds yes
uring-max-registered-fds 10000

# SQPOLL (for high-throughput workloads)
uring-sqpoll no
uring-sqpoll-idle-ms 10
uring-sqpoll-cpu 0
EOF

# Start Redis with configuration
./src/redis-server redis-uring.conf
```

### **Method 3: Runtime Configuration**

```bash
# Start Redis normally
./src/redis-server --port 6379 &

# Enable io_uring at runtime
redis-cli CONFIG SET uring-enabled yes
redis-cli CONFIG SET uring-queue-depth 4096

# Save configuration
redis-cli CONFIG REWRITE
```

---

## ⚙️ **Configuration Options**

### **Basic Settings**
```bash
uring-enabled yes                    # Enable io_uring
uring-queue-depth 4096              # Queue size (1024-8192)
uring-buffer-pool-size 16384        # Buffer pool size
uring-buffer-size 16384             # Individual buffer size
```

### **Advanced Settings**
```bash
uring-multishot-accept yes          # Multishot accept operations
uring-use-provided-buffers yes      # Use provided buffer rings
uring-provided-buffer-count 8192    # Number of provided buffers
uring-use-registered-fds yes        # Register file descriptors
uring-max-registered-fds 10000      # Max registered FDs
```

### **SQPOLL Settings (High-Performance)**
```bash
uring-sqpoll yes                     # Enable SQPOLL mode
uring-sqpoll-idle-ms 10             # SQPOLL idle timeout
uring-sqpoll-cpu 0                  # CPU for SQPOLL thread
```

---

## 🎯 **Workload-Specific Configurations**

### **High Throughput Workload**
```bash
./src/redis-server \
  --uring-enabled yes \
  --uring-queue-depth 8192 \
  --uring-buffer-pool-size 32768 \
  --uring-sqpoll yes \
  --uring-multishot-accept yes \
  --uring-use-provided-buffers yes
```

### **Low Latency Workload**
```bash
./src/redis-server \
  --uring-enabled yes \
  --uring-queue-depth 2048 \
  --uring-buffer-pool-size 8192 \
  --uring-sqpoll no \
  --tcp-nodelay yes
```

### **Balanced Workload (Recommended)**
```bash
./src/redis-server \
  --uring-enabled yes \
  --uring-queue-depth 4096 \
  --uring-buffer-pool-size 16384 \
  --uring-multishot-accept yes \
  --uring-use-provided-buffers yes
```

---

## 🔍 **Verification and Monitoring**

### **Verify io_uring is Active**
```bash
# Check io_uring status
redis-cli INFO uring

# Expected output when working:
# uring_enabled:1
# uring_queue_depth:4096
# uring_buffer_pool_size:16384
# uring_operations_submitted:X
# uring_operations_completed:X
```

### **Monitor Performance**
```bash
# Real-time latency monitoring
redis-cli --latency-history -i 1

# Performance benchmarking
redis-benchmark -t set,get,incr -n 100000 -c 50

# System monitoring
iostat -x 1
top -p $(pgrep redis-server)
```

### **Debug Information**
```bash
# Detailed io_uring stats
redis-cli INFO uring

# General Redis stats
redis-cli INFO stats
redis-cli INFO memory

# Check for errors
redis-cli LASTSAVE
tail -f /var/log/redis.log
```

---

## 🛠️ **Troubleshooting**

### **Common Issues and Solutions**

#### **Issue: "uring-enabled not recognized"**
```bash
# Solution: Rebuild Redis with liburing
sudo apt-get install liburing-dev
make clean && make
```

#### **Issue: "Failed to initialize io_uring"**
```bash
# Check kernel version
uname -r  # Need 5.6+

# Check permissions
ulimit -n  # Should be > 1024

# Check system limits
cat /proc/sys/fs/file-max
```

#### **Issue: "Performance not improved"**
```bash
# Try different configurations
redis-cli CONFIG SET uring-queue-depth 8192
redis-cli CONFIG SET uring-sqpoll yes

# Monitor system resources
iostat -x 1
sar -u 1
```

### **Rollback Procedure**
```bash
# Disable io_uring
redis-cli CONFIG SET uring-enabled no

# Or restart without io_uring
./src/redis-server --uring-enabled no

# Or use backup configuration
cp redis.conf.backup redis.conf
systemctl restart redis
```

---

## 📊 **Performance Expectations**

### **Expected Improvements with io_uring**
- **Throughput**: 10-30% increase for high-concurrency workloads
- **Latency**: 5-15% reduction in P99 latency
- **CPU Usage**: 5-20% reduction in system CPU
- **Context Switches**: Significant reduction

### **Benchmark Comparison**
```bash
# Before enabling io_uring
redis-benchmark -t set,get -n 100000 -c 50 -q

# After enabling io_uring
redis-cli CONFIG SET uring-enabled yes
redis-benchmark -t set,get -n 100000 -c 50 -q
```

---

## 🚀 **Production Deployment**

### **Recommended Production Setup**
```bash
# 1. Create production configuration
cat > /etc/redis/redis.conf << EOF
# Production Redis with io_uring
port 6379
bind 0.0.0.0
protected-mode yes
requirepass your_secure_password

# io_uring configuration
uring-enabled yes
uring-queue-depth 4096
uring-buffer-pool-size 16384
uring-multishot-accept yes
uring-use-provided-buffers yes

# Production settings
daemonize yes
pidfile /var/run/redis/redis.pid
logfile /var/log/redis/redis.log
dir /var/lib/redis
save 900 1
save 300 10
save 60 10000
EOF

# 2. Start Redis
./src/redis-server /etc/redis/redis.conf

# 3. Verify
redis-cli -a your_secure_password INFO uring
```

### **Monitoring Setup**
```bash
# Add to monitoring script
#!/bin/bash
while true; do
    echo "$(date): $(redis-cli INFO uring | grep uring_enabled)"
    sleep 60
done > /var/log/redis-uring-monitor.log &
```

---

## 🎉 **Success Indicators**

You'll know io_uring is working when:
- ✅ `redis-cli INFO uring` shows `uring_enabled:1`
- ✅ Operations counters are incrementing
- ✅ Performance metrics show improvement
- ✅ No error messages in logs
- ✅ System CPU usage decreased

---

## 📞 **Quick Reference Commands**

```bash
# Start with io_uring
./src/redis-server --uring-enabled yes

# Check status
redis-cli INFO uring

# Enable at runtime
redis-cli CONFIG SET uring-enabled yes

# Disable if needed
redis-cli CONFIG SET uring-enabled no

# Monitor performance
redis-cli --latency-history

# Benchmark
redis-benchmark -t set,get -n 10000
```

**🎯 You're now ready to run Redis with io_uring for enhanced performance!**
