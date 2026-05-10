#!/bin/bash

# Simple end-to-end test

PROJ_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJ_DIR"

# Kill any old processes
killall -9 controller 2>/dev/null || true
killall -9 runner 2>/dev/null || true
sleep 1

# Clean up old FIFOs and logs
echo "(skipping FIFO cleanup)"

echo "=== Starting Controller (1 parallel, FCFS) ==="
./controller 1 FCFS &
CONTROLLER_PID=$!
echo "Controller PID: $CONTROLLER_PID"

# Give controller time to create its FIFO
sleep 2

# Check if controller FIFO exists
if [ -p "/tmp/so_controller_fifo" ]; then
    echo "✓ Controller FIFO created"
else
    echo "✗ Controller FIFO not found"
    exit 1
fi

echo ""
echo "=== Test 1: Simple command execution ==="
./runner -e 1 "echo 'Hello from runner'"
sleep 1

echo ""
echo "=== Test 2: Longer command (sleep) ==="
./runner -e 2 "sleep 2"
sleep 3

echo ""
echo "=== Test 3: Check status ==="
./runner -c
sleep 1

echo ""
echo "=== Test 4: Shutdown ==="
./runner -s
sleep 2

echo ""
echo "=== Results from execution log ==="
if [ -f "tmp/executions.log" ]; then
    echo "Number of completed jobs:"
    wc -l < tmp/executions.log
    echo ""
    echo "Log contents:"
    cat tmp/executions.log
else
    echo "No execution log found"
fi

# Kill controller if still running
killall controller 2>/dev/null || true
