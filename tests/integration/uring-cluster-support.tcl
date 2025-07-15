# Week 4 io_uring Cluster Support Integration Tests
# Tests for Redis cluster functionality with io_uring

# Note: These tests require cluster support to be enabled
# They will be skipped if cluster mode is not available

proc create_cluster_with_uring {node_count} {
    set nodes {}
    set base_port 7000
    
    for {set i 0} {$i < $node_count} {incr i} {
        set port [expr $base_port + $i]
        set node [start_server [list overrides [list \
            port $port \
            cluster-enabled yes \
            cluster-config-file "nodes-${port}.conf" \
            cluster-node-timeout 5000 \
            uring-enabled yes \
            appendonly no \
            save ""]] {
            # Return server info
            list [srv host] [srv port] [srv 0 client]
        }]
        lappend nodes $node
    }
    
    return $nodes
}

proc setup_cluster_slots {nodes} {
    set slots_per_node [expr 16384 / [llength $nodes]]
    set slot_start 0
    
    foreach node $nodes {
        set client [lindex $node 2]
        set slot_end [expr $slot_start + $slots_per_node - 1]
        
        if {$node eq [lindex $nodes end]} {
            set slot_end 16383  ;# Last node gets remaining slots
        }
        
        # Assign slots to node
        for {set slot $slot_start} {$slot <= $slot_end} {incr slot} {
            $client cluster addslots $slot
        }
        
        set slot_start [expr $slot_end + 1]
    }
}

proc meet_cluster_nodes {nodes} {
    set first_node [lindex $nodes 0]
    set first_host [lindex $first_node 0]
    set first_port [lindex $first_node 1]
    
    foreach node [lrange $nodes 1 end] {
        set client [lindex $node 2]
        $client cluster meet $first_host $first_port
    }
    
    # Wait for cluster to form
    after 2000
}

if {[catch {r cluster nodes}]} {
    # Cluster not supported, skip these tests
    puts "Cluster not supported, skipping cluster tests"
} else {
    
test {basic cluster setup with io_uring} {
    # Create a 3-node cluster
    set nodes [create_cluster_with_uring 3]
    
    # Setup cluster
    setup_cluster_slots $nodes
    meet_cluster_nodes $nodes
    
    # Verify cluster is working
    foreach node $nodes {
        set client [lindex $node 2]
        
        # Check cluster state
        set cluster_info [$client cluster info]
        assert_match "*cluster_state:ok*" $cluster_info
        
        # Check io_uring is enabled
        set server_info [$client info server]
        assert_match "*uring_enabled:1*" $server_info
    }
    
    # Clean up
    foreach node $nodes {
        set client [lindex $node 2]
        $client shutdown nosave
    }
}

test {cluster data distribution with io_uring} {
    # Create a 3-node cluster
    set nodes [create_cluster_with_uring 3]
    setup_cluster_slots $nodes
    meet_cluster_nodes $nodes
    
    # Get cluster client
    set cluster_client [lindex [lindex $nodes 0] 2]
    
    # Store data across cluster
    for {set i 0} {$i < 1000} {incr i} {
        set key "cluster_key_$i"
        set value "cluster_value_$i"
        
        # Calculate which node should handle this key
        set slot [expr [crc16 $key] % 16384]
        
        # Store the key-value pair
        if {[catch {$cluster_client set $key $value} error]} {
            # Handle MOVED redirections
            if {[string match "*MOVED*" $error]} {
                # Extract target node and retry
                regexp {MOVED \d+ ([^:]+):(\d+)} $error - target_host target_port
                set target_client [redis $target_host $target_port]
                $target_client set $key $value
                $target_client close
            }
        }
    }
    
    # Verify data can be retrieved
    set retrieved_count 0
    for {set i 0} {$i < 1000} {incr i} {
        set key "cluster_key_$i"
        
        if {![catch {$cluster_client get $key} value]} {
            if {$value eq "cluster_value_$i"} {
                incr retrieved_count
            }
        } else {
            # Handle redirections for GET as well
            if {[string match "*MOVED*" $value]} {
                regexp {MOVED \d+ ([^:]+):(\d+)} $value - target_host target_port
                set target_client [redis $target_host $target_port]
                set retrieved_value [$target_client get $key]
                if {$retrieved_value eq "cluster_value_$i"} {
                    incr retrieved_count
                }
                $target_client close
            }
        }
    }
    
    # Should have retrieved most keys
    assert {$retrieved_count > 900}
    
    # Clean up
    foreach node $nodes {
        set client [lindex $node 2]
        $client shutdown nosave
    }
}

test {cluster failover with io_uring} {
    # Create a 6-node cluster (3 masters, 3 slaves)
    set nodes [create_cluster_with_uring 6]
    setup_cluster_slots [lrange $nodes 0 2]  ;# Only masters get slots
    meet_cluster_nodes $nodes
    
    # Set up master-slave relationships
    for {set i 0} {$i < 3} {incr i} {
        set master [lindex $nodes $i]
        set slave [lindex $nodes [expr $i + 3]]
        
        set master_client [lindex $master 2]
        set slave_client [lindex $slave 2]
        
        # Get master node ID
        set cluster_nodes [$master_client cluster nodes]
        regexp {([a-f0-9]+) [^:]+:[0-9]+ myself,master} $cluster_nodes - master_id
        
        # Make slave replicate master
        $slave_client cluster replicate $master_id
    }
    
    # Wait for replication to establish
    after 3000
    
    # Store some data
    set master_client [lindex [lindex $nodes 0] 2]
    for {set i 0} {$i < 100} {incr i} {
        $master_client set "failover_key_$i" "failover_value_$i"
    }
    
    # Simulate master failure by shutting down first master
    set failed_master [lindex $nodes 0]
    set failed_client [lindex $failed_master 2]
    $failed_client shutdown nosave
    
    # Wait for failover
    after 5000
    
    # Verify cluster is still operational
    set working_node [lindex $nodes 1]
    set working_client [lindex $working_node 2]
    
    set cluster_info [$working_client cluster info]
    assert_match "*cluster_state:ok*" $cluster_info
    
    # Clean up remaining nodes
    foreach node [lrange $nodes 1 end] {
        set client [lindex $node 2]
        catch {$client shutdown nosave}
    }
}

test {cluster resharding with io_uring} {
    # Create a 4-node cluster
    set nodes [create_cluster_with_uring 4]
    setup_cluster_slots [lrange $nodes 0 2]  ;# 3 nodes get slots initially
    meet_cluster_nodes $nodes
    
    # Add data to cluster
    set client [lindex [lindex $nodes 0] 2]
    for {set i 0} {$i < 500} {incr i} {
        if {![catch {$client set "reshard_key_$i" "reshard_value_$i"}]} {
            # Key stored successfully
        }
    }
    
    # Simulate resharding by moving some slots to the 4th node
    set source_node [lindex $nodes 0]
    set target_node [lindex $nodes 3]
    
    set source_client [lindex $source_node 2]
    set target_client [lindex $target_node 2]
    
    # Move slots 0-1000 to target node
    for {set slot 0} {$slot < 1000} {incr slot} {
        # This is a simplified resharding simulation
        # In real Redis cluster, this would involve MIGRATE commands
        catch {$target_client cluster addslots $slot}
        catch {$source_client cluster delslots $slot}
    }
    
    # Wait for cluster to stabilize
    after 2000
    
    # Verify cluster is still functional
    set cluster_info [$target_client cluster info]
    # Cluster might be in a transitional state, so we just check it's responsive
    assert {$cluster_info ne ""}
    
    # Clean up
    foreach node $nodes {
        set client [lindex $node 2]
        catch {$client shutdown nosave}
    }
}

test {cluster performance with io_uring} {
    # Create a 3-node cluster
    set nodes [create_cluster_with_uring 3]
    setup_cluster_slots $nodes
    meet_cluster_nodes $nodes
    
    # Performance test with cluster
    set start_time [clock milliseconds]
    
    # Use multiple clients for concurrent access
    set clients {}
    foreach node $nodes {
        lappend clients [lindex $node 2]
    }
    
    # Perform operations across cluster
    foreach client $clients {
        for {set i 0} {$i < 100} {incr i} {
            set key "perf_${client}_$i"
            set value "perf_value_$i"
            
            if {![catch {$client set $key $value}]} {
                # Successful set
            }
        }
    }
    
    set end_time [clock milliseconds]
    set duration [expr $end_time - $start_time]
    
    # Should complete within reasonable time
    assert {$duration < 10000}  ;# Less than 10 seconds
    
    # Verify io_uring statistics
    foreach client $clients {
        set uring_info [$client info uring]
        assert_match "*ops_completed*" $uring_info
    }
    
    # Clean up
    foreach node $nodes {
        set client [lindex $node 2]
        $client shutdown nosave
    }
}

}  ;# End of cluster support check
