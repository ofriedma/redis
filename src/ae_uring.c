/* Linux io_uring based ae.c module
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "ae.h"

#ifdef HAVE_LIBURING

#include "ae_uring.h"
#include "anet.h"
#include "redisassert.h"
#include "zmalloc.h"

/* Only include server.h if we're building the server */
#ifndef REDIS_CLI_BUILD
#include "server.h"
#include "sds.h"
#endif

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

/* Include buffer management functions */
#include "uring_buffer.c"

/* Forward declarations for static functions */
static int detect_uring_capabilities(void);
static int setup_sqpoll(aeApiState *state, struct io_uring_params *params);

static void free_op_context(uring_op_context *ctx);
static int submit_write_operation(aeApiState *state, int fd, uring_op_context *ctx);
static int process_completion(aeEventLoop *eventLoop, struct io_uring_cqe *cqe, int *numevents);
static void handle_read_completion(aeEventLoop *eventLoop, uring_op_context *ctx, int result, int *numevents);
static void handle_write_completion(aeEventLoop *eventLoop, uring_op_context *ctx, int result, int *numevents);
static void handle_accept_completion(aeEventLoop *eventLoop, uring_op_context *ctx, int result, int *numevents);

static void get_uring_stats(aeApiState *state, char **info);
static const char *uring_op_type_to_string(int op_type);
static void update_operation_stats(aeApiState *state, uring_op_context *ctx, int result);

/* Detect io_uring capabilities at runtime */
static int detect_uring_capabilities(void) {
    struct io_uring ring;
    struct io_uring_params params = {0};
    int capabilities = URING_CAP_NONE;

    /* Test basic io_uring support */
    if (io_uring_queue_init_params(32, &ring, &params) < 0) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "io_uring basic support test failed: %s", strerror(errno));
        #endif
        return URING_CAP_NONE;
    }

    capabilities |= URING_CAP_BASIC;

    /* Check for supported features from params */
    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        capabilities |= URING_CAP_SINGLE_MMAP;
    }
    if (params.features & IORING_FEAT_NODROP) {
        capabilities |= URING_CAP_NODROP;
    }
    if (params.features & IORING_FEAT_SUBMIT_STABLE) {
        capabilities |= URING_CAP_SUBMIT_STABLE;
    }
    if (params.features & IORING_FEAT_RW_CUR_POS) {
        capabilities |= URING_CAP_RW_CUR_POS;
    }
    if (params.features & IORING_FEAT_CUR_PERSONALITY) {
        capabilities |= URING_CAP_CUR_PERSONALITY;
    }
    if (params.features & IORING_FEAT_FAST_POLL) {
        capabilities |= URING_CAP_FAST_POLL;
    }

    io_uring_queue_exit(&ring);

    /* Test SQPOLL support */
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 1000; /* 1 second idle timeout for test */

    if (io_uring_queue_init_params(32, &ring, &params) == 0) {
        capabilities |= URING_CAP_SQPOLL;
        io_uring_queue_exit(&ring);
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_DEBUG, "SQPOLL support detected");
        #endif
    } else {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_DEBUG, "SQPOLL not available: %s", strerror(errno));
        #endif
    }

    /* Test buffer ring support (requires newer kernel/liburing) */
    memset(&params, 0, sizeof(params));
    if (io_uring_queue_init_params(32, &ring, &params) == 0) {
        /* Try to register a small buffer ring to test support */
        struct io_uring_buf_ring *br = NULL;
        size_t ring_size = 64 * 16; /* Small test ring */

        br = mmap(NULL, ring_size, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (br != MAP_FAILED) {
            /* Test buffer ring registration - this will fail on older kernels */
            struct io_uring_buf_reg reg = {
                .ring_addr = (unsigned long)br,
                .ring_entries = 16,
                .bgid = 0
            };
            if (io_uring_register_buf_ring(&ring, &reg, 0) == 0) {
                capabilities |= URING_CAP_BUFFER_RING;
                io_uring_unregister_buf_ring(&ring, 0);
                #ifndef REDIS_CLI_BUILD
                serverLog(LL_DEBUG, "Buffer ring support detected");
                #endif
            }
            munmap(br, ring_size);
        }
        io_uring_queue_exit(&ring);
    }

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_NOTICE, "io_uring capabilities detected: 0x%x", capabilities);
    #endif

    return capabilities;
}

/* Setup SQPOLL with optimal configuration */
static int setup_sqpoll(aeApiState *state, struct io_uring_params *params) {
    #ifndef REDIS_CLI_BUILD
    if (!server.uring_config.sqpoll_enabled) {
        return 0;
    }

    params->flags |= IORING_SETUP_SQPOLL;

    /* Set CPU affinity if specified */
    if (server.uring_config.sqpoll_cpu >= 0) {
        /* Validate CPU number */
        long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
        if (server.uring_config.sqpoll_cpu >= nprocs) {
            serverLog(LL_WARNING, "SQPOLL CPU %d exceeds available CPUs (%ld), using auto-select",
                      server.uring_config.sqpoll_cpu, nprocs);
        } else {
            params->flags |= IORING_SETUP_SQ_AFF;
            params->sq_thread_cpu = server.uring_config.sqpoll_cpu;
            state->sqpoll_cpu = server.uring_config.sqpoll_cpu;
        }
    } else if (server.uring_config.sqpoll_cpu == -1) {
        /* Auto-select optimal CPU - prefer last CPU to avoid main thread */
        long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
        if (nprocs > 1) {
            params->flags |= IORING_SETUP_SQ_AFF;
            params->sq_thread_cpu = nprocs - 1;
            state->sqpoll_cpu = nprocs - 1;
            serverLog(LL_DEBUG, "Auto-selected CPU %ld for SQPOLL", nprocs - 1);
        }
    }

    /* Set idle timeout with bounds checking */
    int idle_ms = server.uring_config.sqpoll_idle_ms;
    if (idle_ms < 100) {
        serverLog(LL_WARNING, "SQPOLL idle timeout %d too low, using 100ms", idle_ms);
        idle_ms = 100;
    } else if (idle_ms > 60000) {
        serverLog(LL_WARNING, "SQPOLL idle timeout %d too high, using 60000ms", idle_ms);
        idle_ms = 60000;
    }
    params->sq_thread_idle = idle_ms;
    state->sqpoll_idle_ms = idle_ms;

    serverLog(LL_DEBUG, "SQPOLL configured: cpu=%d, idle=%dms",
              state->sqpoll_cpu, state->sqpoll_idle_ms);

    return 1;
    #else
    /* SQPOLL disabled for CLI builds */
    return 0;
    #endif
}



/* Create io_uring backend */
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));
    if (!state) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Failed to allocate memory for io_uring state");
        #endif
        return -1;
    }

    memset(state, 0, sizeof(aeApiState));

    /* Detect capabilities */
    state->capabilities = detect_uring_capabilities();
    if (!(state->capabilities & URING_CAP_BASIC)) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "io_uring basic capabilities not available - Redis will exit");
        serverLog(LL_WARNING, "This may be due to:");
        serverLog(LL_WARNING, "  - Kernel version < 5.1 (io_uring not supported)");
        serverLog(LL_WARNING, "  - liburing not installed or too old");
        serverLog(LL_WARNING, "  - Insufficient permissions");
        serverLog(LL_WARNING, "  - System resource limits");
        serverLog(LL_WARNING, "To use Redis without io_uring, recompile without HAVE_LIBURING");
        #endif
        zfree(state);
        return -1;
    }

    /* Configure queue sizes with validation */
    #ifndef REDIS_CLI_BUILD
    int configured_sq = server.uring_config.sq_entries;
    int configured_cq = server.uring_config.cq_entries;
    #else
    int configured_sq = 0;
    int configured_cq = 0;
    #endif

    /* Calculate optimal queue sizes */
    int min_sq_entries = eventLoop->setsize;
    int max_sq_entries = 32768; /* Reasonable upper limit */

    if (configured_sq > 0) {
        state->sq_entries = configured_sq;
        /* Validate configured size */
        if (state->sq_entries < min_sq_entries) {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "Configured SQ entries %d too small, using %d",
                      configured_sq, min_sq_entries);
            #endif
            state->sq_entries = min_sq_entries;
        } else if (state->sq_entries > max_sq_entries) {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "Configured SQ entries %d too large, using %d",
                      configured_sq, max_sq_entries);
            #endif
            state->sq_entries = max_sq_entries;
        }
    } else {
        /* Auto-calculate based on event loop size */
        state->sq_entries = eventLoop->setsize * 2;
        if (state->sq_entries < 64) state->sq_entries = 64;
        if (state->sq_entries > max_sq_entries) state->sq_entries = max_sq_entries;
    }

    /* Ensure SQ entries is power of 2 for optimal performance */
    if (state->sq_entries & (state->sq_entries - 1)) {
        int orig = state->sq_entries;
        state->sq_entries--;
        state->sq_entries |= state->sq_entries >> 1;
        state->sq_entries |= state->sq_entries >> 2;
        state->sq_entries |= state->sq_entries >> 4;
        state->sq_entries |= state->sq_entries >> 8;
        state->sq_entries |= state->sq_entries >> 16;
        state->sq_entries++;
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_DEBUG, "Rounded SQ entries from %d to %d (power of 2)", orig, state->sq_entries);
        #endif
    }

    /* Configure CQ entries */
    if (configured_cq > 0) {
        state->cq_entries = configured_cq;
        if (state->cq_entries < state->sq_entries) {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "CQ entries %d smaller than SQ entries %d, adjusting",
                      state->cq_entries, state->sq_entries);
            #endif
            state->cq_entries = state->sq_entries * 2;
        }
    } else {
        /* CQ should be larger than SQ to handle completion bursts */
        state->cq_entries = state->sq_entries * 2;
    }

    /* Ensure CQ entries is reasonable */
    if (state->cq_entries > 65536) {
        state->cq_entries = 65536;
    }

    /* Setup io_uring parameters */
    struct io_uring_params params = {0};
    params.cq_entries = state->cq_entries;

    /* Configure SQPOLL if supported and enabled */
    if (state->capabilities & URING_CAP_SQPOLL) {
        state->sqpoll_enabled = setup_sqpoll(state, &params);
    }

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Initializing io_uring: sq_entries=%d, cq_entries=%d, sqpoll=%s",
              state->sq_entries, state->cq_entries,
              state->sqpoll_enabled ? "yes" : "no");
    #endif

    /* Initialize io_uring */
    int ret = io_uring_queue_init_params(state->sq_entries, &state->ring, &params);
    if (ret < 0) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "io_uring_queue_init_params failed: %s (sq=%d, cq=%d, sqpoll=%s)",
                  strerror(-ret), state->sq_entries, state->cq_entries,
                  state->sqpoll_enabled ? "yes" : "no");
        #endif

        /* Try without SQPOLL if it failed */
        if (state->sqpoll_enabled) {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_NOTICE, "Retrying io_uring initialization without SQPOLL");
            #endif
            state->sqpoll_enabled = 0;
            memset(&params, 0, sizeof(params));
            params.cq_entries = state->cq_entries;
            ret = io_uring_queue_init_params(state->sq_entries, &state->ring, &params);
        }

        if (ret < 0) {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "io_uring initialization failed completely: %s", strerror(-ret));
            #endif
            zfree(state);
            return -1;
        } else {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_NOTICE, "io_uring initialized without SQPOLL after retry");
            #endif
        }
    }

    /* Validate actual queue sizes */
    if (params.sq_entries != state->sq_entries) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_NOTICE, "Actual SQ entries: %d (requested: %d)",
                  params.sq_entries, state->sq_entries);
        #endif
        state->sq_entries = params.sq_entries;
    }
    if (params.cq_entries != state->cq_entries) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_NOTICE, "Actual CQ entries: %d (requested: %d)",
                  params.cq_entries, state->cq_entries);
        #endif
        state->cq_entries = params.cq_entries;
    }

    /* Log supported features */
    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "io_uring features: 0x%x", params.features);
    #endif

    state->ring_fd = state->ring.ring_fd;
    if (anetCloexec(state->ring_fd) == ANET_ERR) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Failed to set CLOEXEC on io_uring fd");
        #endif
    }

    /* Setup buffer ring if supported */
    if (state->capabilities & URING_CAP_BUFFER_RING) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_DEBUG, "Attempting to setup buffer ring");
        #endif
        if (setup_buffer_ring(state) == 0) {
            state->buffer_ring_enabled = 1;
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_DEBUG, "Buffer ring setup successful");
            #endif
        } else {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_DEBUG, "Buffer ring setup failed, using regular allocation");
            #endif
        }
    } else {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_DEBUG, "Buffer ring not supported by kernel");
        #endif
    }

    /* Initialize context tracking with validation */
    int max_contexts = eventLoop->setsize;
    if (max_contexts < 64) max_contexts = 64;
    if (max_contexts > 65536) max_contexts = 65536;

    state->contexts = zmalloc(sizeof(uring_op_context*) * max_contexts);
    if (!state->contexts) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Failed to allocate context tracking array");
        #endif
        io_uring_queue_exit(&state->ring);
        zfree(state);
        return -1;
    }
    state->max_contexts = max_contexts;
    memset(state->contexts, 0, sizeof(uring_op_context*) * max_contexts);

    /* Initialize connection tracking */
    state->connections = zmalloc(sizeof(uring_conn_context*) * max_contexts);
    if (!state->connections) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Failed to allocate connection tracking array");
        #endif
        zfree(state->contexts);
        io_uring_queue_exit(&state->ring);
        zfree(state);
        return -1;
    }
    state->max_connections = max_contexts;
    state->active_connections = 0;
    state->conn_list_head = NULL;
    state->conn_list_tail = NULL;
    memset(state->connections, 0, sizeof(uring_conn_context*) * max_contexts);

    /* Initialize batch submission mutex */
    if (pthread_mutex_init(&state->batch_lock, NULL) != 0) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Failed to initialize batch submission mutex");
        #endif
        zfree(state->connections);
        zfree(state->contexts);
        io_uring_queue_exit(&state->ring);
        zfree(state);
        return -1;
    }

    /* Initialize connection tracking mutex */
    if (pthread_mutex_init(&state->conn_lock, NULL) != 0) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Failed to initialize connection tracking mutex");
        #endif
        pthread_mutex_destroy(&state->batch_lock);
        zfree(state->connections);
        zfree(state->contexts);
        io_uring_queue_exit(&state->ring);
        zfree(state);
        return -1;
    }

    /* Initialize operation tracking */
    state->op_list_head = NULL;
    state->op_list_tail = NULL;
    state->next_op_id = 1;
    state->next_sequence_num = 1;
    state->active_operations = 0;
    state->max_operations = max_contexts * 2; /* Allow more operations than contexts */

    /* Initialize operation tracking mutex */
    if (pthread_mutex_init(&state->op_lock, NULL) != 0) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Failed to initialize operation tracking mutex");
        #endif
        pthread_mutex_destroy(&state->conn_lock);
        pthread_mutex_destroy(&state->batch_lock);
        zfree(state->connections);
        zfree(state->contexts);
        io_uring_queue_exit(&state->ring);
        zfree(state);
        return -1;
    }

    /* Initialize priority queues */
    for (int i = 0; i < 4; i++) {
        state->priority_queues[i].head = NULL;
        state->priority_queues[i].tail = NULL;
        state->priority_queues[i].count = 0;
    }

    /* Initialize statistics */
    memset(&state->stats, 0, sizeof(state->stats));

    eventLoop->apidata = state;

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_NOTICE, "io_uring initialized successfully: sq_entries=%d, cq_entries=%d, sqpoll=%s, buffer_ring=%s, contexts=%d",
              state->sq_entries, state->cq_entries,
              state->sqpoll_enabled ? "yes" : "no",
              state->buffer_ring_enabled ? "yes" : "no",
              state->max_contexts);
    #endif

    return 0;
}

/* Resize io_uring backend */
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;
    
    if (setsize <= state->max_contexts) {
        return 0;
    }
    
    /* Resize context array */
    uring_op_context **new_contexts = zrealloc(state->contexts, 
                                               sizeof(uring_op_context*) * setsize);
    if (!new_contexts) {
        return -1;
    }
    
    /* Initialize new entries */
    memset(new_contexts + state->max_contexts, 0, 
           sizeof(uring_op_context*) * (setsize - state->max_contexts));
    
    state->contexts = new_contexts;
    state->max_contexts = setsize;
    
    return 0;
}

/* Free io_uring backend */
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;
    if (!state) return;
    

    
    /* Clean up buffer ring */
    if (state->buffer_ring_enabled) {
        cleanup_buffer_ring(state);
    }
    
    /* Clean up connection contexts */
    if (state->connections) {
        for (int i = 0; i < state->max_connections; i++) {
            if (state->connections[i]) {
                cleanup_connection_operations(state, state->connections[i]);
                destroy_connection_context(state, state->connections[i]);
            }
        }
        zfree(state->connections);
    }

    /* Clean up operation contexts */
    for (int i = 0; i < state->max_contexts; i++) {
        if (state->contexts[i]) {
            free_op_context(state->contexts[i]);
        }
    }
    zfree(state->contexts);

    /* Clean up io_uring */
    io_uring_queue_exit(&state->ring);

    /* Clean up operation tracking */
    if (state->op_list_head) {
        uring_op_context *op_ctx = state->op_list_head;
        while (op_ctx) {
            uring_op_context *next = op_ctx->next;
            destroy_operation_context(state, op_ctx);
            op_ctx = next;
        }
    }

    /* Clean up mutexes */
    pthread_mutex_destroy(&state->batch_lock);
    pthread_mutex_destroy(&state->conn_lock);
    pthread_mutex_destroy(&state->op_lock);

    zfree(state);
}

/* Create operation context */
uring_op_context *create_op_context(int fd, int op_type, int mask) {
    uring_op_context *ctx = zmalloc(sizeof(uring_op_context));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(uring_op_context));
    ctx->fd = fd;
    ctx->op_type = op_type;
    ctx->mask = mask;
    ctx->submit_time = getMonotonicUs();

    /* Note: We could track context allocations here if we had access to state.
     * For now, this is tracked at the higher level where state is available. */

    return ctx;
}

/* Free operation context */
static void free_op_context(uring_op_context *ctx) {
    if (!ctx) return;
    
    if (ctx->buffer && ctx->buffer_id == -1) {
        /* Free non-buffer-ring buffer */
        zfree(ctx->buffer);
        /* Note: We need access to state for stats, but it's not available here.
         * This could be improved by passing state or storing it in context. */
    }
    
    zfree(ctx);
}

/* Helper function to check if a socket is a listening socket */
static int is_listening_socket(int fd) {
    int val;
    socklen_t len = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &val, &len) == 0) {
        return val;
    }
    return 0;
}

/* Add event to io_uring */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;

    if (fd >= state->max_contexts) {
        return -1;
    }

    uring_op_context *ctx = state->contexts[fd];
    if (!ctx) {
        /* Create new context for this FD */
        ctx = create_op_context(fd, 0, mask);
        if (!ctx) return -1;
        state->contexts[fd] = ctx;
        state->stats.context_allocations++;
    } else {
        /* Update existing context */
        ctx->mask |= mask;
    }

    /* Handle readable events */
    if ((mask & AE_READABLE) && !(ctx->read_op.active)) {
        if (is_listening_socket(fd)) {
            /* For listening sockets, submit accept operation */
            if (submit_accept_operation(state, fd, ctx) < 0) {
                return -1;
            }
            ctx->accept_op.active = 1;
        } else {
            /* For client sockets, submit read operation */
            if (submit_read_operation(state, fd, ctx) < 0) {
                return -1;
            }
            ctx->read_op.active = 1;
        }
    }

    /* Submit write operation if writable event requested */
    if ((mask & AE_WRITABLE) && !(ctx->write_op.active)) {
        if (submit_write_operation(state, fd, ctx) < 0) {
            return -1;
        }
        ctx->write_op.active = 1;
    }

    return 0;
}

/* Remove event from io_uring */
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;

    if (fd >= state->max_contexts) {
        return;
    }

    uring_op_context *ctx = state->contexts[fd];
    if (!ctx) return;

    ctx->mask &= (~delmask);

    /* Cancel operations if no longer needed */
    if (delmask & AE_READABLE) {
        if (ctx->read_op.active) {
            /* Note: io_uring doesn't have direct cancellation for pending ops
             * We'll handle this in completion processing */
            ctx->read_op.active = 0;
        }
        if (ctx->accept_op.active) {
            ctx->accept_op.active = 0;
        }
    }

    if ((delmask & AE_WRITABLE) && ctx->write_op.active) {
        ctx->write_op.active = 0;
    }

    /* Clean up context if no events remain */
    if (ctx->mask == AE_NONE) {
        free_op_context(ctx);
        state->contexts[fd] = NULL;
        state->stats.context_deallocations++;
    }
}

/* Enhanced submit read operation with error recovery */
int submit_read_operation(aeApiState *state, int fd, uring_op_context *ctx) {
    if (!state || !ctx || fd < 0) {
        return -1;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        state->stats.sq_full_count++;
        /* Add to priority queue for later submission */
        if (ctx->priority == 0) ctx->priority = URING_OP_PRIORITY_NORMAL;
        add_operation_to_queue(state, ctx);
        return -EAGAIN;
    }

    /* Get buffer for read operation */
    void *buffer = NULL;
    int buffer_id = -1;

    if (state->buffer_ring_enabled) {
        buffer = get_buffer_from_ring(state, &buffer_id);
        if (buffer && buffer != (void*)0x1) {
            /* Got actual buffer from ring */
            ctx->buffer_id = buffer_id;
            state->stats.buffer_ring_hits++;
        } else if (buffer == (void*)0x1) {
            /* Using buffer ring - kernel will provide buffer */
            ctx->buffer_id = buffer_id;
            ctx->buffer = NULL; /* Will be set from completion */
            buffer = NULL; /* Don't use in prep call */
        } else {
            state->stats.buffer_ring_misses++;
        }
    }

    if (!buffer && !state->buffer_ring_enabled) {
        /* Use optimized buffer allocation */
        buffer = allocate_optimized_buffer(URING_DEFAULT_BUFFER_SIZE);
        if (!buffer) {
            /* Memory allocation failed - retry later with lower priority */
            ctx->priority = URING_OP_PRIORITY_LOW;
            add_operation_to_queue(state, ctx);
            return -ENOMEM;
        }
        ctx->buffer_id = -1;
        state->stats.buffer_allocations++;
    }

    ctx->buffer = buffer;
    ctx->buffer_size = URING_DEFAULT_BUFFER_SIZE;

    /* Prepare read operation with enhanced options */
    if (state->buffer_ring_enabled && buffer_id >= 0) {
        io_uring_prep_recv(sqe, fd, NULL, 0, 0);
        sqe->buf_group = BUFFER_RING_ID;
        sqe->flags |= IOSQE_BUFFER_SELECT;
    } else {
        io_uring_prep_read(sqe, fd, buffer, ctx->buffer_size, 0);
    }

    /* Set operation timeout if configured */
    if (ctx->timeout_us > 0) {
        struct __kernel_timespec ts;
        ts.tv_sec = ctx->timeout_us / 1000000;
        ts.tv_nsec = (ctx->timeout_us % 1000000) * 1000;
        sqe->flags |= IOSQE_IO_LINK;

        /* Add timeout SQE */
        struct io_uring_sqe *timeout_sqe = io_uring_get_sqe(&state->ring);
        if (timeout_sqe) {
            io_uring_prep_link_timeout(timeout_sqe, &ts, 0);
            io_uring_sqe_set_data(timeout_sqe, ctx);
        }
    }

    /* Update context for read operation */
    ctx->op_type = URING_OP_READ;
    ctx->read_op.active = 1;
    ctx->read_op.last_attempt = getMonotonicUs();
    update_operation_state(ctx, URING_OP_STATE_SUBMITTED);

    io_uring_sqe_set_data(sqe, ctx);

    state->stats.ops_submitted++;
    state->stats.read_ops++;

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Submitted read operation: fd=%d, op_id=%lu, buffer_size=%zu",
              fd, ctx->op_id, ctx->buffer_size);
    #endif

    return 0;
}

/* Enhanced submit write operation with error recovery */
static int submit_write_operation(aeApiState *state, int fd, uring_op_context *ctx) {
    if (!state || !ctx || fd < 0) {
        return -1;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        state->stats.sq_full_count++;
        /* Add to priority queue for later submission */
        if (ctx->priority == 0) ctx->priority = URING_OP_PRIORITY_NORMAL;
        add_operation_to_queue(state, ctx);
        return -EAGAIN;
    }

    /* For write operations, we can either:
     * 1. Use NOP to signal write readiness (current approach)
     * 2. Use actual write operation with provided data
     * 3. Use poll for write readiness
     */

    if (ctx->buffer && ctx->buffer_size > 0) {
        /* Actual write operation with data */
        io_uring_prep_write(sqe, fd, ctx->buffer, ctx->buffer_size, 0);

        /* Set operation timeout if configured */
        if (ctx->timeout_us > 0) {
            struct __kernel_timespec ts;
            ts.tv_sec = ctx->timeout_us / 1000000;
            ts.tv_nsec = (ctx->timeout_us % 1000000) * 1000;
            sqe->flags |= IOSQE_IO_LINK;

            /* Add timeout SQE */
            struct io_uring_sqe *timeout_sqe = io_uring_get_sqe(&state->ring);
            if (timeout_sqe) {
                io_uring_prep_link_timeout(timeout_sqe, &ts, 0);
                io_uring_sqe_set_data(timeout_sqe, ctx);
            }
        }
    } else {
        /* Poll for write readiness */
        io_uring_prep_poll_add(sqe, fd, POLLOUT);
    }

    /* Update context for write operation */
    ctx->op_type = URING_OP_WRITE;
    ctx->write_op.active = 1;
    ctx->write_op.last_attempt = getMonotonicUs();
    update_operation_state(ctx, URING_OP_STATE_SUBMITTED);

    io_uring_sqe_set_data(sqe, ctx);

    state->stats.ops_submitted++;
    state->stats.write_ops++;

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Submitted write operation: fd=%d, op_id=%lu, buffer_size=%zu",
              fd, ctx->op_id, ctx->buffer_size);
    #endif

    return 0;
}

/* Enhanced submit accept operation with multishot support */
int submit_accept_operation(aeApiState *state, int fd, uring_op_context *ctx) {
    if (!state || !ctx || fd < 0) {
        return -1;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        state->stats.sq_full_count++;
        /* Add to priority queue for later submission */
        if (ctx->priority == 0) ctx->priority = URING_OP_PRIORITY_HIGH; /* Accept has high priority */
        add_operation_to_queue(state, ctx);
        return -EAGAIN;
    }

    /* Prepare client address storage */
    ctx->client_addr_len = sizeof(ctx->client_addr);

    /* Check if multishot accept is supported */
    #ifdef IORING_ACCEPT_MULTISHOT
    if (state->capabilities & URING_CAP_MULTISHOT) {
        /* Use multishot accept for better performance */
        io_uring_prep_multishot_accept(sqe, fd,
                                       (struct sockaddr*)&ctx->client_addr,
                                       &ctx->client_addr_len, 0);
        sqe->flags |= IOSQE_BUFFER_SELECT; /* Use buffer selection if available */

        #ifndef REDIS_CLI_BUILD
        serverLog(LL_DEBUG, "Using multishot accept for fd %d", fd);
        #endif
    } else
    #endif
    {
        /* Use regular accept */
        io_uring_prep_accept(sqe, fd,
                            (struct sockaddr*)&ctx->client_addr,
                            &ctx->client_addr_len, 0);
    }

    /* Set operation timeout if configured */
    if (ctx->timeout_us > 0) {
        struct __kernel_timespec ts;
        ts.tv_sec = ctx->timeout_us / 1000000;
        ts.tv_nsec = (ctx->timeout_us % 1000000) * 1000;
        sqe->flags |= IOSQE_IO_LINK;

        /* Add timeout SQE */
        struct io_uring_sqe *timeout_sqe = io_uring_get_sqe(&state->ring);
        if (timeout_sqe) {
            io_uring_prep_link_timeout(timeout_sqe, &ts, 0);
            io_uring_sqe_set_data(timeout_sqe, ctx);
        }
    }

    /* Update context for accept operation */
    ctx->op_type = URING_OP_ACCEPT;
    ctx->accept_op.active = 1;
    ctx->accept_op.last_attempt = getMonotonicUs();
    update_operation_state(ctx, URING_OP_STATE_SUBMITTED);

    io_uring_sqe_set_data(sqe, ctx);

    state->stats.ops_submitted++;
    state->stats.accept_ops++;

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Submitted accept operation: fd=%d, op_id=%lu", fd, ctx->op_id);
    #endif

    return 0;
}

/* Submit any pending operations from priority queues */
static int submit_pending_operations(aeApiState *state) {
    int submitted = 0;

    /* Process operations from priority queues */
    for (int priority = URING_OP_PRIORITY_CRITICAL; priority >= URING_OP_PRIORITY_LOW; priority--) {
        uring_op_context *op_ctx = get_next_operation(state, priority);
        if (op_ctx) {
            /* Try to submit the operation */
            struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
            if (!sqe) {
                /* SQ full - put operation back and stop */
                add_operation_to_queue(state, op_ctx);
                break;
            }

            /* Submit based on operation type */
            int result = -1;
            switch (op_ctx->op_type) {
                case URING_OP_READ:
                    result = submit_read_operation(state, op_ctx->fd, op_ctx);
                    break;
                case URING_OP_WRITE:
                    result = submit_write_operation(state, op_ctx->fd, op_ctx);
                    break;
                case URING_OP_ACCEPT:
                    result = submit_accept_operation(state, op_ctx->fd, op_ctx);
                    break;
                default:
                    break;
            }

            if (result == 0) {
                submitted++;
            } else {
                /* Submission failed - put back in queue */
                add_operation_to_queue(state, op_ctx);
            }
        }
    }

    return submitted;
}

/* Event-driven io_uring integration - NO POLLING APPROACH */
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int numevents = 0;

    /* tvp is unused in event-driven approach - timeout handled by main event loop */
    (void)tvp;

    /*
     * CRITICAL INSIGHT: Instead of using io_uring_wait_cqe_timeout() which is
     * essentially polling, we integrate io_uring with the existing event loop.
     *
     * The proper approach is:
     * 1. Submit operations to io_uring
     * 2. Add io_uring's eventfd to the main event loop (epoll/kqueue)
     * 3. Only process completions when the eventfd becomes readable
     * 4. Let the main event loop handle timeouts naturally
     */

    /* Submit any pending operations */
    int submitted = submit_pending_operations(state);
    if (submitted > 0) {
        /* Tell the kernel to start processing submitted operations */
        int submit_result = io_uring_submit(&state->ring);
        if (submit_result >= 0) {
            state->stats.ops_submitted += submit_result;
            state->stats.batch_submissions++;
        } else {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "io_uring_submit failed: %s", strerror(-submit_result));
            #endif
        }
    }

    /* Process any immediately available completions (non-blocking) */
    struct io_uring_cqe *cqe;

    /*
     * KEY: Use io_uring_for_each_cqe which only processes available completions
     * without waiting. This is the non-polling approach.
     */
    unsigned head;
    unsigned count = 0;
    unsigned max_process = eventLoop->setsize;

    io_uring_for_each_cqe(&state->ring, head, cqe) {
        int process_result = process_completion(eventLoop, cqe, &numevents);
        if (process_result < 0) {
            break;
        }
        count++;

        /* Limit processing to maintain responsiveness */
        if (count >= max_process) {
            break;
        }
    }

    /* Advance completion queue if we processed any */
    if (count > 0) {
        io_uring_cq_advance(&state->ring, count);
        state->stats.ops_completed += count;
        state->stats.completion_batches++;
    }

    /* Track completion processing calls */
    state->stats.completion_processing_calls++;

    /*
     * IMPORTANT: We return the number of events processed.
     * If there are no events, we return 0 and let the main event loop
     * handle waiting with its normal timeout mechanisms (epoll_wait, etc.).
     *
     * This eliminates the need for io_uring-specific polling!
     */

    return numevents;
}

/* Get the name of the io_uring backend */
static const char *aeApiName(void) {
    return "uring";
}

/* Enhanced process completion queue entry with robust error handling */
static int process_completion(aeEventLoop *eventLoop, struct io_uring_cqe *cqe, int *numevents) {
    aeApiState *state = eventLoop->apidata;
    uring_op_context *ctx = (uring_op_context *)io_uring_cqe_get_data(cqe);

    if (!ctx) {
        /* Completion without context - this shouldn't happen but handle gracefully */
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Received completion without context, result=%d", cqe->res);
        #endif
        return 0;
    }

    int result = cqe->res;
    monotime completion_time = getMonotonicUs();
    uint64_t operation_time = completion_time - ctx->submit_time;

    /* Validate operation context */
    if (ctx->fd < 0 || ctx->op_type < 0) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Invalid operation context: fd=%d, op_type=%d", ctx->fd, ctx->op_type);
        #endif
        free_op_context(ctx);
        return -1;
    }

    /* Update operation state */
    update_operation_state(ctx, URING_OP_STATE_PROCESSING);

    /* Update general statistics */
    state->stats.ops_completed++;
    state->stats.total_completion_time_us += operation_time;
    if (operation_time > state->stats.max_completion_time_us) {
        state->stats.max_completion_time_us = operation_time;
    }

    /* Update operation-specific statistics */
    update_operation_stats(state, ctx, result);

    /* Handle CQE flags for special cases */
    if (cqe->flags & IORING_CQE_F_MORE) {
        /* Multishot operation - more completions expected */
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_DEBUG, "Multishot operation continues: fd=%d, op_type=%d", ctx->fd, ctx->op_type);
        #endif
    }

    if (cqe->flags & IORING_CQE_F_BUFFER) {
        /* Buffer was selected from buffer ring */
        int buffer_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        ctx->buffer_id = buffer_id;
        state->stats.buffer_ring_hits++;

        #ifndef REDIS_CLI_BUILD
        serverLog(LL_DEBUG, "Buffer ring buffer selected: id=%d", buffer_id);
        #endif
    }

    /* Handle errors with detailed analysis */
    if (result < 0) {
        state->stats.ops_failed++;
        ctx->last_error = result;

        /* Categorize and handle different error types */
        switch (result) {
            case -EAGAIN:
            #if EAGAIN != EWOULDBLOCK
            case -EWOULDBLOCK:
            #endif
                state->stats.eagain_errors++;
                /* These are typically retryable - let specific handlers decide */
                break;
            case -ECONNRESET:
                state->stats.econnreset_errors++;
                /* Connection was reset by peer */
                break;
            case -EPIPE:
                state->stats.epipe_errors++;
                /* Broken pipe - connection closed */
                break;
            case -ETIMEDOUT:
                /* Operation timed out */
                update_operation_state(ctx, URING_OP_STATE_TIMEOUT);
                break;
            case -ECANCELED:
                /* Operation was cancelled */
                update_operation_state(ctx, URING_OP_STATE_CANCELLED);
                break;
            case -EBADF:
                /* Bad file descriptor - serious error */
                #ifndef REDIS_CLI_BUILD
                serverLog(LL_WARNING, "Bad file descriptor in io_uring operation: fd=%d", ctx->fd);
                #endif
                break;
            case -ENOMEM:
            case -ENOBUFS:
                /* Memory/buffer exhaustion */
                #ifndef REDIS_CLI_BUILD
                serverLog(LL_WARNING, "Resource exhaustion in io_uring: %s", strerror(-result));
                #endif
                break;
            default:
                state->stats.other_errors++;
                #ifndef REDIS_CLI_BUILD
                serverLog(LL_DEBUG, "io_uring operation failed: fd=%d, op_type=%d, error=%s",
                          ctx->fd, ctx->op_type, strerror(-result));
                #endif
                break;
        }
    }

    /* Dispatch to operation-specific completion handlers */
    int handler_result = 0;
    switch (ctx->op_type) {
        case URING_OP_READ:
            handle_read_completion(eventLoop, ctx, result, numevents);
            break;
        case URING_OP_WRITE:
            handle_write_completion(eventLoop, ctx, result, numevents);
            break;
        case URING_OP_ACCEPT:
            handle_accept_completion(eventLoop, ctx, result, numevents);
            break;
        case URING_OP_RECV:
            /* Handle recv operations similar to read */
            handle_read_completion(eventLoop, ctx, result, numevents);
            break;
        case URING_OP_SEND:
            /* Handle send operations similar to write */
            handle_write_completion(eventLoop, ctx, result, numevents);
            break;
        default:
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "Unknown io_uring operation type: %d (fd=%d)", ctx->op_type, ctx->fd);
            #endif
            /* Clean up unknown operation */
            free_op_context(ctx);
            handler_result = -1;
            break;
    }

    return handler_result;
}

/* Enhanced handle read completion with error recovery */
static void handle_read_completion(aeEventLoop *eventLoop, uring_op_context *ctx, int result, int *numevents) {
    aeApiState *state = eventLoop->apidata;

    /* Update operation state and record completion time */
    ctx->completion_time = getMonotonicUs();
    ctx->bytes_transferred = (result > 0) ? result : 0;

    if (result > 0) {
        /* Data available - fire read event */
        aeFileEvent *fe = &eventLoop->events[ctx->fd];
        if (fe->mask & AE_READABLE && fe->rfileProc) {
            eventLoop->fired[*numevents].fd = ctx->fd;
            eventLoop->fired[*numevents].mask = AE_READABLE;
            (*numevents)++;
        }

        /* Update connection context if available */
        uring_conn_context *conn_ctx = get_connection_context(state, ctx->fd);
        if (conn_ctx) {
            conn_ctx->bytes_read += result;
            conn_ctx->ops_completed++;
            update_connection_activity(conn_ctx);
            update_connection_state(conn_ctx, URING_CONN_READING);
        }

        /* For zero-copy operations, the buffer information is in the CQE */
        if (state->buffer_ring_enabled && ctx->buffer_id >= 0) {
            /* Buffer was provided by kernel via buffer ring */
            /* The actual buffer address and data are available through the completion */
            /* Application can access the data directly without copying */
        }

        update_operation_state(ctx, URING_OP_STATE_COMPLETED);

        /* Call completion callback if set */
        if (ctx->completion_callback) {
            ctx->completion_callback(ctx, result);
        }

        /* Re-submit read operation for continuous monitoring */
        if (state->contexts[ctx->fd] && (state->contexts[ctx->fd]->mask & AE_READABLE)) {
            submit_read_operation(state, ctx->fd, state->contexts[ctx->fd]);
        }
    } else if (result == 0) {
        /* Connection closed */
        aeFileEvent *fe = &eventLoop->events[ctx->fd];
        if (fe->mask & AE_READABLE && fe->rfileProc) {
            eventLoop->fired[*numevents].fd = ctx->fd;
            eventLoop->fired[*numevents].mask = AE_READABLE;
            (*numevents)++;
        }

        /* Update connection context */
        uring_conn_context *conn_ctx = get_connection_context(state, ctx->fd);
        if (conn_ctx) {
            update_connection_state(conn_ctx, URING_CONN_CLOSED);
        }

        update_operation_state(ctx, URING_OP_STATE_COMPLETED);

        /* Call completion callback if set */
        if (ctx->completion_callback) {
            ctx->completion_callback(ctx, result);
        }
    } else {
        /* Error occurred */
        ctx->last_error = result;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Read failed: %s", strerror(-result));

        /* Update connection context */
        uring_conn_context *conn_ctx = get_connection_context(state, ctx->fd);
        if (conn_ctx) {
            conn_ctx->ops_failed++;
            conn_ctx->error_count++;
            if (result != -EAGAIN && result != -EWOULDBLOCK) {
                update_connection_state(conn_ctx, URING_CONN_ERROR);
            }
        }

        /* Handle retryable errors */
        if ((result == -EAGAIN || result == -EWOULDBLOCK || result == -EINTR) &&
            ctx->retry_count < ctx->max_retries) {

            #ifndef REDIS_CLI_BUILD
            serverLog(LL_DEBUG, "Retrying read operation on fd %d: %s (attempt %d/%d)",
                      ctx->fd, strerror(-result), ctx->retry_count + 1, ctx->max_retries);
            #endif

            if (retry_operation(state, ctx) == 0) {
                return; /* Operation queued for retry */
            }
        }

        update_operation_state(ctx, URING_OP_STATE_FAILED);

        /* Call error callback if set */
        if (ctx->error_callback) {
            ctx->error_callback(ctx, result);
        }

        #ifndef REDIS_CLI_BUILD
        if (result != -EAGAIN && result != -EWOULDBLOCK) {
            serverLog(LL_DEBUG, "Read operation failed on fd %d: %s", ctx->fd, strerror(-result));
        }
        #endif
    }

    /* Return buffer to ring if applicable */
    if (ctx->buffer_id >= 0 && state->buffer_ring_enabled) {
        return_buffer_to_ring(state, ctx->buffer_id);
    } else if (ctx->buffer && ctx->buffer_id == -1) {
        /* Return regular buffer to pool */
        return_buffer_to_pool(ctx->buffer);
        state->stats.buffer_deallocations++;
    }

    /* Mark read operation as inactive */
    ctx->read_op.active = 0;

    free_op_context(ctx);
}

/* Enhanced handle write completion with error recovery */
static void handle_write_completion(aeEventLoop *eventLoop, uring_op_context *ctx, int result, int *numevents) {
    aeApiState *state = eventLoop->apidata;

    /* Update operation state and record completion time */
    ctx->completion_time = getMonotonicUs();
    ctx->bytes_transferred = (result > 0) ? result : 0;

    if (result >= 0) {
        /* Write successful or socket is writable */
        aeFileEvent *fe = &eventLoop->events[ctx->fd];
        if (fe->mask & AE_WRITABLE && fe->wfileProc) {
            eventLoop->fired[*numevents].fd = ctx->fd;
            eventLoop->fired[*numevents].mask = AE_WRITABLE;
            (*numevents)++;
        }

        /* Update connection context if available */
        uring_conn_context *conn_ctx = get_connection_context(state, ctx->fd);
        if (conn_ctx) {
            if (result > 0) {
                conn_ctx->bytes_written += result;
            }
            conn_ctx->ops_completed++;
            update_connection_activity(conn_ctx);
            update_connection_state(conn_ctx, URING_CONN_WRITING);
        }

        update_operation_state(ctx, URING_OP_STATE_COMPLETED);

        /* Call completion callback if set */
        if (ctx->completion_callback) {
            ctx->completion_callback(ctx, result);
        }
    } else {
        /* Error occurred */
        ctx->last_error = result;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Write failed: %s", strerror(-result));

        /* Update connection context */
        uring_conn_context *conn_ctx = get_connection_context(state, ctx->fd);
        if (conn_ctx) {
            conn_ctx->ops_failed++;
            conn_ctx->error_count++;
            if (result != -EAGAIN && result != -EWOULDBLOCK && result != -EPIPE) {
                update_connection_state(conn_ctx, URING_CONN_ERROR);
            }
        }

        /* Handle retryable errors */
        if ((result == -EAGAIN || result == -EWOULDBLOCK || result == -EINTR) &&
            ctx->retry_count < ctx->max_retries) {

            #ifndef REDIS_CLI_BUILD
            serverLog(LL_DEBUG, "Retrying write operation on fd %d: %s (attempt %d/%d)",
                      ctx->fd, strerror(-result), ctx->retry_count + 1, ctx->max_retries);
            #endif

            if (retry_operation(state, ctx) == 0) {
                return; /* Operation queued for retry */
            }
        }

        update_operation_state(ctx, URING_OP_STATE_FAILED);

        /* Call error callback if set */
        if (ctx->error_callback) {
            ctx->error_callback(ctx, result);
        }

        #ifndef REDIS_CLI_BUILD
        if (result != -EAGAIN && result != -EWOULDBLOCK) {
            serverLog(LL_DEBUG, "Write operation failed on fd %d: %s", ctx->fd, strerror(-result));
        }
        #endif
    }

    /* Mark write operation as inactive */
    ctx->write_op.active = 0;

    free_op_context(ctx);
}

/* Enhanced handle accept completion with connection setup */
static void handle_accept_completion(aeEventLoop *eventLoop, uring_op_context *ctx, int result, int *numevents) {
    aeApiState *state = eventLoop->apidata;

    /* Update operation state and record completion time */
    ctx->completion_time = getMonotonicUs();

    if (result >= 0) {
        /* New connection accepted successfully */
        int client_fd = result;

        #ifndef REDIS_CLI_BUILD
        serverLog(LL_DEBUG, "Accepted new connection: client_fd=%d, listening_fd=%d",
                  client_fd, ctx->fd);
        #endif

        /* Create connection context for the new client */
        uring_conn_context *client_conn_ctx = create_connection_context(state, client_fd, NULL);
        if (client_conn_ctx) {
            /* Copy client address information */
            memcpy(&client_conn_ctx->remote_addr, &ctx->client_addr, ctx->client_addr_len);
            client_conn_ctx->remote_addr_len = ctx->client_addr_len;

            /* Get local address for the client socket */
            client_conn_ctx->local_addr_len = sizeof(client_conn_ctx->local_addr);
            if (getsockname(client_fd, (struct sockaddr*)&client_conn_ctx->local_addr,
                           &client_conn_ctx->local_addr_len) != 0) {
                client_conn_ctx->local_addr_len = 0;
            }

            update_connection_state(client_conn_ctx, URING_CONN_CONNECTED);

            #ifndef REDIS_CLI_BUILD
            char client_ip[INET6_ADDRSTRLEN];
            int client_port = 0;
            if (client_conn_ctx->remote_addr.ss_family == AF_INET) {
                struct sockaddr_in *addr_in = (struct sockaddr_in*)&client_conn_ctx->remote_addr;
                inet_ntop(AF_INET, &addr_in->sin_addr, client_ip, sizeof(client_ip));
                client_port = ntohs(addr_in->sin_port);
            } else if (client_conn_ctx->remote_addr.ss_family == AF_INET6) {
                struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6*)&client_conn_ctx->remote_addr;
                inet_ntop(AF_INET6, &addr_in6->sin6_addr, client_ip, sizeof(client_ip));
                client_port = ntohs(addr_in6->sin6_port);
            }
            serverLog(LL_DEBUG, "New client connection from %s:%d", client_ip, client_port);
            #endif
        }

        /* Update listening socket connection context */
        uring_conn_context *listen_conn_ctx = get_connection_context(state, ctx->fd);
        if (listen_conn_ctx) {
            listen_conn_ctx->ops_completed++;
            update_connection_activity(listen_conn_ctx);
        }

        /* Notify the event loop about the new connection */
        aeFileEvent *fe = &eventLoop->events[ctx->fd];
        if (fe->mask & AE_READABLE && fe->rfileProc) {
            eventLoop->fired[*numevents].fd = ctx->fd;
            eventLoop->fired[*numevents].mask = AE_READABLE;
            (*numevents)++;
        }

        update_operation_state(ctx, URING_OP_STATE_COMPLETED);

        /* Call completion callback if set */
        if (ctx->completion_callback) {
            ctx->completion_callback(ctx, result);
        }

        /* For multishot accept, the operation continues automatically
         * For regular accept, re-submit for the next connection */
        #ifndef IORING_ACCEPT_MULTISHOT
        ctx->accept_op.active = 0;  /* Mark as inactive before re-submitting */
        if (submit_accept_operation(state, ctx->fd, ctx) == 0) {
            ctx->accept_op.active = 1;
        }
        #endif
    } else {
        /* Accept failed */
        ctx->last_error = result;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Accept failed: %s", strerror(-result));

        /* Update listening socket connection context */
        uring_conn_context *listen_conn_ctx = get_connection_context(state, ctx->fd);
        if (listen_conn_ctx) {
            listen_conn_ctx->ops_failed++;
            listen_conn_ctx->error_count++;
        }

        /* Handle retryable errors */
        if ((result == -EAGAIN || result == -EWOULDBLOCK || result == -EINTR) &&
            ctx->retry_count < ctx->max_retries) {

            #ifndef REDIS_CLI_BUILD
            serverLog(LL_DEBUG, "Retrying accept operation on fd %d: %s (attempt %d/%d)",
                      ctx->fd, strerror(-result), ctx->retry_count + 1, ctx->max_retries);
            #endif

            if (retry_operation(state, ctx) == 0) {
                return; /* Operation queued for retry */
            }
        }

        update_operation_state(ctx, URING_OP_STATE_FAILED);

        /* Call error callback if set */
        if (ctx->error_callback) {
            ctx->error_callback(ctx, result);
        }

        /* Mark operation as inactive */
        ctx->accept_op.active = 0;

        /* For serious errors, log them */
        if (result != -EAGAIN && result != -EWOULDBLOCK) {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "Accept operation failed on fd %d: %s", ctx->fd, strerror(-result));
            #endif
        }

        /* Try to re-submit accept operation after a brief delay for non-fatal errors */
        if (result == -EMFILE || result == -ENFILE || result == -ENOBUFS || result == -ENOMEM) {
            /* Resource exhaustion - retry with lower priority */
            ctx->priority = URING_OP_PRIORITY_LOW;
            add_operation_to_queue(state, ctx);
        }
    }
}





/* Get statistics for INFO command */
static void get_uring_stats(aeApiState *state, char **info) {
    #ifndef REDIS_CLI_BUILD
    /* Calculate derived metrics */
    double avg_completion_time = state->stats.ops_completed > 0 ?
        (double)state->stats.total_completion_time_us / state->stats.ops_completed : 0.0;

    double success_rate = state->stats.ops_submitted > 0 ?
        (double)state->stats.ops_completed / state->stats.ops_submitted * 100.0 : 0.0;

    double buffer_hit_rate = (state->stats.buffer_ring_hits + state->stats.buffer_ring_misses) > 0 ?
        (double)state->stats.buffer_ring_hits / (state->stats.buffer_ring_hits + state->stats.buffer_ring_misses) * 100.0 : 0.0;

    uint64_t total_ops = state->stats.read_ops + state->stats.write_ops + state->stats.accept_ops +
                         state->stats.recv_ops + state->stats.send_ops;

    *info = sdscatprintf(*info,
        "# io_uring\r\n"
        "uring_enabled:yes\r\n"
        "uring_backend:%s\r\n"
        "uring_sqpoll_enabled:%s\r\n"
        "uring_sqpoll_cpu:%d\r\n"
        "uring_sq_entries:%d\r\n"
        "uring_cq_entries:%d\r\n"
        "uring_buffer_ring_enabled:%s\r\n"
        "uring_capabilities:0x%x\r\n"
        "uring_ops_submitted:%lu\r\n"
        "uring_ops_completed:%lu\r\n"
        "uring_ops_failed:%lu\r\n"
        "uring_success_rate:%.2f\r\n"
        "uring_sq_full_count:%lu\r\n"
        "uring_cq_overflow_count:%lu\r\n"
        "uring_avg_completion_time_us:%.2f\r\n"
        "uring_max_completion_time_us:%lu\r\n"
        "uring_buffer_ring_hits:%lu\r\n"
        "uring_buffer_ring_misses:%lu\r\n"
        "uring_buffer_hit_rate:%.2f\r\n"
        "uring_read_ops:%lu\r\n"
        "uring_write_ops:%lu\r\n"
        "uring_accept_ops:%lu\r\n"
        "uring_recv_ops:%lu\r\n"
        "uring_send_ops:%lu\r\n"
        "uring_total_ops:%lu\r\n"
        "uring_eagain_errors:%lu\r\n"
        "uring_econnreset_errors:%lu\r\n"
        "uring_epipe_errors:%lu\r\n"
        "uring_other_errors:%lu\r\n"
        "uring_batch_submissions:%lu\r\n"
        "uring_single_submissions:%lu\r\n"
        "uring_completion_batches:%lu\r\n"
        "uring_completion_processing_calls:%lu\r\n"
        "uring_buffer_allocations:%lu\r\n"
        "uring_buffer_deallocations:%lu\r\n"
        "uring_context_allocations:%lu\r\n"
        "uring_context_deallocations:%lu\r\n",
        aeApiName(),
        state->sqpoll_enabled ? "yes" : "no",
        state->sqpoll_cpu,
        state->sq_entries,
        state->cq_entries,
        state->buffer_ring_enabled ? "yes" : "no",
        state->capabilities,
        (unsigned long)state->stats.ops_submitted,
        (unsigned long)state->stats.ops_completed,
        (unsigned long)state->stats.ops_failed,
        success_rate,
        (unsigned long)state->stats.sq_full_count,
        (unsigned long)state->stats.cq_overflow_count,
        avg_completion_time,
        (unsigned long)state->stats.max_completion_time_us,
        (unsigned long)state->stats.buffer_ring_hits,
        (unsigned long)state->stats.buffer_ring_misses,
        buffer_hit_rate,
        (unsigned long)state->stats.read_ops,
        (unsigned long)state->stats.write_ops,
        (unsigned long)state->stats.accept_ops,
        (unsigned long)state->stats.recv_ops,
        (unsigned long)state->stats.send_ops,
        (unsigned long)total_ops,
        (unsigned long)state->stats.eagain_errors,
        (unsigned long)state->stats.econnreset_errors,
        (unsigned long)state->stats.epipe_errors,
        (unsigned long)state->stats.other_errors,
        (unsigned long)state->stats.batch_submissions,
        (unsigned long)state->stats.single_submissions,
        (unsigned long)state->stats.completion_batches,
        (unsigned long)state->stats.completion_processing_calls,
        (unsigned long)state->stats.buffer_allocations,
        (unsigned long)state->stats.buffer_deallocations,
        (unsigned long)state->stats.context_allocations,
        (unsigned long)state->stats.context_deallocations);

    /* Add buffer pool statistics */
    get_buffer_pool_stats(info);

    /* Add connection statistics */
    get_connection_stats(state, info);

    /* Add operation statistics */
    get_operation_stats(state, info);
    #endif
}

/* Utility function to convert operation type to string */
static const char *uring_op_type_to_string(int op_type) {
    switch (op_type) {
        case URING_OP_READ: return "read";
        case URING_OP_WRITE: return "write";
        case URING_OP_ACCEPT: return "accept";
        case URING_OP_RECV: return "recv";
        case URING_OP_SEND: return "send";

        default: return "unknown";
    }
}

/* Update operation statistics */
static void update_operation_stats(aeApiState *state, uring_op_context *ctx, int result) {
    monotime now = getMonotonicUs();
    uint64_t operation_time = now - ctx->submit_time;

    state->stats.total_completion_time_us += operation_time;
    if (operation_time > state->stats.max_completion_time_us) {
        state->stats.max_completion_time_us = operation_time;
    }

    if (result < 0) {
        state->stats.ops_failed++;
    } else {
        state->stats.ops_completed++;
    }
}



/* Public interface for getting io_uring stats */
void aeGetUringStats(aeEventLoop *eventLoop, char **info) {
    if (eventLoop && eventLoop->apidata) {
        get_uring_stats((aeApiState*)eventLoop->apidata, info);
    }
}

/* ============================================================================
 * Team Member B - Enhanced Connection Management for io_uring
 * ============================================================================ */

/* Create connection context for io_uring operations */
static uring_conn_context *create_conn_context(connection *conn) {
    uring_conn_context *ctx = zmalloc(sizeof(uring_conn_context));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(uring_conn_context));
    ctx->conn = conn;
    ctx->pending_ops = 0;

    /* Initialize operation queues */
    ctx->pending_reads = listCreate();
    ctx->pending_writes = listCreate();

    if (!ctx->pending_reads || !ctx->pending_writes) {
        if (ctx->pending_reads) listRelease(ctx->pending_reads);
        if (ctx->pending_writes) listRelease(ctx->pending_writes);
        zfree(ctx);
        return NULL;
    }

    return ctx;
}

/* Free connection context */
static void free_conn_context(uring_conn_context *ctx) {
    if (!ctx) return;

    /* Clean up pending operations */
    if (ctx->pending_reads) {
        listRelease(ctx->pending_reads);
    }
    if (ctx->pending_writes) {
        listRelease(ctx->pending_writes);
    }

    /* Clean up active operations */
    if (ctx->read_op.ctx) {
        free_op_context(ctx->read_op.ctx);
    }
    if (ctx->write_op.ctx) {
        free_op_context(ctx->write_op.ctx);
    }

    zfree(ctx);
}

/* Enhanced connection setup for io_uring */
static int setup_uring_connection(connection *conn) {
    if (!conn) return -1;

    /* Create io_uring specific context */
    uring_conn_context *uring_ctx = create_conn_context(conn);
    if (!uring_ctx) return -1;

    /* Store context in connection private data */
    /* Note: This would require extending the connection structure */
    /* For now, we'll use a simple approach */

    return 0;
}

/* Connection optimization for io_uring */
static void optimize_connection_for_uring(connection *conn) {
    if (!conn || conn->fd < 0) return;

    /* Set socket options for optimal io_uring performance */
    int val = 1;

    /* Enable TCP_NODELAY for low latency */
    setsockopt(conn->fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));

    /* Set socket buffer sizes for optimal throughput */
    val = 65536;  /* 64KB */
    setsockopt(conn->fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val));
    setsockopt(conn->fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val));

    /* Enable SO_REUSEADDR */
    val = 1;
    setsockopt(conn->fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
}

/* Enhanced buffer management for connections */
static void *get_connection_buffer(uring_conn_context *ctx, size_t size) {
    if (!ctx) return zmalloc(size);

    /* Use buffer pool for optimal performance */
    void *buffer = get_buffer_from_pool();
    if (!buffer) {
        buffer = zmalloc(size);
    }

    return buffer;
}

/* Return connection buffer */
static void return_connection_buffer(uring_conn_context *ctx, void *buffer) {
    if (!ctx) {
        zfree(buffer);
        return;
    }

    /* Return to buffer pool */
    return_buffer_to_pool(buffer);
}

/* Batch operation submission for connections */
static int submit_batched_operations(aeApiState *state, uring_conn_context *ctx) {
    if (!state || !ctx) return -1;

    int submitted = 0;

    /* Submit pending read operations */
    listIter li;
    listNode *ln;
    listRewind(ctx->pending_reads, &li);

    while ((ln = listNext(&li)) != NULL) {
        uring_op_context *op_ctx = listNodeValue(ln);
        if (submit_read_operation(state, op_ctx->fd, op_ctx) == 0) {
            submitted++;
            listDelNode(ctx->pending_reads, ln);
        }
    }

    /* Submit pending write operations */
    listRewind(ctx->pending_writes, &li);

    while ((ln = listNext(&li)) != NULL) {
        uring_op_context *op_ctx = listNodeValue(ln);
        if (submit_write_operation(state, op_ctx->fd, op_ctx) == 0) {
            submitted++;
            listDelNode(ctx->pending_writes, ln);
        }
    }

    return submitted;
}

/* Create connection context for lifecycle management */
uring_conn_context *create_connection_context(aeApiState *state, int fd, connection *conn) {
    if (!state || fd < 0 || fd >= state->max_connections) {
        return NULL;
    }

    pthread_mutex_lock(&state->conn_lock);

    /* Check if connection already exists */
    if (state->connections[fd]) {
        pthread_mutex_unlock(&state->conn_lock);
        return state->connections[fd];
    }

    uring_conn_context *conn_ctx = zmalloc(sizeof(uring_conn_context));
    if (!conn_ctx) {
        pthread_mutex_unlock(&state->conn_lock);
        return NULL;
    }

    memset(conn_ctx, 0, sizeof(uring_conn_context));

    /* Initialize connection context */
    conn_ctx->conn = conn;
    conn_ctx->fd = fd;
    conn_ctx->state = URING_CONN_INIT;
    conn_ctx->created_time = getMonotonicUs();
    conn_ctx->last_activity = conn_ctx->created_time;
    conn_ctx->state_change_time = conn_ctx->created_time;

    /* Initialize operation queues */
    conn_ctx->pending_reads = listCreate();
    conn_ctx->pending_writes = listCreate();

    /* Get address information if available */
    if (conn) {
        conn_ctx->remote_addr_len = sizeof(conn_ctx->remote_addr);
        if (getpeername(fd, (struct sockaddr*)&conn_ctx->remote_addr, &conn_ctx->remote_addr_len) != 0) {
            conn_ctx->remote_addr_len = 0;
        }

        conn_ctx->local_addr_len = sizeof(conn_ctx->local_addr);
        if (getsockname(fd, (struct sockaddr*)&conn_ctx->local_addr, &conn_ctx->local_addr_len) != 0) {
            conn_ctx->local_addr_len = 0;
        }
    }

    /* Add to connection array */
    state->connections[fd] = conn_ctx;

    /* Add to linked list */
    if (state->conn_list_tail) {
        state->conn_list_tail->next = conn_ctx;
        conn_ctx->prev = state->conn_list_tail;
        state->conn_list_tail = conn_ctx;
    } else {
        state->conn_list_head = state->conn_list_tail = conn_ctx;
    }

    state->active_connections++;
    state->stats.context_allocations++;

    pthread_mutex_unlock(&state->conn_lock);

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Created connection context for fd %d, total connections: %d",
              fd, state->active_connections);
    #endif

    return conn_ctx;
}

/* Destroy connection context */
void destroy_connection_context(aeApiState *state, uring_conn_context *conn_ctx) {
    if (!state || !conn_ctx) {
        return;
    }

    pthread_mutex_lock(&state->conn_lock);

    int fd = conn_ctx->fd;

    /* Remove from array */
    if (fd >= 0 && fd < state->max_connections && state->connections[fd] == conn_ctx) {
        state->connections[fd] = NULL;
    }

    /* Remove from linked list */
    if (conn_ctx->prev) {
        conn_ctx->prev->next = conn_ctx->next;
    } else {
        state->conn_list_head = conn_ctx->next;
    }

    if (conn_ctx->next) {
        conn_ctx->next->prev = conn_ctx->prev;
    } else {
        state->conn_list_tail = conn_ctx->prev;
    }

    /* Clean up connection-specific buffers */
    if (conn_ctx->read_buffer) {
        return_buffer_to_pool(conn_ctx->read_buffer);
    }
    if (conn_ctx->write_buffer) {
        return_buffer_to_pool(conn_ctx->write_buffer);
    }

    /* Clean up operation queues */
    if (conn_ctx->pending_reads) {
        listRelease(conn_ctx->pending_reads);
    }
    if (conn_ctx->pending_writes) {
        listRelease(conn_ctx->pending_writes);
    }

    state->active_connections--;
    state->stats.context_deallocations++;

    pthread_mutex_unlock(&state->conn_lock);

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Destroyed connection context for fd %d, total connections: %d",
              fd, state->active_connections);
    #endif

    zfree(conn_ctx);
}

/* Get connection context by fd */
uring_conn_context *get_connection_context(aeApiState *state, int fd) {
    if (!state || fd < 0 || fd >= state->max_connections) {
        return NULL;
    }

    return state->connections[fd];
}

/* Update connection state */
void update_connection_state(uring_conn_context *conn_ctx, uring_connection_state new_state) {
    if (!conn_ctx || conn_ctx->state == new_state) {
        return;
    }

    uring_connection_state old_state = conn_ctx->state;
    conn_ctx->state = new_state;
    conn_ctx->state_change_time = getMonotonicUs();

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Connection fd %d state changed: %d -> %d",
              conn_ctx->fd, old_state, new_state);
    #endif
}

/* Update connection activity timestamp */
void update_connection_activity(uring_conn_context *conn_ctx) {
    if (!conn_ctx) {
        return;
    }

    conn_ctx->last_activity = getMonotonicUs();
}

/* Cleanup connection operations */
void cleanup_connection_operations(aeApiState *state, uring_conn_context *conn_ctx) {
    if (!state || !conn_ctx) {
        return;
    }

    /* Cancel pending read operations */
    if (conn_ctx->read_op.active && conn_ctx->read_op.ctx) {
        /* Mark operation for cancellation */
        conn_ctx->read_op.active = 0;
        free_op_context(conn_ctx->read_op.ctx);
        conn_ctx->read_op.ctx = NULL;
    }

    /* Cancel pending write operations */
    if (conn_ctx->write_op.active && conn_ctx->write_op.ctx) {
        /* Mark operation for cancellation */
        conn_ctx->write_op.active = 0;
        free_op_context(conn_ctx->write_op.ctx);
        conn_ctx->write_op.ctx = NULL;
    }

    /* Cancel pending accept operations */
    if (conn_ctx->accept_op.active && conn_ctx->accept_op.ctx) {
        /* Mark operation for cancellation */
        conn_ctx->accept_op.active = 0;
        free_op_context(conn_ctx->accept_op.ctx);
        conn_ctx->accept_op.ctx = NULL;
    }

    /* Clear pending operation queues */
    if (conn_ctx->pending_reads) {
        listIter li;
        listNode *ln;
        listRewind(conn_ctx->pending_reads, &li);
        while ((ln = listNext(&li)) != NULL) {
            uring_op_context *op_ctx = listNodeValue(ln);
            free_op_context(op_ctx);
        }
        listEmpty(conn_ctx->pending_reads);
    }

    if (conn_ctx->pending_writes) {
        listIter li;
        listNode *ln;
        listRewind(conn_ctx->pending_writes, &li);
        while ((ln = listNext(&li)) != NULL) {
            uring_op_context *op_ctx = listNodeValue(ln);
            free_op_context(op_ctx);
        }
        listEmpty(conn_ctx->pending_writes);
    }

    /* Update connection state */
    update_connection_state(conn_ctx, URING_CONN_CLOSED);
}

/* Get connection statistics */
void get_connection_stats(aeApiState *state, char **info) {
    #ifndef REDIS_CLI_BUILD
    if (!state) {
        *info = sdscatprintf(*info, "connection_tracking_enabled:no\r\n");
        return;
    }

    pthread_mutex_lock(&state->conn_lock);

    int state_counts[8] = {0}; /* Count connections by state */
    uint64_t total_bytes_read = 0;
    uint64_t total_bytes_written = 0;
    uint64_t total_ops_completed = 0;
    uint64_t total_ops_failed = 0;
    uint64_t oldest_connection_age = 0;
    uint64_t now = getMonotonicUs();

    /* Iterate through all connections */
    uring_conn_context *conn_ctx = state->conn_list_head;
    while (conn_ctx) {
        if (conn_ctx->state < 8) {
            state_counts[conn_ctx->state]++;
        }

        total_bytes_read += conn_ctx->bytes_read;
        total_bytes_written += conn_ctx->bytes_written;
        total_ops_completed += conn_ctx->ops_completed;
        total_ops_failed += conn_ctx->ops_failed;

        uint64_t age = now - conn_ctx->created_time;
        if (age > oldest_connection_age) {
            oldest_connection_age = age;
        }

        conn_ctx = conn_ctx->next;
    }

    *info = sdscatprintf(*info,
        "connection_tracking_enabled:yes\r\n"
        "active_connections:%d\r\n"
        "max_connections:%d\r\n"
        "connections_init:%d\r\n"
        "connections_connecting:%d\r\n"
        "connections_connected:%d\r\n"
        "connections_reading:%d\r\n"
        "connections_writing:%d\r\n"
        "connections_closing:%d\r\n"
        "connections_closed:%d\r\n"
        "connections_error:%d\r\n"
        "total_bytes_read:%lu\r\n"
        "total_bytes_written:%lu\r\n"
        "total_ops_completed:%lu\r\n"
        "total_ops_failed:%lu\r\n"
        "oldest_connection_age_us:%lu\r\n",
        state->active_connections,
        state->max_connections,
        state_counts[URING_CONN_INIT],
        state_counts[URING_CONN_CONNECTING],
        state_counts[URING_CONN_CONNECTED],
        state_counts[URING_CONN_READING],
        state_counts[URING_CONN_WRITING],
        state_counts[URING_CONN_CLOSING],
        state_counts[URING_CONN_CLOSED],
        state_counts[URING_CONN_ERROR],
        (unsigned long)total_bytes_read,
        (unsigned long)total_bytes_written,
        (unsigned long)total_ops_completed,
        (unsigned long)total_ops_failed,
        (unsigned long)oldest_connection_age);

    pthread_mutex_unlock(&state->conn_lock);
    #endif
}

/* Enhanced operation management functions */

/* Create enhanced operation context */
uring_op_context *create_operation_context(aeApiState *state, int fd, int op_type, uring_op_priority priority) {
    if (!state || fd < 0) {
        return NULL;
    }

    uring_op_context *op_ctx = zmalloc(sizeof(uring_op_context));
    if (!op_ctx) {
        return NULL;
    }

    memset(op_ctx, 0, sizeof(uring_op_context));

    /* Initialize basic fields */
    op_ctx->fd = fd;
    op_ctx->op_type = op_type;
    op_ctx->state = URING_OP_STATE_INIT;
    op_ctx->priority = priority;
    op_ctx->submit_time = getMonotonicUs();
    op_ctx->max_retries = 3; /* Default retry count */
    op_ctx->ref_count = 1;

    /* Initialize reference counting mutex */
    if (pthread_mutex_init(&op_ctx->ref_lock, NULL) != 0) {
        zfree(op_ctx);
        return NULL;
    }

    pthread_mutex_lock(&state->op_lock);

    /* Assign unique operation ID and sequence number */
    op_ctx->op_id = state->next_op_id++;
    op_ctx->sequence_num = state->next_sequence_num++;

    /* Add to active operations list */
    if (state->op_list_tail) {
        state->op_list_tail->next = op_ctx;
        op_ctx->prev = state->op_list_tail;
        state->op_list_tail = op_ctx;
    } else {
        state->op_list_head = state->op_list_tail = op_ctx;
    }

    state->active_operations++;

    pthread_mutex_unlock(&state->op_lock);

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Created operation context: id=%lu, fd=%d, type=%d, priority=%d",
              op_ctx->op_id, fd, op_type, priority);
    #endif

    return op_ctx;
}

/* Destroy operation context */
void destroy_operation_context(aeApiState *state, uring_op_context *op_ctx) {
    if (!state || !op_ctx) {
        return;
    }

    pthread_mutex_lock(&state->op_lock);

    /* Remove from active operations list */
    if (op_ctx->prev) {
        op_ctx->prev->next = op_ctx->next;
    } else {
        state->op_list_head = op_ctx->next;
    }

    if (op_ctx->next) {
        op_ctx->next->prev = op_ctx->prev;
    } else {
        state->op_list_tail = op_ctx->prev;
    }

    /* Remove from priority queue if present */
    if (op_ctx->priority < 4) {
        struct {
            uring_op_context *head;
            uring_op_context *tail;
            int count;
        } *queue = &state->priority_queues[op_ctx->priority];

        /* Simple removal - in a real implementation, we'd need better queue management */
        if (queue->head == op_ctx) {
            queue->head = op_ctx->next;
        }
        if (queue->tail == op_ctx) {
            queue->tail = op_ctx->prev;
        }
        if (queue->count > 0) {
            queue->count--;
        }
    }

    state->active_operations--;

    pthread_mutex_unlock(&state->op_lock);

    /* Clean up operation-specific resources */
    if (op_ctx->buffer && op_ctx->buffer_id == -1) {
        return_buffer_to_pool(op_ctx->buffer);
    }

    /* Destroy reference counting mutex */
    pthread_mutex_destroy(&op_ctx->ref_lock);

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Destroyed operation context: id=%lu, fd=%d",
              op_ctx->op_id, op_ctx->fd);
    #endif

    zfree(op_ctx);
}

/* Add operation to priority queue */
void add_operation_to_queue(aeApiState *state, uring_op_context *op_ctx) {
    if (!state || !op_ctx || op_ctx->priority >= 4) {
        return;
    }

    pthread_mutex_lock(&state->op_lock);

    struct {
        uring_op_context *head;
        uring_op_context *tail;
        int count;
    } *queue = &state->priority_queues[op_ctx->priority];

    /* Add to tail of priority queue */
    if (queue->tail) {
        queue->tail->next = op_ctx;
        op_ctx->prev = queue->tail;
        queue->tail = op_ctx;
    } else {
        queue->head = queue->tail = op_ctx;
    }

    queue->count++;
    update_operation_state(op_ctx, URING_OP_STATE_QUEUED);

    pthread_mutex_unlock(&state->op_lock);
}

/* Get next operation from priority queues */
uring_op_context *get_next_operation(aeApiState *state, uring_op_priority min_priority) {
    if (!state) {
        return NULL;
    }

    pthread_mutex_lock(&state->op_lock);

    uring_op_context *op_ctx = NULL;

    /* Check priority queues from highest to lowest priority */
    for (int i = URING_OP_PRIORITY_CRITICAL; i >= min_priority; i--) {
        struct {
            uring_op_context *head;
            uring_op_context *tail;
            int count;
        } *queue = &state->priority_queues[i];

        if (queue->head) {
            op_ctx = queue->head;
            queue->head = op_ctx->next;
            if (queue->head) {
                queue->head->prev = NULL;
            } else {
                queue->tail = NULL;
            }
            queue->count--;

            op_ctx->next = NULL;
            op_ctx->prev = NULL;
            break;
        }
    }

    pthread_mutex_unlock(&state->op_lock);

    return op_ctx;
}

/* Update operation state */
void update_operation_state(uring_op_context *op_ctx, uring_op_state new_state) {
    if (!op_ctx || op_ctx->state == new_state) {
        return;
    }

    uring_op_state old_state = op_ctx->state;
    op_ctx->state = new_state;

    if (new_state == URING_OP_STATE_COMPLETED || new_state == URING_OP_STATE_FAILED) {
        op_ctx->completion_time = getMonotonicUs();
    }

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Operation id=%lu state changed: %d -> %d",
              op_ctx->op_id, old_state, new_state);
    #endif
}

/* Cancel operation */
void cancel_operation(aeApiState *state, uring_op_context *op_ctx) {
    if (!state || !op_ctx) {
        return;
    }

    update_operation_state(op_ctx, URING_OP_STATE_CANCELLED);

    /* Call error callback if set */
    if (op_ctx->error_callback) {
        op_ctx->error_callback(op_ctx, -ECANCELED);
    }

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Cancelled operation id=%lu", op_ctx->op_id);
    #endif
}

/* Timeout operations */
void timeout_operations(aeApiState *state) {
    if (!state) {
        return;
    }

    uint64_t now = getMonotonicUs();

    pthread_mutex_lock(&state->op_lock);

    uring_op_context *op_ctx = state->op_list_head;
    while (op_ctx) {
        uring_op_context *next = op_ctx->next;

        if (op_ctx->timeout_us > 0 &&
            (now - op_ctx->submit_time) > op_ctx->timeout_us &&
            (op_ctx->state == URING_OP_STATE_SUBMITTED || op_ctx->state == URING_OP_STATE_PROCESSING)) {

            update_operation_state(op_ctx, URING_OP_STATE_TIMEOUT);

            /* Call error callback if set */
            if (op_ctx->error_callback) {
                op_ctx->error_callback(op_ctx, -ETIMEDOUT);
            }

            #ifndef REDIS_CLI_BUILD
            serverLog(LL_DEBUG, "Operation id=%lu timed out after %lu us",
                      op_ctx->op_id, now - op_ctx->submit_time);
            #endif
        }

        op_ctx = next;
    }

    pthread_mutex_unlock(&state->op_lock);
}

/* Retry operation */
int retry_operation(aeApiState *state, uring_op_context *op_ctx) {
    if (!state || !op_ctx) {
        return -1;
    }

    if (op_ctx->retry_count >= op_ctx->max_retries) {
        update_operation_state(op_ctx, URING_OP_STATE_FAILED);
        return -1;
    }

    op_ctx->retry_count++;
    op_ctx->submit_time = getMonotonicUs();
    update_operation_state(op_ctx, URING_OP_STATE_INIT);

    /* Add back to appropriate priority queue */
    add_operation_to_queue(state, op_ctx);

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Retrying operation id=%lu (attempt %d/%d)",
              op_ctx->op_id, op_ctx->retry_count, op_ctx->max_retries);
    #endif

    return 0;
}

/* Cleanup completed operations */
void cleanup_completed_operations(aeApiState *state) {
    if (!state) {
        return;
    }

    pthread_mutex_lock(&state->op_lock);

    uring_op_context *op_ctx = state->op_list_head;
    while (op_ctx) {
        uring_op_context *next = op_ctx->next;

        if (op_ctx->state == URING_OP_STATE_COMPLETED ||
            op_ctx->state == URING_OP_STATE_FAILED ||
            op_ctx->state == URING_OP_STATE_CANCELLED ||
            op_ctx->state == URING_OP_STATE_TIMEOUT) {

            /* Remove from list and destroy if ref count is 1 */
            if (op_ctx->ref_count <= 1) {
                pthread_mutex_unlock(&state->op_lock);
                destroy_operation_context(state, op_ctx);
                pthread_mutex_lock(&state->op_lock);
            }
        }

        op_ctx = next;
    }

    pthread_mutex_unlock(&state->op_lock);
}

/* Operation reference counting */
void op_context_ref(uring_op_context *op_ctx) {
    if (!op_ctx) {
        return;
    }

    pthread_mutex_lock(&op_ctx->ref_lock);
    op_ctx->ref_count++;
    pthread_mutex_unlock(&op_ctx->ref_lock);
}

void op_context_unref(uring_op_context *op_ctx) {
    if (!op_ctx) {
        return;
    }

    pthread_mutex_lock(&op_ctx->ref_lock);
    op_ctx->ref_count--;
    int should_destroy = (op_ctx->ref_count <= 0);
    pthread_mutex_unlock(&op_ctx->ref_lock);

    if (should_destroy) {
        /* Note: This is simplified - in practice we'd need the state pointer */
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_DEBUG, "Operation id=%lu ref count reached 0", op_ctx->op_id);
        #endif
    }
}

/* Set operation timeout */
void set_operation_timeout(uring_op_context *op_ctx, uint64_t timeout_us) {
    if (!op_ctx) {
        return;
    }

    op_ctx->timeout_us = timeout_us;
}

/* Set operation callbacks */
void set_operation_callback(uring_op_context *op_ctx,
                           void (*completion_cb)(uring_op_context *, int),
                           void (*error_cb)(uring_op_context *, int)) {
    if (!op_ctx) {
        return;
    }

    op_ctx->completion_callback = completion_cb;
    op_ctx->error_callback = error_cb;
}

/* Get operation statistics */
void get_operation_stats(aeApiState *state, char **info) {
    #ifndef REDIS_CLI_BUILD
    if (!state) {
        *info = sdscatprintf(*info, "operation_tracking_enabled:no\r\n");
        return;
    }

    pthread_mutex_lock(&state->op_lock);

    int state_counts[8] = {0}; /* Count operations by state */
    int priority_counts[4] = {0}; /* Count operations by priority */
    uint64_t total_operations = 0;
    uint64_t avg_completion_time = 0;
    uint64_t total_completion_time = 0;
    uint64_t completed_ops = 0;

    /* Iterate through all operations */
    uring_op_context *op_ctx = state->op_list_head;
    while (op_ctx) {
        total_operations++;

        if (op_ctx->state < 8) {
            state_counts[op_ctx->state]++;
        }

        if (op_ctx->priority < 4) {
            priority_counts[op_ctx->priority]++;
        }

        if (op_ctx->state == URING_OP_STATE_COMPLETED && op_ctx->completion_time > 0) {
            total_completion_time += (op_ctx->completion_time - op_ctx->submit_time);
            completed_ops++;
        }

        op_ctx = op_ctx->next;
    }

    if (completed_ops > 0) {
        avg_completion_time = total_completion_time / completed_ops;
    }

    *info = sdscatprintf(*info,
        "operation_tracking_enabled:yes\r\n"
        "active_operations:%d\r\n"
        "max_operations:%d\r\n"
        "next_op_id:%lu\r\n"
        "operations_init:%d\r\n"
        "operations_queued:%d\r\n"
        "operations_submitted:%d\r\n"
        "operations_processing:%d\r\n"
        "operations_completed:%d\r\n"
        "operations_failed:%d\r\n"
        "operations_cancelled:%d\r\n"
        "operations_timeout:%d\r\n"
        "priority_low:%d\r\n"
        "priority_normal:%d\r\n"
        "priority_high:%d\r\n"
        "priority_critical:%d\r\n"
        "avg_completion_time_us:%lu\r\n",
        state->active_operations,
        state->max_operations,
        state->next_op_id,
        state_counts[URING_OP_STATE_INIT],
        state_counts[URING_OP_STATE_QUEUED],
        state_counts[URING_OP_STATE_SUBMITTED],
        state_counts[URING_OP_STATE_PROCESSING],
        state_counts[URING_OP_STATE_COMPLETED],
        state_counts[URING_OP_STATE_FAILED],
        state_counts[URING_OP_STATE_CANCELLED],
        state_counts[URING_OP_STATE_TIMEOUT],
        priority_counts[URING_OP_PRIORITY_LOW],
        priority_counts[URING_OP_PRIORITY_NORMAL],
        priority_counts[URING_OP_PRIORITY_HIGH],
        priority_counts[URING_OP_PRIORITY_CRITICAL],
        avg_completion_time);

    pthread_mutex_unlock(&state->op_lock);
    #endif
}

/* Enhanced error recovery and result processing functions */

/* Analyze operation result and determine recovery strategy */
static int analyze_operation_result(uring_op_context *ctx, int result) {
    if (result >= 0) {
        return 0; /* Success */
    }

    /* Categorize error severity */
    switch (result) {
        case -EAGAIN:
        #if EAGAIN != EWOULDBLOCK
        case -EWOULDBLOCK:
        #endif
        case -EINTR:
            return 1; /* Retryable error */

        case -ETIMEDOUT:
            return 2; /* Timeout - may be retryable */

        case -ECONNRESET:
        case -EPIPE:
        case -ECONNABORTED:
            return 3; /* Connection error - not retryable */

        case -EBADF:
        case -EINVAL:
            return 4; /* Invalid operation - not retryable */

        case -ENOMEM:
        case -ENOBUFS:
        case -EMFILE:
        case -ENFILE:
            return 5; /* Resource exhaustion - may be retryable later */

        default:
            return 6; /* Unknown error */
    }
}

/* Process operation result with comprehensive error handling */
static void process_operation_result(aeApiState *state, uring_op_context *ctx, int result) {
    int error_category = analyze_operation_result(ctx, result);

    switch (error_category) {
        case 0: /* Success */
            ctx->retry_count = 0; /* Reset retry count on success */
            break;

        case 1: /* Retryable error */
            if (ctx->retry_count < ctx->max_retries) {
                #ifndef REDIS_CLI_BUILD
                serverLog(LL_DEBUG, "Scheduling retry for operation id=%lu (attempt %d/%d): %s",
                          ctx->op_id, ctx->retry_count + 1, ctx->max_retries, strerror(-result));
                #endif
                retry_operation(state, ctx);
                return;
            }
            break;

        case 2: /* Timeout */
            if (ctx->retry_count < ctx->max_retries) {
                /* Increase timeout for retry */
                ctx->timeout_us = ctx->timeout_us * 2;
                if (ctx->timeout_us > 30000000) { /* Cap at 30 seconds */
                    ctx->timeout_us = 30000000;
                }
                retry_operation(state, ctx);
                return;
            }
            break;

        case 5: /* Resource exhaustion */
            if (ctx->retry_count < ctx->max_retries) {
                /* Retry with lower priority and delay */
                ctx->priority = URING_OP_PRIORITY_LOW;
                retry_operation(state, ctx);
                return;
            }
            break;

        default:
            /* Non-retryable errors */
            break;
    }

    /* If we reach here, either success or non-retryable error */
    if (result < 0) {
        update_operation_state(ctx, URING_OP_STATE_FAILED);
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "Operation failed after %d retries: %s", ctx->retry_count, strerror(-result));
    } else {
        update_operation_state(ctx, URING_OP_STATE_COMPLETED);
    }
}

/* Batch process multiple completions for better performance */
static int batch_process_completions(aeEventLoop *eventLoop, struct io_uring_cqe **cqes,
                                     int count, int *numevents) {
    aeApiState *state = eventLoop->apidata;
    int processed = 0;

    for (int i = 0; i < count; i++) {
        struct io_uring_cqe *cqe = cqes[i];
        if (!cqe) continue;

        int result = process_completion(eventLoop, cqe, numevents);
        if (result < 0) {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "Error processing completion %d/%d", i + 1, count);
            #endif
            /* Continue processing other completions */
        }
        processed++;
    }

    return processed;
}

/* Validate operation context before processing */
static int validate_operation_context(uring_op_context *ctx) {
    if (!ctx) {
        return -1;
    }

    /* Check for context corruption */
    if (ctx->fd < 0 || ctx->fd >= 65536) {
        return -2; /* Invalid file descriptor */
    }

    if (ctx->op_type < 0 || ctx->op_type > URING_OP_SEND) {
        return -3; /* Invalid operation type */
    }

    if (ctx->state < 0 || ctx->state > URING_OP_STATE_TIMEOUT) {
        return -4; /* Invalid state */
    }

    /* Check for reasonable timing values */
    uint64_t now = getMonotonicUs();
    if (ctx->submit_time > now || (now - ctx->submit_time) > 3600000000ULL) {
        return -5; /* Invalid timing */
    }

    return 0; /* Valid context */
}

/* Emergency cleanup for corrupted operations */
static void emergency_cleanup_operation(aeApiState *state, uring_op_context *ctx) {
    #ifndef REDIS_CLI_BUILD
    serverLog(LL_WARNING, "Emergency cleanup for corrupted operation: fd=%d, op_type=%d",
              ctx ? ctx->fd : -1, ctx ? ctx->op_type : -1);
    #endif

    if (ctx) {
        /* Try to clean up resources safely */
        if (ctx->buffer && ctx->buffer_id == -1) {
            return_buffer_to_pool(ctx->buffer);
        }

        /* Remove from tracking structures */
        pthread_mutex_lock(&state->op_lock);
        if (ctx->prev) ctx->prev->next = ctx->next;
        if (ctx->next) ctx->next->prev = ctx->prev;
        if (state->op_list_head == ctx) state->op_list_head = ctx->next;
        if (state->op_list_tail == ctx) state->op_list_tail = ctx->prev;
        if (state->active_operations > 0) state->active_operations--;
        pthread_mutex_unlock(&state->op_lock);

        /* Destroy the context */
        pthread_mutex_destroy(&ctx->ref_lock);
        zfree(ctx);
    }
}

#endif /* HAVE_LIBURING */
