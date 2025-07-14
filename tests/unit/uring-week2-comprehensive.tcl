# Comprehensive io_uring Week 2 tests covering all team member implementations
# Tests for Team Members A, B, C, and D Week 2 deliverables

start_server {tags {"uring week2 comprehensive"}} {
    test {Team Member A - Enhanced Core Implementation: Improved capability detection} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test enhanced capability detection
            assert {[string match "*uring_capabilities*" $info]}
            assert {[string match "*uring_backend*" $info]}
            
            # Test that capabilities are properly reported
            set capabilities [extract_info_field $info "uring_capabilities"]
            assert {$capabilities >= 1} ;# At least basic capability should be present
        } else {
            skip "io_uring not available"
        }
    }
    
    test {Team Member A - Enhanced Core Implementation: SQPOLL configuration} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test SQPOLL configuration reporting
            assert {[string match "*uring_sqpoll_enabled*" $info]}
            assert {[string match "*uring_sqpoll_cpu*" $info]}
            
            # Test that SQPOLL settings are reasonable
            set sqpoll_enabled [extract_info_field $info "uring_sqpoll_enabled"]
            if {$sqpoll_enabled eq "yes"} {
                set sqpoll_cpu [extract_info_field $info "uring_sqpoll_cpu"]
                assert {$sqpoll_cpu >= -1} ;# -1 means auto-select, >= 0 means specific CPU
            }
        }
    }
    
    test {Team Member A - Enhanced Core Implementation: Event-driven completion processing} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test that completion processing statistics are tracked
            assert {[string match "*uring_ops_completed*" $info]}
            assert {[string match "*completion_batches*" $info]}

            # Perform operations to trigger completion processing
            for {set i 0} {$i < 5} {incr i} {
                r set "completion_test_$i" "value_$i"
                r get "completion_test_$i"
            }

            # Check that operations were completed
            set new_info [r info uring]
            set ops_completed [extract_info_field $new_info "uring_ops_completed"]
            set completion_batches [extract_info_field $new_info "uring_completion_batches"]
            assert {$ops_completed > 0}
            assert {$completion_batches > 0}
        }
    }
    
    test {Team Member A - Enhanced Core Implementation: Statistics and monitoring} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test comprehensive statistics
            assert {[string match "*uring_ops_submitted*" $info]}
            assert {[string match "*uring_ops_completed*" $info]}
            assert {[string match "*uring_success_rate*" $info]}
            assert {[string match "*uring_avg_completion_time_us*" $info]}
            
            # Test error statistics
            assert {[string match "*uring_eagain_errors*" $info]}
            assert {[string match "*uring_econnreset_errors*" $info]}
            assert {[string match "*uring_epipe_errors*" $info]}
            
            # Test operation type statistics
            assert {[string match "*uring_read_ops*" $info]}
            assert {[string match "*uring_write_ops*" $info]}
            assert {[string match "*uring_accept_ops*" $info]}
        }
    }
    
    test {Team Member B - Enhanced Buffer Management: Buffer pool statistics} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test buffer pool statistics
            assert {[string match "*buffer_pool_enabled*" $info]}
            assert {[string match "*buffer_pool_hits*" $info]}
            assert {[string match "*buffer_pool_misses*" $info]}
            assert {[string match "*buffer_pool_hit_rate*" $info]}
            
            # Test buffer pool configuration
            assert {[string match "*buffer_pool_count*" $info]}
            assert {[string match "*buffer_pool_size*" $info]}
            assert {[string match "*buffer_pool_alignment*" $info]}
        }
    }
    
    test {Team Member B - Enhanced Buffer Management: Zero-copy buffer operations} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test buffer ring support
            assert {[string match "*uring_buffer_ring_enabled*" $info]}
            assert {[string match "*uring_buffer_ring_hits*" $info]}
            assert {[string match "*uring_buffer_ring_misses*" $info]}
            
            # Test buffer ring hit rate calculation
            set buffer_ring_enabled [extract_info_field $info "uring_buffer_ring_enabled"]
            if {$buffer_ring_enabled eq "yes"} {
                assert {[string match "*uring_buffer_hit_rate*" $info]}
            }
        }
    }
    
    test {Team Member B - Enhanced Buffer Management: Memory optimization} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test memory usage tracking
            assert {[string match "*buffer_pool_allocations*" $info]}
            assert {[string match "*buffer_pool_deallocations*" $info]}
            assert {[string match "*buffer_pool_current_usage*" $info]}
            assert {[string match "*buffer_pool_peak_usage*" $info]}
            
            # Test memory optimization statistics
            assert {[string match "*buffer_pool_total_allocated_bytes*" $info]}
            assert {[string match "*buffer_pool_total_freed_bytes*" $info]}
        }
    }
    
    test {Team Member B - Enhanced Buffer Management: Connection lifecycle tracking} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test connection tracking
            assert {[string match "*connection_tracking_enabled*" $info]}
            assert {[string match "*active_connections*" $info]}
            assert {[string match "*max_connections*" $info]}
            
            # Test connection state tracking
            assert {[string match "*connections_init*" $info]}
            assert {[string match "*connections_connected*" $info]}
            assert {[string match "*connections_reading*" $info]}
            assert {[string match "*connections_writing*" $info]}
        }
    }
    
    test {Team Member C - Operation Management: Enhanced operation tracking} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test operation tracking
            assert {[string match "*operation_tracking_enabled*" $info]}
            assert {[string match "*active_operations*" $info]}
            assert {[string match "*max_operations*" $info]}
            assert {[string match "*next_op_id*" $info]}
            
            # Test operation state tracking
            assert {[string match "*operations_init*" $info]}
            assert {[string match "*operations_queued*" $info]}
            assert {[string match "*operations_submitted*" $info]}
            assert {[string match "*operations_completed*" $info]}
        }
    }
    
    test {Team Member C - Operation Management: Priority queue management} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test priority queue statistics
            assert {[string match "*priority_low*" $info]}
            assert {[string match "*priority_normal*" $info]}
            assert {[string match "*priority_high*" $info]}
            assert {[string match "*priority_critical*" $info]}
            
            # Test average completion time tracking
            assert {[string match "*avg_completion_time_us*" $info]}
        }
    }
    
    test {Team Member C - Operation Management: Error handling and retry logic} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test error categorization
            assert {[string match "*operations_failed*" $info]}
            assert {[string match "*operations_cancelled*" $info]}
            assert {[string match "*operations_timeout*" $info]}
            
            # Test batch operation statistics
            assert {[string match "*uring_batch_submissions*" $info]}
            assert {[string match "*uring_single_submissions*" $info]}
        }
    }
    
    test {Team Member D - Testing Framework: Performance baseline establishment} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test performance metrics are available
            assert {[string match "*uring_max_completion_time_us*" $info]}
            assert {[string match "*uring_avg_completion_time_us*" $info]}
            
            # Establish baseline by performing operations
            set start_time [clock milliseconds]
            for {set i 0} {$i < 100} {incr i} {
                r set "perf_test_$i" "baseline_value_$i"
            }
            set end_time [clock milliseconds]
            set duration [expr {$end_time - $start_time}]
            
            # Verify operations completed in reasonable time
            assert {$duration < 1000} ;# Should complete in less than 1 second
            
            # Clean up
            for {set i 0} {$i < 100} {incr i} {
                r del "perf_test_$i"
            }
        }
    }
    
    test {Week 2 Integration: All components work together} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test that all Week 2 enhancements work together
            
            # Perform mixed operations to test integration
            r set integration_test "start"
            r lpush integration_list "item1" "item2" "item3"
            r hset integration_hash "field1" "value1" "field2" "value2"
            r sadd integration_set "member1" "member2" "member3"
            
            # Verify operations completed successfully
            assert_equal "start" [r get integration_test]
            assert_equal 3 [r llen integration_list]
            assert_equal 2 [r hlen integration_hash]
            assert_equal 3 [r scard integration_set]
            
            # Test that statistics were updated
            set final_info [r info uring]
            set ops_completed [extract_info_field $final_info "uring_ops_completed"]
            assert {$ops_completed > 0}
            
            # Clean up
            r del integration_test integration_list integration_hash integration_set
        }
    }
    
    test {Week 2 Stress Test: High load operation handling} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Stress test with many concurrent operations
            set num_ops 1000
            
            # Record initial statistics
            set initial_info [r info uring]
            set initial_ops [extract_info_field $initial_info "uring_ops_completed"]
            
            # Perform stress test
            for {set i 0} {$i < $num_ops} {incr i} {
                r set "stress_$i" "value_$i"
            }
            
            # Verify all operations completed
            for {set i 0} {$i < $num_ops} {incr i} {
                assert_equal "value_$i" [r get "stress_$i"]
            }
            
            # Check that operations were processed
            set final_info [r info uring]
            set final_ops [extract_info_field $final_info "uring_ops_completed"]
            assert {$final_ops > $initial_ops}
            
            # Clean up
            for {set i 0} {$i < $num_ops} {incr i} {
                r del "stress_$i"
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
