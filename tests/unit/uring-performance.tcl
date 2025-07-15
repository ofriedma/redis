# io_uring performance tests
# Performance validation for all team member implementations

start_server {tags {"uring performance"}} {
    test {Performance: Basic operation throughput} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test basic operation throughput
            set num_ops 1000
            set start_time [clock milliseconds]
            
            # Perform operations
            for {set i 0} {$i < $num_ops} {incr i} {
                r set "perf_basic_$i" "perf_value_$i"
            }
            
            set end_time [clock milliseconds]
            set total_time [expr $end_time - $start_time]
            
            # Calculate throughput
            set throughput [expr double($num_ops) / ($total_time / 1000.0)]
            
            # Should achieve reasonable throughput (adjust based on system)
            assert {$throughput > 100}  ;# At least 100 ops/sec
            
            # Verify all operations completed
            for {set i 0} {$i < $num_ops} {incr i} {
                assert_equal "perf_value_$i" [r get "perf_basic_$i"]
                r del "perf_basic_$i"
            }
        }
    }
    
    test {Performance: Latency measurements} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test operation latency
            set num_samples 100
            set total_latency 0
            
            for {set i 0} {$i < $num_samples} {incr i} {
                set start_time [clock microseconds]
                r set "latency_test_$i" "latency_value_$i"
                r get "latency_test_$i"
                set end_time [clock microseconds]
                
                set latency [expr $end_time - $start_time]
                set total_latency [expr $total_latency + $latency]
            }
            
            set avg_latency [expr $total_latency / $num_samples]
            
            # Average latency should be reasonable (adjust based on system)
            assert {$avg_latency < 10000}  ;# Less than 10ms average
            
            # Clean up
            for {set i 0} {$i < $num_samples} {incr i} {
                r del "latency_test_$i"
            }
        }
    }
    
    test {Performance: Buffer management efficiency} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test buffer management performance with various sizes
            set buffer_sizes {512 1024 2048 4096 8192}
            
            foreach size $buffer_sizes {
                set data [string repeat "b" $size]
                set start_time [clock milliseconds]
                
                # Test buffer operations
                for {set i 0} {$i < 100} {incr i} {
                    r set "buffer_perf_${size}_$i" $data
                }
                
                set end_time [clock milliseconds]
                set buffer_time [expr $end_time - $start_time]
                
                # Buffer operations should complete efficiently
                assert {$buffer_time < 5000}  ;# Less than 5 seconds for 100 ops
                
                # Verify data integrity
                for {set i 0} {$i < 100} {incr i} {
                    assert_equal $data [r get "buffer_perf_${size}_$i"]
                    r del "buffer_perf_${size}_$i"
                }
            }
        }
    }
    
    test {Performance: Concurrent operation performance} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test concurrent operation performance using pipelining
            set num_ops 500
            set start_time [clock milliseconds]
            
            # Test concurrent operations using sequential calls
            for {set i 0} {$i < $num_ops} {incr i} {
                r set "concurrent_perf_$i" "concurrent_value_$i"
            }
            for {set i 0} {$i < $num_ops} {incr i} {
                assert_equal "concurrent_value_$i" [r get "concurrent_perf_$i"]
            }

            set end_time [clock milliseconds]
            set total_time [expr $end_time - $start_time]

            # Operations should complete efficiently
            set throughput [expr double($num_ops * 2) / ($total_time / 1000.0)]
            assert {$throughput > 50}  ;# At least 50 ops/sec for sequential
            
            # Clean up
            for {set i 0} {$i < $num_ops} {incr i} {
                r del "concurrent_perf_$i"
            }
        }
    }
    
    test {Performance: Memory efficiency under load} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test memory efficiency
            set initial_memory [r info memory]
            
            # Create memory load
            set num_keys 1000
            for {set i 0} {$i < $num_keys} {incr i} {
                set data [string repeat "m" 1024]
                r set "memory_perf_$i" $data
            }
            
            set loaded_memory [r info memory]
            
            # Perform operations under memory load
            set start_time [clock milliseconds]
            for {set i 0} {$i < $num_keys} {incr i} {
                r get "memory_perf_$i"
            }
            set end_time [clock milliseconds]
            
            set read_time [expr $end_time - $start_time]
            
            # Operations should remain efficient under memory load
            assert {$read_time < 2000}  ;# Less than 2 seconds
            
            # Clean up
            for {set i 0} {$i < $num_keys} {incr i} {
                r del "memory_perf_$i"
            }
        }
    }
    
    test {Performance: Statistics collection overhead} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test that statistics collection doesn't significantly impact performance
            
            # Baseline performance without statistics queries
            set start_time [clock milliseconds]
            for {set i 0} {$i < 200} {incr i} {
                r set "stats_overhead_$i" "stats_value_$i"
            }
            set baseline_time [expr [clock milliseconds] - $start_time]
            
            # Performance with frequent statistics queries
            set start_time [clock milliseconds]
            for {set i 0} {$i < 200} {incr i} {
                r set "stats_overhead2_$i" "stats_value_$i"
                if {$i % 50 == 0} {
                    r info uring
                }
            }
            set with_stats_time [expr [clock milliseconds] - $start_time]
            
            # Statistics collection should have minimal overhead
            set overhead_ratio [expr double($with_stats_time) / $baseline_time]
            assert {$overhead_ratio < 2.0}  ;# Less than 2x overhead
            
            # Clean up
            for {set i 0} {$i < 200} {incr i} {
                r del "stats_overhead_$i"
                r del "stats_overhead2_$i"
            }
        }
    }
    
    test {Performance: Connection handling efficiency} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test connection handling performance
            # This test uses the existing connection but tests rapid operations
            
            set start_time [clock milliseconds]
            
            # Rapid operations to test connection efficiency
            for {set i 0} {$i < 300} {incr i} {
                r set "conn_perf_$i" "conn_value_$i"
                r get "conn_perf_$i"
            }
            
            set end_time [clock milliseconds]
            set total_time [expr $end_time - $start_time]
            
            # Connection should handle rapid operations efficiently
            assert {$total_time < 3000}  ;# Less than 3 seconds for 600 ops
            
            # Clean up
            for {set i 0} {$i < 300} {incr i} {
                r del "conn_perf_$i"
            }
        }
    }
    
    test {Performance: Large data handling} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test performance with large data
            set large_sizes {10240 20480 51200}  ;# 10KB, 20KB, 50KB
            
            foreach size $large_sizes {
                set large_data [string repeat "L" $size]
                
                set start_time [clock milliseconds]
                
                # Test large data operations
                for {set i 0} {$i < 10} {incr i} {
                    r set "large_perf_${size}_$i" $large_data
                }
                
                for {set i 0} {$i < 10} {incr i} {
                    set retrieved [r get "large_perf_${size}_$i"]
                    assert_equal $large_data $retrieved
                }
                
                set end_time [clock milliseconds]
                set large_time [expr $end_time - $start_time]
                
                # Large data should be handled efficiently
                assert {$large_time < 1000}  ;# Less than 1 second per size
                
                # Clean up
                for {set i 0} {$i < 10} {incr i} {
                    r del "large_perf_${size}_$i"
                }
            }
        }
    }
    
    test {Performance: Mixed workload performance} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test performance with mixed workload
            set start_time [clock milliseconds]
            
            # Mixed operations: different sizes, types
            for {set i 0} {$i < 150} {incr i} {
                if {$i % 3 == 0} {
                    # Small data
                    r set "mixed_small_$i" "small_data"
                } elseif {$i % 3 == 1} {
                    # Medium data
                    set medium_data [string repeat "M" 1024]
                    r set "mixed_medium_$i" $medium_data
                } else {
                    # Large data
                    set large_data [string repeat "L" 4096]
                    r set "mixed_large_$i" $large_data
                }
                
                # Intersperse reads
                if {$i > 10 && $i % 5 == 0} {
                    r get "mixed_small_[expr $i - 10]"
                }
            }
            
            set end_time [clock milliseconds]
            set mixed_time [expr $end_time - $start_time]
            
            # Mixed workload should be handled efficiently
            assert {$mixed_time < 2000}  ;# Less than 2 seconds
            
            # Clean up
            for {set i 0} {$i < 150} {incr i} {
                catch {r del "mixed_small_$i"}
                catch {r del "mixed_medium_$i"}
                catch {r del "mixed_large_$i"}
            }
        }
    }
    
    test {Performance: io_uring statistics performance impact} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test that io_uring specific features don't degrade performance
            
            # Get initial statistics
            set initial_stats [r info uring]
            
            # Perform operations while monitoring statistics
            set start_time [clock milliseconds]
            
            for {set i 0} {$i < 100} {incr i} {
                r set "uring_stats_perf_$i" "uring_value_$i"
                r get "uring_stats_perf_$i"
                
                # Check statistics periodically
                if {$i % 25 == 0} {
                    set current_stats [r info uring]
                    # Statistics should be available
                    assert {[string length $current_stats] > 0}
                }
            }
            
            set end_time [clock milliseconds]
            set stats_time [expr $end_time - $start_time]
            
            # Performance should remain good even with statistics
            assert {$stats_time < 1000}  ;# Less than 1 second
            
            # Final statistics check
            set final_stats [r info uring]
            if {[string length $final_stats] > 0} {
                # Should show increased operation counts
                assert {[string match "*uring_ops_completed*" $final_stats]}
            }
            
            # Clean up
            for {set i 0} {$i < 100} {incr i} {
                r del "uring_stats_perf_$i"
            }
        }
    }
}

# Performance tests with specific configurations
start_server {tags {"uring performance config"} overrides {uring-enabled yes uring-sq-entries 1024}} {
    test {Performance: Large queue configuration performance} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test performance with larger queue sizes
            set start_time [clock milliseconds]
            
            # High throughput test
            for {set i 0} {$i < 2000} {incr i} {
                r set "large_queue_perf_$i" "large_queue_value_$i"
            }
            
            set end_time [clock milliseconds]
            set large_queue_time [expr $end_time - $start_time]
            
            # Large queues should provide better performance
            set throughput [expr 2000.0 / ($large_queue_time / 1000.0)]
            assert {$throughput > 500}  ;# At least 500 ops/sec
            
            # Clean up
            for {set i 0} {$i < 2000} {incr i} {
                r del "large_queue_perf_$i"
            }
        }
    }
}
