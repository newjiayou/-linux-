#!/bin/bash
set -euo pipefail

HOST="${1:-127.0.0.1}"
PORT="${2:-12345}"
MODE="${3:-hb}"
OUT_DIR="${4:-./bench_tcpkali_results}"

DURATION="${DURATION:-20s}"
WARMUP="${WARMUP:-3s}"

TARGET="${HOST}:${PORT}"
PACKET_DIR="${OUT_DIR}/packets"
RAW_DIR="${OUT_DIR}/raw"
SUMMARY_FILE="${OUT_DIR}/summary.csv"
PACKET_FILE="${PACKET_DIR}/${MODE}.bin"

mkdir -p "$PACKET_DIR" "$RAW_DIR"

if ! command -v tcpkali >/dev/null 2>&1; then
    echo "[ERROR] tcpkali 未安装。Ubuntu 可先执行: sudo apt-get install tcpkali"
    exit 1
fi

case "$MODE" in
    hb)
        PACKET_SIZE=6
        CONNECTIONS=( ${CONNECTIONS:-200 500 1000 2000} )
        RATES=( ${RATES:-2000 5000 10000 20000 50000} )
        python3 - <<'PY' "$PACKET_FILE"
import struct, sys
with open(sys.argv[1], 'wb') as f:
    f.write(struct.pack('!IH', 6, 2))
PY
        MODE_DESC="心跳包压测（最适合测服务器 I/O 吞吐上限）"
        ;;
    chat)
        PACKET_SIZE=57
        CONNECTIONS=( ${CONNECTIONS:-50 100 200} )
        RATES=( ${RATES:-100 300 500 1000} )
        python3 - <<'PY' "$PACKET_FILE"
import struct, sys
body = b'{"sender":"bench","target":"broadcast","message":"hello"}'
with open(sys.argv[1], 'wb') as f:
    f.write(struct.pack('!I', 6 + len(body)) + struct.pack('!H', 1) + body)
PY
        MODE_DESC="广播聊天包压测（包含业务处理开销，已降为安全默认档）"
        ;;
    *)
        echo "[ERROR] MODE 仅支持 hb 或 chat，当前: $MODE"
        exit 1
        ;;
esac

echo "round,mode,connections,target_rate,duration,warmup,messages_sent,messages_received,bytes_sent,bytes_received,send_rate_msgps,recv_rate_msgps,errors,raw_file" > "$SUMMARY_FILE"

echo "[INFO] tcpkali 吞吐压测开始"
echo "[INFO] 目标        : $TARGET"
echo "[INFO] 模式        : $MODE ($MODE_DESC)"
echo "[INFO] 时长        : $DURATION"
echo "[INFO] 预热        : $WARMUP"
echo "[INFO] 连接梯度    : ${CONNECTIONS[*]}"
echo "[INFO] 速率梯度    : ${RATES[*]}"
echo "[INFO] 包文件      : $PACKET_FILE"

to_num() {
    local value="$1"
    value="${value//,/}"
    echo "${value:-0}"
}

extract_value() {
    local file="$1"
    local regex="$2"
    local fallback="${3:-0}"
    local value
    value=$(python3 - <<'PY' "$file" "$regex"
import re, sys
text = open(sys.argv[1], 'r', encoding='utf-8', errors='ignore').read()
match = re.search(sys.argv[2], text, re.I | re.M)
print(match.group(1) if match else "")
PY
)
    if [[ -z "$value" ]]; then
        echo "$fallback"
    else
        echo "$(to_num "$value")"
    fi
}

calc_rate_from_duration() {
    local total="$1"
    local duration_seconds="$2"
    awk -v total="$total" -v seconds="$duration_seconds" 'BEGIN { if (seconds <= 0) print "0"; else printf "%.2f", total / seconds }'
}

round=0
for c in "${CONNECTIONS[@]}"; do
    for rate in "${RATES[@]}"; do
        round=$((round + 1))
        raw_file="$RAW_DIR/round_${round}_c${c}_r${rate}.log"

        echo ""
        echo "======================================================================"
        echo "[INFO] Round $round | mode=$MODE | connections=$c | rate=$rate msg/s"
        echo "======================================================================"

        set +e
        tcpkali \
            --connect-rate="$c" \
            -c "$c" \
            -T "$DURATION" \
            --channel-lifetime "$DURATION" \
            --message-file "$PACKET_FILE" \
            -r "$rate" \
            "$TARGET" > "$raw_file" 2>&1
        exit_code=$?
        set -e

        sent=$(extract_value "$raw_file" 'Messages sent:\s*([0-9,]+)')
        recv=$(extract_value "$raw_file" 'Messages received:\s*([0-9,]+)')
        bytes_sent=$(extract_value "$raw_file" 'Total data sent:\s*.*\(([0-9,]+) bytes\)')
        bytes_recv=$(extract_value "$raw_file" 'Total data received:\s*.*\(([0-9,]+) bytes\)')
        send_rate=$(extract_value "$raw_file" 'Message rate:\s*([0-9,.]+)\s*/sec')
        recv_rate=$(extract_value "$raw_file" 'Incoming rate:\s*([0-9,.]+)\s*/sec')
        duration_actual=$(extract_value "$raw_file" 'Test duration:\s*([0-9.]+)\s*s')

        if [[ "$sent" == "0" && "$bytes_sent" != "0" ]]; then
            sent=$((bytes_sent / PACKET_SIZE))
        fi
        if [[ "$recv" == "0" && "$bytes_recv" != "0" ]]; then
            recv=$((bytes_recv / PACKET_SIZE))
        fi
        if [[ "$send_rate" == "0" && "$sent" != "0" ]]; then
            send_rate=$(calc_rate_from_duration "$sent" "$duration_actual")
        fi
        if [[ "$recv_rate" == "0" && "$recv" != "0" ]]; then
            recv_rate=$(calc_rate_from_duration "$recv" "$duration_actual")
        fi

        errors=0
        if (( exit_code != 0 )); then
            errors=$exit_code
        fi

        echo "$round,$MODE,$c,$rate,$DURATION,$WARMUP,$sent,$recv,$bytes_sent,$bytes_recv,$send_rate,$recv_rate,$errors,$raw_file" >> "$SUMMARY_FILE"

        echo "[RESULT] round=$round"
        echo "  sent messages : $sent"
        echo "  recv messages : $recv"
        echo "  sent bytes    : $bytes_sent"
        echo "  recv bytes    : $bytes_recv"
        echo "  send rate     : $send_rate msg/s"
        echo "  recv rate     : $recv_rate msg/s"
        echo "  raw output    : $raw_file"

        sleep 3
    done
done

echo ""
echo "[INFO] 压测完成，汇总文件: $SUMMARY_FILE"
echo "[INFO] hb 默认档用于测 I/O 上限；chat 默认档已降为更安全的业务压测档。"