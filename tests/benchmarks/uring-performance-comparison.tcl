# Week 4 io_uring Performance Benchmarking
# Comprehensive performance comparison between io_uring and epoll

proc benchmark_operation {server_config operation_proc iterations description} {
    puts "Benchmarking: $description"
    
    start_server $server_config {
        set start_time [clock microseconds]
        
        # Run the benchmark operation
        eval $operation_proc $iterations
        
        set end_time [clock microseconds]
        set duration [expr $end_time - $start_time]
        set ops_per_sec [expr $iterations * 1000000.0 / $duration]
        
        puts "  Duration: [expr $duration / 1000.0] ms"
        puts "  Operations: $iterations"
        puts "  Ops/sec: [format %.2f $ops_per_sec]"
        puts "  Avg latency: [format %.3f [expr $duration / double($iterations)]] μs"
        
        # Get memory usage
        set memory_info [r info memory]
        regexp {used_memory:(\d+)} $memory_info - used_memory
        puts "  Memory used: [expr $used_memory / 1024] KB"
        
        # Get io_uring specific stats if available
        if {![catch {r info uring} uring_info]} {
            if {[regexp {ops_completed:(\d+)} $uring_info - ops_completed]} {
                puts "  io_uring ops completed: $ops_completed"
            }
            if {[regexp {avg_completions_per_batch:(\d+)} $uring_info - avg_batch]} {
                puts "  Avg completions per batch: $avg_batch"
            }
        }
        
        puts ""
        return [list $duration $ops_per_sec $used_memory]
    }
}

# Basic SET operation benchmark
proc benchmark_set_operations {iterations} {
    for {set i 0} {$i < $iterations} {incr i} {
        r set "bench_key_$i" "bench_value_$i"
    }
}

# Basic GET operation benchmark
proc benchmark_get_operations {iterations} {
    # Pre-populate data
    for {set i 0} {$i < $iterations} {incr i} {
        r set "bench_key_$i" "bench_value_$i"
    }
    
    # Benchmark GET operations
    for {set i 0} {$i < $iterations} {incr i} {
        r get "bench_key_$i"
    }
}

# Mixed read/write benchmark
proc benchmark_mixed_operations {iterations} {
    # Pre-populate some data
    for {set i 0} {$i < [expr $iterations / 2]} {incr i} {
        r set "mixed_key_$i" "mixed_value_$i"
    }
    
    # 70% reads, 30% writes
    for {set i 0} {$i < $iterations} {incr i} {
        if {[expr rand()] < 0.7} {
            # Read operation
            set key_id [expr int(rand() * ($iterations / 2))]
            r get "mixed_key_$key_id"
        } else {
            # Write operation
            r set "mixed_key_$i" "mixed_value_$i"
        }
    }
}

# Pipeline benchmark
proc benchmark_pipeline_operations {iterations} {
    set pipe [r pipeline]
    
    for {set i 0} {$i < $iterations} {incr i} {
        $pipe set "pipe_key_$i" "pipe_value_$i"
    }
    
    $pipe execute
}

# Large value benchmark
proc benchmark_large_values {iterations} {
    set large_value [string repeat "x" 10000]
    
    for {set i 0} {$i < $iterations} {incr i} {
        r set "large_key_$i" $large_value
    }
}

# Concurrent clients benchmark
proc benchmark_concurrent_clients {iterations} {
    set client_count 10
    set ops_per_client [expr $iterations / $client_count]
    
    set clients {}
    for {set i 0} {$i < $client_count} {incr i} {
        lappend clients [redis [srv host] [srv port]]
    }
    
    # Perform operations concurrently
    foreach client $clients {
        for {set i 0} {$i < $ops_per_client} {incr i} {
            $client set "concurrent_${client}_$i" "value_$i"
        }
    }
    
    # Clean up clients
    foreach client $clients {
        $client close
    }
}

puts "=== io_uring vs epoll Performance Comparison ==="
puts ""

set test_iterations 10000

# Test with io_uring enabled
puts "=== Testing with io_uring ENABLED ==="
set uring_config {tags {"benchmark uring"} overrides {uring-enabled yes uring-sqpoll no}}

set uring_set_results [benchmark_operation $uring_config benchmark_set_operations $test_iterations "SET operations with io_uring"]
set uring_get_results [benchmark_operation $uring_config benchmark_get_operations $test_iterations "GET operations with io_uring"]
set uring_mixed_results [benchmark_operation $uring_config benchmark_mixed_operations $test_iterations "Mixed operations with io_uring"]
set uring_pipeline_results [benchmark_operation $uring_config benchmark_pipeline_operations $test_iterations "Pipeline operations with io_uring"]
set uring_large_results [benchmark_operation $uring_config benchmark_large_values 1000 "Large value operations with io_uring"]
set uring_concurrent_results [benchmark_operation $uring_config benchmark_concurrent_clients $test_iterations "Concurrent client operations with io_uring"]

# Test with io_uring disabled (epoll)
puts "=== Testing with io_uring DISABLED (epoll) ==="
set epoll_config {tags {"benchmark epoll"} overrides {uring-enabled no}}

set epoll_set_results [benchmark_operation $epoll_config benchmark_set_operations $test_iterations "SET operations with epoll"]
set epoll_get_results [benchmark_operation $epoll_config benchmark_get_operations $test_iterations "GET operations with epoll"]
set epoll_mixed_results [benchmark_operation $epoll_config benchmark_mixed_operations $test_iterations "Mixed operations with epoll"]
set epoll_pipeline_results [benchmark_operation $epoll_config benchmark_pipeline_operations $test_iterations "Pipeline operations with epoll"]
set epoll_large_results [benchmark_operation $epoll_config benchmark_large_values 1000 "Large value operations with epoll"]
set epoll_concurrent_results [benchmark_operation $epoll_config benchmark_concurrent_clients $test_iterations "Concurrent client operations with epoll"]

# Test with io_uring + SQPOLL
puts "=== Testing with io_uring + SQPOLL ENABLED ==="
set sqpoll_config {tags {"benchmark sqpoll"} overrides {uring-enabled yes uring-sqpoll yes}}

set sqpoll_set_results [benchmark_operation $sqpoll_config benchmark_set_operations $test_iterations "SET operations with io_uring + SQPOLL"]
set sqpoll_get_results [benchmark_operation $sqpoll_config benchmark_get_operations $test_iterations "GET operations with io_uring + SQPOLL"]
set sqpoll_mixed_results [benchmark_operation $sqpoll_config benchmark_mixed_operations $test_iterations "Mixed operations with io_uring + SQPOLL"]

# Performance comparison summary
puts "=== PERFORMANCE COMPARISON SUMMARY ==="
puts ""

proc compare_results {test_name uring_results epoll_results {sqpoll_results {}}} {
    set uring_ops [lindex $uring_results 1]
    set epoll_ops [lindex $epoll_results 1]
    set improvement [expr ($uring_ops - $epoll_ops) / $epoll_ops * 100]
    
    puts "$test_name:"
    puts "  io_uring: [format %.2f $uring_ops] ops/sec"
    puts "  epoll:    [format %.2f $epoll_ops] ops/sec"
    puts "  Improvement: [format %+.1f $improvement]%"
    
    if {$sqpoll_results ne {}} {
        set sqpoll_ops [lindex $sqpoll_results 1]
        set sqpoll_improvement [expr ($sqpoll_ops - $epoll_ops) / $epoll_ops * 100]
        puts "  SQPOLL:   [format %.2f $sqpoll_ops] ops/sec"
        puts "  SQPOLL Improvement: [format %+.1f $sqpoll_improvement]%"
    }
    
    # Memory comparison
    set uring_memory [lindex $uring_results 2]
    set epoll_memory [lindex $epoll_results 2]
    set memory_diff [expr ($uring_memory - $epoll_memory) / double($epoll_memory) * 100]
    puts "  Memory difference: [format %+.1f $memory_diff]%"
    puts ""
}

compare_results "SET Operations" $uring_set_results $epoll_set_results $sqpoll_set_results
compare_results "GET Operations" $uring_get_results $epoll_get_results $sqpoll_get_results
compare_results "Mixed Operations" $uring_mixed_results $epoll_mixed_results $sqpoll_mixed_results
compare_results "Pipeline Operations" $uring_pipeline_results $epoll_pipeline_results
compare_results "Large Value Operations" $uring_large_results $epoll_large_results
compare_results "Concurrent Client Operations" $uring_concurrent_results $epoll_concurrent_results

# CPU usage test (simplified)
puts "=== CPU USAGE COMPARISON ==="
puts ""

proc measure_cpu_usage {server_config test_proc description} {
    puts "Measuring CPU usage: $description"
    
    start_server $server_config {
        # Get initial CPU time
        set start_info [r info cpu]
        regexp {used_cpu_sys:([0-9.]+)} $start_info - start_cpu_sys
        regexp {used_cpu_user:([0-9.]+)} $start_info - start_cpu_user
        
        # Run test
        eval $test_proc 5000
        
        # Get final CPU time
        set end_info [r info cpu]
        regexp {used_cpu_sys:([0-9.]+)} $end_info - end_cpu_sys
        regexp {used_cpu_user:([0-9.]+)} $end_info - end_cpu_user
        
        set cpu_sys_used [expr $end_cpu_sys - $start_cpu_sys]
        set cpu_user_used [expr $end_cpu_user - $start_cpu_user]
        set total_cpu [expr $cpu_sys_used + $cpu_user_used]
        
        puts "  System CPU: [format %.3f $cpu_sys_used]s"
        puts "  User CPU: [format %.3f $cpu_user_used]s"
        puts "  Total CPU: [format %.3f $total_cpu]s"
        puts ""
        
        return $total_cpu
    }
}

set uring_cpu [measure_cpu_usage $uring_config benchmark_mixed_operations "Mixed operations with io_uring"]
set epoll_cpu [measure_cpu_usage $epoll_config benchmark_mixed_operations "Mixed operations with epoll"]

set cpu_improvement [expr ($epoll_cpu - $uring_cpu) / $epoll_cpu * 100]
puts "CPU Usage Improvement with io_uring: [format %+.1f $cpu_improvement]%"
puts ""

puts "=== BENCHMARK COMPLETE ==="
