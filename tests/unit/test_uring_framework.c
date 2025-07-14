/*
 * Redis io_uring Test Framework Implementation
 * Team Member D - Week 1 Task 1.D.2
 */

#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#include "test_uring_framework.h"
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Global test statistics */
test_stats_t test_framework_stats = {0, 0, 0, 0};

/* Memory tracking for leak detection */
static void **allocated_ptrs = NULL;
static int allocated_count = 0;
static int allocated_capacity = 0;

/* Temporary file tracking */
static char **temp_files = NULL;
static int temp_file_count = 0;
static int temp_file_capacity = 0;

/* Test framework initialization */
void test_framework_init(void) {
    memset(&test_framework_stats, 0, sizeof(test_framework_stats));
    allocated_ptrs = NULL;
    allocated_count = 0;
    allocated_capacity = 0;
    temp_files = NULL;
    temp_file_count = 0;
    temp_file_capacity = 0;
    
    printf("Test framework initialized\n");
}

/* Test framework cleanup */
void test_framework_cleanup(void) {
    test_check_memory_leaks();
    test_cleanup_temp_files();
    
    if (allocated_ptrs) {
        free(allocated_ptrs);
        allocated_ptrs = NULL;
    }
    
    if (temp_files) {
        for (int i = 0; i < temp_file_count; i++) {
            if (temp_files[i]) {
                free(temp_files[i]);
            }
        }
        free(temp_files);
        temp_files = NULL;
    }
    
    printf("Test framework cleaned up\n");
}

/* Run a single test case */
int run_test_case(test_case_t *test) {
    if (test->skip) {
        printf("SKIP: %s\n", test->name);
        test_framework_stats.skipped++;
        return 0;
    }
    
    printf("RUN:  %s ... ", test->name);
    fflush(stdout);

    int result = test->function();
    
    if (result == 0) {
        printf("PASS\n");
        return 0;
    } else {
        printf("FAIL\n");
        return -1;
    }
}

/* Run a test suite */
int run_test_suite(test_case_t tests[], int count, const char *suite_name) {
    printf("\n=== Running Test Suite: %s ===\n", suite_name);
    
    test_framework_stats.total = count;
    int suite_failures = 0;
    
    for (int i = 0; i < count; i++) {
        if (run_test_case(&tests[i]) != 0) {
            suite_failures++;
        }
    }
    
    return suite_failures;
}

/* Print test summary */
void print_test_summary(const char *suite_name) {
    printf("\n=== Test Suite Summary: %s ===\n", suite_name);
    printf("Total tests: %d\n", test_framework_stats.total);
    printf("Passed: %d\n", test_framework_stats.passed);
    printf("Failed: %d\n", test_framework_stats.failed);
    printf("Skipped: %d\n", test_framework_stats.skipped);
    
    if (test_framework_stats.failed == 0) {
        printf("Result: ALL TESTS PASSED\n");
    } else {
        printf("Result: %d TESTS FAILED\n", test_framework_stats.failed);
    }
}

/* Logging utilities */
void test_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    printf("[LOG] ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

void test_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[ERROR] ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void test_skip(const char *reason) {
    printf("SKIP: %s\n", reason);
    test_framework_stats.skipped++;
}

/* Memory utilities */
void* test_malloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr) {
        /* Track allocation for leak detection */
        if (allocated_count >= allocated_capacity) {
            allocated_capacity = allocated_capacity ? allocated_capacity * 2 : 16;
            allocated_ptrs = realloc(allocated_ptrs, allocated_capacity * sizeof(void*));
        }
        allocated_ptrs[allocated_count++] = ptr;
    }
    return ptr;
}

void test_free(void *ptr) {
    if (!ptr) return;
    
    /* Remove from tracking */
    for (int i = 0; i < allocated_count; i++) {
        if (allocated_ptrs[i] == ptr) {
            allocated_ptrs[i] = allocated_ptrs[--allocated_count];
            break;
        }
    }
    free(ptr);
}

void test_check_memory_leaks(void) {
    if (allocated_count > 0) {
        test_error("Memory leak detected: %d unfreed allocations", allocated_count);
        for (int i = 0; i < allocated_count; i++) {
            test_error("Leaked pointer: %p", allocated_ptrs[i]);
        }
    }
}

/* File utilities */
int test_create_temp_file(char *template) {
    int fd = mkstemp(template);
    if (fd >= 0) {
        /* Track temp file for cleanup */
        if (temp_file_count >= temp_file_capacity) {
            temp_file_capacity = temp_file_capacity ? temp_file_capacity * 2 : 16;
            temp_files = realloc(temp_files, temp_file_capacity * sizeof(char*));
        }
        temp_files[temp_file_count++] = strdup(template);
    }
    return fd;
}

int test_create_temp_dir(char *template) {
    char *result = mkdtemp(template);
    if (result) {
        /* Track temp dir for cleanup */
        if (temp_file_count >= temp_file_capacity) {
            temp_file_capacity = temp_file_capacity ? temp_file_capacity * 2 : 16;
            temp_files = realloc(temp_files, temp_file_capacity * sizeof(char*));
        }
        temp_files[temp_file_count++] = strdup(template);
        return 0;
    }
    return -1;
}

void test_cleanup_temp_files(void) {
    for (int i = 0; i < temp_file_count; i++) {
        if (temp_files[i]) {
            unlink(temp_files[i]);  /* Try to remove as file */
            rmdir(temp_files[i]);   /* Try to remove as directory */
        }
    }
}

/* Network utilities */
int test_create_socket_pair(int fds[2]) {
    return socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
}

int test_bind_random_port(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;  /* Let system choose port */
    
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &len) < 0) {
        close(fd);
        return -1;
    }
    
    *port = ntohs(addr.sin_port);
    return fd;
}

/* Time utilities */
long long test_get_time_usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

void test_sleep_usec(long long usec) {
    usleep(usec);
}

#ifdef HAVE_LIBURING
/* io_uring specific utilities */
int test_uring_supported(void) {
    struct io_uring ring;
    int ret = io_uring_queue_init(8, &ring, 0);
    if (ret < 0) {
        return 0;
    }
    io_uring_queue_exit(&ring);
    return 1;
}

int test_uring_sqpoll_supported(void) {
    struct io_uring ring;
    struct io_uring_params params = {0};
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 1000;
    
    int ret = io_uring_queue_init_params(8, &ring, &params);
    if (ret < 0) {
        return 0;
    }
    io_uring_queue_exit(&ring);
    return 1;
}

int test_create_uring_ring(struct io_uring *ring, int entries) {
    return io_uring_queue_init(entries, ring, 0);
}

void test_cleanup_uring_ring(struct io_uring *ring) {
    io_uring_queue_exit(ring);
}

test_uring_op_t* test_create_uring_op(int fd, int op_type, size_t buffer_size) {
    test_uring_op_t *op = test_malloc(sizeof(test_uring_op_t));
    if (!op) return NULL;
    
    op->fd = fd;
    op->op_type = op_type;
    op->buffer_size = buffer_size;
    op->completed = 0;
    op->result = 0;
    
    if (buffer_size > 0) {
        op->buffer = test_malloc(buffer_size);
        if (!op->buffer) {
            test_free(op);
            return NULL;
        }
    } else {
        op->buffer = NULL;
    }
    
    return op;
}

void test_free_uring_op(test_uring_op_t *op) {
    if (!op) return;
    if (op->buffer) test_free(op->buffer);
    test_free(op);
}

#endif /* HAVE_LIBURING */
