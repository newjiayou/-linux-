#!/bin/bash
set -euo pipefail

HOST="${1:-127.0.0.1}"
PORT="${2:-12345}"
PACKET_FILE="${3:-./bench_packets/chat.bin}"
OUT_DIR="${4:-./bench_chat_results}"
DURATION="${DURATION:-60s}"
CONNECTION_LEVELS=(${CONNECTION_LEVELS:-100 500 1000})
RATE_LEVELS=(${RATE_LEVELS:-100 500 1000 2000 5000})

mkdir -p "$OUT_DIR"
RAW_DIR="$OUT_DIR/raw"
mkdir -p "$RAW_DIR"
SUMMARY_FILE="$OUT_DIR/summary.csv"

if ! command -v tcpkali >/dev/null 2>&1; then
    echo "[ERROR] tcpkali 未安装，请先安装 tcpkali。"
    exit 1
fi

if [[ ! -f "$PACKET_FILE" ]]; then
    echo "[ERROR] 压测报文文件不存在: $PACKET_FILE"
    echo "[HINT] 先执行: python3 scripts/gen_bench_packets.py --output-dir ./bench_packets"
    exit 1
fi

TARGET="${HOST}:${PORT}"
DURATION_SECONDS="${DURATION%s}"
if [[ ! "$DURATION_SECONDS" =~ ^[0-9]+$ ]]; then
    echo "[ERROR] 当前脚本仅支持类似 60s 这种秒级时长，收到: $DURATION"
    exit 1
fi

echo "round,connections,target_send_rate,duration,connections_observed,messages_sent,messages_received,send_rate_actual,recv_rate_actual,connect_errors,other_errors,disconnects,notes,raw_file" > "$SUMMARY_FILE"

echo "[INFO] 开始并发聊天压测"
echo "[INFO] 目标: $TARGET"
echo "[INFO] 报文: $PACKET_FILE"
echo "[INFO] 压测时长: $DURATION"
echo "[INFO] 连接梯度: ${CONNECTION_LEVELS[*]}"
echo "[INFO] 速率梯度: ${RATE_LEVELS[*]}"

extract_first_number() {
    local file="$1"
    shift
    local patterns=("$@")
    local value=""
    for pattern in "${patterns[@]}"; do
        value=$(grep -Eio "$pattern" "$file" | head -n 1 | grep -Eo '[0-9]+' || true)
        if [[ -n "$value" ]]; then
            echo "$value"
            return 0
        fi
    done
    echo "0"
}

calc_rate() {
    local total="$1"
    local seconds="$2"
    awk -v total="$total" -v seconds="$seconds" 'BEGIN { if (seconds == 0) print "0.00"; else printf "%.2f", total / seconds }'
}

round=0
for c in "${CONNECTION_LEVELS[@]}"; do
    for rate in "${RATE_LEVELS[@]}"; do
        round=$((round + 1))
        raw_file="$RAW_DIR/round_${round}_c${c}_r${rate}.log"

        echo ""
        echo "======================================================================"
        echo "[INFO] Round $round | 并发连接=$c | 目标发送速率=${rate} msg/s | 时长=$DURATION"
        echo "======================================================================"

        set +e
        tcpkali -c "$c" -T "$DURATION" -r "$rate" --message-file "$PACKET_FILE" "$TARGET" > "$raw_file" 2>&1
        exit_code=$?
        set -e

        connections_observed=$(extract_first_number "$raw_file" \
            'Connections[[:space:]]*:[[:space:]]*[0-9]+' \
            'connection rate[[:space:]]*:[[:space:]]*[0-9]+' \
            'connect rate[[:space:]]*:[[:space:]]*[0-9]+')

        messages_sent=$(extract_first_number "$raw_file" \
            'messages sent[[:space:]]*:[[:space:]]*[0-9]+' \
            'message[s]?[[:space:]]*sent[[:space:]]*:[[:space:]]*[0-9]+' \
            'sent[[:space:]]*[0-9]+[[:space:]]*messages')

        messages_received=$(extract_first_number "$raw_file" \
            'messages received[[:space:]]*:[[:space:]]*[0-9]+' \
            'message[s]?[[:space:]]*received[[:space:]]*:[[:space:]]*[0-9]+' \
            'received[[:space:]]*[0-9]+[[:space:]]*messages')

        connect_errors=$(extract_first_number "$raw_file" \
            'connect errors[[:space:]]*:[[:space:]]*[0-9]+' \
            'connection errors[[:space:]]*:[[:space:]]*[0-9]+')

        other_errors=$(extract_first_number "$raw_file" \
            '^errors[[:space:]]*:[[:space:]]*[0-9]+' \
            'errors[[:space:]]*:[[:space:]]*[0-9]+' \
            'error[s]?[[:space:]]*=[[:space:]]*[0-9]+')

        disconnects=$(extract_first_number "$raw_file" \
            'disconnects[[:space:]]*:[[:space:]]*[0-9]+' \
            'disconnections[[:space:]]*:[[:space:]]*[0-9]+' \
            'closed[[:space:]]*:[[:space:]]*[0-9]+')

        send_rate_actual=$(calc_rate "$messages_sent" "$DURATION_SECONDS")
        recv_rate_actual=$(calc_rate "$messages_received" "$DURATION_SECONDS")

        notes="ok"
        if (( exit_code != 0 )); then
            notes="tcpkali_exit_${exit_code}"
        fi

        echo "$round,$c,$rate,$DURATION,$connections_observed,$messages_sent,$messages_received,$send_rate_actual,$recv_rate_actual,$connect_errors,$other_errors,$disconnects,$notes,$raw_file" >> "$SUMMARY_FILE"

        echo "[RESULT] Round $round"
        echo "  并发连接数       : $c"
        echo "  目标发送速率     : ${rate} msg/s"
        echo "  实际观测连接数   : $connections_observed"
        echo "  总发送消息数     : $messages_sent"
        echo "  总接收消息数     : $messages_received"
        echo "  每秒发送消息数   : $send_rate_actual msg/s"
        echo "  每秒接收消息数   : $recv_rate_actual msg/s"
        echo "  连接错误数       : $connect_errors"
        echo "  其他错误数       : $other_errors"
        echo "  断连数           : $disconnects"
        echo "  原始输出         : $raw_file"

        sleep 5
    done
done

echo ""
echo "[INFO] 并发聊天压测完成"
echo "[INFO] 汇总文件: $SUMMARY_FILE"
echo "[INFO] 你可以用 summary.csv 找到最大稳定每秒发送/接收消息数。"