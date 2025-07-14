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
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

/* Include buffer management functions */
#include "uring_buffer.c"

/* Team Member C - Week 1 Task 1: Operation Management Implementation */

/* Operation type enumeration as specified in implementation guide */
typedef enum {
    URING_OP_ACCEPT = 1,
    URING_OP_READ,
    URING_OP_WRITE,
    URING_OP_TIMEOUT
} uring_op_type_t;

/* Operation structure as specified in implementation guide */
typedef struct uring_operation {
    int fd;
    uring_op_type_t op_type;
    void *buffer;
    size_t buffer_size;
    void (*completion_handler)(struct uring_operation *op, int result);
    int persistent;
    monotime submit_time;
} uring_operation;

/* Forward declarations for Team Member C operation management functions */
uring_operation *create_operation(int fd, uring_op_type_t op_type);
void free_operation(uring_operation *op);
int submit_operation(aeApiState *state, uring_operation *op);

/* Forward declarations for static functions */
static int detect_uring_capabilities(void);
static int setup_sqpoll(aeApiState *state, struct io_uring_params *params);
static int setup_wakeup_mechanism(aeApiState *state);
static void free_op_context(uring_op_context *ctx);
static int submit_write_operation(aeApiState *state, int fd, uring_op_context *ctx);
static int process_completion(aeEventLoop *eventLoop, struct io_uring_cqe *cqe, int *numevents);
static void handle_read_completion(aeEventLoop *eventLoop, uring_op_context *ctx, int result, int *numevents);
static void handle_write_completion(aeEventLoop *eventLoop, uring_op_context *ctx, int result, int *numevents);
static void handle_accept_completion(aeEventLoop *eventLoop, uring_op_context *ctx, int result, int *numevents);
static void aeWakeEventLoopUring(aeEventLoop *eventLoop);
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
        return URING_CAP_NONE;
    }
    
    capabilities |= URING_CAP_BASIC;
    io_uring_queue_exit(&ring);
    
    /* Test SQPOLL support */
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL;
    if (io_uring_queue_init_params(32, &ring, &params) == 0) {
        capabilities |= URING_CAP_SQPOLL;
        io_uring_queue_exit(&ring);
    }
    
    /* Test buffer ring support (kernel 5.19+) - skip for now as it requires newer liburing */
    /* This will be detected at runtime when setting up buffer rings */
    
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
        params->flags |= IORING_SETUP_SQ_AFF;
        params->sq_thread_cpu = server.uring_config.sqpoll_cpu;
    }

    /* Set idle timeout */
    params->sq_thread_idle = server.uring_config.sqpoll_idle_ms;

    return 1;
    #else
    /* SQPOLL disabled for CLI builds */
    return 0;
    #endif
}

/* Setup wake-up mechanism for SQPOLL */
static int setup_wakeup_mechanism(aeApiState *state) {
    if (!state->sqpoll_enabled) {
        return 0;
    }
    
    /* Create eventfd for wake-up */
    state->wakeup_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (state->wakeup_eventfd == -1) {
        return -1;
    }
    
    /* Create wake-up context */
    state->wakeup_ctx = create_op_context(state->wakeup_eventfd, URING_OP_WAKEUP, AE_READABLE);
    if (!state->wakeup_ctx) {
        close(state->wakeup_eventfd);
        return -1;
    }
    
    /* Submit persistent read operation for wake-up detection */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        free_op_context(state->wakeup_ctx);
        close(state->wakeup_eventfd);
        return -1;
    }
    
    io_uring_prep_read(sqe, state->wakeup_eventfd, &state->wakeup_data, 
                       sizeof(state->wakeup_data), 0);
    io_uring_sqe_set_data(sqe, state->wakeup_ctx);
    
    return 0;
}

/* Create io_uring backend */
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));
    if (!state) return -1;
    
    memset(state, 0, sizeof(aeApiState));
    
    /* Detect capabilities */
    state->capabilities = detect_uring_capabilities();
    if (!(state->capabilities & URING_CAP_BASIC)) {
        zfree(state);
        return -1;
    }
    
    /* Configure queue sizes */
    #ifndef REDIS_CLI_BUILD
    state->sq_entries = server.uring_config.sq_entries > 0 ?
                        server.uring_config.sq_entries :
                        eventLoop->setsize * 2;
    state->cq_entries = server.uring_config.cq_entries > 0 ?
                        server.uring_config.cq_entries :
                        state->sq_entries * 2;
    #else
    state->sq_entries = eventLoop->setsize * 2;
    state->cq_entries = state->sq_entries * 2;
    #endif
    
    /* Setup io_uring parameters */
    struct io_uring_params params = {0};
    params.cq_entries = state->cq_entries;
    
    /* Configure SQPOLL if supported and enabled */
    if (state->capabilities & URING_CAP_SQPOLL) {
        state->sqpoll_enabled = setup_sqpoll(state, &params);
    }
    
    /* Initialize io_uring */
    int ret = io_uring_queue_init_params(state->sq_entries, &state->ring, &params);
    if (ret < 0) {
        /* Try without SQPOLL if it failed */
        if (state->sqpoll_enabled) {
            state->sqpoll_enabled = 0;
            memset(&params, 0, sizeof(params));
            params.cq_entries = state->cq_entries;
            ret = io_uring_queue_init_params(state->sq_entries, &state->ring, &params);
        }
        
        if (ret < 0) {
            zfree(state);
            return -1;
        }
    }
    
    state->ring_fd = state->ring.ring_fd;
    anetCloexec(state->ring_fd);
    
    /* Setup buffer ring if supported */
    if (state->capabilities & URING_CAP_BUFFER_RING) {
        if (setup_buffer_ring(state) == 0) {
            state->buffer_ring_enabled = 1;
        }
    }
    
    /* Setup wake-up mechanism for SQPOLL */
    if (state->sqpoll_enabled) {
        if (setup_wakeup_mechanism(state) < 0) {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "Failed to setup io_uring wake-up mechanism");
            #endif
            state->sqpoll_enabled = 0;
        }
    }
    
    /* Initialize context tracking */
    state->contexts = zmalloc(sizeof(uring_op_context*) * eventLoop->setsize);
    state->max_contexts = eventLoop->setsize;
    memset(state->contexts, 0, sizeof(uring_op_context*) * eventLoop->setsize);
    
    /* Initialize batch submission mutex */
    pthread_mutex_init(&state->batch_lock, NULL);

    /* Initialize buffer pool */
    #ifndef REDIS_CLI_BUILD
    int pool_size = server.uring_config.buffer_ring_size > 0 ?
                    server.uring_config.buffer_ring_size : 1024;
    int buffer_size = server.uring_config.buffer_size > 0 ?
                      server.uring_config.buffer_size : 4096;
    #else
    int pool_size = 1024;
    int buffer_size = 4096;
    #endif

    state->buffer_pool = create_buffer_pool(pool_size, buffer_size);
    if (!state->buffer_pool) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Failed to create io_uring buffer pool");
        #endif
        /* Continue without buffer pool - will use regular allocation */
    }

    eventLoop->apidata = state;

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_NOTICE, "io_uring initialized: sq_entries=%d, cq_entries=%d, sqpoll=%s, buffer_ring=%s, buffer_pool=%s",
              state->sq_entries, state->cq_entries,
              state->sqpoll_enabled ? "yes" : "no",
              state->buffer_ring_enabled ? "yes" : "no",
              state->buffer_pool ? "yes" : "no");
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
    
    /* Clean up wake-up mechanism */
    if (state->wakeup_eventfd != -1) {
        close(state->wakeup_eventfd);
    }
    if (state->wakeup_ctx) {
        free_op_context(state->wakeup_ctx);
    }
    
    /* Clean up buffer ring */
    if (state->buffer_ring_enabled) {
        cleanup_buffer_ring(state);
    }

    /* Clean up buffer pool */
    if (state->buffer_pool) {
        free_buffer_pool(state->buffer_pool);
    }

    /* Clean up contexts */
    for (int i = 0; i < state->max_contexts; i++) {
        if (state->contexts[i]) {
            free_op_context(state->contexts[i]);
        }
    }
    zfree(state->contexts);
    
    /* Clean up io_uring */
    io_uring_queue_exit(&state->ring);
    
    /* Clean up mutex */
    pthread_mutex_destroy(&state->batch_lock);
    
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
    
    return ctx;
}

/* Free operation context */
static void free_op_context(uring_op_context *ctx) {
    if (!ctx) return;
    
    if (ctx->buffer && ctx->buffer_id == -1) {
        /* Free non-buffer-ring buffer */
        zfree(ctx->buffer);
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
    }
}

/* Submit read operation */
int submit_read_operation(aeApiState *state, int fd, uring_op_context *ctx) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        state->stats.sq_full_count++;
        return -1;
    }

    /* Get buffer for read operation */
    void *buffer;
    int buffer_id = -1;

    if (state->buffer_ring_enabled) {
        buffer = get_buffer_from_ring(state, &buffer_id);
        if (buffer) {
            ctx->buffer_id = buffer_id;
            state->stats.buffer_ring_hits++;
        } else {
            state->stats.buffer_ring_misses++;
        }
    }

    if (!buffer) {
        /* Fallback to regular buffer allocation */
        buffer = zmalloc(URING_DEFAULT_BUFFER_SIZE);
        if (!buffer) return -1;
        ctx->buffer_id = -1;
    }

    ctx->buffer = buffer;
    ctx->buffer_size = URING_DEFAULT_BUFFER_SIZE;

    /* Prepare read operation */
    if (state->buffer_ring_enabled && buffer_id >= 0) {
        io_uring_prep_recv(sqe, fd, NULL, 0, 0);
        sqe->buf_group = BUFFER_RING_ID;
        sqe->flags |= IOSQE_BUFFER_SELECT;
    } else {
        io_uring_prep_read(sqe, fd, buffer, ctx->buffer_size, 0);
    }

    /* Update context for read operation */
    ctx->op_type = URING_OP_READ;
    io_uring_sqe_set_data(sqe, ctx);

    state->stats.ops_submitted++;
    return 0;
}

/* Submit write operation */
static int submit_write_operation(aeApiState *state, int fd, uring_op_context *ctx) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        state->stats.sq_full_count++;
        return -1;
    }

    /* Update context for write operation */
    ctx->op_type = URING_OP_WRITE;
    io_uring_sqe_set_data(sqe, ctx);

    /* Mark as ready for write - actual write will be handled by connection layer */
    io_uring_prep_nop(sqe);  /* NOP operation to signal write readiness */

    state->stats.ops_submitted++;
    return 0;
}

/* Submit accept operation */
int submit_accept_operation(aeApiState *state, int fd, uring_op_context *ctx) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        state->stats.sq_full_count++;
        return -1;
    }

    /* Update context for accept operation */
    ctx->op_type = URING_OP_ACCEPT;

    /* Use regular accept for now - multishot accept requires newer liburing */
    io_uring_prep_accept(sqe, fd, NULL, NULL, 0);

    io_uring_sqe_set_data(sqe, ctx);

    state->stats.ops_submitted++;
    return 0;
}

/* Main polling function */
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

    /* Submit any pending operations */
    int submitted = io_uring_submit(&state->ring);
    if (submitted < 0 && submitted != -EAGAIN) {
        return -1;
    }

    /* Wait for completions */
    int ret = io_uring_wait_cqe_timeout(&state->ring, &cqe, timeout_ptr);

    if (ret == 0) {
        /* Process all available completions */
        unsigned head;
        unsigned count = 0;

        io_uring_for_each_cqe(&state->ring, head, cqe) {
            if (process_completion(eventLoop, cqe, &numevents) < 0) {
                break;
            }
            count++;

            /* Prevent processing too many events in one iteration */
            if (count >= eventLoop->setsize) {
                break;
            }
        }

        io_uring_cq_advance(&state->ring, count);
    } else if (ret == -ETIME) {
        /* Timeout - this is normal */
        return 0;
    } else if (ret != -EINTR) {
        /* Real error */
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "io_uring_wait_cqe_timeout failed: %s", strerror(-ret));
        #endif
        return -1;
    }

    return numevents;
}

/* Process completion queue entry */
static int process_completion(aeEventLoop *eventLoop, struct io_uring_cqe *cqe, int *numevents) {
    aeApiState *state = eventLoop->apidata;
    uring_op_context *ctx = (uring_op_context *)io_uring_cqe_get_data(cqe);

    if (!ctx) {
        return 0;
    }

    int result = cqe->res;
    monotime completion_time = getMonotonicUs();
    uint64_t operation_time = completion_time - ctx->submit_time;

    /* Update statistics */
    state->stats.ops_completed++;
    state->stats.total_completion_time_us += operation_time;
    if (operation_time > state->stats.max_completion_time_us) {
        state->stats.max_completion_time_us = operation_time;
    }

    if (result < 0) {
        state->stats.ops_failed++;

        /* Handle specific errors */
        if (result == -EAGAIN || result == -EWOULDBLOCK) {
            /* Re-submit operation */
            if (ctx->op_type == URING_OP_READ) {
                submit_read_operation(state, ctx->fd, ctx);
            } else if (ctx->op_type == URING_OP_WRITE) {
                submit_write_operation(state, ctx->fd, ctx);
            }
            free_op_context(ctx);
            return 0;
        }
    }

    /* Handle completion based on operation type */
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
        case URING_OP_WAKEUP:
            /* Wake-up event - re-submit for next wake-up */
            if (result > 0) {
                state->stats.sqpoll_wakeups++;
                setup_wakeup_mechanism(state);
            }
            break;
        default:
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "Unknown io_uring operation type: %d", ctx->op_type);
            #endif
            break;
    }

    return 0;
}

/* Handle read completion */
static void handle_read_completion(aeEventLoop *eventLoop, uring_op_context *ctx, int result, int *numevents) {
    if (result > 0) {
        /* Data available - fire read event */
        aeFileEvent *fe = &eventLoop->events[ctx->fd];
        if (fe->mask & AE_READABLE && fe->rfileProc) {
            eventLoop->fired[*numevents].fd = ctx->fd;
            eventLoop->fired[*numevents].mask = AE_READABLE;
            (*numevents)++;
        }

        /* Re-submit read operation for continuous monitoring */
        aeApiState *state = eventLoop->apidata;
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
    }

    /* Return buffer to ring if applicable */
    aeApiState *state = eventLoop->apidata;
    if (ctx->buffer_id >= 0) {
        return_buffer_to_ring(state, ctx->buffer_id);
    }

    free_op_context(ctx);
}

/* Handle write completion */
static void handle_write_completion(aeEventLoop *eventLoop, uring_op_context *ctx, int result, int *numevents) {
    /* Fire write event to indicate socket is writable */
    aeFileEvent *fe = &eventLoop->events[ctx->fd];
    if (fe->mask & AE_WRITABLE && fe->wfileProc) {
        eventLoop->fired[*numevents].fd = ctx->fd;
        eventLoop->fired[*numevents].mask = AE_WRITABLE;
        (*numevents)++;
    }

    free_op_context(ctx);
}

/* Handle accept completion */
static void handle_accept_completion(aeEventLoop *eventLoop, uring_op_context *ctx, int result, int *numevents) {
    aeApiState *state = eventLoop->apidata;

    if (result >= 0) {
        /* New connection accepted - notify the event loop */
        aeFileEvent *fe = &eventLoop->events[ctx->fd];
        if (fe->mask & AE_READABLE && fe->rfileProc) {
            eventLoop->fired[*numevents].fd = ctx->fd;
            eventLoop->fired[*numevents].mask = AE_READABLE;
            (*numevents)++;
        }

        /* Re-submit accept operation for the next connection */
        ctx->accept_op.active = 0;  /* Mark as inactive before re-submitting */
        if (submit_accept_operation(state, ctx->fd, ctx) == 0) {
            ctx->accept_op.active = 1;
        }
    } else {
        /* Accept failed - mark operation as inactive */
        ctx->accept_op.active = 0;

        /* For serious errors, we might want to log them */
        if (result != -EAGAIN && result != -EWOULDBLOCK) {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "Accept operation failed: %s", strerror(-result));
            #endif
        }
    }
}

/* Wake up event loop (for SQPOLL) */
static void aeWakeEventLoopUring(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    if (state->sqpoll_enabled && state->wakeup_eventfd != -1) {
        uint64_t wake_val = 1;
        ssize_t ret = write(state->wakeup_eventfd, &wake_val, sizeof(wake_val));
        (void)ret; /* Suppress unused variable warning */
    }
}

/* Forward declaration */
static const char *aeApiName(void);

/* Get statistics for INFO command */
static void get_uring_stats(aeApiState *state, char **info) {
    #ifndef REDIS_CLI_BUILD
    *info = sdscatprintf(*info,
        "# io_uring\r\n"
        "uring_enabled:yes\r\n"
        "uring_backend:%s\r\n"
        "uring_sqpoll_enabled:%s\r\n"
        "uring_sqpoll_cpu:%d\r\n"
        "uring_sq_entries:%d\r\n"
        "uring_cq_entries:%d\r\n"
        "uring_ops_submitted:%lu\r\n"
        "uring_ops_completed:%lu\r\n"
        "uring_ops_failed:%lu\r\n"
        "uring_sq_full_count:%lu\r\n"
        "uring_cq_overflow_count:%lu\r\n"
        "uring_sqpoll_wakeups:%lu\r\n"
        "uring_avg_completion_time_us:%.2f\r\n"
        "uring_max_completion_time_us:%lu\r\n",
        aeApiName(),
        state->sqpoll_enabled ? "yes" : "no",
        state->sqpoll_cpu,
        state->sq_entries,
        state->cq_entries,
        (unsigned long)state->stats.ops_submitted,
        (unsigned long)state->stats.ops_completed,
        (unsigned long)state->stats.ops_failed,
        (unsigned long)state->stats.sq_full_count,
        (unsigned long)state->stats.cq_overflow_count,
        (unsigned long)state->stats.sqpoll_wakeups,
        state->stats.ops_completed > 0 ?
            (double)state->stats.total_completion_time_us / state->stats.ops_completed : 0.0,
        (unsigned long)state->stats.max_completion_time_us);

    /* Add buffer pool statistics if available */
    if (state->buffer_pool) {
        char *buffer_stats = NULL;
        get_buffer_pool_stats(state->buffer_pool, &buffer_stats);
        if (buffer_stats) {
            *info = sdscatsds(*info, buffer_stats);
            sdsfree(buffer_stats);
        }
    }
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
        case URING_OP_WAKEUP: return "wakeup";
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

static const char *aeApiName(void) {
    return "uring";
}

/* Public interface for getting io_uring stats */
void aeGetUringStats(aeEventLoop *eventLoop, char **info) {
    if (eventLoop && eventLoop->apidata) {
        get_uring_stats((aeApiState*)eventLoop->apidata, info);
    }
}

/* ============================= Buffer Pool Management ============================= */

/* Create a new buffer pool with specified size and buffer size */
uring_buffer_pool *create_buffer_pool(int pool_size, int buffer_size) {
    if (pool_size <= 0 || buffer_size <= 0) {
        return NULL;
    }

    uring_buffer_pool *pool = zmalloc(sizeof(uring_buffer_pool));
    if (!pool) {
        return NULL;
    }

    /* Initialize pool structure */
    pool->pool_size = pool_size;
    pool->buffer_size = buffer_size;
    pool->free_count = pool_size;
    pool->allocated_count = 0;
    pool->free_list = NULL;

    /* Initialize statistics */
    pool->total_allocations = 0;
    pool->pool_hits = 0;
    pool->pool_misses = 0;
    pool->total_deallocations = 0;
    pool->peak_usage = 0;

    /* Initialize mutex for thread safety */
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        zfree(pool);
        return NULL;
    }

    /* Allocate buffer entries array */
    pool->entries = zmalloc(sizeof(uring_buffer_entry) * pool_size);
    if (!pool->entries) {
        pthread_mutex_destroy(&pool->mutex);
        zfree(pool);
        return NULL;
    }

    /* Initialize buffer entries and build free list */
    for (int i = 0; i < pool_size; i++) {
        uring_buffer_entry *entry = &pool->entries[i];

        /* Allocate buffer memory */
        entry->buffer = zmalloc(buffer_size);
        if (!entry->buffer) {
            /* Clean up previously allocated buffers */
            for (int j = 0; j < i; j++) {
                zfree(pool->entries[j].buffer);
            }
            zfree(pool->entries);
            pthread_mutex_destroy(&pool->mutex);
            zfree(pool);
            return NULL;
        }

        entry->size = buffer_size;
        entry->in_use = 0;
        entry->last_used = 0;

        /* Add to free list */
        entry->next = pool->free_list;
        pool->free_list = entry;
    }

    return pool;
}

/* Get a buffer from the pool */
void *get_buffer_from_pool(uring_buffer_pool *pool) {
    if (!pool) {
        return NULL;
    }

    pthread_mutex_lock(&pool->mutex);

    pool->total_allocations++;

    /* Check if we have free buffers */
    if (pool->free_list == NULL) {
        pool->pool_misses++;
        pthread_mutex_unlock(&pool->mutex);
        return NULL;  /* Pool exhausted */
    }

    /* Get buffer from free list */
    uring_buffer_entry *entry = pool->free_list;
    pool->free_list = entry->next;

    entry->in_use = 1;
    entry->last_used = time(NULL);
    entry->next = NULL;

    pool->free_count--;
    pool->allocated_count++;
    pool->pool_hits++;

    /* Update peak usage */
    if (pool->allocated_count > pool->peak_usage) {
        pool->peak_usage = pool->allocated_count;
    }

    void *buffer = entry->buffer;
    pthread_mutex_unlock(&pool->mutex);

    return buffer;
}

/* Return a buffer to the pool */
void return_buffer_to_pool(uring_buffer_pool *pool, void *buffer) {
    if (!pool || !buffer) {
        return;
    }

    pthread_mutex_lock(&pool->mutex);

    pool->total_deallocations++;

    /* Find the buffer entry */
    uring_buffer_entry *entry = NULL;
    for (int i = 0; i < pool->pool_size; i++) {
        if (pool->entries[i].buffer == buffer) {
            entry = &pool->entries[i];
            break;
        }
    }

    if (!entry || !entry->in_use) {
        /* Buffer not found or already free */
        pthread_mutex_unlock(&pool->mutex);
        return;
    }

    /* Mark as free and add to free list */
    entry->in_use = 0;
    entry->next = pool->free_list;
    pool->free_list = entry;

    pool->free_count++;
    pool->allocated_count--;

    pthread_mutex_unlock(&pool->mutex);
}

/* Free the entire buffer pool */
void free_buffer_pool(uring_buffer_pool *pool) {
    if (!pool) {
        return;
    }

    pthread_mutex_lock(&pool->mutex);

    /* Free all buffer memory */
    for (int i = 0; i < pool->pool_size; i++) {
        if (pool->entries[i].buffer) {
            zfree(pool->entries[i].buffer);
        }
    }

    /* Free entries array */
    zfree(pool->entries);

    pthread_mutex_unlock(&pool->mutex);
    pthread_mutex_destroy(&pool->mutex);

    /* Free pool structure */
    zfree(pool);
}

/* Get buffer pool statistics */
void get_buffer_pool_stats(uring_buffer_pool *pool, char **info) {
    if (!pool || !info) {
        return;
    }

    pthread_mutex_lock(&pool->mutex);

    /* Calculate hit rate */
    double hit_rate = 0.0;
    if (pool->total_allocations > 0) {
        hit_rate = (double)pool->pool_hits / pool->total_allocations * 100.0;
    }

    /* Calculate utilization */
    double utilization = (double)pool->allocated_count / pool->pool_size * 100.0;

    /* Format statistics string */
    *info = sdscatprintf(sdsempty(),
        "buffer_pool_size:%d\r\n"
        "buffer_pool_buffer_size:%d\r\n"
        "buffer_pool_free_count:%d\r\n"
        "buffer_pool_allocated_count:%d\r\n"
        "buffer_pool_total_allocations:%llu\r\n"
        "buffer_pool_hits:%llu\r\n"
        "buffer_pool_misses:%llu\r\n"
        "buffer_pool_hit_rate:%.2f\r\n"
        "buffer_pool_total_deallocations:%llu\r\n"
        "buffer_pool_peak_usage:%llu\r\n"
        "buffer_pool_utilization:%.2f\r\n",
        pool->pool_size,
        pool->buffer_size,
        pool->free_count,
        pool->allocated_count,
        pool->total_allocations,
        pool->pool_hits,
        pool->pool_misses,
        hit_rate,
        pool->total_deallocations,
        pool->peak_usage,
        utilization
    );

    pthread_mutex_unlock(&pool->mutex);
}

/* ============================================================================
 * Team Member C - Week 1 Task 1: Operation Management Implementation
 * ============================================================================ */

/* Operation management functions as specified in implementation guide Step 2.3 */

/* Create a new operation with specified file descriptor and operation type */
uring_operation *create_operation(int fd, uring_op_type_t op_type) {
    uring_operation *op = zmalloc(sizeof(uring_operation));
    if (!op) return NULL;

    op->fd = fd;
    op->op_type = op_type;
    op->buffer = NULL;
    op->buffer_size = 0;
    op->completion_handler = NULL;
    op->persistent = 0;
    op->submit_time = getMonotonicUs();

    return op;
}

/* Free an operation and its associated resources */
void free_operation(uring_operation *op) {
    if (!op) return;

    /* Return buffer to pool if it was allocated */
    if (op->buffer) {
        /* Note: This requires access to the buffer pool */
        zfree(op->buffer);  /* Simplified - should use return_buffer_to_pool */
    }

    zfree(op);
}

/* Submit an operation to the io_uring submission queue */
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
        case URING_OP_TIMEOUT:
            /* Timeout operations would be implemented here */
            return -1; /* Not implemented yet */
        default:
            state->stats.ops_failed++;
            return -1;
    }

    io_uring_sqe_set_data(sqe, op);

    /* Update statistics */
    state->stats.ops_submitted++;

    /* Track operation by FD for proper lifecycle management */
    if (op->fd >= 0 && op->fd < state->max_contexts) {
        /* Store operation reference for completion handling */
        /* Note: In the current implementation, we use the existing contexts array
         * In a full implementation following the guide, we would have a separate
         * operations array: state->operations[op->fd] = op; */
    }

    return 0;
}

/* Helper function to update operation statistics */
static void update_operation_statistics(aeApiState *state, uring_operation *op, int result) {
    if (result >= 0) {
        state->stats.ops_completed++;

        /* Calculate completion time */
        monotime completion_time = getMonotonicUs();
        uint64_t operation_duration = completion_time - op->submit_time;

        state->stats.total_completion_time_us += operation_duration;
        if (operation_duration > state->stats.max_completion_time_us) {
            state->stats.max_completion_time_us = operation_duration;
        }
    } else {
        state->stats.ops_failed++;
    }
}

/* Process completion for uring_operation (Team Member C implementation) */
static int process_operation_completion(aeEventLoop *eventLoop, uring_operation *op, int result) {
    aeApiState *state = eventLoop->apidata;

    /* Update operation statistics */
    update_operation_statistics(state, op, result);

    /* Call operation-specific completion handler if provided */
    if (op->completion_handler) {
        op->completion_handler(op, result);
    }

    /* Handle persistent operations (auto-resubmit) */
    if (op->persistent && result >= 0) {
        /* Reset submit time for the resubmitted operation */
        op->submit_time = getMonotonicUs();

        /* Resubmit the operation */
        if (submit_operation(state, op) < 0) {
            /* If resubmission fails, clean up the operation */
            free_operation(op);
            return -1;
        }
        return 0; /* Operation resubmitted, don't free it */
    }

    /* Clear operation tracking */
    if (op->fd >= 0 && op->fd < state->max_contexts) {
        /* In a full implementation: state->operations[op->fd] = NULL; */
    }

    /* Free the operation */
    free_operation(op);
    return 0;
}

#endif /* HAVE_LIBURING */
