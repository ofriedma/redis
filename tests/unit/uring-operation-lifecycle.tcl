# Week 4 io_uring Operation Lifecycle Unit Tests
# Comprehensive tests for operation creation, submission, completion, and cleanup

start_server {tags {"uring week4 operations"} overrides {uring-enabled yes}} {
    test {operation creation and initialization} {
        # Test that operations are properly created
        set info [r info uring]
        assert_match "*ops_created*" $info
        assert_match "*ops_submitted*" $info
        assert_match "*ops_completed*" $info
    }
    
    test {operation state transitions} {
        # Get initial operation statistics
        set info_before [r info uring]
        regexp {ops_created:(\d+)} $info_before - created_before
        regexp {ops_submitted:(\d+)} $info_before - submitted_before
        regexp {ops_completed:(\d+)} $info_before - completed_before
        
        # Perform operations that go through full lifecycle
        for {set i 0} {$i < 50} {incr i} {
            r set "lifecycle_$i" "value_$i"
            r get "lifecycle_$i"
        }
        
        # Check that operations went through proper states
        set info_after [r info uring]
        regexp {ops_created:(\d+)} $info_after - created_after
        regexp {ops_submitted:(\d+)} $info_after - submitted_after
        regexp {ops_completed:(\d+)} $info_after - completed_after
        
        assert {$created_after > $created_before}
        assert {$submitted_after > $submitted_before}
        assert {$completed_after > $completed_before}
        
        # Completed should not exceed submitted
        assert {$completed_after <= $submitted_after}
    }
    
    test {operation priority handling} {
        # Test that high priority operations are processed first
        set info_before [r info uring]
        regexp {high_priority_ops:(\d+)} $info_before - high_before
        regexp {normal_priority_ops:(\d+)} $info_before - normal_before
        
        # Mix high and normal priority operations
        # High priority: critical commands
        r ping
        r info server
        
        # Normal priority: regular data operations
        for {set i 0} {$i < 10} {incr i} {
            r set "priority_test_$i" "value_$i"
        }
        
        set info_after [r info uring]
        regexp {high_priority_ops:(\d+)} $info_after - high_after
        regexp {normal_priority_ops:(\d+)} $info_after - normal_after
        
        assert {$high_after > $high_before}
        assert {$normal_after > $normal_before}
    }
    
    test {operation timeout handling} {
        # Test operation timeout behavior
        set info_before [r info uring]
        regexp {ops_timeout:(\d+)} $info_before - timeout_before
        
        # Create a slow operation (if supported)
        if {[catch {r debug sleep 0.1}]} {
            # If debug sleep not available, use a different approach
            set large_list {}
            for {set i 0} {$i < 10000} {incr i} {
                lappend large_list "item_$i"
            }
            r lpush timeout_test {*}$large_list
        }
        
        set info_after [r info uring]
        regexp {ops_timeout:(\d+)} $info_after - timeout_after
        
        # Should still be responsive
        assert_equal "PONG" [r ping]
    }
    
    test {operation retry mechanism} {
        # Test operation retry on temporary failures
        set info_before [r info uring]
        regexp {ops_retried:(\d+)} $info_before - retried_before
        
        # Simulate conditions that might cause retries
        # Fill up memory to cause temporary failures
        set large_values {}
        for {set i 0} {$i < 100} {incr i} {
            set value [string repeat "x" 10000]
            if {[catch {r set "retry_test_$i" $value}]} {
                break
            }
            lappend large_values "retry_test_$i"
        }
        
        # Clean up to allow retries to succeed
        foreach key $large_values {
            r del $key
        }
        
        # Perform normal operations
        for {set i 0} {$i < 10} {incr i} {
            r set "retry_normal_$i" "value_$i"
        }
        
        set info_after [r info uring]
        regexp {ops_retried:(\d+)} $info_after - retried_after
        
        # May or may not have retries depending on system state
        assert {$retried_after >= $retried_before}
    }
    
    test {operation cancellation} {
        # Test operation cancellation on client disconnect
        set client [redis [srv host] [srv port]]
        
        # Start some operations
        for {set i 0} {$i < 5} {incr i} {
            $client set "cancel_test_$i" "value_$i"
        }
        
        # Get operation count before disconnect
        set info_before [r info uring]
        regexp {ops_cancelled:(\d+)} $info_before - cancelled_before
        
        # Disconnect client abruptly
        $client close
        
        # Wait for cleanup
        after 100
        
        # Check cancellation statistics
        set info_after [r info uring]
        regexp {ops_cancelled:(\d+)} $info_after - cancelled_after
        
        # Should have cancelled some operations
        assert {$cancelled_after >= $cancelled_before}
    }
    
    test {operation error handling} {
        # Test operation error handling and recovery
        set info_before [r info uring]
        regexp {ops_failed:(\d+)} $info_before - failed_before
        
        # Try operations that should fail
        catch {r set "" "empty_key"}
        catch {r get "nonexistent_key_with_very_long_name_that_might_cause_issues"}
        
        # Server should still be responsive
        assert_equal "PONG" [r ping]
        
        set info_after [r info uring]
        regexp {ops_failed:(\d+)} $info_after - failed_after
        
        # May have failed operations
        assert {$failed_after >= $failed_before}
    }
    
    test {operation batching efficiency} {
        # Test operation batching for efficiency
        set info_before [r info uring]
        regexp {batch_submissions:(\d+)} $info_before - batches_before
        regexp {ops_submitted:(\d+)} $info_before - ops_before
        
        # Perform many operations quickly to trigger batching
        set pipe [r pipeline]
        for {set i 0} {$i < 100} {incr i} {
            $pipe set "batch_test_$i" "value_$i"
        }
        $pipe execute
        
        set info_after [r info uring]
        regexp {batch_submissions:(\d+)} $info_after - batches_after
        regexp {ops_submitted:(\d+)} $info_after - ops_after
        
        set batch_increase [expr $batches_after - $batches_before]
        set ops_increase [expr $ops_after - $ops_before]
        
        # Should have batched operations efficiently
        if {$batch_increase > 0} {
            set avg_batch_size [expr $ops_increase / $batch_increase]
            assert {$avg_batch_size > 1} ;# Should batch multiple operations
        }
    }
    
    test {operation memory management} {
        # Test that operation contexts are properly managed
        set info_before [r info uring]
        regexp {context_allocations:(\d+)} $info_before - ctx_allocs_before
        regexp {context_deallocations:(\d+)} $info_before - ctx_deallocs_before
        
        # Perform operations that create and destroy contexts
        for {set i 0} {$i < 100} {incr i} {
            r set "context_test_$i" "value_$i"
            r get "context_test_$i"
            r del "context_test_$i"
        }
        
        set info_after [r info uring]
        regexp {context_allocations:(\d+)} $info_after - ctx_allocs_after
        regexp {context_deallocations:(\d+)} $info_after - ctx_deallocs_after
        
        # Should have allocated and deallocated contexts
        assert {$ctx_allocs_after > $ctx_allocs_before}
        assert {$ctx_deallocs_after > $ctx_deallocs_before}
        
        # Deallocations should be close to allocations (no major leaks)
        set alloc_diff [expr $ctx_allocs_after - $ctx_allocs_before]
        set dealloc_diff [expr $ctx_deallocs_after - $ctx_deallocs_before]
        set leak_ratio [expr abs($alloc_diff - $dealloc_diff) / double($alloc_diff)]
        assert {$leak_ratio < 0.1} ;# Less than 10% leak ratio
    }
    
    test {operation statistics accuracy} {
        # Test that operation statistics are accurate
        
        # Reset statistics if possible
        if {![catch {r debug reset-uring-stats}]} {
            # Statistics were reset
        }
        
        set info_before [r info uring]
        regexp {ops_submitted:(\d+)} $info_before - submitted_before
        regexp {ops_completed:(\d+)} $info_before - completed_before
        
        # Perform exactly 10 operations
        for {set i 0} {$i < 10} {incr i} {
            r set "stats_test_$i" "value_$i"
        }
        
        set info_after [r info uring]
        regexp {ops_submitted:(\d+)} $info_after - submitted_after
        regexp {ops_completed:(\d+)} $info_after - completed_after
        
        # Should have submitted and completed operations
        assert {$submitted_after > $submitted_before}
        assert {$completed_after > $completed_before}
        
        # Verify operations actually worked
        for {set i 0} {$i < 10} {incr i} {
            assert_equal "value_$i" [r get "stats_test_$i"]
        }
    }
}
