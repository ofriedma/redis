/*
 * Redis io_uring Performance Benchmark Runner
 * Team Member D - Week 1 Additional Utilities
 *
 * This program runs performance benchmarks comparing Redis with
 * and without io_uring support.
 */

#define _GNU_SOURCE
#include "benchmark_framework.h"
#include <getopt.h>

static void print_usage(const char *program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help              Show this help message\n");
    printf("  -r, --redis-server PATH Path to redis-server binary\n");
    printf("  -b, --redis-benchmark PATH Path to redis-benchmark binary\n");
    printf("  -c, --clients NUM       Number of concurrent clients (default: 50)\n");
    printf("  -n, --operations NUM    Number of operations per client (default: 10000)\n");
    printf("  -t, --tests TYPES       Comma-separated test types (default: set,get)\n");
    printf("  -o, --output FILE       Output file for results (CSV format)\n");
    printf("  --epoll-only            Only test epoll (skip io_uring)\n");
    printf("  --uring-only            Only test io_uring (skip epoll)\n");
    printf("  --port-base NUM         Base port number (default: 6379)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s -c 100 -n 50000 -t set,get,incr\n", program_name);
    printf("  %s --redis-server ./src/redis-server --output results.csv\n", program_name);
    printf("\n");
}

static int run_benchmark_suite(const char *suite_name, const char *redis_server,
                              const char *redis_benchmark, const char *extra_args,
                              int port, benchmark_config_t *config,
                              benchmark_suite_t *suite) {
    printf("Running %s benchmark suite...\n", suite_name);
    
    pid_t server_pid;
    int ret = benchmark_start_redis_server(redis_server, NULL, port, extra_args, &server_pid);
    if (ret != BENCHMARK_SUCCESS) {
        printf("Failed to start Redis server for %s\n", suite_name);
        return ret;
    }
    
    printf("Redis server started (PID: %d, Port: %d)\n", server_pid, port);
    
    /* Parse test types and run each one */
    char *test_types = strdup(config->test_types);
    char *test = strtok(test_types, ",");
    
    while (test) {
        printf("  Running %s test...\n", test);
        
        benchmark_result_t result = {0};
        
        /* Update config for this specific test */
        free(config->test_types);
        config->test_types = strdup(test);
        
        ret = benchmark_run_redis_benchmark(config, port, redis_benchmark, &result);
        if (ret == BENCHMARK_SUCCESS) {
            snprintf(result.test_name, sizeof(result.test_name), "%s_%s", suite_name, test);
            benchmark_add_result(suite, &result);
            printf("    %.2f ops/sec\n", result.ops_per_second);
        } else {
            printf("    Failed to run benchmark\n");
        }
        
        test = strtok(NULL, ",");
    }
    
    free(test_types);
    
    /* Stop Redis server */
    benchmark_stop_redis_server(server_pid);
    printf("Redis server stopped\n\n");
    
    return BENCHMARK_SUCCESS;
}

int main(int argc, char *argv[]) {
    /* Default configuration */
    const char *redis_server = "src/redis-server";
    const char *redis_benchmark = "src/redis-benchmark";
    const char *output_file = NULL;
    int port_base = 6379;
    int epoll_only = 0;
    int uring_only = 0;

    benchmark_config_t *config = benchmark_create_config();
    if (!config) {
        fprintf(stderr, "Failed to create benchmark configuration\n");
        return 1;
    }

    /* Parse command line options */
    struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"redis-server", required_argument, 0, 'r'},
        {"redis-benchmark", required_argument, 0, 'b'},
        {"clients", required_argument, 0, 'c'},
        {"operations", required_argument, 0, 'n'},
        {"tests", required_argument, 0, 't'},
        {"output", required_argument, 0, 'o'},
        {"epoll-only", no_argument, 0, 'E'},
        {"uring-only", no_argument, 0, 'U'},
        {"port-base", required_argument, 0, 'p'},
        {0, 0, 0, 0}
    };
    
    int c;
    while ((c = getopt_long(argc, argv, "hr:b:c:n:t:o:p:EU", long_options, NULL)) != -1) {
        switch (c) {
            case 'h':
                print_usage(argv[0]);
                benchmark_free_config(config);
                return 0;
            case 'r':
                redis_server = optarg;
                break;
            case 'b':
                redis_benchmark = optarg;
                break;
            case 'c':
                config->num_clients = atoi(optarg);
                break;
            case 'n':
                config->num_operations = atoi(optarg);
                break;
            case 't':
                free(config->test_types);
                config->test_types = strdup(optarg);
                break;
            case 'o':
                output_file = optarg;
                break;
            case 'p':
                port_base = atoi(optarg);
                break;
            case 'E':
                epoll_only = 1;
                break;
            case 'U':
                uring_only = 1;
                break;
            case '?':
                print_usage(argv[0]);
                benchmark_free_config(config);
                return 1;
        }
    }
    
    printf("=== Redis io_uring Performance Benchmark ===\n");
    printf("Configuration:\n");
    printf("  Clients: %d\n", config->num_clients);
    printf("  Operations: %d\n", config->num_operations);
    printf("  Test types: %s\n", config->test_types);
    printf("  Redis server: %s\n", redis_server);
    printf("  Redis benchmark: %s\n", redis_benchmark);
    printf("\n");
    
    /* Create benchmark suite */
    benchmark_suite_t *suite = benchmark_create_suite("Redis Performance Comparison");
    if (!suite) {
        fprintf(stderr, "Failed to create benchmark suite\n");
        benchmark_free_config(config);
        return 1;
    }
    
    int ret = 0;
    
    /* Run epoll benchmark */
    if (!uring_only) {
        ret = run_benchmark_suite("epoll", redis_server, redis_benchmark,
                                 config->redis_args, port_base, config, suite);
        if (ret != BENCHMARK_SUCCESS) {
            printf("Epoll benchmark failed\n");
        }
    }
    
    /* Run io_uring benchmark */
    if (!epoll_only) {
        char uring_args[512];
        snprintf(uring_args, sizeof(uring_args), "%s --uring-enabled yes", 
                config->redis_args ? config->redis_args : "");
        
        ret = run_benchmark_suite("uring", redis_server, redis_benchmark,
                                 uring_args, port_base + 1, config, suite);
        if (ret != BENCHMARK_SUCCESS) {
            printf("io_uring benchmark failed (may be expected if not implemented)\n");
        }
    }
    
    /* Print results */
    printf("=== Benchmark Results ===\n");
    benchmark_print_suite(suite);
    
    /* Export results if requested */
    if (output_file) {
        ret = benchmark_export_csv(suite, output_file);
        if (ret == BENCHMARK_SUCCESS) {
            printf("Results exported to %s\n", output_file);
        } else {
            printf("Failed to export results to %s\n", output_file);
        }
    }
    
    /* Cleanup */
    benchmark_free_suite(suite);
    benchmark_free_config(config);
    
    return 0;
}
