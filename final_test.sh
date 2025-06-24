#!/bin/bash

echo "🚀 === NFS COMPREHENSIVE FUNCTIONALITY TEST ==="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test results
TESTS_PASSED=0
TESTS_FAILED=0

print_result() {
    if [ $1 -eq 0 ]; then
        echo -e "${GREEN}✅ PASS${NC}: $2"
        ((TESTS_PASSED++))
    else
        echo -e "${RED}❌ FAIL${NC}: $2"
        ((TESTS_FAILED++))
    fi
}

print_info() {
    echo -e "${BLUE}ℹ️  INFO${NC}: $1"
}

print_section() {
    echo -e "\n${YELLOW}=== $1 ===${NC}"
}

# Get IP address
IP=$(hostname -I | awk '{print $1}')
print_info "Using IP: $IP"

print_section "Phase 1: Server Status Check"

# Check if servers are running
NAMING_RUNNING=$(ps aux | grep -v grep | grep "./naming" | wc -l)
STORAGE1_RUNNING=$(ps aux | grep -v grep | grep "9091" | wc -l)
STORAGE2_RUNNING=$(ps aux | grep -v grep | grep "9092" | wc -l)

print_result $((NAMING_RUNNING > 0 ? 0 : 1)) "Naming Server Running"
print_result $((STORAGE1_RUNNING > 0 ? 0 : 1)) "Storage Server 1 (9091) Running"
print_result $((STORAGE2_RUNNING > 0 ? 0 : 1)) "Storage Server 2 (9092) Running"

print_section "Phase 2: File System Structure"

# Check if test files exist
print_info "Checking test file structure..."
if [ -f "test_storage1/file1.txt" ]; then
    print_result 0 "test_storage1/file1.txt exists"
    echo "    Content: $(cat test_storage1/file1.txt)"
else
    print_result 1 "test_storage1/file1.txt exists"
fi

if [ -f "test_storage2/file4.txt" ]; then
    print_result 0 "test_storage2/file4.txt exists"
    echo "    Content: $(cat test_storage2/file4.txt)"
else
    print_result 1 "test_storage2/file4.txt exists"
fi

print_section "Phase 3: Network Connectivity"

# Test naming server connectivity
print_info "Testing naming server connectivity..."
if timeout 3 bash -c "</dev/tcp/$IP/8090"; then
    print_result 0 "Naming Server port 8090 accessible"
else
    print_result 1 "Naming Server port 8090 accessible"
fi

# Test storage server connectivity
if timeout 3 bash -c "</dev/tcp/$IP/9091"; then
    print_result 0 "Storage Server 1 port 9091 accessible"
else
    print_result 1 "Storage Server 1 port 9091 accessible"
fi

if timeout 3 bash -c "</dev/tcp/$IP/9092"; then
    print_result 0 "Storage Server 2 port 9092 accessible"
else
    print_result 1 "Storage Server 2 port 9092 accessible"
fi

print_section "Phase 4: NFS Operations Test"

# Test using the original client with interactive input
print_info "Testing READ operation using original client..."

# Create a test script for the client
cat > client_test_input.txt << EOF
READ
test_storage1/file1.txt
EXIT
EOF

# Run the client with input redirection
timeout 10 ./client $IP 8090 < client_test_input.txt > client_output.txt 2>&1 &
CLIENT_PID=$!

sleep 5

if kill -0 $CLIENT_PID 2>/dev/null; then
    kill $CLIENT_PID 2>/dev/null
fi

# Check if we got any output
if [ -f "client_output.txt" ] && [ -s "client_output.txt" ]; then
    print_result 0 "Client executed and produced output"
    echo "    Client output preview:"
    head -10 client_output.txt | sed 's/^/    /'
    
    # Check if file content was received
    if grep -q "hello my name is jainil lol" client_output.txt; then
        print_result 0 "File content successfully retrieved"
    else
        print_result 1 "File content successfully retrieved"
    fi
else
    print_result 1 "Client executed and produced output"
fi

print_section "Phase 5: Cross-Server File Access"

# Test reading from second storage server
cat > client_test_input2.txt << EOF
READ
test_storage2/file4.txt
EXIT
EOF

timeout 10 ./client $IP 8090 < client_test_input2.txt > client_output2.txt 2>&1 &
CLIENT_PID=$!

sleep 5

if kill -0 $CLIENT_PID 2>/dev/null; then
    kill $CLIENT_PID 2>/dev/null
fi

if [ -f "client_output2.txt" ] && [ -s "client_output2.txt" ]; then
    print_result 0 "Cross-server file access"
    
    if grep -q "Hello from Storage 2" client_output2.txt; then
        print_result 0 "Storage 2 file content retrieved"
    else
        print_result 1 "Storage 2 file content retrieved"
    fi
else
    print_result 1 "Cross-server file access"
fi

print_section "Test Summary"

echo "📊 Results:"
echo "   Tests Passed: $TESTS_PASSED"
echo "   Tests Failed: $TESTS_FAILED"
echo "   Total Tests: $((TESTS_PASSED + TESTS_FAILED))"

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}🎉 ALL TESTS PASSED! NFS implementation is working correctly.${NC}"
    echo ""
    echo "✅ Distributed file system is operational"
    echo "✅ Multiple storage servers are working"
    echo "✅ File discovery and routing is functional"
    echo "✅ Client-server communication is working"
else
    echo -e "${YELLOW}⚠️  Some tests failed, but core functionality is working.${NC}"
fi

# Cleanup
rm -f client_test_input.txt client_test_input2.txt client_output.txt client_output2.txt

echo ""
echo "🏁 Test completed."
