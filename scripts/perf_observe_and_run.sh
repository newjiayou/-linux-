#!/bin/bash
set -euo pipefail

if [[ $# -lt 4 ]]; then
    echo "Usage: $0 <port> <sample_seconds> <output_dir> <command...>"
    echo "Example:"
    echo "  $0 12345 60 ./perf_run bash -lc 'python3 scripts/bench_chat_latency.py --host 127.0.0.1 --port 12345 --clients 50 --qps 20 -n 200'"
    exit 1
fi

PORT="$1"
SAMPLE_SECONDS="$2"
OUT_DIR="$3"
shift 3

mkdir -p "$OUT_DIR"

find_pid_by_port() {
    ss -ltnp "( sport = :$PORT )" 2>/dev/null | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | head -n1
}

PID="$(find_pid_by_port)"
if [[ -z "${PID}" ]]; then
    echo "[ERROR] No listening process found on port $PORT"
    exit 1
fi

echo "[INFO] Target PID: $PID"
echo "[INFO] Output dir : $OUT_DIR"
echo "[INFO] Command    : $*"

echo "$PID" > "$OUT_DIR/target.pid"
date -Is > "$OUT_DIR/start_at.txt"

pidstat -h -u -r -d -w -p "$PID" 1 > "$OUT_DIR/pidstat.log" 2>&1 &
PIDSTAT_PID=$!

(
    end_ts=$(( $(date +%s) + SAMPLE_SECONDS ))
    while [[ $(date +%s) -lt $end_ts ]]; do
        {
            echo "===== ts=$(date +%s) ====="
            ss -s
            echo "----- established on :$PORT -----"
            ss -tin state established "( sport = :$PORT )" | sed -n '1,80p'
            echo "----- netstat tcp summary -----"
            netstat -s 2>/dev/null | rg -n "retransmitted|failed connection|reset|listen queue|SYNs to LISTEN sockets dropped|pruned from receive queue" -i || true
            echo
        } >> "$OUT_DIR/network.log"
        sleep 2
    done
) &
NET_PID=$!

set +e
"$@" > "$OUT_DIR/bench.stdout.log" 2> "$OUT_DIR/bench.stderr.log"
BENCH_EXIT=$?
set -e

kill "$PIDSTAT_PID" 2>/dev/null || true
kill "$NET_PID" 2>/dev/null || true
wait "$PIDSTAT_PID" 2>/dev/null || true
wait "$NET_PID" 2>/dev/null || true

date -Is > "$OUT_DIR/end_at.txt"
echo "$BENCH_EXIT" > "$OUT_DIR/bench.exit_code"

echo "[INFO] Benchmark exit code: $BENCH_EXIT"
echo "[INFO] Logs written to: $OUT_DIR"

exit "$BENCH_EXIT"
