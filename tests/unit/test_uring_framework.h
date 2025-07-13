#ifndef __TEST_URING_FRAMEWORK_H__
#define __TEST_URING_FRAMEWORK_H__

/*
 * Redis io_uring Test Framework
 * Team Member D - Week 1 Task 1.D.2
 * 
 * This header provides a simple test framework for io_uring unit tests.
 * It includes assertion macros, test utilities, and common setup/teardown functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/* Test framework macros */
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "ASSERTION FAILED: %s:%d: %s\n", __FILE__, __LINE__, message); \
            test_framework_stats.failed++; \
            return -1; \
        } else { \
            test_framework_stats.passed++; \
        } \
    } while(0)

#define TEST_ASSERT_EQ(expected, actual, message) \
    do { \
        if ((expected) != (actual)) { \
            fprintf(stderr, "ASSERTION FAILED: %s:%d: %s (expected: %ld, actual: %ld)\n", \
                    __FILE__, __LINE__, message, (long)(expected), (long)(actual)); \
            test_framework_stats.failed++; \
            return -1; \
        } else { \
            test_framework_stats.passed++; \
        } \
    } while(0)

#define TEST_ASSERT_NE(not_expected, actual, message) \
    do { \
        if ((not_expected) == (actual)) { \
            fprintf(stderr, "ASSERTION FAILED: %s:%d: %s (should not equal: %ld)\n", \
                    __FILE__, __LINE__, message, (long)(not_expected)); \
            test_framework_stats.failed++; \
            return -1; \
        } else { \
            test_framework_stats.passed++; \
        } \
    } while(0)

#define TEST_ASSERT_NULL(ptr, message) \
    TEST_ASSERT((ptr) == NULL, message)

#define TEST_ASSERT_NOT_NULL(ptr, message) \
    TEST_ASSERT((ptr) != NULL, message)

#define TEST_ASSERT_STR_EQ(expected, actual, message) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            fprintf(stderr, "ASSERTION FAILED: %s:%d: %s (expected: '%s', actual: '%s')\n", \
                    __FILE__, __LINE__, message, (expected), (actual)); \
            test_framework_stats.failed++; \
            return -1; \
        } else { \
            test_framework_stats.passed++; \
        } \
    } while(0)

/* Test function type */
typedef int (*test_function_t)(void);

/* Test case structure */
typedef struct test_case {
    const char *name;
    test_function_t function;
    int skip;  /* Set to 1 to skip this test */
} test_case_t;

/* Test statistics */
typedef struct test_stats {
    int passed;
    int failed;
    int skipped;
    int total;
} test_stats_t;

/* Global test statistics */
extern test_stats_t test_framework_stats;

/* Test framework functions */
void test_framework_init(void);
void test_framework_cleanup(void);
int run_test_case(test_case_t *test);
int run_test_suite(test_case_t tests[], int count, const char *suite_name);
void print_test_summary(const char *suite_name);

/* Test utilities */
void test_log(const char *format, ...);
void test_error(const char *format, ...);
void test_skip(const char *reason);

/* Memory testing utilities */
void* test_malloc(size_t size);
void test_free(void *ptr);
void test_check_memory_leaks(void);

/* File/directory utilities for testing */
int test_create_temp_file(char *template);
int test_create_temp_dir(char *template);
void test_cleanup_temp_files(void);

/* Network testing utilities */
int test_create_socket_pair(int fds[2]);
int test_bind_random_port(int *port);

/* Time utilities */
long long test_get_time_usec(void);
void test_sleep_usec(long long usec);

/* Conditional compilation for io_uring tests */
#ifdef HAVE_LIBURING
#include <liburing.h>

/* io_uring specific test utilities */
int test_uring_supported(void);
int test_uring_sqpoll_supported(void);
int test_create_uring_ring(struct io_uring *ring, int entries);
void test_cleanup_uring_ring(struct io_uring *ring);

/* Test operation helpers */
typedef struct test_uring_op {
    int fd;
    int op_type;
    void *buffer;
    size_t buffer_size;
    int completed;
    int result;
} test_uring_op_t;

test_uring_op_t* test_create_uring_op(int fd, int op_type, size_t buffer_size);
void test_free_uring_op(test_uring_op_t *op);

#endif /* HAVE_LIBURING */

/* Test macros for conditional execution */
#define RUN_TEST_IF_URING_SUPPORTED(test_func) \
    do { \
        if (test_uring_supported()) { \
            if (test_func() != 0) return -1; \
        } else { \
            test_skip("io_uring not supported"); \
        } \
    } while(0)

#define RUN_TEST_IF_SQPOLL_SUPPORTED(test_func) \
    do { \
        if (test_uring_sqpoll_supported()) { \
            if (test_func() != 0) return -1; \
        } else { \
            test_skip("SQPOLL not supported"); \
        } \
    } while(0)

/* Test suite registration macro */
#define REGISTER_TEST_SUITE(suite_name, tests) \
    int main(int argc, char *argv[]) { \
        test_framework_init(); \
        int result = run_test_suite(tests, sizeof(tests)/sizeof(tests[0]), suite_name); \
        print_test_summary(suite_name); \
        test_framework_cleanup(); \
        return result; \
    }

/* Individual test registration macro */
#define REGISTER_TEST(test_name, test_func) \
    { .name = test_name, .function = test_func, .skip = 0 }

#define REGISTER_SKIP_TEST(test_name, test_func) \
    { .name = test_name, .function = test_func, .skip = 1 }

#endif /* __TEST_URING_FRAMEWORK_H__ */
