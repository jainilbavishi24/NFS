#!/bin/bash

echo "=== Simple NFS Test ==="

# Get IP
IP=$(hostname -I | awk '{print $1}')
echo "Using IP: $IP"

# Start naming server
echo "Starting naming server..."
./naming &
NAMING_PID=$!
sleep 3

# Start storage servers
echo "Starting storage servers..."
./storage $IP 8090 9091 test_storage1 &
SS1_PID=$!
sleep 2

./storage $IP 8090 9092 test_storage2 &
SS2_PID=$!
sleep 3

echo "Servers started. Testing basic READ operation..."

# Test READ
echo "Testing READ from storage1..."
echo -e "READ\ntest_storage1/file1.txt" | timeout 10 ./client $IP 8090

echo -e "\nTesting READ from storage2..."
echo -e "READ\ntest_storage2/file4.txt" | timeout 10 ./client $IP 8090

echo -e "\nTesting WRITE operation..."
echo -e "WRITE\ntest_storage1/file2.txt\nyes\nHello from test script" | timeout 10 ./client $IP 8090

echo -e "\nVerifying WRITE by reading back..."
echo -e "READ\ntest_storage1/file2.txt" | timeout 10 ./client $IP 8090

echo -e "\nCleaning up..."
kill $NAMING_PID $SS1_PID $SS2_PID 2>/dev/null
sleep 2

echo "Test completed."
