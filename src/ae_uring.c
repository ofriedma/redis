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

/* Team Member C - Operation Management Functions */
typedef struct uring_operation {
    int fd;
    int op_type;
    void *buffer;
    size_t buffer_size;
    void (*completion_handler)(struct uring_operation *op, int result);
    int persistent;
    monotime submit_time;
    uring_op_context *ctx;  /* Link to underlying context */
} uring_operation;

static uring_operation *create_operation(int fd, int op_type);
static void free_operation(uring_operation *op);
static int submit_operation(aeApiState *state, uring_operation *op);
static void handle_operation_completion(uring_operation *op, int result);

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
    
    eventLoop->apidata = state;

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_NOTICE, "io_uring initialized: sq_entries=%d, cq_entries=%d, sqpoll=%s, buffer_ring=%s",
              state->sq_entries, state->cq_entries,
              state->sqpoll_enabled ? "yes" : "no",
              state->buffer_ring_enabled ? "yes" : "no");
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

/* Enhanced completion processing for both contexts and operations */
static int process_completion_enhanced(aeEventLoop *eventLoop, struct io_uring_cqe *cqe, int *numevents) {
    aeApiState *state = eventLoop->apidata;
    void *user_data = io_uring_cqe_get_data(cqe);
    int result = cqe->res;

    if (!user_data) {
        return 0;
    }

    /* Check if this is a new-style operation or old-style context */
    /* We can distinguish by checking the first few bytes - operations have fd, contexts have fd too
     * but we'll use a simple heuristic: if it looks like an operation, treat it as one */
    uring_operation *op = (uring_operation *)user_data;
    uring_op_context *ctx = (uring_op_context *)user_data;

    /* Simple heuristic: operations have completion_handler field, contexts don't */
    int is_operation = 0;
    if (op && op->completion_handler) {
        is_operation = 1;
    }

    monotime completion_time = getMonotonicUs();
    uint64_t operation_time;

    if (is_operation) {
        operation_time = completion_time - op->submit_time;
        /* Handle new-style operation */
        state->stats.ops_completed++;
        state->stats.total_completion_time_us += operation_time;
        if (operation_time > state->stats.max_completion_time_us) {
            state->stats.max_completion_time_us = operation_time;
        }

        if (result < 0) {
            state->stats.ops_failed++;
        }

        /* Call operation completion handler */
        handle_operation_completion(op, result);

        /* For persistent operations, resubmit */
        if (op->persistent && result >= 0) {
            submit_operation(state, op);
        }
    } else {
        /* Handle old-style context */
        operation_time = completion_time - ctx->submit_time;
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
    }

    return 0;
}

/* Process completion queue entry - legacy function for compatibility */
static int process_completion(aeEventLoop *eventLoop, struct io_uring_cqe *cqe, int *numevents) {
    /* For now, use the enhanced processor */
    int ret = process_completion_enhanced(eventLoop, cqe, numevents);
    if (ret != 0) return ret;

    /* Continue with legacy completion handling for contexts */
    aeApiState *state = eventLoop->apidata;
    uring_op_context *ctx = (uring_op_context *)io_uring_cqe_get_data(cqe);

    if (!ctx) {
        return 0;
    }

    int result = cqe->res;

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

/* Enhanced statistics for INFO command */
static void get_uring_stats(aeApiState *state, char **info) {
    #ifndef REDIS_CLI_BUILD
    /* Calculate derived statistics */
    double avg_completion_time = state->stats.ops_completed > 0 ?
        (double)state->stats.total_completion_time_us / state->stats.ops_completed : 0.0;

    double success_rate = (state->stats.ops_submitted > 0) ?
        (double)state->stats.ops_completed / state->stats.ops_submitted * 100.0 : 0.0;

    uint64_t pending_ops = state->stats.ops_submitted -
                          state->stats.ops_completed -
                          state->stats.ops_failed;

    *info = sdscatprintf(*info,
        "# io_uring\r\n"
        "uring_enabled:yes\r\n"
        "uring_backend:%s\r\n"
        "uring_sqpoll_enabled:%s\r\n"
        "uring_sqpoll_cpu:%d\r\n"
        "uring_sq_entries:%d\r\n"
        "uring_cq_entries:%d\r\n"
        "uring_buffer_ring_enabled:%s\r\n"
        "uring_ops_submitted:%lu\r\n"
        "uring_ops_completed:%lu\r\n"
        "uring_ops_failed:%lu\r\n"
        "uring_ops_pending:%lu\r\n"
        "uring_success_rate:%.2f\r\n"
        "uring_sq_full_count:%lu\r\n"
        "uring_cq_overflow_count:%lu\r\n"
        "uring_sqpoll_wakeups:%lu\r\n"
        "uring_buffer_ring_hits:%lu\r\n"
        "uring_buffer_ring_misses:%lu\r\n"
        "uring_avg_completion_time_us:%.2f\r\n"
        "uring_max_completion_time_us:%lu\r\n",
        aeApiName(),
        state->sqpoll_enabled ? "yes" : "no",
        state->sqpoll_cpu,
        state->sq_entries,
        state->cq_entries,
        state->buffer_ring_enabled ? "yes" : "no",
        (unsigned long)state->stats.ops_submitted,
        (unsigned long)state->stats.ops_completed,
        (unsigned long)state->stats.ops_failed,
        (unsigned long)pending_ops,
        success_rate,
        (unsigned long)state->stats.sq_full_count,
        (unsigned long)state->stats.cq_overflow_count,
        (unsigned long)state->stats.sqpoll_wakeups,
        (unsigned long)state->stats.buffer_ring_hits,
        (unsigned long)state->stats.buffer_ring_misses,
        avg_completion_time,
        (unsigned long)state->stats.max_completion_time_us);
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

/* Enhanced operation statistics tracking */
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

/* Enhanced statistics for operations */
static void update_enhanced_operation_stats(aeApiState *state, uring_operation *op, int result) {
    if (!state || !op) return;

    monotime now = getMonotonicUs();
    uint64_t operation_time = now - op->submit_time;

    /* Update timing statistics */
    state->stats.total_completion_time_us += operation_time;
    if (operation_time > state->stats.max_completion_time_us) {
        state->stats.max_completion_time_us = operation_time;
    }

    /* Update operation counts */
    if (result < 0) {
        state->stats.ops_failed++;

        /* Track specific error types for debugging */
        switch (result) {
            case -EAGAIN:
            case -EWOULDBLOCK:
                /* These are tracked separately as they're often retried */
                break;
            case -ECONNRESET:
            case -EPIPE:
                /* Connection errors */
                break;
            case -ENOMEM:
                /* Memory pressure */
                break;
            default:
                /* Other errors */
                break;
        }
    } else {
        state->stats.ops_completed++;
    }
}

/* Performance monitoring and debugging */
static void log_performance_warning(aeApiState *state, uring_operation *op, uint64_t operation_time) {
    #ifndef REDIS_CLI_BUILD
    /* Log slow operations for debugging */
    if (operation_time > 10000) {  /* 10ms threshold */
        serverLog(LL_WARNING,
            "Slow io_uring operation: fd=%d, type=%s, time=%lu us",
            op->fd, uring_op_type_to_string(op->op_type), operation_time);
    }

    /* Log queue pressure */
    if (state->stats.sq_full_count > 0 &&
        (state->stats.sq_full_count % 1000) == 0) {
        serverLog(LL_WARNING,
            "io_uring submission queue full %lu times",
            (unsigned long)state->stats.sq_full_count);
    }
    #else
    (void)state; (void)op; (void)operation_time;
    #endif
}

/* Debug information for operations */
static void debug_operation_info(uring_operation *op, int result) {
    #ifndef REDIS_CLI_BUILD
    if (server.verbosity >= LL_DEBUG) {
        serverLog(LL_DEBUG,
            "io_uring operation completed: fd=%d, type=%s, result=%d, persistent=%d",
            op->fd, uring_op_type_to_string(op->op_type), result, op->persistent);
    }
    #else
    (void)op; (void)result;
    #endif
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

/* ============================================================================
 * Team Member C - Operation Management Functions
 * ============================================================================ */

/* Create operation with completion handler support */
static uring_operation *create_operation(int fd, int op_type) {
    uring_operation *op = zmalloc(sizeof(uring_operation));
    if (!op) return NULL;

    memset(op, 0, sizeof(uring_operation));
    op->fd = fd;
    op->op_type = op_type;
    op->buffer = NULL;
    op->buffer_size = 0;
    op->completion_handler = NULL;
    op->persistent = 0;
    op->submit_time = getMonotonicUs();
    op->ctx = NULL;

    return op;
}

/* Free operation with proper buffer cleanup */
static void free_operation(uring_operation *op) {
    if (!op) return;

    /* Return buffer to pool if it was allocated */
    if (op->buffer) {
        return_buffer_to_pool(op->buffer);
        op->buffer = NULL;
    }

    /* Free underlying context if it exists */
    if (op->ctx) {
        free_op_context(op->ctx);
        op->ctx = NULL;
    }

    zfree(op);
}

/* Submit operation to io_uring */
static int submit_operation(aeApiState *state, uring_operation *op) {
    if (!state || !op) return -1;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        state->stats.ops_failed++;
        return -1;
    }

    /* Create underlying context if needed */
    if (!op->ctx) {
        op->ctx = create_op_context(op->fd, op->op_type, 0);
        if (!op->ctx) {
            state->stats.ops_failed++;
            return -1;
        }
    }

    /* Setup operation based on type */
    switch (op->op_type) {
        case URING_OP_READ:
            if (!op->buffer) {
                op->buffer = get_buffer_from_pool();
                if (!op->buffer) {
                    state->stats.ops_failed++;
                    return -1;
                }
                op->buffer_size = URING_DEFAULT_BUFFER_SIZE;
            }
            io_uring_prep_recv(sqe, op->fd, op->buffer, op->buffer_size, 0);
            break;

        case URING_OP_WRITE:
            if (!op->buffer || op->buffer_size == 0) {
                state->stats.ops_failed++;
                return -1;
            }
            io_uring_prep_send(sqe, op->fd, op->buffer, op->buffer_size, 0);
            break;

        case URING_OP_ACCEPT:
            io_uring_prep_accept(sqe, op->fd, NULL, NULL, 0);
            break;

        default:
            state->stats.ops_failed++;
            return -1;
    }

    /* Store operation pointer in SQE user data */
    io_uring_sqe_set_data(sqe, op);

    /* Update statistics */
    state->stats.ops_submitted++;
    op->submit_time = getMonotonicUs();

    return 0;
}

/* Enhanced operation completion handler with statistics */
static void handle_operation_completion(uring_operation *op, int result) {
    if (!op) return;

    /* Debug logging */
    debug_operation_info(op, result);

    /* Performance monitoring */
    monotime now = getMonotonicUs();
    uint64_t operation_time = now - op->submit_time;

    /* Call user-defined completion handler if available */
    if (op->completion_handler) {
        op->completion_handler(op, result);
    }

    /* Handle persistent operations */
    if (op->persistent && result >= 0) {
        /* For persistent operations, we need access to the state to resubmit */
        /* This will be handled by the main completion processing logic */
        return;
    }

    /* Non-persistent operations or failed operations are freed */
    free_operation(op);
}

/* ============================================================================
 * Enhanced Completion Handlers with Redis Connection Integration
 * ============================================================================ */

/* Enhanced accept completion handler */
static void enhanced_handle_accept_completion(uring_operation *op, int result) {
    if (result < 0) {
        if (result != -EAGAIN && result != -EWOULDBLOCK) {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "Accept failed: %s", strerror(-result));
            #endif
        }
        return;
    }

    int client_fd = result;

    #ifndef REDIS_CLI_BUILD
    /* Create new client connection using existing Redis logic */
    connection *conn = connCreateAcceptedSocket(client_fd, NULL);
    if (!conn) {
        close(client_fd);
        return;
    }

    /* Set up connection for io_uring operations */
    /* Note: This would require extending the connection structure */
    /* For now, we'll use the standard connection setup */

    /* The connection will be handled by the normal Redis networking code */
    /* which will set up appropriate handlers */
    #else
    /* For CLI builds, just close the connection */
    close(client_fd);
    #endif
}

/* Enhanced read completion handler */
static void enhanced_handle_read_completion(uring_operation *op, int result) {
    if (result > 0) {
        #ifndef REDIS_CLI_BUILD
        /* Data received - find the connection and process it */
        /* Note: Redis doesn't have a direct connByFd function, so we'll need to
         * work with the event loop to find the connection */

        /* For now, we'll trigger the read event in the event loop */
        /* This allows existing Redis code to handle the data */

        /* The buffer data is in op->buffer with size result */
        /* We need to make this available to the connection handler */

        /* This is a simplified approach - in a full implementation,
         * we would need to extend the connection structure to hold
         * the io_uring buffer data */
        #endif

        /* Return buffer to pool */
        return_buffer_to_pool(op->buffer);
        op->buffer = NULL;

        /* For persistent read operations, get a new buffer and resubmit */
        if (op->persistent) {
            op->buffer = get_buffer_from_pool();
            if (op->buffer) {
                op->buffer_size = URING_DEFAULT_BUFFER_SIZE;
                /* Resubmission will be handled by the main completion logic */
            }
        }
    } else if (result == 0) {
        /* Connection closed */
        #ifndef REDIS_CLI_BUILD
        /* Signal connection closure to Redis */
        /* This would normally be handled by the connection's close handler */
        #endif
        op->persistent = 0;  /* Stop resubmitting */
    } else {
        /* Error occurred */
        if (result != -EAGAIN && result != -EWOULDBLOCK) {
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_DEBUG, "Read error on fd %d: %s", op->fd, strerror(-result));
            #endif
        }
        op->persistent = 0;  /* Stop resubmitting */
    }
}

/* Enhanced write completion handler */
static void enhanced_handle_write_completion(uring_operation *op, int result) {
    if (result > 0) {
        /* Write successful */
        #ifndef REDIS_CLI_BUILD
        /* Notify Redis that write completed */
        /* This would normally trigger the write handler to send more data */
        #endif
    } else if (result < 0 && result != -EAGAIN && result != -EWOULDBLOCK) {
        /* Write error */
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_DEBUG, "Write error on fd %d: %s", op->fd, strerror(-result));
        /* Signal error to connection */
        #endif
    }
    /* Write operations are typically one-shot, don't resubmit */
    op->persistent = 0;
}

/* Error handling for operations */
static void handle_operation_error(uring_operation *op, int error) {
    switch (error) {
        case -EAGAIN:
        case -EWOULDBLOCK:
            /* Temporary error - operation will be retried by caller if persistent */
            break;

        case -ECONNRESET:
        case -EPIPE:
            /* Connection closed - clean up */
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_DEBUG, "Connection closed on fd %d: %s", op->fd, strerror(-error));
            #endif
            op->persistent = 0;
            break;

        case -ENOMEM:
            /* Memory pressure - stop operation */
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "Memory pressure on fd %d, stopping operation", op->fd);
            #endif
            op->persistent = 0;
            break;

        default:
            /* Other errors - log and stop operation */
            #ifndef REDIS_CLI_BUILD
            serverLog(LL_WARNING, "io_uring operation failed: fd=%d, op=%d, error=%s",
                     op->fd, op->op_type, strerror(-error));
            #endif
            op->persistent = 0;
            break;
    }
}

#endif /* HAVE_LIBURING */
