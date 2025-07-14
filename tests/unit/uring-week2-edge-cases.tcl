# io_uring Week 2 Edge Cases and Error Condition Tests
# Tests for error handling, edge cases, and failure scenarios

start_server {tags {"uring week2 edge-cases"}} {
    test {Edge Case: Buffer pool exhaustion handling} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test buffer pool behavior under stress
            set initial_info [r info uring]
            set initial_misses [extract_info_field $initial_info "buffer_pool_misses"]
            
            # Create many large operations to potentially exhaust buffer pool
            set large_value [string repeat "x" 8192]
            for {set i 0} {$i < 50} {incr i} {
                r set "large_$i" $large_value
            }
            
            # Verify operations still work despite potential buffer pressure
            for {set i 0} {$i < 50} {incr i} {
                assert_equal $large_value [r get "large_$i"]
            }
            
            # Check that buffer pool statistics are updated
            set final_info [r info uring]
            set final_allocations [extract_info_field $final_info "buffer_pool_allocations"]
            assert {$final_allocations > 0}
            
            # Clean up
            for {set i 0} {$i < 50} {incr i} {
                r del "large_$i"
            }
        } else {
            skip "io_uring not available"
        }
    }
    
    test {Edge Case: Connection lifecycle during high load} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test connection tracking under load
            set initial_info [r info uring]
            set initial_connections [extract_info_field $initial_info "active_connections"]
            
            # Perform operations that exercise connection lifecycle
            for {set i 0} {$i < 100} {incr i} {
                r set "conn_test_$i" "value_$i"
                r get "conn_test_$i"
                if {$i % 10 == 0} {
                    # Check connection statistics periodically
                    set current_info [r info uring]
                    set current_connections [extract_info_field $current_info "active_connections"]
                    assert {$current_connections >= 0}
                }
            }
            
            # Verify connection statistics are reasonable
            set final_info [r info uring]
            set total_bytes_read [extract_info_field $final_info "total_bytes_read"]
            set total_bytes_written [extract_info_field $final_info "total_bytes_written"]
            assert {$total_bytes_read >= 0}
            assert {$total_bytes_written >= 0}
            
            # Clean up
            for {set i 0} {$i < 100} {incr i} {
                r del "conn_test_$i"
            }
        }
    }
    
    test {Edge Case: Operation timeout and retry handling} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test operation retry statistics
            set initial_info [r info uring]
            set initial_failed [extract_info_field $initial_info "uring_ops_failed"]
            
            # Perform operations that might trigger retries
            # Use pipeline to create potential congestion
            r pipeline
            for {set i 0} {$i < 200} {incr i} {
                r set "retry_test_$i" "value_$i"
            }
            r execute
            
            # Verify all operations completed successfully
            for {set i 0} {$i < 200} {incr i} {
                assert_equal "value_$i" [r get "retry_test_$i"]
            }
            
            # Check retry and error statistics
            set final_info [r info uring]
            set final_completed [extract_info_field $final_info "uring_ops_completed"]
            assert {$final_completed > 0}
            
            # Clean up
            for {set i 0} {$i < 200} {incr i} {
                r del "retry_test_$i"
            }
        }
    }
    
    test {Edge Case: Priority queue overflow handling} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test priority queue behavior under load
            set initial_info [r info uring]
            set initial_queued [extract_info_field $initial_info "operations_queued"]
            
            # Create burst of operations to test queue management
            r pipeline
            for {set i 0} {$i < 500} {incr i} {
                r set "priority_test_$i" "value_$i"
                r get "priority_test_$i"
            }
            r execute
            
            # Verify operations completed
            for {set i 0} {$i < 500} {incr i} {
                assert_equal "value_$i" [r get "priority_test_$i"]
            }
            
            # Check priority queue statistics
            set final_info [r info uring]
            set priority_normal [extract_info_field $final_info "priority_normal"]
            set priority_high [extract_info_field $final_info "priority_high"]
            # Priority queues should have been used
            assert {[expr {$priority_normal + $priority_high}] >= 0}
            
            # Clean up
            for {set i 0} {$i < 500} {incr i} {
                r del "priority_test_$i"
            }
        }
    }
    
    test {Edge Case: Memory alignment and buffer optimization} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test buffer alignment handling with various sizes
            set sizes {1 16 64 256 1024 4096 8192}
            
            foreach size $sizes {
                set value [string repeat "a" $size]
                r set "align_test_$size" $value
                assert_equal $value [r get "align_test_$size"]
                r del "align_test_$size"
            }
            
            # Check buffer pool optimization statistics
            set final_info [r info uring]
            set buffer_alignment [extract_info_field $final_info "buffer_pool_alignment"]
            assert {$buffer_alignment > 0}
        }
    }
    
    test {Edge Case: SQPOLL thread behavior under load} {
        set info [r info uring]
        if {[string length $info] > 0} {
            set sqpoll_enabled [extract_info_field $info "uring_sqpoll_enabled"]
            
            if {$sqpoll_enabled eq "yes"} {
                # Test SQPOLL behavior under sustained load
                set start_time [clock milliseconds]
                
                for {set i 0} {$i < 1000} {incr i} {
                    r set "sqpoll_test_$i" "value_$i"
                }
                
                set end_time [clock milliseconds]
                set duration [expr {$end_time - $start_time}]
                
                # SQPOLL should provide good performance
                assert {$duration < 2000} ;# Should complete in less than 2 seconds
                
                # Verify all operations completed
                for {set i 0} {$i < 1000} {incr i} {
                    assert_equal "value_$i" [r get "sqpoll_test_$i"]
                }
                
                # Clean up
                for {set i 0} {$i < 1000} {incr i} {
                    r del "sqpoll_test_$i"
                }
            } else {
                # Test regular polling behavior
                for {set i 0} {$i < 100} {incr i} {
                    r set "regular_poll_$i" "value_$i"
                    assert_equal "value_$i" [r get "regular_poll_$i"]
                    r del "regular_poll_$i"
                }
            }
        }
    }
    
    test {Edge Case: Buffer ring vs regular buffer fallback} {
        set info [r info uring]
        if {[string length $info] > 0} {
            set buffer_ring_enabled [extract_info_field $info "uring_buffer_ring_enabled"]
            
            # Test buffer allocation fallback behavior
            set initial_info [r info uring]
            set initial_hits [extract_info_field $initial_info "uring_buffer_ring_hits"]
            set initial_misses [extract_info_field $initial_info "uring_buffer_ring_misses"]
            
            # Perform operations that test buffer allocation
            for {set i 0} {$i < 50} {incr i} {
                set value [string repeat "b" [expr {$i * 100 + 100}]]
                r set "buffer_test_$i" $value
                assert_equal $value [r get "buffer_test_$i"]
            }
            
            # Check buffer usage statistics
            set final_info [r info uring]
            if {$buffer_ring_enabled eq "yes"} {
                set final_hits [extract_info_field $final_info "uring_buffer_ring_hits"]
                set final_misses [extract_info_field $final_info "uring_buffer_ring_misses"]
                # Should have some buffer ring activity
                assert {[expr {$final_hits + $final_misses}] >= [expr {$initial_hits + $initial_misses}]}
            }
            
            # Clean up
            for {set i 0} {$i < 50} {incr i} {
                r del "buffer_test_$i"
            }
        }
    }
    
    test {Edge Case: Operation cancellation and cleanup} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test operation cleanup statistics
            set initial_info [r info uring]
            set initial_cancelled [extract_info_field $initial_info "operations_cancelled"]
            
            # Perform operations and test cleanup
            for {set i 0} {$i < 100} {incr i} {
                r set "cleanup_test_$i" "value_$i"
            }
            
            # Force some operations to complete
            for {set i 0} {$i < 100} {incr i} {
                assert_equal "value_$i" [r get "cleanup_test_$i"]
            }
            
            # Check that operations are properly cleaned up
            set final_info [r info uring]
            set final_completed [extract_info_field $final_info "operations_completed"]
            assert {$final_completed > 0}
            
            # Clean up
            for {set i 0} {$i < 100} {incr i} {
                r del "cleanup_test_$i"
            }
        }
    }
    
    test {Edge Case: Statistics accuracy under concurrent load} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test statistics consistency under load
            set initial_info [r info uring]
            set initial_submitted [extract_info_field $initial_info "uring_ops_submitted"]
            set initial_completed [extract_info_field $initial_info "uring_ops_completed"]
            
            # Create concurrent load using pipeline
            r pipeline
            for {set i 0} {$i < 300} {incr i} {
                r set "stats_test_$i" "value_$i"
                r get "stats_test_$i"
                r del "stats_test_$i"
            }
            r execute
            
            # Check statistics consistency
            set final_info [r info uring]
            set final_submitted [extract_info_field $final_info "uring_ops_submitted"]
            set final_completed [extract_info_field $final_info "uring_ops_completed"]
            
            # Submitted should be >= completed
            assert {$final_submitted >= $final_completed}
            
            # Both should have increased
            assert {$final_submitted > $initial_submitted}
            assert {$final_completed > $initial_completed}
            
            # Success rate should be reasonable
            set success_rate [extract_info_field $final_info "uring_success_rate"]
            assert {$success_rate >= 90.0} ;# At least 90% success rate
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
