#!/bin/bash
set -e

PROJ_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJ_DIR"

echo "=== Building ==="
make clean >/dev/null 2>&1
make >/dev/null 2>&1

echo "=== Killing old processes ==="
killall -9 controller runner 2>/dev/null || true
echo "(skipping FIFO cleanup)"
cd "$PROJ_DIR"
sleep 1

echo "=== Starting controller (1 parallel, FCFS) ==="
./bin/controller 1 FCFS > /tmp/controller.log 2>&1 &
CTRL_PID=$!
sleep 2

echo "=== Test 1: Echo command ==="
./bin/runner -e 1 "echo Hello"
sleep 1

echo "=== Test 2: Date command ==="
./bin/runner -e 2 "date" 
sleep 1

echo "=== Test 3: Status ==="
timeout 5 ./bin/runner -c || echo "(Status timeout)"
sleep 1

echo "=== Test 4: Shutdown ==="
timeout 5 ./bin/runner -s || echo "(Shutdown timeout)"
sleep 2

echo "=== Execution Log ==="
if [ -f "tmp/executions.log" ]; then
    echo "Results:"
    cat tmp/executions.log
    echo ""
    echo "Total jobs: $(wc -l < tmp/executions.log)"
else
    echo "No log created"
fi

killall controller 2>/dev/null || true
echo "=== Test Complete ==="
