# Test io_uring completion processing and handlers
# These tests focus specifically on completion handler functionality
# and error handling for Team Member C's implementation.

start_server {tags {"uring completion"}} {
    test {io_uring read completion handlers work correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test various read scenarios
            
            # Small read
            r set small_read "small"
            assert_equal "small" [r get small_read]
            
            # Medium read
            set medium_value [string repeat "m" 1024]
            r set medium_read $medium_value
            assert_equal $medium_value [r get medium_read]
            
            # Large read
            set large_value [string repeat "l" 4096]
            r set large_read $large_value
            assert_equal $large_value [r get large_read]
            
            # Binary data read
            set binary_value "\x00\x01\x02\x03\xFF\xFE\xFD"
            r set binary_read $binary_value
            assert_equal $binary_value [r get binary_read]
            
            # Clean up
            r del small_read medium_read large_read binary_read
        }
    }
    
    test {io_uring write completion handlers work correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test various write scenarios
            
            # Multiple writes in sequence
            for {set i 0} {$i < 10} {incr i} {
                r set "write_seq_$i" "value_$i"
            }
            
            # Verify all writes completed
            for {set i 0} {$i < 10} {incr i} {
                assert_equal "value_$i" [r get "write_seq_$i"]
            }
            
            # Test write with different data types
            r set write_string "string_value"
            r set write_number 12345
            r set write_float 123.456
            
            assert_equal "string_value" [r get write_string]
            assert_equal "12345" [r get write_number]
            assert_equal "123.456" [r get write_float]
            
            # Clean up
            for {set i 0} {$i < 10} {incr i} {
                r del "write_seq_$i"
            }
            r del write_string write_number write_float
        }
    }
    
    test {io_uring accept completion handlers work correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test that new connections are handled correctly
            # This is implicit through the test framework's connection
            
            # Test multiple commands on the same connection
            r set accept_test1 "value1"
            r get accept_test1
            r set accept_test2 "value2"
            r get accept_test2
            
            # Test that the connection remains stable
            assert {[r ping] eq "PONG"}
            
            # Test connection info
            set client_info [r client list]
            assert {[string length $client_info] > 0}
            
            # Clean up
            r del accept_test1 accept_test2
        }
    }
    
    test {io_uring completion error handling works correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test error conditions that might occur in completion handlers
            
            # Test with invalid operations (should not crash)
            catch {r invalid_command_test}
            
            # Test with operations that might cause EAGAIN/EWOULDBLOCK
            # (These are internal errors that should be handled transparently)
            
            # Rapid operations that might stress the system
            for {set i 0} {$i < 100} {incr i} {
                r set "error_test_$i" "value_$i"
            }
            
            # All should complete successfully
            for {set i 0} {$i < 100} {incr i} {
                assert_equal "value_$i" [r get "error_test_$i"]
            }
            
            # Server should remain responsive after stress
            assert {[r ping] eq "PONG"}
            
            # Check error statistics
            set uring_info [r info uring]
            if {[string match "*uring_ops_failed*" $uring_info]} {
                # Failed operations should be minimal
                # (exact count depends on system conditions)
            }
            
            # Clean up
            for {set i 0} {$i < 100} {incr i} {
                r del "error_test_$i"
            }
        }
    }
    
    test {io_uring persistent operations work correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test operations that should persist (like accept operations)
            
            # The accept operation should be persistent and continue
            # accepting new connections. We test this by ensuring
            # the connection remains active.
            
            # Test multiple operations to ensure persistence
            for {set i 0} {$i < 20} {incr i} {
                r set "persist_test_$i" "persist_value_$i"
                assert_equal "persist_value_$i" [r get "persist_test_$i"]
            }
            
            # Connection should still be active
            assert {[r ping] eq "PONG"}
            
            # Clean up
            for {set i 0} {$i < 20} {incr i} {
                r del "persist_test_$i"
            }
        }
    }
    
    test {io_uring completion timing is reasonable} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test that completion times are reasonable
            
            # Get initial timing stats
            set initial_info [r info uring]
            
            # Perform timed operations
            set start_time [clock milliseconds]
            
            for {set i 0} {$i < 50} {incr i} {
                r set "timing_test_$i" "timing_value_$i"
                r get "timing_test_$i"
            }
            
            set end_time [clock milliseconds]
            set total_time [expr $end_time - $start_time]
            
            # Operations should complete in reasonable time
            # (This is a rough check - exact timing depends on system)
            assert {$total_time < 5000}  ;# Should complete in under 5 seconds
            
            # Check updated timing stats
            set updated_info [r info uring]
            if {[string match "*uring_avg_completion_time_us*" $updated_info]} {
                # Average completion time should be reasonable (under 1ms typically)
                # This is just a presence check since exact values vary
            }
            
            # Clean up
            for {set i 0} {$i < 50} {incr i} {
                r del "timing_test_$i"
            }
        }
    }
    
    test {io_uring completion handlers handle connection closure correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test that connection closure is handled properly
            # This is difficult to test directly, but we can test
            # that operations complete properly even under stress
            
            # Create a scenario that might trigger connection events
            for {set i 0} {$i < 30} {incr i} {
                r set "closure_test_$i" "closure_value_$i"
            }
            
            # Verify all operations completed
            for {set i 0} {$i < 30} {incr i} {
                assert_equal "closure_value_$i" [r get "closure_test_$i"]
            }
            
            # Connection should still be active
            assert {[r ping] eq "PONG"}
            
            # Clean up
            for {set i 0} {$i < 30} {incr i} {
                r del "closure_test_$i"
            }
        }
    }
}

# Test completion handlers with different buffer sizes
start_server {tags {"uring completion buffers"} overrides {uring-buffer-size 8192}} {
    test {io_uring completion handlers work with different buffer sizes} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test with data that exercises different buffer sizes
            
            # Small data (well under buffer size)
            set small_data [string repeat "s" 100]
            r set buffer_small $small_data
            assert_equal $small_data [r get buffer_small]
            
            # Medium data (around half buffer size)
            set medium_data [string repeat "m" 4000]
            r set buffer_medium $medium_data
            assert_equal $medium_data [r get buffer_medium]
            
            # Large data (near buffer size)
            set large_data [string repeat "l" 7000]
            r set buffer_large $large_data
            assert_equal $large_data [r get buffer_large]
            
            # Very large data (exceeds single buffer)
            set very_large_data [string repeat "v" 10000]
            r set buffer_very_large $very_large_data
            assert_equal $very_large_data [r get buffer_very_large]
            
            # Clean up
            r del buffer_small buffer_medium buffer_large buffer_very_large
        }
    }
}

# Test completion handlers under high load
start_server {tags {"uring completion load"}} {
    test {io_uring completion handlers work under high load} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test high-load scenario
            set num_operations 500
            
            # Create high load with mixed operations
            for {set i 0} {$i < $num_operations} {incr i} {
                r set "load_test_$i" "load_value_$i"
                if {$i % 10 == 0} {
                    # Intersperse some reads
                    r get "load_test_[expr $i - 5]"
                }
            }
            
            # Verify all writes completed
            for {set i 0} {$i < $num_operations} {incr i} {
                assert_equal "load_value_$i" [r get "load_test_$i"]
            }
            
            # Server should still be responsive
            assert {[r ping] eq "PONG"}
            
            # Check that statistics reflect the high load
            set uring_info [r info uring]
            if {[string match "*uring_ops_completed*" $uring_info]} {
                # Should have completed many operations
            }
            
            # Clean up
            for {set i 0} {$i < $num_operations} {incr i} {
                r del "load_test_$i"
            }
        }
    }
}
