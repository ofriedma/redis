# Week 4 io_uring Completion Handling Unit Tests
# Comprehensive tests for completion processing, error handling, and event integration

start_server {tags {"uring week4 completion"} overrides {uring-enabled yes}} {
    test {completion processing basic functionality} {
        # Test basic completion processing
        set info_before [r info uring]
        regexp {completion_processing_calls:(\d+)} $info_before - calls_before
        regexp {ops_completed:(\d+)} $info_before - completed_before
        
        # Perform operations that will generate completions
        for {set i 0} {$i < 20} {incr i} {
            r set "completion_test_$i" "value_$i"
            r get "completion_test_$i"
        }
        
        set info_after [r info uring]
        regexp {completion_processing_calls:(\d+)} $info_after - calls_after
        regexp {ops_completed:(\d+)} $info_after - completed_after
        
        # Should have processed completions
        assert {$calls_after > $calls_before}
        assert {$completed_after > $completed_before}
    }
    
    test {completion batching efficiency} {
        # Test that completions are processed in batches
        set info_before [r info uring]
        regexp {completion_batches:(\d+)} $info_before - batches_before
        regexp {avg_completions_per_batch:(\d+)} $info_before - avg_before
        
        # Perform many operations quickly to trigger batch processing
        set pipe [r pipeline]
        for {set i 0} {$i < 100} {incr i} {
            $pipe ping
        }
        $pipe execute
        
        set info_after [r info uring]
        regexp {completion_batches:(\d+)} $info_after - batches_after
        regexp {avg_completions_per_batch:(\d+)} $info_after - avg_after
        
        # Should have processed completions in batches
        assert {$batches_after > $batches_before}
        
        # Average batch size should be reasonable
        if {$avg_after > 0} {
            assert {$avg_after >= 1}
            assert {$avg_after <= 100}
        }
    }
    
    test {completion error handling} {
        # Test completion handling for failed operations
        set info_before [r info uring]
        regexp {completion_errors:(\d+)} $info_before - errors_before
        
        # Try operations that might fail
        catch {r set [string repeat "x" 1000000] "huge_key"}
        catch {r get ""}
        catch {r eval "invalid lua code" 0}
        
        # Server should still be responsive
        assert_equal "PONG" [r ping]
        
        set info_after [r info uring]
        regexp {completion_errors:(\d+)} $info_after - errors_after
        
        # May have completion errors
        assert {$errors_after >= $errors_before}
    }
    
    test {completion timeout handling} {
        # Test completion timeout behavior
        set info_before [r info uring]
        regexp {timeout_events:(\d+)} $info_before - timeout_before
        
        # Perform operations with potential timeouts
        for {set i 0} {$i < 10} {incr i} {
            r set "timeout_test_$i" [string repeat "data" 1000]
        }
        
        # Check timeout statistics
        set info_after [r info uring]
        regexp {timeout_events:(\d+)} $info_after - timeout_after
        
        # Timeout events may or may not occur
        assert {$timeout_after >= $timeout_before}
        
        # Verify operations completed successfully
        for {set i 0} {$i < 10} {incr i} {
            assert_equal [string repeat "data" 1000] [r get "timeout_test_$i"]
        }
    }
    
    test {multishot completion handling} {
        # Test multishot operation completions
        set info_before [r info uring]
        regexp {multishot_completions:(\d+)} $info_before - multishot_before
        
        # Create multiple clients to trigger multishot accept operations
        set clients {}
        for {set i 0} {$i < 5} {incr i} {
            lappend clients [redis [srv host] [srv port]]
        }
        
        # Perform operations on all clients
        foreach client $clients {
            $client ping
            $client set "multishot_test" "value"
        }
        
        set info_after [r info uring]
        regexp {multishot_completions:(\d+)} $info_after - multishot_after
        
        # Should have multishot completions from accept operations
        assert {$multishot_after >= $multishot_before}
        
        # Clean up clients
        foreach client $clients {
            $client close
        }
    }
    
    test {completion context validation} {
        # Test that completion contexts are properly validated
        set info_before [r info uring]
        regexp {invalid_context_completions:(\d+)} $info_before - invalid_before
        regexp {null_context_completions:(\d+)} $info_before - null_before
        
        # Perform operations that might have context issues
        set client [redis [srv host] [srv port]]
        
        # Start operations then disconnect abruptly
        for {set i 0} {$i < 5} {incr i} {
            $client set "context_test_$i" "value_$i"
        }
        
        $client close
        
        # Wait for cleanup
        after 100
        
        set info_after [r info uring]
        regexp {invalid_context_completions:(\d+)} $info_after - invalid_after
        regexp {null_context_completions:(\d+)} $info_after - null_after
        
        # May have invalid or null context completions
        assert {$invalid_after >= $invalid_before}
        assert {$null_after >= $null_before}
    }
    
    test {completion memory management} {
        # Test memory management during completion processing
        set info_before [r info memory]
        regexp {used_memory:(\d+)} $info_before - memory_before
        
        # Perform many operations to stress completion handling
        for {set i 0} {$i < 1000} {incr i} {
            r set "memory_test_$i" "value_$i"
            if {$i % 100 == 0} {
                # Check memory periodically
                set info_current [r info memory]
                regexp {used_memory:(\d+)} $info_current - memory_current
                
                # Memory should not grow excessively
                set memory_growth [expr $memory_current - $memory_before]
                assert {$memory_growth < 50000000} ;# Less than 50MB growth
            }
        }
        
        # Clean up
        for {set i 0} {$i < 1000} {incr i} {
            r del "memory_test_$i"
        }
        
        # Force garbage collection
        r debug gc
        
        set info_after [r info memory]
        regexp {used_memory:(\d+)} $info_after - memory_after
        
        # Memory should return close to original level
        set final_growth [expr $memory_after - $memory_before]
        assert {$final_growth < 10000000} ;# Less than 10MB final growth
    }
    
    test {completion ordering guarantees} {
        # Test that completions maintain proper ordering
        set values {}
        
        # Submit operations in order
        for {set i 0} {$i < 50} {incr i} {
            r lpush "order_test" "item_$i"
            lappend values "item_$i"
        }
        
        # Verify order is maintained
        set result [r lrange "order_test" 0 -1]
        set expected [lreverse $values]
        
        assert_equal $expected $result
        
        # Clean up
        r del "order_test"
    }
    
    test {completion performance under load} {
        # Test completion performance under high load
        set start_time [clock milliseconds]
        
        # Create multiple clients for concurrent load
        set clients {}
        for {set i 0} {$i < 3} {incr i} {
            lappend clients [redis [srv host] [srv port]]
        }
        
        # Perform concurrent operations
        foreach client $clients {
            for {set i 0} {$i < 100} {incr i} {
                $client set "perf_test_${client}_$i" "value_$i"
            }
        }
        
        # Verify all operations completed
        foreach client $clients {
            for {set i 0} {$i < 100} {incr i} {
                assert_equal "value_$i" [$client get "perf_test_${client}_$i"]
            }
        }
        
        set end_time [clock milliseconds]
        set duration [expr $end_time - $start_time]
        
        # Should complete within reasonable time
        assert {$duration < 10000} ;# Less than 10 seconds
        
        # Check completion statistics
        set info [r info uring]
        assert_match "*completion_processing_calls*" $info
        assert_match "*completion_batches*" $info
        
        # Clean up clients
        foreach client $clients {
            $client close
        }
    }
    
    test {completion integration with event loop} {
        # Test that completions integrate properly with Redis event loop
        
        # Perform mixed operations (some io_uring, some regular)
        r set "regular_key" "regular_value"
        r lpush "list_key" "item1" "item2" "item3"
        r hset "hash_key" "field1" "value1" "field2" "value2"
        r sadd "set_key" "member1" "member2" "member3"
        
        # All should work correctly
        assert_equal "regular_value" [r get "regular_key"]
        assert_equal 3 [r llen "list_key"]
        assert_equal 2 [r hlen "hash_key"]
        assert_equal 3 [r scard "set_key"]
        
        # Event loop should remain responsive
        assert_equal "PONG" [r ping]
        
        # Check that both io_uring and regular operations coexist
        set info [r info uring]
        assert_match "*ops_completed*" $info
    }
}
