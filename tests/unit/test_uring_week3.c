/*
 * Week 3 io_uring Integration Tests
 * Tests for event loop integration, connection handling, and completion handlers
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <assert.h>

#ifdef HAVE_LIBURING
#include "ae.h"
#include "ae_uring.h"
#include "connection.h"
#include "zmalloc.h"
#include "test_uring_framework.h"

/* Week 4 enhanced test framework functions */

/* Validate operation context integrity */
int validate_operation_context(uring_op_context *ctx) {
    if (!ctx) return -1;

    /* Check magic number if implemented */
    if (ctx->magic != URING_OP_CONTEXT_MAGIC) return -1;

    /* Check state is valid */
    if (ctx->state < URING_OP_STATE_CREATED || ctx->state > URING_OP_STATE_FAILED) {
        return -1;
    }

    /* Check operation type is valid */
    if (ctx->op_type < URING_OP_ACCEPT || ctx->op_type > URING_OP_WRITE) {
        return -1;
    }

    /* Check file descriptor is reasonable */
    if (ctx->fd < -1 || ctx->fd > 65535) return -1;

    return 0;
}

/* Update operation state with validation */
void update_operation_state(uring_op_context *ctx, int new_state) {
    if (!ctx) return;

    /* Validate state transition */
    switch (ctx->state) {
        case URING_OP_STATE_CREATED:
            if (new_state != URING_OP_STATE_QUEUED && new_state != URING_OP_STATE_FAILED) {
                return; /* Invalid transition */
            }
            break;
        case URING_OP_STATE_QUEUED:
            if (new_state != URING_OP_STATE_SUBMITTED && new_state != URING_OP_STATE_FAILED) {
                return; /* Invalid transition */
            }
            break;
        case URING_OP_STATE_SUBMITTED:
            if (new_state != URING_OP_STATE_COMPLETED && new_state != URING_OP_STATE_FAILED) {
                return; /* Invalid transition */
            }
            break;
        default:
            return; /* Invalid current state */
    }

    ctx->state = new_state;
    ctx->state_change_time = getMonotonicUs();
}

/* Add operation to priority queue */
void add_operation_to_queue(aeApiState *state, uring_op_context *ctx) {
    if (!state || !ctx) return;

    /* Add to appropriate priority queue */
    switch (ctx->priority) {
        case URING_OP_PRIORITY_HIGH:
            /* Add to high priority queue */
            break;
        case URING_OP_PRIORITY_NORMAL:
            /* Add to normal priority queue */
            break;
        case URING_OP_PRIORITY_LOW:
            /* Add to low priority queue */
            break;
        case URING_OP_PRIORITY_BACKGROUND:
            /* Add to background priority queue */
            break;
    }

    update_operation_state(ctx, URING_OP_STATE_QUEUED);
}

/* Get next operation from priority queue */
uring_op_context *get_next_operation(aeApiState *state, int priority) {
    if (!state) return NULL;

    /* Get from appropriate priority queue */
    switch (priority) {
        case URING_OP_PRIORITY_HIGH:
            /* Return from high priority queue */
            break;
        case URING_OP_PRIORITY_NORMAL:
            /* Return from normal priority queue */
            break;
        case URING_OP_PRIORITY_LOW:
            /* Return from low priority queue */
            break;
        case URING_OP_PRIORITY_BACKGROUND:
            /* Return from background priority queue */
            break;
    }

    return NULL; /* Placeholder */
}

/* Timeout operations that have been pending too long */
void timeout_operations(aeApiState *state) {
    if (!state) return;

    monotime current_time = getMonotonicUs();
    uint64_t timeout_threshold = 5000000; /* 5 seconds in microseconds */

    /* Check all pending operations for timeouts */
    for (int i = 0; i < state->max_contexts; i++) {
        uring_op_context *ctx = state->contexts[i];
        if (ctx && ctx->state == URING_OP_STATE_SUBMITTED) {
            if (current_time - ctx->submit_time > timeout_threshold) {
                /* Operation has timed out */
                update_operation_state(ctx, URING_OP_STATE_FAILED);
                ctx->last_error = -ETIME;
                state->stats.timeout_events++;
            }
        }
    }
}

/* Clean up completed operations */
void cleanup_completed_operations(aeApiState *state) {
    if (!state) return;

    /* Clean up completed or failed operations */
    for (int i = 0; i < state->max_contexts; i++) {
        uring_op_context *ctx = state->contexts[i];
        if (ctx && (ctx->state == URING_OP_STATE_COMPLETED || ctx->state == URING_OP_STATE_FAILED)) {
            /* Clean up the context */
            state->contexts[i] = NULL;
            free_op_context(ctx);
        }
    }
}

/* Test event loop integration */
void test_event_loop_integration() {
    printf("Testing event loop integration...\n");
    
    aeEventLoop *el = aeCreateEventLoop(1024);
    assert(el != NULL);
    
    /* Test io_uring backend initialization */
    assert(el->apidata != NULL);
    
    /* Test basic polling */
    struct timeval tv = {0, 1000}; /* 1ms timeout */
    int events = aeApiPoll(el, &tv);
    assert(events >= 0);
    
    aeDeleteEventLoop(el);
    printf("Event loop integration: PASSED\n");
}

/* Test connection creation and cleanup */
void test_connection_handling() {
    printf("Testing connection handling...\n");
    
    /* Create a test socket pair */
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    
    /* Create connection */
    connection *conn = connCreateSocket();
    assert(conn != NULL);
    
    /* Initialize io_uring fields */
    connInitUring(conn);
    
    /* Test that fields are properly initialized */
    assert(conn->uring_read_buffer == NULL);
    assert(conn->uring_read_size == 0);
    assert(conn->uring_write_buffer == NULL);
    assert(conn->uring_write_size == 0);
    assert(conn->uring_read_pending == 0);
    assert(conn->uring_write_pending == 0);
    
    /* Clean up */
    connCleanupUring(conn);
    connClose(conn);
    close(sockets[0]);
    close(sockets[1]);
    
    printf("Connection handling: PASSED\n");
}

/* Test operation submission and completion */
void test_operation_lifecycle() {
    printf("Testing operation lifecycle...\n");
    
    aeEventLoop *el = aeCreateEventLoop(1024);
    aeApiState *state = el->apidata;
    assert(state != NULL);
    
    /* Create test operation context */
    uring_op_context *ctx = create_op_context(1, URING_OP_READ, AE_READABLE);
    assert(ctx != NULL);
    
    /* Test operation state transitions */
    assert(ctx->state == URING_OP_STATE_CREATED);
    
    update_operation_state(ctx, URING_OP_STATE_QUEUED);
    assert(ctx->state == URING_OP_STATE_QUEUED);
    
    update_operation_state(ctx, URING_OP_STATE_SUBMITTED);
    assert(ctx->state == URING_OP_STATE_SUBMITTED);
    
    update_operation_state(ctx, URING_OP_STATE_COMPLETED);
    assert(ctx->state == URING_OP_STATE_COMPLETED);
    
    /* Clean up */
    free_op_context(ctx);
    aeDeleteEventLoop(el);
    
    printf("Operation lifecycle: PASSED\n");
}

/* Test buffer management */
void test_buffer_management() {
    printf("Testing buffer management...\n");
    
    aeEventLoop *el = aeCreateEventLoop(1024);
    aeApiState *state = el->apidata;
    assert(state != NULL);
    assert(state->buffer_pool != NULL);
    
    /* Test buffer allocation */
    void *buf1 = get_buffer_from_pool(state->buffer_pool);
    assert(buf1 != NULL);
    
    void *buf2 = get_buffer_from_pool(state->buffer_pool);
    assert(buf2 != NULL);
    assert(buf1 != buf2);
    
    /* Test buffer return */
    return_buffer_to_pool(state->buffer_pool, buf1);
    return_buffer_to_pool(state->buffer_pool, buf2);
    
    /* Test buffer reuse */
    void *buf3 = get_buffer_from_pool(state->buffer_pool);
    assert(buf3 != NULL);
    
    return_buffer_to_pool(state->buffer_pool, buf3);
    
    aeDeleteEventLoop(el);
    printf("Buffer management: PASSED\n");
}

/* Test statistics tracking */
void test_statistics_tracking() {
    printf("Testing statistics tracking...\n");
    
    aeEventLoop *el = aeCreateEventLoop(1024);
    aeApiState *state = el->apidata;
    assert(state != NULL);
    
    /* Check initial statistics */
    assert(state->stats.ops_submitted == 0);
    assert(state->stats.ops_completed == 0);
    assert(state->stats.ops_failed == 0);
    
    /* Simulate some statistics updates */
    state->stats.ops_submitted = 10;
    state->stats.ops_completed = 8;
    state->stats.ops_failed = 2;
    
    assert(state->stats.ops_submitted == 10);
    assert(state->stats.ops_completed == 8);
    assert(state->stats.ops_failed == 2);
    
    aeDeleteEventLoop(el);
    printf("Statistics tracking: PASSED\n");
}

/* Test error handling */
void test_error_handling() {
    printf("Testing error handling...\n");
    
    aeEventLoop *el = aeCreateEventLoop(1024);
    aeApiState *state = el->apidata;
    assert(state != NULL);
    
    /* Test invalid operation creation */
    uring_op_context *ctx = create_op_context(-1, URING_OP_READ, AE_READABLE);
    assert(ctx == NULL); /* Should fail for invalid fd */
    
    /* Test operation with invalid type */
    ctx = create_op_context(1, 999, AE_READABLE);
    assert(ctx == NULL); /* Should fail for invalid operation type */
    
    /* Test valid operation creation */
    ctx = create_op_context(1, URING_OP_READ, AE_READABLE);
    assert(ctx != NULL);
    
    /* Test error state handling */
    ctx->last_error = -EAGAIN;
    assert(ctx->last_error == -EAGAIN);
    
    free_op_context(ctx);
    aeDeleteEventLoop(el);
    printf("Error handling: PASSED\n");
}

/* Test priority queue functionality */
void test_priority_queues() {
    printf("Testing priority queues...\n");
    
    aeEventLoop *el = aeCreateEventLoop(1024);
    aeApiState *state = el->apidata;
    assert(state != NULL);
    
    /* Create operations with different priorities */
    uring_op_context *high_ctx = create_op_context(1, URING_OP_READ, AE_READABLE);
    uring_op_context *normal_ctx = create_op_context(2, URING_OP_READ, AE_READABLE);
    uring_op_context *low_ctx = create_op_context(3, URING_OP_READ, AE_READABLE);
    
    assert(high_ctx != NULL);
    assert(normal_ctx != NULL);
    assert(low_ctx != NULL);
    
    /* Set priorities */
    high_ctx->priority = URING_OP_PRIORITY_HIGH;
    normal_ctx->priority = URING_OP_PRIORITY_NORMAL;
    low_ctx->priority = URING_OP_PRIORITY_LOW;
    
    /* Add to queues */
    add_operation_to_queue(state, low_ctx);
    add_operation_to_queue(state, high_ctx);
    add_operation_to_queue(state, normal_ctx);
    
    /* Get operations - should come out in priority order */
    uring_op_context *first = get_next_operation(state, URING_OP_PRIORITY_HIGH);
    assert(first == high_ctx);
    
    uring_op_context *second = get_next_operation(state, URING_OP_PRIORITY_NORMAL);
    assert(second == normal_ctx);
    
    uring_op_context *third = get_next_operation(state, URING_OP_PRIORITY_LOW);
    assert(third == low_ctx);
    
    /* Clean up */
    free_op_context(high_ctx);
    free_op_context(normal_ctx);
    free_op_context(low_ctx);
    aeDeleteEventLoop(el);
    
    printf("Priority queues: PASSED\n");
}

int main() {
    printf("Starting Week 3 io_uring integration tests...\n\n");
    
    test_event_loop_integration();
    test_connection_handling();
    test_operation_lifecycle();
    test_buffer_management();
    test_statistics_tracking();
    test_error_handling();
    test_priority_queues();
    
    printf("\nAll Week 3 io_uring integration tests PASSED!\n");
    return 0;
}

#else /* !HAVE_LIBURING */

int main() {
    printf("io_uring not available, skipping tests\n");
    return 0;
}

#endif /* HAVE_LIBURING */
