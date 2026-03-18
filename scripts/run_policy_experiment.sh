#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: ./scripts/run_policy_experiment.sh <FCFS|RR_USER> [parallel-commands]" >&2
  exit 1
fi

policy="$1"
parallel="${2:-1}"

if [[ "$policy" != "FCFS" && "$policy" != "RR_USER" ]]; then
  echo "invalid policy: $policy (use FCFS or RR_USER)" >&2
  exit 1
fi

if ! [[ "$parallel" =~ ^[0-9]+$ ]] || [[ "$parallel" -le 0 ]]; then
  echo "parallel-commands must be a positive integer" >&2
  exit 1
fi

cleanup() {
  if [[ -n "${controller_pid:-}" ]]; then
    kill -TERM "$controller_pid" >/dev/null 2>&1 || true
    wait "$controller_pid" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

make >/dev/null
make fake_runner >/dev/null

: > tmp/executions.log
./bin/controller "$parallel" "$policy" >/tmp/controller_experiment.log 2>&1 &
controller_pid=$!
sleep 0.2

# Mixed workload with repeated user IDs to stress fairness.
./bin/fake_runner submit 1 7001 2 >/tmp/fr_7001.log 2>&1 & p1=$!
sleep 0.05
./bin/fake_runner submit 1 7002 0 >/tmp/fr_7002.log 2>&1 & p2=$!
sleep 0.05
./bin/fake_runner submit 2 7003 1 >/tmp/fr_7003.log 2>&1 & p3=$!
sleep 0.05
./bin/fake_runner submit 3 7004 0 >/tmp/fr_7004.log 2>&1 & p4=$!
sleep 0.05
./bin/fake_runner submit 2 7005 0 >/tmp/fr_7005.log 2>&1 & p5=$!

wait "$p1" "$p2" "$p3" "$p4" "$p5"
./bin/fake_runner shutdown >/tmp/fr_shutdown.log 2>&1
wait "$controller_pid"
controller_pid=""

echo "policy=$policy parallel=$parallel"
awk -f scripts/summarize_executions.awk tmp/executions.log
