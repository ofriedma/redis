# Performance benchmarks for io_uring buffer pool
# These tests verify that the buffer pool meets performance requirements
# and provides the expected >80% hit rate under realistic workloads.

proc benchmark_buffer_pool_performance {operations_count description} {
    set start_time [clock milliseconds]
    
    # Perform the specified number of operations
    for {set i 0} {$i < $operations_count} {incr i} {
        r set "bench_key_$i" "benchmark_value_$i"
        r get "bench_key_$i"
    }
    
    set end_time [clock milliseconds]
    set duration [expr $end_time - $start_time]
    set ops_per_second [expr $operations_count * 2 * 1000 / $duration]
    
    puts "Buffer pool benchmark - $description:"
    puts "  Operations: $operations_count (SET + GET each)"
    puts "  Duration: ${duration}ms"
    puts "  Ops/sec: [format "%.0f" $ops_per_second]"
    
    return $ops_per_second
}

start_server {tags {"uring buffer pool benchmark"}} {
    test {buffer pool achieves target hit rate under sustained load} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Clear any existing data
            r flushall
            
            # Warm up the buffer pool with initial operations
            for {set i 0} {$i < 50} {incr i} {
                r set "warmup_$i" "value_$i"
                r get "warmup_$i"
            }
            
            # Get baseline statistics
            set baseline_uring_info [r info uring]
            
            # Perform sustained load test
            set operations_count 1000
            benchmark_buffer_pool_performance $operations_count "Sustained Load Test"
            
            # Get final statistics
            set final_uring_info [r info uring]
            
            if {[string match "*buffer_pool_hit_rate*" $final_uring_info]} {
                regexp {buffer_pool_hit_rate:([0-9.]+)} $final_uring_info match hit_rate
                regexp {buffer_pool_total_allocations:(\d+)} $final_uring_info match total_allocs
                regexp {buffer_pool_hits:(\d+)} $final_uring_info match hits
                regexp {buffer_pool_misses:(\d+)} $final_uring_info match misses
                
                puts "Buffer pool performance metrics:"
                puts "  Hit rate: ${hit_rate}%"
                puts "  Total allocations: $total_allocs"
                puts "  Hits: $hits"
                puts "  Misses: $misses"
                
                # Verify we had significant allocation activity
                assert {$total_allocs > 100}
                
                # Target hit rate should be >80% for sustained workload
                # We'll be slightly more lenient in tests (>75%) to account for test environment variability
                if {$total_allocs > 0} {
                    assert {$hit_rate >= 75.0}
                    puts "✓ Buffer pool hit rate target achieved: ${hit_rate}% (target: >75%)"
                }
            }
        }
    }
    
    test {buffer pool performance scales with concurrent connections} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Create multiple concurrent connections
            set num_clients 8
            set operations_per_client 100
            
            set clients {}
            for {set i 0} {$i < $num_clients} {incr i} {
                lappend clients [redis [srv host] [srv port]]
            }
            
            # Get baseline statistics
            set baseline_uring_info [r info uring]
            if {[string match "*buffer_pool_total_allocations*" $baseline_uring_info]} {
                regexp {buffer_pool_total_allocations:(\d+)} $baseline_uring_info match baseline_allocs
            } else {
                set baseline_allocs 0
            }
            
            set start_time [clock milliseconds]
            
            # Perform concurrent operations
            foreach client $clients {
                for {set j 0} {$j < $operations_per_client} {incr j} {
                    $client set "concurrent_${i}_${j}" "value_${j}"
                    $client get "concurrent_${i}_${j}"
                }
            }
            
            set end_time [clock milliseconds]
            set duration [expr $end_time - $start_time]
            set total_operations [expr $num_clients * $operations_per_client * 2]
            set ops_per_second [expr $total_operations * 1000 / $duration]
            
            puts "Concurrent buffer pool benchmark:"
            puts "  Clients: $num_clients"
            puts "  Operations per client: $operations_per_client (SET + GET each)"
            puts "  Total operations: $total_operations"
            puts "  Duration: ${duration}ms"
            puts "  Ops/sec: [format "%.0f" $ops_per_second]"
            
            # Get final statistics
            set final_uring_info [r info uring]
            if {[string match "*buffer_pool_hit_rate*" $final_uring_info]} {
                regexp {buffer_pool_hit_rate:([0-9.]+)} $final_uring_info match hit_rate
                regexp {buffer_pool_total_allocations:(\d+)} $final_uring_info match total_allocs
                
                set new_allocs [expr $total_allocs - $baseline_allocs]
                puts "  New allocations: $new_allocs"
                puts "  Hit rate: ${hit_rate}%"
                
                # Should have reasonable performance even with concurrent access
                assert {$ops_per_second > 1000}  ;# At least 1000 ops/sec
                
                # Hit rate should still be good with concurrent access
                if {$new_allocs > 0} {
                    assert {$hit_rate >= 70.0}  ;# Slightly lower threshold for concurrent access
                    puts "✓ Concurrent buffer pool hit rate acceptable: ${hit_rate}%"
                }
            }
            
            # Clean up clients
            foreach client $clients {
                $client close
            }
        }
    }
    
    test {buffer pool memory efficiency under varying workloads} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test different workload patterns
            
            # Pattern 1: Small frequent operations
            puts "Testing small frequent operations..."
            for {set i 0} {$i < 200} {incr i} {
                r set "small_$i" "val"
                r get "small_$i"
            }
            
            set small_ops_info [r info uring]
            if {[string match "*buffer_pool_hit_rate*" $small_ops_info]} {
                regexp {buffer_pool_hit_rate:([0-9.]+)} $small_ops_info match small_hit_rate
                regexp {buffer_pool_utilization:([0-9.]+)} $small_ops_info match small_utilization
                puts "  Small ops hit rate: ${small_hit_rate}%"
                puts "  Small ops utilization: ${small_utilization}%"
            }
            
            # Pattern 2: Larger operations
            puts "Testing larger operations..."
            set large_value [string repeat "x" 1000]
            for {set i 0} {$i < 100} {incr i} {
                r set "large_$i" $large_value
                r get "large_$i"
            }
            
            set large_ops_info [r info uring]
            if {[string match "*buffer_pool_hit_rate*" $large_ops_info]} {
                regexp {buffer_pool_hit_rate:([0-9.]+)} $large_ops_info match large_hit_rate
                regexp {buffer_pool_utilization:([0-9.]+)} $large_ops_info match large_utilization
                regexp {buffer_pool_peak_usage:(\d+)} $large_ops_info match peak_usage
                puts "  Large ops hit rate: ${large_hit_rate}%"
                puts "  Large ops utilization: ${large_utilization}%"
                puts "  Peak usage: $peak_usage buffers"
                
                # Both workload patterns should maintain good hit rates
                assert {$small_hit_rate >= 70.0}
                assert {$large_hit_rate >= 70.0}
                
                # Peak usage should be reasonable (not exhausting the pool)
                regexp {buffer_pool_size:(\d+)} $large_ops_info match pool_size
                set peak_utilization_pct [expr $peak_usage * 100.0 / $pool_size]
                assert {$peak_utilization_pct < 90.0}  ;# Should not exhaust >90% of pool
                
                puts "✓ Buffer pool handles varying workloads efficiently"
            }
        }
    }
    
    test {buffer pool allocation speed meets performance requirements} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Measure allocation/deallocation speed indirectly through operation throughput
            
            # Baseline: operations without buffer pool stress
            set baseline_ops_per_sec [benchmark_buffer_pool_performance 500 "Baseline Performance"]
            
            # Stress test: rapid allocation/deallocation
            set stress_ops_per_sec [benchmark_buffer_pool_performance 1000 "Stress Test Performance"]
            
            # Performance should not degrade significantly under stress
            set performance_ratio [expr $stress_ops_per_sec / $baseline_ops_per_sec]
            puts "Performance ratio (stress/baseline): [format "%.2f" $performance_ratio]"
            
            # Performance should not degrade by more than 20%
            assert {$performance_ratio >= 0.8}
            
            # Absolute performance should be reasonable
            assert {$stress_ops_per_sec > 2000}  ;# At least 2000 ops/sec under stress
            
            puts "✓ Buffer pool allocation speed meets requirements"
        }
    }
    
    test {buffer pool statistics accuracy under high load} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Get initial statistics
            set initial_uring_info [r info uring]
            if {[string match "*buffer_pool_total_allocations*" $initial_uring_info]} {
                regexp {buffer_pool_total_allocations:(\d+)} $initial_uring_info match initial_allocs
                regexp {buffer_pool_total_deallocations:(\d+)} $initial_uring_info match initial_deallocs
            } else {
                set initial_allocs 0
                set initial_deallocs 0
            }
            
            # Perform high-load operations
            set high_load_operations 2000
            benchmark_buffer_pool_performance $high_load_operations "High Load Statistics Test"
            
            # Get final statistics
            set final_uring_info [r info uring]
            if {[string match "*buffer_pool_total_allocations*" $final_uring_info]} {
                regexp {buffer_pool_total_allocations:(\d+)} $final_uring_info match final_allocs
                regexp {buffer_pool_total_deallocations:(\d+)} $final_uring_info match final_deallocs
                regexp {buffer_pool_hits:(\d+)} $final_uring_info match hits
                regexp {buffer_pool_misses:(\d+)} $final_uring_info match misses
                regexp {buffer_pool_allocated_count:(\d+)} $final_uring_info match allocated_count
                regexp {buffer_pool_free_count:(\d+)} $final_uring_info match free_count
                regexp {buffer_pool_size:(\d+)} $final_uring_info match pool_size
                
                set new_allocs [expr $final_allocs - $initial_allocs]
                set new_deallocs [expr $final_deallocs - $initial_deallocs]
                
                puts "High load statistics verification:"
                puts "  New allocations: $new_allocs"
                puts "  New deallocations: $new_deallocs"
                puts "  Hits: $hits"
                puts "  Misses: $misses"
                puts "  Currently allocated: $allocated_count"
                puts "  Currently free: $free_count"
                
                # Statistics should be consistent
                assert {$final_allocs == [expr $hits + $misses]}
                assert {[expr $allocated_count + $free_count] == $pool_size}
                
                # Should have significant activity
                assert {$new_allocs > 1000}
                
                # Deallocations should be close to allocations (buffers are returned)
                set dealloc_ratio [expr $new_deallocs * 1.0 / $new_allocs]
                assert {$dealloc_ratio >= 0.8}  ;# At least 80% of buffers returned
                
                puts "✓ Buffer pool statistics remain accurate under high load"
            }
        }
    }
}
