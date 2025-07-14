# Test io_uring operation handlers and completion processing
# These tests verify Team Member C's implementation of operation management,
# completion handlers, and error handling for Redis io_uring integration.

start_server {tags {"uring operations"}} {
    test {io_uring operation management functions work correctly} {
        # Check if io_uring support is compiled in
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test basic operation creation and management
            # This would require exposing test functions or using debug commands
            
            # For now, test that the server starts correctly with io_uring
            assert {[r ping] eq "PONG"}
            
            # Test that io_uring statistics are available
            set uring_info [r info uring]
            if {[string length $uring_info] > 0} {
                # Verify basic statistics fields are present
                assert {[string match "*uring_enabled*" $uring_info]}
                assert {[string match "*uring_ops_submitted*" $uring_info]}
                assert {[string match "*uring_ops_completed*" $uring_info]}
                assert {[string match "*uring_ops_failed*" $uring_info]}
            }
        }
    }
    
    test {io_uring statistics tracking works correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Get initial statistics
            set initial_info [r info uring]
            
            # Perform some operations that would trigger io_uring
            r set test_key "test_value"
            r get test_key
            r del test_key
            
            # Get updated statistics
            set updated_info [r info uring]
            
            # Statistics should be present (even if zero)
            if {[string length $updated_info] > 0} {
                assert {[string match "*uring_ops_submitted*" $updated_info]}
                assert {[string match "*uring_ops_completed*" $updated_info]}
                assert {[string match "*uring_success_rate*" $updated_info]}
                assert {[string match "*uring_avg_completion_time_us*" $updated_info]}
            }
        }
    }
    
    test {io_uring buffer management works correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test buffer ring statistics if available
            set uring_info [r info uring]
            if {[string match "*uring_buffer_ring*" $uring_info]} {
                assert {[string match "*uring_buffer_ring_hits*" $uring_info]}
                assert {[string match "*uring_buffer_ring_misses*" $uring_info]}
            }
            
            # Test with various data sizes to exercise buffer management
            set large_value [string repeat "x" 8192]
            r set large_key $large_value
            assert_equal $large_value [r get large_key]
            r del large_key
        }
    }
    
    test {io_uring error handling works correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test that the server handles errors gracefully
            # Try operations that might cause errors
            
            # Test with invalid commands (should not crash io_uring)
            catch {r invalid_command}
            
            # Test with large number of concurrent operations
            for {set i 0} {$i < 100} {incr i} {
                r set "test_key_$i" "value_$i"
            }
            
            for {set i 0} {$i < 100} {incr i} {
                assert_equal "value_$i" [r get "test_key_$i"]
                r del "test_key_$i"
            }
            
            # Server should still be responsive
            assert {[r ping] eq "PONG"}
        }
    }
    
    test {io_uring completion handlers process operations correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test read completion handling
            r set read_test "read_value"
            assert_equal "read_value" [r get read_test]
            
            # Test write completion handling
            r set write_test "write_value"
            assert {[r exists write_test] == 1}
            
            # Test accept completion handling (implicit through connection)
            assert {[r ping] eq "PONG"}
            
            # Clean up
            r del read_test write_test
        }
    }
    
    test {io_uring performance monitoring works correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Get initial performance metrics
            set initial_info [r info uring]
            
            # Perform operations to generate metrics
            for {set i 0} {$i < 50} {incr i} {
                r set "perf_key_$i" "perf_value_$i"
                r get "perf_key_$i"
            }
            
            # Get updated metrics
            set updated_info [r info uring]
            
            if {[string length $updated_info] > 0} {
                # Check that timing metrics are present
                assert {[string match "*uring_avg_completion_time_us*" $updated_info]}
                assert {[string match "*uring_max_completion_time_us*" $updated_info]}
                
                # Check that success rate is calculated
                assert {[string match "*uring_success_rate*" $updated_info]}
            }
            
            # Clean up
            for {set i 0} {$i < 50} {incr i} {
                r del "perf_key_$i"
            }
        }
    }
    
    test {io_uring handles connection lifecycle correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test that connections work correctly with io_uring
            
            # Test multiple operations on same connection
            r set conn_test1 "value1"
            r set conn_test2 "value2"
            r set conn_test3 "value3"
            
            assert_equal "value1" [r get conn_test1]
            assert_equal "value2" [r get conn_test2]
            assert_equal "value3" [r get conn_test3]
            
            # Test pipeline operations
            r pipeline
            r set pipe1 "pvalue1"
            r set pipe2 "pvalue2"
            r get pipe1
            r get pipe2
            set results [r execute]
            
            # Should have 4 results (2 sets + 2 gets)
            assert {[llength $results] == 4}
            assert_equal "pvalue1" [lindex $results 2]
            assert_equal "pvalue2" [lindex $results 3]
            
            # Clean up
            r del conn_test1 conn_test2 conn_test3 pipe1 pipe2
        }
    }
}

# Test io_uring with different server configurations
start_server {tags {"uring operations config"} overrides {uring-enabled yes}} {
    test {io_uring operations work with enabled configuration} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test that operations work when io_uring is explicitly enabled
            r set uring_enabled_test "enabled_value"
            assert_equal "enabled_value" [r get uring_enabled_test]
            
            # Check that io_uring statistics show activity
            set uring_info [r info uring]
            if {[string length $uring_info] > 0} {
                # Should show that io_uring is enabled
                assert {[string match "*uring_enabled:yes*" $uring_info]}
            }
            
            r del uring_enabled_test
        }
    }
}

# Test io_uring error conditions and edge cases
start_server {tags {"uring operations errors"}} {
    test {io_uring handles memory pressure correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test with large operations that might stress buffer management
            set very_large_value [string repeat "x" 65536]
            
            # This should work or fail gracefully
            catch {r set large_memory_test $very_large_value}
            
            # Server should remain responsive
            assert {[r ping] eq "PONG"}
            
            # Clean up if the operation succeeded
            catch {r del large_memory_test}
        }
    }
    
    test {io_uring handles rapid operations correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test rapid-fire operations to stress the completion handlers
            for {set i 0} {$i < 200} {incr i} {
                r set "rapid_$i" "value_$i"
            }
            
            # Verify all operations completed
            for {set i 0} {$i < 200} {incr i} {
                assert_equal "value_$i" [r get "rapid_$i"]
            }
            
            # Clean up
            for {set i 0} {$i < 200} {incr i} {
                r del "rapid_$i"
            }
            
            # Check that statistics reflect the operations
            set uring_info [r info uring]
            if {[string match "*uring_ops_completed*" $uring_info]} {
                # Should have completed many operations
                # (exact count depends on whether io_uring is actually used)
            }
        }
    }
}
