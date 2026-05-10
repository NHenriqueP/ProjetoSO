set -e
make >/dev/null
pkill -f "bin/controller|bin/runner" 2>/dev/null || true

./controller 1 FCFS > /tmp/fix_ctrl_a.log 2>&1 &
ctrl_a=$!
sleep 0.4
./runner -e 1 "echo 'hello world'" > /tmp/fix_quote.log 2>&1
printf "a\nb\n" > /tmp/fix_in.txt
./runner -e 1 "cat < /tmp/fix_in.txt | wc -l > /tmp/fix_out.txt" > /tmp/fix_pipe.log 2>&1
./runner -s > /tmp/fix_shutdown_a.log 2>&1
wait "$ctrl_a" >/dev/null 2>&1 || true

echo "---fix_quote.log---"
cat /tmp/fix_quote.log
echo "---fix_pipe.log---"
cat /tmp/fix_pipe.log
echo "---fix_out.txt---"
cat /tmp/fix_out.txt

pkill -f "bin/controller|bin/runner" 2>/dev/null || true
./controller 1 RR_USER > /tmp/fix_ctrl_b.log 2>&1 &
ctrl_b=$!
sleep 0.4
./runner -e 1 "sleep 2" > /tmp/fix_r1.log 2>&1 &
sleep 0.05
./runner -e 1 "sleep 2" > /tmp/fix_r2.log 2>&1 &
sleep 0.05
./runner -e 2 "sleep 2" > /tmp/fix_r3.log 2>&1 &
sleep 0.35
./runner -c > /tmp/fix_status_rr.log 2>&1 || true
./runner -s > /tmp/fix_shutdown_b.log 2>&1 || true
wait "$ctrl_b" >/dev/null 2>&1 || true

echo "---fix_status_rr.log---"
cat /tmp/fix_status_rr.log
pkill -f "bin/controller|bin/runner" 2>/dev/null || true
