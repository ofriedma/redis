# Week 3 io_uring Integration Tests
# Tests for event loop integration, connection handling, and completion handlers

start_server {tags {"uring week3"} overrides {uring-enabled yes uring-sqpoll no}} {
    test {io_uring basic connection and command execution} {
        # Test basic PING command
        assert_equal "PONG" [r ping]
        
        # Test basic SET/GET operations
        r set test_key "test_value"
        assert_equal "test_value" [r get test_key]
        
        # Test multiple operations
        for {set i 0} {$i < 100} {incr i} {
            r set "key_$i" "value_$i"
        }
        
        for {set i 0} {$i < 100} {incr i} {
            assert_equal "value_$i" [r get "key_$i"]
        }
    }
    
    test {io_uring concurrent connections} {
        # Create multiple connections
        set clients {}
        for {set i 0} {$i < 10} {incr i} {
            lappend clients [redis [srv host] [srv port]]
        }
        
        # Execute commands concurrently
        foreach client $clients {
            $client ping
            $client set "concurrent_key_[llength $clients]" "concurrent_value"
        }
        
        # Verify all operations succeeded
        foreach client $clients {
            assert_equal "PONG" [$client ping]
            assert_equal "concurrent_value" [$client get "concurrent_key_[llength $clients]"]
        }
        
        # Clean up connections
        foreach client $clients {
            $client close
        }
    }
    
    test {io_uring large data handling} {
        # Test with large values
        set large_value [string repeat "x" 10000]
        r set large_key $large_value
        assert_equal $large_value [r get large_key]
        
        # Test with multiple large values
        for {set i 0} {$i < 10} {incr i} {
            set value [string repeat "data_$i" 1000]
            r set "large_key_$i" $value
            assert_equal $value [r get "large_key_$i"]
        }
    }
    
    test {io_uring pipeline operations} {
        # Test pipelined operations
        set pipe [r pipeline]
        for {set i 0} {$i < 100} {incr i} {
            $pipe set "pipe_key_$i" "pipe_value_$i"
        }
        set results [$pipe execute]
        
        # Verify all SET operations succeeded
        foreach result $results {
            assert_equal "OK" $result
        }
        
        # Verify values can be retrieved
        for {set i 0} {$i < 100} {incr i} {
            assert_equal "pipe_value_$i" [r get "pipe_key_$i"]
        }
    }
    
    test {io_uring error handling} {
        # Test invalid commands
        catch {r invalid_command} error
        assert_match "*unknown command*" $error
        
        # Test invalid arguments
        catch {r set} error
        assert_match "*wrong number of arguments*" $error
        
        # Verify server is still responsive after errors
        assert_equal "PONG" [r ping]
    }
    
    test {io_uring memory efficiency} {
        # Get initial memory usage
        set info_before [r info memory]
        regexp {used_memory:(\d+)} $info_before - memory_before
        
        # Perform operations that should use io_uring buffers efficiently
        for {set i 0} {$i < 1000} {incr i} {
            r set "mem_test_$i" "value_$i"
            r get "mem_test_$i"
        }
        
        # Get memory usage after operations
        set info_after [r info memory]
        regexp {used_memory:(\d+)} $info_after - memory_after
        
        # Memory increase should be reasonable (not excessive)
        set memory_increase [expr $memory_after - $memory_before]
        assert {$memory_increase < 10000000} ;# Less than 10MB increase
    }
    
    test {io_uring statistics reporting} {
        # Check if io_uring statistics are available
        set info [r info server]
        
        # Should contain io_uring related information
        if {[string match "*uring*" $info]} {
            # Verify basic statistics are present
            assert_match "*uring_enabled*" $info
        }
    }
}

start_server {tags {"uring week3 sqpoll"} overrides {uring-enabled yes uring-sqpoll yes}} {
    test {io_uring with SQPOLL enabled} {
        # Test basic operations with SQPOLL
        assert_equal "PONG" [r ping]
        
        # Test multiple operations
        for {set i 0} {$i < 50} {incr i} {
            r set "sqpoll_key_$i" "sqpoll_value_$i"
            assert_equal "sqpoll_value_$i" [r get "sqpoll_key_$i"]
        }
    }
    
    test {io_uring SQPOLL performance} {
        # Measure performance with SQPOLL
        set start_time [clock milliseconds]
        
        for {set i 0} {$i < 1000} {incr i} {
            r set "perf_key_$i" "perf_value_$i"
        }
        
        set end_time [clock milliseconds]
        set duration [expr $end_time - $start_time]
        
        # Should complete within reasonable time
        assert {$duration < 5000} ;# Less than 5 seconds
    }
}

# Test fallback to regular event handling
start_server {tags {"uring week3 fallback"} overrides {uring-enabled no}} {
    test {fallback to regular event handling when io_uring disabled} {
        # Should work normally without io_uring
        assert_equal "PONG" [r ping]
        
        # Test basic operations
        r set fallback_key "fallback_value"
        assert_equal "fallback_value" [r get fallback_key]
        
        # Verify no io_uring statistics
        set info [r info server]
        if {[string match "*uring_enabled*" $info]} {
            assert_match "*uring_enabled:0*" $info
        }
    }
}

# Test mixed workload
start_server {tags {"uring week3 mixed"} overrides {uring-enabled yes}} {
    test {io_uring mixed data types and operations} {
        # Test strings
        r set string_key "string_value"
        assert_equal "string_value" [r get string_key]
        
        # Test lists
        r lpush list_key "item1" "item2" "item3"
        assert_equal 3 [r llen list_key]
        assert_equal "item3" [r lpop list_key]
        
        # Test hashes
        r hset hash_key field1 "value1" field2 "value2"
        assert_equal "value1" [r hget hash_key field1]
        assert_equal 2 [r hlen hash_key]
        
        # Test sets
        r sadd set_key "member1" "member2" "member3"
        assert_equal 3 [r scard set_key]
        assert_equal 1 [r sismember set_key "member1"]
        
        # Test sorted sets
        r zadd zset_key 1 "member1" 2 "member2" 3 "member3"
        assert_equal 3 [r zcard zset_key]
        assert_equal "member1" [lindex [r zrange zset_key 0 0] 0]
    }
}
