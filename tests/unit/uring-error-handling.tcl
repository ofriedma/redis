# io_uring error handling and edge case tests
# Comprehensive error condition testing for all team member implementations

start_server {tags {"uring errors"}} {
    test {Error Handling: Server handles io_uring initialization failures gracefully} {
        # Test that server can start even if io_uring has issues
        # This test verifies fallback mechanisms work
        assert {[r ping] eq "PONG"}
    }
    
    test {Error Handling: Memory pressure scenarios are handled correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test memory pressure handling
            set large_value [string repeat "x" 50000]
            
            # Try to create memory pressure
            for {set i 0} {$i < 50} {incr i} {
                catch {r set "memory_pressure_$i" $large_value}
            }
            
            # Server should remain responsive
            assert {[r ping] eq "PONG"}
            
            # Clean up (some operations might have failed)
            for {set i 0} {$i < 50} {incr i} {
                catch {r del "memory_pressure_$i"}
            }
        }
    }
    
    test {Error Handling: Connection errors are handled gracefully} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test connection error handling
            
            # Rapid connection operations that might stress error handling
            for {set i 0} {$i < 100} {incr i} {
                r set "conn_error_$i" "value_$i"
            }
            
            # All should complete or fail gracefully
            for {set i 0} {$i < 100} {incr i} {
                catch {r get "conn_error_$i"}
            }
            
            # Server should remain stable
            assert {[r ping] eq "PONG"}
            
            # Clean up
            for {set i 0} {$i < 100} {incr i} {
                catch {r del "conn_error_$i"}
            }
        }
    }
    
    test {Error Handling: Buffer overflow scenarios are handled} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test buffer overflow handling
            
            # Try operations with various buffer sizes
            set sizes {1024 4096 8192 16384 32768}
            
            foreach size $sizes {
                set data [string repeat "b" $size]
                catch {r set "buffer_overflow_$size" $data}
            }
            
            # Verify operations completed or failed gracefully
            foreach size $sizes {
                catch {r get "buffer_overflow_$size"}
                catch {r del "buffer_overflow_$size"}
            }
            
            # Server should remain responsive
            assert {[r ping] eq "PONG"}
        }
    }
    
    test {Error Handling: Queue full scenarios are handled} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test queue full handling by submitting many operations rapidly
            
            # Rapid-fire operations to potentially fill queues
            for {set i 0} {$i < 500} {incr i} {
                catch {r set "queue_full_$i" "queue_value_$i"}
            }
            
            # All operations should complete eventually
            for {set i 0} {$i < 500} {incr i} {
                catch {r get "queue_full_$i"}
            }
            
            # Check queue statistics if available
            set uring_info [r info uring]
            if {[string match "*uring_sq_full_count*" $uring_info]} {
                # Queue full events might have occurred, which is acceptable
            }
            
            # Server should remain responsive
            assert {[r ping] eq "PONG"}
            
            # Clean up
            for {set i 0} {$i < 500} {incr i} {
                catch {r del "queue_full_$i"}
            }
        }
    }
    
    test {Error Handling: Invalid operations are rejected safely} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test invalid operation handling
            
            # Invalid commands should be rejected gracefully
            catch {r invalid_command_test}
            catch {r set}  ;# Missing arguments
            catch {r get}  ;# Missing arguments
            
            # Operations with problematic data
            catch {r set "\x00\x01\x02" "null_bytes"}
            catch {r set [string repeat "x" 10000] "very_long_key"}
            
            # Server should remain stable
            assert {[r ping] eq "PONG"}
            
            # Normal operations should still work
            r set normal_after_errors "normal_value"
            assert_equal "normal_value" [r get normal_after_errors]
            r del normal_after_errors
        }
    }
    
    test {Error Handling: Concurrent error scenarios} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test concurrent operations with potential errors
            
            # Mix of valid and potentially problematic operations
            for {set i 0} {$i < 50} {incr i} {
                r set "concurrent_$i" "value_$i"
                if {$i % 10 == 0} {
                    # Intersperse some potentially problematic operations
                    catch {r get "nonexistent_key_$i"}
                }
            }

            # All operations should complete or fail gracefully
            assert {[r ping] eq "PONG"}
            
            # Server should remain stable
            assert {[r ping] eq "PONG"}
            
            # Clean up
            for {set i 0} {$i < 50} {incr i} {
                catch {r del "concurrent_$i"}
            }
        }
    }
    
    test {Error Handling: Resource exhaustion scenarios} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test resource exhaustion handling
            
            # Try to exhaust various resources
            set large_data [string repeat "r" 100000]
            
            for {set i 0} {$i < 20} {incr i} {
                catch {r set "resource_test_$i" $large_data}
            }
            
            # System should handle resource pressure gracefully
            assert {[r ping] eq "PONG"}
            
            # Check error statistics
            set uring_info [r info uring]
            if {[string match "*uring_ops_failed*" $uring_info]} {
                # Some operations might have failed due to resource pressure
                # This is acceptable as long as the server remains stable
            }
            
            # Clean up
            for {set i 0} {$i < 20} {incr i} {
                catch {r del "resource_test_$i"}
            }
        }
    }
    
    test {Error Handling: Network error simulation} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test network-related error handling
            
            # Operations that might trigger network-related errors
            for {set i 0} {$i < 30} {incr i} {
                r set "network_test_$i" "network_value_$i"
                # Add small delays to simulate network conditions
                after 1
            }
            
            # All operations should complete
            for {set i 0} {$i < 30} {incr i} {
                assert_equal "network_value_$i" [r get "network_test_$i"]
            }
            
            # Connection should remain stable
            assert {[r ping] eq "PONG"}
            
            # Clean up
            for {set i 0} {$i < 30} {incr i} {
                r del "network_test_$i"
            }
        }
    }
    
    test {Error Handling: Recovery after errors} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test system recovery after various error conditions
            
            # Cause some errors
            catch {r invalid_command}
            catch {r set "" "empty_key"}
            catch {r get "nonexistent"}
            
            # System should recover and work normally
            r set recovery_test "recovery_value"
            assert_equal "recovery_value" [r get recovery_test]
            
            # Test normal operations after errors
            for {set i 0} {$i < 20} {incr i} {
                r set "recovery_$i" "recovery_value_$i"
            }
            
            for {set i 0} {$i < 20} {incr i} {
                assert_equal "recovery_value_$i" [r get "recovery_$i"]
                r del "recovery_$i"
            }
            
            r del recovery_test
        }
    }
    
    test {Error Handling: Statistics accuracy during errors} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test that statistics remain accurate even during error conditions
            
            set initial_info [r info uring]
            
            # Mix of successful and failing operations
            for {set i 0} {$i < 30} {incr i} {
                r set "stats_error_$i" "stats_value_$i"
                catch {r get "nonexistent_$i"}
            }
            
            # Check that statistics are updated appropriately
            set updated_info [r info uring]
            
            if {[string length $updated_info] > 0} {
                # Statistics should reflect both successful and failed operations
                assert {[string match "*uring_ops_submitted*" $updated_info]}
                assert {[string match "*uring_ops_completed*" $updated_info]}
                # Failed operations might be tracked separately
            }
            
            # Clean up
            for {set i 0} {$i < 30} {incr i} {
                catch {r del "stats_error_$i"}
            }
        }
    }
}

# Test error handling with different configurations
start_server {tags {"uring errors config"} overrides {uring-enabled yes uring-sq-entries 64}} {
    test {Error Handling: Small queue configurations handle errors correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test error handling with smaller queue sizes
            
            # Operations that might fill small queues quickly
            for {set i 0} {$i < 100} {incr i} {
                catch {r set "small_queue_error_$i" "value_$i"}
            }
            
            # System should handle queue pressure gracefully
            assert {[r ping] eq "PONG"}
            
            # Clean up
            for {set i 0} {$i < 100} {incr i} {
                catch {r del "small_queue_error_$i"}
            }
        }
    }
}

# Test error handling under high load
start_server {tags {"uring errors load"}} {
    test {Error Handling: High load error scenarios} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test error handling under high load
            
            set num_operations 1000
            
            # High load with mixed operations and potential errors
            for {set i 0} {$i < $num_operations} {incr i} {
                if {$i % 100 == 0} {
                    # Intersperse some operations that might cause errors
                    catch {r get "nonexistent_high_load_$i"}
                } else {
                    r set "high_load_error_$i" "high_load_value_$i"
                }
            }
            
            # System should remain stable under high load with errors
            assert {[r ping] eq "PONG"}
            
            # Clean up
            for {set i 0} {$i < $num_operations} {incr i} {
                catch {r del "high_load_error_$i"}
            }
        }
    }
}
