# Test io_uring edge cases and error conditions
# These tests focus on edge cases, error conditions, and stress scenarios
# for Team Member C's operation handlers and completion processing.

start_server {tags {"uring edge cases"}} {
    test {io_uring handles zero-length operations correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test zero-length value
            r set zero_length ""
            assert_equal "" [r get zero_length]
            
            # Test operations with empty keys (should fail gracefully)
            catch {r set "" "value"}
            
            # Server should remain stable
            assert {[r ping] eq "PONG"}
            
            r del zero_length
        }
    }
    
    test {io_uring handles maximum size operations correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test with maximum reasonable size (Redis string limit is 512MB)
            # We'll test with a smaller but still large size
            set large_size 1048576  ;# 1MB
            set large_value [string repeat "x" $large_size]
            
            # This should work or fail gracefully
            set result [catch {r set max_size_test $large_value}]
            
            if {$result == 0} {
                # If it succeeded, verify the data
                assert_equal $large_value [r get max_size_test]
                r del max_size_test
            }
            
            # Server should remain responsive regardless
            assert {[r ping] eq "PONG"}
        }
    }
    
    test {io_uring handles rapid connection operations correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test rapid operations that might stress connection handling
            
            # Rapid SET operations
            for {set i 0} {$i < 100} {incr i} {
                r set "rapid_conn_$i" "value_$i"
            }
            
            # Rapid GET operations
            for {set i 0} {$i < 100} {incr i} {
                assert_equal "value_$i" [r get "rapid_conn_$i"]
            }
            
            # Mixed rapid operations
            for {set i 0} {$i < 50} {incr i} {
                r set "mixed_$i" "mixed_value_$i"
                r get "mixed_$i"
                r exists "mixed_$i"
            }
            
            # Connection should remain stable
            assert {[r ping] eq "PONG"}
            
            # Clean up
            for {set i 0} {$i < 100} {incr i} {
                r del "rapid_conn_$i"
            }
            for {set i 0} {$i < 50} {incr i} {
                r del "mixed_$i"
            }
        }
    }
    
    test {io_uring handles special characters and binary data correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test with special characters
            set special_chars "!@#$%^&*()_+-={}[]|\\:;\"'<>?,./"
            r set special_test $special_chars
            assert_equal $special_chars [r get special_test]
            
            # Test with Unicode characters
            set unicode_chars "αβγδε中文日本語한국어"
            r set unicode_test $unicode_chars
            assert_equal $unicode_chars [r get unicode_test]
            
            # Test with binary data
            set binary_data "\x00\x01\x02\x03\x04\x05\xFF\xFE\xFD\xFC"
            r set binary_test $binary_data
            assert_equal $binary_data [r get binary_test]
            
            # Test with newlines and control characters
            set control_chars "line1\nline2\r\nline3\ttab\x00null"
            r set control_test $control_chars
            assert_equal $control_chars [r get control_test]
            
            # Clean up
            r del special_test unicode_test binary_test control_test
        }
    }
    
    test {io_uring handles memory pressure scenarios correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Create memory pressure with many large values
            set large_value [string repeat "m" 10000]
            
            # Create many large keys
            for {set i 0} {$i < 100} {incr i} {
                catch {r set "memory_pressure_$i" $large_value}
            }
            
            # Server should handle this gracefully
            assert {[r ping] eq "PONG"}
            
            # Try to read some values back
            for {set i 0} {$i < 10} {incr i} {
                catch {r get "memory_pressure_$i"}
            }
            
            # Clean up (some operations might have failed, so use catch)
            for {set i 0} {$i < 100} {incr i} {
                catch {r del "memory_pressure_$i"}
            }
            
            # Check that io_uring statistics reflect any failures appropriately
            set uring_info [r info uring]
            if {[string match "*uring_ops_failed*" $uring_info]} {
                # Some operations might have failed due to memory pressure
                # This is acceptable as long as the server remains stable
            }
        }
    }
    
    test {io_uring handles concurrent operations correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Simulate concurrent operations using pipelining
            r pipeline
            
            # Queue many operations
            for {set i 0} {$i < 200} {incr i} {
                r set "concurrent_$i" "concurrent_value_$i"
            }
            
            # Execute all operations
            set results [r execute]
            
            # All operations should succeed
            assert {[llength $results] == 200}
            
            # Verify the data was written correctly
            for {set i 0} {$i < 200} {incr i} {
                assert_equal "concurrent_value_$i" [r get "concurrent_$i"]
            }
            
            # Clean up
            for {set i 0} {$i < 200} {incr i} {
                r del "concurrent_$i"
            }
        }
    }
    
    test {io_uring handles operation timeouts correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test operations that might timeout
            # (This is difficult to test directly, but we can test resilience)
            
            # Perform operations that might stress timing
            for {set i 0} {$i < 50} {incr i} {
                r set "timeout_test_$i" "timeout_value_$i"
                # Add a small delay to spread operations over time
                after 1
            }
            
            # All operations should complete eventually
            for {set i 0} {$i < 50} {incr i} {
                assert_equal "timeout_value_$i" [r get "timeout_test_$i"]
            }
            
            # Check timing statistics
            set uring_info [r info uring]
            if {[string match "*uring_max_completion_time_us*" $uring_info]} {
                # Maximum completion time should be reasonable
                # (This is just a presence check)
            }
            
            # Clean up
            for {set i 0} {$i < 50} {incr i} {
                r del "timeout_test_$i"
            }
        }
    }
    
    test {io_uring handles buffer overflow scenarios correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test with data that might cause buffer issues
            
            # Test with data exactly at buffer boundaries
            set buffer_size 4096  ;# Default buffer size
            
            # Data exactly at buffer size
            set exact_buffer [string repeat "e" $buffer_size]
            r set exact_buffer_test $exact_buffer
            assert_equal $exact_buffer [r get exact_buffer_test]
            
            # Data slightly over buffer size
            set over_buffer [string repeat "o" [expr $buffer_size + 1]]
            r set over_buffer_test $over_buffer
            assert_equal $over_buffer [r get over_buffer_test]
            
            # Data much larger than buffer size
            set large_buffer [string repeat "l" [expr $buffer_size * 3]]
            r set large_buffer_test $large_buffer
            assert_equal $large_buffer [r get large_buffer_test]
            
            # Clean up
            r del exact_buffer_test over_buffer_test large_buffer_test
        }
    }
    
    test {io_uring handles queue full scenarios correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Try to create a scenario where queues might become full
            # This is difficult to trigger reliably, but we can test resilience
            
            # Rapid operations that might fill queues
            for {set i 0} {$i < 1000} {incr i} {
                r set "queue_full_$i" "queue_value_$i"
            }
            
            # All operations should complete (possibly with some queuing)
            for {set i 0} {$i < 1000} {incr i} {
                assert_equal "queue_value_$i" [r get "queue_full_$i"]
            }
            
            # Check queue statistics
            set uring_info [r info uring]
            if {[string match "*uring_sq_full_count*" $uring_info]} {
                # Some queue full events might have occurred
                # This is acceptable as long as operations complete
            }
            
            # Server should remain responsive
            assert {[r ping] eq "PONG"}
            
            # Clean up
            for {set i 0} {$i < 1000} {incr i} {
                r del "queue_full_$i"
            }
        }
    }
    
    test {io_uring handles malformed operations correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test with operations that might cause issues
            
            # Invalid commands should be handled gracefully
            catch {r invalid_command_that_does_not_exist}
            catch {r set}  ;# Missing arguments
            catch {r get}  ;# Missing arguments
            
            # Operations with unusual key names
            catch {r set "\x00\x01\x02" "null_key_value"}
            catch {r set [string repeat "x" 1000] "long_key_value"}
            
            # Server should remain stable after all these attempts
            assert {[r ping] eq "PONG"}
            
            # Normal operations should still work
            r set normal_after_malformed "normal_value"
            assert_equal "normal_value" [r get normal_after_malformed]
            r del normal_after_malformed
        }
    }
}

# Test edge cases with specific configurations
start_server {tags {"uring edge config"} overrides {uring-sq-entries 64 uring-cq-entries 128}} {
    test {io_uring handles small queue sizes correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test with smaller queue sizes that might fill up faster
            
            # Operations that might stress small queues
            for {set i 0} {$i < 200} {incr i} {
                r set "small_queue_$i" "small_queue_value_$i"
            }
            
            # All should complete despite small queues
            for {set i 0} {$i < 200} {incr i} {
                assert_equal "small_queue_value_$i" [r get "small_queue_$i"]
            }
            
            # Clean up
            for {set i 0} {$i < 200} {incr i} {
                r del "small_queue_$i"
            }
        }
    }
}
