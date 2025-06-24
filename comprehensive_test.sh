#!/bin/bash

# Comprehensive NFS Test Script
echo "=== NFS Comprehensive Test Suite ==="

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test results
TESTS_PASSED=0
TESTS_FAILED=0

# Function to print test results
print_result() {
    if [ $1 -eq 0 ]; then
        echo -e "${GREEN}✓ PASS${NC}: $2"
        ((TESTS_PASSED++))
    else
        echo -e "${RED}✗ FAIL${NC}: $2"
        ((TESTS_FAILED++))
    fi
}

# Function to cleanup processes
cleanup() {
    echo "Cleaning up processes..."
    pkill -f "./naming" 2>/dev/null
    pkill -f "./storage" 2>/dev/null
    pkill -f "./client" 2>/dev/null
    sleep 2
}

# Get IP address
IP=$(hostname -I | awk '{print $1}')
echo "Using IP: $IP"

# Cleanup any existing processes
cleanup

echo -e "\n${YELLOW}=== Phase 1: Basic Server Startup Tests ===${NC}"

# Test 1: Naming Server Startup
echo "Test 1: Starting Naming Server..."
./naming &
NAMING_PID=$!
sleep 3

if ps -p $NAMING_PID > /dev/null; then
    print_result 0 "Naming Server startup"
else
    print_result 1 "Naming Server startup"
    exit 1
fi

# Test 2: Storage Server Registration
echo "Test 2: Starting Storage Servers..."
./storage $IP 8090 9091 test_storage1 &
SS1_PID=$!
sleep 2

./storage $IP 8090 9092 test_storage2 &
SS2_PID=$!
sleep 2

./storage $IP 8090 9093 test_storage3 &
SS3_PID=$!
sleep 3

# Check if all storage servers are running
if ps -p $SS1_PID > /dev/null && ps -p $SS2_PID > /dev/null && ps -p $SS3_PID > /dev/null; then
    print_result 0 "Storage Servers startup"
else
    print_result 1 "Storage Servers startup"
fi

echo -e "\n${YELLOW}=== Phase 2: Basic File Operations ===${NC}"

# Test 3: READ operation
echo "Test 3: Testing READ operation..."
RESULT=$(echo -e "READ\ntest_storage1/file1.txt" | timeout 10 ./client $IP 8090 2>/dev/null | grep -o "hello my name is jainil lol")
if [ "$RESULT" = "hello my name is jainil lol" ]; then
    print_result 0 "READ operation"
else
    print_result 1 "READ operation"
fi

# Test 4: WRITE operation
echo "Test 4: Testing WRITE operation..."
echo -e "WRITE\ntest_storage1/file2.txt\nyes\nThis is a test write operation" | timeout 10 ./client $IP 8090 >/dev/null 2>&1
sleep 2

# Verify the write
RESULT=$(echo -e "READ\ntest_storage1/file2.txt" | timeout 10 ./client $IP 8090 2>/dev/null | grep -o "This is a test write operation")
if [ "$RESULT" = "This is a test write operation" ]; then
    print_result 0 "WRITE operation"
else
    print_result 1 "WRITE operation"
fi

# Test 5: Cross-server file access
echo "Test 5: Testing cross-server file access..."
RESULT=$(echo -e "READ\ntest_storage2/file4.txt" | timeout 10 ./client $IP 8090 2>/dev/null | grep -o "Hello from Storage 2")
if [ "$RESULT" = "Hello from Storage 2" ]; then
    print_result 0 "Cross-server file access"
else
    print_result 1 "Cross-server file access"
fi

echo -e "\n${YELLOW}=== Phase 3: Advanced Operations ===${NC}"

# Test 6: Multiple concurrent clients
echo "Test 6: Testing concurrent client access..."
(echo -e "READ\ntest_storage1/file1.txt" | ./client $IP 8090 >/dev/null 2>&1) &
(echo -e "READ\ntest_storage2/file4.txt" | ./client $IP 8090 >/dev/null 2>&1) &
(echo -e "READ\ntest_storage1/file2.txt" | ./client $IP 8090 >/dev/null 2>&1) &
wait
print_result 0 "Concurrent client access"

echo -e "\n${YELLOW}=== Test Summary ===${NC}"
echo "Tests Passed: $TESTS_PASSED"
echo "Tests Failed: $TESTS_FAILED"
echo "Total Tests: $((TESTS_PASSED + TESTS_FAILED))"

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed! NFS implementation is working correctly.${NC}"
else
    echo -e "${RED}Some tests failed. Check the implementation.${NC}"
fi

# Cleanup
cleanup
echo "Test completed."
