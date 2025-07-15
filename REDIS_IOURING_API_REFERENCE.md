# Redis io_uring API Reference

This document provides a comprehensive reference for the Redis io_uring implementation API, including configuration options, monitoring commands, and programming interfaces.

## Table of Contents

1. [Configuration API](#configuration-api)
2. [Monitoring API](#monitoring-api)
3. [Debug API](#debug-api)
4. [Programming Interface](#programming-interface)
5. [Statistics Reference](#statistics-reference)
6. [Error Codes](#error-codes)

## Configuration API

### Core Configuration Options

#### `uring-enabled <yes|no>`
**Default**: `no`  
**Description**: Enable or disable io_uring support.  
**Example**: `uring-enabled yes`

#### `uring-queue-depth <number>`
**Default**: `4096`  
**Range**: `64-32768`  
**Description**: Size of the io_uring submission and completion queues.  
**Example**: `uring-queue-depth 8192`

#### `uring-buffer-pool-size <number>`
**Default**: `16384`  
**Range**: `1024-65536`  
**Description**: Number of buffers in the buffer pool.  
**Example**: `uring-buffer-pool-size 32768`

#### `uring-buffer-size <bytes>`
**Default**: `16384`  
**Range**: `4096-65536`  
**Description**: Size of each buffer in bytes.  
**Example**: `uring-buffer-size 32768`

### Advanced Configuration Options

#### `uring-multishot-accept <yes|no>`
**Default**: `yes`  
**Description**: Enable multishot accept operations for better connection handling.  
**Example**: `uring-multishot-accept yes`

#### `uring-sqpoll <yes|no>`
**Default**: `no`  
**Description**: Enable SQPOLL mode for reduced system call overhead.  
**Example**: `uring-sqpoll yes`

#### `uring-sqpoll-idle-ms <milliseconds>`
**Default**: `10`  
**Range**: `1-1000`  
**Description**: SQPOLL idle timeout in milliseconds.  
**Example**: `uring-sqpoll-idle-ms 20`

#### `uring-sqpoll-cpu <number>`
**Default**: `0`  
**Range**: `0-<num_cpus-1>`  
**Description**: CPU core for SQPOLL thread.  
**Example**: `uring-sqpoll-cpu 2`

#### `uring-use-provided-buffers <yes|no>`
**Default**: `yes`  
**Description**: Use kernel-provided buffers for read operations.  
**Example**: `uring-use-provided-buffers yes`

#### `uring-provided-buffer-count <number>`
**Default**: `8192`  
**Range**: `1024-32768`  
**Description**: Number of provided buffers.  
**Example**: `uring-provided-buffer-count 16384`

#### `uring-use-registered-fds <yes|no>`
**Default**: `yes`  
**Description**: Use registered file descriptors for better performance.  
**Example**: `uring-use-registered-fds yes`

#### `uring-max-registered-fds <number>`
**Default**: `10000`  
**Range**: `1000-100000`  
**Description**: Maximum number of registered file descriptors.  
**Example**: `uring-max-registered-fds 20000`

## Monitoring API

### INFO Command Extensions

#### `INFO uring`
Returns comprehensive io_uring statistics and configuration information.

**Example Output**:
```
# io_uring
uring_enabled:1
uring_version:2.3
uring_ring_fd:5
uring_sq_entries:4096
uring_cq_entries:4096
uring_sqpoll_enabled:1
uring_sqpoll_cpu:0
uring_sqpoll_idle_ms:10
uring_ops_submitted:1234567
uring_ops_completed:1234560
uring_ops_failed:7
uring_ops_cancelled:0
uring_ops_timeout:0
uring_accept_ops:12345
uring_read_ops:567890
uring_write_ops:654321
uring_successful_accepts:12345
uring_successful_reads:567885
uring_successful_writes:654320
uring_batch_submissions:12345
uring_completion_batches:12340
uring_avg_completions_per_batch:100
uring_multishot_completions:5678
uring_buffer_pool_size:16384
uring_buffer_pool_used:1234
uring_buffer_pool_free:15150
uring_buffer_allocations:567890
uring_buffer_deallocations:566656
uring_provided_buffers_enabled:1
uring_provided_buffer_count:8192
uring_provided_buffer_size:16384
uring_registered_fds_enabled:1
uring_max_registered_fds:10000
uring_registered_fd_count:1234
uring_sq_full_errors:12
uring_submit_errors:5
uring_wait_errors:0
uring_memory_errors:0
uring_active_connections:1234
uring_max_connections:10000
uring_connection_closures:567
uring_bytes_read:123456789
uring_bytes_written:987654321
uring_capabilities:0x1f
uring_supports_multishot:1
uring_supports_fast_poll:1
uring_supports_sqpoll:1
```

## Debug API

### DEBUG Command Extensions

#### `DEBUG uring`
Returns detailed debug information about the current io_uring state.

**Example**:
```
redis-cli DEBUG uring
```

**Output**:
```
=== io_uring Debug Information ===
Ring FD: 5
SQ Entries: 4096
CQ Entries: 4096
Capabilities: 0x1f

Active Operations: 123
Active Connections: 1234/10000
Provided Buffers: 8192 x 16384 bytes
Registered FDs: 1234/10000

Performance Metrics:
  Operations submitted: 1234567
  Operations completed: 1234560
  Operations failed: 7
  Success rate: 99.99%
  Batch submissions: 12345
  Completion batches: 12340
```

#### `DEBUG uring reset-stats`
Resets all io_uring statistics counters.

**Example**:
```
redis-cli DEBUG uring reset-stats
```

#### `DEBUG uring info`
Returns the same information as `INFO uring` but in a more detailed format.

## Programming Interface

### Core Functions

#### `aeApiCreate(aeEventLoop *eventLoop)`
Initializes the io_uring event loop backend.

**Parameters**:
- `eventLoop`: Pointer to the Redis event loop structure

**Returns**: `0` on success, `-1` on error

#### `aeApiResize(aeEventLoop *eventLoop, int setsize)`
Resizes the io_uring queues and associated data structures.

**Parameters**:
- `eventLoop`: Pointer to the Redis event loop structure
- `setsize`: New maximum number of file descriptors

**Returns**: `0` on success, `-1` on error

#### `aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask)`
Adds a file descriptor to the io_uring monitoring set.

**Parameters**:
- `eventLoop`: Pointer to the Redis event loop structure
- `fd`: File descriptor to monitor
- `mask`: Event mask (`AE_READABLE`, `AE_WRITABLE`, or both)

**Returns**: `0` on success, `-1` on error

#### `aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask)`
Removes a file descriptor from the io_uring monitoring set.

**Parameters**:
- `eventLoop`: Pointer to the Redis event loop structure
- `fd`: File descriptor to stop monitoring
- `mask`: Event mask to remove

**Returns**: `0` on success, `-1` on error

#### `aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp)`
Polls for io_uring events and processes completions.

**Parameters**:
- `eventLoop`: Pointer to the Redis event loop structure
- `tvp`: Timeout value (can be NULL for blocking)

**Returns**: Number of events processed, `-1` on error

### Buffer Management Functions

#### `get_buffer_from_pool(uring_buffer_pool *pool)`
Allocates a buffer from the io_uring buffer pool.

**Parameters**:
- `pool`: Pointer to the buffer pool

**Returns**: Pointer to buffer on success, `NULL` on error

#### `return_buffer_to_pool(uring_buffer_pool *pool, void *buffer)`
Returns a buffer to the io_uring buffer pool.

**Parameters**:
- `pool`: Pointer to the buffer pool
- `buffer`: Buffer to return

### Operation Management Functions

#### `create_op_context(int fd, int op_type, int mask)`
Creates a new operation context for io_uring operations.

**Parameters**:
- `fd`: File descriptor
- `op_type`: Operation type (`URING_OP_ACCEPT`, `URING_OP_READ`, `URING_OP_WRITE`)
- `mask`: Event mask

**Returns**: Pointer to operation context on success, `NULL` on error

#### `submit_read_operation(aeApiState *state, int fd, uring_op_context *ctx)`
Submits a read operation to io_uring.

**Parameters**:
- `state`: io_uring API state
- `fd`: File descriptor
- `ctx`: Operation context

**Returns**: `0` on success, `-1` on error

#### `submit_write_operation(aeApiState *state, int fd, uring_op_context *ctx)`
Submits a write operation to io_uring.

**Parameters**:
- `state`: io_uring API state
- `fd`: File descriptor
- `ctx`: Operation context

**Returns**: `0` on success, `-1` on error

#### `submit_accept_operation(aeApiState *state, int fd, uring_op_context *ctx)`
Submits an accept operation to io_uring.

**Parameters**:
- `state`: io_uring API state
- `fd`: Listening socket file descriptor
- `ctx`: Operation context

**Returns**: `0` on success, `-1` on error

## Statistics Reference

### Operation Statistics

| Metric | Description | Type |
|--------|-------------|------|
| `ops_submitted` | Total operations submitted to io_uring | Counter |
| `ops_completed` | Total operations completed successfully | Counter |
| `ops_failed` | Total operations that failed | Counter |
| `ops_cancelled` | Total operations cancelled | Counter |
| `ops_timeout` | Total operations that timed out | Counter |

### Performance Statistics

| Metric | Description | Type |
|--------|-------------|------|
| `batch_submissions` | Number of batch submissions | Counter |
| `completion_batches` | Number of completion batches processed | Counter |
| `avg_completions_per_batch` | Average completions per batch | Gauge |
| `multishot_completions` | Multishot operation completions | Counter |

### Error Statistics

| Metric | Description | Type |
|--------|-------------|------|
| `sq_full_errors` | Submission queue full errors | Counter |
| `submit_errors` | Submission errors | Counter |
| `wait_errors` | Wait operation errors | Counter |
| `memory_errors` | Memory allocation errors | Counter |

### Buffer Statistics

| Metric | Description | Type |
|--------|-------------|------|
| `buffer_allocations` | Total buffer allocations | Counter |
| `buffer_deallocations` | Total buffer deallocations | Counter |
| `buffer_pool_used` | Currently used buffers | Gauge |
| `buffer_pool_free` | Currently free buffers | Gauge |

## Error Codes

### io_uring Specific Errors

| Code | Name | Description |
|------|------|-------------|
| `-EAGAIN` | `EAGAIN` | Resource temporarily unavailable |
| `-ENOSPC` | `ENOSPC` | No space left (queue full) |
| `-EINVAL` | `EINVAL` | Invalid argument |
| `-ENOMEM` | `ENOMEM` | Out of memory |
| `-ETIME` | `ETIME` | Operation timed out |
| `-ECANCELED` | `ECANCELED` | Operation cancelled |

### Redis Integration Errors

| Code | Description |
|------|-------------|
| `REDIS_ERR` | General Redis error |
| `REDIS_ERR_IO` | I/O operation failed |
| `REDIS_ERR_OTHER` | Other error condition |

---

This API reference is part of the Redis io_uring implementation project. For usage examples and best practices, see the training guide and implementation documentation.
