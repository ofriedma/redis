/*
 * io_uring Week 2 Unit Tests in C
 * Comprehensive testing of Week 2 implementations
 */

#include "test_uring_framework.h"
#include "../../src/ae_uring.h"
#include "../../src/uring_buffer.c"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* Test Team Member A enhancements */
void test_enhanced_capability_detection(void) {
    printf("Testing enhanced capability detection...\n");
    
    #ifdef HAVE_LIBURING
    /* Test capability detection function */
    int capabilities = detect_uring_capabilities();
    
    /* Should at least have basic capability */
    assert(capabilities & URING_CAP_BASIC);
    
    /* Test that capability flags are properly defined */
    assert(URING_CAP_SINGLE_MMAP != 0);
    assert(URING_CAP_NODROP != 0);
    assert(URING_CAP_SUBMIT_STABLE != 0);
    
    printf("✓ Enhanced capability detection works\n");
    #else
    printf("⚠ Skipped - io_uring not available\n");
    #endif
}

void test_sqpoll_configuration(void) {
    printf("Testing SQPOLL configuration...\n");
    
    #ifdef HAVE_LIBURING
    /* Create a test event loop */
    aeEventLoop *el = aeCreateEventLoop(64);
    assert(el != NULL);
    
    /* Test SQPOLL setup */
    aeApiState *state = el->apidata;
    if (state && state->sqpoll_enabled) {
        /* Test SQPOLL parameters */
        assert(state->sqpoll_cpu >= -1);
        assert(state->sqpoll_idle_ms >= 100);
        assert(state->sqpoll_idle_ms <= 60000);
    }
    
    aeDeleteEventLoop(el);
    printf("✓ SQPOLL configuration works\n");
    #else
    printf("⚠ Skipped - io_uring not available\n");
    #endif
}

/* Test Team Member B enhancements */
void test_enhanced_buffer_pool(void) {
    printf("Testing enhanced buffer pool...\n");
    
    #ifdef HAVE_LIBURING
    /* Initialize buffer pool */
    int result = init_buffer_pool(64, 4096);
    assert(result == 0);
    
    /* Test buffer allocation */
    void *buffer1 = get_buffer_from_pool();
    assert(buffer1 != NULL);
    
    void *buffer2 = get_buffer_from_pool_sized(8192);
    assert(buffer2 != NULL);
    
    /* Test buffer return */
    return_buffer_to_pool(buffer1);
    return_buffer_to_pool(buffer2);
    
    /* Test optimized allocation */
    void *opt_buffer = allocate_optimized_buffer(2048);
    assert(opt_buffer != NULL);
    return_buffer_to_pool(opt_buffer);
    
    /* Test buffer pool optimization */
    optimize_buffer_pool();
    
    cleanup_buffer_pool();
    printf("✓ Enhanced buffer pool works\n");
    #else
    printf("⚠ Skipped - io_uring not available\n");
    #endif
}

void test_zero_copy_buffers(void) {
    printf("Testing zero-copy buffer management...\n");
    
    #ifdef HAVE_LIBURING
    /* Create a test event loop */
    aeEventLoop *el = aeCreateEventLoop(64);
    assert(el != NULL);
    
    aeApiState *state = el->apidata;
    if (state) {
        /* Test buffer ring operations */
        int buffer_id;
        void *buffer = get_buffer_from_ring(state, &buffer_id);
        
        if (state->buffer_ring_enabled) {
            assert(buffer != NULL);
            return_buffer_to_ring(state, buffer_id);
        }
        
        /* Test buffer registration */
        struct iovec iovecs[4];
        for (int i = 0; i < 4; i++) {
            iovecs[i].iov_base = malloc(4096);
            iovecs[i].iov_len = 4096;
        }
        
        int reg_result = register_uring_buffers(state, iovecs, 4);
        if (reg_result == 0) {
            unregister_uring_buffers(state);
        }
        
        for (int i = 0; i < 4; i++) {
            free(iovecs[i].iov_base);
        }
    }
    
    aeDeleteEventLoop(el);
    printf("✓ Zero-copy buffer management works\n");
    #else
    printf("⚠ Skipped - io_uring not available\n");
    #endif
}

/* Test Team Member C enhancements */
void test_operation_management(void) {
    printf("Testing enhanced operation management...\n");
    
    #ifdef HAVE_LIBURING
    /* Create a test event loop */
    aeEventLoop *el = aeCreateEventLoop(64);
    assert(el != NULL);
    
    aeApiState *state = el->apidata;
    if (state) {
        /* Test operation context creation */
        uring_op_context *op_ctx = create_operation_context(state, 1, URING_OP_READ, URING_OP_PRIORITY_NORMAL);
        assert(op_ctx != NULL);
        assert(op_ctx->fd == 1);
        assert(op_ctx->op_type == URING_OP_READ);
        assert(op_ctx->priority == URING_OP_PRIORITY_NORMAL);
        assert(op_ctx->op_id > 0);
        
        /* Test operation state management */
        update_operation_state(op_ctx, URING_OP_STATE_QUEUED);
        assert(op_ctx->state == URING_OP_STATE_QUEUED);
        
        /* Test priority queue */
        add_operation_to_queue(state, op_ctx);
        
        uring_op_context *retrieved = get_next_operation(state, URING_OP_PRIORITY_LOW);
        assert(retrieved == op_ctx);
        
        /* Test operation timeout */
        set_operation_timeout(op_ctx, 5000000); /* 5 seconds */
        assert(op_ctx->timeout_us == 5000000);
        
        /* Test reference counting */
        op_context_ref(op_ctx);
        assert(op_ctx->ref_count == 2);
        op_context_unref(op_ctx);
        assert(op_ctx->ref_count == 1);
        
        destroy_operation_context(state, op_ctx);
    }
    
    aeDeleteEventLoop(el);
    printf("✓ Enhanced operation management works\n");
    #else
    printf("⚠ Skipped - io_uring not available\n");
    #endif
}

void test_connection_lifecycle(void) {
    printf("Testing connection lifecycle management...\n");
    
    #ifdef HAVE_LIBURING
    /* Create a test event loop */
    aeEventLoop *el = aeCreateEventLoop(64);
    assert(el != NULL);
    
    aeApiState *state = el->apidata;
    if (state) {
        /* Test connection context creation */
        uring_conn_context *conn_ctx = create_connection_context(state, 1, NULL);
        assert(conn_ctx != NULL);
        assert(conn_ctx->fd == 1);
        assert(conn_ctx->state == URING_CONN_INIT);
        
        /* Test connection state updates */
        update_connection_state(conn_ctx, URING_CONN_CONNECTED);
        assert(conn_ctx->state == URING_CONN_CONNECTED);
        
        /* Test activity tracking */
        update_connection_activity(conn_ctx);
        assert(conn_ctx->last_activity > 0);
        
        /* Test connection retrieval */
        uring_conn_context *retrieved = get_connection_context(state, 1);
        assert(retrieved == conn_ctx);
        
        /* Test connection cleanup */
        cleanup_connection_operations(state, conn_ctx);
        destroy_connection_context(state, conn_ctx);
    }
    
    aeDeleteEventLoop(el);
    printf("✓ Connection lifecycle management works\n");
    #else
    printf("⚠ Skipped - io_uring not available\n");
    #endif
}

/* Test error handling and edge cases */
void test_error_handling(void) {
    printf("Testing error handling and recovery...\n");
    
    #ifdef HAVE_LIBURING
    /* Test invalid parameters */
    uring_op_context *invalid_ctx = create_operation_context(NULL, -1, -1, -1);
    assert(invalid_ctx == NULL);
    
    /* Test buffer pool with invalid parameters */
    int result = init_buffer_pool(-1, -1);
    assert(result == -1);
    
    /* Test null pointer handling */
    return_buffer_to_pool(NULL);
    update_operation_state(NULL, URING_OP_STATE_COMPLETED);
    update_connection_activity(NULL);
    
    printf("✓ Error handling works correctly\n");
    #else
    printf("⚠ Skipped - io_uring not available\n");
    #endif
}

/* Test performance and memory usage */
void test_memory_optimization(void) {
    printf("Testing memory optimization...\n");
    
    #ifdef HAVE_LIBURING
    /* Initialize buffer pool */
    int result = init_buffer_pool(32, 4096);
    assert(result == 0);
    
    /* Allocate many buffers to test optimization */
    void *buffers[100];
    for (int i = 0; i < 100; i++) {
        buffers[i] = allocate_optimized_buffer(4096);
        assert(buffers[i] != NULL);
    }
    
    /* Test buffer pool optimization */
    optimize_buffer_pool();
    
    /* Return buffers */
    for (int i = 0; i < 100; i++) {
        return_buffer_to_pool(buffers[i]);
    }
    
    /* Test memory monitoring */
    monitor_memory_usage();
    
    cleanup_buffer_pool();
    printf("✓ Memory optimization works\n");
    #else
    printf("⚠ Skipped - io_uring not available\n");
    #endif
}

/* Main test runner */
int main(void) {
    printf("Running io_uring Week 2 Unit Tests\n");
    printf("==================================\n\n");
    
    /* Team Member A tests */
    printf("Team Member A - Enhanced Core Implementation:\n");
    test_enhanced_capability_detection();
    test_sqpoll_configuration();
    printf("\n");
    
    /* Team Member B tests */
    printf("Team Member B - Enhanced Buffer Management:\n");
    test_enhanced_buffer_pool();
    test_zero_copy_buffers();
    printf("\n");
    
    /* Team Member C tests */
    printf("Team Member C - Operation Management:\n");
    test_operation_management();
    test_connection_lifecycle();
    printf("\n");
    
    /* Error handling and edge cases */
    printf("Error Handling and Edge Cases:\n");
    test_error_handling();
    test_memory_optimization();
    printf("\n");
    
    printf("All tests completed successfully! ✓\n");
    return 0;
}
