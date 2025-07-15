# Week 4 io_uring Latency Measurement Benchmarks
# Detailed latency analysis and percentile measurements

proc measure_latency_distribution {server_config operation_proc iterations description} {
    puts "Measuring latency distribution: $description"
    
    start_server $server_config {
        set latencies {}
        
        # Warm up
        for {set i 0} {$i < 100} {incr i} {
            eval $operation_proc 1
        }
        
        # Measure individual operation latencies
        for {set i 0} {$i < $iterations} {incr i} {
            set start_time [clock microseconds]
            eval $operation_proc 1
            set end_time [clock microseconds]
            
            set latency [expr $end_time - $start_time]
            lappend latencies $latency
        }
        
        # Sort latencies for percentile calculation
        set sorted_latencies [lsort -real $latencies]
        set count [llength $sorted_latencies]
        
        # Calculate percentiles
        set p50_idx [expr int($count * 0.50)]
        set p90_idx [expr int($count * 0.90)]
        set p95_idx [expr int($count * 0.95)]
        set p99_idx [expr int($count * 0.99)]
        set p999_idx [expr int($count * 0.999)]
        
        set min_latency [lindex $sorted_latencies 0]
        set max_latency [lindex $sorted_latencies end]
        set p50_latency [lindex $sorted_latencies $p50_idx]
        set p90_latency [lindex $sorted_latencies $p90_idx]
        set p95_latency [lindex $sorted_latencies $p95_idx]
        set p99_latency [lindex $sorted_latencies $p99_idx]
        set p999_latency [lindex $sorted_latencies $p999_idx]
        
        # Calculate average
        set total_latency 0
        foreach lat $latencies {
            set total_latency [expr $total_latency + $lat]
        }
        set avg_latency [expr $total_latency / double($count)]
        
        puts "  Samples: $count"
        puts "  Min:     [format %.1f $min_latency] μs"
        puts "  Avg:     [format %.1f $avg_latency] μs"
        puts "  P50:     [format %.1f $p50_latency] μs"
        puts "  P90:     [format %.1f $p90_latency] μs"
        puts "  P95:     [format %.1f $p95_latency] μs"
        puts "  P99:     [format %.1f $p99_latency] μs"
        puts "  P99.9:   [format %.1f $p999_latency] μs"
        puts "  Max:     [format %.1f $max_latency] μs"
        puts ""
        
        return [list $min_latency $avg_latency $p50_latency $p90_latency $p95_latency $p99_latency $p999_latency $max_latency]
    }
}

# Single SET operation
proc single_set_operation {iterations} {
    for {set i 0} {$i < $iterations} {incr i} {
        r set "latency_key" "latency_value"
    }
}

# Single GET operation
proc single_get_operation {iterations} {
    # Ensure key exists
    r set "latency_key" "latency_value"
    
    for {set i 0} {$i < $iterations} {incr i} {
        r get "latency_key"
    }
}

# Single PING operation
proc single_ping_operation {iterations} {
    for {set i 0} {$i < $iterations} {incr i} {
        r ping
    }
}

# Single INCR operation
proc single_incr_operation {iterations} {
    r set "counter" 0
    
    for {set i 0} {$i < $iterations} {incr i} {
        r incr "counter"
    }
}

# List operation
proc single_lpush_operation {iterations} {
    for {set i 0} {$i < $iterations} {incr i} {
        r lpush "latency_list" "item"
    }
}

puts "=== io_uring Latency Distribution Analysis ==="
puts ""

set test_iterations 1000

# Test with io_uring enabled
puts "=== Latency with io_uring ENABLED ==="
set uring_config {tags {"latency uring"} overrides {uring-enabled yes uring-sqpoll no}}

set uring_set_latency [measure_latency_distribution $uring_config single_set_operation $test_iterations "SET operation with io_uring"]
set uring_get_latency [measure_latency_distribution $uring_config single_get_operation $test_iterations "GET operation with io_uring"]
set uring_ping_latency [measure_latency_distribution $uring_config single_ping_operation $test_iterations "PING operation with io_uring"]
set uring_incr_latency [measure_latency_distribution $uring_config single_incr_operation $test_iterations "INCR operation with io_uring"]
set uring_lpush_latency [measure_latency_distribution $uring_config single_lpush_operation $test_iterations "LPUSH operation with io_uring"]

# Test with io_uring disabled (epoll)
puts "=== Latency with io_uring DISABLED (epoll) ==="
set epoll_config {tags {"latency epoll"} overrides {uring-enabled no}}

set epoll_set_latency [measure_latency_distribution $epoll_config single_set_operation $test_iterations "SET operation with epoll"]
set epoll_get_latency [measure_latency_distribution $epoll_config single_get_operation $test_iterations "GET operation with epoll"]
set epoll_ping_latency [measure_latency_distribution $epoll_config single_ping_operation $test_iterations "PING operation with epoll"]
set epoll_incr_latency [measure_latency_distribution $epoll_config single_incr_operation $test_iterations "INCR operation with epoll"]
set epoll_lpush_latency [measure_latency_distribution $epoll_config single_lpush_operation $test_iterations "LPUSH operation with epoll"]

# Test with io_uring + SQPOLL
puts "=== Latency with io_uring + SQPOLL ENABLED ==="
set sqpoll_config {tags {"latency sqpoll"} overrides {uring-enabled yes uring-sqpoll yes}}

set sqpoll_set_latency [measure_latency_distribution $sqpoll_config single_set_operation $test_iterations "SET operation with io_uring + SQPOLL"]
set sqpoll_get_latency [measure_latency_distribution $sqpoll_config single_get_operation $test_iterations "GET operation with io_uring + SQPOLL"]
set sqpoll_ping_latency [measure_latency_distribution $sqpoll_config single_ping_operation $test_iterations "PING operation with io_uring + SQPOLL"]

# Latency comparison summary
puts "=== LATENCY COMPARISON SUMMARY ==="
puts ""

proc compare_latency {operation_name uring_latency epoll_latency {sqpoll_latency {}}} {
    puts "$operation_name Latency Comparison:"
    puts "                 io_uring    epoll      Improvement"
    
    set metrics [list "Average" "P50" "P90" "P95" "P99" "P99.9"]
    set indices [list 1 2 3 4 5 6]
    
    foreach metric $metrics index $indices {
        set uring_val [lindex $uring_latency $index]
        set epoll_val [lindex $epoll_latency $index]
        set improvement [expr ($epoll_val - $uring_val) / $epoll_val * 100]
        
        puts "  [format %-8s $metric]: [format %8.1f $uring_val] μs [format %8.1f $epoll_val] μs [format %+6.1f $improvement]%"
    }
    
    if {$sqpoll_latency ne {}} {
        puts ""
        puts "  SQPOLL Comparison:"
        puts "                 SQPOLL      epoll      Improvement"
        
        foreach metric $metrics index $indices {
            set sqpoll_val [lindex $sqpoll_latency $index]
            set epoll_val [lindex $epoll_latency $index]
            set improvement [expr ($epoll_val - $sqpoll_val) / $epoll_val * 100]
            
            puts "  [format %-8s $metric]: [format %8.1f $sqpoll_val] μs [format %8.1f $epoll_val] μs [format %+6.1f $improvement]%"
        }
    }
    
    puts ""
}

compare_latency "SET" $uring_set_latency $epoll_set_latency $sqpoll_set_latency
compare_latency "GET" $uring_get_latency $epoll_get_latency $sqpoll_get_latency
compare_latency "PING" $uring_ping_latency $epoll_ping_latency $sqpoll_ping_latency
compare_latency "INCR" $uring_incr_latency $epoll_incr_latency
compare_latency "LPUSH" $uring_lpush_latency $epoll_lpush_latency

# Tail latency analysis
puts "=== TAIL LATENCY ANALYSIS ==="
puts ""

proc analyze_tail_latency {operation_name uring_latency epoll_latency} {
    set uring_p99 [lindex $uring_latency 5]
    set uring_p999 [lindex $uring_latency 6]
    set uring_max [lindex $uring_latency 7]
    
    set epoll_p99 [lindex $epoll_latency 5]
    set epoll_p999 [lindex $epoll_latency 6]
    set epoll_max [lindex $epoll_latency 7]
    
    set p99_improvement [expr ($epoll_p99 - $uring_p99) / $epoll_p99 * 100]
    set p999_improvement [expr ($epoll_p999 - $uring_p999) / $epoll_p999 * 100]
    set max_improvement [expr ($epoll_max - $uring_max) / $epoll_max * 100]
    
    puts "$operation_name Tail Latency:"
    puts "  P99:   io_uring [format %6.1f $uring_p99] μs vs epoll [format %6.1f $epoll_p99] μs ([format %+.1f $p99_improvement]%)"
    puts "  P99.9: io_uring [format %6.1f $uring_p999] μs vs epoll [format %6.1f $epoll_p999] μs ([format %+.1f $p999_improvement]%)"
    puts "  Max:   io_uring [format %6.1f $uring_max] μs vs epoll [format %6.1f $epoll_max] μs ([format %+.1f $max_improvement]%)"
    puts ""
}

analyze_tail_latency "SET" $uring_set_latency $epoll_set_latency
analyze_tail_latency "GET" $uring_get_latency $epoll_get_latency
analyze_tail_latency "PING" $uring_ping_latency $epoll_ping_latency

# Load-dependent latency test
puts "=== LOAD-DEPENDENT LATENCY TEST ==="
puts ""

proc measure_latency_under_load {server_config load_level description} {
    puts "Measuring latency under load: $description (Load level: $load_level)"
    
    start_server $server_config {
        # Create background load
        set load_clients {}
        for {set i 0} {$i < $load_level} {incr i} {
            lappend load_clients [redis [srv host] [srv port]]
        }
        
        # Start background operations
        foreach client $load_clients {
            for {set i 0} {$i < 100} {incr i} {
                $client set "load_key_${client}_$i" "load_value_$i"
            }
        }
        
        # Measure latency of test operations
        set latencies {}
        for {set i 0} {$i < 100} {incr i} {
            set start_time [clock microseconds]
            r ping
            set end_time [clock microseconds]
            lappend latencies [expr $end_time - $start_time]
        }
        
        # Clean up load clients
        foreach client $load_clients {
            $client close
        }
        
        # Calculate average latency
        set total 0
        foreach lat $latencies {
            set total [expr $total + $lat]
        }
        set avg_latency [expr $total / double([llength $latencies])]
        
        puts "  Average latency under load: [format %.1f $avg_latency] μs"
        puts ""
        
        return $avg_latency
    }
}

# Test latency under different load levels
foreach load_level {1 5 10 20} {
    set uring_load_latency [measure_latency_under_load $uring_config $load_level "io_uring under load"]
    set epoll_load_latency [measure_latency_under_load $epoll_config $load_level "epoll under load"]
    
    set improvement [expr ($epoll_load_latency - $uring_load_latency) / $epoll_load_latency * 100]
    puts "Load level $load_level: io_uring [format %.1f $uring_load_latency] μs vs epoll [format %.1f $epoll_load_latency] μs ([format %+.1f $improvement]%)"
}

puts ""
puts "=== LATENCY MEASUREMENT COMPLETE ==="
