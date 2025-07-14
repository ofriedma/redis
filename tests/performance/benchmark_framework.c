/*
 * Redis io_uring Performance Benchmark Framework Implementation
 * Team Member D - Week 1 Additional Utilities
 */

#define _GNU_SOURCE
#include "benchmark_framework.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>

/* Configuration management */
benchmark_config_t* benchmark_create_config(void) {
    benchmark_config_t *config = malloc(sizeof(benchmark_config_t));
    if (!config) return NULL;
    
    benchmark_set_default_config(config);
    return config;
}

void benchmark_free_config(benchmark_config_t *config) {
    if (!config) return;
    if (config->test_types) free(config->test_types);
    if (config->redis_args) free(config->redis_args);
    free(config);
}

void benchmark_set_default_config(benchmark_config_t *config) {
    if (!config) return;
    
    config->num_clients = BENCHMARK_DEFAULT_CLIENTS;
    config->num_operations = BENCHMARK_DEFAULT_OPERATIONS;
    config->key_size = BENCHMARK_DEFAULT_KEY_SIZE;
    config->value_size = BENCHMARK_DEFAULT_VALUE_SIZE;
    config->pipeline_size = BENCHMARK_DEFAULT_PIPELINE;
    config->test_types = strdup("set,get");
    config->duration_seconds = 0;
    config->warmup_seconds = BENCHMARK_DEFAULT_WARMUP;
    config->redis_args = strdup("--save \"\" --appendonly no");
}

/* Suite management */
benchmark_suite_t* benchmark_create_suite(const char *name) {
    benchmark_suite_t *suite = malloc(sizeof(benchmark_suite_t));
    if (!suite) return NULL;
    
    strncpy(suite->name, name, sizeof(suite->name) - 1);
    suite->name[sizeof(suite->name) - 1] = '\0';
    
    suite->result_capacity = 16;
    suite->results = malloc(sizeof(benchmark_result_t) * suite->result_capacity);
    if (!suite->results) {
        free(suite);
        return NULL;
    }
    
    suite->result_count = 0;
    return suite;
}

void benchmark_free_suite(benchmark_suite_t *suite) {
    if (!suite) return;
    if (suite->results) free(suite->results);
    free(suite);
}

void benchmark_add_result(benchmark_suite_t *suite, const benchmark_result_t *result) {
    if (!suite || !result) return;
    
    if (suite->result_count >= suite->result_capacity) {
        suite->result_capacity *= 2;
        suite->results = realloc(suite->results, 
                               sizeof(benchmark_result_t) * suite->result_capacity);
        if (!suite->results) return;
    }
    
    suite->results[suite->result_count++] = *result;
}

/* Redis server management */
int benchmark_start_redis_server(const char *redis_binary, const char *config_file,
                                int port, const char *extra_args, pid_t *server_pid) {
    pid_t pid = fork();
    if (pid == 0) {
        /* Child process - start Redis server */
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);
        
        if (config_file) {
            execl(redis_binary, redis_binary, config_file, 
                  "--port", port_str, extra_args ? extra_args : "", NULL);
        } else {
            execl(redis_binary, redis_binary, 
                  "--port", port_str, extra_args ? extra_args : "", NULL);
        }
        exit(1); /* execl failed */
    } else if (pid > 0) {
        /* Parent process */
        *server_pid = pid;
        return benchmark_wait_for_redis(port, 10); /* Wait up to 10 seconds */
    } else {
        return BENCHMARK_ERROR_SERVER;
    }
}

int benchmark_stop_redis_server(pid_t server_pid) {
    if (server_pid <= 0) return BENCHMARK_ERROR_SERVER;
    
    /* Send SIGTERM first */
    if (kill(server_pid, SIGTERM) == 0) {
        /* Wait for graceful shutdown */
        int status;
        for (int i = 0; i < 50; i++) { /* Wait up to 5 seconds */
            if (waitpid(server_pid, &status, WNOHANG) == server_pid) {
                return BENCHMARK_SUCCESS;
            }
            usleep(100000); /* 100ms */
        }
    }
    
    /* Force kill if graceful shutdown failed */
    kill(server_pid, SIGKILL);
    waitpid(server_pid, NULL, 0);
    return BENCHMARK_SUCCESS;
}

int benchmark_wait_for_redis(int port, int timeout_seconds) {
    for (int i = 0; i < timeout_seconds * 10; i++) {
        if (benchmark_test_connection("127.0.0.1", port) == 0) {
            return BENCHMARK_SUCCESS;
        }
        usleep(100000); /* 100ms */
    }
    return BENCHMARK_ERROR_TIMEOUT;
}

/* Network utilities */
int benchmark_test_connection(const char *host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    
    int result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    close(sock);
    
    return result;
}

/* Benchmark execution */
int benchmark_run_redis_benchmark(const benchmark_config_t *config, int port,
                                 const char *redis_benchmark_binary,
                                 benchmark_result_t *result) {
    if (!config || !redis_benchmark_binary || !result) {
        return BENCHMARK_ERROR_CONFIG;
    }
    
    /* Create command line for redis-benchmark */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "%s -h 127.0.0.1 -p %d -c %d -n %d -t %s --csv",
             redis_benchmark_binary, port, config->num_clients,
             config->num_operations, config->test_types);
    
    /* Execute redis-benchmark and capture output */
    FILE *fp = popen(cmd, "r");
    if (!fp) return BENCHMARK_ERROR_CLIENT;
    
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* Parse CSV output from redis-benchmark */
        if (strstr(line, "SET") || strstr(line, "GET")) {
            /* Simple parsing - in real implementation, parse CSV properly */
            char *token = strtok(line, ",");
            if (token) {
                strncpy(result->test_name, token, sizeof(result->test_name) - 1);
                token = strtok(NULL, ",");
                if (token) {
                    result->ops_per_second = atof(token);
                }
            }
        }
    }
    
    int exit_code = pclose(fp);
    return (exit_code == 0) ? BENCHMARK_SUCCESS : BENCHMARK_ERROR_CLIENT;
}

/* Utility functions */
long long benchmark_get_time_usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

void benchmark_sleep_usec(long long usec) {
    usleep(usec);
}

char* benchmark_generate_random_string(int length) {
    char *str = malloc(length + 1);
    if (!str) return NULL;
    
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < length; i++) {
        str[i] = charset[rand() % (sizeof(charset) - 1)];
    }
    str[length] = '\0';
    
    return str;
}

/* Reporting */
void benchmark_print_result(const benchmark_result_t *result) {
    if (!result) return;
    
    printf("Test: %s\n", result->test_name);
    printf("  Operations/sec: %.2f\n", result->ops_per_second);
    printf("  Avg Latency: %.3f ms\n", result->avg_latency_ms);
    printf("  P95 Latency: %.3f ms\n", result->p95_latency_ms);
    printf("  P99 Latency: %.3f ms\n", result->p99_latency_ms);
    printf("  Max Latency: %.3f ms\n", result->max_latency_ms);
    printf("  Total Ops: %d\n", result->total_operations);
    printf("  Errors: %d\n", result->errors);
    printf("  CPU Usage: %.1f%%\n", result->cpu_usage_percent);
    printf("  Memory Usage: %.1f MB\n", result->memory_usage_mb);
    printf("\n");
}

void benchmark_print_suite(const benchmark_suite_t *suite) {
    if (!suite) return;
    
    printf("=== Benchmark Suite: %s ===\n", suite->name);
    for (int i = 0; i < suite->result_count; i++) {
        benchmark_print_result(&suite->results[i]);
    }
}

int benchmark_export_csv(const benchmark_suite_t *suite, const char *filename) {
    if (!suite || !filename) return BENCHMARK_ERROR_CONFIG;
    
    FILE *fp = fopen(filename, "w");
    if (!fp) return BENCHMARK_ERROR_CONFIG;
    
    /* Write CSV header */
    fprintf(fp, "test_name,ops_per_second,avg_latency_ms,p95_latency_ms,p99_latency_ms,max_latency_ms,total_operations,errors,cpu_usage_percent,memory_usage_mb\n");
    
    /* Write data */
    for (int i = 0; i < suite->result_count; i++) {
        const benchmark_result_t *r = &suite->results[i];
        fprintf(fp, "%s,%.2f,%.3f,%.3f,%.3f,%.3f,%d,%d,%.1f,%.1f\n",
                r->test_name, r->ops_per_second, r->avg_latency_ms,
                r->p95_latency_ms, r->p99_latency_ms, r->max_latency_ms,
                r->total_operations, r->errors, r->cpu_usage_percent,
                r->memory_usage_mb);
    }
    
    fclose(fp);
    return BENCHMARK_SUCCESS;
}

/* Statistical analysis */
void benchmark_calculate_percentiles(double *latencies, int count,
                                    double *p50, double *p95, double *p99) {
    if (!latencies || count <= 0) return;
    
    /* Simple percentile calculation - sort and pick values */
    /* In real implementation, use proper sorting algorithm */
    *p50 = latencies[count * 50 / 100];
    *p95 = latencies[count * 95 / 100];
    *p99 = latencies[count * 99 / 100];
}

double benchmark_calculate_average(double *values, int count) {
    if (!values || count <= 0) return 0.0;
    
    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        sum += values[i];
    }
    return sum / count;
}
