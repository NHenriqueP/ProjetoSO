#!/bin/bash

# Test script for the scheduling system

set -e

PROJ_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJ_DIR"

echo "====== Building project ======"
make clean >/dev/null 2>&1
make >/dev/null 2>&1

echo "====== Cleaning up old files ======"
# Kill any lingering controller processes
pkill -f "bin/controller" >/dev/null 2>&1 || true
sleep 1

echo "====== Starting controller with 2 parallelism, FCFS policy ======"
./bin/controller 2 FCFS &
CONTROLLER_PID=$!
sleep 1

echo "====== Running test scenario ======"
echo ""

echo "Test 1: User 1 executes 'echo hello'"
./bin/runner -e 1 "echo hello"
sleep 0.5

echo ""
echo "Test 2: User 2 executes 'sleep 1'"
./bin/runner -e 2 "sleep 1" &
SLEEP_RUNNER_PID=$!
sleep 0.5

echo "Test 3: User 1 executes 'date'"
./bin/runner -e 1 "date"

wait "$SLEEP_RUNNER_PID"

sleep 1

echo ""
echo "====== Checking controller status ======"
./bin/runner -c || true

sleep 1

echo ""
echo "====== Shutting down controller ======"
./bin/runner -s || true

# Wait for controller to actually exit.
wait "$CONTROLLER_PID" 2>/dev/null || true

sleep 2

echo ""
echo "====== Job execution log ======"
if [ -f "tmp/executions.log" ]; then
    echo "user_id,command_id,submit_ms,start_ms,end_ms,wait_ms,total_ms"
    cat tmp/executions.log
else
    echo "No execution log found"
fi

echo ""
echo "====== Test completed ======"
