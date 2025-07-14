# Redis io_uring Migration Design Document

## Executive Summary

This document outlines the design for migrating Redis from epoll-based event handling to io_uring, eliminating traditional event-driven architecture in favor of completion-based I/O. The design focuses on using only io_uring completion queue events (CQE) for all I/O operations, implementing SQPOLL for kernel-side polling, and using accept, read, and write operations for network I/O.

## Current Architecture Analysis

### Existing Event Loop (ae.c)
- **Event-driven model**: Uses epoll to monitor file descriptors for readability/writability
- **Event types**: AE_READABLE, AE_WRITABLE, AE_TIME_EVENTS
- **Main loop**: `aeMain()` → `aeProcessEvents()` → `aeApiPoll()` (epoll_wait)
- **Event handlers**: Callback functions triggered on FD events

### Current io_uring Implementation
Redis already has a partial io_uring implementation in `ae_uring.c` that:
- Maintains event-driven compatibility
- Uses io_uring operations but still relies on event masks
- Implements SQPOLL support with wake-up mechanisms
- Provides buffer management and operation contexts

## New Design: Pure io_uring Architecture

### Core Principles

1. **No Event Masks**: Eliminate AE_READABLE/AE_WRITABLE event tracking
2. **CQE-Only Processing**: All I/O operations driven by completion queue entries
3. **Continuous Operations**: Submit persistent operations that auto-resubmit
4. **SQPOLL Thread**: Kernel-side polling with minimal userspace intervention
5. **Auto-Wake SQPOLL**: SQPOLL automatically wakes on new submissions

### Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    Redis Main Thread                        │
├─────────────────────────────────────────────────────────────┤
│  aeMain() → aeProcessEvents() → aeApiPoll()                 │
│                        │                                    │
│                        ▼                                    │
│              io_uring_wait_cqe()                           │
│                        │                                    │
│                        ▼                                    │
│              Process CQE Completions                       │
│              ┌─────────┬─────────┬─────────┐           │
│              │ ACCEPT  │  READ   │  WRITE  │           │
│              └─────────┴─────────┴─────────┘           │
└─────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────┐
│                  Kernel SQPOLL Thread                       │
├─────────────────────────────────────────────────────────────┤
│  • Continuously polls submission queue                      │
│  • Executes I/O operations asynchronously                  │
│  • Fills completion queue with results                     │
│  • Sleeps when idle (configurable timeout)                 │
└─────────────────────────────────────────────────────────────┘
```

### Data Structure Changes

#### New aeApiState Structure
```c
typedef struct aeApiState {
    struct io_uring ring;               /* io_uring instance - contains all needed state */

    /* SQPOLL configuration */
    int sqpoll_enabled;
    int sqpoll_cpu;
    int sqpoll_idle_ms;
    
    /* Operation tracking - no event masks */
    uring_operation **operations;     /* Active operations by FD */
    int max_operations;
    
    /* Buffer management */
    uring_buffer_pool *buffer_pool;
    
    /* Statistics */
    uring_stats stats;
} aeApiState;
```

#### Operation Context (Simplified)
```c
typedef struct uring_operation {
    int fd;
    int op_type;                    /* ACCEPT, READ, WRITE */
    void *buffer;
    size_t buffer_size;

    /* Callback for completion */
    void (*completion_handler)(struct uring_operation *op, int result);

    /* Auto-resubmit flag */
    int persistent;

    /* Timing */
    monotime submit_time;
} uring_operation;
```

### Simplified Operation Flow

#### Basic Redis Request-Response Pattern
The design follows a simple, efficient flow for each client connection:

```
Accept → Read(command) → Execute → Write(response) → Read(next command) → ...
```

**Key principles:**
- **One operation per step**: No complex chaining required
- **Completion-driven**: Each completion triggers the next operation
- **Async multi-client**: Handle many clients concurrently
- **Simple state management**: Minimal operation tracking

#### Flow Implementation
```c
/* 1. Accept new connections */
static void handle_accept_completion(uring_operation *op, int result) {
    if (result > 0) {
        int client_fd = result;
        connection *conn = connCreateAcceptedSocket(server.el, client_fd, NULL);

        /* Immediately start reading from new client */
        submit_persistent_read(server.el->apidata, client_fd);
    }
    /* Accept operation auto-resubmits (multishot) */
}

/* 2. Read commands from clients */
static void handle_read_completion(uring_operation *op, int result) {
    if (result > 0) {
        /* Process the command */
        connection *conn = connByFd(op->fd);
        conn->uring_read_buffer = op->buffer;
        conn->uring_read_size = result;

        /* Call Redis command processing */
        conn->read_handler(conn);  /* This executes the command */

        /* Resubmit read for next command */
        resubmit_read_operation(server.el->apidata, op);
    }
}

/* 3. Write responses to clients */
static void handle_write_completion(uring_operation *op, int result) {
    /* Write completed - nothing else needed */
    /* Next read operation will handle the next command */
}
```

#### Multi-Client Async Handling
```c
/* Multiple clients handled concurrently */
Client 1: Read(GET key1) → Execute → Write("value1") → Read(SET key2) → ...
Client 2: Read(INCR counter) → Execute → Write("5") → Read(GET key3) → ...
Client 3: Read(LPUSH list item) → Execute → Write("2") → Read(PING) → ...

/* All operations execute asynchronously via io_uring */
```

### Core Operations

#### 1. Accept Operations
```c
int submit_persistent_accept(aeApiState *state, int listen_fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    uring_operation *op = create_operation(listen_fd, URING_OP_ACCEPT);
    
    /* Use multishot accept for continuous operation */
    io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, op);
    
    op->persistent = 1;
    op->completion_handler = handle_accept_completion;
    
    return 0;
}
```

#### 2. Read Operations
```c
int submit_persistent_read(aeApiState *state, int fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    uring_operation *op = create_operation(fd, URING_OP_READ);
    
    op->buffer = get_buffer_from_pool(state->buffer_pool);
    op->buffer_size = BUFFER_SIZE;
    
    io_uring_prep_recv(sqe, fd, op->buffer, op->buffer_size, 0);
    io_uring_sqe_set_data(sqe, op);
    
    op->persistent = 1;
    op->completion_handler = handle_read_completion;
    
    return 0;
}
```

#### 3. Write Operations
```c
int submit_write_when_ready(aeApiState *state, int fd, void *data, size_t len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    uring_operation *op = create_operation(fd, URING_OP_WRITE);
    
    /* Copy data to operation buffer */
    op->buffer = zmalloc(len);
    memcpy(op->buffer, data, len);
    op->buffer_size = len;
    
    io_uring_prep_send(sqe, fd, op->buffer, len, 0);
    io_uring_sqe_set_data(sqe, op);
    
    op->persistent = 0;  /* One-shot operation */
    op->completion_handler = handle_write_completion;
    
    return 0;
}
```



### Event Loop Transformation

#### New aeApiPoll Implementation
```c
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    struct io_uring_cqe *cqe;
    int numevents = 0;
    
    /* Calculate timeout */
    struct __kernel_timespec timeout = {0};
    struct __kernel_timespec *timeout_ptr = NULL;
    
    if (tvp) {
        timeout.tv_sec = tvp->tv_sec;
        timeout.tv_nsec = tvp->tv_usec * 1000;
        timeout_ptr = &timeout;
    }
    
    /* Submit any pending operations (SQPOLL will handle them) */
    io_uring_submit(&state->ring);
    
    /* Wait for completions */
    int ret = io_uring_wait_cqe_timeout(&state->ring, &cqe, timeout_ptr);
    
    if (ret == 0) {
        /* Process all available completions */
        unsigned head;
        unsigned count = 0;
        
        io_uring_for_each_cqe(&state->ring, head, cqe) {
            process_completion(eventLoop, cqe, &numevents);
            count++;
            
            if (count >= eventLoop->setsize) break;
        }
        
        io_uring_cq_advance(&state->ring, count);
    }
    
    return numevents;
}
```

#### Completion Processing
```c
static int process_completion(aeEventLoop *eventLoop, struct io_uring_cqe *cqe, int *numevents) {
    uring_operation *op = (uring_operation *)io_uring_cqe_get_data(cqe);
    int result = cqe->res;
    
    if (!op) return 0;
    
    /* Call operation-specific completion handler */
    if (op->completion_handler) {
        op->completion_handler(op, result);
    }
    
    /* Auto-resubmit persistent operations */
    if (op->persistent && result >= 0) {
        resubmit_operation(eventLoop->apidata, op);
    } else {
        free_operation(op);
    }
    
    (*numevents)++;
    return 0;
}
```

### Single-Threaded Design Rationale

#### Why No Thread-Safety is Needed
Redis's io_uring implementation is single-threaded by design:

1. **Main Event Loop**: Single-threaded, only one thread accesses io_uring structures
2. **SQPOLL Thread**: Runs in **kernel space**, not userspace - no synchronization needed
3. **No Concurrent Access**: Only the main thread submits operations and processes completions
4. **Performance**: Eliminates atomic operations, mutexes, and memory barriers

#### io_uring Resource Management
Important clarifications about io_uring design:

- **Memory-mapped regions**: io_uring uses shared memory for submission/completion queues
- **No ring FD**: Unlike epoll, there's no file descriptor for the ring itself
- **liburing handles cleanup**: `io_uring_queue_exit()` manages all resources
- **Only socket FDs matter**: Individual client connections still use normal FDs

#### SQPOLL Auto-Wake Behavior
**SQPOLL automatically wakes up** when new operations are submitted:

- **Auto-wake on submission**: `io_uring_submit()` automatically wakes sleeping SQPOLL
- **No manual wake-up needed**: SQPOLL detects new SQEs in submission queue
- **Eliminates wake-up complexity**: No eventfd or manual signaling required

```c
/* Simple operation submission - no locking needed */
static int submit_operation_simple(aeApiState *state, uring_operation *op) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        state->stats.sq_full_events++;  // Simple increment, no atomic needed
        return -1;
    }

    /* Setup operation */
    io_uring_sqe_set_data(sqe, op);
    state->stats.ops_submitted++;  // Simple increment

    return 0;
}

/* Correct resource management - simplified without wake-up mechanism */
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;
    if (!state) return;

    /* Clean up pending operations */
    for (int i = 0; i < state->max_operations; i++) {
        if (state->operations[i]) {
            free_operation(state->operations[i]);
        }
    }

    /* Clean up io_uring - liburing handles all internal resources */
    io_uring_queue_exit(&state->ring);  // No separate ring FD to close

    /* Clean up our own allocations */
    zfree(state->operations);
    zfree(state);
}
```

### SQPOLL Configuration

#### Optimal SQPOLL Setup
```c
static int setup_sqpoll_optimized(aeApiState *state, struct io_uring_params *params) {
    params->flags |= IORING_SETUP_SQPOLL;
    
    /* CPU affinity for SQPOLL thread */
    if (state->sqpoll_cpu >= 0) {
        params->flags |= IORING_SETUP_SQ_AFF;
        params->sq_thread_cpu = state->sqpoll_cpu;
    }
    
    /* Idle timeout - balance between responsiveness and CPU usage */
    params->sq_thread_idle = state->sqpoll_idle_ms;
    
    /* Enable cooperative task running */
    params->flags |= IORING_SETUP_COOP_TASKRUN;
    
    return 0;
}
```

### Simplified Event Loop Wake-up

**SQPOLL automatically wakes up** - no manual wake-up needed:

```c
/* Simple operation submission - SQPOLL wakes automatically */
static int submit_new_operation(aeApiState *state, uring_operation *op) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) return -1;

    /* Setup operation */
    io_uring_sqe_set_data(sqe, op);

    /* Submit - SQPOLL wakes automatically, no manual wake-up needed */
    io_uring_submit(&state->ring);

    return 0;
}
```

### Buffer Management Strategy

#### Zero-Copy Buffer Pool
```c
typedef struct uring_buffer_pool {
    void **buffers;
    int *available;
    int count;
    int size;
    int next_free;              /* Simple index - no atomics needed in single-threaded Redis */

    /* Statistics - simple counters */
    uint64_t allocations;
    uint64_t deallocations;
    uint64_t pool_hits;
    uint64_t pool_misses;
} uring_buffer_pool;
```

### Configuration Options

```conf
# io_uring configuration
uring-enabled yes
uring-sqpoll yes
uring-sqpoll-cpu -1              # Auto-detect optimal CPU
uring-sqpoll-idle 1000           # 1 second idle timeout
uring-sq-entries 1024            # Submission queue size
uring-cq-entries 2048            # Completion queue size (2x SQ)
uring-buffer-pool-size 2048      # Buffer pool size
uring-buffer-size 4096           # Individual buffer size
uring-multishot-accept yes       # Use multishot accept
uring-provided-buffers yes       # Use kernel-managed buffer rings
uring-cooperative-taskrun yes    # Enable cooperative task running
```

### Migration Strategy

#### Phase 1: Preparation
1. Extend current io_uring implementation
2. Add pure CQE processing mode
3. Implement operation-based tracking
4. Add comprehensive testing

#### Phase 2: Core Migration
1. Replace event mask logic with operation tracking
2. Implement persistent operation submission
3. Modify connection handling for CQE-based processing
4. Update networking layer integration

#### Phase 3: Optimization
1. Fine-tune SQPOLL parameters
2. Optimize buffer management
3. Implement advanced io_uring features (linked operations, etc.)
4. Performance testing and tuning

### Performance Considerations

#### Expected Benefits
- **Reduced System Calls**: SQPOLL eliminates most syscalls
- **Lower Latency**: Direct kernel-to-userspace communication
- **Better Scalability**: More efficient handling of many connections
- **CPU Efficiency**: Kernel-side polling reduces context switches

#### Potential Challenges
- **Memory Usage**: Buffer pools and operation contexts
- **Complexity**: More complex error handling and state management
- **Kernel Dependency**: Requires modern Linux kernel (5.1+)

### Testing Strategy

#### Unit Tests
- Operation submission and completion
- Buffer pool management
- SQPOLL wake-up mechanisms
- Error handling scenarios

#### Integration Tests
- Full Redis server with io_uring
- Client connection handling
- High-load scenarios
- Failover to epoll when io_uring unavailable

#### Performance Tests
- Latency measurements
- Throughput comparisons
- CPU usage analysis
- Memory consumption monitoring

### Backward Compatibility

The design maintains backward compatibility by:
- Keeping epoll as fallback mechanism
- Runtime detection of io_uring support
- Configuration-driven selection
- Graceful degradation on older kernels

This design provides a complete migration path from Redis's current epoll-based event system to a pure io_uring completion-based architecture, eliminating traditional event handling in favor of operation completion processing.

## Detailed Implementation

### Connection Handler Modifications

#### Required Changes to Connection Interface

The existing Redis connection handlers need modifications to work with io_uring's completion-based model:

```c
/* Enhanced connection structure for io_uring */
typedef struct connection {
    int fd;
    void (*read_handler)(struct connection *conn);
    void (*write_handler)(struct connection *conn);
    void (*close_handler)(struct connection *conn);

    /* NEW: io_uring specific fields */
    void *uring_read_buffer;     /* Buffer from completed read operation */
    size_t uring_read_size;      /* Bytes read in last operation */
    connection_type_t conn_type; /* CLIENT, REPLICA, CLUSTER_BUS, etc. */

    /* Existing fields unchanged */
    int state;
    int last_errno;
    // ... other existing fields
} connection;
```

#### Read Handler Modifications

Read handlers must be updated to access pre-read data from io_uring operations:

```c
/* Current epoll approach */
static void readQueryFromClient_epoll(connection *conn) {
    char buf[PROTO_IOBUF_LEN];
    int nread = read(conn->fd, buf, sizeof(buf));  /* Direct read syscall */
    if (nread > 0) {
        processInputBuffer(conn->client, buf, nread);
    }
}

/* New io_uring approach */
static void readQueryFromClient_uring(connection *conn) {
    /* Data already read by io_uring - access from connection */
    void *buf = conn->uring_read_buffer;
    int nread = conn->uring_read_size;

    if (!buf || nread <= 0) return;

    /* Process the pre-read data */
    processInputBuffer(conn->client, buf, nread);

    /* Clear buffer reference - will be reused by io_uring */
    conn->uring_read_buffer = NULL;
    conn->uring_read_size = 0;
}
```

#### Write Handler Modifications

Write handlers must queue operations instead of performing direct writes:

```c
/* Current epoll approach */
static void sendReplyToClient_epoll(connection *conn) {
    client *c = connGetPrivateData(conn);
    ssize_t nwritten = write(conn->fd, c->buf + c->sentlen, c->bufpos - c->sentlen);

    if (nwritten > 0) {
        c->sentlen += nwritten;
        if (c->sentlen == c->bufpos) {
            /* All data sent */
            c->sentlen = c->bufpos = 0;
        }
    }
}

/* New io_uring approach */
static void sendReplyToClient_uring(connection *conn) {
    client *c = connGetPrivateData(conn);

    if (c->bufpos > c->sentlen) {
        /* Queue write operation instead of direct write */
        size_t pending = c->bufpos - c->sentlen;
        int ret = submit_write_operation(server.el->apidata, conn->fd,
                                        c->buf + c->sentlen, pending);
        if (ret == 0) {
            /* Write queued successfully - mark as sent */
            c->sentlen = c->bufpos;
        }
    }
}
```

#### Close Handler Modifications

Close handlers need to cancel pending io_uring operations:

```c
/* Enhanced close handler for io_uring */
static void freeClient_uring(connection *conn) {
    client *c = connGetPrivateData(conn);

    /* NEW: Cancel any pending io_uring operations for this FD */
    aeApiState *state = server.el->apidata;
    if (state->operations[conn->fd]) {
        /* Mark operation as non-persistent to prevent resubmission */
        state->operations[conn->fd]->persistent = 0;
        state->operations[conn->fd] = NULL;
    }

    /* Existing cleanup logic unchanged */
    if (c->querybuf) sdsfree(c->querybuf);
    if (c->pending_querybuf) sdsfree(c->pending_querybuf);
    close(conn->fd);
    zfree(c);
}
```

#### Handler Registration (Unchanged)

The handler registration mechanism remains the same:

```c
/* Handler registration stays identical */
static void setupClientConnection(connection *conn) {
    conn->read_handler = readQueryFromClient_uring;   /* Updated implementation */
    conn->write_handler = sendReplyToClient_uring;    /* Updated implementation */
    conn->close_handler = freeClient_uring;           /* Updated implementation */
    conn->conn_type = CONN_TYPE_CLIENT;               /* NEW: Set connection type */
}
```

#### Backward Compatibility Strategy

Maintain compatibility through conditional compilation:

```c
/* Compatibility wrapper approach */
#ifdef USE_URING
    #define readQueryFromClient readQueryFromClient_uring
    #define sendReplyToClient sendReplyToClient_uring
    #define freeClient freeClient_uring
#else
    #define readQueryFromClient readQueryFromClient_epoll
    #define sendReplyToClient sendReplyToClient_epoll
    #define freeClient freeClient_epoll
#endif

/* Or runtime detection */
static void setupConnectionHandlers(connection *conn) {
    if (server.event_backend_type == AE_URING) {
        conn->read_handler = readQueryFromClient_uring;
        conn->write_handler = sendReplyToClient_uring;
        conn->close_handler = freeClient_uring;
    } else {
        conn->read_handler = readQueryFromClient_epoll;
        conn->write_handler = sendReplyToClient_epoll;
        conn->close_handler = freeClient_epoll;
    }
}
```

### Connection Lifecycle Management

#### Connection Establishment
```c
/* Replace traditional accept handler */
static void handle_accept_completion(uring_operation *op, int result) {
    if (result < 0) {
        if (result != -EAGAIN && result != -EWOULDBLOCK) {
            serverLog(LL_WARNING, "Accept failed: %s", strerror(-result));
        }
        return;
    }

    int client_fd = result;

    /* Create new client connection */
    connection *conn = connCreateAcceptedSocket(server.el, client_fd, NULL);
    if (!conn) {
        close(client_fd);
        return;
    }

    /* Immediately submit persistent read operation for new client */
    submit_persistent_read(server.el->apidata, client_fd);

    /* Accept operation is multishot, so it auto-resubmits */
}
```

#### Connection Read Processing
```c
static void handle_read_completion(uring_operation *op, int result) {
    aeApiState *state = server.el->apidata;

    if (result > 0) {
        /* Data received - process it */
        connection *conn = connByFd(op->fd);
        if (conn && conn->read_handler) {
            /* Set buffer data for connection to process */
            conn->uring_read_buffer = op->buffer;
            conn->uring_read_size = result;

            /* Call Redis connection read handler */
            conn->read_handler(conn);

            /* Clear buffer reference */
            conn->uring_read_buffer = NULL;
            conn->uring_read_size = 0;
        }

        /* Return buffer to pool */
        return_buffer_to_pool(state->buffer_pool, op->buffer);
        op->buffer = NULL;

        /* Resubmit read operation with new buffer */
        op->buffer = get_buffer_from_pool(state->buffer_pool);
        if (op->buffer) {
            resubmit_read_operation(state, op);
        }
    } else if (result == 0) {
        /* Connection closed by client */
        connection *conn = connByFd(op->fd);
        if (conn) {
            conn->state = CONN_STATE_CLOSED;
            if (conn->conn_handler) {
                conn->conn_handler(conn);
            }
        }

        /* Don't resubmit - connection is closed */
        op->persistent = 0;
    } else {
        /* Error occurred */
        if (result != -EAGAIN && result != -EWOULDBLOCK) {
            connection *conn = connByFd(op->fd);
            if (conn) {
                conn->state = CONN_STATE_ERROR;
                conn->last_errno = -result;
                if (conn->conn_handler) {
                    conn->conn_handler(conn);
                }
            }
        }
        /* Don't resubmit on error */
        op->persistent = 0;
    }
}
```

### Write Operation Management

#### Buffered Write Strategy
```c
/* Modified connection write function */
static ssize_t connUringWrite(connection *conn, const void *data, size_t data_len) {
    aeApiState *state = server.el->apidata;

    /* For io_uring, we queue the write operation */
    uring_write_request *write_req = zmalloc(sizeof(uring_write_request));
    write_req->data = zmalloc(data_len);
    memcpy(write_req->data, data, data_len);
    write_req->len = data_len;
    write_req->conn = conn;

    /* Submit write operation */
    int ret = submit_write_operation(state, conn->fd, write_req);
    if (ret < 0) {
        zfree(write_req->data);
        zfree(write_req);
        return -1;
    }

    /* Return data_len to indicate all data was "written" (queued) */
    return data_len;
}

static void handle_write_completion(uring_operation *op, int result) {
    uring_write_request *write_req = (uring_write_request *)op->user_data;
    connection *conn = write_req->conn;

    if (result > 0) {
        /* Write successful */
        if (conn->write_handler) {
            conn->write_handler(conn);
        }
    } else if (result < 0 && result != -EAGAIN && result != -EWOULDBLOCK) {
        /* Write error */
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = -result;
        if (conn->conn_handler) {
            conn->conn_handler(conn);
        }
    }

    /* Clean up write request */
    zfree(write_req->data);
    zfree(write_req);
}
```

### Time Event Integration

#### Timer Operations via io_uring
```c
/* Replace traditional time events with io_uring timeouts */
static int aeCreateTimeEventUring(aeEventLoop *eventLoop, long long milliseconds,
                                  aeTimeProc *proc, void *clientData) {
    aeApiState *state = eventLoop->apidata;

    uring_operation *op = create_operation(-1, URING_OP_TIMEOUT);
    op->time_proc = proc;
    op->client_data = clientData;
    op->timeout_id = ++eventLoop->timeEventNextId;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    struct __kernel_timespec ts = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000
    };

    io_uring_prep_timeout(sqe, &ts, 0, 0);
    io_uring_sqe_set_data(sqe, op);

    op->completion_handler = handle_timeout_completion;

    return op->timeout_id;
}

static void handle_timeout_completion(uring_operation *op, int result) {
    if (result == -ETIME) {
        /* Timeout expired - call the time procedure */
        int retval = op->time_proc(server.el, op->timeout_id, op->client_data);

        if (retval != AE_NOMORE) {
            /* Reschedule the timer */
            aeCreateTimeEventUring(server.el, retval, op->time_proc, op->client_data);
        }
    }
    /* Timer operations are always one-shot */
    op->persistent = 0;
}
```

### Error Handling and Recovery

#### Robust Error Management
```c
static void handle_operation_error(uring_operation *op, int error) {
    aeApiState *state = server.el->apidata;

    switch (error) {
        case -EAGAIN:
        case -EWOULDBLOCK:
            /* Temporary error - resubmit operation */
            if (op->persistent) {
                resubmit_operation_delayed(state, op, 1); /* 1ms delay */
            }
            break;

        case -ECONNRESET:
        case -EPIPE:
            /* Connection closed - clean up */
            connection *conn = connByFd(op->fd);
            if (conn) {
                conn->state = CONN_STATE_CLOSED;
                if (conn->conn_handler) {
                    conn->conn_handler(conn);
                }
            }
            op->persistent = 0;
            break;

        case -ENOMEM:
            /* Memory pressure - reduce buffer pool size temporarily */
            reduce_buffer_pool_size(state->buffer_pool);
            op->persistent = 0;
            break;

        default:
            /* Other errors - log and stop operation */
            serverLog(LL_WARNING, "io_uring operation failed: fd=%d, op=%d, error=%s",
                     op->fd, op->op_type, strerror(-error));
            op->persistent = 0;
            break;
    }
}
```

### Memory Management Optimization

#### Simple Buffer Pool (Single-Threaded)
```c
/* Simple buffer allocation - no locking needed in single-threaded Redis */
void *get_buffer_simple(uring_buffer_pool *pool) {
    /* Find available buffer */
    for (int i = 0; i < pool->count; i++) {
        int idx = (pool->next_free + i) % pool->count;
        if (pool->available[idx]) {
            pool->available[idx] = 0;
            pool->next_free = (idx + 1) % pool->count;
            pool->pool_hits++;
            return pool->buffers[idx];
        }
    }

    /* No buffer available, allocate new one */
    pool->pool_misses++;
    return zmalloc(pool->size);
}

void return_buffer_simple(uring_buffer_pool *pool, void *buffer) {
    /* Check if buffer belongs to pool */
    for (int i = 0; i < pool->count; i++) {
        if (pool->buffers[i] == buffer) {
            pool->available[i] = 1;
            return;
        }
    }

    /* Buffer doesn't belong to pool, free it */
    zfree(buffer);
}
```

### Performance Monitoring

#### Simple Statistics (Single-Threaded)
```c
typedef struct uring_stats {
    /* Operation counts - simple counters, no atomics needed */
    uint64_t ops_submitted;
    uint64_t ops_completed;
    uint64_t ops_failed;
    uint64_t ops_resubmitted;

    /* Timing statistics */
    uint64_t total_completion_time_us;
    uint64_t max_completion_time_us;
    uint64_t min_completion_time_us;

    /* Queue statistics */
    uint64_t sq_full_events;
    uint64_t cq_overflow_events;
    uint64_t sqpoll_wakeups;
    uint64_t sqpoll_idle_time_us;

    /* Buffer statistics */
    uint64_t buffer_pool_hits;
    uint64_t buffer_pool_misses;
    uint64_t buffer_allocations;
    uint64_t buffer_deallocations;

    /* Error statistics */
    uint64_t connection_errors;
    uint64_t timeout_errors;
    uint64_t memory_errors;
    uint64_t kernel_errors;
} uring_stats;

/* Statistics reporting for INFO command */
void aeGetUringStatsDetailed(aeEventLoop *eventLoop, sds *info) {
    aeApiState *state = eventLoop->apidata;
    uring_stats *stats = &state->stats;

    *info = sdscatprintf(*info,
        "# io_uring_detailed\r\n"
        "uring_ops_submitted:%lu\r\n"
        "uring_ops_completed:%lu\r\n"
        "uring_ops_failed:%lu\r\n"
        "uring_ops_resubmitted:%lu\r\n"
        "uring_avg_completion_time_us:%.2f\r\n"
        "uring_max_completion_time_us:%lu\r\n"
        "uring_min_completion_time_us:%lu\r\n"
        "uring_sq_full_events:%lu\r\n"
        "uring_cq_overflow_events:%lu\r\n"
        "uring_sqpoll_wakeups:%lu\r\n"
        "uring_buffer_pool_hit_rate:%.2f\r\n"
        "uring_connection_errors:%lu\r\n",
        stats->ops_submitted,
        stats->ops_completed,
        stats->ops_failed,
        stats->ops_resubmitted,
        stats->ops_completed > 0 ?
            (double)stats->total_completion_time_us / stats->ops_completed : 0.0,
        stats->max_completion_time_us,
        stats->min_completion_time_us,
        stats->sq_full_events,
        stats->cq_overflow_events,
        stats->sqpoll_wakeups,
        (stats->buffer_pool_hits + stats->buffer_pool_misses) > 0 ?
            (double)stats->buffer_pool_hits /
            (stats->buffer_pool_hits + stats->buffer_pool_misses) * 100.0 : 0.0,
        stats->connection_errors);
}

## Advanced Features

### Multishot Operations
```c
/* Continuous accept without resubmission */
static int setup_multishot_accept(aeApiState *state, int listen_fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    uring_operation *op = create_operation(listen_fd, URING_OP_ACCEPT);

    /* Enable multishot accept */
    io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, op);

    op->persistent = 1;
    op->multishot = 1;
    op->completion_handler = handle_multishot_accept;

    return 0;
}

static void handle_multishot_accept(uring_operation *op, int result) {
    if (result < 0) {
        if (result == -ECANCELED) {
            /* Multishot was cancelled - resubmit if needed */
            if (op->persistent) {
                setup_multishot_accept(server.el->apidata, op->fd);
            }
        }
        return;
    }

    /* New connection accepted */
    int client_fd = result;
    connection *conn = connCreateAcceptedSocket(server.el, client_fd, NULL);
    if (conn) {
        submit_persistent_read(server.el->apidata, client_fd);
    }

    /* Multishot accept continues automatically */
}
```

### Zero-Copy Buffer Management
```c
/* Provided buffer ring for zero-copy reads */
static int setup_provided_buffers(aeApiState *state) {
    int buffer_count = 1024;
    int buffer_size = 4096;
    int bgid = 0; /* Buffer group ID */

    /* Allocate buffer memory */
    void *buffer_base = mmap(NULL, buffer_count * buffer_size,
                            PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (buffer_base == MAP_FAILED) return -1;

    /* Register buffers with io_uring */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    io_uring_prep_provide_buffers(sqe, buffer_base, buffer_size, buffer_count, bgid, 0);

    state->provided_buffers.base = buffer_base;
    state->provided_buffers.count = buffer_count;
    state->provided_buffers.size = buffer_size;
    state->provided_buffers.group_id = bgid;

    return 0;
}

/* Use provided buffers for reads */
static int submit_read_with_provided_buffer(aeApiState *state, int fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    uring_operation *op = create_operation(fd, URING_OP_READ);

    /* Use buffer selection */
    io_uring_prep_recv(sqe, fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = state->provided_buffers.group_id;

    io_uring_sqe_set_data(sqe, op);
    op->completion_handler = handle_provided_buffer_read;

    return 0;
}
```

## Deployment Strategy

### Gradual Migration Plan

#### Phase 1: Infrastructure (Weeks 1-2)
1. **Extend Configuration System**
   - Add io_uring specific configuration options
   - Implement runtime capability detection
   - Add fallback mechanisms

2. **Core Data Structures**
   - Implement new operation tracking system
   - Create buffer pool management
   - Add statistics collection framework

#### Phase 2: Core Operations (Weeks 3-4)
1. **Basic Operations**
   - Implement accept, read, write operations
   - Add completion handlers
   - Integrate with existing connection system

2. **SQPOLL Integration**
   - Configure SQPOLL parameters
   - Implement wake-up mechanisms
   - Add CPU affinity support

#### Phase 3: Advanced Features (Weeks 5-6)
1. **Performance Optimizations**
   - Add multishot support
   - Optimize buffer management
   - Implement provided buffer rings

2. **Error Handling**
   - Comprehensive error recovery
   - Graceful degradation
   - Monitoring and alerting

#### Phase 4: Testing and Validation (Weeks 7-8)
1. **Comprehensive Testing**
   - Unit tests for all components
   - Integration tests with Redis workloads
   - Performance benchmarking

2. **Production Readiness**
   - Documentation updates
   - Configuration guides
   - Monitoring setup

### Configuration Migration

#### Automatic Detection
```c
static int detect_optimal_uring_config(uring_config *config) {
    /* Detect kernel version and capabilities */
    struct utsname kernel_info;
    uname(&kernel_info);

    int major, minor;
    sscanf(kernel_info.release, "%d.%d", &major, &minor);

    /* Configure based on kernel version */
    if (major > 5 || (major == 5 && minor >= 19)) {
        config->multishot_accept = 1;
        config->provided_buffers = 1;
    } else if (major > 5 || (major == 5 && minor >= 1)) {
        config->multishot_accept = 0;
        config->provided_buffers = 0;
    } else {
        /* Kernel too old for io_uring */
        return -1;
    }

    /* Detect CPU count for SQPOLL configuration */
    int cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count > 4) {
        config->sqpoll_enabled = 1;
        config->sqpoll_cpu = cpu_count - 1; /* Use last CPU */
    }

    /* Configure queue sizes based on expected load */
    config->sq_entries = 1024;
    config->cq_entries = 2048;

    return 0;
}
```

### Monitoring and Observability

#### Health Monitoring and Recovery
```c
typedef struct uring_health {
    monotime last_completion;
    int consecutive_errors;
    int sqpoll_responsive;
    double completion_rate;
    double error_rate;
    int recovery_attempts;
} uring_health;

static int check_uring_health_and_recover(aeApiState *state) {
    uring_stats *stats = &state->stats;
    uring_health *health = &state->health;
    monotime now = getMonotonicUs();

    /* Check SQPOLL responsiveness and restart if needed */
    if (state->sqpoll_enabled) {
        static uint64_t last_completed = 0;
        uint64_t current_completed = stats->ops_completed;

        if (current_completed == last_completed && stats->ops_submitted > last_completed) {
            /* No progress despite pending operations - restart SQPOLL */
            serverLog(LL_WARNING, "io_uring: SQPOLL unresponsive, attempting restart");
            if (restart_sqpoll_thread(state) == 0) {
                health->sqpoll_responsive = 1;
                health->recovery_attempts++;
            } else {
                serverLog(LL_WARNING, "io_uring: Failed to restart SQPOLL");
                return 0;
            }
        } else {
            health->sqpoll_responsive = 1;
        }

        last_completed = current_completed;
    }

    /* Check error rate and reset queues if needed */
    if (stats->ops_completed > 1000) {
        double error_rate = (double)stats->ops_failed /
                           (stats->ops_completed + stats->ops_failed);
        if (error_rate > 0.1) {
            serverLog(LL_WARNING, "io_uring: High error rate: %.2f%%, resetting queues",
                     error_rate * 100);
            if (reset_uring_queues(state) == 0) {
                health->recovery_attempts++;
                /* Reset error counters after recovery */
                stats->ops_failed = 0;
                stats->ops_completed = 0;
            } else {
                serverLog(LL_WARNING, "io_uring: Failed to reset queues");
                return 0;
            }
        }
    }

    /* Check buffer pool efficiency and expand if needed */
    if (stats->buffer_pool_hits + stats->buffer_pool_misses > 100) {
        double hit_rate = (double)stats->buffer_pool_hits /
                         (stats->buffer_pool_hits + stats->buffer_pool_misses);
        if (hit_rate < 0.5) {
            serverLog(LL_WARNING, "io_uring: Low buffer pool hit rate: %.2f%%, expanding pool",
                     hit_rate * 100);
            if (expand_buffer_pool(state) == 0) {
                health->recovery_attempts++;
            }
        }
    }

    return 1;
}
```

### Recovery Functions

#### io_uring Recovery Implementation
```c
/* Restart SQPOLL thread when unresponsive */
static int restart_sqpoll_thread(aeApiState *state) {
    serverLog(LL_NOTICE, "Restarting SQPOLL thread");

    /* Save current operations */
    uring_operation **saved_ops = zmalloc(sizeof(uring_operation*) * state->max_operations);
    int saved_count = 0;

    for (int i = 0; i < state->max_operations; i++) {
        if (state->operations[i] && state->operations[i]->persistent) {
            saved_ops[saved_count++] = state->operations[i];
        }
    }

    /* Reinitialize io_uring with SQPOLL */
    struct io_uring_params params = {0};
    params.flags = IORING_SETUP_SQPOLL;
    if (state->sqpoll_cpu >= 0) {
        params.flags |= IORING_SETUP_SQ_AFF;
        params.sq_thread_cpu = state->sqpoll_cpu;
    }
    params.sq_thread_idle = state->sqpoll_idle_ms;

    io_uring_queue_exit(&state->ring);
    int ret = io_uring_queue_init_params(state->sq_entries, &state->ring, &params);
    if (ret < 0) {
        serverLog(LL_WARNING, "Failed to restart SQPOLL: %s", strerror(-ret));
        zfree(saved_ops);
        return -1;
    }

    /* Resubmit saved operations */
    for (int i = 0; i < saved_count; i++) {
        resubmit_operation(state, saved_ops[i]);
    }

    zfree(saved_ops);
    serverLog(LL_NOTICE, "SQPOLL thread restarted successfully");
    return 0;
}

/* Reset io_uring queues when operations are stuck */
static int reset_uring_queues(aeApiState *state) {
    serverLog(LL_NOTICE, "Resetting io_uring queues");

    /* Drain completion queue */
    struct io_uring_cqe *cqe;
    while (io_uring_peek_cqe(&state->ring, &cqe) == 0) {
        io_uring_cqe_seen(&state->ring, cqe);
    }

    /* Cancel and resubmit essential operations */
    for (int i = 0; i < state->max_operations; i++) {
        if (state->operations[i] && state->operations[i]->persistent) {
            /* Resubmit persistent operations */
            resubmit_operation(state, state->operations[i]);
        }
    }

    serverLog(LL_NOTICE, "io_uring queues reset successfully");
    return 0;
}

/* Expand buffer pool when hit rate is low */
static int expand_buffer_pool(aeApiState *state) {
    uring_buffer_pool *pool = state->buffer_pool;
    int new_count = pool->count * 2;

    serverLog(LL_NOTICE, "Expanding buffer pool from %d to %d buffers",
              pool->count, new_count);

    /* Allocate new buffer arrays */
    void **new_buffers = zrealloc(pool->buffers, sizeof(void*) * new_count);
    int *new_available = zrealloc(pool->available, sizeof(int) * new_count);

    if (!new_buffers || !new_available) {
        serverLog(LL_WARNING, "Failed to expand buffer pool: out of memory");
        return -1;
    }

    /* Allocate new buffers */
    for (int i = pool->count; i < new_count; i++) {
        new_buffers[i] = zmalloc(pool->size);
        if (!new_buffers[i]) {
            /* Cleanup on failure */
            for (int j = pool->count; j < i; j++) {
                zfree(new_buffers[j]);
            }
            serverLog(LL_WARNING, "Failed to allocate new buffers");
            return -1;
        }
        new_available[i] = 1;
    }

    pool->buffers = new_buffers;
    pool->available = new_available;
    pool->count = new_count;

    serverLog(LL_NOTICE, "Buffer pool expanded successfully");
    return 0;
}
```

### Startup-Time Backend Selection

#### Compile-Time and Runtime Detection
```c
/* Backend selection at Redis startup - no runtime fallback */
int redis_networking_init(void) {
    #ifdef HAVE_LIBURING
        /* Try to initialize io_uring */
        if (init_uring_backend() == 0) {
            serverLog(LL_NOTICE, "Using io_uring networking backend");
            server.event_backend = "io_uring";
            return 0;
        }
        serverLog(LL_WARNING, "io_uring initialization failed");
    #else
        serverLog(LL_NOTICE, "liburing not available at compile time");
    #endif

    /* Fallback to epoll only if liburing not available */
    if (init_epoll_backend() == 0) {
        serverLog(LL_NOTICE, "Using epoll networking backend");
        server.event_backend = "epoll";
        return 0;
    }

    serverLog(LL_WARNING, "Failed to initialize any networking backend");
    return -1;
}

/* No runtime fallback - commit to io_uring once started */
static int init_uring_backend(void) {
    /* Detect kernel support */
    struct io_uring test_ring;
    int ret = io_uring_queue_init(8, &test_ring, 0);
    if (ret < 0) {
        serverLog(LL_WARNING, "Kernel does not support io_uring: %s", strerror(-ret));
        return -1;
    }
    io_uring_queue_exit(&test_ring);

    /* Initialize full io_uring backend */
    if (aeApiCreate_uring(server.el) < 0) {
        serverLog(LL_WARNING, "Failed to create io_uring event loop");
        return -1;
    }

    serverLog(LL_NOTICE, "io_uring backend initialized successfully");
    return 0;
}
```

## Cluster Replication Support

### Overview

Redis cluster replication requires handling multiple connection types and protocols. The io_uring design must support:

1. **Master-Replica replication streams**
2. **Cluster bus communication** (binary protocol on port+10000)
3. **Cross-node data synchronization**
4. **Replication buffer management**

### Connection Type Extensions

#### Enhanced Connection Types
```c
typedef enum {
    CONN_TYPE_CLIENT = 0,
    CONN_TYPE_REPLICA,
    CONN_TYPE_MASTER,
    CONN_TYPE_CLUSTER_BUS,
    CONN_TYPE_SENTINEL
} connection_type_t;

typedef struct uring_operation {
    int fd;
    int op_type;                    /* ACCEPT, READ, WRITE */
    connection_type_t conn_type;    /* Connection classification */
    void *buffer;
    size_t buffer_size;

    /* Protocol-specific handlers */
    void (*completion_handler)(struct uring_operation *op, int result);
    void (*protocol_handler)(struct uring_operation *op, void *data, size_t len);

    int persistent;
    monotime submit_time;

    /* Replication-specific fields */
    struct {
        uint64_t repl_offset;       /* Current replication offset */
        int repl_state;             /* REPL_STATE_* */
        void *repl_backlog_ptr;     /* Position in backlog */
    } replication;
} uring_operation;
```

### Replication Protocol Handling

#### Master-Replica Communication
```c
/* Handle replication stream data */
static void handle_replication_completion(uring_operation *op, int result) {
    if (result <= 0) {
        handle_replication_disconnect(op);
        return;
    }

    connection *conn = connByFd(op->fd);

    switch (op->conn_type) {
        case CONN_TYPE_REPLICA:
            process_replica_data(conn, op->buffer, result);
            break;
        case CONN_TYPE_MASTER:
            process_master_commands(conn, op->buffer, result);
            break;
        default:
            break;
    }

    /* Update replication offset */
    op->replication.repl_offset += result;

    /* Resubmit persistent read */
    if (op->persistent) {
        resubmit_replication_read(server.el->apidata, op);
    }
}

/* Process data from replica */
static void process_replica_data(connection *conn, void *data, size_t len) {
    client *c = connGetPrivateData(conn);

    /* Handle REPLCONF ACK, PSYNC responses, etc. */
    if (c->replstate == REPL_STATE_SEND_BULK) {
        /* Replica is receiving RDB */
        update_replica_bulk_transfer(c, data, len);
    } else if (c->replstate == REPL_STATE_ONLINE) {
        /* Normal replication stream */
        process_replica_commands(c, data, len);
    }
}

/* Process commands from master */
static void process_master_commands(connection *conn, void *data, size_t len) {
    client *c = connGetPrivateData(conn);

    /* Parse and execute replication commands */
    parse_replication_stream(c, data, len);

    /* Send ACK if needed */
    if (should_send_repl_ack(c)) {
        send_replication_ack(c);
    }
}
```

#### Cluster Bus Protocol
```c
/* Handle cluster bus communication */
static void handle_cluster_bus_completion(uring_operation *op, int result) {
    if (result <= 0) {
        handle_cluster_node_disconnect(op);
        return;
    }

    /* Process cluster bus messages */
    process_cluster_bus_message(op->buffer, result);

    /* Resubmit read for more cluster messages */
    if (op->persistent) {
        resubmit_cluster_read(server.el->apidata, op);
    }
}

/* Process cluster bus binary protocol */
static void process_cluster_bus_message(void *data, size_t len) {
    clusterMsg *hdr = (clusterMsg*)data;

    if (len < sizeof(clusterMsg)) return;

    /* Validate message */
    if (memcmp(hdr->sig, "RCmb", 4) != 0) return;

    /* Process based on message type */
    switch (ntohs(hdr->type)) {
        case CLUSTERMSG_TYPE_PING:
            handle_cluster_ping(hdr);
            break;
        case CLUSTERMSG_TYPE_PONG:
            handle_cluster_pong(hdr);
            break;
        case CLUSTERMSG_TYPE_MEET:
            handle_cluster_meet(hdr);
            break;
        case CLUSTERMSG_TYPE_FAIL:
            handle_cluster_fail(hdr);
            break;
        case CLUSTERMSG_TYPE_UPDATE:
            handle_cluster_update(hdr);
            break;
        default:
            serverLog(LL_WARNING, "Unknown cluster message type: %d", ntohs(hdr->type));
            break;
    }
}
```

### Enhanced Buffer Management

#### Replication-Specific Buffers
```c
typedef struct replication_buffer_pool {
    /* Large buffers for replication backlog */
    void **repl_buffers;
    int repl_buffer_count;
    size_t repl_buffer_size;        /* Typically 1MB+ */

    /* Small buffers for cluster bus */
    void **cluster_buffers;
    int cluster_buffer_count;
    size_t cluster_buffer_size;     /* Typically 64KB */

    /* Standard client buffers */
    void **client_buffers;
    int client_buffer_count;
    size_t client_buffer_size;      /* Typically 4KB */

    /* Buffer allocation tracking */
    int next_repl_free;
    int next_cluster_free;
    int next_client_free;
} replication_buffer_pool;

/* Get buffer based on connection type */
static void *get_buffer_by_type(aeApiState *state, connection_type_t conn_type) {
    replication_buffer_pool *pool = &state->repl_buffer_pool;

    switch (conn_type) {
        case CONN_TYPE_REPLICA:
        case CONN_TYPE_MASTER:
            return get_replication_buffer(pool);
        case CONN_TYPE_CLUSTER_BUS:
            return get_cluster_buffer(pool);
        default:
            return get_client_buffer(pool);
    }
}
```

### Connection Establishment

#### Enhanced Accept Handler
```c
static void handle_accept_completion(uring_operation *op, int result) {
    if (result < 0) return;

    int client_fd = result;
    int listen_port = get_listen_port(op->fd);
    connection_type_t conn_type;

    /* Determine connection type based on listening port */
    if (listen_port == server.port) {
        conn_type = CONN_TYPE_CLIENT;
    } else if (listen_port == server.port + CLUSTER_PORT_INCR) {
        conn_type = CONN_TYPE_CLUSTER_BUS;
    } else {
        conn_type = CONN_TYPE_CLIENT; /* Default */
    }

    /* Create connection with appropriate type */
    connection *conn = connCreateAcceptedSocket(server.el, client_fd, NULL);
    if (!conn) {
        close(client_fd);
        return;
    }

    /* Set connection type for proper handling */
    connSetType(conn, conn_type);

    /* Submit appropriate read operation */
    submit_typed_read_operation(server.el->apidata, client_fd, conn_type);
}

/* Submit read operation based on connection type */
static int submit_typed_read_operation(aeApiState *state, int fd, connection_type_t conn_type) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) return -1;

    uring_operation *op = create_operation(fd, URING_OP_READ);
    op->conn_type = conn_type;

    /* Set appropriate completion handler */
    switch (conn_type) {
        case CONN_TYPE_REPLICA:
        case CONN_TYPE_MASTER:
            op->completion_handler = handle_replication_completion;
            break;
        case CONN_TYPE_CLUSTER_BUS:
            op->completion_handler = handle_cluster_bus_completion;
            break;
        default:
            op->completion_handler = handle_client_completion;
            break;
    }

    /* Get appropriate buffer size */
    op->buffer = get_buffer_by_type(state, conn_type);
    op->buffer_size = get_buffer_size_by_type(conn_type);

    io_uring_prep_recv(sqe, fd, op->buffer, op->buffer_size, 0);
    io_uring_sqe_set_data(sqe, op);

    op->persistent = 1;
    state->stats.ops_submitted++;

    return 0;
}
```

### Replication State Management

#### Enhanced Operation Context
```c
/* Extended operation context for replication */
typedef struct replication_context {
    /* Replication stream state */
    int repl_state;                 /* REPL_STATE_* */
    uint64_t repl_offset;           /* Current offset */
    uint64_t repl_ack_offset;       /* Last ACK'd offset */
    monotime last_interaction;      /* For timeout detection */

    /* Partial resync support */
    char repl_id[CONFIG_RUN_ID_SIZE+1];
    uint64_t psync_offset;

    /* Backlog management */
    void *backlog_ptr;
    size_t backlog_size;

    /* Cluster node info (for cluster bus) */
    clusterNode *node;
    char node_id[CLUSTER_NAMELEN];
} replication_context;

/* Attach replication context to operation */
static void setup_replication_context(uring_operation *op, connection *conn) {
    replication_context *repl_ctx = zmalloc(sizeof(replication_context));

    /* Initialize replication state */
    repl_ctx->repl_state = REPL_STATE_NONE;
    repl_ctx->repl_offset = 0;
    repl_ctx->last_interaction = getMonotonicUs();

    /* Attach to operation */
    op->replication.repl_context = repl_ctx;

    /* Link to connection */
    connSetPrivateData(conn, repl_ctx);
}
```

## Conclusion

This design document provides a comprehensive roadmap for migrating Redis from epoll to a pure io_uring architecture. The key innovations include:

1. **Complete elimination of event masks** in favor of operation-based completion handling
2. **SQPOLL integration** for kernel-side polling with minimal userspace overhead
3. **Persistent operations** that auto-resubmit for continuous monitoring
4. **Advanced buffer management** with zero-copy optimizations
5. **Comprehensive error handling** and recovery mechanisms
6. **Detailed monitoring** and observability features
7. **Full cluster replication support** with protocol-aware connection handling

The migration strategy focuses on **full commitment to io_uring** with recovery-based reliability rather than runtime fallback. Backend selection occurs only at startup based on liburing availability. The phased approach allows for gradual adoption and thorough testing at each stage.

Expected benefits include reduced system call overhead, lower latency, better scalability, and improved CPU efficiency, making Redis more performant on modern high-throughput workloads including cluster deployments. The design eliminates runtime fallback complexity in favor of robust io_uring recovery mechanisms.
