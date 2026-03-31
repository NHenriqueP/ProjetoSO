#!/bin/bash
# Debug test

PROJ_DIR="/home/cerqueira/UNI/SO/projeto/ProjetoSO"
cd "$PROJ_DIR"

echo "=== Step 1: Kill any old processes ==="
killall -9 controller 2>/dev/null || true
killall -9 runner 2>/dev/null || true
sleep 1

echo "=== Step 2: Clean FIFOs ==="
find /tmp -name "so_*.fifo" -delete 2>/dev/null || true

echo "=== Step 3: Build ==="
make >/dev/null 2>&1

echo "=== Step 4: Start controller in background ==="
./bin/controller 1 FCFS > /tmp/controller.log 2>&1 &
CONTROLLER_PID=$!
echo "Controller started with PID: $CONTROLLER_PID"
sleep 2

echo "=== Step 5: Check if controller FIFO exists ==="
if [ -p "/tmp/so_controller_fifo" ]; then
    echo "✓ FIFO found"
else
    echo "✗ FIFO NOT found!"
    echo "Controller log:"
    cat /tmp/controller.log
    exit 1
fi

echo "=== Step 6: Run first test ==="
echo "Executing: ./bin/runner -e 1 'echo test'"
timeout 5 ./bin/runner -e 1 "echo test" 2>&1 || echo "Runner exited with code: $?"
sleep 1

echo "=== Step 7: Check controller log ==="
if [ -f "/tmp/controller.log" ]; then
    echo "Controller log output:"
    tail -20 /tmp/controller.log
fi

echo "=== Step 8: Check if execution log was created ==="
if [ -f "tmp/executions.log" ]; then
    echo "✓ Execution log found:"
    cat tmp/executions.log
else
    echo "✗ No execution log created"
fi

echo "=== Cleanup ==="
killall controller 2>/dev/null || true
