#!/usr/bin/env python3

import socket
import time
import sys

def test_redis_connection():
    """Test Redis connection and basic operations"""
    try:
        # Connect to Redis
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(('localhost', 6379))
        
        # Test PING
        sock.send(b'PING\r\n')
        response = sock.recv(1024)
        print(f"PING response: {response.decode().strip()}")
        
        # Test SET
        sock.send(b'SET test_key "test_value"\r\n')
        response = sock.recv(1024)
        print(f"SET response: {response.decode().strip()}")
        
        # Test GET
        sock.send(b'GET test_key\r\n')
        response = sock.recv(1024)
        print(f"GET response: {response.decode().strip()}")
        
        # Test INFO uring
        sock.send(b'INFO uring\r\n')
        response = sock.recv(4096)
        info_response = response.decode().strip()
        print(f"INFO uring response length: {len(info_response)} bytes")
        
        # Check for non-polling statistics
        if 'completion_batches' in info_response:
            print("✓ Found completion_batches - non-polling approach working")
        else:
            print("✗ completion_batches not found")
            
        if 'completion_processing_calls' in info_response:
            print("✓ Found completion_processing_calls - event-driven approach working")
        else:
            print("✗ completion_processing_calls not found")
            
        # Check that old polling stats are not present
        if 'poll_calls' not in info_response:
            print("✓ Old polling statistics removed")
        else:
            print("✗ Old polling statistics still present")
        
        sock.close()
        return True
        
    except Exception as e:
        print(f"Error: {e}")
        return False

def performance_test():
    """Simple performance test"""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(('localhost', 6379))
        
        start_time = time.time()
        
        # Perform multiple operations
        for i in range(100):
            sock.send(f'SET perf_test_{i} "value_{i}"\r\n'.encode())
            response = sock.recv(1024)
            
        for i in range(100):
            sock.send(f'GET perf_test_{i}\r\n'.encode())
            response = sock.recv(1024)
            
        end_time = time.time()
        duration = end_time - start_time
        
        print(f"Performance test: 200 operations in {duration:.3f} seconds")
        print(f"Operations per second: {200/duration:.1f}")
        
        sock.close()
        return True
        
    except Exception as e:
        print(f"Performance test error: {e}")
        return False

if __name__ == "__main__":
    print("Testing Redis io_uring non-polling implementation")
    print("=" * 50)
    
    if test_redis_connection():
        print("\n✓ Basic connection test passed")
    else:
        print("\n✗ Basic connection test failed")
        sys.exit(1)
    
    if performance_test():
        print("✓ Performance test passed")
    else:
        print("✗ Performance test failed")
        
    print("\nTest completed!")
