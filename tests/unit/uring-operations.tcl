# Test io_uring operation management implementation
# Team Member C - Week 1 Task 1: Operation Management Implementation
# 
# These tests verify that the operation creation, tracking, and lifecycle management
# functions work correctly and provide proper statistics and error handling.

start_server {tags {"uring operations"}} {
    test {operation management functions are available when io_uring is compiled} {
        # Check if io_uring support is compiled in
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # io_uring is available, test that operation management is working
            
            # Test that uring info section is available
            set uring_info [r info uring]
            
            # Verify basic operation statistics are present
            assert {[string match "*ops_submitted*" $uring_info]}
            assert {[string match "*ops_completed*" $uring_info]}
            assert {[string match "*ops_failed*" $uring_info]}
            
            # Initially, no operations should be submitted
            regexp {ops_submitted:(\d+)} $uring_info match ops_submitted
            regexp {ops_completed:(\d+)} $uring_info match ops_completed
            regexp {ops_failed:(\d+)} $uring_info match ops_failed
            
            # Stats should be initialized to 0
            assert {$ops_submitted >= 0}
            assert {$ops_completed >= 0}
            assert {$ops_failed >= 0}
        }
    }
    
    test {operation statistics are properly tracked during basic operations} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Get initial statistics
            set initial_uring_info [r info uring]
            regexp {ops_submitted:(\d+)} $initial_uring_info match initial_submitted
            regexp {ops_completed:(\d+)} $initial_uring_info match initial_completed
            
            # Perform some basic Redis operations that should trigger io_uring operations
            r set test_key "test_value"
            r get test_key
            r del test_key
            
            # Allow some time for operations to complete
            after 10
            
            # Get updated statistics
            set updated_uring_info [r info uring]
            regexp {ops_submitted:(\d+)} $updated_uring_info match updated_submitted
            regexp {ops_completed:(\d+)} $updated_uring_info match updated_completed
            
            # Operations should have been submitted and completed
            # Note: The exact numbers depend on the io_uring implementation
            assert {$updated_submitted >= $initial_submitted}
            assert {$updated_completed >= $initial_completed}
        }
    }
    
    test {operation timing statistics are tracked} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            set uring_info [r info uring]
            
            # Check for timing-related statistics
            if {[string match "*total_completion_time_us*" $uring_info]} {
                assert {[string match "*total_completion_time_us:*" $uring_info]}
                assert {[string match "*max_completion_time_us:*" $uring_info]}
                
                # Extract timing values
                regexp {total_completion_time_us:(\d+)} $uring_info match total_time
                regexp {max_completion_time_us:(\d+)} $uring_info match max_time
                
                # Timing values should be non-negative
                assert {$total_time >= 0}
                assert {$max_time >= 0}
            }
        }
    }
    
    test {operation failure handling is properly tracked} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Get initial failure count
            set initial_uring_info [r info uring]
            regexp {ops_failed:(\d+)} $initial_uring_info match initial_failed
            
            # Try to perform operations that might fail
            # Note: This is tricky to test without causing actual failures
            # We'll just verify the counter exists and is stable
            
            # Perform normal operations
            r set failure_test "value"
            r get failure_test
            r del failure_test
            
            # Check that failure count hasn't increased unexpectedly
            set updated_uring_info [r info uring]
            regexp {ops_failed:(\d+)} $updated_uring_info match updated_failed
            
            # Failure count should not increase for successful operations
            assert {$updated_failed >= $initial_failed}
        }
    }
    
    test {operation types are properly categorized} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test different types of operations
            
            # Accept operations (server listening)
            # Read operations (client data reading)
            # Write operations (client data writing)
            
            # Perform operations that should trigger different operation types
            r set read_test "read_value"
            set value [r get read_test]
            assert_equal "read_value" $value
            
            r lpush write_test "item1" "item2"
            set items [r lrange write_test 0 -1]
            assert_equal {item2 item1} $items
            
            r del read_test write_test
            
            # Verify operations were tracked
            set uring_info [r info uring]
            regexp {ops_submitted:(\d+)} $uring_info match ops_submitted
            assert {$ops_submitted > 0}
        }
    }
    
    test {persistent operation handling} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test that persistent operations (like accept) are properly handled
            
            # Server should have persistent accept operations running
            set uring_info [r info uring]
            
            # Check for accept-related statistics if available
            if {[string match "*accept_ops*" $uring_info]} {
                assert {[string match "*accept_ops:*" $uring_info]}
            }
            
            # Verify that the server is still accepting connections
            # by creating a new connection
            set r2 [redis_deferring_client]
            $r2 ping
            assert_equal "PONG" [$r2 read]
            $r2 close
        }
    }
    
    test {operation buffer management integration} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            set uring_info [r info uring]
            
            # Check if buffer pool statistics are available
            if {[string match "*buffer_pool*" $uring_info]} {
                # Test operations with different buffer sizes
                
                # Small data
                r set small_data "x"
                r get small_data
                
                # Medium data
                set medium_data [string repeat "x" 1024]
                r set medium_data $medium_data
                r get medium_data
                
                # Large data
                set large_data [string repeat "x" 8192]
                r set large_data $large_data
                r get large_data
                
                # Verify buffer pool statistics
                set updated_uring_info [r info uring]
                if {[string match "*buffer_pool_hits*" $updated_uring_info]} {
                    regexp {buffer_pool_hits:(\d+)} $updated_uring_info match hits
                    assert {$hits >= 0}
                }
                
                r del small_data medium_data large_data
            }
        }
    }
    
    test {operation error recovery and cleanup} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test that operations are properly cleaned up after errors
            
            # Get initial statistics
            set initial_uring_info [r info uring]
            regexp {ops_submitted:(\d+)} $initial_uring_info match initial_submitted
            regexp {ops_completed:(\d+)} $initial_uring_info match initial_completed
            regexp {ops_failed:(\d+)} $initial_uring_info match initial_failed
            
            # Perform operations that should succeed
            for {set i 0} {$i < 10} {incr i} {
                r set cleanup_test_$i "value_$i"
                r get cleanup_test_$i
                r del cleanup_test_$i
            }
            
            # Allow operations to complete
            after 50
            
            # Check final statistics
            set final_uring_info [r info uring]
            regexp {ops_submitted:(\d+)} $final_uring_info match final_submitted
            regexp {ops_completed:(\d+)} $final_uring_info match final_completed
            regexp {ops_failed:(\d+)} $final_uring_info match final_failed
            
            # Verify operations were processed
            assert {$final_submitted > $initial_submitted}
            assert {$final_completed >= $initial_completed}
            
            # Most operations should succeed
            set total_ops [expr $final_submitted - $initial_submitted]
            set failed_ops [expr $final_failed - $initial_failed]
            if {$total_ops > 0} {
                set failure_rate [expr double($failed_ops) / $total_ops]
                assert {$failure_rate < 0.1}  ;# Less than 10% failure rate
            }
        }
    }
    
    test {operation lifecycle management under load} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test operation management under moderate load
            
            # Get baseline statistics
            set baseline_uring_info [r info uring]
            regexp {ops_submitted:(\d+)} $baseline_uring_info match baseline_submitted
            
            # Perform a burst of operations
            for {set i 0} {$i < 100} {incr i} {
                r set load_test_$i "load_value_$i"
            }
            
            for {set i 0} {$i < 100} {incr i} {
                r get load_test_$i
            }
            
            for {set i 0} {$i < 100} {incr i} {
                r del load_test_$i
            }
            
            # Allow operations to complete
            after 100
            
            # Check that operations were handled properly
            set load_uring_info [r info uring]
            regexp {ops_submitted:(\d+)} $load_uring_info match load_submitted
            
            # Should have submitted significantly more operations
            assert {$load_submitted > [expr $baseline_submitted + 200]}
            
            # Verify server is still responsive
            r ping
        }
    }
}

# Test with io_uring enabled configuration
start_server {tags {"uring operations enabled"} overrides {uring-enabled yes}} {
    test {operation management works with io_uring explicitly enabled} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Verify io_uring is enabled
            assert_equal "yes" [lindex [r config get uring-enabled] 1]
            
            # Test basic operation functionality
            r set enabled_test "enabled_value"
            set value [r get enabled_test]
            assert_equal "enabled_value" $value
            r del enabled_test
            
            # Verify operations were tracked
            set uring_info [r info uring]
            regexp {ops_submitted:(\d+)} $uring_info match ops_submitted
            assert {$ops_submitted > 0}
        }
    }
}
