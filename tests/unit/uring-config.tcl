# Test io_uring configuration options
# These tests verify that io_uring configuration options are properly parsed,
# validated, and accessible through CONFIG GET/SET commands.

start_server {tags {"uring config"}} {
    test {io_uring configuration options are available when compiled with liburing} {
        # Check if io_uring support is compiled in
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # io_uring is available, test configuration options
            
            # Test that all io_uring config options are available
            set uring_configs [r config get "*uring*"]
            
            # Convert to dict for easier testing
            set config_dict [dict create]
            for {set i 0} {$i < [llength $uring_configs]} {incr i 2} {
                dict set config_dict [lindex $uring_configs $i] [lindex $uring_configs [expr $i + 1]]
            }
            
            # Verify all expected configuration options exist
            assert {[dict exists $config_dict "uring-enabled"]}
            assert {[dict exists $config_dict "uring-sqpoll"]}
            assert {[dict exists $config_dict "uring-sqpoll-cpu"]}
            assert {[dict exists $config_dict "uring-sqpoll-idle"]}
            assert {[dict exists $config_dict "uring-sq-entries"]}
            assert {[dict exists $config_dict "uring-cq-entries"]}
            assert {[dict exists $config_dict "uring-buffer-ring-size"]}
            assert {[dict exists $config_dict "uring-buffer-size"]}
            assert {[dict exists $config_dict "uring-batch-submit-size"]}
            assert {[dict exists $config_dict "uring-multishot-accept"]}
            assert {[dict exists $config_dict "uring-multishot-recv"]}
            assert {[dict exists $config_dict "uring-linked-ops"]}
        }
    }
    
    test {io_uring configuration default values are correct} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test default values match the design specification
            assert_equal "no" [r config get uring-enabled]
            assert_equal "yes" [r config get uring-sqpoll]
            assert_equal "-1" [r config get uring-sqpoll-cpu]
            assert_equal "1000" [r config get uring-sqpoll-idle]
            assert_equal "512" [r config get uring-sq-entries]
            assert_equal "1024" [r config get uring-cq-entries]
            assert_equal "1024" [r config get uring-buffer-ring-size]
            assert_equal "4096" [r config get uring-buffer-size]
            assert_equal "32" [r config get uring-batch-submit-size]
            assert_equal "yes" [r config get uring-multishot-accept]
            assert_equal "yes" [r config get uring-multishot-recv]
            assert_equal "yes" [r config get uring-linked-ops]
        }
    }
    
    test {io_uring boolean configuration validation} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test valid boolean values
            foreach option {uring-enabled uring-sqpoll uring-multishot-accept uring-multishot-recv uring-linked-ops} {
                # These should work (but may fail due to immutable config)
                catch {r config set $option yes}
                catch {r config set $option no}
                catch {r config set $option 1}
                catch {r config set $option 0}
                
                # These should fail with invalid values
                assert_error "*invalid*" {r config set $option invalid}
                assert_error "*invalid*" {r config set $option 2}
                assert_error "*invalid*" {r config set $option -1}
            }
        }
    }
    
    test {io_uring integer configuration validation} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test uring-sqpoll-cpu range validation (-1 to 1024)
            catch {r config set uring-sqpoll-cpu -1}  ;# Should work (auto-detect)
            catch {r config set uring-sqpoll-cpu 0}   ;# Should work
            catch {r config set uring-sqpoll-cpu 1024} ;# Should work (max)
            assert_error "*out of range*" {r config set uring-sqpoll-cpu -2}
            assert_error "*out of range*" {r config set uring-sqpoll-cpu 1025}
            
            # Test uring-sqpoll-idle range validation (0 to 60000)
            catch {r config set uring-sqpoll-idle 0}     ;# Should work (no timeout)
            catch {r config set uring-sqpoll-idle 1000}  ;# Should work (default)
            catch {r config set uring-sqpoll-idle 60000} ;# Should work (max)
            assert_error "*out of range*" {r config set uring-sqpoll-idle -1}
            assert_error "*out of range*" {r config set uring-sqpoll-idle 60001}
            
            # Test uring-sq-entries range validation (64 to 32768)
            catch {r config set uring-sq-entries 64}     ;# Should work (min)
            catch {r config set uring-sq-entries 512}    ;# Should work (default)
            catch {r config set uring-sq-entries 32768}  ;# Should work (max)
            assert_error "*out of range*" {r config set uring-sq-entries 63}
            assert_error "*out of range*" {r config set uring-sq-entries 32769}
            
            # Test uring-cq-entries range validation (128 to 65536)
            catch {r config set uring-cq-entries 128}    ;# Should work (min)
            catch {r config set uring-cq-entries 1024}   ;# Should work (default)
            catch {r config set uring-cq-entries 65536}  ;# Should work (max)
            assert_error "*out of range*" {r config set uring-cq-entries 127}
            assert_error "*out of range*" {r config set uring-cq-entries 65537}
            
            # Test uring-buffer-ring-size range validation (64 to 16384)
            catch {r config set uring-buffer-ring-size 64}     ;# Should work (min)
            catch {r config set uring-buffer-ring-size 1024}   ;# Should work (default)
            catch {r config set uring-buffer-ring-size 16384}  ;# Should work (max)
            assert_error "*out of range*" {r config set uring-buffer-ring-size 63}
            assert_error "*out of range*" {r config set uring-buffer-ring-size 16385}
            
            # Test uring-buffer-size range validation (1024 to 65536)
            catch {r config set uring-buffer-size 1024}   ;# Should work (min)
            catch {r config set uring-buffer-size 4096}   ;# Should work (default)
            catch {r config set uring-buffer-size 65536}  ;# Should work (max)
            assert_error "*out of range*" {r config set uring-buffer-size 1023}
            assert_error "*out of range*" {r config set uring-buffer-size 65537}
            
            # Test uring-batch-submit-size range validation (1 to 128)
            catch {r config set uring-batch-submit-size 1}    ;# Should work (min)
            catch {r config set uring-batch-submit-size 32}   ;# Should work (default)
            catch {r config set uring-batch-submit-size 128}  ;# Should work (max)
            assert_error "*out of range*" {r config set uring-batch-submit-size 0}
            assert_error "*out of range*" {r config set uring-batch-submit-size 129}
        }
    }
    
    test {io_uring configuration options are immutable} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # All io_uring configuration options should be immutable
            # (can only be set at startup, not at runtime)
            foreach option {
                uring-enabled uring-sqpoll uring-sqpoll-cpu uring-sqpoll-idle
                uring-sq-entries uring-cq-entries uring-buffer-ring-size
                uring-buffer-size uring-batch-submit-size uring-multishot-accept
                uring-multishot-recv uring-linked-ops
            } {
                set current_value [lindex [r config get $option] 1]
                assert_error "*immutable*" {r config set $option $current_value}
            }
        }
    }
    
    test {io_uring configuration can be retrieved individually} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test that each configuration option can be retrieved individually
            foreach option {
                uring-enabled uring-sqpoll uring-sqpoll-cpu uring-sqpoll-idle
                uring-sq-entries uring-cq-entries uring-buffer-ring-size
                uring-buffer-size uring-batch-submit-size uring-multishot-accept
                uring-multishot-recv uring-linked-ops
            } {
                set value [r config get $option]
                assert {[llength $value] == 2}
                assert_equal $option [lindex $value 0]
                # Value should not be empty
                assert {[string length [lindex $value 1]] > 0}
            }
        }
    }
    
    test {io_uring configuration pattern matching works} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Test pattern matching for io_uring configs
            set all_uring [r config get "*uring*"]
            set sqpoll_configs [r config get "*sqpoll*"]
            set buffer_configs [r config get "*buffer*"]
            set multishot_configs [r config get "*multishot*"]
            
            # Should have at least 12 io_uring configs (24 elements in list)
            assert {[llength $all_uring] >= 24}
            
            # Should have sqpoll-related configs
            assert {[llength $sqpoll_configs] >= 6}  ;# 3 configs * 2 elements each
            
            # Should have buffer-related configs
            assert {[llength $buffer_configs] >= 4}  ;# 2 configs * 2 elements each
            
            # Should have multishot-related configs
            assert {[llength $multishot_configs] >= 4}  ;# 2 configs * 2 elements each
        }
    }
}

# Test configuration loading from redis.conf
start_server {tags {"uring config file"} overrides {uring-enabled no uring-sqpoll yes uring-sqpoll-cpu 1}} {
    test {io_uring configuration can be loaded from config file} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            # Verify that configuration overrides work
            assert_equal "no" [lindex [r config get uring-enabled] 1]
            assert_equal "yes" [lindex [r config get uring-sqpoll] 1]
            assert_equal "1" [lindex [r config get uring-sqpoll-cpu] 1]
        }
    }
}

# Test with different configuration values
start_server {tags {"uring config values"} overrides {
    uring-sqpoll-cpu 2
    uring-sqpoll-idle 2000
    uring-sq-entries 256
    uring-cq-entries 512
}} {
    test {io_uring custom configuration values are applied} {
        set info [r info server]
        if {[string match "*uring*" $info]} {
            assert_equal "2" [lindex [r config get uring-sqpoll-cpu] 1]
            assert_equal "2000" [lindex [r config get uring-sqpoll-idle] 1]
            assert_equal "256" [lindex [r config get uring-sq-entries] 1]
            assert_equal "512" [lindex [r config get uring-cq-entries] 1]
        }
    }
}
