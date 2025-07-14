/*
 * Basic io_uring Tests
 * Team Member D - Week 1 Task 1.D.2
 *
 * This file contains basic tests for io_uring functionality
 * to validate the test framework and environment setup.
 */

#define _GNU_SOURCE
#include "test_uring_framework.h"
#include <sys/socket.h>

/* Test basic framework functionality */
int test_framework_basic(void) {
    TEST_ASSERT(1 == 1, "Basic assertion should pass");
    TEST_ASSERT_EQ(42, 42, "Equality assertion should pass");
    TEST_ASSERT_NE(1, 2, "Inequality assertion should pass");
    TEST_ASSERT_NOT_NULL("test", "String should not be null");
    TEST_ASSERT_STR_EQ("hello", "hello", "String equality should pass");
    
    return 0;
}

/* Test memory utilities */
int test_memory_utilities(void) {
    void *ptr1 = test_malloc(100);
    TEST_ASSERT_NOT_NULL(ptr1, "Memory allocation should succeed");
    
    void *ptr2 = test_malloc(200);
    TEST_ASSERT_NOT_NULL(ptr2, "Second memory allocation should succeed");
    TEST_ASSERT_NE((long)ptr1, (long)ptr2, "Allocations should be different");
    
    test_free(ptr1);
    test_free(ptr2);
    
    return 0;
}

/* Test file utilities */
int test_file_utilities(void) {
    char template[] = "/tmp/redis_test_XXXXXX";
    int fd = test_create_temp_file(template);
    TEST_ASSERT(fd >= 0, "Temp file creation should succeed");
    
    /* Write some data */
    const char *data = "test data";
    ssize_t written = write(fd, data, strlen(data));
    TEST_ASSERT_EQ((ssize_t)strlen(data), written, "Write should succeed");
    
    close(fd);
    return 0;
}

/* Test network utilities */
int test_network_utilities(void) {
    int fds[2];
    int ret = test_create_socket_pair(fds);
    TEST_ASSERT_EQ(0, ret, "Socket pair creation should succeed");
    
    /* Test communication */
    const char *msg = "hello";
    ssize_t sent = send(fds[0], msg, strlen(msg), 0);
    TEST_ASSERT_EQ((ssize_t)strlen(msg), sent, "Send should succeed");

    char buffer[100];
    ssize_t received = recv(fds[1], buffer, sizeof(buffer), 0);
    TEST_ASSERT_EQ((ssize_t)strlen(msg), received, "Receive should succeed");
    
    buffer[received] = '\0';
    TEST_ASSERT_STR_EQ(msg, buffer, "Received message should match sent");
    
    close(fds[0]);
    close(fds[1]);
    
    return 0;
}

/* Test time utilities */
int test_time_utilities(void) {
    long long start = test_get_time_usec();
    test_sleep_usec(1000);  /* Sleep 1ms */
    long long end = test_get_time_usec();
    
    long long elapsed = end - start;
    TEST_ASSERT(elapsed >= 1000, "Sleep should take at least 1ms");
    TEST_ASSERT(elapsed < 10000, "Sleep should not take more than 10ms");
    
    return 0;
}

#ifdef HAVE_LIBURING
/* Test io_uring support detection */
int test_uring_support_detection(void) {
    int supported = test_uring_supported();
    test_log("io_uring supported: %s", supported ? "yes" : "no");
    
    if (supported) {
        int sqpoll_supported = test_uring_sqpoll_supported();
        test_log("SQPOLL supported: %s", sqpoll_supported ? "yes" : "no");
    }
    
    return 0;
}

/* Test basic io_uring ring creation */
int test_uring_ring_creation(void) {
    struct io_uring ring;
    int ret = test_create_uring_ring(&ring, 8);
    TEST_ASSERT_EQ(0, ret, "io_uring ring creation should succeed");
    
    test_cleanup_uring_ring(&ring);
    return 0;
}

/* Test io_uring operation utilities */
int test_uring_operation_utilities(void) {
    test_uring_op_t *op = test_create_uring_op(1, 1, 1024);
    TEST_ASSERT_NOT_NULL(op, "Operation creation should succeed");
    TEST_ASSERT_EQ(1, op->fd, "FD should be set correctly");
    TEST_ASSERT_EQ(1, op->op_type, "Op type should be set correctly");
    TEST_ASSERT_EQ(1024, op->buffer_size, "Buffer size should be set correctly");
    TEST_ASSERT_NOT_NULL(op->buffer, "Buffer should be allocated");
    
    test_free_uring_op(op);
    return 0;
}

/* Test SQPOLL functionality if supported */
int test_uring_sqpoll(void) {
    struct io_uring ring;
    struct io_uring_params params = {0};
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 1000;
    
    int ret = io_uring_queue_init_params(8, &ring, &params);
    if (ret < 0) {
        test_skip("SQPOLL not supported");
        return 0;
    }
    
    TEST_ASSERT_EQ(0, ret, "SQPOLL ring creation should succeed");
    
    /* Verify SQPOLL is actually enabled */
    TEST_ASSERT(params.features & IORING_FEAT_SQPOLL_NONFIXED, "SQPOLL should be enabled");
    
    test_cleanup_uring_ring(&ring);
    return 0;
}

#endif /* HAVE_LIBURING */

/* Test suite definition */
test_case_t basic_tests[] = {
    REGISTER_TEST("framework_basic", test_framework_basic),
    REGISTER_TEST("memory_utilities", test_memory_utilities),
    REGISTER_TEST("file_utilities", test_file_utilities),
    REGISTER_TEST("network_utilities", test_network_utilities),
    REGISTER_TEST("time_utilities", test_time_utilities),
    
#ifdef HAVE_LIBURING
    REGISTER_TEST("uring_support_detection", test_uring_support_detection),
    REGISTER_TEST("uring_ring_creation", test_uring_ring_creation),
    REGISTER_TEST("uring_operation_utilities", test_uring_operation_utilities),
    REGISTER_TEST("uring_sqpoll", test_uring_sqpoll),
#endif
};

/* Register the test suite */
REGISTER_TEST_SUITE("Basic io_uring Tests", basic_tests);
