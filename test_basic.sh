#!/bin/bash

# Basic test script for NFS implementation
echo "Starting NFS Basic Test..."

# Get the current IP address
IP=$(hostname -I | awk '{print $1}')
echo "Using IP: $IP"

# Kill any existing processes
pkill -f "./naming"
pkill -f "./storage"
pkill -f "./client"
sleep 2

# Start the naming server
echo "Starting Naming Server..."
./naming &
NAMING_PID=$!
sleep 3

# Start storage servers
echo "Starting Storage Server 1 on port 9091..."
./storage $IP 8090 9091 test_storage1 &
SS1_PID=$!
sleep 2

echo "Starting Storage Server 2 on port 9092..."
./storage $IP 8090 9092 test_storage2 &
SS2_PID=$!
sleep 2

echo "Starting Storage Server 3 on port 9093..."
./storage $IP 8090 9093 test_storage3 &
SS3_PID=$!
sleep 3

echo "All servers started. Testing basic functionality..."

# Test basic read operation
echo "Testing READ operation..."
echo "READ" | ./client $IP 8090 &
CLIENT_PID=$!
sleep 5

# Cleanup
echo "Cleaning up..."
kill $NAMING_PID $SS1_PID $SS2_PID $SS3_PID $CLIENT_PID 2>/dev/null
sleep 2

echo "Test completed."
