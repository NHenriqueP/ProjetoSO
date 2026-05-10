set -e
make clean && make
pkill -f "bin/controller|bin/runner" 2>/dev/null || true

# Test A: quote + pipes/redirection
./controller 1 FCFS > /tmp/fix_ctrl_a.log 2>&1 &
ctrl_a=$!
sleep 0.3
./runner -e 1 "echo 'hello world'" > /tmp/fix_quote.log 2>&1
quote_rc=$?
printf "a\nb\n" > /tmp/fix_in.txt
./runner -e 1 "cat < /tmp/fix_in.txt | wc -l > /tmp/fix_out.txt" > /tmp/fix_pipe.log 2>&1
pipe_rc=$?
./runner -s > /tmp/fix_shutdown_a.log 2>&1
shutdown_a_rc=$?
wait "$ctrl_a" 2>/dev/null || true

# Test B: RR order in -c
pkill -f "bin/controller|bin/runner" 2>/dev/null || true
./controller 1 RR_USER > /tmp/fix_ctrl_b.log 2>&1 &
ctrl_b=$!
sleep 0.3
./runner -e 1 "sleep 2" > /tmp/fix_r1.log 2>&1 &
p1=$!
sleep 0.05
./runner -e 1 "sleep 2" > /tmp/fix_r2.log 2>&1 &
p2=$!
sleep 0.05
./runner -e 2 "sleep 2" > /tmp/fix_r3.log 2>&1 &
p3=$!
sleep 0.5
./runner -c > /tmp/fix_status_rr.log 2>&1
status_rr_rc=$?
wait "$p1" "$p2" "$p3"
./runner -s > /tmp/fix_shutdown_b.log 2>&1
shutdown_b_rc=$?
wait "$ctrl_b" 2>/dev/null || true

# Test C: multiple shutdown requesters
pkill -f "bin/controller|bin/runner" 2>/dev/null || true
./controller 1 FCFS > /tmp/fix_ctrl_c.log 2>&1 &
ctrl_c=$!
sleep 0.3
./runner -e 1 "sleep 2" > /tmp/fix_longjob.log 2>&1 &
job_pid=$!
sleep 0.1
timeout 10 ./runner -s > /tmp/fix_s1.log 2>&1 &
s1=$!
timeout 10 ./runner -s > /tmp/fix_s2.log 2>&1 &
s2=$!
wait "$job_pid"
wait "$s1"; shutdown_multi_rc1=$?
wait "$s2"; shutdown_multi_rc2=$?
wait "$ctrl_c" 2>/dev/null || true
pkill -f "bin/controller|bin/runner" 2>/dev/null || true

echo "quote_rc=$quote_rc"
echo "pipe_rc=$pipe_rc"
echo "shutdown_a_rc=$shutdown_a_rc"
echo "status_rr_rc=$status_rr_rc"
echo "shutdown_b_rc=$shutdown_b_rc"
echo "shutdown_multi_rc1=$shutdown_multi_rc1"
echo "shutdown_multi_rc2=$shutdown_multi_rc2"
echo "---fix_quote.log---"
cat /tmp/fix_quote.log
echo "---fix_pipe.log---"
cat /tmp/fix_pipe.log
echo "---fix_out.txt---"
cat /tmp/fix_out.txt
echo "---fix_status_rr.log---"
cat /tmp/fix_status_rr.log
echo "---fix_s1.log---"
cat /tmp/fix_s1.log
echo "---fix_s2.log---"
cat /tmp/fix_s2.log
