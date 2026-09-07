#!/bin/bash
# Reproduces every benchmark in the README.
#
# Each latency figure is the median of several runs rather than a single shot,
# because a one-off number on a shared machine is noise with a decimal point.
#
# Usage: bench/run_all.sh [reps]

set -u
cd "$(dirname "$0")/.." || exit 1
REPS=${1:-5}

make >/dev/null 2>&1 || { echo "build failed"; exit 1; }

median() { sort -n | awk '{v[NR]=$1} END{print (NR%2) ? v[(NR+1)/2] : (v[NR/2]+v[NR/2+1])/2}'; }

hr() { printf '%s\n' "------------------------------------------------------------"; }

echo "host: $(uname -sr)  cores: $(nproc)"
echo "reps per measurement: $REPS"
hr

echo "1. CONTEXT SWITCH LATENCY (ns per switch, median of $REPS)"
for target in switch_bench switch_bench_uctx pthread_switch_bench; do
  vals=""
  for _ in $(seq 1 "$REPS"); do
    vals+="$(./bin/$target | awk '/per switch/ {print $3}')"$'\n'
  done
  printf "  %-22s %s ns\n" "$target" "$(printf '%s' "$vals" | median)"
done
hr

echo "2. MEMORY PER UNIT OF CONCURRENCY (10,000 concurrent)"
./bin/memory_bench green 10000
./bin/memory_bench pthread-small 10000
./bin/memory_bench pthread 10000
hr

echo "3. MULTI-CORE SCALING (work stealing)"
./bin/scaling_bench 8
hr

echo "4. I/O THROUGHPUT AND LATENCY (100 connections, 5s, 64-byte echo)"
for server in echo_server pthread_echo_server; do
  port=$(( RANDOM % 1000 + 9400 ))
  if [ "$server" = "echo_server" ]; then
    ./bin/echo_server "$port" 1 >/dev/null 2>&1 &
  else
    ./bin/pthread_echo_server "$port" >/dev/null 2>&1 &
  fi
  srv=$!
  sleep 1
  echo "  --- $server ---"
  ./bin/loadgen "$port" 100 5 64 | sed 's/^/    /'
  kill -9 $srv >/dev/null 2>&1
  wait $srv 2>/dev/null
  sleep 0.5
done
hr
echo "done"
