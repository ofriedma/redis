# Redis io_uring Implementation Guide

## Overview

This document provides a step-by-step implementation guide for migrating Redis from epoll to io_uring. The work is designed for a team of 3-4 developers working in parallel, with each step independently verifiable.

## Team Structure

- **Team Member A**: Core io_uring infrastructure and event loop
- **Team Member B**: Connection handling and buffer management  
- **Team Member C**: Operation handlers and completion processing
- **Team Member D**: Testing, integration, and cluster support

## Prerequisites

### Development Environment Setup
```bash
# Install liburing development headers
sudo apt-get install liburing-dev  # Ubuntu/Debian
sudo yum install liburing-devel     # RHEL/CentOS

# Verify io_uring support
uname -r  # Should be 5.1+ for basic support, 5.19+ for advanced features

# Test liburing installation
cat > test_uring.c << 'EOF'
#include <liburing.h>
#include <stdio.h>
int main() {
    struct io_uring ring;
    int ret = io_uring_queue_init(8, &ring, 0);
    if (ret < 0) {
        printf("io_uring not supported: %d\n", ret);
        return 1;
    }
    printf("io_uring supported\n");
    io_uring_queue_exit(&ring);
    return 0;
}
EOF
gcc -o test_uring test_uring.c -luring && ./test_uring
```

## Phase 1: Foundation (Week 1)

### Step 1.1: Build System Integration (Team Member A)

**Objective**: Add io_uring support to Redis build system

**Files to modify**:
- `src/Makefile`
- `src/config.h`
- `configure` (if exists)

**Implementation**:
```makefile
# Add to src/Makefile
ifdef USE_URING
    FINAL_CFLAGS+= -DHAVE_LIBURING
    FINAL_LIBS+= -luring
endif

# Detection logic
ifeq ($(shell pkg-config --exists liburing && echo yes),yes)
    USE_URING=yes
endif
```

**Verification checklist**:
- [ ] `make USE_URING=yes` compiles successfully
- [ ] `make` without USE_URING compiles successfully (fallback)
- [ ] `redis-server --version` shows build flags
- [ ] No new compiler warnings introduced

**Test command**:
```bash
make clean && make USE_URING=yes && ./src/redis-server --version | grep -i uring
```

### Step 1.2: Core Data Structures (Team Member A)

**Objective**: Define core io_uring data structures

**Files to create/modify**:
- `src/ae_uring.h` (new)
- `src/ae.h` (modify)

**Implementation**:
```c
/* src/ae_uring.h */
#ifndef __AE_URING_H__
#define __AE_URING_H__

#ifdef HAVE_LIBURING
#include <liburing.h>

typedef enum {
    URING_OP_ACCEPT = 1,
    URING_OP_READ,
    URING_OP_WRITE,
    URING_OP_TIMEOUT
} uring_op_type_t;

typedef struct uring_operation {
    int fd;
    uring_op_type_t op_type;
    void *buffer;
    size_t buffer_size;
    void (*completion_handler)(struct uring_operation *op, int result);
    int persistent;
    monotime submit_time;
} uring_operation;

typedef struct uring_buffer_pool {
    void **buffers;
    int *available;
    int count;
    int size;
    int next_free;
    uint64_t pool_hits;
    uint64_t pool_misses;
} uring_buffer_pool;

typedef struct uring_stats {
    uint64_t ops_submitted;
    uint64_t ops_completed;
    uint64_t ops_failed;
    uint64_t buffer_pool_hits;
    uint64_t buffer_pool_misses;
} uring_stats;

typedef struct aeApiState {
    struct io_uring ring;
    int sqpoll_enabled;
    int sqpoll_cpu;
    int sqpoll_idle_ms;
    uring_operation **operations;
    int max_operations;
    uring_buffer_pool *buffer_pool;
    uring_stats stats;
} aeApiState;

/* Function declarations */
int aeApiCreate_uring(aeEventLoop *eventLoop);
void aeApiFree_uring(aeEventLoop *eventLoop);
int aeApiPoll_uring(aeEventLoop *eventLoop, struct timeval *tvp);

#endif /* HAVE_LIBURING */
#endif /* __AE_URING_H__ */
```

**Verification checklist**:
- [ ] Header compiles without errors
- [ ] All struct sizes are reasonable (check with `sizeof()`)
- [ ] No circular dependencies
- [ ] Conditional compilation works (`#ifdef HAVE_LIBURING`)

**Test command**:
```bash
# Create test file
cat > test_structs.c << 'EOF'
#define HAVE_LIBURING
#include "ae_uring.h"
#include <stdio.h>
int main() {
    printf("uring_operation size: %zu\n", sizeof(uring_operation));
    printf("aeApiState size: %zu\n", sizeof(aeApiState));
    return 0;
}
EOF
gcc -I. -o test_structs test_structs.c -luring && ./test_structs
```

### Step 1.3: Configuration Integration (Team Member B)

**Objective**: Add io_uring configuration options

**Files to modify**:
- `src/config.c`
- `src/server.h`
- `redis.conf`

**Implementation**:
```c
/* Add to server.h */
struct redisServer {
    // ... existing fields ...
    
    /* io_uring configuration */
    int uring_enabled;
    int uring_sqpoll;
    int uring_sqpoll_cpu;
    int uring_sqpoll_idle;
    int uring_sq_entries;
    int uring_cq_entries;
    int uring_buffer_pool_size;
    int uring_buffer_size;
};
```

```conf
# Add to redis.conf
################################ IO_URING #####################################

# Enable io_uring networking backend (requires Linux 5.1+)
# uring-enabled yes

# Enable SQPOLL for kernel-side polling
# uring-sqpoll yes

# CPU for SQPOLL thread (-1 for auto-detect)
# uring-sqpoll-cpu -1

# SQPOLL idle timeout in milliseconds
# uring-sqpoll-idle 1000

# Submission queue size
# uring-sq-entries 1024

# Completion queue size (should be 2x submission queue)
# uring-cq-entries 2048

# Buffer pool size
# uring-buffer-pool-size 2048

# Individual buffer size
# uring-buffer-size 4096
```

**Verification checklist**:
- [ ] Configuration options parse correctly
- [ ] `CONFIG GET uring-*` returns all options
- [ ] `CONFIG SET uring-enabled yes` works (if runtime changeable)
- [ ] Invalid values are rejected with proper error messages
- [ ] Default values are sensible

**Test command**:
```bash
./src/redis-server --test-memory && echo "Config parsing OK"
./src/redis-cli CONFIG GET "*uring*"
```

## Phase 2: Core Implementation (Week 2)

### Step 2.1: Basic io_uring Setup (Team Member A)

**Objective**: Implement basic io_uring initialization

**Files to create**:
- `src/ae_uring.c`

**Implementation**:
```c
/* src/ae_uring.c - Basic setup */
#include "ae.h"
#include "ae_uring.h"
#include "zmalloc.h"

#ifdef HAVE_LIBURING

int aeApiCreate_uring(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));
    struct io_uring_params params = {0};
    
    /* Configure SQPOLL if enabled */
    if (server.uring_sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
        if (server.uring_sqpoll_cpu >= 0) {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = server.uring_sqpoll_cpu;
        }
        params.sq_thread_idle = server.uring_sqpoll_idle;
    }
    
    /* Initialize io_uring */
    int ret = io_uring_queue_init_params(server.uring_sq_entries, &state->ring, &params);
    if (ret < 0) {
        zfree(state);
        return -1;
    }
    
    /* Initialize operation tracking */
    state->max_operations = eventLoop->setsize;
    state->operations = zcalloc(sizeof(uring_operation*) * state->max_operations);
    
    /* Initialize buffer pool */
    state->buffer_pool = create_buffer_pool(server.uring_buffer_pool_size, 
                                           server.uring_buffer_size);
    if (!state->buffer_pool) {
        io_uring_queue_exit(&state->ring);
        zfree(state->operations);
        zfree(state);
        return -1;
    }
    
    /* Initialize statistics */
    memset(&state->stats, 0, sizeof(state->stats));
    
    eventLoop->apidata = state;
    return 0;
}

void aeApiFree_uring(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;
    if (!state) return;
    
    /* Clean up pending operations */
    for (int i = 0; i < state->max_operations; i++) {
        if (state->operations[i]) {
            free_operation(state->operations[i]);
        }
    }
    
    /* Clean up buffer pool */
    free_buffer_pool(state->buffer_pool);
    
    /* Clean up io_uring */
    io_uring_queue_exit(&state->ring);
    
    zfree(state->operations);
    zfree(state);
    eventLoop->apidata = NULL;
}

#endif /* HAVE_LIBURING */
```

**Verification checklist**:
- [ ] `aeApiCreate_uring()` succeeds with valid parameters
- [ ] `aeApiCreate_uring()` fails gracefully with invalid parameters
- [ ] `aeApiFree_uring()` cleans up all resources
- [ ] No memory leaks (test with valgrind)
- [ ] SQPOLL configuration is applied correctly

**Test command**:
```bash
# Create minimal test
cat > test_init.c << 'EOF'
#include "ae.h"
#include "ae_uring.h"
int main() {
    aeEventLoop *el = aeCreateEventLoop(1024);
    if (aeApiCreate_uring(el) == 0) {
        printf("io_uring init: OK\n");
        aeApiFree_uring(el);
        aeDeleteEventLoop(el);
        return 0;
    }
    return 1;
}
EOF
gcc -I. -o test_init test_init.c ae.c ae_uring.c zmalloc.c -luring && ./test_init
```

### Step 2.2: Buffer Pool Implementation (Team Member B)

**Objective**: Implement efficient buffer management

**Files to modify**:
- `src/ae_uring.c` (add buffer pool functions)

**Implementation**:
```c
/* Buffer pool implementation */
uring_buffer_pool *create_buffer_pool(int count, int size) {
    uring_buffer_pool *pool = zmalloc(sizeof(uring_buffer_pool));
    
    pool->buffers = zmalloc(sizeof(void*) * count);
    pool->available = zmalloc(sizeof(int) * count);
    pool->count = count;
    pool->size = size;
    pool->next_free = 0;
    
    /* Allocate all buffers */
    for (int i = 0; i < count; i++) {
        pool->buffers[i] = zmalloc(size);
        if (!pool->buffers[i]) {
            /* Cleanup on failure */
            for (int j = 0; j < i; j++) {
                zfree(pool->buffers[j]);
            }
            zfree(pool->buffers);
            zfree(pool->available);
            zfree(pool);
            return NULL;
        }
        pool->available[i] = 1;
    }
    
    pool->pool_hits = 0;
    pool->pool_misses = 0;
    
    return pool;
}

void *get_buffer_from_pool(uring_buffer_pool *pool) {
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

void return_buffer_to_pool(uring_buffer_pool *pool, void *buffer) {
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

void free_buffer_pool(uring_buffer_pool *pool) {
    if (!pool) return;
    
    for (int i = 0; i < pool->count; i++) {
        zfree(pool->buffers[i]);
    }
    zfree(pool->buffers);
    zfree(pool->available);
    zfree(pool);
}
```

**Verification checklist**:
- [ ] Buffer allocation/deallocation works correctly
- [ ] Pool hit rate is reasonable (>80% under normal load)
- [ ] No buffer leaks or double-frees
- [ ] Pool handles exhaustion gracefully
- [ ] Thread-safety not required (single-threaded Redis)

**Test command**:
```bash
# Create buffer pool test
cat > test_buffers.c << 'EOF'
#include "ae_uring.h"
int main() {
    uring_buffer_pool *pool = create_buffer_pool(10, 4096);
    void *buf1 = get_buffer_from_pool(pool);
    void *buf2 = get_buffer_from_pool(pool);
    return_buffer_to_pool(pool, buf1);
    void *buf3 = get_buffer_from_pool(pool);
    printf("Pool hits: %lu, misses: %lu\n", pool->pool_hits, pool->pool_misses);
    free_buffer_pool(pool);
    return 0;
}
EOF
gcc -I. -o test_buffers test_buffers.c ae_uring.c zmalloc.c && ./test_buffers
```

### Step 2.3: Operation Management (Team Member C)

**Objective**: Implement operation creation and tracking

**Files to modify**:
- `src/ae_uring.c` (add operation functions)

**Implementation**:
```c
/* Operation management */
uring_operation *create_operation(int fd, uring_op_type_t op_type) {
    uring_operation *op = zmalloc(sizeof(uring_operation));
    
    op->fd = fd;
    op->op_type = op_type;
    op->buffer = NULL;
    op->buffer_size = 0;
    op->completion_handler = NULL;
    op->persistent = 0;
    op->submit_time = getMonotonicUs();
    
    return op;
}

void free_operation(uring_operation *op) {
    if (!op) return;
    
    /* Return buffer to pool if it was allocated */
    if (op->buffer) {
        /* Note: This requires access to the buffer pool */
        zfree(op->buffer);  /* Simplified - should use return_buffer_to_pool */
    }
    
    zfree(op);
}

int submit_operation(aeApiState *state, uring_operation *op) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        state->stats.ops_failed++;
        return -1;
    }
    
    /* Setup operation based on type */
    switch (op->op_type) {
        case URING_OP_READ:
            io_uring_prep_recv(sqe, op->fd, op->buffer, op->buffer_size, 0);
            break;
        case URING_OP_WRITE:
            io_uring_prep_send(sqe, op->fd, op->buffer, op->buffer_size, 0);
            break;
        case URING_OP_ACCEPT:
            io_uring_prep_accept(sqe, op->fd, NULL, NULL, 0);
            break;
        default:
            return -1;
    }
    
    io_uring_sqe_set_data(sqe, op);
    state->stats.ops_submitted++;
    
    /* Track operation by FD */
    if (op->fd >= 0 && op->fd < state->max_operations) {
        state->operations[op->fd] = op;
    }
    
    return 0;
}
```

**Verification checklist**:
- [ ] Operations are created and freed correctly
- [ ] Operation submission works for all types
- [ ] Operation tracking by FD works
- [ ] No memory leaks in operation lifecycle
- [ ] Statistics are updated correctly

**Test command**:
```bash
# Create operation test
cat > test_operations.c << 'EOF'
#include "ae_uring.h"
int main() {
    uring_operation *op = create_operation(5, URING_OP_READ);
    printf("Created operation: fd=%d, type=%d\n", op->fd, op->op_type);
    free_operation(op);
    return 0;
}
EOF
gcc -I. -o test_operations test_operations.c ae_uring.c zmalloc.c && ./test_operations
```

## Phase 3: Integration (Week 3)

### Step 3.1: Event Loop Integration (Team Member A)

**Objective**: Implement the main polling function

**Files to modify**:
- `src/ae_uring.c` (add aeApiPoll_uring)
- `src/ae.c` (integrate uring backend)

**Implementation**:
```c
/* Main polling function */
int aeApiPoll_uring(aeEventLoop *eventLoop, struct timeval *tvp) {
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
    
    /* Submit any pending operations */
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

static int process_completion(aeEventLoop *eventLoop, struct io_uring_cqe *cqe, int *numevents) {
    uring_operation *op = (uring_operation *)io_uring_cqe_get_data(cqe);
    int result = cqe->res;
    
    if (!op) return 0;
    
    /* Update statistics */
    aeApiState *state = eventLoop->apidata;
    if (result >= 0) {
        state->stats.ops_completed++;
    } else {
        state->stats.ops_failed++;
    }
    
    /* Call operation-specific completion handler */
    if (op->completion_handler) {
        op->completion_handler(op, result);
    }
    
    /* Auto-resubmit persistent operations */
    if (op->persistent && result >= 0) {
        submit_operation(state, op);
    } else {
        /* Clear operation tracking */
        if (op->fd >= 0 && op->fd < state->max_operations) {
            state->operations[op->fd] = NULL;
        }
        free_operation(op);
    }
    
    (*numevents)++;
    return 0;
}
```

**Verification checklist**:
- [ ] Polling function integrates with existing event loop
- [ ] Timeouts work correctly
- [ ] Completion processing handles all operation types
- [ ] Statistics are updated accurately
- [ ] No infinite loops or deadlocks

**Test command**:
```bash
# Integration test with minimal Redis
make USE_URING=yes && ./src/redis-server --port 0 --save "" --appendonly no
```

### Step 3.2: Connection Handler Integration (Team Member B)

**Objective**: Integrate io_uring with Redis connection handling

**Files to modify**:
- `src/connection.h`
- `src/connection.c`
- `src/networking.c`

**Implementation**:
```c
/* Add to connection.h */
typedef struct connection {
    /* Existing fields */
    int fd;
    void (*read_handler)(struct connection *conn);
    void (*write_handler)(struct connection *conn);
    void (*close_handler)(struct connection *conn);

    /* NEW: io_uring specific fields */
    void *uring_read_buffer;
    size_t uring_read_size;
    int conn_type;  /* CLIENT, REPLICA, CLUSTER_BUS */

    /* Existing fields continue... */
} connection;

/* Modified read handler for io_uring */
static void readQueryFromClient_uring(connection *conn) {
    client *c = connGetPrivateData(conn);

    /* Access pre-read data from io_uring */
    void *buf = conn->uring_read_buffer;
    int nread = conn->uring_read_size;

    if (!buf || nread <= 0) return;

    /* Process the data (existing Redis logic) */
    if (c->querybuf_peak < nread) c->querybuf_peak = nread;
    c->querybuf = sdscatlen(c->querybuf, buf, nread);
    c->lastinteraction = server.unixtime;

    /* Clear buffer reference */
    conn->uring_read_buffer = NULL;
    conn->uring_read_size = 0;

    /* Process commands (existing logic) */
    processInputBuffer(c);
}

/* Modified write handler for io_uring */
static void sendReplyToClient_uring(connection *conn) {
    client *c = connGetPrivateData(conn);

    if (c->bufpos > 0) {
        /* Queue write operation instead of direct write */
        submit_write_operation(server.el->apidata, conn->fd,
                              c->buf, c->bufpos);
        c->bufpos = 0;  /* Mark as queued */
    }

    /* Handle reply list if present */
    if (listLength(c->reply) > 0) {
        /* Process reply list... */
    }
}
```

**Verification checklist**:
- [ ] Connection structure modifications compile
- [ ] Read handler processes io_uring data correctly
- [ ] Write handler queues operations properly
- [ ] Existing Redis command processing works unchanged
- [ ] No regression in connection lifecycle

**Test command**:
```bash
# Test basic Redis commands with io_uring
make USE_URING=yes && ./src/redis-server --port 6380 &
sleep 1
echo "PING" | nc localhost 6380
echo "SET test value" | nc localhost 6380
echo "GET test" | nc localhost 6380
pkill redis-server
```

### Step 3.3: Completion Handlers (Team Member C)

**Objective**: Implement specific completion handlers for each operation type

**Files to modify**:
- `src/ae_uring.c` (add completion handlers)

**Implementation**:
```c
/* Accept completion handler */
static void handle_accept_completion(uring_operation *op, int result) {
    if (result < 0) {
        if (result != -EAGAIN && result != -EWOULDBLOCK) {
            serverLog(LL_WARNING, "Accept failed: %s", strerror(-result));
        }
        return;
    }

    int client_fd = result;

    /* Create new client connection (existing Redis logic) */
    connection *conn = connCreateAcceptedSocket(server.el, client_fd, NULL);
    if (!conn) {
        close(client_fd);
        return;
    }

    /* Set up io_uring specific handlers */
    conn->read_handler = readQueryFromClient_uring;
    conn->write_handler = sendReplyToClient_uring;
    conn->close_handler = freeClient_uring;

    /* Start reading from new client */
    submit_read_operation(server.el->apidata, client_fd);
}

/* Read completion handler */
static void handle_read_completion(uring_operation *op, int result) {
    aeApiState *state = server.el->apidata;

    if (result > 0) {
        /* Data received - set up for connection processing */
        connection *conn = connByFd(op->fd);
        if (conn && conn->read_handler) {
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

        /* Resubmit read operation */
        op->buffer = get_buffer_from_pool(state->buffer_pool);
        if (op->buffer) {
            op->buffer_size = state->buffer_pool->size;
            submit_operation(state, op);
        }
    } else if (result == 0) {
        /* Connection closed */
        connection *conn = connByFd(op->fd);
        if (conn && conn->close_handler) {
            conn->close_handler(conn);
        }
        op->persistent = 0;  /* Stop resubmitting */
    } else {
        /* Error occurred */
        if (result != -EAGAIN && result != -EWOULDBLOCK) {
            serverLog(LL_DEBUG, "Read error on fd %d: %s", op->fd, strerror(-result));
        }
        op->persistent = 0;  /* Stop resubmitting */
    }
}

/* Write completion handler */
static void handle_write_completion(uring_operation *op, int result) {
    if (result > 0) {
        /* Write successful */
        connection *conn = connByFd(op->fd);
        if (conn && conn->write_handler) {
            /* Notify that write completed */
            conn->write_handler(conn);
        }
    } else if (result < 0 && result != -EAGAIN && result != -EWOULDBLOCK) {
        /* Write error */
        serverLog(LL_DEBUG, "Write error on fd %d: %s", op->fd, strerror(-result));
        connection *conn = connByFd(op->fd);
        if (conn && conn->close_handler) {
            conn->close_handler(conn);
        }
    }
    /* Write operations are one-shot, don't resubmit */
}
```

**Verification checklist**:
- [ ] Accept handler creates connections correctly
- [ ] Read handler processes data and resubmits
- [ ] Write handler completes without errors
- [ ] Error conditions are handled gracefully
- [ ] Connection lifecycle is maintained

**Test command**:
```bash
# Test with multiple concurrent connections
make USE_URING=yes && ./src/redis-server --port 6380 &
sleep 1
for i in {1..10}; do
    (echo "SET key$i value$i"; echo "GET key$i") | nc localhost 6380 &
done
wait
pkill redis-server
```

## Phase 4: Testing and Validation (Week 4)

### Step 4.1: Unit Tests (Team Member D)

**Objective**: Create comprehensive unit tests for io_uring components

**Files to create**:
- `tests/unit/test_uring_buffers.c`
- `tests/unit/test_uring_operations.c`
- `tests/unit/test_uring_integration.c`

**Implementation**:
```c
/* tests/unit/test_uring_buffers.c */
#include "test_helpers.h"
#include "ae_uring.h"

void test_buffer_pool_creation() {
    uring_buffer_pool *pool = create_buffer_pool(10, 4096);
    test_assert(pool != NULL);
    test_assert(pool->count == 10);
    test_assert(pool->size == 4096);
    free_buffer_pool(pool);
}

void test_buffer_allocation() {
    uring_buffer_pool *pool = create_buffer_pool(2, 1024);

    void *buf1 = get_buffer_from_pool(pool);
    void *buf2 = get_buffer_from_pool(pool);
    void *buf3 = get_buffer_from_pool(pool);  /* Should allocate new */

    test_assert(buf1 != NULL);
    test_assert(buf2 != NULL);
    test_assert(buf3 != NULL);
    test_assert(buf1 != buf2);

    test_assert(pool->pool_hits == 2);
    test_assert(pool->pool_misses == 1);

    return_buffer_to_pool(pool, buf1);
    void *buf4 = get_buffer_from_pool(pool);
    test_assert(buf4 == buf1);  /* Should reuse */

    free_buffer_pool(pool);
}

int main() {
    test_buffer_pool_creation();
    test_buffer_allocation();
    printf("Buffer pool tests: PASSED\n");
    return 0;
}
```

**Verification checklist**:
- [ ] All unit tests pass
- [ ] Code coverage >80% for new io_uring code
- [ ] Tests run in CI/CD pipeline
- [ ] Memory leak detection (valgrind) passes
- [ ] Performance benchmarks show improvement

**Test command**:
```bash
# Run unit tests
make test-uring
# Run with memory checking
valgrind --leak-check=full ./tests/unit/test_uring_buffers
```

### Step 4.2: Integration Tests (Team Member D)

**Objective**: Test io_uring with real Redis workloads

**Files to create**:
- `tests/integration/test_uring_redis.py`
- `tests/integration/test_uring_cluster.py`

**Implementation**:
```python
# tests/integration/test_uring_redis.py
import redis
import threading
import time
import subprocess

def test_basic_operations():
    """Test basic Redis operations with io_uring"""
    # Start Redis with io_uring
    proc = subprocess.Popen([
        './src/redis-server',
        '--port', '6381',
        '--uring-enabled', 'yes'
    ])
    time.sleep(1)

    try:
        r = redis.Redis(port=6381)

        # Test basic operations
        assert r.ping() == True
        assert r.set('test', 'value') == True
        assert r.get('test') == b'value'
        assert r.incr('counter') == 1
        assert r.incr('counter') == 2

        # Test pipeline
        pipe = r.pipeline()
        for i in range(100):
            pipe.set(f'key{i}', f'value{i}')
        pipe.execute()

        # Verify all keys
        for i in range(100):
            assert r.get(f'key{i}') == f'value{i}'.encode()

        print("Basic operations: PASSED")

    finally:
        proc.terminate()
        proc.wait()

def test_concurrent_clients():
    """Test multiple concurrent clients"""
    proc = subprocess.Popen([
        './src/redis-server',
        '--port', '6382',
        '--uring-enabled', 'yes'
    ])
    time.sleep(1)

    def client_worker(client_id):
        r = redis.Redis(port=6382)
        for i in range(100):
            key = f'client{client_id}_key{i}'
            value = f'client{client_id}_value{i}'
            r.set(key, value)
            assert r.get(key) == value.encode()

    try:
        threads = []
        for i in range(10):
            t = threading.Thread(target=client_worker, args=(i,))
            threads.append(t)
            t.start()

        for t in threads:
            t.join()

        print("Concurrent clients: PASSED")

    finally:
        proc.terminate()
        proc.wait()

if __name__ == '__main__':
    test_basic_operations()
    test_concurrent_clients()
    print("All integration tests: PASSED")
```

**Verification checklist**:
- [ ] All Redis commands work correctly
- [ ] Performance is equal or better than epoll
- [ ] No data corruption under load
- [ ] Graceful handling of connection failures
- [ ] Memory usage is reasonable

**Test command**:
```bash
# Run integration tests
python3 tests/integration/test_uring_redis.py
# Run Redis test suite with io_uring
make test USE_URING=yes
```

## Phase 5: Production Readiness (Week 5)

### Step 5.1: Performance Optimization (Team Member A)

**Objective**: Optimize io_uring performance for production

**Files to modify**:
- `src/ae_uring.c` (add optimizations)

**Implementation**:
```c
/* Add multishot accept support */
static int setup_multishot_accept(aeApiState *state, int listen_fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) return -1;

    uring_operation *op = create_operation(listen_fd, URING_OP_ACCEPT);
    op->completion_handler = handle_accept_completion;
    op->persistent = 1;

    /* Use multishot accept if available */
    #ifdef IORING_ACCEPT_MULTISHOT
    io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
    #else
    io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);
    #endif

    io_uring_sqe_set_data(sqe, op);
    return submit_operation(state, op);
}

/* Add provided buffer support */
static int setup_provided_buffers(aeApiState *state) {
    #ifdef IORING_FEAT_BUF_RING
    /* Use provided buffer rings for zero-copy */
    int buffer_count = 1024;
    int buffer_size = 4096;

    void *buffer_base = mmap(NULL, buffer_count * buffer_size,
                            PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (buffer_base == MAP_FAILED) return -1;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    io_uring_prep_provide_buffers(sqe, buffer_base, buffer_size,
                                 buffer_count, 0, 0);

    return 0;
    #else
    return 0;  /* Not supported */
    #endif
}
```

**Verification checklist**:
- [ ] Multishot accept reduces syscall overhead
- [ ] Provided buffers improve memory efficiency
- [ ] SQPOLL configuration is optimal
- [ ] Latency improvements are measurable
- [ ] CPU usage is reduced

**Test command**:
```bash
# Performance benchmark
redis-benchmark -p 6380 -t set,get -n 100000 -c 50
```

### Step 5.2: Monitoring and Observability (Team Member B)

**Objective**: Add comprehensive monitoring for io_uring

**Files to modify**:
- `src/info.c` (add io_uring stats)
- `src/ae_uring.c` (add detailed metrics)

**Implementation**:
```c
/* Add to info.c */
void addUringInfoSection(sds *info) {
    if (server.uring_enabled) {
        aeApiState *state = server.el->apidata;
        uring_stats *stats = &state->stats;

        *info = sdscatprintf(*info,
            "# io_uring\r\n"
            "uring_enabled:1\r\n"
            "uring_sqpoll:%s\r\n"
            "uring_ops_submitted:%lu\r\n"
            "uring_ops_completed:%lu\r\n"
            "uring_ops_failed:%lu\r\n"
            "uring_buffer_pool_hits:%lu\r\n"
            "uring_buffer_pool_misses:%lu\r\n"
            "uring_buffer_pool_hit_rate:%.2f\r\n",
            server.uring_sqpoll ? "yes" : "no",
            stats->ops_submitted,
            stats->ops_completed,
            stats->ops_failed,
            stats->buffer_pool_hits,
            stats->buffer_pool_misses,
            (stats->buffer_pool_hits + stats->buffer_pool_misses) > 0 ?
                (double)stats->buffer_pool_hits /
                (stats->buffer_pool_hits + stats->buffer_pool_misses) * 100.0 : 0.0);
    } else {
        *info = sdscatprintf(*info,
            "# io_uring\r\n"
            "uring_enabled:0\r\n");
    }
}
```

**Verification checklist**:
- [ ] INFO command shows io_uring statistics
- [ ] Metrics are accurate and useful
- [ ] Performance impact of monitoring is minimal
- [ ] Logs provide adequate debugging information
- [ ] Alerts can be configured for issues

**Test command**:
```bash
# Check monitoring
./src/redis-cli INFO uring
./src/redis-cli CONFIG GET "*uring*"
```

## Deployment Checklist

### Pre-deployment Validation
- [ ] All unit tests pass
- [ ] Integration tests pass with real workloads
- [ ] Performance benchmarks show improvement
- [ ] Memory usage is acceptable
- [ ] No regressions in existing functionality
- [ ] Documentation is complete and accurate

### Production Deployment
- [ ] Kernel version supports io_uring (5.1+)
- [ ] liburing is installed and compatible
- [ ] Configuration is optimized for workload
- [ ] Monitoring is configured
- [ ] Rollback plan is prepared
- [ ] Team is trained on new features

### Post-deployment Monitoring
- [ ] Monitor io_uring statistics
- [ ] Watch for error rates
- [ ] Verify performance improvements
- [ ] Check memory usage patterns
- [ ] Monitor SQPOLL thread behavior

This implementation guide provides a complete roadmap for implementing Redis io_uring support with a team of 3-4 developers. Each step is independently verifiable and includes specific test commands and verification criteria.
