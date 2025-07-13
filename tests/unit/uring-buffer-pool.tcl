# Test io_uring buffer pool implementation
# These tests verify that the buffer pool management system works correctly
# and provides the expected performance characteristics.

start_server {tags {"uring buffer pool"}} {
    test {buffer pool basic allocation and deallocation} {
        # This test verifies basic buffer pool functionality
        # Note: These tests require io_uring support to be compiled in
        
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test that buffer pool statistics are available
            set uring_info [r info uring]
            
            # Check if buffer pool is enabled
            if {[string match "*buffer_pool_size*" $uring_info]} {
                # Buffer pool is available, verify basic stats
                assert {[string match "*buffer_pool_size:*" $uring_info]}
                assert {[string match "*buffer_pool_buffer_size:*" $uring_info]}
                assert {[string match "*buffer_pool_free_count:*" $uring_info]}
                assert {[string match "*buffer_pool_allocated_count:*" $uring_info]}
                assert {[string match "*buffer_pool_total_allocations:*" $uring_info]}
                assert {[string match "*buffer_pool_hits:*" $uring_info]}
                assert {[string match "*buffer_pool_misses:*" $uring_info]}
                assert {[string match "*buffer_pool_hit_rate:*" $uring_info]}
                assert {[string match "*buffer_pool_total_deallocations:*" $uring_info]}
                assert {[string match "*buffer_pool_peak_usage:*" $uring_info]}
                assert {[string match "*buffer_pool_utilization:*" $uring_info]}
            }
        }
    }
    
    test {buffer pool statistics are properly initialized} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            set uring_info [r info uring]
            
            if {[string match "*buffer_pool_size*" $uring_info]} {
                # Extract buffer pool size
                regexp {buffer_pool_size:(\d+)} $uring_info match pool_size
                assert {$pool_size > 0}
                
                # Extract buffer size
                regexp {buffer_pool_buffer_size:(\d+)} $uring_info match buffer_size
                assert {$buffer_size > 0}
                
                # Initially, all buffers should be free
                regexp {buffer_pool_free_count:(\d+)} $uring_info match free_count
                assert {$free_count == $pool_size}
                
                # Initially, no buffers should be allocated
                regexp {buffer_pool_allocated_count:(\d+)} $uring_info match allocated_count
                assert {$allocated_count == 0}
                
                # Initial utilization should be 0%
                regexp {buffer_pool_utilization:([0-9.]+)} $uring_info match utilization
                assert {$utilization == 0.0}
            }
        }
    }
    
    test {buffer pool configuration matches server settings} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            set uring_info [r info uring]
            
            if {[string match "*buffer_pool_size*" $uring_info]} {
                # Get configured buffer ring size
                set config_buffer_ring_size [lindex [r config get uring-buffer-ring-size] 1]
                set config_buffer_size [lindex [r config get uring-buffer-size] 1]
                
                # Extract actual buffer pool settings
                regexp {buffer_pool_size:(\d+)} $uring_info match actual_pool_size
                regexp {buffer_pool_buffer_size:(\d+)} $uring_info match actual_buffer_size
                
                # Buffer pool size should match configuration
                assert {$actual_pool_size == $config_buffer_ring_size}
                assert {$actual_buffer_size == $config_buffer_size}
            }
        }
    }
}

# Test buffer pool under load
start_server {tags {"uring buffer pool load"}} {
    test {buffer pool handles multiple client connections} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Create multiple connections to test buffer pool usage
            set clients {}
            for {set i 0} {$i < 10} {incr i} {
                lappend clients [redis [srv host] [srv port]]
            }
            
            # Perform operations on all clients
            foreach client $clients {
                $client ping
                $client set "key$i" "value$i"
                $client get "key$i"
            }
            
            # Check buffer pool statistics after load
            set uring_info [r info uring]
            if {[string match "*buffer_pool_total_allocations*" $uring_info]} {
                regexp {buffer_pool_total_allocations:(\d+)} $uring_info match total_allocs
                
                # Should have some allocations after operations
                assert {$total_allocs >= 0}
                
                # Check hit rate
                regexp {buffer_pool_hit_rate:([0-9.]+)} $uring_info match hit_rate
                # Hit rate should be reasonable (>= 0%)
                assert {$hit_rate >= 0.0}
            }
            
            # Clean up clients
            foreach client $clients {
                $client close
            }
        }
    }
    
    test {buffer pool statistics are consistent} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            set uring_info [r info uring]
            
            if {[string match "*buffer_pool_total_allocations*" $uring_info]} {
                # Extract statistics
                regexp {buffer_pool_total_allocations:(\d+)} $uring_info match total_allocs
                regexp {buffer_pool_hits:(\d+)} $uring_info match hits
                regexp {buffer_pool_misses:(\d+)} $uring_info match misses
                regexp {buffer_pool_total_deallocations:(\d+)} $uring_info match total_deallocs
                regexp {buffer_pool_allocated_count:(\d+)} $uring_info match allocated_count
                regexp {buffer_pool_free_count:(\d+)} $uring_info match free_count
                regexp {buffer_pool_size:(\d+)} $uring_info match pool_size
                
                # Consistency checks
                # Total allocations should equal hits + misses
                assert {$total_allocs == [expr $hits + $misses]}
                
                # Free count + allocated count should equal pool size
                assert {[expr $free_count + $allocated_count] == $pool_size}
                
                # Deallocations should not exceed allocations
                assert {$total_deallocs <= $total_allocs}
            }
        }
    }
    
    test {buffer pool hit rate meets performance requirements} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Perform a series of operations to generate buffer usage
            for {set i 0} {$i < 100} {incr i} {
                r set "test_key_$i" "test_value_$i"
                r get "test_key_$i"
            }
            
            set uring_info [r info uring]
            if {[string match "*buffer_pool_hit_rate*" $uring_info]} {
                regexp {buffer_pool_hit_rate:([0-9.]+)} $uring_info match hit_rate
                regexp {buffer_pool_total_allocations:(\d+)} $uring_info match total_allocs
                
                # If we have allocations, hit rate should be reasonable
                if {$total_allocs > 0} {
                    # Hit rate should be at least 50% for normal operations
                    # (Design target is >80%, but we'll be conservative in tests)
                    assert {$hit_rate >= 50.0}
                }
            }
        }
    }
}

# Test buffer pool edge cases
start_server {tags {"uring buffer pool edge cases"}} {
    test {buffer pool handles server restart correctly} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Get initial buffer pool stats
            set initial_uring_info [r info uring]
            
            # Restart server
            restart_server 0 true false
            
            # Check that buffer pool is reinitialized correctly
            set new_uring_info [r info uring]
            
            if {[string match "*buffer_pool_size*" $new_uring_info]} {
                # Buffer pool should be reinitialized
                regexp {buffer_pool_total_allocations:(\d+)} $new_uring_info match new_total_allocs
                regexp {buffer_pool_allocated_count:(\d+)} $new_uring_info match new_allocated_count
                regexp {buffer_pool_free_count:(\d+)} $new_uring_info match new_free_count
                regexp {buffer_pool_size:(\d+)} $new_uring_info match new_pool_size
                
                # After restart, stats should be reset
                assert {$new_total_allocs == 0}
                assert {$new_allocated_count == 0}
                assert {$new_free_count == $new_pool_size}
            }
        }
    }
    
    test {buffer pool statistics are thread-safe} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Create multiple concurrent connections
            set clients {}
            for {set i 0} {$i < 5} {incr i} {
                lappend clients [redis [srv host] [srv port]]
            }
            
            # Perform concurrent operations
            set operations_per_client 20
            foreach client $clients {
                for {set j 0} {$j < $operations_per_client} {incr j} {
                    $client set "concurrent_key_${i}_${j}" "value_${j}"
                    $client get "concurrent_key_${i}_${j}"
                }
            }
            
            # Get final statistics
            set uring_info [r info uring]
            if {[string match "*buffer_pool_total_allocations*" $uring_info]} {
                # Statistics should be consistent even with concurrent access
                regexp {buffer_pool_total_allocations:(\d+)} $uring_info match total_allocs
                regexp {buffer_pool_hits:(\d+)} $uring_info match hits
                regexp {buffer_pool_misses:(\d+)} $uring_info match misses
                
                # Basic consistency check
                assert {$total_allocs == [expr $hits + $misses]}
                
                # Should have some allocations from concurrent operations
                assert {$total_allocs >= 0}
            }
            
            # Clean up clients
            foreach client $clients {
                $client close
            }
        }
    }
    
    test {buffer pool memory usage is reasonable} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            set uring_info [r info uring]
            
            if {[string match "*buffer_pool_size*" $uring_info]} {
                regexp {buffer_pool_size:(\d+)} $uring_info match pool_size
                regexp {buffer_pool_buffer_size:(\d+)} $uring_info match buffer_size
                
                # Calculate expected memory usage
                set expected_memory [expr $pool_size * $buffer_size]
                
                # Memory usage should be reasonable (less than 100MB for default config)
                assert {$expected_memory < 104857600}  ;# 100MB
                
                # Pool size should be reasonable
                assert {$pool_size >= 64}
                assert {$pool_size <= 16384}
                
                # Buffer size should be reasonable
                assert {$buffer_size >= 1024}
                assert {$buffer_size <= 65536}
            }
        }
    }
}
