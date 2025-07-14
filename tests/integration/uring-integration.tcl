# Redis io_uring Integration Tests
# Comprehensive integration testing for Week 2 implementations

# Test client connections with io_uring
start_server {tags {"uring integration"}} {
    test {Integration: Basic client connection with io_uring} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test basic connection
            assert {[r ping] eq "PONG"}
            
            # Test that connection is tracked
            set active_connections [extract_info_field $info "active_connections"]
            assert {$active_connections >= 1}
            
            # Test basic operations
            r set integration_key "integration_value"
            assert_equal "integration_value" [r get integration_key]
            r del integration_key
        } else {
            skip "io_uring not available"
        }
    }
    
    test {Integration: Multiple concurrent client connections} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Create multiple connections
            set clients {}
            for {set i 0} {$i < 5} {incr i} {
                lappend clients [redis [srv host] [srv port]]
            }
            
            # Test operations on all connections
            for {set i 0} {$i < 5} {incr i} {
                set client [lindex $clients $i]
                $client set "multi_conn_$i" "value_$i"
            }
            
            # Verify operations from main connection
            for {set i 0} {$i < 5} {incr i} {
                assert_equal "value_$i" [r get "multi_conn_$i"]
            }
            
            # Check connection statistics
            set final_info [r info uring]
            set final_connections [extract_info_field $final_info "active_connections"]
            assert {$final_connections > 1}
            
            # Close additional connections
            foreach client $clients {
                $client close
            }
            
            # Clean up
            for {set i 0} {$i < 5} {incr i} {
                r del "multi_conn_$i"
            }
        }
    }
    
    test {Integration: High-throughput data operations} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Record initial statistics
            set initial_info [r info uring]
            set initial_ops [extract_info_field $initial_info "uring_ops_completed"]
            set initial_bytes_read [extract_info_field $initial_info "total_bytes_read"]
            set initial_bytes_written [extract_info_field $initial_info "total_bytes_written"]
            
            # Perform high-throughput operations
            set num_ops 2000
            set start_time [clock milliseconds]
            
            # Use pipeline for maximum throughput
            r pipeline
            for {set i 0} {$i < $num_ops} {incr i} {
                r set "throughput_$i" "data_value_$i"
            }
            r execute
            
            r pipeline
            for {set i 0} {$i < $num_ops} {incr i} {
                r get "throughput_$i"
            }
            set results [r execute]
            
            set end_time [clock milliseconds]
            set duration [expr {$end_time - $start_time}]
            
            # Verify all operations completed correctly
            for {set i 0} {$i < $num_ops} {incr i} {
                assert_equal "data_value_$i" [lindex $results $i]
            }
            
            # Check performance
            set ops_per_sec [expr {($num_ops * 2 * 1000) / $duration}]
            assert {$ops_per_sec > 1000} ;# Should handle at least 1000 ops/sec
            
            # Check statistics
            set final_info [r info uring]
            set final_ops [extract_info_field $final_info "uring_ops_completed"]
            set final_bytes_read [extract_info_field $final_info "total_bytes_read"]
            set final_bytes_written [extract_info_field $final_info "total_bytes_written"]
            
            assert {$final_ops > $initial_ops}
            assert {$final_bytes_read > $initial_bytes_read}
            assert {$final_bytes_written > $initial_bytes_written}
            
            # Clean up
            for {set i 0} {$i < $num_ops} {incr i} {
                r del "throughput_$i"
            }
        }
    }
    
    test {Integration: Mixed data type operations} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test various Redis data types with io_uring
            
            # Strings
            r set string_key "string_value"
            assert_equal "string_value" [r get string_key]
            
            # Lists
            r lpush list_key "item1" "item2" "item3"
            assert_equal 3 [r llen list_key]
            assert_equal "item3" [r lpop list_key]
            
            # Hashes
            r hset hash_key field1 "value1" field2 "value2"
            assert_equal 2 [r hlen hash_key]
            assert_equal "value1" [r hget hash_key field1]
            
            # Sets
            r sadd set_key "member1" "member2" "member3"
            assert_equal 3 [r scard set_key]
            assert {[r sismember set_key "member1"]}
            
            # Sorted Sets
            r zadd zset_key 1.0 "member1" 2.0 "member2" 3.0 "member3"
            assert_equal 3 [r zcard zset_key]
            assert_equal "member1" [lindex [r zrange zset_key 0 0] 0]
            
            # Verify io_uring handled all operations
            set final_info [r info uring]
            set success_rate [extract_info_field $final_info "uring_success_rate"]
            assert {$success_rate >= 95.0}
            
            # Clean up
            r del string_key list_key hash_key set_key zset_key
        }
    }
    
    test {Integration: Large value handling} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test large values to stress buffer management
            set large_value [string repeat "x" 65536] ;# 64KB value
            
            r set large_key $large_value
            assert_equal $large_value [r get large_key]
            
            # Test multiple large values
            for {set i 0} {$i < 10} {incr i} {
                set value [string repeat "y" [expr {$i * 1000 + 1000}]]
                r set "large_$i" $value
            }
            
            # Verify all large values
            for {set i 0} {$i < 10} {incr i} {
                set expected [string repeat "y" [expr {$i * 1000 + 1000}]]
                assert_equal $expected [r get "large_$i"]
            }
            
            # Check buffer pool statistics
            set final_info [r info uring]
            set buffer_allocations [extract_info_field $final_info "buffer_pool_allocations"]
            assert {$buffer_allocations > 0}
            
            # Clean up
            r del large_key
            for {set i 0} {$i < 10} {incr i} {
                r del "large_$i"
            }
        }
    }
    
    test {Integration: Connection lifecycle during operations} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test connection state changes during operations
            set initial_info [r info uring]
            set initial_connections [extract_info_field $initial_info "active_connections"]
            
            # Create and close connections while performing operations
            for {set round 0} {$round < 3} {incr round} {
                # Create temporary connections
                set temp_clients {}
                for {set i 0} {$i < 3} {incr i} {
                    lappend temp_clients [redis [srv host] [srv port]]
                }
                
                # Perform operations on temporary connections
                for {set i 0} {$i < 3} {incr i} {
                    set client [lindex $temp_clients $i]
                    $client set "temp_${round}_$i" "value_${round}_$i"
                }
                
                # Close temporary connections
                foreach client $temp_clients {
                    $client close
                }
                
                # Verify data is still accessible from main connection
                for {set i 0} {$i < 3} {incr i} {
                    assert_equal "value_${round}_$i" [r get "temp_${round}_$i"]
                }
            }
            
            # Check connection statistics
            set final_info [r info uring]
            set connection_ops_completed [extract_info_field $final_info "total_ops_completed"]
            assert {$connection_ops_completed > 0}
            
            # Clean up
            for {set round 0} {$round < 3} {incr round} {
                for {set i 0} {$i < 3} {incr i} {
                    r del "temp_${round}_$i"
                }
            }
        }
    }
    
    test {Integration: Error recovery and resilience} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test error recovery mechanisms
            set initial_info [r info uring]
            set initial_failed [extract_info_field $initial_info "uring_ops_failed"]
            
            # Perform operations that might trigger retries
            for {set i 0} {$i < 100} {incr i} {
                r set "resilience_$i" "value_$i"
                
                # Occasionally try operations that exercise error paths
                if {$i % 20 == 0} {
                    # Try to get non-existent keys
                    r get "nonexistent_$i"
                    
                    # Try operations on wrong data types (will return errors but shouldn't crash)
                    catch {r lpush "resilience_$i" "item"}
                }
            }
            
            # Verify all valid operations completed
            for {set i 0} {$i < 100} {incr i} {
                assert_equal "value_$i" [r get "resilience_$i"]
            }
            
            # Check that system remained stable
            set final_info [r info uring]
            set success_rate [extract_info_field $final_info "uring_success_rate"]
            assert {$success_rate >= 90.0} ;# Should maintain high success rate
            
            # Clean up
            for {set i 0} {$i < 100} {incr i} {
                r del "resilience_$i"
            }
        }
    }
    
    test {Integration: Performance under sustained load} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Test sustained performance
            set duration_seconds 10
            set start_time [clock seconds]
            set operations 0
            
            while {[expr {[clock seconds] - $start_time}] < $duration_seconds} {
                # Perform mixed operations
                r set "perf_$operations" "value_$operations"
                r get "perf_$operations"
                r del "perf_$operations"
                incr operations
                
                # Check system health periodically
                if {$operations % 1000 == 0} {
                    set current_info [r info uring]
                    set current_success_rate [extract_info_field $current_info "uring_success_rate"]
                    assert {$current_success_rate >= 90.0}
                }
            }
            
            set actual_duration [expr {[clock seconds] - $start_time}]
            set ops_per_second [expr {$operations / $actual_duration}]
            
            # Should maintain reasonable performance
            assert {$ops_per_second > 100} ;# At least 100 ops/sec sustained
            
            # Check final statistics
            set final_info [r info uring]
            set avg_completion_time [extract_info_field $final_info "uring_avg_completion_time_us"]
            assert {$avg_completion_time < 50000} ;# Less than 50ms average
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
