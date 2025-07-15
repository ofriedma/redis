# Redis io_uring Real-World Scenario Tests
# Tests that simulate real application usage patterns

# Test web application session storage scenario
start_server {tags {"uring real-world web-sessions"}} {
    test {Real-World: Web application session storage} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Simulate web application session management
            set num_users 100
            set session_duration 300 ;# 5 minutes in seconds
            
            # Create user sessions
            for {set user_id 1} {$user_id <= $num_users} {incr user_id} {
                set session_id "sess_[expr {int(rand() * 1000000)}]"
                set session_data [dict create \
                    user_id $user_id \
                    login_time [clock seconds] \
                    last_activity [clock seconds] \
                    ip_address "192.168.1.[expr {$user_id % 255}]" \
                    user_agent "Mozilla/5.0 (Test Browser)" \
                ]
                
                # Store session with expiration
                r setex "session:$session_id" $session_duration [dict_to_json $session_data]
                r set "user:$user_id:session" $session_id
            }
            
            # Simulate session access patterns
            for {set i 0} {$i < 500} {incr i} {
                set user_id [expr {int(rand() * $num_users) + 1}]
                set session_id [r get "user:$user_id:session"]
                
                if {$session_id ne ""} {
                    # Update session activity
                    set session_data [r get "session:$session_id"]
                    if {$session_data ne ""} {
                        set data_dict [json_to_dict $session_data]
                        dict set data_dict last_activity [clock seconds]
                        r setex "session:$session_id" $session_duration [dict_to_json $data_dict]
                    }
                }
            }
            
            # Verify session data integrity
            set valid_sessions 0
            for {set user_id 1} {$user_id <= $num_users} {incr user_id} {
                set session_id [r get "user:$user_id:session"]
                if {$session_id ne ""} {
                    set session_data [r get "session:$session_id"]
                    if {$session_data ne ""} {
                        incr valid_sessions
                    }
                }
            }
            
            assert {$valid_sessions >= [expr {$num_users * 0.8}]} ;# At least 80% should be valid
            
            # Check io_uring performance
            set final_info [r info uring]
            set success_rate [extract_info_field $final_info "uring_success_rate"]
            assert {$success_rate >= 95.0}
            
            # Clean up
            for {set user_id 1} {$user_id <= $num_users} {incr user_id} {
                set session_id [r get "user:$user_id:session"]
                if {$session_id ne ""} {
                    r del "session:$session_id"
                }
                r del "user:$user_id:session"
            }
        } else {
            skip "io_uring not available"
        }
    }
}

# Test real-time analytics scenario
start_server {tags {"uring real-world analytics"}} {
    test {Real-World: Real-time analytics data processing} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Simulate real-time analytics data ingestion
            set num_events 1000
            set start_time [clock milliseconds]
            
            # Ingest events
            for {set i 0} {$i < $num_events} {incr i} {
                set timestamp [clock seconds]
                set event_type [lindex {"page_view" "click" "purchase" "signup"} [expr {$i % 4}]]
                set user_id [expr {int(rand() * 10000)}]
                set value [expr {rand() * 100}]
                
                # Store event
                r lpush "events:$event_type" [dict_to_json [dict create \
                    timestamp $timestamp \
                    user_id $user_id \
                    value $value \
                    event_id $i \
                ]]
                
                # Update counters
                r incr "counter:$event_type:total"
                r incrbyfloat "counter:$event_type:value" $value
                
                # Update user activity
                r zadd "user_activity" $timestamp $user_id
                
                # Maintain sliding window (keep last 1000 events per type)
                r ltrim "events:$event_type" 0 999
            }
            
            set ingestion_time [expr {[clock milliseconds] - $start_time}]
            
            # Perform analytics queries
            set query_start [clock milliseconds]
            
            # Get event counts
            set page_views [r get "counter:page_view:total"]
            set clicks [r get "counter:click:total"]
            set purchases [r get "counter:purchase:total"]
            set signups [r get "counter:signup:total"]
            
            # Get recent events
            set recent_purchases [r lrange "events:purchase" 0 9]
            
            # Get active users (last hour)
            set hour_ago [expr {[clock seconds] - 3600}]
            set active_users [r zcount "user_activity" $hour_ago "+inf"]
            
            set query_time [expr {[clock milliseconds] - $query_start}]
            
            # Verify data integrity
            assert {[expr {$page_views + $clicks + $purchases + $signups}] == $num_events}
            assert {[llength $recent_purchases] <= 10}
            assert {$active_users > 0}
            
            # Performance checks
            assert {$ingestion_time < 5000} ;# Less than 5 seconds for ingestion
            assert {$query_time < 1000}     ;# Less than 1 second for queries
            
            # Check io_uring statistics
            set final_info [r info uring]
            set avg_completion_time [extract_info_field $final_info "uring_avg_completion_time_us"]
            assert {$avg_completion_time < 10000} ;# Less than 10ms average
            
            # Clean up
            r del "counter:page_view:total" "counter:click:total" "counter:purchase:total" "counter:signup:total"
            r del "counter:page_view:value" "counter:click:value" "counter:purchase:value" "counter:signup:value"
            r del "events:page_view" "events:click" "events:purchase" "events:signup"
            r del "user_activity"
        }
    }
}

# Test caching layer scenario
start_server {tags {"uring real-world caching"}} {
    test {Real-World: Application caching layer} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Simulate application caching patterns
            set cache_keys {}
            set cache_hit_count 0
            set cache_miss_count 0
            
            # Populate cache with frequently accessed data
            for {set i 0} {$i < 200} {incr i} {
                set key "cache:item:$i"
                set data [dict_to_json [dict create \
                    id $i \
                    name "Item $i" \
                    description "Description for item $i" \
                    price [expr {rand() * 1000}] \
                    category [lindex {"electronics" "books" "clothing" "home"} [expr {$i % 4}]] \
                ]]
                
                r setex $key 3600 $data ;# 1 hour TTL
                lappend cache_keys $key
            }
            
            # Simulate cache access patterns (80/20 rule - 80% of requests for 20% of data)
            for {set request 0} {$request < 1000} {incr request} {
                if {[expr {rand()}] < 0.8} {
                    # 80% of requests go to top 20% of items (hot data)
                    set key_index [expr {int(rand() * 40)}] ;# Top 20% of 200 items
                } else {
                    # 20% of requests go to remaining 80% of items (cold data)
                    set key_index [expr {40 + int(rand() * 160)}]
                }
                
                set key [lindex $cache_keys $key_index]
                set cached_data [r get $key]
                
                if {$cached_data ne ""} {
                    incr cache_hit_count
                    # Simulate cache hit processing
                    set data_dict [json_to_dict $cached_data]
                    # Update access count
                    r incr "access_count:$key"
                } else {
                    incr cache_miss_count
                    # Simulate cache miss - reload data
                    set new_data [dict_to_json [dict create \
                        id $key_index \
                        name "Reloaded Item $key_index" \
                        description "Reloaded description" \
                        price [expr {rand() * 1000}] \
                        category "reloaded" \
                    ]]
                    r setex $key 3600 $new_data
                }
            }
            
            # Calculate cache hit rate
            set total_requests [expr {$cache_hit_count + $cache_miss_count}]
            set hit_rate [expr {double($cache_hit_count) / $total_requests * 100}]
            
            # Cache hit rate should be reasonable for this access pattern
            assert {$hit_rate >= 70.0} ;# At least 70% hit rate
            
            # Check io_uring buffer efficiency
            set final_info [r info uring]
            set buffer_hit_rate [extract_info_field $final_info "buffer_pool_hit_rate"]
            if {$buffer_hit_rate ne ""} {
                assert {$buffer_hit_rate >= 50.0} ;# Buffer pool should be effective
            }
            
            # Clean up
            foreach key $cache_keys {
                r del $key "access_count:$key"
            }
        }
    }
}

# Test message queue scenario
start_server {tags {"uring real-world messaging"}} {
    test {Real-World: Message queue processing} {
        set info [r info uring]
        if {[string length $info] > 0} {
            # Simulate message queue with multiple producers and consumers
            set num_producers 5
            set num_consumers 3
            set messages_per_producer 100
            
            # Producer simulation - add messages to queues
            for {set producer 0} {$producer < $num_producers} {incr producer} {
                for {set msg 0} {$msg < $messages_per_producer} {incr msg} {
                    set message [dict_to_json [dict create \
                        id "${producer}_${msg}" \
                        producer_id $producer \
                        timestamp [clock milliseconds] \
                        payload "Message payload from producer $producer, message $msg" \
                        priority [expr {int(rand() * 5)}] \
                    ]]
                    
                    # Add to appropriate queue based on priority
                    set priority [expr {int(rand() * 5)}]
                    r lpush "queue:priority:$priority" $message
                    r incr "stats:producer:$producer:sent"
                }
            }
            
            # Consumer simulation - process messages from queues
            set total_processed 0
            for {set consumer 0} {$consumer < $num_consumers} {incr consumer} {
                set processed_by_consumer 0
                
                # Process high priority messages first
                for {set priority 4} {$priority >= 0} {incr priority -1} {
                    while {[r llen "queue:priority:$priority"] > 0} {
                        set message [r rpop "queue:priority:$priority"]
                        if {$message ne ""} {
                            # Simulate message processing
                            set msg_dict [json_to_dict $message]
                            set msg_id [dict get $msg_dict id]
                            
                            # Mark as processed
                            r set "processed:$msg_id" $consumer
                            incr processed_by_consumer
                            incr total_processed
                            
                            # Simulate processing time
                            if {$processed_by_consumer % 50 == 0} {
                                # Occasional yield to allow other operations
                                after 1
                            }
                        }
                    }
                }
                
                r set "stats:consumer:$consumer:processed" $processed_by_consumer
            }
            
            # Verify all messages were processed
            set total_sent [expr {$num_producers * $messages_per_producer}]
            assert {$total_processed == $total_sent}
            
            # Check queue processing efficiency
            for {set priority 0} {$priority < 5} {incr priority} {
                assert {[r llen "queue:priority:$priority"] == 0}
            }
            
            # Check io_uring operation statistics
            set final_info [r info uring]
            set ops_completed [extract_info_field $final_info "uring_ops_completed"]
            assert {$ops_completed > $total_sent}
            
            # Clean up
            for {set priority 0} {$priority < 5} {incr priority} {
                r del "queue:priority:$priority"
            }
            for {set producer 0} {$producer < $num_producers} {incr producer} {
                r del "stats:producer:$producer:sent"
            }
            for {set consumer 0} {$consumer < $num_consumers} {incr consumer} {
                r del "stats:consumer:$consumer:processed"
            }
            # Clean up processed message markers
            for {set producer 0} {$producer < $num_producers} {incr producer} {
                for {set msg 0} {$msg < $messages_per_producer} {incr msg} {
                    r del "processed:${producer}_${msg}"
                }
            }
        }
    }
}

# Helper functions for JSON-like data handling
proc dict_to_json {dict_data} {
    # Simple dict to JSON conversion for testing
    set json "{"
    set first 1
    dict for {key value} $dict_data {
        if {!$first} {
            append json ","
        }
        append json "\"$key\":\"$value\""
        set first 0
    }
    append json "}"
    return $json
}

proc json_to_dict {json_data} {
    # Simple JSON to dict conversion for testing
    set dict_data {}
    # This is a simplified parser for testing purposes
    regsub -all {[{}\"]} $json_data "" clean_data
    foreach pair [split $clean_data ","] {
        set kv [split $pair ":"]
        if {[llength $kv] == 2} {
            dict set dict_data [lindex $kv 0] [lindex $kv 1]
        }
    }
    return $dict_data
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
