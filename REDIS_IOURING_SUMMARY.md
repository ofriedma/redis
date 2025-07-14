# Redis io_uring Migration - Executive Summary

## Overview

This document outlines a complete architectural transformation of Redis from epoll-based event handling to a pure io_uring completion-based system. The design eliminates traditional event masks and wake-up methods in favor of completion queue event (CQE) processing with SQPOLL kernel-side polling.

## Key Architectural Changes

### 1. Event System Elimination
- **Remove**: AE_READABLE/AE_WRITABLE event masks
- **Remove**: Traditional event handlers and wake-up mechanisms  
- **Replace**: Pure completion-based operation processing

### 2. Operation-Centric Design
- **Accept Operations**: Multishot accept for continuous connection handling
- **Read Operations**: Persistent read operations that auto-resubmit
- **Write Operations**: Queued write operations with completion callbacks
- **NOP Operations**: Used for event loop wake-up instead of eventfd

### 3. SQPOLL Integration
- **Kernel-side polling**: Eliminates most system calls
- **CPU affinity**: Dedicated CPU core for SQPOLL thread
- **Idle timeout**: Configurable sleep when no operations pending
- **Wake-up mechanism**: NOP operations trigger SQPOLL activity

## Core Benefits

### Performance Improvements
- **Reduced System Calls**: SQPOLL handles I/O in kernel space
- **Lower Latency**: Direct kernel-to-userspace completion notification
- **Better Scalability**: More efficient handling of many connections
- **CPU Efficiency**: Reduced context switches and interrupts

### Operational Benefits
- **Zero-copy I/O**: Advanced buffer management with provided buffers
- **Linked Operations**: Chain related I/O operations for efficiency
- **Comprehensive Monitoring**: Detailed statistics and health checks
- **Graceful Fallback**: Automatic fallback to epoll when needed

## Implementation Strategy

### Phase 1: Infrastructure (Weeks 1-2)
```c
// New operation tracking instead of event masks
typedef struct uring_operation {
    int fd;
    int op_type;                    // ACCEPT, READ, WRITE, NOP
    void (*completion_handler)(struct uring_operation *op, int result);
    int persistent;                 // Auto-resubmit flag
    void *buffer;
    size_t buffer_size;
} uring_operation;
```

### Phase 2: Core Operations (Weeks 3-4)
```c
// Replace aeApiPoll with pure CQE processing
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    
    // Submit pending operations (SQPOLL handles them)
    io_uring_submit(&state->ring);
    
    // Wait for completions only
    int ret = io_uring_wait_cqe_timeout(&state->ring, &cqe, timeout_ptr);
    
    // Process all available completions
    io_uring_for_each_cqe(&state->ring, head, cqe) {
        process_completion(eventLoop, cqe, &numevents);
    }
    
    return numevents;
}
```

### Phase 3: Advanced Features (Weeks 5-6)
- Multishot operations for continuous accept/recv
- Linked operations for request-response patterns
- Provided buffer rings for zero-copy I/O
- Advanced error handling and recovery

### Phase 4: Testing & Deployment (Weeks 7-8)
- Comprehensive testing with Redis workloads
- Performance benchmarking vs epoll
- Production readiness validation

## Configuration

### New Configuration Options
```conf
# io_uring configuration
uring-enabled yes
uring-sqpoll yes                 # Enable kernel-side polling
uring-sqpoll-cpu -1              # Auto-detect optimal CPU
uring-sqpoll-idle 1000           # 1 second idle timeout
uring-sq-entries 1024            # Submission queue size
uring-cq-entries 2048            # Completion queue size (2x SQ)
uring-buffer-pool-size 2048      # Buffer pool size
uring-buffer-size 4096           # Individual buffer size
uring-multishot-accept yes       # Continuous accept operations
uring-linked-ops yes             # Chain related operations
```

### Automatic Detection
```c
// Runtime capability detection
static int detect_optimal_uring_config(uring_config *config) {
    // Detect kernel version and features
    // Configure based on available capabilities
    // Set optimal parameters for hardware
}
```

## Connection Lifecycle Changes

### Traditional epoll Flow
```
Listen Socket → epoll_wait → EPOLLIN → accept() → 
New Client FD → epoll_ctl(ADD) → epoll_wait → EPOLLIN → read()
```

### New io_uring Flow  
```
Listen Socket → submit_multishot_accept() → 
CQE(new_client_fd) → submit_persistent_read(client_fd) →
CQE(data_available) → process_data() → submit_write() → CQE(write_complete)
```

## Memory Management

### Advanced Buffer Pool
```c
typedef struct uring_buffer_pool {
    void **buffers;
    atomic_int head, tail;          // Lock-free ring buffer
    int min_buffers, max_buffers;   // Dynamic sizing
    atomic_uint64_t pool_hits;      // Performance metrics
    atomic_uint64_t pool_misses;
} uring_buffer_pool;
```

### Zero-Copy Operations
- Provided buffer rings for kernel-managed buffers
- Direct buffer selection by kernel
- Reduced memory copying in I/O path

## Error Handling & Reliability

### Comprehensive Error Recovery
```c
static void handle_operation_error(uring_operation *op, int error) {
    switch (error) {
        case -EAGAIN: /* Resubmit with backoff */
        case -ECONNRESET: /* Clean up connection */
        case -ENOMEM: /* Reduce buffer pool size */
        default: /* Log and stop operation */
    }
}
```

### Health Monitoring
- Completion rate tracking
- Error rate monitoring  
- SQPOLL responsiveness checks
- Automatic fallback triggers

### Graceful Fallback
```c
static int fallback_to_epoll(aeEventLoop *eventLoop, const char *reason) {
    serverLog(LL_WARNING, "io_uring fallback to epoll: %s", reason);
    // Clean up io_uring state
    // Reinitialize with epoll
    // Update server configuration
}
```

## Performance Expectations

### Benchmarking Targets
- **Latency**: 20-30% reduction in P99 latency
- **Throughput**: 15-25% increase in operations/second
- **CPU Usage**: 10-20% reduction in CPU utilization
- **System Calls**: 80-90% reduction in syscall count

### Monitoring Metrics
```c
typedef struct uring_stats {
    atomic_uint64_t ops_submitted;
    atomic_uint64_t ops_completed;
    atomic_uint64_t ops_failed;
    atomic_uint64_t sqpoll_wakeups;
    atomic_uint64_t buffer_pool_hits;
    atomic_uint64_t avg_completion_time_us;
} uring_stats;
```

## Risk Mitigation

### Compatibility Concerns
- **Kernel Requirements**: Linux 5.1+ for basic io_uring, 5.19+ for advanced features
- **Library Dependencies**: liburing for userspace interface
- **Fallback Strategy**: Automatic detection and graceful degradation to epoll

### Testing Strategy
- Unit tests for all operation types
- Integration tests with Redis test suite
- Performance regression testing
- Stress testing under high load
- Compatibility testing across kernel versions

## Conclusion

This design represents a fundamental shift from event-driven to completion-driven I/O in Redis. By eliminating traditional event handling and leveraging io_uring's advanced features like SQPOLL and multishot operations, Redis will achieve significantly better performance on modern Linux systems while maintaining backward compatibility through intelligent fallback mechanisms.

The phased implementation approach ensures thorough testing and validation at each stage, minimizing risk while maximizing the performance benefits of this architectural transformation.
