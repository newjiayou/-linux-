#!/bin/bash
LOG_FILE="/var/log/chatserver.log"
MAX_SIZE=52428800 # 50MB in bytes

if [ -f "$LOG_FILE" ]; then
    FILE_SIZE=$(stat -c%s "$LOG_FILE")
    if [ "$FILE_SIZE" -gt "$MAX_SIZE" ]; then
        # 备份并压缩
        mv "$LOG_FILE" "$LOG_FILE.$(date +%Y%m%d%H%M%S)"
        gzip "$LOG_FILE.$(date +%Y%m%d%H%M%S)"
        # 创建新的日志文件
        touch "$LOG_FILE"
    fi
fi