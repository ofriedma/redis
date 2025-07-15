# Week 4 io_uring Real Workload Integration Tests
# Tests with realistic Redis usage patterns and workloads

start_server {tags {"uring week4 integration workload"} overrides {uring-enabled yes}} {
    test {web application session store workload} {
        # Simulate web application session storage pattern
        set session_count 1000
        set session_ttl 3600
        
        # Create sessions
        for {set i 0} {$i < $session_count} {incr i} {
            set session_id "sess_[format %08d $i]"
            set session_data [dict create \
                user_id $i \
                username "user$i" \
                login_time [clock seconds] \
                last_activity [clock seconds] \
                permissions "read,write" \
                cart_items [list "item1" "item2" "item3"]]
            
            r setex $session_id $session_ttl $session_data
        }
        
        # Simulate session access pattern
        for {set i 0} {$i < 100} {incr i} {
            set session_id "sess_[format %08d [expr int(rand() * $session_count)]]"
            
            # Read session
            set session_data [r get $session_id]
            assert {$session_data ne ""}
            
            # Update last activity
            r expire $session_id $session_ttl
        }
        
        # Verify sessions exist
        set active_sessions 0
        for {set i 0} {$i < $session_count} {incr i} {
            set session_id "sess_[format %08d $i]"
            if {[r exists $session_id]} {
                incr active_sessions
            }
        }
        
        assert {$active_sessions > 900} ;# Most sessions should still exist
    }
    
    test {real-time analytics workload} {
        # Simulate real-time analytics data ingestion
        set metrics [list "page_views" "user_signups" "purchases" "api_calls"]
        set time_buckets 100
        
        # Ingest metrics data
        for {set bucket 0} {$bucket < $time_buckets} {incr bucket} {
            set timestamp [expr [clock seconds] - $bucket * 60]
            
            foreach metric $metrics {
                set key "${metric}:${timestamp}"
                set value [expr int(rand() * 1000)]
                
                # Store metric value
                r set $key $value
                
                # Add to time series
                r zadd "${metric}:timeseries" $timestamp $value
                
                # Update counters
                r incr "${metric}:total"
                r hincrby "metrics:hourly" $metric $value
            }
        }
        
        # Query analytics data
        foreach metric $metrics {
            # Get recent values
            set recent [r zrevrange "${metric}:timeseries" 0 9 WITHSCORES]
            assert {[llength $recent] > 0}
            
            # Get total count
            set total [r get "${metric}:total"]
            assert {$total > 0}
            
            # Get hourly aggregation
            set hourly [r hget "metrics:hourly" $metric]
            assert {$hourly > 0}
        }
    }
    
    test {chat application workload} {
        # Simulate chat application message handling
        set rooms [list "general" "random" "tech" "sports"]
        set users [list "alice" "bob" "charlie" "diana" "eve"]
        set message_count 500
        
        # Send messages
        for {set i 0} {$i < $message_count} {incr i} {
            set room [lindex $rooms [expr int(rand() * [llength $rooms])]]
            set user [lindex $users [expr int(rand() * [llength $users])]]
            set message "Message $i from $user"
            set timestamp [clock milliseconds]
            
            # Store message in room
            set message_data [dict create \
                user $user \
                message $message \
                timestamp $timestamp]
            
            r lpush "room:${room}:messages" $message_data
            r ltrim "room:${room}:messages" 0 99 ;# Keep last 100 messages
            
            # Update user activity
            r zadd "users:activity" $timestamp $user
            
            # Update room statistics
            r hincrby "room:${room}:stats" "message_count" 1
            r hset "room:${room}:stats" "last_message" $timestamp
        }
        
        # Query chat data
        foreach room $rooms {
            # Get recent messages
            set messages [r lrange "room:${room}:messages" 0 9]
            assert {[llength $messages] > 0}
            
            # Get room stats
            set stats [r hgetall "room:${room}:stats"]
            assert {[dict exists $stats "message_count"]}
            assert {[dict get $stats "message_count"] > 0}
        }
        
        # Get active users
        set active_users [r zrevrange "users:activity" 0 -1 WITHSCORES]
        assert {[llength $active_users] > 0}
    }
    
    test {e-commerce product catalog workload} {
        # Simulate e-commerce product catalog operations
        set categories [list "electronics" "clothing" "books" "home" "sports"]
        set product_count 1000
        
        # Create products
        for {set i 0} {$i < $product_count} {incr i} {
            set product_id "prod_$i"
            set category [lindex $categories [expr int(rand() * [llength $categories])]]
            set price [expr int(rand() * 10000) / 100.0]
            set stock [expr int(rand() * 100)]
            
            # Store product data
            r hmset "product:$product_id" \
                name "Product $i" \
                category $category \
                price $price \
                stock $stock \
                description "Description for product $i" \
                rating [expr int(rand() * 50) / 10.0]
            
            # Add to category index
            r zadd "category:${category}:products" $price $product_id
            
            # Add to search index (simplified)
            r sadd "search:product" $product_id
            r sadd "search:${category}" $product_id
        }
        
        # Simulate product searches and updates
        for {set i 0} {$i < 100} {incr i} {
            # Search by category
            set category [lindex $categories [expr int(rand() * [llength $categories])]]
            set products [r zrange "category:${category}:products" 0 9]
            assert {[llength $products] > 0}
            
            # Get product details
            set product_id [lindex $products [expr int(rand() * [llength $products])]]
            set product_data [r hgetall "product:$product_id"]
            assert {[dict exists $product_data "name"]}
            assert {[dict exists $product_data "price"]}
            
            # Update stock (simulate purchase)
            if {[expr int(rand() * 10)] < 3} {
                r hincrby "product:$product_id" "stock" -1
            }
        }
    }
    
    test {gaming leaderboard workload} {
        # Simulate gaming leaderboard operations
        set players 1000
        set games [list "puzzle" "racing" "strategy" "action"]
        
        # Initialize players
        for {set i 0} {$i < $players} {incr i} {
            set player_id "player_$i"
            
            # Player profile
            r hmset "player:$player_id" \
                username "Player$i" \
                level [expr int(rand() * 100) + 1] \
                experience [expr int(rand() * 100000)] \
                coins [expr int(rand() * 10000)] \
                join_date [clock seconds]
            
            # Add to global leaderboard
            set score [expr int(rand() * 100000)]
            r zadd "leaderboard:global" $score $player_id
            
            # Add to game-specific leaderboards
            foreach game $games {
                set game_score [expr int(rand() * 50000)]
                r zadd "leaderboard:$game" $game_score $player_id
            }
        }
        
        # Simulate game sessions and score updates
        for {set i 0} {$i < 200} {incr i} {
            set player_id "player_[expr int(rand() * $players)]"
            set game [lindex $games [expr int(rand() * [llength $games])]]
            set score_increase [expr int(rand() * 1000)]
            
            # Update scores
            r zincrby "leaderboard:global" $score_increase $player_id
            r zincrby "leaderboard:$game" $score_increase $player_id
            
            # Update player stats
            r hincrby "player:$player_id" "experience" $score_increase
            r hincrby "player:$player_id" "coins" [expr $score_increase / 10]
        }
        
        # Query leaderboards
        foreach game $games {
            # Get top 10 players
            set top_players [r zrevrange "leaderboard:$game" 0 9 WITHSCORES]
            assert {[llength $top_players] >= 20} ;# 10 players with scores
            
            # Get player rank
            set random_player "player_[expr int(rand() * $players)]"
            set rank [r zrevrank "leaderboard:$game" $random_player]
            assert {$rank ne ""}
        }
    }
    
    test {IoT sensor data workload} {
        # Simulate IoT sensor data collection
        set sensors [list "temp_01" "humid_01" "pressure_01" "light_01" "motion_01"]
        set data_points 1000
        
        # Collect sensor data
        for {set i 0} {$i < $data_points} {incr i} {
            set timestamp [expr [clock milliseconds] - $i * 1000]
            
            foreach sensor $sensors {
                # Generate sensor reading
                switch $sensor {
                    "temp_01" { set value [expr 20 + rand() * 15] }
                    "humid_01" { set value [expr 30 + rand() * 40] }
                    "pressure_01" { set value [expr 1000 + rand() * 50] }
                    "light_01" { set value [expr rand() * 1000] }
                    "motion_01" { set value [expr int(rand() * 2)] }
                }
                
                # Store time series data
                r zadd "sensor:${sensor}:data" $timestamp $value
                
                # Keep only recent data (last 1000 points)
                r zremrangebyrank "sensor:${sensor}:data" 0 -1001
                
                # Update current value
                r set "sensor:${sensor}:current" $value
                
                # Update statistics
                r lpush "sensor:${sensor}:recent" $value
                r ltrim "sensor:${sensor}:recent" 0 99
            }
        }
        
        # Query sensor data
        foreach sensor $sensors {
            # Get current value
            set current [r get "sensor:${sensor}:current"]
            assert {$current ne ""}
            
            # Get recent history
            set recent [r lrange "sensor:${sensor}:recent" 0 9]
            assert {[llength $recent] > 0}
            
            # Get time series data
            set timeseries [r zrevrange "sensor:${sensor}:data" 0 9 WITHSCORES]
            assert {[llength $timeseries] > 0}
        }
    }
}

# Concurrent client tests
start_server {tags {"uring week4 integration concurrent"} overrides {uring-enabled yes}} {
    test {high concurrency client connections} {
        # Test with many concurrent clients
        set client_count 50
        set clients {}

        # Create clients
        for {set i 0} {$i < $client_count} {incr i} {
            lappend clients [redis [srv host] [srv port]]
        }

        # Perform concurrent operations
        set start_time [clock milliseconds]

        foreach client $clients {
            for {set i 0} {$i < 20} {incr i} {
                $client set "concurrent_${client}_$i" "value_$i"
            }
        }

        # Verify all operations succeeded
        foreach client $clients {
            for {set i 0} {$i < 20} {incr i} {
                assert_equal "value_$i" [$client get "concurrent_${client}_$i"]
            }
        }

        set end_time [clock milliseconds]
        set duration [expr $end_time - $start_time]

        # Should handle concurrent clients efficiently
        assert {$duration < 5000} ;# Less than 5 seconds

        # Clean up clients
        foreach client $clients {
            $client close
        }
    }

    test {mixed read/write workload with multiple clients} {
        # Test mixed read/write operations with multiple clients
        set clients {}
        for {set i 0} {$i < 10} {incr i} {
            lappend clients [redis [srv host] [srv port]]
        }

        # Pre-populate data
        for {set i 0} {$i < 100} {incr i} {
            r set "shared_key_$i" "initial_value_$i"
        }

        # Perform mixed operations
        foreach client $clients {
            # 70% reads, 30% writes
            for {set i 0} {$i < 100} {incr i} {
                set key "shared_key_[expr int(rand() * 100)]"

                if {[expr rand()] < 0.7} {
                    # Read operation
                    set value [$client get $key]
                    assert {$value ne ""}
                } else {
                    # Write operation
                    $client set $key "updated_by_${client}_$i"
                }
            }
        }

        # Verify data integrity
        for {set i 0} {$i < 100} {incr i} {
            set value [r get "shared_key_$i"]
            assert {$value ne ""}
        }

        # Clean up clients
        foreach client $clients {
            $client close
        }
    }

    test {pipeline operations with concurrent clients} {
        # Test pipelined operations with multiple clients
        set clients {}
        for {set i 0} {$i < 5} {incr i} {
            lappend clients [redis [srv host] [srv port]]
        }

        # Perform pipelined operations
        foreach client $clients {
            set pipe [$client pipeline]

            for {set i 0} {$i < 100} {incr i} {
                $pipe set "pipeline_${client}_$i" "value_$i"
                $pipe get "pipeline_${client}_$i"
            }

            set results [$pipe execute]

            # Verify results (every other result should be "OK", others should be values)
            for {set i 0} {$i < [llength $results]} {incr i 2} {
                assert_equal "OK" [lindex $results $i]
                assert_equal "value_[expr $i/2]" [lindex $results [expr $i+1]]
            }
        }

        # Clean up clients
        foreach client $clients {
            $client close
        }
    }

    test {pub/sub with concurrent clients} {
        # Test pub/sub functionality with concurrent clients
        set publishers {}
        set subscribers {}

        # Create publishers and subscribers
        for {set i 0} {$i < 3} {incr i} {
            lappend publishers [redis [srv host] [srv port]]
            lappend subscribers [redis [srv host] [srv port]]
        }

        # Set up subscriptions
        set channels [list "news" "sports" "tech"]
        foreach subscriber $subscribers {
            foreach channel $channels {
                $subscriber subscribe $channel
            }
        }

        # Publish messages
        foreach publisher $publishers {
            foreach channel $channels {
                for {set i 0} {$i < 5} {incr i} {
                    $publisher publish $channel "Message $i from ${publisher} on $channel"
                }
            }
        }

        # Wait for message delivery
        after 100

        # Clean up
        foreach subscriber $subscribers {
            $subscriber unsubscribe
            $subscriber close
        }
        foreach publisher $publishers {
            $publisher close
        }
    }

    test {transaction handling with concurrent clients} {
        # Test Redis transactions with concurrent clients
        set clients {}
        for {set i 0} {$i < 5} {incr i} {
            lappend clients [redis [srv host] [srv port]]
        }

        # Initialize shared counters
        r set "counter1" 0
        r set "counter2" 0

        # Perform concurrent transactions
        foreach client $clients {
            for {set i 0} {$i < 10} {incr i} {
                $client multi
                $client incr "counter1"
                $client incr "counter2"
                set results [$client exec]

                # Transaction should succeed
                assert {[llength $results] == 2}
                assert {[lindex $results 0] > 0}
                assert {[lindex $results 1] > 0}
            }
        }

        # Verify final counter values
        set final1 [r get "counter1"]
        set final2 [r get "counter2"]

        # Should have incremented 50 times total (5 clients * 10 transactions)
        assert_equal 50 $final1
        assert_equal 50 $final2

        # Clean up clients
        foreach client $clients {
            $client close
        }
    }
}
