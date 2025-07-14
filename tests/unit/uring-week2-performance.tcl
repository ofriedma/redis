# io_uring Week 2 Performance Tests
# Performance benchmarks and regression tests for Week 2 implementations

start_server {tags {"uring week2 performance"}} {
    test {Performance: Buffer pool efficiency benchmark} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Benchmark buffer pool performance
            set initial_info [r info uring]
            set initial_hits [extract_info_field $initial_info "buffer_pool_hits"]
            set initial_misses [extract_info_field $initial_info "buffer_pool_misses"]
            
            set start_time [clock milliseconds]
            
            # Perform operations that stress buffer pool
            for {set i 0} {$i < 1000} {incr i} {
                set value [string repeat "x" 1024]
                r set "perf_buffer_$i" $value
            }
            
            set mid_time [clock milliseconds]
            
            # Read back to test buffer reuse
            for {set i 0} {$i < 1000} {incr i} {
                r get "perf_buffer_$i"
            }
            
            set end_time [clock milliseconds]
            
            set write_duration [expr {$mid_time - $start_time}]
            set read_duration [expr {$end_time - $mid_time}]
            
            # Performance should be reasonable
            assert {$write_duration < 5000} ;# Less than 5 seconds for writes
            assert {$read_duration < 3000}  ;# Less than 3 seconds for reads
            
            # Check buffer pool efficiency
            set final_info [r info uring]
            set final_hits [extract_info_field $final_info "buffer_pool_hits"]
            set final_misses [extract_info_field $final_info "buffer_pool_misses"]
            
            set total_requests [expr {$final_hits + $final_misses - $initial_hits - $initial_misses}]
            if {$total_requests > 0} {
                set hit_rate [expr {double($final_hits - $initial_hits) / $total_requests * 100}]
                # Buffer pool should have reasonable hit rate
                assert {$hit_rate >= 50.0} ;# At least 50% hit rate
            }
            
            # Clean up
            for {set i 0} {$i < 1000} {incr i} {
                r del "perf_buffer_$i"
            }
        } else {
            skip "io_uring not available"
        }
    }
    
    test {Performance: Operation completion time benchmark} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Benchmark operation completion times
            set initial_info [r info uring]
            set initial_completion_time [extract_info_field $initial_info "uring_avg_completion_time_us"]
            
            set start_time [clock microseconds]
            
            # Perform mixed operations
            for {set i 0} {$i < 500} {incr i} {
                r set "perf_op_$i" "value_$i"
                r get "perf_op_$i"
            }
            
            set end_time [clock microseconds]
            set total_duration [expr {$end_time - $start_time}]
            set avg_per_op [expr {$total_duration / 1000.0}] ;# 500 sets + 500 gets = 1000 ops
            
            # Average operation time should be reasonable
            assert {$avg_per_op < 1000} ;# Less than 1ms per operation on average
            
            # Check completion time statistics
            set final_info [r info uring]
            set final_completion_time [extract_info_field $final_info "uring_avg_completion_time_us"]
            set max_completion_time [extract_info_field $final_info "uring_max_completion_time_us"]
            
            # Completion times should be reasonable
            assert {$final_completion_time < 10000} ;# Less than 10ms average
            assert {$max_completion_time < 100000}   ;# Less than 100ms max
            
            # Clean up
            for {set i 0} {$i < 500} {incr i} {
                r del "perf_op_$i"
            }
        }
    }
    
    test {Performance: SQPOLL vs event-driven completion processing} {
        set info [r info uring]
        if {[string length $info] > 0} {
            set sqpoll_enabled [extract_info_field $info "uring_sqpoll_enabled"]

            # Benchmark current configuration
            set start_time [clock milliseconds]

            for {set i 0} {$i < 2000} {incr i} {
                r set "perf_completion_$i" "value_$i"
            }

            set end_time [clock milliseconds]
            set duration [expr {$end_time - $start_time}]

            if {$sqpoll_enabled eq "yes"} {
                # SQPOLL should provide better performance
                assert {$duration < 3000} ;# Less than 3 seconds with SQPOLL
            } else {
                # Event-driven processing should still be efficient
                assert {$duration < 5000} ;# Less than 5 seconds with event-driven approach
            }

            # Check completion processing statistics
            set final_info [r info uring]
            set ops_completed [extract_info_field $final_info "uring_ops_completed"]
            set completion_batches [extract_info_field $final_info "uring_completion_batches"]

            # Should have completion activity
            assert {$ops_completed > 0}

            # Batch processing should be efficient
            if {$completion_batches > 0} {
                set avg_batch_size [expr {double($ops_completed) / $completion_batches}]
                assert {$avg_batch_size >= 1.0} ;# At least 1 operation per batch
            }

            # Clean up
            for {set i 0} {$i < 2000} {incr i} {
                r del "perf_completion_$i"
            }
        }
    }
    
    test {Performance: Priority queue efficiency} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test priority queue performance
            set initial_info [r info uring]
            set initial_queued [extract_info_field $initial_info "operations_queued"]
            
            set start_time [clock milliseconds]
            
            # Create burst load to test queue efficiency
            r pipeline
            for {set i 0} {$i < 1500} {incr i} {
                r set "perf_queue_$i" "value_$i"
            }
            r execute
            
            set mid_time [clock milliseconds]
            
            # Verify all operations completed
            for {set i 0} {$i < 1500} {incr i} {
                assert_equal "value_$i" [r get "perf_queue_$i"]
            }
            
            set end_time [clock milliseconds]
            
            set submit_duration [expr {$mid_time - $start_time}]
            set verify_duration [expr {$end_time - $mid_time}]
            
            # Queue processing should be efficient
            assert {$submit_duration < 2000} ;# Less than 2 seconds to submit
            assert {$verify_duration < 3000}  ;# Less than 3 seconds to verify
            
            # Check queue statistics
            set final_info [r info uring]
            set final_completed [extract_info_field $final_info "operations_completed"]
            assert {$final_completed > 0}
            
            # Clean up
            for {set i 0} {$i < 1500} {incr i} {
                r del "perf_queue_$i"
            }
        }
    }
    
    test {Performance: Memory optimization effectiveness} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test memory optimization performance
            set initial_info [r info uring]
            set initial_allocated [extract_info_field $initial_info "buffer_pool_total_allocated_bytes"]
            set initial_freed [extract_info_field $initial_info "buffer_pool_total_freed_bytes"]
            
            # Create memory pressure
            set large_values {}
            for {set i 0} {$i < 100} {incr i} {
                set value [string repeat "m" [expr {$i * 100 + 1000}]]
                lappend large_values $value
                r set "perf_mem_$i" $value
            }
            
            # Force memory optimization by accessing data
            for {set i 0} {$i < 100} {incr i} {
                set retrieved [r get "perf_mem_$i"]
                assert_equal [lindex $large_values $i] $retrieved
            }
            
            # Check memory usage
            set final_info [r info uring]
            set final_allocated [extract_info_field $final_info "buffer_pool_total_allocated_bytes"]
            set final_freed [extract_info_field $final_info "buffer_pool_total_freed_bytes"]
            set peak_usage [extract_info_field $final_info "buffer_pool_peak_usage"]
            
            # Memory should have been allocated
            assert {$final_allocated > $initial_allocated}
            
            # Peak usage should be tracked
            assert {$peak_usage > 0}
            
            # Clean up
            for {set i 0} {$i < 100} {incr i} {
                r del "perf_mem_$i"
            }
        }
    }
    
    test {Performance: Connection lifecycle overhead} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test connection tracking overhead
            set initial_info [r info uring]
            set initial_connections [extract_info_field $initial_info "active_connections"]
            
            set start_time [clock milliseconds]
            
            # Simulate connection activity
            for {set i 0} {$i < 1000} {incr i} {
                r set "perf_conn_$i" "value_$i"
                r get "perf_conn_$i"
                
                # Check connection stats periodically
                if {$i % 100 == 0} {
                    set current_info [r info uring]
                    set current_connections [extract_info_field $current_info "active_connections"]
                    # Connection tracking shouldn't add significant overhead
                }
            }
            
            set end_time [clock milliseconds]
            set duration [expr {$end_time - $start_time}]
            
            # Connection tracking shouldn't significantly impact performance
            assert {$duration < 4000} ;# Less than 4 seconds
            
            # Check connection statistics
            set final_info [r info uring]
            set total_bytes_read [extract_info_field $final_info "total_bytes_read"]
            set total_bytes_written [extract_info_field $final_info "total_bytes_written"]
            
            # Should have tracked some data transfer
            assert {$total_bytes_read > 0}
            assert {$total_bytes_written > 0}
            
            # Clean up
            for {set i 0} {$i < 1000} {incr i} {
                r del "perf_conn_$i"
            }
        }
    }
    
    test {Performance: Error handling overhead} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test error handling performance impact
            set initial_info [r info uring]
            set initial_failed [extract_info_field $initial_info "uring_ops_failed"]
            
            set start_time [clock milliseconds]
            
            # Mix of successful and potentially problematic operations
            for {set i 0} {$i < 800} {incr i} {
                r set "perf_error_$i" "value_$i"
                
                # Occasionally try operations that might stress error handling
                if {$i % 50 == 0} {
                    # Try to get non-existent key (not an error, but exercises code paths)
                    r get "nonexistent_key_$i"
                }
            }
            
            set end_time [clock milliseconds]
            set duration [expr {$end_time - $start_time}]
            
            # Error handling shouldn't significantly impact performance
            assert {$duration < 3000} ;# Less than 3 seconds
            
            # Verify operations completed successfully
            for {set i 0} {$i < 800} {incr i} {
                assert_equal "value_$i" [r get "perf_error_$i"]
            }
            
            # Check error statistics
            set final_info [r info uring]
            set success_rate [extract_info_field $final_info "uring_success_rate"]
            
            # Success rate should remain high
            assert {$success_rate >= 95.0} ;# At least 95% success rate
            
            # Clean up
            for {set i 0} {$i < 800} {incr i} {
                r del "perf_error_$i"
            }
        }
    }
}

# Helper function to extract info field values
proc extract_info_field {info field} {
    set lines [split $info "\r\n"]
    foreach line $lines {
        if {[string match "$field:*" $line]} {
            return [string range $line [expr {[string length $field] + 1}] end]
        }
    }
    return ""
}
