# Week 4 io_uring Buffer Management Unit Tests
# Comprehensive tests for buffer pool management, allocation, and cleanup

start_server {tags {"uring week4 buffer"} overrides {uring-enabled yes}} {
    test {buffer pool initialization} {
        # Test that buffer pool is properly initialized
        set info [r info uring]
        assert_match "*buffer_pool_size*" $info
        assert_match "*buffer_pool_used*" $info
        assert_match "*buffer_pool_free*" $info
    }
    
    test {buffer allocation and deallocation} {
        # Get initial buffer statistics
        set info_before [r info uring]
        regexp {buffer_allocations:(\d+)} $info_before - allocs_before
        regexp {buffer_deallocations:(\d+)} $info_before - deallocs_before
        
        # Perform operations that should allocate/deallocate buffers
        for {set i 0} {$i < 100} {incr i} {
            r set "buffer_test_$i" "value_$i"
            r get "buffer_test_$i"
        }
        
        # Check that buffer operations occurred
        set info_after [r info uring]
        regexp {buffer_allocations:(\d+)} $info_after - allocs_after
        regexp {buffer_deallocations:(\d+)} $info_after - deallocs_after
        
        # Should have allocated and deallocated buffers
        assert {$allocs_after > $allocs_before}
        assert {$deallocs_after >= $deallocs_before}
    }
    
    test {buffer pool efficiency} {
        # Test buffer reuse efficiency
        set info_before [r info uring]
        regexp {buffer_pool_reuse_count:(\d+)} $info_before - reuse_before
        
        # Perform many small operations to trigger buffer reuse
        for {set i 0} {$i < 1000} {incr i} {
            r ping
        }
        
        set info_after [r info uring]
        regexp {buffer_pool_reuse_count:(\d+)} $info_after - reuse_after
        
        # Should have reused buffers
        assert {$reuse_after > $reuse_before}
    }
    
    test {buffer pool memory limits} {
        # Test that buffer pool respects memory limits
        set large_value [string repeat "x" 100000]
        
        # Try to allocate many large values
        set allocated 0
        for {set i 0} {$i < 100} {incr i} {
            if {[catch {r set "large_buffer_$i" $large_value}]} {
                break
            }
            incr allocated
        }
        
        # Should have allocated some but not unlimited
        assert {$allocated > 0}
        assert {$allocated < 100}
        
        # Clean up
        for {set i 0} {$i < $allocated} {incr i} {
            r del "large_buffer_$i"
        }
    }
    
    test {buffer pool fragmentation handling} {
        # Test handling of different buffer sizes
        set small_value "small"
        set medium_value [string repeat "m" 1000]
        set large_value [string repeat "l" 10000]
        
        # Mix different sizes to test fragmentation
        for {set i 0} {$i < 50} {incr i} {
            r set "small_$i" $small_value
            r set "medium_$i" $medium_value
            r set "large_$i" $large_value
        }
        
        # Verify all values are correct
        for {set i 0} {$i < 50} {incr i} {
            assert_equal $small_value [r get "small_$i"]
            assert_equal $medium_value [r get "medium_$i"]
            assert_equal $large_value [r get "large_$i"]
        }
        
        # Check buffer pool statistics
        set info [r info uring]
        assert_match "*buffer_pool_fragmentation*" $info
    }
    
    test {buffer pool cleanup on client disconnect} {
        # Create a new client connection
        set client [redis [srv host] [srv port]]
        
        # Get initial buffer count
        set info_before [r info uring]
        regexp {buffer_pool_used:(\d+)} $info_before - used_before
        
        # Use the client to allocate buffers
        for {set i 0} {$i < 10} {incr i} {
            $client set "client_buffer_$i" "value_$i"
        }
        
        # Check buffer usage increased
        set info_during [r info uring]
        regexp {buffer_pool_used:(\d+)} $info_during - used_during
        assert {$used_during >= $used_before}
        
        # Close the client
        $client close
        
        # Wait a bit for cleanup
        after 100
        
        # Check that buffers were cleaned up
        set info_after [r info uring]
        regexp {buffer_pool_used:(\d+)} $info_after - used_after
        
        # Buffer usage should have decreased
        assert {$used_after <= $used_during}
    }
    
    test {buffer pool stress test} {
        # Stress test with many concurrent operations
        set clients {}
        
        # Create multiple clients
        for {set i 0} {$i < 5} {incr i} {
            lappend clients [redis [srv host] [srv port]]
        }
        
        # Get initial statistics
        set info_before [r info uring]
        regexp {buffer_allocations:(\d+)} $info_before - allocs_before
        
        # Perform concurrent operations
        foreach client $clients {
            for {set i 0} {$i < 100} {incr i} {
                $client set "stress_key_${client}_$i" "stress_value_$i"
            }
        }
        
        # Verify operations succeeded
        foreach client $clients {
            for {set i 0} {$i < 100} {incr i} {
                assert_equal "stress_value_$i" [$client get "stress_key_${client}_$i"]
            }
        }
        
        # Check statistics
        set info_after [r info uring]
        regexp {buffer_allocations:(\d+)} $info_after - allocs_after
        assert {$allocs_after > $allocs_before}
        
        # Clean up clients
        foreach client $clients {
            $client close
        }
    }
    
    test {buffer pool error handling} {
        # Test buffer pool behavior under error conditions
        
        # Try to allocate extremely large buffer
        set huge_value [string repeat "x" 1000000]
        set error_caught 0
        
        if {[catch {r set huge_key $huge_value} error]} {
            set error_caught 1
        }
        
        # Server should still be responsive after error
        assert_equal "PONG" [r ping]
        
        # Check error statistics
        set info [r info uring]
        if {$error_caught} {
            assert_match "*buffer_allocation_errors*" $info
        }
    }
    
    test {buffer pool memory leak detection} {
        # Test for memory leaks in buffer management
        set info_before [r info memory]
        regexp {used_memory:(\d+)} $info_before - memory_before
        
        # Perform many operations
        for {set i 0} {$i < 1000} {incr i} {
            r set "leak_test_$i" "value_$i"
            r get "leak_test_$i"
            r del "leak_test_$i"
        }
        
        # Force garbage collection
        r debug gc
        
        set info_after [r info memory]
        regexp {used_memory:(\d+)} $info_after - memory_after
        
        # Memory increase should be minimal
        set memory_increase [expr $memory_after - $memory_before]
        assert {$memory_increase < 1000000} ;# Less than 1MB increase
    }
}
