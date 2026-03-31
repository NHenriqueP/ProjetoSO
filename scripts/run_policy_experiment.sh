#!/usr/bin/env bash
set -euo pipefail

export LC_ALL=C

if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "usage: ./scripts/run_policy_experiment.sh <FCFS|RR_USER|SJF_PROXY> [parallel-commands] [repetitions]" >&2
  exit 1
fi

policy="$1"
parallel="${2:-1}"
repetitions="${3:-3}"

if [[ "$policy" != "FCFS" && "$policy" != "RR_USER" && "$policy" != "SJF_PROXY" ]]; then
  echo "invalid policy: $policy (use FCFS, RR_USER or SJF_PROXY)" >&2
  exit 1
fi

if ! [[ "$parallel" =~ ^[0-9]+$ ]] || [[ "$parallel" -le 0 ]]; then
  echo "parallel-commands must be a positive integer" >&2
  exit 1
fi

if ! [[ "$repetitions" =~ ^[0-9]+$ ]] || [[ "$repetitions" -le 0 ]]; then
  echo "repetitions must be a positive integer" >&2
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

summary_csv="tmp/experiment_runs.csv"
table_file="tmp/final_metrics_table.txt"

echo "run,avg_wait_ms,avg_total_ms,fairness_wait_gap_ms" > "$summary_csv"

run_once() {
  : > tmp/executions.log
  : > tmp/events.log

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
}

for run in $(seq 1 "$repetitions"); do
  run_once
  summary_output="$(awk -f scripts/summarize_executions.awk tmp/executions.log)"
  avg_wait="$(printf '%s\n' "$summary_output" | awk -F= '/^avg_wait_ms=/{print $2}' | tr ',' '.')"
  avg_total="$(printf '%s\n' "$summary_output" | awk -F= '/^avg_total_ms=/{print $2}' | tr ',' '.')"
  fairness="$(printf '%s\n' "$summary_output" |
    awk -F= '/^fairness_wait_gap_ms=/{print $2}' |
    awk '{print $1}' |
    tr ',' '.')"
  echo "$run,$avg_wait,$avg_total,$fairness" >> "$summary_csv"
done

read -r mean_wait mean_total mean_fairness <<EOF
$(awk -F, 'NR>1 {w+=$2; t+=$3; f+=$4; n++} END {if (n==0) {print "0 0 0"} else {printf "%.3f %.3f %.3f", w/n, t/n, f/n}}' "$summary_csv")
EOF

{
  echo "policy=$policy"
  echo "parallel=$parallel"
  echo "repetitions=$repetitions"
  echo "avg_wait_ms=$mean_wait"
  echo "avg_total_ms=$mean_total"
  echo "fairness_wait_gap_ms=$mean_fairness"
} | tee "$table_file"

echo "run_details_file=$summary_csv"
echo "final_table_file=$table_file"
