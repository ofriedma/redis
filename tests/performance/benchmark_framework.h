#ifndef __BENCHMARK_FRAMEWORK_H__
#define __BENCHMARK_FRAMEWORK_H__

/*
 * Redis io_uring Performance Benchmark Framework
 * Team Member D - Week 1 Additional Utilities
 * 
 * This framework provides utilities for benchmarking Redis performance
 * with and without io_uring to measure the impact of the implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

/* Benchmark configuration */
typedef struct benchmark_config {
    int num_clients;           /* Number of concurrent clients */
    int num_operations;        /* Operations per client */
    int key_size;             /* Size of keys in bytes */
    int value_size;           /* Size of values in bytes */
    int pipeline_size;        /* Pipeline depth */
    char *test_types;         /* Comma-separated test types (set,get,incr,etc) */
    int duration_seconds;     /* Test duration in seconds (0 = use num_operations) */
    int warmup_seconds;       /* Warmup period before measurement */
    char *redis_args;         /* Additional Redis server arguments */
} benchmark_config_t;

/* Benchmark results */
typedef struct benchmark_result {
    char test_name[64];       /* Name of the test */
    double ops_per_second;    /* Operations per second */
    double avg_latency_ms;    /* Average latency in milliseconds */
    double p50_latency_ms;    /* 50th percentile latency */
    double p95_latency_ms;    /* 95th percentile latency */
    double p99_latency_ms;    /* 99th percentile latency */
    double max_latency_ms;    /* Maximum latency */
    int total_operations;     /* Total operations completed */
    int errors;               /* Number of errors */
    double cpu_usage_percent; /* CPU usage during test */
    double memory_usage_mb;   /* Memory usage in MB */
} benchmark_result_t;

/* Benchmark suite */
typedef struct benchmark_suite {
    char name[128];           /* Suite name */
    benchmark_result_t *results; /* Array of results */
    int result_count;         /* Number of results */
    int result_capacity;      /* Capacity of results array */
} benchmark_suite_t;

/* Function declarations */

/* Configuration management */
benchmark_config_t* benchmark_create_config(void);
void benchmark_free_config(benchmark_config_t *config);
void benchmark_set_default_config(benchmark_config_t *config);
int benchmark_load_config_from_file(benchmark_config_t *config, const char *filename);

/* Suite management */
benchmark_suite_t* benchmark_create_suite(const char *name);
void benchmark_free_suite(benchmark_suite_t *suite);
void benchmark_add_result(benchmark_suite_t *suite, const benchmark_result_t *result);

/* Redis server management */
int benchmark_start_redis_server(const char *redis_binary, const char *config_file, 
                                 int port, const char *extra_args, pid_t *server_pid);
int benchmark_stop_redis_server(pid_t server_pid);
int benchmark_wait_for_redis(int port, int timeout_seconds);

/* Benchmark execution */
int benchmark_run_redis_benchmark(const benchmark_config_t *config, int port,
                                 const char *redis_benchmark_binary,
                                 benchmark_result_t *result);
int benchmark_run_custom_test(const benchmark_config_t *config, int port,
                             benchmark_result_t *result);

/* Performance comparison */
int benchmark_compare_suites(const benchmark_suite_t *suite1, 
                            const benchmark_suite_t *suite2,
                            const char *output_file);

/* Reporting */
void benchmark_print_result(const benchmark_result_t *result);
void benchmark_print_suite(const benchmark_suite_t *suite);
int benchmark_export_csv(const benchmark_suite_t *suite, const char *filename);
int benchmark_export_json(const benchmark_suite_t *suite, const char *filename);

/* System monitoring */
int benchmark_get_cpu_usage(pid_t pid, double *cpu_percent);
int benchmark_get_memory_usage(pid_t pid, double *memory_mb);
int benchmark_monitor_system(pid_t pid, int duration_seconds, 
                            double *avg_cpu, double *avg_memory);

/* Utility functions */
long long benchmark_get_time_usec(void);
void benchmark_sleep_usec(long long usec);
int benchmark_create_temp_config(const char *template_config, 
                                const char *output_config,
                                int port, const char *extra_options);

/* Test data generation */
int benchmark_generate_test_data(int num_keys, int key_size, int value_size,
                                const char *output_file);
char* benchmark_generate_random_string(int length);

/* Network utilities */
int benchmark_test_connection(const char *host, int port);
int benchmark_measure_network_latency(const char *host, int port, double *latency_ms);

/* Statistical analysis */
void benchmark_calculate_percentiles(double *latencies, int count,
                                    double *p50, double *p95, double *p99);
double benchmark_calculate_average(double *values, int count);
double benchmark_calculate_stddev(double *values, int count, double average);

/* Macros for common operations */
#define BENCHMARK_DEFAULT_PORT 6379
#define BENCHMARK_DEFAULT_CLIENTS 50
#define BENCHMARK_DEFAULT_OPERATIONS 10000
#define BENCHMARK_DEFAULT_KEY_SIZE 16
#define BENCHMARK_DEFAULT_VALUE_SIZE 256
#define BENCHMARK_DEFAULT_PIPELINE 1
#define BENCHMARK_DEFAULT_WARMUP 5
#define BENCHMARK_MAX_LATENCY_SAMPLES 100000

/* Error codes */
#define BENCHMARK_SUCCESS 0
#define BENCHMARK_ERROR_CONFIG -1
#define BENCHMARK_ERROR_SERVER -2
#define BENCHMARK_ERROR_CLIENT -3
#define BENCHMARK_ERROR_TIMEOUT -4
#define BENCHMARK_ERROR_MEMORY -5

/* Benchmark test types */
#define BENCHMARK_TEST_SET "set"
#define BENCHMARK_TEST_GET "get"
#define BENCHMARK_TEST_INCR "incr"
#define BENCHMARK_TEST_LPUSH "lpush"
#define BENCHMARK_TEST_LPOP "lpop"
#define BENCHMARK_TEST_SADD "sadd"
#define BENCHMARK_TEST_SPOP "spop"
#define BENCHMARK_TEST_ZADD "zadd"
#define BENCHMARK_TEST_ZPOP "zpop"
#define BENCHMARK_TEST_PING "ping"

/* Configuration file format example:
 * 
 * [benchmark]
 * clients = 50
 * operations = 10000
 * key_size = 16
 * value_size = 256
 * pipeline = 1
 * test_types = set,get,incr
 * duration = 0
 * warmup = 5
 * redis_args = --save "" --appendonly no
 */

#endif /* __BENCHMARK_FRAMEWORK_H__ */
